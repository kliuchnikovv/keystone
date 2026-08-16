package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func TestInstallFromDir_CopiesManifestAndBinary(t *testing.T) {
	src := scaffoldForInstall(t)
	target := t.TempDir()

	name, err := installFromDir(src, target, false)
	if err != nil {
		t.Fatalf("installFromDir: %v", err)
	}
	if name != "demo" {
		t.Fatalf("name = %s", name)
	}
	for _, want := range []string{"plugin.yaml", "bin/demo-plugin"} {
		if _, err := os.Stat(filepath.Join(target, "demo", want)); err != nil {
			t.Errorf("missing %s: %v", want, err)
		}
	}
	// Exec bit preserved.
	info, err := os.Stat(filepath.Join(target, "demo", "bin", "demo-plugin"))
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode()&0o111 == 0 {
		t.Errorf("exec bit lost: %o", info.Mode())
	}
}

func TestInstallFromDir_RefusesOverwrite(t *testing.T) {
	src := scaffoldForInstall(t)
	target := t.TempDir()
	if _, err := installFromDir(src, target, false); err != nil {
		t.Fatal(err)
	}
	if _, err := installFromDir(src, target, false); err == nil {
		t.Fatal("expected refusal without --force")
	}
	if _, err := installFromDir(src, target, true); err != nil {
		t.Fatalf("--force should overwrite: %v", err)
	}
}

func TestInstallFromTarball_RoundTrip(t *testing.T) {
	src := scaffoldForInstall(t)
	// Pack, then install the tarball.
	tarball := filepath.Join(t.TempDir(), "demo.tgz")
	if err := writeTarball(src, tarball); err != nil {
		t.Fatal(err)
	}
	target := t.TempDir()
	name, err := installFromTarball(tarball, target, false)
	if err != nil {
		t.Fatalf("installFromTarball: %v", err)
	}
	if name != "demo" {
		t.Fatalf("name = %s", name)
	}
	info, err := os.Stat(filepath.Join(target, "demo", "bin", "demo-plugin"))
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode()&0o111 == 0 {
		t.Errorf("exec bit lost through tarball round-trip: %o", info.Mode())
	}
}

func TestInstall_NotifyDiscover(t *testing.T) {
	var hits int
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/plugins/discover" {
			http.Error(w, "wrong path", 400)
			return
		}
		hits++
		_ = json.NewEncoder(w).Encode(map[string]any{"plugins": []any{}})
	}))
	defer srv.Close()
	if err := notifyDiscover(srv.URL); err != nil {
		t.Fatalf("notifyDiscover: %v", err)
	}
	if hits != 1 {
		t.Fatalf("expected 1 hit, got %d", hits)
	}
}

func TestSafePluginName_Rejects(t *testing.T) {
	bad := []string{"", "../evil", "sub/dir", ".hidden", `back\slash`}
	for _, n := range bad {
		if err := safePluginName(n); err == nil {
			t.Errorf("safePluginName(%q) should error", n)
		}
	}
	if err := safePluginName("demo"); err != nil {
		t.Errorf("safePluginName(\"demo\") errored: %v", err)
	}
}

func TestPrepareDest_RefusesSymlinkTarget(t *testing.T) {
	targetRoot := t.TempDir()
	elsewhere := t.TempDir()
	if err := os.Symlink(elsewhere, filepath.Join(targetRoot, "demo")); err != nil {
		t.Skipf("symlink not supported: %v", err)
	}
	err := prepareDest(filepath.Join(targetRoot, "demo"), targetRoot, true)
	if err == nil {
		t.Fatal("prepareDest should refuse a symlink target — RemoveAll would follow it")
	}
}

// scaffoldForInstall builds a minimal but valid plugin dir the install
// path can consume.
func scaffoldForInstall(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "plugin.yaml"), []byte(demoManifest), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(dir, "bin"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "bin", "demo-plugin"), []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	return dir
}

var demoManifest = `apiVersion: keystone.plugin/v1
kind: Plugin
metadata:
  name: demo
  displayName: Demo
  version: 0.1.0
  description: A demo plugin for install tests.
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
`
