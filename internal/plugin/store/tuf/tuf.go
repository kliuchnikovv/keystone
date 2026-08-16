// Package tuf is a minimal TUF client for keeping Sigstore trust
// anchors (Fulcio root, Rekor public key, and their siblings) fresh
// without pulling in the full sigstore-go dependency tree.
//
// Scope:
//   - Load a trusted root.json (embedded or supplied).
//   - Refresh: fetch timestamp → snapshot → targets, verify each
//     signature threshold, expiry, monotonically-increasing version.
//   - Fetch(target): download by verified sha256.
//   - Signature algorithms: ed25519 and ecdsa-sha2-nistp256, which
//     is what Sigstore's production root uses today.
//
// Not landed:
//   - Root rotation. The initial root is trusted once; a follow-on
//     will do the rolling-key-verify chain.
//   - Delegations, custom mirrors, multi-repo config.
//   - Snapshot's optional per-role hash pinning.
//
// The Sigstore-specific glue (which targets to fetch, where to
// stash them) sits above this package.
package tuf

import (
	"context"
	"crypto/ecdsa"
	"crypto/ed25519"
	"crypto/sha256"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// Client holds trusted metadata after Refresh.
type Client struct {
	root      *SignedRoot
	targets   *SignedTargets
	snapshot  *SignedSnapshot
	timestamp *SignedTimestamp
	now       func() time.Time
}

// New parses and verifies a trusted root.json. The caller obtains the
// bytes out-of-band (embedded, checked into the repo, or fetched over
// a channel they trust) — that root is the ground truth for everything
// Refresh verifies later.
func New(rootJSON []byte) (*Client, error) {
	c := &Client{now: time.Now}
	if err := c.loadInitialRoot(rootJSON); err != nil {
		return nil, err
	}
	return c, nil
}

// WithClock lets tests pin the verification time. Prod uses time.Now.
func (c *Client) WithClock(now func() time.Time) *Client {
	c.now = now
	return c
}

// Refresh fetches timestamp → snapshot → targets from repoURL and
// verifies each against the previously trusted metadata. Before the
// role chain runs, it walks any newer root.json versions the repo
// publishes and rotates the trusted root forward — a signing-key
// compromise handled via a normal root rotation reaches every
// installation without a manual re-pin.
//
// Client state (timestamp / snapshot / targets) is updated
// atomically: either all three verify and land together, or none
// do. Root rotation is a series of atomic swaps on its own; each
// successful version-N+1 root becomes the trust anchor before
// version N+2 is attempted.
func (c *Client) Refresh(ctx context.Context, http *http.Client, repoURL string) error {
	baseURL := strings.TrimRight(repoURL, "/")

	if err := c.refreshRoot(ctx, http, baseURL); err != nil {
		return err
	}

	// Refresh cannot proceed if the root the whole chain hangs off
	// of has expired. refreshRoot may have moved us forward; if the
	// current trusted root is still expired, everything below is
	// untrustworthy.
	if err := notExpired(c.root.Signed.Expires, c.now()); err != nil {
		return fmt.Errorf("tuf: trusted root expired: %w", err)
	}

	tsBytes, err := fetch(ctx, http, baseURL+"/timestamp.json")
	if err != nil {
		return fmt.Errorf("tuf: fetch timestamp: %w", err)
	}
	ts, err := c.verifyTimestamp(tsBytes)
	if err != nil {
		return err
	}
	// A timestamp that does not name a snapshot version is a
	// fail-open: verifySnapshot's cross-check reduces to "any
	// version accepted". Require it explicitly so a stripped meta
	// map cannot smuggle a version through.
	tsSnapMeta, ok := ts.Signed.Meta["snapshot.json"]
	if !ok || tsSnapMeta.Version < 1 {
		return errors.New("tuf: timestamp meta missing snapshot.json version")
	}

	snapURL := baseURL + fmt.Sprintf("/%d.snapshot.json", tsSnapMeta.Version)
	snapBytes, err := fetch(ctx, http, snapURL)
	if err != nil {
		return fmt.Errorf("tuf: fetch snapshot: %w", err)
	}
	snap, err := c.verifySnapshot(snapBytes, ts)
	if err != nil {
		return err
	}

	targetsMeta, ok := snap.Signed.Meta["targets.json"]
	if !ok || targetsMeta.Version < 1 {
		return errors.New("tuf: snapshot missing targets.json meta")
	}
	tgURL := baseURL + fmt.Sprintf("/%d.targets.json", targetsMeta.Version)
	tgBytes, err := fetch(ctx, http, tgURL)
	if err != nil {
		return fmt.Errorf("tuf: fetch targets: %w", err)
	}
	tg, err := c.verifyTargets(tgBytes, targetsMeta.Version)
	if err != nil {
		return err
	}
	// Atomic swap: everything verified against the trusted root, so
	// the client's view moves forward in one step.
	c.timestamp = ts
	c.snapshot = snap
	c.targets = tg
	return nil
}

// Target reports the trusted metadata for a target by its path in
// targets.json. Fetch downloads it and confirms the sha256 matches.
func (c *Client) Target(name string) (*Target, error) {
	if c.targets == nil {
		return nil, errors.New("tuf: Refresh not called")
	}
	t, ok := c.targets.Signed.Targets[name]
	if !ok {
		return nil, fmt.Errorf("tuf: unknown target %q", name)
	}
	return &t, nil
}

// Fetch downloads a target from repoURL and verifies its sha256.
func (c *Client) Fetch(ctx context.Context, http *http.Client, repoURL, name string) ([]byte, error) {
	t, err := c.Target(name)
	if err != nil {
		return nil, err
	}
	want, ok := t.Hashes["sha256"]
	if !ok || want == "" {
		return nil, fmt.Errorf("tuf: target %q has no sha256", name)
	}
	// TUF stores content-addressed targets under /targets/<sha>.<name>.
	// A version-tolerant client fetches by hash first, then falls back
	// to a plain name; we support both — production Sigstore hosts by
	// hash-prefixed path.
	url := strings.TrimRight(repoURL, "/") + "/targets/" + want + "." + name
	data, err := fetch(ctx, http, url)
	if err != nil {
		// Fall back to the un-prefixed path some mirrors use.
		fallback := strings.TrimRight(repoURL, "/") + "/targets/" + name
		data, err = fetch(ctx, http, fallback)
		if err != nil {
			return nil, err
		}
	}
	// Length must match too. Otherwise an attacker could append
	// garbage after a legitimate payload and hope the caller
	// truncates on their own — or exhaust memory on a naive reader.
	// The fetch cap already bounds the top; this enforces the
	// author's declared size.
	if t.Length > 0 && int64(len(data)) != t.Length {
		return nil, fmt.Errorf("tuf: length mismatch on target %q (got %d, want %d)", name, len(data), t.Length)
	}
	sum := sha256.Sum256(data)
	if hex.EncodeToString(sum[:]) != want {
		return nil, fmt.Errorf("tuf: sha256 mismatch on target %q", name)
	}
	return data, nil
}

// ---------------------------------------------------------------------
// Metadata types
// ---------------------------------------------------------------------

// SignedRoot / SignedTargets / SignedSnapshot / SignedTimestamp all
// share the same envelope: a "signed" block plus a "signatures"
// array. We deserialize twice — once as a raw wrapper (to keep the
// signed bytes intact for verification), once as the typed body.

type envelope struct {
	Signed     json.RawMessage `json:"signed"`
	Signatures []Signature     `json:"signatures"`
}

// Signature is one signer's contribution to the metadata's signature
// threshold.
type Signature struct {
	KeyID string `json:"keyid"`
	Sig   string `json:"sig"` // hex-encoded
}

// Key is one signing key referenced by role.keyids. Sigstore's root
// uses ed25519 and ecdsa-sha2-nistp256; other TUF impls use RSA.
type Key struct {
	Type   string   `json:"keytype"`
	Scheme string   `json:"scheme"`
	Values KeyValue `json:"keyval"`
}

type KeyValue struct {
	Public string `json:"public"` // PEM for ECDSA/RSA, hex for ed25519
}

// Role is a threshold + list of keyids.
type Role struct {
	KeyIDs    []string `json:"keyids"`
	Threshold int      `json:"threshold"`
}

// RootSpec is the body of the root role.
type RootSpec struct {
	Type    string          `json:"_type"`
	Version int             `json:"version"`
	Expires string          `json:"expires"`
	Keys    map[string]Key  `json:"keys"`
	Roles   map[string]Role `json:"roles"`
}

// SignedRoot pairs the signed body with its signatures. The raw
// signed bytes are kept so signature verification can use exactly
// what was signed rather than re-encoding and hoping key order
// matches.
type SignedRoot struct {
	Signed     RootSpec        `json:"-"`
	Raw        json.RawMessage `json:"-"`
	Signatures []Signature     `json:"-"`
}

// TargetsSpec, SnapshotSpec, TimestampSpec each define what their
// role signs. All three share the type + version + expires fields.

type TargetsSpec struct {
	Type    string            `json:"_type"`
	Version int               `json:"version"`
	Expires string            `json:"expires"`
	Targets map[string]Target `json:"targets"`
}

type Target struct {
	Length int64             `json:"length"`
	Hashes map[string]string `json:"hashes"`
}

type SignedTargets struct {
	Signed     TargetsSpec
	Raw        json.RawMessage
	Signatures []Signature
}

type SnapshotSpec struct {
	Type    string          `json:"_type"`
	Version int             `json:"version"`
	Expires string          `json:"expires"`
	Meta    map[string]Meta `json:"meta"`
}

type Meta struct {
	Version int `json:"version"`
}

type SignedSnapshot struct {
	Signed     SnapshotSpec
	Raw        json.RawMessage
	Signatures []Signature
}

type TimestampSpec struct {
	Type    string          `json:"_type"`
	Version int             `json:"version"`
	Expires string          `json:"expires"`
	Meta    map[string]Meta `json:"meta"`
}

type SignedTimestamp struct {
	Signed     TimestampSpec
	Raw        json.RawMessage
	Signatures []Signature
}

// ---------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------

// refreshRoot walks 1.root.json, 2.root.json, … until the server
// returns 404. Each newer root must be signed BOTH by the current
// trusted root's role.root keys (so the previous root explicitly
// approves the rotation) AND by the new root's own role.root keys
// (so it is self-consistent). Version must increase by exactly one
// per step — a skip would let an attacker replay an old signed
// rotation that the current chain already superseded.
//
// A 404 is not an error: it means the server has no further root
// versions, and the current trusted root is the current one. Any
// other transport-level failure aborts refresh so a network glitch
// mid-rotation cannot leave the client silently on an older root.
func (c *Client) refreshRoot(ctx context.Context, http *http.Client, baseURL string) error {
	// Bound the walk so a malicious repo cannot spin the client
	// forever with a fake infinite chain of "next" roots. 128
	// rotations is generous — Sigstore has done far fewer than
	// that across its whole history — but still finite.
	const maxRotations = 128

	for i := 0; i < maxRotations; i++ {
		nextVersion := c.root.Signed.Version + 1
		url := baseURL + fmt.Sprintf("/%d.root.json", nextVersion)
		data, status, err := fetchWithStatus(ctx, http, url)
		if err != nil {
			return fmt.Errorf("tuf: fetch %d.root.json: %w", nextVersion, err)
		}
		if status == 404 {
			return nil // no more rotations pending
		}
		if status != 200 {
			return fmt.Errorf("tuf: %d.root.json: HTTP %d", nextVersion, status)
		}
		next, err := c.verifyRootRotation(data, nextVersion)
		if err != nil {
			return err
		}
		c.root = next
	}
	return fmt.Errorf("tuf: root rotation exceeded %d steps — refusing to walk further", maxRotations)
}

// verifyRootRotation confirms a candidate next-version root satisfies
// both trust anchors: the current trusted root and its own declared
// keys. See TUF spec §5.3.
func (c *Client) verifyRootRotation(data []byte, wantVersion int) (*SignedRoot, error) {
	var env envelope
	if err := json.Unmarshal(data, &env); err != nil {
		return nil, fmt.Errorf("tuf: parse next root: %w", err)
	}
	var spec RootSpec
	if err := json.Unmarshal(env.Signed, &spec); err != nil {
		return nil, fmt.Errorf("tuf: parse next root spec: %w", err)
	}
	if spec.Type != "root" {
		return nil, fmt.Errorf("tuf: next root._type = %q", spec.Type)
	}
	if spec.Version != wantVersion {
		// Skipping a version would allow replaying a signed rotation
		// that the current chain has already superseded.
		return nil, fmt.Errorf("tuf: next root version %d, want %d", spec.Version, wantVersion)
	}
	if err := notExpired(spec.Expires, c.now()); err != nil {
		return nil, fmt.Errorf("tuf: next root expired: %w", err)
	}
	// Signed by the current trusted root's threshold — this is
	// what makes the rotation an authorised action.
	if err := verifySignedBy(env.Signed, env.Signatures, c.root.Signed.Keys, c.root.Signed.Roles["root"]); err != nil {
		return nil, fmt.Errorf("tuf: next root not signed by trusted root: %w", err)
	}
	// Signed by its own declared root keys — self-consistency.
	// Without this, a rotation could hand off to a spec that says
	// "trust these new keys" but does not itself demonstrate
	// possession of them.
	if err := verifySignedBy(env.Signed, env.Signatures, spec.Keys, spec.Roles["root"]); err != nil {
		return nil, fmt.Errorf("tuf: next root not self-signed: %w", err)
	}
	return &SignedRoot{Signed: spec, Raw: env.Signed, Signatures: env.Signatures}, nil
}

// fetchWithStatus is fetch's twin that surfaces the HTTP status so
// refreshRoot can differentiate 404 (stop walking) from other codes.
func fetchWithStatus(ctx context.Context, httpClient *http.Client, url string) ([]byte, int, error) {
	req, err := makeReq(ctx, url)
	if err != nil {
		return nil, 0, err
	}
	resp, err := httpClient.Do(req)
	if err != nil {
		return nil, 0, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, resp.StatusCode, err
	}
	return body, resp.StatusCode, nil
}

func (c *Client) loadInitialRoot(data []byte) error {
	var env envelope
	if err := json.Unmarshal(data, &env); err != nil {
		return fmt.Errorf("tuf: parse root: %w", err)
	}
	var spec RootSpec
	if err := json.Unmarshal(env.Signed, &spec); err != nil {
		return fmt.Errorf("tuf: parse root spec: %w", err)
	}
	if spec.Type != "root" {
		return fmt.Errorf("tuf: root._type is %q", spec.Type)
	}
	root := &SignedRoot{Signed: spec, Raw: env.Signed, Signatures: env.Signatures}
	// The initial root is self-verifying: its own role.root keys are
	// what sign it. Anyone loading a hostile root would already have
	// lost the argument — that trust comes from wherever the bytes
	// originated (embedded, TUF-of-trust, chain-of-command).
	if err := verifySignedBy(root.Raw, root.Signatures, spec.Keys, spec.Roles["root"]); err != nil {
		return fmt.Errorf("tuf: initial root signature: %w", err)
	}
	if err := notExpired(spec.Expires, c.now()); err != nil {
		return fmt.Errorf("tuf: initial root expired: %w", err)
	}
	c.root = root
	return nil
}

func (c *Client) verifyTimestamp(data []byte) (*SignedTimestamp, error) {
	env := envelope{}
	if err := json.Unmarshal(data, &env); err != nil {
		return nil, err
	}
	var spec TimestampSpec
	if err := json.Unmarshal(env.Signed, &spec); err != nil {
		return nil, err
	}
	if spec.Type != "timestamp" {
		return nil, fmt.Errorf("tuf: timestamp._type = %q", spec.Type)
	}
	if err := verifySignedBy(env.Signed, env.Signatures, c.root.Signed.Keys, c.root.Signed.Roles["timestamp"]); err != nil {
		return nil, fmt.Errorf("tuf: timestamp signature: %w", err)
	}
	if err := notExpired(spec.Expires, c.now()); err != nil {
		return nil, fmt.Errorf("tuf: timestamp expired: %w", err)
	}
	if c.timestamp != nil && spec.Version <= c.timestamp.Signed.Version {
		return nil, fmt.Errorf("tuf: timestamp version %d not greater than trusted %d", spec.Version, c.timestamp.Signed.Version)
	}
	return &SignedTimestamp{Signed: spec, Raw: env.Signed, Signatures: env.Signatures}, nil
}

func (c *Client) verifySnapshot(data []byte, ts *SignedTimestamp) (*SignedSnapshot, error) {
	env := envelope{}
	if err := json.Unmarshal(data, &env); err != nil {
		return nil, err
	}
	var spec SnapshotSpec
	if err := json.Unmarshal(env.Signed, &spec); err != nil {
		return nil, err
	}
	if spec.Type != "snapshot" {
		return nil, fmt.Errorf("tuf: snapshot._type = %q", spec.Type)
	}
	if err := verifySignedBy(env.Signed, env.Signatures, c.root.Signed.Keys, c.root.Signed.Roles["snapshot"]); err != nil {
		return nil, fmt.Errorf("tuf: snapshot signature: %w", err)
	}
	if err := notExpired(spec.Expires, c.now()); err != nil {
		return nil, fmt.Errorf("tuf: snapshot expired: %w", err)
	}
	// Timestamp binds snapshot's version — otherwise a rollback
	// attacker could replay an older signed snapshot.
	want := ts.Signed.Meta["snapshot.json"].Version
	if want != 0 && spec.Version != want {
		return nil, fmt.Errorf("tuf: snapshot version %d disagrees with timestamp meta %d", spec.Version, want)
	}
	if c.snapshot != nil && spec.Version < c.snapshot.Signed.Version {
		return nil, fmt.Errorf("tuf: snapshot version %d rolled back from trusted %d", spec.Version, c.snapshot.Signed.Version)
	}
	return &SignedSnapshot{Signed: spec, Raw: env.Signed, Signatures: env.Signatures}, nil
}

func (c *Client) verifyTargets(data []byte, wantVersion int) (*SignedTargets, error) {
	env := envelope{}
	if err := json.Unmarshal(data, &env); err != nil {
		return nil, err
	}
	var spec TargetsSpec
	if err := json.Unmarshal(env.Signed, &spec); err != nil {
		return nil, err
	}
	if spec.Type != "targets" {
		return nil, fmt.Errorf("tuf: targets._type = %q", spec.Type)
	}
	if err := verifySignedBy(env.Signed, env.Signatures, c.root.Signed.Keys, c.root.Signed.Roles["targets"]); err != nil {
		return nil, fmt.Errorf("tuf: targets signature: %w", err)
	}
	if err := notExpired(spec.Expires, c.now()); err != nil {
		return nil, fmt.Errorf("tuf: targets expired: %w", err)
	}
	if wantVersion != 0 && spec.Version != wantVersion {
		return nil, fmt.Errorf("tuf: targets version %d disagrees with snapshot meta %d", spec.Version, wantVersion)
	}
	return &SignedTargets{Signed: spec, Raw: env.Signed, Signatures: env.Signatures}, nil
}

// verifySignedBy confirms enough keys from role signed data. Threshold
// >1 is respected — a Sigstore production root typically requires
// several offline maintainers to co-sign.
func verifySignedBy(data []byte, sigs []Signature, keys map[string]Key, role Role) error {
	if role.Threshold < 1 {
		return fmt.Errorf("tuf: role threshold %d < 1", role.Threshold)
	}
	valid := 0
	seen := map[string]struct{}{}
	for _, s := range sigs {
		if _, dup := seen[s.KeyID]; dup {
			continue
		}
		if !inList(role.KeyIDs, s.KeyID) {
			continue
		}
		key, ok := keys[s.KeyID]
		if !ok {
			continue
		}
		if err := verifyOne(data, s, key); err != nil {
			continue
		}
		seen[s.KeyID] = struct{}{}
		valid++
	}
	if valid < role.Threshold {
		return fmt.Errorf("tuf: %d valid signatures below threshold %d", valid, role.Threshold)
	}
	return nil
}

func verifyOne(data []byte, s Signature, k Key) error {
	sig, err := hex.DecodeString(s.Sig)
	if err != nil {
		return err
	}
	digest := sha256.Sum256(data)
	switch strings.ToLower(k.Scheme) {
	case "ed25519":
		pub, err := hex.DecodeString(k.Values.Public)
		if err != nil || len(pub) != ed25519.PublicKeySize {
			return errors.New("tuf: invalid ed25519 public key")
		}
		if !ed25519.Verify(ed25519.PublicKey(pub), data, sig) {
			return errors.New("tuf: ed25519 signature invalid")
		}
		return nil
	case "ecdsa-sha2-nistp256", "ecdsa":
		key, err := parseECDSAPEM(k.Values.Public)
		if err != nil {
			return err
		}
		if !ecdsa.VerifyASN1(key, digest[:], sig) {
			return errors.New("tuf: ecdsa signature invalid")
		}
		return nil
	default:
		return fmt.Errorf("tuf: unsupported scheme %q", k.Scheme)
	}
}

func parseECDSAPEM(s string) (*ecdsa.PublicKey, error) {
	block := s
	if !strings.Contains(block, "BEGIN PUBLIC KEY") {
		block = "-----BEGIN PUBLIC KEY-----\n" + s + "\n-----END PUBLIC KEY-----"
	}
	pblock, _ := pem.Decode([]byte(block))
	if pblock == nil {
		return nil, errors.New("tuf: no PEM block")
	}
	pub, err := x509.ParsePKIXPublicKey(pblock.Bytes)
	if err != nil {
		return nil, err
	}
	ec, ok := pub.(*ecdsa.PublicKey)
	if !ok {
		return nil, fmt.Errorf("tuf: %T is not ECDSA", pub)
	}
	return ec, nil
}

func inList(xs []string, s string) bool {
	for _, x := range xs {
		if x == s {
			return true
		}
	}
	return false
}

func notExpired(iso string, now time.Time) error {
	t, err := time.Parse(time.RFC3339, iso)
	if err != nil {
		return fmt.Errorf("tuf: parse expires %q: %w", iso, err)
	}
	if !now.Before(t) {
		return fmt.Errorf("expires %s < now %s", iso, now.Format(time.RFC3339))
	}
	return nil
}

func fetch(ctx context.Context, http *http.Client, url string) ([]byte, error) {
	req, err := makeReq(ctx, url)
	if err != nil {
		return nil, err
	}
	resp, err := http.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("HTTP %s", resp.Status)
	}
	// Cap the read — TUF metadata files are small (10s of KB); a
	// runaway body ceiling of 8 MB is generous, refuses a truly
	// hostile server.
	return io.ReadAll(io.LimitReader(resp.Body, 8<<20))
}

// makeReq is factored out so a caller can inject a request-shaping
// hook later without touching every fetch site.
func makeReq(ctx context.Context, url string) (*http.Request, error) {
	return http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
}
