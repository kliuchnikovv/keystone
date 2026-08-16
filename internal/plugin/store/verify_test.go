package store

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/pem"
	"net"
	"testing"
)

func TestSHA256Verifier_NoOp(t *testing.T) {
	if err := (SHA256Verifier{}).Verify([]byte("payload"), Package{}); err != nil {
		t.Fatalf("SHA256Verifier should be a no-op, got %v", err)
	}
}

func TestEd25519Verifier_HappyPath(t *testing.T) {
	pub, priv, _ := ed25519.GenerateKey(rand.Reader)
	payload := []byte("plugin tarball bytes")
	digest := sha256.Sum256(payload)
	sig := ed25519.Sign(priv, digest[:])

	v := Ed25519Verifier{Keys: []ed25519.PublicKey{pub}}
	err := v.Verify(payload, Package{Signature: base64.StdEncoding.EncodeToString(sig)})
	if err != nil {
		t.Fatalf("unexpected: %v", err)
	}
}

func TestEd25519Verifier_RejectsUnsigned(t *testing.T) {
	pub, _, _ := ed25519.GenerateKey(rand.Reader)
	v := Ed25519Verifier{Keys: []ed25519.PublicKey{pub}}
	if err := v.Verify([]byte("x"), Package{}); err == nil {
		t.Fatal("Ed25519Verifier must reject an unsigned package")
	}
}

func TestEd25519Verifier_RejectsWrongKey(t *testing.T) {
	_, priv, _ := ed25519.GenerateKey(rand.Reader)
	otherPub, _, _ := ed25519.GenerateKey(rand.Reader)

	payload := []byte("body")
	digest := sha256.Sum256(payload)
	sig := ed25519.Sign(priv, digest[:])

	v := Ed25519Verifier{Keys: []ed25519.PublicKey{otherPub}}
	if err := v.Verify(payload, Package{Signature: base64.StdEncoding.EncodeToString(sig)}); err == nil {
		t.Fatal("signature under an untrusted key must be refused")
	}
}

func TestLoadEd25519PublicKeys_Roundtrip(t *testing.T) {
	pub, _, _ := ed25519.GenerateKey(rand.Reader)
	der, err := x509.MarshalPKIXPublicKey(pub)
	if err != nil {
		t.Fatal(err)
	}
	pemBytes := pem.EncodeToMemory(&pem.Block{Type: "PUBLIC KEY", Bytes: der})

	got, err := LoadEd25519PublicKeys(pemBytes)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 {
		t.Fatalf("want 1 key, got %d", len(got))
	}
	if string(got[0]) != string(pub) {
		t.Fatal("key round-trip mismatch")
	}
}

func TestValidateURL_RejectsSchemesAndSchemes(t *testing.T) {
	cases := map[string]bool{
		"":                                    false,
		"file:///etc/passwd":                  false,
		"ftp://example.com":                   false,
		"http://example.com":                  false, // non-loopback http refused
		"http://localhost:18000":              true,
		"http://127.0.0.1:80":                 true,
		"https://example.com":                 true,
		"https://":                            false, // missing host
	}
	for u, ok := range cases {
		_, err := validateURL(u)
		if ok && err != nil {
			t.Errorf("validateURL(%q) rejected but should accept: %v", u, err)
		}
		if !ok && err == nil {
			t.Errorf("validateURL(%q) accepted but should reject", u)
		}
	}
}

func TestCheckResolvedIP_RejectsSSRFRanges(t *testing.T) {
	for _, ip := range []string{"0.0.0.0", "169.254.169.254", "10.0.0.1", "192.168.1.1", "172.16.0.1"} {
		if err := checkResolvedIP(parseIP(t, ip), false); err == nil {
			t.Errorf("checkResolvedIP(%s) should have refused", ip)
		}
	}
	// Loopback allowed only when the caller opts in.
	if err := checkResolvedIP(parseIP(t, "127.0.0.1"), false); err == nil {
		t.Error("loopback should be refused without allowLoopback")
	}
	if err := checkResolvedIP(parseIP(t, "127.0.0.1"), true); err != nil {
		t.Errorf("loopback with allowLoopback should pass: %v", err)
	}
}

func parseIP(t *testing.T, s string) net.IP {
	t.Helper()
	ip := net.ParseIP(s)
	if ip == nil {
		t.Fatalf("parse %s", s)
	}
	return ip
}
