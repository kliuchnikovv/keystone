package store

import (
	"context"
	"crypto/ecdsa"
	"crypto/ed25519"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"math/big"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/kliuchnikovv/keystone/internal/plugin/store/tuf"
)

// TestLoadSigstoreTrustFromTUF spins up a mini-TUF repository that
// serves a real Fulcio-shaped root cert and a real Rekor-shaped
// public key, refreshes trust through it, and asserts the returned
// SigstoreTrust builds a working SigstoreVerifier.
func TestLoadSigstoreTrustFromTUF(t *testing.T) {
	// Fulcio: build a CA cert operators would trust.
	fulcioKey, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	rootTmpl := &x509.Certificate{
		SerialNumber:          big.NewInt(1),
		Subject:               pkix.Name{CommonName: "test-fulcio"},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(24 * time.Hour),
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              x509.KeyUsageCertSign,
	}
	rootDER, _ := x509.CreateCertificate(rand.Reader, rootTmpl, rootTmpl, &fulcioKey.PublicKey, fulcioKey)
	fulcioPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: rootDER})

	// Rekor: a real ECDSA public key in PKIX PEM.
	rekorKey, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	rekorDER, _ := x509.MarshalPKIXPublicKey(&rekorKey.PublicKey)
	rekorPEM := pem.EncodeToMemory(&pem.Block{Type: "PUBLIC KEY", Bytes: rekorDER})

	// Mini TUF repo signed by one ed25519 key across all roles.
	tufPub, tufPriv, _ := ed25519.GenerateKey(rand.Reader)
	keyID := "k1"
	future := time.Now().Add(24 * time.Hour).Format(time.RFC3339)

	targetsBody := targetsSpec(future, map[string][]byte{
		"fulcio_v1.crt.pem": fulcioPEM,
		"rekor.pub":         rekorPEM,
	})
	targetsJSON := signMetadata(tufPriv, keyID, targetsBody)

	snapshotBody := snapshotSpec(future, 1)
	snapshotJSON := signMetadata(tufPriv, keyID, snapshotBody)

	timestampBody := timestampSpec(future, 1)
	timestampJSON := signMetadata(tufPriv, keyID, timestampBody)

	rootBody := rootSpec(future, keyID, tufPub)
	rootJSON := signMetadata(tufPriv, keyID, rootBody)

	handler := http.NewServeMux()
	handler.HandleFunc("/timestamp.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(timestampJSON)
	})
	handler.HandleFunc("/1.snapshot.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(snapshotJSON)
	})
	handler.HandleFunc("/1.targets.json", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(targetsJSON)
	})
	handler.HandleFunc("/targets/", func(w http.ResponseWriter, req *http.Request) {
		path := strings.TrimPrefix(req.URL.Path, "/targets/")
		for name, body := range map[string][]byte{
			"fulcio_v1.crt.pem": fulcioPEM,
			"rekor.pub":         rekorPEM,
		} {
			if path == name || strings.HasSuffix(path, "."+name) {
				_, _ = w.Write(body)
				return
			}
		}
		http.Error(w, "not found", 404)
	})
	srv := httptest.NewServer(handler)
	defer srv.Close()

	trust, err := LoadSigstoreTrustFromTUF(context.Background(), srv.Client(), srv.URL, rootJSON)
	if err != nil {
		t.Fatalf("LoadSigstoreTrustFromTUF: %v", err)
	}
	if len(trust.FulcioRoots) != 1 {
		t.Fatalf("want 1 fulcio root, got %d", len(trust.FulcioRoots))
	}
	if trust.RekorPublicKey == nil {
		t.Fatal("rekor key is nil")
	}
	// Confirm the loaded Rekor key matches by re-marshalling.
	roundtrip, _ := x509.MarshalPKIXPublicKey(trust.RekorPublicKey)
	if string(roundtrip) != string(rekorDER) {
		t.Fatal("rekor key round-trip mismatch")
	}

	v := trust.Verifier([]string{"you@example.com"})
	if v.Rekor == nil || v.Rekor.PublicKey == nil {
		t.Fatal("verifier should carry a Rekor policy")
	}
	if !v.Rekor.VerifyInclusion {
		t.Fatal("verifier should require inclusion by default")
	}
}

// signMetadata is a tiny copy of the tuf package's envelope wrapper
// so the trust_test doesn't reach into unexported territory. It
// signs the raw body with priv and wraps it in {signed, signatures}.
func signMetadata(priv ed25519.PrivateKey, keyID string, body []byte) []byte {
	sig := ed25519.Sign(priv, body)
	env := struct {
		Signed     json.RawMessage `json:"signed"`
		Signatures []struct {
			KeyID string `json:"keyid"`
			Sig   string `json:"sig"`
		} `json:"signatures"`
	}{
		Signed: body,
	}
	env.Signatures = append(env.Signatures, struct {
		KeyID string `json:"keyid"`
		Sig   string `json:"sig"`
	}{KeyID: keyID, Sig: hex.EncodeToString(sig)})
	out, _ := json.Marshal(env)
	return out
}

func rootSpec(expires, keyID string, pub ed25519.PublicKey) []byte {
	spec := tuf.RootSpec{
		Type: "root", Version: 1, Expires: expires,
		Keys: map[string]tuf.Key{
			keyID: {Type: "ed25519", Scheme: "ed25519", Values: tuf.KeyValue{Public: hex.EncodeToString(pub)}},
		},
		Roles: map[string]tuf.Role{
			"root":      {KeyIDs: []string{keyID}, Threshold: 1},
			"timestamp": {KeyIDs: []string{keyID}, Threshold: 1},
			"snapshot":  {KeyIDs: []string{keyID}, Threshold: 1},
			"targets":   {KeyIDs: []string{keyID}, Threshold: 1},
		},
	}
	b, _ := json.Marshal(spec)
	return b
}

func targetsSpec(expires string, files map[string][]byte) []byte {
	m := map[string]tuf.Target{}
	for name, body := range files {
		sum := sha256.Sum256(body)
		m[name] = tuf.Target{Length: int64(len(body)), Hashes: map[string]string{"sha256": hex.EncodeToString(sum[:])}}
	}
	spec := tuf.TargetsSpec{Type: "targets", Version: 1, Expires: expires, Targets: m}
	b, _ := json.Marshal(spec)
	return b
}

func snapshotSpec(expires string, targetsVersion int) []byte {
	spec := tuf.SnapshotSpec{
		Type: "snapshot", Version: 1, Expires: expires,
		Meta: map[string]tuf.Meta{"targets.json": {Version: targetsVersion}},
	}
	b, _ := json.Marshal(spec)
	return b
}

func timestampSpec(expires string, snapshotVersion int) []byte {
	spec := tuf.TimestampSpec{
		Type: "timestamp", Version: 1, Expires: expires,
		Meta: map[string]tuf.Meta{"snapshot.json": {Version: snapshotVersion}},
	}
	b, _ := json.Marshal(spec)
	return b
}

func TestLoadSigstoreTrust_RequiresTargetNames(t *testing.T) {
	_, err := LoadSigstoreTrust(context.Background(), http.DefaultClient, "http://x", []byte(`{}`), SigstoreTargets{})
	if err == nil {
		t.Fatal("empty target names must be refused")
	}
}

// TestSigstoreTrust_Verifier tests the builder in isolation — no TUF
// network path required.
func TestSigstoreTrust_Verifier(t *testing.T) {
	rekorKey, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	cert := &x509.Certificate{Subject: pkix.Name{CommonName: "x"}}
	trust := &SigstoreTrust{
		FulcioRoots:    []*x509.Certificate{cert},
		RekorPublicKey: &rekorKey.PublicKey,
	}
	v := trust.Verifier([]string{"you@example.com"})
	if len(v.AllowedIdentities) != 1 {
		t.Fatalf("identities = %v", v.AllowedIdentities)
	}
	if v.Rekor == nil || v.Rekor.PublicKey == nil {
		t.Fatal("verifier lost rekor policy")
	}
	if !v.Rekor.VerifyInclusion {
		t.Fatal("VerifyInclusion should default to true when TUF is doing the work")
	}
	// Silence unused
	_ = fmt.Sprintf
}
