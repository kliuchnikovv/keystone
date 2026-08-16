package store

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func TestResolveVersion_Latest(t *testing.T) {
	idx := &Index{Plugins: map[string]PluginEntry{
		"demo": {Versions: map[string]Package{
			"0.9.0":  {URL: "packages/demo-0.9.0.tgz"},
			"0.10.0": {URL: "packages/demo-0.10.0.tgz"},
			"1.0.0":  {URL: "packages/demo-1.0.0.tgz"},
		}},
	}}
	_, v, err := idx.ResolveVersion("demo", "")
	if err != nil {
		t.Fatal(err)
	}
	if v != "1.0.0" {
		t.Fatalf("latest = %s, want 1.0.0", v)
	}
	_, v, err = idx.ResolveVersion("demo", "0.10.0")
	if err != nil {
		t.Fatal(err)
	}
	if v != "0.10.0" {
		t.Fatalf("exact = %s", v)
	}
}

func TestResolveVersion_UnknownName(t *testing.T) {
	idx := &Index{Plugins: map[string]PluginEntry{"a": {Versions: map[string]Package{"1.0.0": {}}}}}
	_, _, err := idx.ResolveVersion("b", "")
	if err == nil {
		t.Fatal("expected error on unknown name")
	}
}

func TestInstall_HappyPath(t *testing.T) {
	tarball := buildDemoTarball(t)
	sha := sha256.Sum256(tarball)

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/index.json":
			_ = json.NewEncoder(w).Encode(Index{Plugins: map[string]PluginEntry{
				"demo": {Versions: map[string]Package{"0.1.0": {URL: "packages/demo-0.1.0.tgz", SHA256: hex.EncodeToString(sha[:])}}},
			}})
		case "/packages/demo-0.1.0.tgz":
			_, _ = w.Write(tarball)
		default:
			http.Error(w, "not found", 404)
		}
	}))
	defer srv.Close()

	reg, err := New(srv.URL)
	if err != nil {
		t.Fatal(err)
	}
	target := t.TempDir()
	name, version, err := reg.Install(context.Background(), "demo", "latest", target, false)
	if err != nil {
		t.Fatalf("Install: %v", err)
	}
	if name != "demo" || version != "0.1.0" {
		t.Fatalf("name=%s version=%s", name, version)
	}
	if _, err := os.Stat(filepath.Join(target, "demo", "plugin.yaml")); err != nil {
		t.Errorf("manifest not installed: %v", err)
	}
}

func TestInstall_ShaMismatch(t *testing.T) {
	tarball := buildDemoTarball(t)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/index.json":
			_ = json.NewEncoder(w).Encode(Index{Plugins: map[string]PluginEntry{
				"demo": {Versions: map[string]Package{"0.1.0": {
					URL:    "packages/demo-0.1.0.tgz",
					SHA256: "0000000000000000000000000000000000000000000000000000000000000000",
				}}},
			}})
		case "/packages/demo-0.1.0.tgz":
			_, _ = w.Write(tarball)
		}
	}))
	defer srv.Close()

	reg, _ := New(srv.URL)
	_, _, err := reg.Install(context.Background(), "demo", "", t.TempDir(), false)
	if err == nil || !containsIgnoreCase(err.Error(), "sha256 mismatch") {
		t.Fatalf("expected sha256 mismatch error, got %v", err)
	}
}

func TestInstall_EmptyShaRefused(t *testing.T) {
	tarball := buildDemoTarball(t)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/index.json":
			_ = json.NewEncoder(w).Encode(Index{Plugins: map[string]PluginEntry{
				"demo": {Versions: map[string]Package{"0.1.0": {URL: "packages/demo-0.1.0.tgz"}}},
			}})
		case "/packages/demo-0.1.0.tgz":
			_, _ = w.Write(tarball)
		}
	}))
	defer srv.Close()

	reg, _ := New(srv.URL)
	_, _, err := reg.Install(context.Background(), "demo", "", t.TempDir(), false)
	if err == nil {
		t.Fatal("expected error on missing sha256")
	}
}

func buildDemoTarball(t *testing.T) []byte {
	t.Helper()
	var buf bytes.Buffer
	gz := gzip.NewWriter(&buf)
	tw := tar.NewWriter(gz)
	body := []byte(`apiVersion: keystone.plugin/v1
kind: Plugin
metadata:
  name: demo
  displayName: Demo
  version: 0.1.0
  description: A demo plugin for store tests.
  maintainer: test <t@example.com>
  license: Apache-2.0
  category: transport
  tags: [demo]
  trustTier: experimental
spec:
  keystoneCoreMin: "0.5.0"
  capabilities:
    - transport.demo
  entrypoint:
    exec: bin/demo-plugin
`)
	_ = tw.WriteHeader(&tar.Header{Name: "plugin.yaml", Mode: 0o644, Size: int64(len(body)), Typeflag: tar.TypeReg})
	_, _ = tw.Write(body)
	_ = tw.Close()
	_ = gz.Close()
	return buf.Bytes()
}

func containsIgnoreCase(s, sub string) bool {
	return len(sub) == 0 || len(s) >= len(sub) && (indexInsensitive(s, sub) >= 0)
}

func indexInsensitive(s, sub string) int {
	for i := 0; i+len(sub) <= len(s); i++ {
		match := true
		for j := 0; j < len(sub); j++ {
			a, b := s[i+j], sub[j]
			if a >= 'A' && a <= 'Z' {
				a += 'a' - 'A'
			}
			if b >= 'A' && b <= 'Z' {
				b += 'a' - 'A'
			}
			if a != b {
				match = false
				break
			}
		}
		if match {
			return i
		}
	}
	return -1
}
