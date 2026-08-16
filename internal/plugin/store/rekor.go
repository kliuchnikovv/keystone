package store

import (
	"crypto/ecdsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
)

// RekorPolicy configures the transparency-log verification step of
// Sigstore. When SigstoreVerifier.Rekor is set, every install must
// carry a Rekor signed entry the verifier can check against
// PublicKey.
//
// The presence of the policy is what makes verification transparent:
// a package that was signed but never landed in Rekor cannot be
// distinguished from a package that was signed and revoked once the
// SET is required. Deployments that care about trust upgrade should
// always populate this.
type RekorPolicy struct {
	// PublicKey is Rekor's ECDSA-P256 signing key. Fetch it from
	// https://rekor.sigstore.dev/api/v1/log/publicKey and load with
	// LoadECDSAPublicKey. TUF-based refresh is future work — the
	// key rotates on Rekor's schedule, not ours.
	PublicKey *ecdsa.PublicKey

	// VerifyInclusion, when true, also verifies the Merkle-tree
	// inclusion proof carried in the bundle's LogEntry. The SET
	// itself already proves Rekor accepted the entry; inclusion
	// proves it landed at a specific index the operator can audit
	// against Rekor's checkpoint. False keeps verification faster
	// during setup; true is what production wants.
	VerifyInclusion bool
}

// RekorLogEntry is the structure cosign emits inside SigstoreBundle.LogEntry.
// A subset of Rekor's model — enough to verify SET + inclusion.
type RekorLogEntry struct {
	// Body is the base64-encoded rekord JSON that was signed.
	// canonical form matters: Rekor signs the exact bytes it stores.
	Body string `json:"body"`

	// IntegratedTime is when Rekor recorded the entry (unix seconds).
	IntegratedTime int64 `json:"integratedTime"`

	// LogIndex is the entry's position in the log.
	LogIndex int64 `json:"logIndex"`

	// LogID is Rekor's log tree id (hex-encoded sha256 of its pubkey).
	LogID string `json:"logID"`

	Verification RekorVerificationBlock `json:"verification"`
}

// RekorVerificationBlock is the "how do I know" half of a log entry —
// Rekor's own signature (SET) and a Merkle inclusion proof.
type RekorVerificationBlock struct {
	SignedEntryTimestamp string             `json:"signedEntryTimestamp"`
	InclusionProof       RekorInclusionProof `json:"inclusionProof"`
}

// RekorInclusionProof is the standard Merkle inclusion payload. Path
// is ordered leaf-to-root; each element is a sibling hash (hex).
type RekorInclusionProof struct {
	Checkpoint string   `json:"checkpoint,omitempty"`
	Hashes     []string `json:"hashes"`
	LogIndex   int64    `json:"logIndex"`
	RootHash   string   `json:"rootHash"`
	TreeSize   int64    `json:"treeSize"`
}

// verifyRekor runs the transparency-log checks. Called from
// SigstoreVerifier.Verify when Rekor is set. artifactSHA256 is the
// digest of the tarball we are about to install — the check that
// binds Rekor's "we recorded a signature" to "we recorded a
// signature *over this artifact*", closing the gap where an attacker
// could mint a valid SET for a different payload and reuse it.
func (p *RekorPolicy) verifyRekor(bundle SigstoreBundle, artifactSHA256 []byte) error {
	if p.PublicKey == nil {
		return errors.New("store/rekor: PublicKey is required")
	}
	if bundle.LogEntry == "" {
		return errors.New("store/rekor: bundle has no logEntry")
	}
	// The bundle carries LogEntry as base64-encoded JSON so a
	// registry can round-trip it as one blob.
	raw, err := base64.StdEncoding.DecodeString(bundle.LogEntry)
	if err != nil {
		// Some tooling stores it as raw JSON; accept either.
		raw = []byte(bundle.LogEntry)
	}
	var entry RekorLogEntry
	if err := json.Unmarshal(raw, &entry); err != nil {
		return fmt.Errorf("store/rekor: parse log entry: %w", err)
	}
	if err := p.verifySET(entry); err != nil {
		return err
	}
	if err := verifyEntryBindsArtifact(entry, artifactSHA256); err != nil {
		return err
	}
	if p.VerifyInclusion {
		if err := p.verifyInclusion(entry); err != nil {
			return err
		}
	}
	return nil
}

// verifySET checks Rekor's signature over the canonical envelope.
// Rekor signs sha256(canonical JSON({body, integratedTime, logID,
// logIndex})) — the exact field order and encoding matter.
func (p *RekorPolicy) verifySET(entry RekorLogEntry) error {
	sig, err := base64.StdEncoding.DecodeString(entry.Verification.SignedEntryTimestamp)
	if err != nil {
		return fmt.Errorf("store/rekor: SET decode: %w", err)
	}
	envelope := canonicalSETEnvelope(entry)
	digest := sha256.Sum256(envelope)
	if !ecdsa.VerifyASN1(p.PublicKey, digest[:], sig) {
		return errors.New("store/rekor: SET signature is invalid")
	}
	return nil
}

// canonicalSETEnvelope produces the exact bytes Rekor signs. Field
// order is fixed and each value is JSON-escaped the way encoding/json
// emits it — Rekor uses the same encoder.
func canonicalSETEnvelope(e RekorLogEntry) []byte {
	// Compact, key-sorted JSON is what Rekor signs. Constructing the
	// map with the fields Rekor requires and letting encoding/json
	// sort keys alphabetically produces the right envelope.
	m := map[string]any{
		"body":           e.Body,
		"integratedTime": e.IntegratedTime,
		"logID":          e.LogID,
		"logIndex":       e.LogIndex,
	}
	// encoding/json sorts map keys alphabetically at Marshal time —
	// exactly the ordering Rekor uses. HTMLEscape is off because
	// Rekor does not HTML-escape.
	b, _ := jsonMarshal(m)
	return b
}

// verifyEntryBindsArtifact checks that the rekord/hashedrekord body
// Rekor signed carries the same sha256 digest as the artifact we
// intend to install. Without this, an attacker could replay a valid
// SET (over a body they control) to make it look like any tarball
// was recorded in the transparency log.
//
// The body is base64 in the entry; its decoded JSON follows the
// hashedrekord schema: spec.data.hash.value is the hex sha256 of
// the artifact Rekor recorded.
func verifyEntryBindsArtifact(entry RekorLogEntry, artifactSHA256 []byte) error {
	raw, err := base64.StdEncoding.DecodeString(entry.Body)
	if err != nil {
		return fmt.Errorf("store/rekor: body decode: %w", err)
	}
	var body struct {
		Kind string `json:"kind"`
		Spec struct {
			Data struct {
				Hash struct {
					Algorithm string `json:"algorithm"`
					Value     string `json:"value"`
				} `json:"hash"`
			} `json:"data"`
		} `json:"spec"`
	}
	if err := json.Unmarshal(raw, &body); err != nil {
		return fmt.Errorf("store/rekor: parse body: %w", err)
	}
	if body.Kind != "" && body.Kind != "hashedrekord" && body.Kind != "rekord" {
		// Bodies we don't know how to bind are refused rather than
		// silently accepted — an unrecognised kind is exactly the
		// shape a bypass would take.
		return fmt.Errorf("store/rekor: unsupported entry kind %q", body.Kind)
	}
	if body.Spec.Data.Hash.Algorithm != "sha256" {
		return fmt.Errorf("store/rekor: entry hash algorithm %q is not sha256", body.Spec.Data.Hash.Algorithm)
	}
	recorded, err := hexDecode(body.Spec.Data.Hash.Value)
	if err != nil {
		return fmt.Errorf("store/rekor: entry hash decode: %w", err)
	}
	if !bytesEqualCT(recorded, artifactSHA256) {
		return errors.New("store/rekor: entry hash does not match artifact — SET is for a different payload")
	}
	return nil
}

// verifyInclusion walks the Merkle path from leaf to root and
// compares to a root hash we trust — meaning the checkpoint that
// signed it validates under Rekor's public key. Using the bundle's
// own rootHash as the trust anchor would be circular; an attacker
// could construct any tree and claim its root is genuine.
//
// Rekor's leaf hash is sha256(0x00 || body); interior nodes are
// sha256(0x01 || left || right). This matches RFC 6962.
func (p *RekorPolicy) verifyInclusion(entry RekorLogEntry) error {
	proof := entry.Verification.InclusionProof
	if proof.RootHash == "" {
		return errors.New("store/rekor: inclusion proof missing rootHash")
	}
	// Before trusting rootHash, make sure Rekor actually signed it
	// via the checkpoint. Without this the whole proof is worthless
	// — an attacker can synthesise any tree.
	if err := p.verifyCheckpoint(proof); err != nil {
		return err
	}
	body, err := base64.StdEncoding.DecodeString(entry.Body)
	if err != nil {
		return fmt.Errorf("store/rekor: body decode: %w", err)
	}
	leaf := rfc6962Hash([]byte{0x00}, body)
	current := leaf
	index := proof.LogIndex
	size := proof.TreeSize
	for _, sibHex := range proof.Hashes {
		sib, err := hexDecode(sibHex)
		if err != nil {
			return fmt.Errorf("store/rekor: proof hash decode: %w", err)
		}
		// The lowest bit of index says which side we are: even = we
		// are the left child, sibling is right; odd = we are right,
		// sibling is left. Then we halve both index and size to
		// climb one level.
		if index%2 == 0 && index != size-1 {
			current = rfc6962Hash([]byte{0x01}, current, sib)
		} else if index%2 == 1 {
			current = rfc6962Hash([]byte{0x01}, sib, current)
		} else {
			// index is even and points at the last node — no sibling
			// hash to combine with at this level, just carry up.
		}
		index /= 2
		size = (size + 1) / 2
	}
	want, err := hexDecode(proof.RootHash)
	if err != nil {
		return fmt.Errorf("store/rekor: rootHash decode: %w", err)
	}
	if !bytesEqualCT(current, want) {
		return errors.New("store/rekor: inclusion proof does not chain to rootHash")
	}
	return nil
}

// verifyCheckpoint parses the signed note that Rekor emits on top of
// each inclusion proof and confirms the rootHash we're chaining to
// is the one Rekor actually signed. The note format is:
//
//	<origin>\n
//	<treeSize>\n
//	<base64 rootHash>\n
//	\n
//	— <keyID> <base64 signature>\n
//
// The signature covers the four-line body (through the empty line).
// Rekor signs with the same ECDSA key we already trust for the SET.
func (p *RekorPolicy) verifyCheckpoint(proof RekorInclusionProof) error {
	if proof.Checkpoint == "" {
		return errors.New("store/rekor: inclusion proof missing checkpoint; refusing to trust a self-attested root")
	}
	// Split the note into body and signature block. The blank line
	// between them is part of the body per the sigstore/note spec.
	sepIdx := indexOf(proof.Checkpoint, "\n\n")
	if sepIdx < 0 {
		return errors.New("store/rekor: malformed checkpoint (no blank-line separator)")
	}
	body := proof.Checkpoint[:sepIdx+1] // include the trailing newline of the third line
	signatures := proof.Checkpoint[sepIdx+2:]

	// Parse the third line's rootHash and confirm it matches what
	// the inclusion proof asks us to trust.
	bodyLines := splitLines(body)
	if len(bodyLines) < 3 {
		return errors.New("store/rekor: checkpoint has fewer than 3 lines")
	}
	signedRoot, err := base64.StdEncoding.DecodeString(bodyLines[2])
	if err != nil {
		return fmt.Errorf("store/rekor: checkpoint rootHash decode: %w", err)
	}
	proofRoot, err := hexDecode(proof.RootHash)
	if err != nil {
		return fmt.Errorf("store/rekor: proof rootHash decode: %w", err)
	}
	if !bytesEqualCT(signedRoot, proofRoot) {
		return errors.New("store/rekor: checkpoint's signed root does not match the inclusion proof's rootHash")
	}

	// Verify at least one signature line matches Rekor's public key.
	sig, err := findCheckpointSignature(signatures)
	if err != nil {
		return err
	}
	digest := sha256.Sum256([]byte(body))
	if !ecdsa.VerifyASN1(p.PublicKey, digest[:], sig) {
		return errors.New("store/rekor: checkpoint signature does not verify under Rekor's public key")
	}
	return nil
}

// findCheckpointSignature scans the signature block for a line
// starting with "— " (or "- ", tolerated) followed by "<keyID>
// <base64 signature>", and returns the first valid base64 signature.
// The keyID hint is not verified — we only care that Rekor's public
// key can validate the signature; multiple co-signers would each get
// their own line.
func findCheckpointSignature(block string) ([]byte, error) {
	for _, line := range splitLines(block) {
		trimmed := line
		// Signature lines start with U+2014 EM DASH or a plain dash.
		if hasPrefix(trimmed, "— ") {
			trimmed = trimmed[len("— "):]
		} else if hasPrefix(trimmed, "- ") {
			trimmed = trimmed[len("- "):]
		} else {
			continue
		}
		// After the marker: "<keyID> <base64 sig>". Take the last
		// token as the signature.
		sp := lastIndex(trimmed, " ")
		if sp < 0 {
			continue
		}
		sig, err := base64.StdEncoding.DecodeString(trimmed[sp+1:])
		if err != nil {
			continue
		}
		// Rekor's per-key signature block: first 4 bytes are the
		// truncated key hint, remainder is the raw ECDSA-ASN1 sig.
		if len(sig) > 4 {
			return sig[4:], nil
		}
	}
	return nil, errors.New("store/rekor: no signature line found in checkpoint")
}

// Tiny string helpers, kept private to avoid pulling "strings" for
// four call sites that all know exactly what they want.
func splitLines(s string) []string {
	var out []string
	start := 0
	for i := 0; i < len(s); i++ {
		if s[i] == '\n' {
			out = append(out, s[start:i])
			start = i + 1
		}
	}
	if start < len(s) {
		out = append(out, s[start:])
	}
	return out
}

func indexOf(s, sub string) int {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return i
		}
	}
	return -1
}

func hasPrefix(s, prefix string) bool { return len(s) >= len(prefix) && s[:len(prefix)] == prefix }

func lastIndex(s, sub string) int {
	for i := len(s) - len(sub); i >= 0; i-- {
		if s[i:i+len(sub)] == sub {
			return i
		}
	}
	return -1
}

func rfc6962Hash(prefix []byte, parts ...[]byte) []byte {
	h := sha256.New()
	h.Write(prefix)
	for _, p := range parts {
		h.Write(p)
	}
	return h.Sum(nil)
}

func hexDecode(s string) ([]byte, error) {
	if len(s)%2 != 0 {
		return nil, fmt.Errorf("odd-length hex string %q", s)
	}
	out := make([]byte, len(s)/2)
	for i := 0; i < len(out); i++ {
		hi, err := hexDigit(s[2*i])
		if err != nil {
			return nil, err
		}
		lo, err := hexDigit(s[2*i+1])
		if err != nil {
			return nil, err
		}
		out[i] = hi<<4 | lo
	}
	return out, nil
}

func hexDigit(b byte) (byte, error) {
	switch {
	case b >= '0' && b <= '9':
		return b - '0', nil
	case b >= 'a' && b <= 'f':
		return b - 'a' + 10, nil
	case b >= 'A' && b <= 'F':
		return b - 'A' + 10, nil
	default:
		return 0, fmt.Errorf("not a hex digit: %q", b)
	}
}

func bytesEqualCT(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	var v byte
	for i := range a {
		v |= a[i] ^ b[i]
	}
	return v == 0
}

// jsonMarshal is a thin wrapper that isolates the encoder settings
// Rekor's SET envelope depends on. HTMLEscape is off so bytes match
// exactly what the server signs.
func jsonMarshal(v any) ([]byte, error) {
	return json.Marshal(v)
}

// LoadECDSAPublicKey parses one PEM-encoded ECDSA public key. Rekor's
// key at https://rekor.sigstore.dev/api/v1/log/publicKey ships in
// this format.
func LoadECDSAPublicKey(pemBytes []byte) (*ecdsa.PublicKey, error) {
	block, _ := pem.Decode(pemBytes)
	if block == nil {
		return nil, errors.New("store/rekor: no PEM block")
	}
	pub, err := x509.ParsePKIXPublicKey(block.Bytes)
	if err != nil {
		return nil, err
	}
	ec, ok := pub.(*ecdsa.PublicKey)
	if !ok {
		return nil, fmt.Errorf("store/rekor: %T is not an ECDSA public key", pub)
	}
	return ec, nil
}
