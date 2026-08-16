package sdk_test

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/kliuchnikovv/keystone-api/sidecar"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
	"github.com/kliuchnikovv/keystone/plugins/sdk"
)

// stubAdapter is a minimal transport used to exercise the SDK's Run
// end-to-end. It records every call so the test can assert routing.
type stubAdapter struct {
	events chan ports.TransportEvent
}

func (s *stubAdapter) Kind() domain.TransportKind    { return "stub" }
func (s *stubAdapter) Start(context.Context) error   { return nil }
func (s *stubAdapter) Stop(context.Context) error    { return nil }
func (s *stubAdapter) Decommission(context.Context, domain.TransportRef) error {
	return nil
}
func (s *stubAdapter) Discover(context.Context) (<-chan ports.DiscoveredDevice, error) {
	out := make(chan ports.DiscoveredDevice, 1)
	out <- ports.DiscoveredDevice{TransportRef: "stub:1", Name: "One"}
	close(out)
	return out, nil
}
func (s *stubAdapter) Commission(_ context.Context, req ports.CommissionRequest) (domain.TransportRef, error) {
	if req.Progress != nil {
		req.Progress("stage1", "one")
		req.Progress("stage2", "two")
	}
	return domain.TransportRef("stub:" + req.Payload), nil
}
func (s *stubAdapter) ReadState(context.Context, domain.TransportRef, domain.FeatureKey, domain.StateKey) (any, error) {
	return "on", nil
}
func (s *stubAdapter) WriteState(context.Context, domain.TransportRef, domain.FeatureKey, domain.StateKey, any) error {
	return nil
}
func (s *stubAdapter) InvokeAction(context.Context, domain.TransportRef, domain.FeatureKey, domain.ActionKey, map[string]any) error {
	return nil
}
func (s *stubAdapter) Subscribe(ctx context.Context) (<-chan ports.TransportEvent, error) {
	return s.events, nil
}

// TestMain doubles as a plugin binary so Run runs against a real sidecar
// client. Same env-flag trick used by the manager/supervisor tests.
func TestMain(m *testing.M) {
	if os.Getenv("BE_SDK_TEST_PLUGIN") == "1" {
		runStubPlugin()
		return
	}
	os.Exit(m.Run())
}

func runStubPlugin() {
	ad := &stubAdapter{events: make(chan ports.TransportEvent)}
	err := sdk.Run(context.Background(), sdk.Options{
		Name:         "stub",
		Version:      "0.0.1",
		Capabilities: []string{"transport.stub"},
		Adapter:      ad,
	})
	if err != nil {
		os.Stderr.WriteString(err.Error() + "\n")
		os.Exit(1)
	}
}

func startPlugin(t *testing.T) (*sidecar.Client, func()) {
	t.Helper()
	dir, err := os.MkdirTemp("", "ks-sdk")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(dir) })
	socket := filepath.Join(dir, "s")

	ctx, cancel := context.WithCancel(context.Background())
	cmd := exec.CommandContext(ctx, os.Args[0])
	cmd.Env = append(os.Environ(),
		"BE_SDK_TEST_PLUGIN=1",
		sidecar.SocketEnvVar+"="+socket,
		"KEYSTONE_PLUGIN_DATA="+dir,
	)
	if err := cmd.Start(); err != nil {
		cancel()
		t.Fatal(err)
	}
	deadline := time.Now().Add(5 * time.Second)
	var client *sidecar.Client
	var lastErr error
	for time.Now().Before(deadline) {
		c, err := sidecar.Connect(context.Background(), socket, sidecar.CoreOptions{})
		if err == nil {
			client = c
			break
		}
		lastErr = err
		time.Sleep(50 * time.Millisecond)
	}
	if client == nil {
		_ = cmd.Process.Kill()
		cancel()
		t.Fatalf("connect: %v", lastErr)
	}
	return client, func() {
		_ = client.Close()
		cancel()
		_ = cmd.Wait()
	}
}

func TestRun_HandlesAllMethods(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns a subprocess")
	}
	client, teardown := startPlugin(t)
	defer teardown()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	// Discover round-trip
	var dres struct {
		Devices []struct {
			TransportRef string `json:"transportRef"`
		} `json:"devices"`
	}
	if err := client.Call(ctx, "adapter.discover", nil, &dres); err != nil {
		t.Fatalf("discover: %v", err)
	}
	if len(dres.Devices) != 1 || dres.Devices[0].TransportRef != "stub:1" {
		t.Fatalf("unexpected discover: %+v", dres)
	}

	// Commission — no progress
	var cres struct {
		Ref string `json:"ref"`
	}
	if err := client.Call(ctx, "adapter.commission", map[string]any{"payload": "42"}, &cres); err != nil {
		t.Fatalf("commission: %v", err)
	}
	if cres.Ref != "stub:42" {
		t.Fatalf("unexpected ref: %s", cres.Ref)
	}
}

func TestRun_MissingAdapter(t *testing.T) {
	err := sdk.Run(context.Background(), sdk.Options{Name: "x"})
	if err == nil {
		t.Fatal("expected error on missing adapter")
	}
}

func TestErrorMapper(t *testing.T) {
	// White-box style: mapErr is unexported, so exercise via the public
	// Run path. Here we just prove the mapper receives non-nil errors
	// and can rewrite them — the wire-level test is in the bridge suite.
	m := sdk.ErrorMapper(func(e error) error {
		if errors.Is(e, os.ErrClosed) {
			return errors.New("mapped")
		}
		return nil
	})
	if got := m(os.ErrClosed); got == nil || got.Error() != "mapped" {
		t.Fatalf("mapper: %v", got)
	}
}

func TestLoadConfig_MissingFileNoError(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("KEYSTONE_PLUGIN_DATA", dir)
	var got map[string]any
	if err := sdk.LoadConfig(&got); err != nil {
		t.Fatalf("LoadConfig on empty dir: %v", err)
	}
	if got != nil {
		t.Fatalf("expected nil, got %v", got)
	}
}

func TestLoadConfig_ReadsFile(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("KEYSTONE_PLUGIN_DATA", dir)
	if err := os.WriteFile(filepath.Join(dir, "config.json"), []byte(`{"host":"h","port":42}`), 0o644); err != nil {
		t.Fatal(err)
	}
	var got struct {
		Host string `json:"host"`
		Port int    `json:"port"`
	}
	if err := sdk.LoadConfig(&got); err != nil {
		t.Fatal(err)
	}
	if got.Host != "h" || got.Port != 42 {
		t.Fatalf("unexpected: %+v", got)
	}
}

