package bridge_test

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/kliuchnikovv/keystone-api/sidecar"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/plugin/bridge"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// A tiny reference plugin the tests spawn. It answers every bridge method
// with recorded fixtures so we can assert the round-trip end-to-end.
func TestMain(m *testing.M) {
	if os.Getenv("BE_STUB_ADAPTER_PLUGIN") == "1" {
		runStubAdapter()
		return
	}
	os.Exit(m.Run())
}

func runStubAdapter() {
	mux := sidecar.NewMux()
	mux.Handle(bridge.MethodStart, sidecar.HandlerFunc(func(_ context.Context, _ *sidecar.Request) (any, error) {
		return map[string]string{}, nil
	}))
	mux.Handle(bridge.MethodStop, sidecar.HandlerFunc(func(_ context.Context, _ *sidecar.Request) (any, error) {
		return map[string]string{}, nil
	}))
	mux.Handle(bridge.MethodDiscover, sidecar.HandlerFunc(func(_ context.Context, _ *sidecar.Request) (any, error) {
		return bridge.DiscoverResult{Devices: []bridge.DiscoveredDevice{
			{TransportRef: "stub:1", Type: "light", Name: "Lamp One"},
		}}, nil
	}))
	mux.Handle(bridge.MethodCommission, sidecar.HandlerFunc(func(_ context.Context, r *sidecar.Request) (any, error) {
		var p bridge.CommissionParams
		if err := r.Bind(&p); err != nil {
			return nil, err
		}
		if p.ProgressID != "" {
			peer := r.Peer()
			for _, stage := range []string{"pase", "attestation", "operational"} {
				_ = peer.Publish(sidecar.Topic(bridge.TopicEvent), bridge.EventPayload{
					Kind:         bridge.KindCommissionProgress,
					CommissionID: p.ProgressID,
					Stage:        stage,
					Message:      "step " + stage,
				})
			}
		}
		return bridge.CommissionResult{Ref: domain.TransportRef("stub:" + p.Payload)}, nil
	}))
	mux.Handle(bridge.MethodReadState, sidecar.HandlerFunc(func(_ context.Context, _ *sidecar.Request) (any, error) {
		return bridge.ReadStateResult{Value: true}, nil
	}))
	mux.Handle(bridge.MethodWriteState, sidecar.HandlerFunc(func(_ context.Context, _ *sidecar.Request) (any, error) {
		return map[string]string{}, nil
	}))
	mux.Handle(bridge.MethodInvoke, sidecar.HandlerFunc(func(_ context.Context, _ *sidecar.Request) (any, error) {
		return map[string]string{}, nil
	}))
	mux.Handle(bridge.MethodDecommission, sidecar.HandlerFunc(func(_ context.Context, _ *sidecar.Request) (any, error) {
		return map[string]string{}, nil
	}))

	mux.Handle(bridge.MethodCameraStart, sidecar.HandlerFunc(func(_ context.Context, r *sidecar.Request) (any, error) {
		var p bridge.StartStreamParams
		if err := r.Bind(&p); err != nil {
			return nil, err
		}
		// Fake a session and push an "answer" signal back.
		peer := r.Peer()
		_ = peer.Publish(sidecar.Topic(bridge.TopicEvent), bridge.EventPayload{
			Kind: bridge.KindCameraSignal,
			Signal: &bridge.CameraSignalPayload{
				Kind: "answer", SessionID: 42, SDP: "v=0-fake",
			},
		})
		return bridge.StartStreamResult{SessionID: 42}, nil
	}))
	mux.Handle(bridge.MethodCameraStop, sidecar.HandlerFunc(func(_ context.Context, _ *sidecar.Request) (any, error) {
		return map[string]string{}, nil
	}))

	mux.Handle(bridge.MethodDiscoverCommissionable, sidecar.HandlerFunc(func(_ context.Context, r *sidecar.Request) (any, error) {
		var p bridge.DiscoverCommissionableParams
		if err := r.Bind(&p); err != nil {
			return nil, err
		}
		peer := r.Peer()
		for _, ref := range []string{"stub:a", "stub:b"} {
			_ = peer.Publish(sidecar.Topic(bridge.TopicEvent), bridge.EventPayload{
				Kind: bridge.KindCommissionableFound,
				Found: &bridge.CommissionableFoundPayload{
					ScanID: p.ScanID,
					Ref:    ref,
				},
			})
		}
		return bridge.DiscoverCommissionableResult{Ended: true}, nil
	}))

	opts := sidecar.PluginOptions{
		Name:    "stub-adapter",
		Version: "0.0.1",
		Handler: mux,
		OnConnect: func(ctx context.Context, p *sidecar.Peer) {
			// Give the test time to Subscribe before publishing.
			time.Sleep(200 * time.Millisecond)
			_ = p.Publish(sidecar.Topic(bridge.TopicEvent), bridge.EventPayload{
				Ref: "stub:1", Kind: ports.TransportEventStateChanged,
				Feature: "on_off", Key: "on", Value: true,
			})
			<-ctx.Done()
		},
	}
	if err := sidecar.RunPlugin(context.Background(), opts); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

// startStub spawns the test binary as an adapter plugin and dials it. It
// gives us a live sidecar.Client without pulling in supervisor+manager.
func startStub(t *testing.T) (*sidecar.Client, func()) {
	t.Helper()
	// Darwin caps Unix socket paths at 104 chars, so t.TempDir() (which
	// embeds the full test name) can exceed the limit for long test
	// names. A short prefix here keeps every case comfortably under.
	dir, err := os.MkdirTemp("", "ks-b")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(dir) })
	socket := filepath.Join(dir, "s")

	ctx, cancel := context.WithCancel(context.Background())
	cmd := exec.CommandContext(ctx, os.Args[0])
	cmd.Env = append(os.Environ(),
		"BE_STUB_ADAPTER_PLUGIN=1",
		sidecar.SocketEnvVar+"="+socket,
	)
	if err := cmd.Start(); err != nil {
		cancel()
		t.Fatalf("spawn stub: %v", err)
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
		t.Fatalf("connect stub: %v", lastErr)
	}
	cleanup := func() {
		_ = client.Close()
		cancel()
		_ = cmd.Wait()
	}
	return client, cleanup
}

func TestAdapter_RoundTrip(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns a subprocess")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	client, teardown := startStub(t)
	defer teardown()

	a := bridge.New(client, "stub", nil)

	if err := a.Start(ctx); err != nil {
		t.Fatalf("Start: %v", err)
	}
	if a.Kind() != "stub" {
		t.Fatalf("Kind: got %q", a.Kind())
	}

	ch, err := a.Discover(ctx)
	if err != nil {
		t.Fatalf("Discover: %v", err)
	}
	got := <-ch
	if got.TransportRef != "stub:1" || got.Name != "Lamp One" {
		t.Fatalf("unexpected discover: %+v", got)
	}
	if _, ok := <-ch; ok {
		t.Fatal("discover channel should close after snapshot")
	}

	ref, err := a.Commission(ctx, ports.CommissionRequest{Payload: "42"})
	if err != nil {
		t.Fatalf("Commission: %v", err)
	}
	if ref != "stub:42" {
		t.Fatalf("Commission ref: %q", ref)
	}

	v, err := a.ReadState(ctx, "stub:1", "on_off", "on")
	if err != nil {
		t.Fatalf("ReadState: %v", err)
	}
	if v != true {
		t.Fatalf("ReadState value: %v", v)
	}

	if err := a.WriteState(ctx, "stub:1", "on_off", "on", false); err != nil {
		t.Fatalf("WriteState: %v", err)
	}
	if err := a.InvokeAction(ctx, "stub:1", "on_off", "toggle", nil); err != nil {
		t.Fatalf("InvokeAction: %v", err)
	}
	if err := a.Decommission(ctx, "stub:1"); err != nil {
		t.Fatalf("Decommission: %v", err)
	}

	if err := a.Stop(ctx); err != nil {
		t.Fatalf("Stop: %v", err)
	}
}

func TestAdapter_CommissionProgress(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns a subprocess")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	client, teardown := startStub(t)
	defer teardown()

	a := bridge.New(client, "stub", nil)
	if err := a.Start(ctx); err != nil {
		t.Fatalf("Start: %v", err)
	}
	// Subscribe must be running so bridge routes progress events.
	if _, err := a.Subscribe(ctx); err != nil {
		t.Fatalf("Subscribe: %v", err)
	}

	var stages []string
	_, err := a.Commission(ctx, ports.CommissionRequest{
		Payload: "42",
		Progress: func(stage, _ string) {
			stages = append(stages, stage)
		},
	})
	if err != nil {
		t.Fatalf("Commission: %v", err)
	}
	// Give the fan-out goroutine a moment to drain — deferred close waits
	// on it, but the value collection happens outside of that.
	time.Sleep(100 * time.Millisecond)
	if len(stages) == 0 {
		t.Fatal("no progress delivered")
	}
	// Order matters — the plugin publishes deterministically.
	want := []string{"pase", "attestation", "operational"}
	for i := range want {
		if i >= len(stages) || stages[i] != want[i] {
			t.Fatalf("stages = %v, want %v", stages, want)
		}
	}
}

func TestAdapter_CameraSignalsFanOut(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns a subprocess")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	client, teardown := startStub(t)
	defer teardown()

	a := bridge.New(client, "stub", nil)
	if err := a.Start(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := a.Subscribe(ctx); err != nil {
		t.Fatal(err)
	}
	sig, err := a.Signals(ctx)
	if err != nil {
		t.Fatal(err)
	}
	id, err := a.StartStream(ctx, "stub:cam1", "v=0-viewer")
	if err != nil {
		t.Fatalf("StartStream: %v", err)
	}
	if id != 42 {
		t.Fatalf("sessionID = %d", id)
	}
	select {
	case s := <-sig:
		if s.Kind != "answer" || s.SessionID != 42 || s.SDP != "v=0-fake" {
			t.Fatalf("unexpected signal: %+v", s)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("no signal delivered")
	}
}

func TestAdapter_DiscoverCommissionable(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns a subprocess")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	client, teardown := startStub(t)
	defer teardown()

	a := bridge.New(client, "stub", nil)
	if err := a.Start(ctx); err != nil {
		t.Fatal(err)
	}
	// Subscribe must be running so find events route into the scan channel.
	if _, err := a.Subscribe(ctx); err != nil {
		t.Fatal(err)
	}
	ch, err := a.DiscoverCommissionable(ctx, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for d := range ch {
		got = append(got, d.Ref)
	}
	if len(got) != 2 || got[0] != "stub:a" || got[1] != "stub:b" {
		t.Fatalf("unexpected finds: %v", got)
	}
}

func TestAdapter_SubscribeFansOutEvents(t *testing.T) {
	if testing.Short() {
		t.Skip("spawns a subprocess")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	client, teardown := startStub(t)
	defer teardown()

	a := bridge.New(client, "stub", nil)
	if err := a.Start(ctx); err != nil {
		t.Fatalf("Start: %v", err)
	}
	sub, err := a.Subscribe(ctx)
	if err != nil {
		t.Fatalf("Subscribe: %v", err)
	}

	select {
	case ev, ok := <-sub:
		if !ok {
			t.Fatal("channel closed early")
		}
		if ev.Ref != "stub:1" || ev.Kind != ports.TransportEventStateChanged || ev.Feature != "on_off" || ev.Value != true {
			t.Fatalf("unexpected event: %+v", ev)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("no event received")
	}
}
