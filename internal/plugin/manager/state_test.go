package manager_test

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/kliuchnikovv/keystone/internal/plugin/manager"
	"github.com/kliuchnikovv/keystone/internal/plugin/registry"
)

func TestStateStore_PersistsEnableDisable(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns a subprocess")
	}
	root := t.TempDir()
	name := "stub"
	writeStubManifest(t, root, name, os.Args[0])
	statePath := filepath.Join(t.TempDir(), "state.json")

	mgr, err := manager.New(manager.Options{
		Registry:   registry.New(root, nil),
		StateStore: &manager.FileStateStore{Path: statePath},
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := mgr.Discover(); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := mgr.Enable(ctx, name); err != nil {
		t.Fatalf("Enable: %v", err)
	}
	if got := readState(t, statePath); !got[name] {
		t.Fatalf("state should mark %q enabled, got %v", name, got)
	}
	if err := mgr.Disable(ctx, name); err != nil {
		t.Fatalf("Disable: %v", err)
	}
	if got := readState(t, statePath); got[name] {
		t.Fatalf("state should mark %q disabled, got %v", name, got)
	}
}

func TestStateStore_RestoreEnabled(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns a subprocess")
	}
	root := t.TempDir()
	name := "stub"
	writeStubManifest(t, root, name, os.Args[0])
	statePath := filepath.Join(t.TempDir(), "state.json")
	// Seed the file as if a prior daemon left this plugin enabled.
	if err := os.WriteFile(statePath, []byte(`{"`+name+`": true}`), 0o644); err != nil {
		t.Fatal(err)
	}

	mgr, err := manager.New(manager.Options{
		Registry:   registry.New(root, nil),
		StateStore: &manager.FileStateStore{Path: statePath},
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := mgr.Discover(); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	mgr.RestoreEnabled(ctx)
	t.Cleanup(func() { _ = mgr.Disable(context.Background(), name) })

	st, _ := mgr.Get(name)
	if st.State != manager.StateRunning {
		t.Fatalf("restore should have started plugin, got %s (err=%s)", st.State, st.LastError)
	}
}

func readState(t *testing.T, path string) map[string]bool {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read state: %v", err)
	}
	var m map[string]bool
	if err := json.Unmarshal(raw, &m); err != nil {
		t.Fatalf("parse state: %v", err)
	}
	return m
}
