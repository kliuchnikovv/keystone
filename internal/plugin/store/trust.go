package store

import (
	"context"
	"crypto/ecdsa"
	"crypto/x509"
	"errors"
	"fmt"
	"net/http"

	"github.com/kliuchnikovv/keystone/internal/plugin/store/tuf"
)

// SigstoreTrust bundles the two things a SigstoreVerifier actually
// needs: Fulcio's root certificate(s) and Rekor's public key.
// Produced by LoadSigstoreTrustFromTUF from the sigstore.dev TUF
// repository, or built by hand for private deployments.
type SigstoreTrust struct {
	// FulcioRoots is the CA that must issue the ephemeral signing
	// certificate.
	FulcioRoots []*x509.Certificate

	// FulcioIntermediates completes the chain when Fulcio issues
	// under an intermediate CA. Empty for the current sigstore.dev
	// deployment; deployments running their own Fulcio may have one.
	FulcioIntermediates []*x509.Certificate

	// RekorPublicKey is used to verify the Signed Entry Timestamp
	// and, when the caller requires it, the inclusion-proof
	// checkpoint.
	RekorPublicKey *ecdsa.PublicKey
}

// Verifier builds a SigstoreVerifier from a trust bundle. identities
// is the OIDC subject allowlist — an empty list is a hard error, same
// as constructing SigstoreVerifier directly.
func (t *SigstoreTrust) Verifier(identities []string) *SigstoreVerifier {
	return &SigstoreVerifier{
		Roots:             t.FulcioRoots,
		Intermediates:     t.FulcioIntermediates,
		AllowedIdentities: identities,
		Rekor: &RekorPolicy{
			PublicKey:       t.RekorPublicKey,
			VerifyInclusion: true,
		},
	}
}

// LoadSigstoreTrustFromTUF fetches the current Fulcio and Rekor
// materials from the given TUF repository. rootJSON is the initial
// trusted root — for Sigstore's public repo the bytes ship in
// go-sigstore-tuf's embedded trusted_root, or the operator can pin a
// specific root they've audited. httpClient should be an
// SSRF-hardened client; the store's own newSafeHTTPClient works.
//
// The target names here match Sigstore's TUF layout: fulcio_v1.crt.pem
// for the Fulcio root and rekor.pub for the Rekor public key.
// Deployments running their own Sigstore may use different names —
// pass them via LoadSigstoreTrust when they do.
func LoadSigstoreTrustFromTUF(ctx context.Context, httpClient *http.Client, repoURL string, rootJSON []byte) (*SigstoreTrust, error) {
	return LoadSigstoreTrust(ctx, httpClient, repoURL, rootJSON, SigstoreTargets{
		FulcioRoot: "fulcio_v1.crt.pem",
		RekorKey:   "rekor.pub",
	})
}

// SigstoreTargets names the TUF targets to fetch. Kept as a struct so
// a private deployment with a different naming scheme (or an operator
// running Fulcio behind an intermediate they publish separately) can
// override without a helper explosion.
type SigstoreTargets struct {
	FulcioRoot         string
	FulcioIntermediate string // optional
	RekorKey           string
}

// LoadSigstoreTrust is LoadSigstoreTrustFromTUF with explicit target
// names. Prefer LoadSigstoreTrustFromTUF unless you need to point at
// something other than sigstore.dev's default layout.
func LoadSigstoreTrust(ctx context.Context, httpClient *http.Client, repoURL string, rootJSON []byte, targets SigstoreTargets) (*SigstoreTrust, error) {
	if targets.FulcioRoot == "" || targets.RekorKey == "" {
		return nil, errors.New("store/trust: FulcioRoot and RekorKey target names are required")
	}
	c, err := tuf.New(rootJSON)
	if err != nil {
		return nil, fmt.Errorf("store/trust: initial root: %w", err)
	}
	if err := c.Refresh(ctx, httpClient, repoURL); err != nil {
		return nil, fmt.Errorf("store/trust: refresh: %w", err)
	}

	fulcio, err := c.Fetch(ctx, httpClient, repoURL, targets.FulcioRoot)
	if err != nil {
		return nil, fmt.Errorf("store/trust: fetch Fulcio root: %w", err)
	}
	roots, err := LoadPEMCertificates(fulcio)
	if err != nil {
		return nil, fmt.Errorf("store/trust: parse Fulcio root: %w", err)
	}

	rekor, err := c.Fetch(ctx, httpClient, repoURL, targets.RekorKey)
	if err != nil {
		return nil, fmt.Errorf("store/trust: fetch Rekor key: %w", err)
	}
	rekorKey, err := LoadECDSAPublicKey(rekor)
	if err != nil {
		return nil, fmt.Errorf("store/trust: parse Rekor key: %w", err)
	}

	trust := &SigstoreTrust{
		FulcioRoots:    roots,
		RekorPublicKey: rekorKey,
	}
	if targets.FulcioIntermediate != "" {
		blob, err := c.Fetch(ctx, httpClient, repoURL, targets.FulcioIntermediate)
		if err != nil {
			return nil, fmt.Errorf("store/trust: fetch Fulcio intermediate: %w", err)
		}
		mids, err := LoadPEMCertificates(blob)
		if err != nil {
			return nil, fmt.Errorf("store/trust: parse Fulcio intermediate: %w", err)
		}
		trust.FulcioIntermediates = mids
	}
	return trust, nil
}
