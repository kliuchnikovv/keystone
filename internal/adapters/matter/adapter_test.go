package matter

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"sync"
	"testing"
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

func testLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

// nextDeviceEvent waits for the next device-scoped event, skipping the
// adapter_status frames every subscriber receives on subscribe and on each
// connectivity change.
func nextDeviceEvent(t *testing.T, sub <-chan ports.TransportEvent, within time.Duration) ports.TransportEvent {
	t.Helper()
	deadline := time.After(within)
	for {
		select {
		case ev := <-sub:
			if ev.Kind == ports.TransportEventAdapterStatus {
				continue
			}
			return ev
		case <-deadline:
			t.Fatal("no device event received")
			return ports.TransportEvent{}
		}
	}
}

// The adapter must come back on its own after the sidecar restarts mid-run.
// Before the Disconnected hand-off, pumpEvents simply blocked forever on a
// channel nobody would ever write to again: the adapter still reported itself
// as started, RPCs failed with a confusing "not connected", and no reconnect
// was ever attempted short of restarting keystone.
func TestAdapterReconnectsAfterSidecarDrop(t *testing.T) {
	sc := newFakeSidecar(t, func(req Request) any { return []Node{} })
	defer sc.Close()

	client := NewWSClient(sc.URL(), testLogger())
	a := New(testLogger(), DefaultConfig(), client)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	if err := a.Start(ctx); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() { _ = a.Stop(context.Background()) }()

	sub, err := a.Subscribe(ctx)
	if err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	if _, err := a.Discover(ctx); err != nil {
		t.Fatalf("discover before drop: %v", err)
	}

	sc.dropCurrent()

	// connectLoop backs off before its first retry, so give it room.
	deadline := time.Now().Add(6 * time.Second)
	for {
		if sc.acceptCount() >= 2 {
			if _, err := a.Discover(ctx); err == nil {
				break
			}
		}
		if time.Now().After(deadline) {
			t.Fatalf("adapter never reconnected (sidecar accepts=%d)", sc.acceptCount())
		}
		time.Sleep(50 * time.Millisecond)
	}

	// The subscription established before the drop must keep working over the
	// new connection — subscribers are not torn down by a reconnect.
	sc.pushes <- Response{
		Event: EventAttributeChanged,
		Data:  json.RawMessage(`{"nodeId":"n1","endpointId":1,"cluster":"OnOff","attribute":"OnOff","value":true}`),
	}

	ev := nextDeviceEvent(t, sub, 3*time.Second)
	if ev.Kind != ports.TransportEventStateChanged {
		t.Errorf("event kind = %v", ev.Kind)
	}
	if ev.Ref != "n1" {
		t.Errorf("event ref = %q", ev.Ref)
	}
}

// Subscribe is allowed while disconnected (the UI attaches whenever it likes),
// and such a subscriber must still receive events once the sidecar shows up.
func TestAdapterSubscribeBeforeSidecarAvailable(t *testing.T) {
	sc := newFakeSidecar(t, func(req Request) any { return []Node{} })
	defer sc.Close()

	client := NewWSClient(sc.URL(), testLogger())
	a := New(testLogger(), DefaultConfig(), client)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	sub, err := a.Subscribe(ctx)
	if err != nil {
		t.Fatalf("subscribe before start: %v", err)
	}
	if err := a.Start(ctx); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() { _ = a.Stop(context.Background()) }()

	sc.pushes <- Response{
		Event: EventAttributeChanged,
		Data:  json.RawMessage(`{"nodeId":"n2","endpointId":1,"cluster":"OnOff","attribute":"OnOff","value":false}`),
	}

	if ev := nextDeviceEvent(t, sub, 3*time.Second); ev.Ref != "n2" {
		t.Errorf("event ref = %q", ev.Ref)
	}
}

// multiEndpointNode is a light strip whose colour lives on a different
// endpoint than its on/off — the case defaultEndpoint always got wrong.
func multiEndpointNode() Node {
	return Node{
		NodeID:      "strip-1",
		VendorName:  "IKEA",
		ProductName: "WARMBLIXT",
		Online:      true,
		Endpoints: []Endpoint{
			{EndpointID: 0, DeviceType: "RootNode"},
			{EndpointID: 1, DeviceType: "ExtendedColorLight", Clusters: []string{ClusterOnOff, ClusterLevelControl}},
			{EndpointID: 4, Clusters: []string{ClusterColorControl}},
		},
	}
}

// End-to-end: the endpoint discovered for a feature must be the one that
// actually appears in the invokeCommand frame sent to the sidecar.
func TestAdapterRoutesCommandToFeatureEndpoint(t *testing.T) {
	invoked := make(chan InvokeParams, 4)
	sc := newFakeSidecar(t, func(req Request) any {
		switch req.Method {
		case MethodListNodes:
			return []Node{multiEndpointNode()}
		case MethodInvokeCommand:
			var p InvokeParams
			if err := json.Unmarshal(req.Params, &p); err != nil {
				return &RPCError{Code: -32602, Message: err.Error()}
			}
			invoked <- p
			return nil
		}
		return &RPCError{Code: -32601, Message: "unknown"}
	})
	defer sc.Close()

	a := New(testLogger(), DefaultConfig(), NewWSClient(sc.URL(), testLogger()))
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := a.Start(ctx); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() { _ = a.Stop(context.Background()) }()

	if _, err := a.Discover(ctx); err != nil {
		t.Fatalf("discover: %v", err)
	}

	if err := a.InvokeAction(ctx, "strip-1", domain.FeatureColorTemp, domain.ActionSet,
		map[string]any{"kelvin": 2700}); err != nil {
		t.Fatalf("invoke color_temp: %v", err)
	}
	if got := (<-invoked).EndpointID; got != 4 {
		t.Errorf("color_temp went to endpoint %d, want 4", got)
	}

	if err := a.InvokeAction(ctx, "strip-1", domain.FeatureOnOff, domain.ActionTurnOn, nil); err != nil {
		t.Fatalf("invoke onoff: %v", err)
	}
	if got := (<-invoked).EndpointID; got != 1 {
		t.Errorf("onoff went to endpoint %d, want 1", got)
	}
}

// A command can arrive before any Discover has run (device commissioned by
// another instance, UI acting during boot). The adapter must learn the layout
// on the miss rather than blindly addressing endpoint 1.
func TestAdapterLearnsRoutesOnMiss(t *testing.T) {
	invoked := make(chan InvokeParams, 1)
	sc := newFakeSidecar(t, func(req Request) any {
		switch req.Method {
		case MethodListNodes:
			return []Node{multiEndpointNode()}
		case MethodInvokeCommand:
			var p InvokeParams
			if err := json.Unmarshal(req.Params, &p); err != nil {
				return &RPCError{Code: -32602, Message: err.Error()}
			}
			invoked <- p
			return nil
		}
		return &RPCError{Code: -32601, Message: "unknown"}
	})
	defer sc.Close()

	a := New(testLogger(), DefaultConfig(), NewWSClient(sc.URL(), testLogger()))
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := a.Start(ctx); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() { _ = a.Stop(context.Background()) }()

	// No Discover call at all — straight to a command.
	if err := a.InvokeAction(ctx, "strip-1", domain.FeatureColorTemp, domain.ActionSet,
		map[string]any{"kelvin": 2700}); err != nil {
		t.Fatalf("invoke color_temp: %v", err)
	}
	if got := (<-invoked).EndpointID; got != 4 {
		t.Errorf("color_temp went to endpoint %d, want 4 — routes were not learned on miss", got)
	}
}

// Subscribers must be able to tell "nothing is happening" from "the sidecar is
// gone", both on subscribe and as connectivity changes.
func TestAdapterEmitsStatusEvents(t *testing.T) {
	sc := newFakeSidecar(t, func(req Request) any { return []Node{} })
	defer sc.Close()

	a := New(testLogger(), DefaultConfig(), NewWSClient(sc.URL(), testLogger()))
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Subscribing before Start must report the adapter as not usable yet.
	early, err := a.Subscribe(ctx)
	if err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	if ev := <-early; ev.Kind != ports.TransportEventAdapterStatus || ev.Value != false {
		t.Errorf("first event for an early subscriber = %+v, want adapter_status false", ev)
	}

	if err := a.Start(ctx); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() { _ = a.Stop(context.Background()) }()

	if ev := <-early; ev.Kind != ports.TransportEventAdapterStatus || ev.Value != true {
		t.Errorf("after connect = %+v, want adapter_status true", ev)
	}

	// A subscriber attaching while connected learns that immediately.
	late, err := a.Subscribe(ctx)
	if err != nil {
		t.Fatalf("subscribe late: %v", err)
	}
	if ev := <-late; ev.Kind != ports.TransportEventAdapterStatus || ev.Value != true {
		t.Errorf("first event for a late subscriber = %+v, want adapter_status true", ev)
	}

	sc.dropCurrent()

	select {
	case ev := <-early:
		if ev.Kind != ports.TransportEventAdapterStatus || ev.Value != false {
			t.Errorf("after drop = %+v, want adapter_status false", ev)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("no status event after the sidecar dropped")
	}
}

// The taxonomy has to survive the JSON round trip and the adapter's error
// wrapping, or callers are back to matching message text.
func TestAdapterPropagatesErrorKind(t *testing.T) {
	sc := newFakeSidecar(t, func(req Request) any {
		return &RPCError{
			Code:    RPCCodeNotFound,
			Message: "matter: node ghost not found",
			ErrKind: ErrKindNotFound,
		}
	})
	defer sc.Close()

	a := New(testLogger(), DefaultConfig(), NewWSClient(sc.URL(), testLogger()))
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := a.Start(ctx); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() { _ = a.Stop(context.Background()) }()

	_, err := a.ReadState(ctx, "ghost", domain.FeatureOnOff, domain.StateOnOff)
	if err == nil {
		t.Fatal("expected an error")
	}
	if !errors.Is(err, ErrNotFound) {
		t.Errorf("errors.Is(err, ErrNotFound) = false for %v", err)
	}
	if got := KindOf(err); got != ErrKindNotFound {
		t.Errorf("KindOf = %q want not_found", got)
	}
	if IsRetryable(err) {
		t.Error("a missing node is not worth retrying")
	}
}

func TestAcceptSeqDropsReplayedDuplicates(t *testing.T) {
	a := New(testLogger(), DefaultConfig(), nil)

	if !a.acceptSeq(0) {
		t.Error("seq 0 means an unsequenced sidecar — must pass through")
	}
	if !a.acceptSeq(5) {
		t.Error("first sequenced event rejected")
	}
	if a.acceptSeq(5) {
		t.Error("a replayed event with the same seq must be dropped")
	}
	if a.acceptSeq(3) {
		t.Error("an older replayed event must be dropped")
	}
	if !a.acceptSeq(6) {
		t.Error("a new event after replay must be accepted")
	}
	if got := a.lastSeq.Load(); got != 6 {
		t.Errorf("lastSeq = %d want 6", got)
	}
}

// After a reconnect the adapter must tell the sidecar where it left off, and
// must cope with the sidecar having restarted: its counter begins at 1 again,
// so a client that keeps its old high-water mark would treat every fresh event
// as a duplicate and go permanently silent.
func TestAdapterResubscribesAndClampsAfterSidecarRestart(t *testing.T) {
	subscribed := make(chan int64, 4)
	// What the fake claims its position is — flipped to simulate a restart.
	var mu sync.Mutex
	serverSeq := int64(5)
	// The second subscribe deliberately answers gap=false. A real sidecar
	// reports gap=true when a client is ahead of it, and that path resets the
	// counter too — answering false here isolates the clamp itself, so this
	// test fails if only the gap branch survives.
	gap := true

	sc := newFakeSidecar(t, func(req Request) any {
		switch req.Method {
		case MethodSubscribe:
			var p SubscribeParams
			_ = json.Unmarshal(req.Params, &p)
			subscribed <- p.SinceSeq
			mu.Lock()
			defer mu.Unlock()
			return SubscribeResult{Seq: serverSeq, Gap: gap}
		case MethodListNodes:
			return []Node{}
		}
		return &RPCError{Code: RPCCodeMethodMissing, Message: "unknown"}
	})
	defer sc.Close()

	a := New(testLogger(), DefaultConfig(), NewWSClient(sc.URL(), testLogger()))
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	if err := a.Start(ctx); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() { _ = a.Stop(context.Background()) }()

	sub, err := a.Subscribe(ctx)
	if err != nil {
		t.Fatalf("subscribe: %v", err)
	}

	if got := <-subscribed; got != 0 {
		t.Errorf("first subscribe sinceSeq = %d want 0 (nothing seen yet)", got)
	}

	// Live event at seq 9 — above the server position we were given.
	sc.pushes <- Response{
		Event: EventAttributeChanged,
		Seq:   9,
		Data:  json.RawMessage(`{"nodeId":"n1","endpointId":1,"cluster":"OnOff","attribute":"OnOff","value":true}`),
	}
	if ev := nextDeviceEvent(t, sub, 3*time.Second); ev.Ref != "n1" {
		t.Fatalf("event ref = %q", ev.Ref)
	}
	if got := a.lastSeq.Load(); got != 9 {
		t.Fatalf("lastSeq = %d want 9", got)
	}

	// The sidecar "restarts": its counter is back near the beginning.
	mu.Lock()
	serverSeq = 1
	gap = false
	mu.Unlock()
	sc.dropCurrent()

	select {
	case got := <-subscribed:
		if got != 9 {
			t.Errorf("resubscribe sinceSeq = %d want 9 (our high-water mark)", got)
		}
	case <-time.After(6 * time.Second):
		t.Fatal("adapter never resubscribed after reconnect")
	}

	// The clamp lands when the subscribe response comes back, just after the
	// request we observed above.
	deadline := time.Now().Add(3 * time.Second)
	for a.lastSeq.Load() != 1 {
		if time.Now().After(deadline) {
			t.Fatalf("lastSeq = %d want 1 — the counter was not clamped to the restarted sidecar",
				a.lastSeq.Load())
		}
		time.Sleep(20 * time.Millisecond)
	}

	// An event the restarted sidecar numbers 2 must be accepted, not mistaken
	// for a duplicate of the pre-restart stream.
	sc.pushes <- Response{
		Event: EventAttributeChanged,
		Seq:   2,
		Data:  json.RawMessage(`{"nodeId":"n1","endpointId":1,"cluster":"OnOff","attribute":"OnOff","value":false}`),
	}
	if ev := nextDeviceEvent(t, sub, 3*time.Second); ev.Value != false {
		t.Errorf("post-restart event = %+v, want the OnOff=false update", ev)
	}
}

// Commissioning progress must reach the caller's callback, so the UI can show
// real pairing stages instead of a spinner on a timer.
func TestAdapterForwardsCommissioningProgress(t *testing.T) {
	var sc *fakeSidecar
	sc = newFakeSidecar(t, func(req Request) any {
		switch req.Method {
		case MethodCommission:
			// Progress arrives while the call is in flight — that is the whole
			// point of it, and the adapter only holds the sink for that window.
			sc.pushes <- Response{Event: EventCommissioningStage, Seq: 1,
				Data: json.RawMessage(`{"stage":"paired","message":"secure session established"}`)}
			sc.pushes <- Response{Event: EventCommissioningStage, Seq: 2,
				Data: json.RawMessage(`{"stage":"attesting","message":"verifying device certificate"}`)}
			time.Sleep(200 * time.Millisecond)
			return CommissionResult{NodeID: "n7", FabricIndex: 1}
		case MethodListNodes:
			return []Node{{NodeID: "n7", Online: true,
				Endpoints: []Endpoint{{EndpointID: 1, Clusters: []string{ClusterOnOff}}}}}
		}
		return &RPCError{Code: RPCCodeMethodMissing, Message: "unknown"}
	})
	defer sc.Close()

	a := New(testLogger(), DefaultConfig(), NewWSClient(sc.URL(), testLogger()))
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := a.Start(ctx); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() { _ = a.Stop(context.Background()) }()

	got := make(chan string, 8)
	ref, err := a.Commission(ctx, ports.CommissionRequest{
		Payload:  "34970112332",
		Progress: func(stage, message string) { got <- stage },
	})
	if err != nil {
		t.Fatalf("commission: %v", err)
	}
	if ref != "n7" {
		t.Errorf("ref = %q want n7", ref)
	}

	for _, want := range []string{"paired", "attesting"} {
		select {
		case stage := <-got:
			if stage != want {
				t.Errorf("stage = %q want %q", stage, want)
			}
		case <-time.After(3 * time.Second):
			t.Fatalf("progress stage %q never arrived", want)
		}
	}
}

// Progress events that arrive when nobody is commissioning must be dropped
// quietly rather than reaching a stale callback or crashing the pump.
func TestAdapterIgnoresProgressWithoutCommission(t *testing.T) {
	sc := newFakeSidecar(t, func(req Request) any { return []Node{} })
	defer sc.Close()

	a := New(testLogger(), DefaultConfig(), NewWSClient(sc.URL(), testLogger()))
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	if err := a.Start(ctx); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() { _ = a.Stop(context.Background()) }()

	sub, err := a.Subscribe(ctx)
	if err != nil {
		t.Fatalf("subscribe: %v", err)
	}

	sc.pushes <- Response{Event: EventCommissioningStage, Seq: 1,
		Data: json.RawMessage(`{"stage":"paired"}`)}
	sc.pushes <- Response{Event: EventAttributeChanged, Seq: 2,
		Data: json.RawMessage(`{"nodeId":"n1","endpointId":1,"cluster":"OnOff","attribute":"OnOff","value":true}`)}

	// The device event still gets through — the progress event was swallowed,
	// not the whole pump.
	if ev := nextDeviceEvent(t, sub, 3*time.Second); ev.Ref != "n1" {
		t.Errorf("event ref = %q", ev.Ref)
	}
}

// A scan must stream advertisements as they arrive, close when the scan ends,
// and refuse to run two at once.
func TestAdapterDiscoverCommissionable(t *testing.T) {
	var sc *fakeSidecar
	sc = newFakeSidecar(t, func(req Request) any {
		if req.Method != MethodDiscoverCommissionable {
			return &RPCError{Code: RPCCodeMethodMissing, Message: "unknown"}
		}
		// The sidecar pushes finds while the scan runs, then answers with the
		// accumulated list.
		sc.pushes <- Response{Event: EventCommissionableFound, Seq: 1,
			Data: json.RawMessage(`{"ref":"dev-a","name":"WARMBLIXT","deviceType":"ExtendedColorLight","discriminator":3840}`)}
		sc.pushes <- Response{Event: EventCommissionableFound, Seq: 2,
			Data: json.RawMessage(`{"ref":"dev-b","deviceType":"OnOffPlugInUnit","discriminator":1234}`)}
		time.Sleep(200 * time.Millisecond)
		return []CommissionableDevice{{Ref: "dev-a"}, {Ref: "dev-b"}}
	})
	defer sc.Close()

	a := New(testLogger(), DefaultConfig(), NewWSClient(sc.URL(), testLogger()))
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := a.Start(ctx); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() { _ = a.Stop(context.Background()) }()

	found, err := a.DiscoverCommissionable(ctx, time.Second)
	if err != nil {
		t.Fatalf("discover: %v", err)
	}

	// A second scan while the first runs is refused rather than interleaved.
	if _, err := a.DiscoverCommissionable(ctx, time.Second); err == nil {
		t.Error("expected the second concurrent scan to be refused")
	}

	var got []ports.CommissionableDevice
	for d := range found {
		got = append(got, d)
	}
	if len(got) != 2 {
		t.Fatalf("found %d devices, want 2: %+v", len(got), got)
	}
	if got[0].Name != "WARMBLIXT" || got[0].Type != domain.DeviceTypeLight {
		t.Errorf("first device = %+v, want the light with its advertised name", got[0])
	}
	if got[1].Type != domain.DeviceTypePlug {
		t.Errorf("second device type = %q want plug", got[1].Type)
	}
	// Nameless advertisements still need something a user can match against
	// the label on the device.
	if got[1].Name != "Matter device 1234" {
		t.Errorf("fallback name = %q, want one carrying the discriminator", got[1].Name)
	}

	// The channel closed, so the scan slot must be free again.
	if _, err := a.DiscoverCommissionable(ctx, time.Second); err != nil {
		t.Errorf("scan slot not released: %v", err)
	}
}

// Picking a device from a scan must target that exact device, while still
// carrying the setup code — discovery never supplies the passcode.
func TestAdapterCommissionPassesTarget(t *testing.T) {
	params := make(chan CommissionParams, 1)
	sc := newFakeSidecar(t, func(req Request) any {
		switch req.Method {
		case MethodCommission:
			var p CommissionParams
			_ = json.Unmarshal(req.Params, &p)
			params <- p
			return CommissionResult{NodeID: "n9"}
		case MethodListNodes:
			return []Node{}
		}
		return &RPCError{Code: RPCCodeMethodMissing, Message: "unknown"}
	})
	defer sc.Close()

	a := New(testLogger(), DefaultConfig(), NewWSClient(sc.URL(), testLogger()))
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := a.Start(ctx); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() { _ = a.Stop(context.Background()) }()

	if _, err := a.Commission(ctx, ports.CommissionRequest{
		Payload: "34970112332",
		Extra:   map[string]string{"matter.target": "dev-a"},
	}); err != nil {
		t.Fatalf("commission: %v", err)
	}

	got := <-params
	if got.Target != "dev-a" {
		t.Errorf("target = %q want dev-a", got.Target)
	}
	if got.SetupCode != "34970112332" {
		t.Errorf("setup code = %q — targeting must not drop the code", got.SetupCode)
	}
}

// Яркость и цвет в Matter — read-only атрибуты (`R V`), менять их можно только
// командами. Запись атрибута на живой лампе тихо ничего не делает: она
// включалась, но не диммировалась. Тест смотрит на фактический кадр, а не на
// то, что вызов вернул nil.
func TestWriteStateUsesCommandsForReadOnlyAttributes(t *testing.T) {
	frames := make(chan Request, 8)
	sc := newFakeSidecar(t, func(req Request) any {
		switch req.Method {
		case MethodListNodes:
			return []Node{{NodeID: "lamp", Online: true, Endpoints: []Endpoint{
				{EndpointID: 1, DeviceType: "ExtendedColorLight", Clusters: []string{
					ClusterOnOff, ClusterLevelControl, ClusterColorControl,
				}},
			}}}
		case MethodInvokeCommand, MethodWriteAttribute:
			frames <- req
			return nil
		}
		return &RPCError{Code: RPCCodeMethodMissing, Message: "unknown"}
	})
	defer sc.Close()

	a := New(testLogger(), DefaultConfig(), NewWSClient(sc.URL(), testLogger()))
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := a.Start(ctx); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() { _ = a.Stop(context.Background()) }()

	cases := []struct {
		name    string
		feature domain.FeatureKey
		key     domain.StateKey
		value   any
		cluster string
		command string
	}{
		{"яркость", domain.FeatureBrightness, domain.StateLevel, 30, ClusterLevelControl, CmdMoveToLevel},
		{"температура", domain.FeatureColorTemp, domain.StateColorTempK, 2700, ClusterColorControl, CmdMoveToColorTempMireds},
		{"оттенок", domain.FeatureColor, domain.StateColorHue, 120, ClusterColorControl, CmdMoveToHue},
		{"насыщенность", domain.FeatureColor, domain.StateColorSat, 80, ClusterColorControl, CmdMoveToSaturation},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if err := a.WriteState(ctx, "lamp", tc.feature, tc.key, tc.value); err != nil {
				t.Fatalf("WriteState: %v", err)
			}
			req := <-frames
			if req.Method != MethodInvokeCommand {
				t.Fatalf("ушёл %s вместо команды — устройство это проигнорирует", req.Method)
			}
			var inv InvokeParams
			if err := json.Unmarshal(req.Params, &inv); err != nil {
				t.Fatalf("unmarshal: %v", err)
			}
			if inv.Cluster != tc.cluster || inv.Command != tc.command {
				t.Errorf("got %s.%s want %s.%s", inv.Cluster, inv.Command, tc.cluster, tc.command)
			}
			if len(inv.Args) == 0 {
				t.Error("команда без аргументов — значение потерялось")
			}
		})
	}

	// Атрибуты, которые Matter разрешает писать, должны так и писаться.
	if err := a.WriteState(ctx, "lamp", domain.FeatureFan, domain.StateFanPercent, 50); err == nil {
		req := <-frames
		if req.Method != MethodWriteAttribute {
			t.Errorf("записываемый атрибут ушёл как %s", req.Method)
		}
	}
}

// Удаление обязано снять наш fabric с самого устройства. Раньше вызывался
// force-delete: keystone забывал устройство, а лампа продолжала числить нас
// среди подключённых сервисов в Apple Home — убрать это можно было только
// сбросом к заводским.
func TestDecommissionReportsForcedRemoval(t *testing.T) {
	cases := []struct {
		name       string
		result     RemoveNodeResult
		wantForced bool
	}{
		{"устройство ответило", RemoveNodeResult{Removed: "decommissioned"}, false},
		{"устройство недоступно", RemoveNodeResult{Removed: "forced", Message: "no response"}, true},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			sc := newFakeSidecar(t, func(req Request) any {
				if req.Method == MethodRemoveNode {
					return tc.result
				}
				return []Node{}
			})
			defer sc.Close()

			a := New(testLogger(), DefaultConfig(), NewWSClient(sc.URL(), testLogger()))
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			if err := a.Start(ctx); err != nil {
				t.Fatalf("start: %v", err)
			}
			defer func() { _ = a.Stop(context.Background()) }()

			err := a.Decommission(ctx, "lamp")
			if tc.wantForced {
				if !errors.Is(err, ErrForcedRemoval) {
					t.Errorf("err = %v, а пользователь должен узнать, что устройство ещё держит наш fabric", err)
				}
				return
			}
			if err != nil {
				t.Errorf("успешный decommission не должен возвращать ошибку: %v", err)
			}
		})
	}
}
