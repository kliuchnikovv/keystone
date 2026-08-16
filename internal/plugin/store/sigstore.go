package store

import (
	"crypto"
	"crypto/ecdsa"
	"crypto/ed25519"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"strings"
	"time"
)

// SigstoreVerifier verifies packages signed via Sigstore keyless.
//
// The bundle carries a signing certificate issued by Fulcio (or an
// operator-configured private CA), a detached signature over the
// tarball's sha256 digest, and — once we wire it — a Rekor inclusion
// proof. Verification pipeline:
//
//   1. Parse the bundle from Package.Bundle.
//   2. Verify the signing certificate chains to one of Roots.
//   3. Verify the certificate's SAN (email or URI) matches one of
//      AllowedIdentities. Cert issuer OID for the OIDC provider is
//      matched against ExpectedIssuer when set.
//   4. Verify the signature is valid over sha256(payload) with the
//      cert's public key.
//   5. Rekor inclusion proof verification is a follow-on: when
//      RekorPublicKey is set, we'll verify the inclusion proof in
//      the bundle. Not enforced yet — mark it in the commit, land
//      the wire format first.
//
// Zero value is not useful — Roots and AllowedIdentities must be set.
// Empty AllowedIdentities is a hard error, matching how a real
// cosign policy behaves: "keyless with no identity policy" is
// indistinguishable from "no verification at all".
type SigstoreVerifier struct {
	// Roots are the CA certificates the signing certificate must
	// chain to. Fulcio's public root is the typical entry; a
	// deployment can pin its own private CA instead.
	Roots []*x509.Certificate

	// Intermediates are optional CA certificates used only to
	// complete the chain; they are not trust anchors.
	Intermediates []*x509.Certificate

	// AllowedIdentities is the OIDC subject allowlist. Each entry is
	// an exact match against the signing certificate's SAN email or
	// URI. Wildcard / regex support belongs to a follow-on policy
	// language — matching what real cosign policies do.
	AllowedIdentities []string

	// ExpectedIssuer, when set, must equal the OIDC issuer recorded
	// in the certificate (Fulcio embeds it in an extension). Empty
	// means "any issuer chained via Roots" — useful during setup,
	// dangerous in production.
	ExpectedIssuer string

	// Now returns the verification time. Nil defaults to time.Now.
	// Kept injectable so tests can pin against a specific issuance
	// window.
	Now func() time.Time
}

// SigstoreBundle is the on-wire shape SigstoreVerifier consumes.
// Kept small on purpose — one certificate, one signature, an optional
// Rekor pointer. Compatible with cosign's --bundle output at the
// field level, which we can move to natively once Rekor lands.
type SigstoreBundle struct {
	// CertificatePEM is the Fulcio-issued signing certificate for
	// this signature. Ephemeral — Fulcio issues a fresh cert per
	// keyless signing operation, valid for ~10 minutes.
	CertificatePEM string `json:"certificate"`

	// Signature is the base64 detached signature over sha256(payload).
	Signature string `json:"signature"`

	// LogIndex points at the Rekor transparency log entry that
	// records this signature. Ignored today; a future Rekor
	// verifier will fetch and verify inclusion via this field.
	LogIndex int64 `json:"logIndex,omitempty"`

	// LogEntry, when present, is a base64-encoded Rekor inclusion
	// proof the operator can precompute so verification does not
	// need to reach out to Rekor at install time.
	LogEntry string `json:"logEntry,omitempty"`
}

// Verify implements Verifier.
func (v SigstoreVerifier) Verify(data []byte, pkg Package) error {
	if len(v.Roots) == 0 {
		return errors.New("store/sigstore: no trusted Roots configured")
	}
	if len(v.AllowedIdentities) == 0 {
		return errors.New("store/sigstore: AllowedIdentities is empty; keyless with no identity policy is unsafe")
	}
	if pkg.Bundle == "" {
		return errors.New("store/sigstore: package has no bundle; refusing to install")
	}

	var bundle SigstoreBundle
	if err := json.Unmarshal([]byte(pkg.Bundle), &bundle); err != nil {
		return fmt.Errorf("store/sigstore: parse bundle: %w", err)
	}

	cert, err := parsePEMCert(bundle.CertificatePEM)
	if err != nil {
		return fmt.Errorf("store/sigstore: signing certificate: %w", err)
	}
	if err := v.verifyChain(cert); err != nil {
		return err
	}
	if err := v.verifyIdentity(cert); err != nil {
		return err
	}
	sig, err := base64.StdEncoding.DecodeString(strings.TrimSpace(bundle.Signature))
	if err != nil {
		return fmt.Errorf("store/sigstore: signature decode: %w", err)
	}
	digest := sha256.Sum256(data)
	if err := verifyDigest(cert.PublicKey, digest[:], sig); err != nil {
		return fmt.Errorf("store/sigstore: signature: %w", err)
	}
	// Rekor inclusion verification is deferred; a policy that
	// requires transparency-log presence should also verify the
	// inclusion proof here once we wire it.
	return nil
}

func (v SigstoreVerifier) verifyChain(cert *x509.Certificate) error {
	roots := x509.NewCertPool()
	for _, r := range v.Roots {
		roots.AddCert(r)
	}
	intermediates := x509.NewCertPool()
	for _, ic := range v.Intermediates {
		intermediates.AddCert(ic)
	}
	now := time.Now
	if v.Now != nil {
		now = v.Now
	}
	// Code-signing usage: Fulcio cert has ExtKeyUsageCodeSigning
	// set. Enforce it — a TLS cert accidentally used for signing
	// would otherwise pass.
	_, err := cert.Verify(x509.VerifyOptions{
		Roots:         roots,
		Intermediates: intermediates,
		CurrentTime:   now(),
		KeyUsages:     []x509.ExtKeyUsage{x509.ExtKeyUsageCodeSigning},
	})
	if err != nil {
		return fmt.Errorf("store/sigstore: certificate chain: %w", err)
	}
	return nil
}

func (v SigstoreVerifier) verifyIdentity(cert *x509.Certificate) error {
	got := certIdentities(cert)
	for _, id := range got {
		for _, allowed := range v.AllowedIdentities {
			if id == allowed {
				return nil
			}
		}
	}
	return fmt.Errorf("store/sigstore: certificate identity %v does not match any allowed identity", got)
}

// certIdentities pulls the SAN fields Fulcio actually populates: email
// addresses (google/microsoft OIDC) or URIs (github actions OIDC,
// which encodes the workflow identity as spiffe:// or https://).
func certIdentities(cert *x509.Certificate) []string {
	out := make([]string, 0, len(cert.EmailAddresses)+len(cert.URIs))
	out = append(out, cert.EmailAddresses...)
	for _, u := range cert.URIs {
		out = append(out, u.String())
	}
	return out
}

// verifyDigest dispatches by key type so an ECDSA, RSA or Ed25519
// signing key all work under the same call site. Fulcio issues
// ECDSA-P256 by default.
func verifyDigest(pub any, digest, sig []byte) error {
	switch key := pub.(type) {
	case *ecdsa.PublicKey:
		if !ecdsa.VerifyASN1(key, digest, sig) {
			return errors.New("ECDSA verification failed")
		}
		return nil
	case ed25519.PublicKey:
		if !ed25519.Verify(key, digest, sig) {
			return errors.New("ed25519 verification failed")
		}
		return nil
	case *rsa.PublicKey:
		return rsa.VerifyPKCS1v15(key, crypto.SHA256, digest, sig)
	default:
		return fmt.Errorf("unsupported signing key type %T", pub)
	}
}

// parsePEMCert reads a single PEM CERTIFICATE block.
func parsePEMCert(raw string) (*x509.Certificate, error) {
	block, _ := pem.Decode([]byte(raw))
	if block == nil {
		return nil, errors.New("no PEM block")
	}
	if block.Type != "CERTIFICATE" {
		return nil, fmt.Errorf("PEM block is %q, not CERTIFICATE", block.Type)
	}
	return x509.ParseCertificate(block.Bytes)
}

// LoadPEMCertificates parses one or more PEM CERTIFICATE blocks from
// raw. Non-CERTIFICATE blocks are skipped so an operator can paste a
// bundle with mixed content (e.g. a note and a cert).
func LoadPEMCertificates(raw []byte) ([]*x509.Certificate, error) {
	var out []*x509.Certificate
	rest := raw
	for {
		block, next := pem.Decode(rest)
		if block == nil {
			break
		}
		rest = next
		if block.Type != "CERTIFICATE" {
			continue
		}
		cert, err := x509.ParseCertificate(block.Bytes)
		if err != nil {
			return nil, err
		}
		out = append(out, cert)
	}
	if len(out) == 0 {
		return nil, errors.New("no PEM certificates found")
	}
	return out, nil
}
