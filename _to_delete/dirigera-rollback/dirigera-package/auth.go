package dirigera

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
)

// GenerateCodeVerifier returns a PKCE code_verifier — 43..128 chars of
// URL-safe random. We use 64 bytes → ~86 chars.
func GenerateCodeVerifier() (string, error) {
	var buf [64]byte
	if _, err := rand.Read(buf[:]); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(buf[:]), nil
}

// CodeChallenge computes S256 challenge from a verifier.
func CodeChallenge(verifier string) string {
	sum := sha256.Sum256([]byte(verifier))
	return base64.RawURLEncoding.EncodeToString(sum[:])
}
