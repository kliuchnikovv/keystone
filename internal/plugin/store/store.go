// Package store is the core's client for a plugin registry.
//
// A registry serves an index at <baseURL>/index.json and tarball
// packages at URLs the index resolves against baseURL. That's the
// whole contract — the format lets a plain HTTPS host or a static
// GitHub Pages site be a registry with no server-side logic.
//
// Sigstore signing and multi-registry taps are follow-on work; this
// v0 supports one registry per install call and verifies packages by
// sha256 recorded in the index.
package store

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/kliuchnikovv/keystone/internal/plugin/pack"
)

// DefaultTimeout bounds one HTTP call — an index fetch or a tarball
// download. Long enough for a slow LAN, short enough that a stuck
// registry does not hang the daemon.
const DefaultTimeout = 60 * time.Second

// Index is what <baseURL>/index.json returns.
type Index struct {
	Plugins map[string]PluginEntry `json:"plugins"`
}

// PluginEntry is one plugin in the index — a name plus every published
// version.
type PluginEntry struct {
	// Description is optional metadata for a store UI. Not used by the
	// install path.
	Description string             `json:"description,omitempty"`
	Homepage    string             `json:"homepage,omitempty"`
	Versions    map[string]Package `json:"versions"`
}

// Package is one downloadable version. URL is resolved against the
// registry's base URL, so a relative path like "packages/matter-0.1.0.tgz"
// lets the whole index and its blobs live under one host.
type Package struct {
	URL    string `json:"url"`
	SHA256 string `json:"sha256"`
	// Signature is reserved for a future Sigstore field. Ignored today.
	Signature string `json:"signature,omitempty"`
}

// Registry is a store client bound to one base URL.
type Registry struct {
	baseURL  string
	http     *http.Client
	verifier Verifier
}

// New builds a Registry against baseURL. The URL is validated up front
// (scheme, host, resolved IP) so an install attempt against a
// dangerous address fails before any bytes leave the daemon.
//
// The default verifier only rechecks the sha256 the index already
// declares. Callers who want signature enforcement swap it via
// WithVerifier.
func New(baseURL string) (*Registry, error) {
	allowLoopback, err := validateURL(baseURL)
	if err != nil {
		return nil, err
	}
	return &Registry{
		baseURL:  strings.TrimRight(baseURL, "/"),
		http:     newSafeHTTPClient(allowLoopback),
		verifier: SHA256Verifier{},
	}, nil
}

// WithVerifier returns a copy of r with the given verifier. Called
// from the manager once trusted keys are loaded from disk.
func (r *Registry) WithVerifier(v Verifier) *Registry {
	if v == nil {
		v = SHA256Verifier{}
	}
	out := *r
	out.verifier = v
	return &out
}

// newSafeHTTPClient builds an HTTP client whose Dialer refuses to
// connect to private / link-local / unspecified addresses. A registry
// index whose package URLs redirect to an internal service cannot
// slip past the initial validateURL check because the dialer
// re-validates every dial.
//
// allowLoopback trickles down from the base URL: an explicit
// loopback registry (e.g. http://localhost:18000) permits loopback
// dials so local development still works, but every other registry
// treats a loopback resolution as a DNS-rebinding attempt and
// refuses.
func newSafeHTTPClient(allowLoopback bool) *http.Client {
	dialer := &net.Dialer{Timeout: 15 * time.Second}
	return &http.Client{
		Timeout: DefaultTimeout,
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, network, addr string) (net.Conn, error) {
				host, port, err := net.SplitHostPort(addr)
				if err != nil {
					return nil, err
				}
				if err := checkHostForDial(host, allowLoopback); err != nil {
					return nil, err
				}
				return dialer.DialContext(ctx, network, net.JoinHostPort(host, port))
			},
		},
	}
}

// validateURL enforces the scheme and host rules for a registry base
// URL: https:// always; http:// only for loopback hostnames (localhost,
// 127.0.0.0/8, ::1) so local development still works. Returns
// allowLoopback=true when the base URL itself is loopback — the HTTP
// client uses that flag to keep the local development path open
// without letting a public https URL smuggle in a loopback IP.
func validateURL(raw string) (bool, error) {
	u, err := url.Parse(raw)
	if err != nil {
		return false, fmt.Errorf("store: invalid base URL: %w", err)
	}
	if u.Host == "" {
		return false, errors.New("store: base URL is missing a host")
	}
	loopback := isLoopbackHost(u.Hostname())
	switch u.Scheme {
	case "https":
		if err := checkHostForDial(u.Hostname(), false); err != nil {
			return false, err
		}
		return false, nil
	case "http":
		if !loopback {
			return false, fmt.Errorf("store: http:// is only permitted for loopback hosts; got %s", u.Host)
		}
		return true, nil
	default:
		return false, fmt.Errorf("store: unsupported URL scheme %q — use https://", u.Scheme)
	}
}

// isLoopbackHost recognises the small set of names and literals that
// mean "this machine". Anything else is treated as remote and forced
// through https://.
func isLoopbackHost(host string) bool {
	host = strings.ToLower(host)
	if host == "localhost" {
		return true
	}
	ip := net.ParseIP(host)
	if ip == nil {
		return false
	}
	return ip.IsLoopback()
}

// checkHostForDial refuses to dial anything that would let an attacker
// turn the daemon into an SSRF probe: private IP ranges, link-local,
// or the unspecified address. Loopback is refused unless the caller
// explicitly permitted it (i.e. the base URL was loopback) — that
// defeats DNS-rebinding attempts against public hosts.
func checkHostForDial(host string, allowLoopback bool) error {
	if host == "" {
		return errors.New("store: empty host")
	}
	ips, err := net.LookupIP(host)
	if err != nil {
		if literal := net.ParseIP(host); literal != nil {
			ips = []net.IP{literal}
		} else {
			return fmt.Errorf("store: resolve %s: %w", host, err)
		}
	}
	for _, ip := range ips {
		if err := checkResolvedIP(ip, allowLoopback); err != nil {
			return fmt.Errorf("store: refusing to dial %s (%s): %w", host, ip, err)
		}
	}
	return nil
}

func checkResolvedIP(ip net.IP, allowLoopback bool) error {
	switch {
	case ip.IsUnspecified():
		return errors.New("unspecified address")
	case ip.IsLinkLocalUnicast(), ip.IsLinkLocalMulticast():
		return errors.New("link-local address")
	case ip.IsPrivate():
		return errors.New("private address")
	case ip.IsLoopback():
		if !allowLoopback {
			return errors.New("loopback address")
		}
	}
	return nil
}

// FetchIndex reads the registry's index.
func (r *Registry) FetchIndex(ctx context.Context) (*Index, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, r.baseURL+"/index.json", nil)
	if err != nil {
		return nil, err
	}
	resp, err := r.http.Do(req)
	if err != nil {
		return nil, fmt.Errorf("store: fetch index: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("store: index HTTP %s", resp.Status)
	}
	var idx Index
	if err := json.NewDecoder(io.LimitReader(resp.Body, 8<<20)).Decode(&idx); err != nil {
		return nil, fmt.Errorf("store: parse index: %w", err)
	}
	return &idx, nil
}

// ErrPluginNotInRegistry is returned when the requested name is
// missing from the index, or when the version is unknown.
var ErrPluginNotInRegistry = errors.New("store: plugin not in registry")

// ResolveVersion picks a package from the index. version may be "" or
// "latest" to select the highest semver.
func (idx *Index) ResolveVersion(name, version string) (Package, string, error) {
	entry, ok := idx.Plugins[name]
	if !ok {
		return Package{}, "", fmt.Errorf("%w: %s", ErrPluginNotInRegistry, name)
	}
	if version == "" || version == "latest" {
		v := latest(entry.Versions)
		if v == "" {
			return Package{}, "", fmt.Errorf("%w: %s has no versions", ErrPluginNotInRegistry, name)
		}
		return entry.Versions[v], v, nil
	}
	pkg, ok := entry.Versions[version]
	if !ok {
		return Package{}, "", fmt.Errorf("%w: %s@%s", ErrPluginNotInRegistry, name, version)
	}
	return pkg, version, nil
}

// Install fetches, verifies and unpacks a plugin into targetRoot.
// Returns the installed plugin name (from the manifest, which the
// registry index name must match) and the resolved version.
//
// force overwrites an existing install directory. Callers that do not
// want an implicit overwrite pass false and get a helpful error.
func (r *Registry) Install(ctx context.Context, name, version, targetRoot string, force bool) (string, string, error) {
	idx, err := r.FetchIndex(ctx)
	if err != nil {
		return "", "", err
	}
	pkg, resolved, err := idx.ResolveVersion(name, version)
	if err != nil {
		return "", "", err
	}
	tarballURL, err := r.resolveURL(pkg.URL)
	if err != nil {
		return "", "", err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, tarballURL, nil)
	if err != nil {
		return "", "", err
	}
	resp, err := r.http.Do(req)
	if err != nil {
		return "", "", fmt.Errorf("store: fetch %s: %w", tarballURL, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", "", fmt.Errorf("store: fetch %s: HTTP %s", tarballURL, resp.Status)
	}
	// Cap the read at a sane ceiling — a broken registry that streams
	// forever should not exhaust disk before we notice.
	body, err := io.ReadAll(io.LimitReader(resp.Body, 256<<20))
	if err != nil {
		return "", "", fmt.Errorf("store: read tarball: %w", err)
	}
	if err := verifySHA256(body, pkg.SHA256); err != nil {
		return "", "", err
	}
	if err := r.verifier.Verify(body, pkg); err != nil {
		return "", "", err
	}
	installed, err := pack.UnpackReader(bytes.NewReader(body), targetRoot, force)
	if err != nil {
		return "", "", err
	}
	// The manifest's name and the index key must match; a mismatched
	// tarball is almost certainly a registry mixup, and installing it
	// under the wrong slug would confuse everything downstream.
	if installed != name {
		return installed, resolved, fmt.Errorf("store: tarball's manifest name %q does not match registry key %q", installed, name)
	}
	return installed, resolved, nil
}

func (r *Registry) resolveURL(raw string) (string, error) {
	u, err := url.Parse(raw)
	if err != nil {
		return "", fmt.Errorf("store: bad package URL %q: %w", raw, err)
	}
	if u.IsAbs() {
		return raw, nil
	}
	base, err := url.Parse(r.baseURL + "/")
	if err != nil {
		return "", err
	}
	return base.ResolveReference(u).String(), nil
}

func verifySHA256(data []byte, want string) error {
	if want == "" {
		return errors.New("store: package has no sha256 in index; refusing to install unverified content")
	}
	sum := sha256.Sum256(data)
	got := hex.EncodeToString(sum[:])
	want = strings.ToLower(strings.TrimSpace(want))
	if got != want {
		return fmt.Errorf("store: sha256 mismatch (got %s, want %s)", got, want)
	}
	return nil
}

// latest returns the highest semver-ish key. A version string that
// does not parse falls back to lexical order; a strict semver
// implementation is a follow-on.
func latest(versions map[string]Package) string {
	if len(versions) == 0 {
		return ""
	}
	keys := make([]string, 0, len(versions))
	for k := range versions {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool {
		return lessSemver(keys[i], keys[j])
	})
	return keys[len(keys)-1]
}

// lessSemver compares two "a.b.c" strings numerically component by
// component; anything that doesn't parse falls back to string compare
// so the sort is total.
func lessSemver(a, b string) bool {
	ai := parseSemver(a)
	bi := parseSemver(b)
	for i := 0; i < 3; i++ {
		if ai[i] != bi[i] {
			return ai[i] < bi[i]
		}
	}
	return a < b
}

func parseSemver(s string) [3]int {
	s = strings.TrimPrefix(s, "v")
	if i := strings.IndexAny(s, "-+"); i >= 0 {
		s = s[:i]
	}
	parts := strings.SplitN(s, ".", 3)
	var out [3]int
	for i, p := range parts {
		if i >= 3 {
			break
		}
		n, err := strconv.Atoi(p)
		if err != nil {
			return [3]int{}
		}
		out[i] = n
	}
	return out
}
