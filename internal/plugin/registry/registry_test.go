package registry_test

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/kliuchnikovv/keystone/internal/plugin/registry"
)

func TestDiscover_EmptyRoot(t *testing.T) {
	entries, err := registry.New(t.TempDir(), nil).Discover()
	if err != nil {
		t.Fatalf("Discover: %v", err)
	}
	if len(entries) != 0 {
		t.Fatalf("expected empty, got %d entries", len(entries))
	}
}

func TestDiscover_MissingRoot(t *testing.T) {
	// A registry pointed at a nonexistent dir should return zero entries,
	// not an error. A fresh install has no plugins yet.
	entries, err := registry.New(filepath.Join(t.TempDir(), "does-not-exist"), nil).Discover()
	if err != nil {
		t.Fatalf("Discover: %v", err)
	}
	if len(entries) != 0 {
		t.Fatalf("expected empty, got %d", len(entries))
	}
}

func TestDiscover_LoadsAndValidates(t *testing.T) {
	root := t.TempDir()
	writePlugin(t, root, "matter", "../testdata/valid/matter.yaml")
	writePlugin(t, root, "dirigera", "../testdata/valid/dirigera.yaml")

	entries, err := registry.New(root, nil).Discover()
	if err != nil {
		t.Fatalf("Discover: %v", err)
	}
	if len(entries) != 2 {
		t.Fatalf("expected 2 entries, got %d", len(entries))
	}
	names := []string{entries[0].Name(), entries[1].Name()}
	// Independent plugins sort alphabetically.
	if names[0] != "dirigera" || names[1] != "matter" {
		t.Fatalf("unexpected order: %v", names)
	}
}

func TestDiscover_BrokenManifestReportedNotDropped(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "junk")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "plugin.yaml"), []byte("kind: NotAPlugin\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	entries, err := registry.New(root, nil).Discover()
	if err != nil {
		t.Fatalf("Discover: %v", err)
	}
	if len(entries) != 1 {
		t.Fatalf("expected 1 broken entry, got %d", len(entries))
	}
	if entries[0].Err == nil {
		t.Fatal("expected Err set on broken manifest")
	}
	if entries[0].Name() != "junk" {
		t.Fatalf("expected fallback name from dir, got %q", entries[0].Name())
	}
}

func TestDiscover_IgnoresNonPluginDirs(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "backups"), 0o755); err != nil {
		t.Fatal(err)
	}
	writePlugin(t, root, "matter", "../testdata/valid/matter.yaml")

	entries, err := registry.New(root, nil).Discover()
	if err != nil {
		t.Fatalf("Discover: %v", err)
	}
	if len(entries) != 1 {
		t.Fatalf("expected 1 entry (backups ignored), got %d", len(entries))
	}
}

func writePlugin(t *testing.T, root, name, src string) {
	t.Helper()
	dir := filepath.Join(root, name)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(src)
	if err != nil {
		t.Fatalf("read fixture %s: %v", src, err)
	}
	if err := os.WriteFile(filepath.Join(dir, "plugin.yaml"), data, 0o644); err != nil {
		t.Fatal(err)
	}
}
