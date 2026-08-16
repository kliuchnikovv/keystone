package main

import (
	"archive/tar"
	"compress/gzip"
	"os"
	"path/filepath"
	"testing"
)

func TestWriteTarball_PacksManifestAndBin(t *testing.T) {
	pluginDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(pluginDir, "plugin.yaml"), []byte("kind: Plugin\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(pluginDir, "bin"), 0o755); err != nil {
		t.Fatal(err)
	}
	// Executable bit must survive the round-trip: unpacking on the
	// core side has to leave the entrypoint runnable.
	if err := os.WriteFile(filepath.Join(pluginDir, "bin", "demo-plugin"), []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}

	tarball := filepath.Join(pluginDir, "out.tgz")
	if err := writeTarball(pluginDir, tarball); err != nil {
		t.Fatal(err)
	}

	names := readTarballNames(t, tarball)
	if !contains(names, "plugin.yaml") {
		t.Errorf("missing plugin.yaml: %v", names)
	}
	if !contains(names, "bin/demo-plugin") {
		t.Errorf("missing bin/demo-plugin: %v", names)
	}
	// Verify the exec bit came across.
	f, err := os.Open(tarball)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	gz, _ := gzip.NewReader(f)
	tr := tar.NewReader(gz)
	for {
		hdr, err := tr.Next()
		if err != nil {
			break
		}
		if hdr.Name == "bin/demo-plugin" && hdr.Mode&0o111 == 0 {
			t.Errorf("exec bit lost: mode = %o", hdr.Mode)
		}
	}
}

func TestWriteTarball_SkipsMissingOptionalDirs(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "plugin.yaml"), []byte("kind: Plugin\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	// No bin/, sidecars/, ui/ — publish must still succeed.
	tarball := filepath.Join(dir, "empty.tgz")
	if err := writeTarball(dir, tarball); err != nil {
		t.Fatalf("writeTarball with only manifest failed: %v", err)
	}
	names := readTarballNames(t, tarball)
	if len(names) != 1 || names[0] != "plugin.yaml" {
		t.Errorf("expected only plugin.yaml, got %v", names)
	}
}

func readTarballNames(t *testing.T, path string) []string {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	gz, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	tr := tar.NewReader(gz)
	var out []string
	for {
		hdr, err := tr.Next()
		if err != nil {
			return out
		}
		if hdr.Typeflag == tar.TypeReg {
			out = append(out, hdr.Name)
		}
	}
}

func contains(list []string, s string) bool {
	for _, v := range list {
		if v == s {
			return true
		}
	}
	return false
}
