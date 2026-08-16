package store

import (
	"crypto/ed25519"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/pem"
	"errors"
	"fmt"
	"strings"
)

// Verifier decides whether a fetched package is trustworthy. It runs
// after sha256 has already matched — so implementations that only care
// about the digest can return nil immediately.
//
// A future Sigstore-keyless verifier drops in here: same interface,
// same call site. Store.Install passes the package's raw bytes so a
// verifier that reads the payload (Rekor inclusion proof, transparency
// log) has what it needs, and the index entry so an implementation
// can pull cert / signature / bundle fields off of Package.
type Verifier interface {
	Verify(data []byte, pkg Package) error
}

// SHA256Verifier is the default. sha256 is already checked by the
// store before Verify is called, so this one is a no-op — it exists so
// the wiring is uniform whether or not signing is on.
type SHA256Verifier struct{}

// Verify implements Verifier.
func (SHA256Verifier) Verify(_ []byte, _ Package) error { return nil }

// Ed25519Verifier checks a detached ed25519 signature over the
// tarball's sha256 digest against a set of trusted public keys. This
// is not full Sigstore keyless — there is no Rekor log, no Fulcio
// certificate chain — but it is a proper cryptographic signing gate
// with a small dependency budget, and it lives behind the same
// Verifier interface a keyless implementation would.
//
// Signatures are base64-encoded in Package.Signature. Trust is
// established by matching against any key in Keys. Empty Signature
// with a non-empty Keys set is a hard fail — a verifier that
// requires signatures cannot let unsigned packages pass.
type Ed25519Verifier struct {
	Keys []ed25519.PublicKey
}

// Verify implements Verifier.
func (v Ed25519Verifier) Verify(data []byte, pkg Package) error {
	if len(v.Keys) == 0 {
		return errors.New("store/verify: Ed25519Verifier configured with no trusted keys")
	}
	if pkg.Signature == "" {
		return errors.New("store/verify: package has no signature; refusing to install")
	}
	sig, err := decodeSignature(pkg.Signature)
	if err != nil {
		return fmt.Errorf("store/verify: decode signature: %w", err)
	}
	// Sign the sha256 digest, not the raw payload — same shape cosign
	// uses so a later migration keeps the wire compatible.
	digest := sha256.Sum256(data)
	for _, key := range v.Keys {
		if ed25519.Verify(key, digest[:], sig) {
			return nil
		}
	}
	return errors.New("store/verify: signature did not match any trusted key")
}

// LoadEd25519PublicKeys parses one or more PEM-encoded ed25519 public
// keys from raw. Empty PEM blocks are tolerated so an operator can
// paste comments between --- BEGIN PUBLIC KEY --- blocks.
func LoadEd25519PublicKeys(raw []byte) ([]ed25519.PublicKey, error) {
	var keys []ed25519.PublicKey
	rest := raw
	for {
		block, next := pem.Decode(rest)
		if block == nil {
			break
		}
		rest = next
		if block.Type != "PUBLIC KEY" {
			continue
		}
		pub, err := x509.ParsePKIXPublicKey(block.Bytes)
		if err != nil {
			return nil, fmt.Errorf("store/verify: parse public key: %w", err)
		}
		ed, ok := pub.(ed25519.PublicKey)
		if !ok {
			return nil, fmt.Errorf("store/verify: %T is not an ed25519 public key", pub)
		}
		keys = append(keys, ed)
	}
	if len(keys) == 0 {
		return nil, errors.New("store/verify: no ed25519 public keys found in input")
	}
	return keys, nil
}

// decodeSignature accepts base64 or lowercase hex. cosign historically
// emits base64; hex is easier to eyeball in an index.json diff.
func decodeSignature(s string) ([]byte, error) {
	s = strings.TrimSpace(s)
	if b, err := base64.StdEncoding.DecodeString(s); err == nil {
		return b, nil
	}
	if b, err := base64.RawStdEncoding.DecodeString(s); err == nil {
		return b, nil
	}
	return hex.DecodeString(s)
}
