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

func TestRekor_SET_HappyPath(t *testing.T) {
	key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	entry := RekorLogEntry{
		Body:           base64.StdEncoding.EncodeToString([]byte(`{"apiVersion":"0.0.1"}`)),
		IntegratedTime: 1700000000,
		LogID:          "deadbeef",
		LogIndex:       42,
	}
	entry.Verification.SignedEntryTimestamp = signSET(t, key, entry)

	policy := &RekorPolicy{PublicKey: &key.PublicKey}
	if err := policy.verifySET(entry); err != nil {
		t.Fatalf("unexpected: %v", err)
	}
}

func TestRekor_SET_RejectsTampered(t *testing.T) {
	key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	entry := RekorLogEntry{
		Body:           base64.StdEncoding.EncodeToString([]byte(`{"apiVersion":"0.0.1"}`)),
		IntegratedTime: 1700000000,
		LogID:          "deadbeef",
		LogIndex:       42,
	}
	entry.Verification.SignedEntryTimestamp = signSET(t, key, entry)

	// Tamper: bump LogIndex after signing.
	entry.LogIndex = 99
	policy := &RekorPolicy{PublicKey: &key.PublicKey}
	if err := policy.verifySET(entry); err == nil {
		t.Fatal("tampered SET must be refused")
	}
}

func TestRekor_MissingLogEntry(t *testing.T) {
	key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	policy := &RekorPolicy{PublicKey: &key.PublicKey}
	if err := policy.verifyRekor(SigstoreBundle{}); err == nil {
		t.Fatal("bundle without logEntry must be refused")
	}
}

func TestVerifyInclusion_TwoLeaves(t *testing.T) {
	// Build a tiny Merkle tree with two leaves and verify inclusion
	// for the first one. Keeps the algorithm honest without a
	// heavyweight fixture.
	body := []byte(`{"apiVersion":"0.0.1"}`)
	bodyB64 := base64.StdEncoding.EncodeToString(body)

	leafA := rfc6962Hash([]byte{0x00}, body)
	leafB := rfc6962Hash([]byte{0x00}, []byte(`{"apiVersion":"0.0.1","x":"b"}`))
	root := rfc6962Hash([]byte{0x01}, leafA, leafB)

	entry := RekorLogEntry{Body: bodyB64}
	entry.Verification.InclusionProof = RekorInclusionProof{
		Hashes:   []string{hex.EncodeToString(leafB)},
		LogIndex: 0,
		TreeSize: 2,
		RootHash: hex.EncodeToString(root),
	}
	if err := verifyInclusion(entry); err != nil {
		t.Fatalf("inclusion verify failed: %v", err)
	}
}

func TestVerifyInclusion_WrongRoot(t *testing.T) {
	body := []byte(`x`)
	bodyB64 := base64.StdEncoding.EncodeToString(body)

	// A random 32-byte root the entry cannot possibly chain to.
	var bogus [32]byte
	if _, err := rand.Read(bogus[:]); err != nil {
		t.Fatal(err)
	}
	entry := RekorLogEntry{Body: bodyB64}
	entry.Verification.InclusionProof = RekorInclusionProof{
		Hashes:   []string{hex.EncodeToString(bogus[:])},
		LogIndex: 0,
		TreeSize: 2,
		RootHash: hex.EncodeToString(bogus[:]),
	}
	if err := verifyInclusion(entry); err == nil {
		t.Fatal("expected inclusion failure against a bogus root")
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
	entry := RekorLogEntry{
		Body:           base64.StdEncoding.EncodeToString([]byte(`{"apiVersion":"0.0.1"}`)),
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
