package manager_test

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/kliuchnikovv/keystone-api/sidecar"

	"github.com/kliuchnikovv/keystone/internal/plugin/manager"
	"github.com/kliuchnikovv/keystone/internal/plugin/registry"
)

// TestMain doubles as the stub plugin binary — see the supervisor package
// for the same pattern. Env-toggled entry so exec of os.Args[0] here spins
// up sidecar.RunPlugin instead of re-running the tests.
func TestMain(m *testing.M) {
	if os.Getenv("BE_STUB_PLUGIN") == "1" {
		runStub()
		return
	}
	os.Exit(m.Run())
}

type pingHandler struct{}

func (pingHandler) HandleRequest(_ context.Context, r *sidecar.Request) (any, error) {
	return map[string]any{"pong": true, "params": json.RawMessage(r.Params)}, nil
}

func runStub() {
	mux := sidecar.NewMux()
	mux.Handle("ping", pingHandler{})
	if err := sidecar.RunPlugin(context.Background(), sidecar.PluginOptions{
		Name:    os.Getenv("STUB_PLUGIN_NAME"),
		Version: "0.0.1",
		Handler: mux,
	}); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func TestManager_DiscoverEnableCallDisable(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns a subprocess")
	}

	root := t.TempDir()
	name := "stub"
	writeStubManifest(t, root, name, os.Args[0])

	reg := registry.New(root, nil)
	mgr, err := manager.New(manager.Options{Registry: reg})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if err := mgr.Discover(); err != nil {
		t.Fatalf("Discover: %v", err)
	}

	list := mgr.List()
	if len(list) != 1 || list[0].Name != name || list[0].State != manager.StateDiscovered {
		t.Fatalf("unexpected initial list: %+v", list)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := mgr.Enable(ctx, name); err != nil {
		t.Fatalf("Enable: %v", err)
	}

	st, _ := mgr.Get(name)
	if st.State != manager.StateRunning {
		t.Fatalf("expected running, got %s (lastErr=%s)", st.State, st.LastError)
	}
	if !st.Connected {
		t.Fatal("expected connected")
	}

	client := mgr.Client(name)
	if client == nil {
		t.Fatal("Client returned nil")
	}
	var got struct {
		Pong bool `json:"pong"`
	}
	if err := client.Call(ctx, "ping", map[string]any{"x": 1}, &got); err != nil {
		t.Fatalf("Call ping: %v", err)
	}
	if !got.Pong {
		t.Fatal("no pong")
	}

	if err := mgr.Disable(ctx, name); err != nil {
		t.Fatalf("Disable: %v", err)
	}
	st, _ = mgr.Get(name)
	if st.State != manager.StateStopped {
		t.Fatalf("expected stopped, got %s", st.State)
	}
}

func TestManager_EnableUnknown(t *testing.T) {
	mgr, err := manager.New(manager.Options{Registry: registry.New(t.TempDir(), nil)})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if err := mgr.Enable(context.Background(), "ghost"); err == nil {
		t.Fatal("expected error on unknown plugin")
	}
}

func TestManager_DiscoverRefreshesButKeepsRunning(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns a subprocess")
	}
	root := t.TempDir()
	name := "stub"
	writeStubManifest(t, root, name, os.Args[0])

	mgr, err := manager.New(manager.Options{Registry: registry.New(root, nil)})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if err := mgr.Discover(); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := mgr.Enable(ctx, name); err != nil {
		t.Fatalf("Enable: %v", err)
	}
	t.Cleanup(func() { _ = mgr.Disable(context.Background(), name) })

	// Second Discover must not reset a running plugin back to Discovered.
	if err := mgr.Discover(); err != nil {
		t.Fatal(err)
	}
	st, _ := mgr.Get(name)
	if st.State != manager.StateRunning {
		t.Fatalf("Discover regressed running plugin to %s", st.State)
	}
}

func writeStubManifest(t *testing.T, root, name, execPath string) {
	t.Helper()
	dir := filepath.Join(root, name)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	// The manifest schema mandates a relative exec, so symlink the test
	// binary into the plugin dir and address it by its short name.
	link := filepath.Join(dir, "bin")
	if err := os.Symlink(execPath, link); err != nil {
		t.Fatalf("symlink test binary: %v", err)
	}
	yaml := fmt.Sprintf(`apiVersion: keystone.plugin/v1
kind: Plugin
metadata:
  name: %s
  displayName: Stub
  version: 0.0.1
  description: Test plugin used by manager tests.
  maintainer: test <test@example.com>
  license: Apache-2.0
  category: transport
  trustTier: verified
spec:
  keystoneCoreMin: "0.5.0"
  capabilities:
    - test.stub
  entrypoint:
    exec: bin
    env:
      BE_STUB_PLUGIN: "1"
      STUB_PLUGIN_NAME: %s
`, name, name)
	if err := os.WriteFile(filepath.Join(dir, "plugin.yaml"), []byte(yaml), 0o644); err != nil {
		t.Fatal(err)
	}
}
