package store

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"testing"
)

// signSET signs the canonical Rekor envelope for one entry with key.
// Used both to build fixtures and to exercise verifySET.
func signSET(t *testing.T, key *ecdsa.PrivateKey, entry RekorLogEntry) string {
	t.Helper()
	env := canonicalSETEnvelope(entry)
	digest := sha256.Sum256(env)
	sig, err := ecdsa.SignASN1(rand.Reader, key, digest[:])
	if err != nil {
		t.Fatal(err)
	}
	return base64.StdEncoding.EncodeToString(sig)
}

// hashedrekordBody builds a Rekor hashedrekord entry body whose
// spec.data.hash.value matches the given artifact digest — so the
// entry claims to record a signature over that specific artifact.
func hashedrekordBody(digest []byte) string {
	body, _ := json.Marshal(map[string]any{
		"apiVersion": "0.0.1",
		"kind":       "hashedrekord",
		"spec": map[string]any{
			"data": map[string]any{
				"hash": map[string]any{
					"algorithm": "sha256",
					"value":     hex.EncodeToString(digest),
				},
			},
		},
	})
	return base64.StdEncoding.EncodeToString(body)
}

func TestRekor_SET_HappyPath(t *testing.T) {
	key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	digest := sha256.Sum256([]byte("payload"))
	entry := RekorLogEntry{
		Body:           hashedrekordBody(digest[:]),
		IntegratedTime: 1700000000,
		LogID:          "deadbeef",
		LogIndex:       42,
	}
	entry.Verification.SignedEntryTimestamp = signSET(t, key, entry)

	policy := &RekorPolicy{PublicKey: &key.PublicKey}
	if err := policy.verifySET(entry); err != nil {
		t.Fatalf("unexpected: %v", err)
	}
	if err := verifyEntryBindsArtifact(entry, digest[:]); err != nil {
		t.Fatalf("binding failed: %v", err)
	}
}

func TestRekor_RejectsMismatchedArtifact(t *testing.T) {
	// An entry that records a signature over payload A can't be
	// replayed to install payload B.
	otherDigest := sha256.Sum256([]byte("other payload"))
	entry := RekorLogEntry{Body: hashedrekordBody(otherDigest[:])}
	ours := sha256.Sum256([]byte("payload"))
	if err := verifyEntryBindsArtifact(entry, ours[:]); err == nil {
		t.Fatal("must refuse: log entry is over a different payload")
	}
}

func TestRekor_RejectsUnknownEntryKind(t *testing.T) {
	body, _ := json.Marshal(map[string]any{
		"apiVersion": "0.0.1",
		"kind":       "attestation",
	})
	entry := RekorLogEntry{Body: base64.StdEncoding.EncodeToString(body)}
	if err := verifyEntryBindsArtifact(entry, []byte("digest")); err == nil {
		t.Fatal("unrecognised kind must be refused, not silently allowed")
	}
}

func TestRekor_SET_RejectsTampered(t *testing.T) {
	key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	digest := sha256.Sum256([]byte("payload"))
	entry := RekorLogEntry{
		Body:           hashedrekordBody(digest[:]),
		IntegratedTime: 1700000000,
		LogID:          "deadbeef",
		LogIndex:       42,
	}
	entry.Verification.SignedEntryTimestamp = signSET(t, key, entry)

	entry.LogIndex = 99
	policy := &RekorPolicy{PublicKey: &key.PublicKey}
	if err := policy.verifySET(entry); err == nil {
		t.Fatal("tampered SET must be refused")
	}
}

func TestRekor_MissingLogEntry(t *testing.T) {
	key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	policy := &RekorPolicy{PublicKey: &key.PublicKey}
	if err := policy.verifyRekor(SigstoreBundle{}, nil); err == nil {
		t.Fatal("bundle without logEntry must be refused")
	}
}

// signCheckpoint produces the signed-note format Rekor emits above
// each inclusion proof so tests can drive verifyCheckpoint.
func signCheckpoint(t *testing.T, key *ecdsa.PrivateKey, rootHash []byte, size int64) string {
	t.Helper()
	body := "rekor.sigstore.dev - 12345\n" +
		fmt.Sprintf("%d\n", size) +
		base64.StdEncoding.EncodeToString(rootHash) + "\n"
	digest := sha256.Sum256([]byte(body))
	sig, err := ecdsa.SignASN1(rand.Reader, key, digest[:])
	if err != nil {
		t.Fatal(err)
	}
	// Rekor prefixes signatures with a 4-byte truncated key hint; a
	// zero prefix works fine for the test since findCheckpointSignature
	// only trims the first four bytes.
	prefixed := append([]byte{0, 0, 0, 0}, sig...)
	return body + "\n— rekor.sigstore.dev " + base64.StdEncoding.EncodeToString(prefixed) + "\n"
}

func TestVerifyInclusion_TwoLeaves_WithCheckpoint(t *testing.T) {
	rekorKey, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	digest := sha256.Sum256([]byte("payload"))
	bodyB64 := hashedrekordBody(digest[:])
	// Rekor's leaf hash is over the raw base64-decoded body.
	body, _ := base64.StdEncoding.DecodeString(bodyB64)

	leafA := rfc6962Hash([]byte{0x00}, body)
	leafB := rfc6962Hash([]byte{0x00}, []byte(`sibling`))
	root := rfc6962Hash([]byte{0x01}, leafA, leafB)

	entry := RekorLogEntry{Body: bodyB64}
	entry.Verification.InclusionProof = RekorInclusionProof{
		Hashes:     []string{hex.EncodeToString(leafB)},
		LogIndex:   0,
		TreeSize:   2,
		RootHash:   hex.EncodeToString(root),
		Checkpoint: signCheckpoint(t, rekorKey, root, 2),
	}
	policy := &RekorPolicy{PublicKey: &rekorKey.PublicKey}
	if err := policy.verifyInclusion(entry); err != nil {
		t.Fatalf("inclusion verify failed: %v", err)
	}
}

func TestVerifyInclusion_RejectsUnsignedRoot(t *testing.T) {
	// A bundle without a checkpoint cannot be trusted — the root
	// hash is attacker-controlled.
	sum := sha256.Sum256([]byte("payload"))
	body := hashedrekordBody(sum[:])
	entry := RekorLogEntry{Body: body}
	entry.Verification.InclusionProof = RekorInclusionProof{
		Hashes:   []string{},
		LogIndex: 0,
		TreeSize: 1,
		RootHash: hex.EncodeToString(make([]byte, 32)),
	}
	policy := &RekorPolicy{PublicKey: nil}
	if err := policy.verifyInclusion(entry); err == nil {
		t.Fatal("missing checkpoint must be refused")
	}
}

func TestVerifyInclusion_RejectsBadCheckpointSignature(t *testing.T) {
	rekorKey, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	otherKey, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	digest := sha256.Sum256([]byte("payload"))
	bodyB64 := hashedrekordBody(digest[:])
	body, _ := base64.StdEncoding.DecodeString(bodyB64)
	leaf := rfc6962Hash([]byte{0x00}, body)

	entry := RekorLogEntry{Body: bodyB64}
	entry.Verification.InclusionProof = RekorInclusionProof{
		Hashes:     []string{},
		LogIndex:   0,
		TreeSize:   1,
		RootHash:   hex.EncodeToString(leaf),
		Checkpoint: signCheckpoint(t, otherKey, leaf, 1),
	}
	policy := &RekorPolicy{PublicKey: &rekorKey.PublicKey}
	if err := policy.verifyInclusion(entry); err == nil {
		t.Fatal("must refuse: checkpoint signed by an unknown key")
	}
}

func TestSigstoreVerifier_WithRekor(t *testing.T) {
	// End-to-end: sign a package with a mini-CA, then wrap with a
	// Rekor SET signed by a mini-Rekor key.
	setup := newSigningSetup(t, []string{"author@example.com"})
	payload := []byte("plugin tarball")
	bundleJSON := setup.bundle(t, payload)
	var bundle SigstoreBundle
	if err := json.Unmarshal([]byte(bundleJSON), &bundle); err != nil {
		t.Fatal(err)
	}

	rekorKey, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	pdigest := sha256.Sum256(payload)
	entry := RekorLogEntry{
		Body:           hashedrekordBody(pdigest[:]),
		IntegratedTime: 1700000000,
		LogID:          "deadbeef",
		LogIndex:       42,
	}
	entry.Verification.SignedEntryTimestamp = signSET(t, rekorKey, entry)
	entryBytes, _ := json.Marshal(entry)
	bundle.LogEntry = base64.StdEncoding.EncodeToString(entryBytes)
	bundleBytes, _ := json.Marshal(bundle)

	v := SigstoreVerifier{
		Roots:             []*x509.Certificate{setup.rootCert},
		AllowedIdentities: []string{"author@example.com"},
		Rekor:             &RekorPolicy{PublicKey: &rekorKey.PublicKey},
	}
	if err := v.Verify(payload, Package{Bundle: string(bundleBytes)}); err != nil {
		t.Fatalf("with-rekor verify: %v", err)
	}
}

func TestLoadECDSAPublicKey_Roundtrip(t *testing.T) {
	key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	der, err := x509.MarshalPKIXPublicKey(&key.PublicKey)
	if err != nil {
		t.Fatal(err)
	}
	pemBytes := pem.EncodeToMemory(&pem.Block{Type: "PUBLIC KEY", Bytes: der})
	got, err := LoadECDSAPublicKey(pemBytes)
	if err != nil {
		t.Fatal(err)
	}
	// Compare via marshalled DER — avoids poking .X/.Y directly,
	// which the standard library flags as unsafe for cryptographic
	// values.
	roundtrip, err := x509.MarshalPKIXPublicKey(got)
	if err != nil {
		t.Fatal(err)
	}
	if string(roundtrip) != string(der) {
		t.Fatal("key round-trip mismatch")
	}
}
