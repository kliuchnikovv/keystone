package tuf

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// mkRepo spins up a mini TUF repository backed by httptest. All four
// roles share one ed25519 signing key — plenty for a threshold-1
// verification test.
type mkRepo struct {
	t       *testing.T
	priv    ed25519.PrivateKey
	pub     ed25519.PublicKey
	keyID   string
	targets map[string][]byte
}

func newRepo(t *testing.T) *mkRepo {
	t.Helper()
	pub, priv, _ := ed25519.GenerateKey(rand.Reader)
	return &mkRepo{
		t:       t,
		priv:    priv,
		pub:     pub,
		keyID:   "k1",
		targets: map[string][]byte{},
	}
}

func (r *mkRepo) addTarget(name string, body []byte) {
	r.targets[name] = body
}

func (r *mkRepo) rootBytes(version int, expires time.Time) []byte {
	spec := RootSpec{
		Type: "root", Version: version, Expires: expires.Format(time.RFC3339),
		Keys: map[string]Key{
			r.keyID: {Type: "ed25519", Scheme: "ed25519", Values: KeyValue{Public: hex.EncodeToString(r.pub)}},
		},
		Roles: map[string]Role{
			"root":      {KeyIDs: []string{r.keyID}, Threshold: 1},
			"timestamp": {KeyIDs: []string{r.keyID}, Threshold: 1},
			"snapshot":  {KeyIDs: []string{r.keyID}, Threshold: 1},
			"targets":   {KeyIDs: []string{r.keyID}, Threshold: 1},
		},
	}
	return r.sign(spec)
}

func (r *mkRepo) targetsBytes(version int, expires time.Time) []byte {
	tg := map[string]Target{}
	for name, body := range r.targets {
		sum := sha256.Sum256(body)
		tg[name] = Target{Length: int64(len(body)), Hashes: map[string]string{"sha256": hex.EncodeToString(sum[:])}}
	}
	spec := TargetsSpec{
		Type: "targets", Version: version, Expires: expires.Format(time.RFC3339),
		Targets: tg,
	}
	return r.sign(spec)
}

func (r *mkRepo) snapshotBytes(version, targetsVersion int, expires time.Time) []byte {
	spec := SnapshotSpec{
		Type: "snapshot", Version: version, Expires: expires.Format(time.RFC3339),
		Meta: map[string]Meta{"targets.json": {Version: targetsVersion}},
	}
	return r.sign(spec)
}

func (r *mkRepo) timestampBytes(version, snapshotVersion int, expires time.Time) []byte {
	spec := TimestampSpec{
		Type: "timestamp", Version: version, Expires: expires.Format(time.RFC3339),
		Meta: map[string]Meta{"snapshot.json": {Version: snapshotVersion}},
	}
	return r.sign(spec)
}

// sign builds an envelope: canonical(spec) → sign → wrap.
func (r *mkRepo) sign(v any) []byte {
	r.t.Helper()
	body, err := json.Marshal(v)
	if err != nil {
		r.t.Fatal(err)
	}
	sig := ed25519.Sign(r.priv, body)
	env := envelope{
		Signed:     body,
		Signatures: []Signature{{KeyID: r.keyID, Sig: hex.EncodeToString(sig)}},
	}
	out, _ := json.Marshal(env)
	return out
}

func (r *mkRepo) serve(t *testing.T) *httptest.Server {
	t.Helper()
	future := time.Now().Add(24 * time.Hour)
	targetsV := 1
	snapshotV := 1
	timestampV := 1

	handler := http.NewServeMux()
	handler.HandleFunc("/timestamp.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(r.timestampBytes(timestampV, snapshotV, future))
	})
	handler.HandleFunc("/1.snapshot.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(r.snapshotBytes(snapshotV, targetsV, future))
	})
	handler.HandleFunc("/1.targets.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(r.targetsBytes(targetsV, future))
	})
	handler.HandleFunc("/targets/", func(w http.ResponseWriter, req *http.Request) {
		// Serve either /targets/<sha>.<name> or /targets/<name>.
		path := strings.TrimPrefix(req.URL.Path, "/targets/")
		for name, body := range r.targets {
			if path == name || strings.HasSuffix(path, "."+name) {
				_, _ = w.Write(body)
				return
			}
		}
		http.Error(w, "not found", 404)
	})
	return httptest.NewServer(handler)
}

func TestRefresh_HappyPath(t *testing.T) {
	repo := newRepo(t)
	repo.addTarget("rekor.pub", []byte("rekor-public-key-bytes"))
	repo.addTarget("fulcio_v1.crt.pem", []byte("fulcio-root-cert-bytes"))
	srv := repo.serve(t)
	defer srv.Close()

	rootJSON := repo.rootBytes(1, time.Now().Add(24*time.Hour))
	c, err := New(rootJSON)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if err := c.Refresh(context.Background(), srv.Client(), srv.URL); err != nil {
		t.Fatalf("Refresh: %v", err)
	}

	got, err := c.Fetch(context.Background(), srv.Client(), srv.URL, "rekor.pub")
	if err != nil {
		t.Fatalf("Fetch: %v", err)
	}
	if string(got) != "rekor-public-key-bytes" {
		t.Fatalf("payload = %s", got)
	}
}

func TestNew_RejectsExpiredRoot(t *testing.T) {
	repo := newRepo(t)
	rootJSON := repo.rootBytes(1, time.Now().Add(-time.Hour))
	if _, err := New(rootJSON); err == nil {
		t.Fatal("expired root must be refused")
	}
}

func TestNew_RejectsBadSignature(t *testing.T) {
	repo := newRepo(t)
	rootJSON := repo.rootBytes(1, time.Now().Add(24*time.Hour))
	// Flip a byte in the signature block by removing the last two
	// hex chars from the sig — malformed but still parseable JSON.
	corrupted := strings.Replace(string(rootJSON), `"sig":"`, `"sig":"deadbeef`, 1)
	if _, err := New([]byte(corrupted)); err == nil {
		t.Fatal("bad-sig root must be refused")
	}
}

func TestRefresh_RejectsRolledBackTimestamp(t *testing.T) {
	repo := newRepo(t)
	repo.addTarget("x", []byte("y"))
	srv := repo.serve(t)
	defer srv.Close()

	rootJSON := repo.rootBytes(1, time.Now().Add(24*time.Hour))
	c, err := New(rootJSON)
	if err != nil {
		t.Fatal(err)
	}
	if err := c.Refresh(context.Background(), srv.Client(), srv.URL); err != nil {
		t.Fatal(err)
	}

	// A second refresh against a version-N timestamp fails: the
	// trusted version is already N.
	if err := c.Refresh(context.Background(), srv.Client(), srv.URL); err == nil {
		t.Fatal("replay of the same version must be refused")
	}
}

// rotateableRepo lets tests add a v2 root and serve both v1 and v2
// under their versioned URLs (1.root.json, 2.root.json, …).
type rotateableRepo struct {
	*mkRepo
	roots       map[int][]byte // version → wire bytes
	keys        map[string]ed25519.PrivateKey
	pubs        map[string]ed25519.PublicKey
	activeKeyID string
}

func newRotateableRepo(t *testing.T) *rotateableRepo {
	base := newRepo(t)
	return &rotateableRepo{
		mkRepo:      base,
		roots:       map[int][]byte{},
		keys:        map[string]ed25519.PrivateKey{base.keyID: base.priv},
		pubs:        map[string]ed25519.PublicKey{base.keyID: base.pub},
		activeKeyID: base.keyID,
	}
}

// signWithKeys signs body with a specific set of key IDs, so tests
// can build cross-signed rotations.
func (r *rotateableRepo) signWithKeys(body []byte, keyIDs []string) []byte {
	r.t.Helper()
	env := envelope{Signed: body}
	for _, id := range keyIDs {
		priv := r.keys[id]
		sig := ed25519.Sign(priv, body)
		env.Signatures = append(env.Signatures, Signature{KeyID: id, Sig: hex.EncodeToString(sig)})
	}
	out, _ := json.Marshal(env)
	return out
}

// addKey generates a new ed25519 key with the given ID so tests can
// build multi-key rotations.
func (r *rotateableRepo) addKey(id string) {
	pub, priv, _ := ed25519.GenerateKey(rand.Reader)
	r.keys[id] = priv
	r.pubs[id] = pub
}

// rootSpecWithKey builds a root spec whose all roles are tied to a
// single key. rotationHelper wraps the whole thing.
func (r *rotateableRepo) rootSpecWithKey(version int, expires time.Time, keyID string) RootSpec {
	return RootSpec{
		Type: "root", Version: version, Expires: expires.Format(time.RFC3339),
		Keys: map[string]Key{
			keyID: {Type: "ed25519", Scheme: "ed25519", Values: KeyValue{Public: hex.EncodeToString(r.pubs[keyID])}},
		},
		Roles: map[string]Role{
			"root":      {KeyIDs: []string{keyID}, Threshold: 1},
			"timestamp": {KeyIDs: []string{keyID}, Threshold: 1},
			"snapshot":  {KeyIDs: []string{keyID}, Threshold: 1},
			"targets":   {KeyIDs: []string{keyID}, Threshold: 1},
		},
	}
}

func (r *rotateableRepo) serveWithRoots(t *testing.T, extraRoots map[int][]byte) *httptest.Server {
	t.Helper()
	future := time.Now().Add(24 * time.Hour)
	handler := http.NewServeMux()
	handler.HandleFunc("/timestamp.json", func(w http.ResponseWriter, _ *http.Request) {
		body, _ := json.Marshal(TimestampSpec{
			Type: "timestamp", Version: 1, Expires: future.Format(time.RFC3339),
			Meta: map[string]Meta{"snapshot.json": {Version: 1}},
		})
		out := r.signWithKeys(body, []string{r.activeKeyID})
		_, _ = w.Write(out)
	})
	handler.HandleFunc("/1.snapshot.json", func(w http.ResponseWriter, _ *http.Request) {
		body, _ := json.Marshal(SnapshotSpec{
			Type: "snapshot", Version: 1, Expires: future.Format(time.RFC3339),
			Meta: map[string]Meta{"targets.json": {Version: 1}},
		})
		out := r.signWithKeys(body, []string{r.activeKeyID})
		_, _ = w.Write(out)
	})
	handler.HandleFunc("/1.targets.json", func(w http.ResponseWriter, _ *http.Request) {
		body, _ := json.Marshal(TargetsSpec{
			Type: "targets", Version: 1, Expires: future.Format(time.RFC3339),
			Targets: map[string]Target{},
		})
		out := r.signWithKeys(body, []string{r.activeKeyID})
		_, _ = w.Write(out)
	})
	for v, blob := range extraRoots {
		v, blob := v, blob
		handler.HandleFunc(fmt.Sprintf("/%d.root.json", v), func(w http.ResponseWriter, _ *http.Request) {
			_, _ = w.Write(blob)
		})
	}
	return httptest.NewServer(handler)
}

func TestRefresh_RootRotation_HappyPath(t *testing.T) {
	repo := newRotateableRepo(t)
	repo.addKey("k2")
	future := time.Now().Add(24 * time.Hour)

	v1Spec := repo.rootSpecWithKey(1, future, "k1")
	v1Body, _ := json.Marshal(v1Spec)
	v1JSON := repo.signWithKeys(v1Body, []string{"k1"})

	// v2 introduces k2; it MUST be signed by both k1 (old) and k2
	// (new) for the rotation to be accepted.
	v2Spec := repo.rootSpecWithKey(2, future, "k2")
	v2Body, _ := json.Marshal(v2Spec)
	v2JSON := repo.signWithKeys(v2Body, []string{"k1", "k2"})

	srv := repo.serveWithRoots(t, map[int][]byte{2: v2JSON})
	defer srv.Close()

	c, err := New(v1JSON)
	if err != nil {
		t.Fatal(err)
	}
	// Client starts at v1; refresh should rotate forward and land
	// on v2. After rotation, timestamp/snapshot/targets sign under
	// k2, so we switch the active key on the repo before refresh
	// completes those.
	repo.activeKeyID = "k2"
	if err := c.Refresh(context.Background(), srv.Client(), srv.URL); err != nil {
		t.Fatalf("Refresh with rotation: %v", err)
	}
	if c.root.Signed.Version != 2 {
		t.Fatalf("expected root v2 after rotation, got v%d", c.root.Signed.Version)
	}
}

func TestRefresh_RootRotation_RejectsMissingCurrentSignature(t *testing.T) {
	repo := newRotateableRepo(t)
	repo.addKey("k2")
	future := time.Now().Add(24 * time.Hour)

	v1Spec := repo.rootSpecWithKey(1, future, "k1")
	v1Body, _ := json.Marshal(v1Spec)
	v1JSON := repo.signWithKeys(v1Body, []string{"k1"})

	// v2 signed only by the NEW key — no continuity from v1. Must
	// be refused; otherwise anyone with a valid v2 key set could
	// hijack the client.
	v2Spec := repo.rootSpecWithKey(2, future, "k2")
	v2Body, _ := json.Marshal(v2Spec)
	v2JSON := repo.signWithKeys(v2Body, []string{"k2"})

	srv := repo.serveWithRoots(t, map[int][]byte{2: v2JSON})
	defer srv.Close()

	c, _ := New(v1JSON)
	if err := c.Refresh(context.Background(), srv.Client(), srv.URL); err == nil {
		t.Fatal("rotation without a signature from the current root must be refused")
	}
}

func TestRefresh_RootRotation_RejectsMissingSelfSignature(t *testing.T) {
	repo := newRotateableRepo(t)
	repo.addKey("k2")
	future := time.Now().Add(24 * time.Hour)

	v1Spec := repo.rootSpecWithKey(1, future, "k1")
	v1Body, _ := json.Marshal(v1Spec)
	v1JSON := repo.signWithKeys(v1Body, []string{"k1"})

	// v2 claims k2 is its root key but does NOT sign with k2. The
	// old root's signature would otherwise let it install any
	// spec — an attacker who owns the old root could pin a
	// signing key they don't control.
	v2Spec := repo.rootSpecWithKey(2, future, "k2")
	v2Body, _ := json.Marshal(v2Spec)
	v2JSON := repo.signWithKeys(v2Body, []string{"k1"})

	srv := repo.serveWithRoots(t, map[int][]byte{2: v2JSON})
	defer srv.Close()

	c, _ := New(v1JSON)
	if err := c.Refresh(context.Background(), srv.Client(), srv.URL); err == nil {
		t.Fatal("rotation without a self-signature must be refused")
	}
}

func TestRefresh_RootRotation_RejectsVersionSkip(t *testing.T) {
	repo := newRotateableRepo(t)
	future := time.Now().Add(24 * time.Hour)

	v1Spec := repo.rootSpecWithKey(1, future, "k1")
	v1Body, _ := json.Marshal(v1Spec)
	v1JSON := repo.signWithKeys(v1Body, []string{"k1"})

	// The repo has jumped straight to v3 without shipping v2. A
	// client walking from v1 fetches 2.root.json (404) and stops
	// — never trusts v3 without seeing v2's approval.
	v3Spec := repo.rootSpecWithKey(3, future, "k1")
	v3Body, _ := json.Marshal(v3Spec)
	v3JSON := repo.signWithKeys(v3Body, []string{"k1"})

	srv := repo.serveWithRoots(t, map[int][]byte{3: v3JSON})
	defer srv.Close()

	c, _ := New(v1JSON)
	if err := c.Refresh(context.Background(), srv.Client(), srv.URL); err != nil {
		// timestamp/snapshot/targets are still v1-signed — refresh
		// succeeds. What we care about is that root stays at v1.
		t.Fatalf("Refresh: %v", err)
	}
	if c.root.Signed.Version != 1 {
		t.Fatalf("root should stay at v1 when v2 is missing; got v%d", c.root.Signed.Version)
	}
}

func TestRefresh_RootRotation_StopsOn404(t *testing.T) {
	// The plain happy path (no v2 published) exercises this — the
	// walk fetches 2.root.json, sees 404, returns. The other
	// refresh tests already cover the no-rotation case; this test
	// codifies the intent so a future regression can't slip in.
	repo := newRotateableRepo(t)
	future := time.Now().Add(24 * time.Hour)
	v1Spec := repo.rootSpecWithKey(1, future, "k1")
	v1Body, _ := json.Marshal(v1Spec)
	v1JSON := repo.signWithKeys(v1Body, []string{"k1"})

	srv := repo.serveWithRoots(t, nil)
	defer srv.Close()

	c, _ := New(v1JSON)
	if err := c.Refresh(context.Background(), srv.Client(), srv.URL); err != nil {
		t.Fatalf("Refresh with no rotations: %v", err)
	}
	if c.root.Signed.Version != 1 {
		t.Fatalf("root moved unexpectedly: v%d", c.root.Signed.Version)
	}
}

func TestRefresh_RejectsExpiredRoot(t *testing.T) {
	// Root is loaded when it is still valid, then time jumps past
	// its expiry before we Refresh — a chain hanging off an expired
	// root cannot be trusted no matter how fresh the timestamp is.
	repo := newRepo(t)
	repo.addTarget("x", []byte("y"))
	srv := repo.serve(t)
	defer srv.Close()

	loadedAt := time.Now()
	rootJSON := repo.rootBytes(1, loadedAt.Add(time.Hour))
	c, err := New(rootJSON)
	if err != nil {
		t.Fatal(err)
	}
	c.WithClock(func() time.Time { return loadedAt.Add(2 * time.Hour) })
	if err := c.Refresh(context.Background(), srv.Client(), srv.URL); err == nil {
		t.Fatal("Refresh must refuse when the trusted root has expired")
	}
}

func TestRefresh_RejectsTimestampWithoutSnapshotMeta(t *testing.T) {
	repo := newRepo(t)
	future := time.Now().Add(24 * time.Hour)

	// Serve a timestamp whose meta map is empty. Every other role is
	// happy, but the missing snapshot.json entry means we can't
	// bind snapshot's version — must refuse rather than fail open.
	handler := http.NewServeMux()
	handler.HandleFunc("/timestamp.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(repo.sign(TimestampSpec{
			Type: "timestamp", Version: 1, Expires: future.Format(time.RFC3339),
			Meta: map[string]Meta{}, // intentionally empty
		}))
	})
	srv := httptest.NewServer(handler)
	defer srv.Close()

	rootJSON := repo.rootBytes(1, future)
	c, err := New(rootJSON)
	if err != nil {
		t.Fatal(err)
	}
	if err := c.Refresh(context.Background(), srv.Client(), srv.URL); err == nil {
		t.Fatal("timestamp without snapshot.json meta must be refused")
	}
}

func TestFetch_LengthMismatch(t *testing.T) {
	// Metadata declares Length N; server returns N+padding. Must be
	// refused — a length gate defends against streaming attacks and
	// makes the sha check happen on the exact bytes the author
	// signed off on.
	repo := newRepo(t)
	repo.addTarget("payload", []byte("original"))
	future := time.Now().Add(24 * time.Hour)

	handler := http.NewServeMux()
	handler.HandleFunc("/timestamp.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(repo.timestampBytes(1, 1, future))
	})
	handler.HandleFunc("/1.snapshot.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(repo.snapshotBytes(1, 1, future))
	})
	handler.HandleFunc("/1.targets.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(repo.targetsBytes(1, future))
	})
	handler.HandleFunc("/targets/", func(w http.ResponseWriter, req *http.Request) {
		// Serve the right content but with extra bytes appended.
		fmt.Fprint(w, "originalXXXXXXX")
	})
	srv := httptest.NewServer(handler)
	defer srv.Close()

	rootJSON := repo.rootBytes(1, future)
	c, _ := New(rootJSON)
	if err := c.Refresh(context.Background(), srv.Client(), srv.URL); err != nil {
		t.Fatal(err)
	}
	if _, err := c.Fetch(context.Background(), srv.Client(), srv.URL, "payload"); err == nil {
		t.Fatal("length mismatch must be refused")
	}
}

func TestFetch_ShaMismatch(t *testing.T) {
	repo := newRepo(t)
	repo.addTarget("payload", []byte("original"))
	// Serve the target with a corrupted body — the metadata still
	// records the original sha256, so Fetch should refuse.
	future := time.Now().Add(24 * time.Hour)
	handler := http.NewServeMux()
	handler.HandleFunc("/timestamp.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(repo.timestampBytes(1, 1, future))
	})
	handler.HandleFunc("/1.snapshot.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(repo.snapshotBytes(1, 1, future))
	})
	handler.HandleFunc("/1.targets.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(repo.targetsBytes(1, future))
	})
	handler.HandleFunc("/targets/", func(w http.ResponseWriter, req *http.Request) {
		fmt.Fprint(w, "TAMPERED")
	})
	srv := httptest.NewServer(handler)
	defer srv.Close()

	rootJSON := repo.rootBytes(1, future)
	c, _ := New(rootJSON)
	if err := c.Refresh(context.Background(), srv.Client(), srv.URL); err != nil {
		t.Fatal(err)
	}
	if _, err := c.Fetch(context.Background(), srv.Client(), srv.URL, "payload"); err == nil {
		t.Fatal("sha mismatch must be refused")
	}
}
