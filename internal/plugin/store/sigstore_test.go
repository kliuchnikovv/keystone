package store

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"math/big"
	"net/url"
	"testing"
	"time"
)

// signingSetup builds a tiny CA that stands in for Fulcio during the
// test — one root, one signing certificate with the given SAN
// identities, and a bundle over the given payload.
type signingSetup struct {
	rootCert  *x509.Certificate
	signCert  *x509.Certificate
	signKey   *ecdsa.PrivateKey
}

func newSigningSetup(t *testing.T, identities []string) *signingSetup {
	t.Helper()
	// Root CA
	rootKey, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	rootTmpl := &x509.Certificate{
		SerialNumber:          big.NewInt(1),
		Subject:               pkix.Name{CommonName: "test-fulcio-root"},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(24 * time.Hour),
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              x509.KeyUsageCertSign,
	}
	rootDER, err := x509.CreateCertificate(rand.Reader, rootTmpl, rootTmpl, &rootKey.PublicKey, rootKey)
	if err != nil {
		t.Fatal(err)
	}
	root, err := x509.ParseCertificate(rootDER)
	if err != nil {
		t.Fatal(err)
	}

	// Signing cert
	signKey, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	signTmpl := &x509.Certificate{
		SerialNumber: big.NewInt(2),
		Subject:      pkix.Name{CommonName: "plugin-signer"},
		NotBefore:    time.Now().Add(-time.Minute),
		NotAfter:     time.Now().Add(time.Hour),
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageCodeSigning},
	}
	for _, id := range identities {
		if u, err := url.Parse(id); err == nil && u.Scheme != "" {
			signTmpl.URIs = append(signTmpl.URIs, u)
		} else {
			signTmpl.EmailAddresses = append(signTmpl.EmailAddresses, id)
		}
	}
	signDER, err := x509.CreateCertificate(rand.Reader, signTmpl, root, &signKey.PublicKey, rootKey)
	if err != nil {
		t.Fatal(err)
	}
	sign, err := x509.ParseCertificate(signDER)
	if err != nil {
		t.Fatal(err)
	}
	return &signingSetup{rootCert: root, signCert: sign, signKey: signKey}
}

func (s *signingSetup) bundle(t *testing.T, payload []byte) string {
	t.Helper()
	digest := sha256.Sum256(payload)
	sig, err := ecdsa.SignASN1(rand.Reader, s.signKey, digest[:])
	if err != nil {
		t.Fatal(err)
	}
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: s.signCert.Raw})
	b, _ := json.Marshal(SigstoreBundle{
		CertificatePEM: string(certPEM),
		Signature:      base64.StdEncoding.EncodeToString(sig),
	})
	return string(b)
}

func TestSigstoreVerifier_HappyPath(t *testing.T) {
	setup := newSigningSetup(t, []string{"author@example.com"})
	payload := []byte("plugin tarball")

	v := SigstoreVerifier{
		Roots:             []*x509.Certificate{setup.rootCert},
		AllowedIdentities: []string{"author@example.com"},
	}
	pkg := Package{Bundle: setup.bundle(t, payload)}
	if err := v.Verify(payload, pkg); err != nil {
		t.Fatalf("unexpected: %v", err)
	}
}

func TestSigstoreVerifier_RejectsWrongIdentity(t *testing.T) {
	setup := newSigningSetup(t, []string{"attacker@evil.com"})
	payload := []byte("plugin tarball")

	v := SigstoreVerifier{
		Roots:             []*x509.Certificate{setup.rootCert},
		AllowedIdentities: []string{"author@example.com"},
	}
	pkg := Package{Bundle: setup.bundle(t, payload)}
	if err := v.Verify(payload, pkg); err == nil {
		t.Fatal("expected rejection: attacker identity not in allowlist")
	}
}

func TestSigstoreVerifier_RejectsWrongRoot(t *testing.T) {
	real := newSigningSetup(t, []string{"author@example.com"})
	fake := newSigningSetup(t, []string{"author@example.com"})
	payload := []byte("plugin tarball")

	v := SigstoreVerifier{
		Roots:             []*x509.Certificate{fake.rootCert},
		AllowedIdentities: []string{"author@example.com"},
	}
	pkg := Package{Bundle: real.bundle(t, payload)}
	if err := v.Verify(payload, pkg); err == nil {
		t.Fatal("expected rejection: signing cert chains to a different root")
	}
}

func TestSigstoreVerifier_RejectsTamperedPayload(t *testing.T) {
	setup := newSigningSetup(t, []string{"author@example.com"})
	payload := []byte("plugin tarball")

	v := SigstoreVerifier{
		Roots:             []*x509.Certificate{setup.rootCert},
		AllowedIdentities: []string{"author@example.com"},
	}
	pkg := Package{Bundle: setup.bundle(t, payload)}
	if err := v.Verify([]byte("tampered payload"), pkg); err == nil {
		t.Fatal("expected rejection: signature is over original payload")
	}
}

func TestSigstoreVerifier_RejectsEmptyIdentityList(t *testing.T) {
	setup := newSigningSetup(t, []string{"author@example.com"})
	payload := []byte("plugin tarball")
	v := SigstoreVerifier{
		Roots:             []*x509.Certificate{setup.rootCert},
		AllowedIdentities: nil,
	}
	if err := v.Verify(payload, Package{Bundle: setup.bundle(t, payload)}); err == nil {
		t.Fatal("empty AllowedIdentities must be a hard error — keyless with no policy is unsafe")
	}
}

func TestLoadPEMCertificates_MultipleAndComments(t *testing.T) {
	setup := newSigningSetup(t, []string{"a@b"})
	pemBytes := append(
		[]byte("This is a plaintext note that should be skipped.\n"),
		pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: setup.rootCert.Raw})...,
	)
	certs, err := LoadPEMCertificates(pemBytes)
	if err != nil {
		t.Fatal(err)
	}
	if len(certs) != 1 {
		t.Fatalf("want 1 cert, got %d", len(certs))
	}
}
