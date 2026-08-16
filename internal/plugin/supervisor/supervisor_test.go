package supervisor

import (
	"context"
	"encoding/json"
	"os"
	"testing"
	"time"

	"github.com/kliuchnikovv/keystone-api/sidecar"
)

type echoHandler struct{}

func (echoHandler) HandleRequest(_ context.Context, r *sidecar.Request) (any, error) {
	return map[string]any{"echoed": json.RawMessage(r.Params)}, nil
}

// TestMain doubles the test binary as the stub plugin. When
// BE_STUB_PLUGIN=1 is set, we skip the test runner and run RunPlugin
// instead — this is the standard Go trick for exercising os/exec paths
// without a separate binary.
func TestMain(m *testing.M) {
	if os.Getenv("BE_STUB_PLUGIN") == "1" {
		runStubPlugin()
		return
	}
	os.Exit(m.Run())
}

func runStubPlugin() {
	mux := sidecar.NewMux()
	mux.Handle("echo", echoHandler{})
	err := sidecar.RunPlugin(context.Background(), sidecar.PluginOptions{
		Name:    "stub",
		Version: "0.0.1",
		Handler: mux,
	})
	if err != nil {
		os.Stderr.WriteString("stub plugin: " + err.Error() + "\n")
		os.Exit(1)
	}
}

func TestSupervisor_SpawnAndCall(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns a subprocess")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	logs := make(chan string, 32)
	sv, err := New(Config{
		Name:    "stub",
		Version: "0.0.1",
		Exec:    []string{os.Args[0]},
		Env:     []string{"BE_STUB_PLUGIN=1"},
		Restart: RestartNever,
		OnStderr: func(line string) {
			select {
			case logs <- line:
			default:
			}
		},
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	client, err := sv.Start(ctx)
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	t.Cleanup(func() { _ = sv.Stop(context.Background()) })

	var got struct {
		Echoed json.RawMessage `json:"echoed"`
	}
	if err := client.Call(ctx, "echo", map[string]any{"hello": "world"}, &got); err != nil {
		t.Fatalf("Call: %v", err)
	}
	if string(got.Echoed) != `{"hello":"world"}` {
		t.Fatalf("unexpected echo payload: %s", got.Echoed)
	}
}

func TestSupervisor_RestartOnCrash(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns a subprocess")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	sv, err := New(Config{
		Name:    "stub",
		Exec:    []string{os.Args[0]},
		Env:     []string{"BE_STUB_PLUGIN=1"},
		Restart: RestartAlways,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	client, err := sv.Start(ctx)
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	t.Cleanup(func() { _ = sv.Stop(context.Background()) })

	sv.mu.Lock()
	proc := sv.proc
	sv.mu.Unlock()
	if proc == nil {
		t.Fatal("expected running child after Start")
	}
	if err := proc.Kill(); err != nil {
		t.Fatalf("Kill first child: %v", err)
	}

	// Client is expected to reconnect once the supervisor respawns the child.
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if err := client.Call(ctx, "echo", map[string]any{"ping": 1}, new(any)); err == nil {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatal("client did not reconnect after respawn")
}
