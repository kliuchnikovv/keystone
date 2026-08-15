package matter

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/coder/websocket"
)

// fakeSidecar is a tiny in-process WebSocket server that mimics the
// matter.js sidecar's JSON-RPC contract: it echoes requests through a
// user-supplied handler and can push server events on demand.
type fakeSidecar struct {
	t       *testing.T
	handler func(req Request) any // returns result | *RPCError
	server  *httptest.Server
	pushes  chan any // events to broadcast to the current live conn

	// Live connection, so tests can yank it out from under the client the way
	// a sidecar restart would.
	connMu  sync.Mutex
	current *websocket.Conn
	accepts int
	stall   bool // accept, then never read — see serve()
}

// dropCurrent kills the live connection without a close handshake — the
// closest in-process approximation of the sidecar process dying.
func (f *fakeSidecar) dropCurrent() {
	f.connMu.Lock()
	conn := f.current
	f.connMu.Unlock()
	if conn != nil {
		conn.CloseNow()
	}
}

// acceptCount reports how many WebSocket connections the fake has served.
func (f *fakeSidecar) acceptCount() int {
	f.connMu.Lock()
	defer f.connMu.Unlock()
	return f.accepts
}

func newFakeSidecar(t *testing.T, handler func(req Request) any) *fakeSidecar {
	t.Helper()
	f := &fakeSidecar{
		t:       t,
		handler: handler,
		pushes:  make(chan any, 8),
	}
	f.server = httptest.NewServer(http.HandlerFunc(f.serve))
	return f
}

func (f *fakeSidecar) URL() string {
	return "ws" + strings.TrimPrefix(f.server.URL, "http")
}

func (f *fakeSidecar) Close() { f.server.Close() }

func (f *fakeSidecar) serve(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, nil)
	if err != nil {
		f.t.Logf("fake sidecar accept: %v", err)
		return
	}
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()
	defer conn.Close(websocket.StatusNormalClosure, "test done")

	f.connMu.Lock()
	f.current = conn
	f.accepts++
	stall := f.stall
	f.connMu.Unlock()

	// Half-open simulation: hold the socket open but never read from it, so
	// the library never auto-answers a ping. This is what a NAT timeout or a
	// wedged sidecar looks like from the client side — bytes go nowhere and
	// nothing ever errors.
	if stall {
		<-ctx.Done()
		return
	}

	// One goroutine to broadcast pushes for the lifetime of this conn.
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			case ev := <-f.pushes:
				raw, _ := json.Marshal(ev)
				_ = conn.Write(ctx, websocket.MessageText, raw)
			}
		}
	}()

	for {
		_, buf, err := conn.Read(ctx)
		if err != nil {
			return
		}
		var req Request
		if err := json.Unmarshal(buf, &req); err != nil {
			continue
		}
		res := f.handler(req)
		var out any
		switch v := res.(type) {
		case *RPCError:
			out = Response{ID: req.ID, Error: v}
		default:
			raw, err := json.Marshal(v)
			if err != nil {
				out = Response{ID: req.ID, Error: &RPCError{Code: -32603, Message: err.Error()}}
			} else {
				out = Response{ID: req.ID, Result: raw}
			}
		}
		raw, _ := json.Marshal(out)
		_ = conn.Write(ctx, websocket.MessageText, raw)
	}
}

func TestWSClientCall(t *testing.T) {
	sc := newFakeSidecar(t, func(req Request) any {
		if req.Method == MethodListNodes {
			return []Node{{NodeID: "n1", Online: true}}
		}
		return &RPCError{Code: -32601, Message: "unknown"}
	})
	defer sc.Close()

	c := NewWSClient(sc.URL(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := c.Connect(ctx); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer func() { _ = c.Close() }()

	var nodes []Node
	if err := c.Call(ctx, MethodListNodes, nil, &nodes); err != nil {
		t.Fatalf("listNodes: %v", err)
	}
	if len(nodes) != 1 || nodes[0].NodeID != "n1" {
		t.Errorf("unexpected result: %+v", nodes)
	}

	if err := c.Call(ctx, "nope", nil, nil); err == nil {
		t.Error("expected RPC error for unknown method")
	}
}

func TestWSClientEventFanOut(t *testing.T) {
	sc := newFakeSidecar(t, func(req Request) any { return nil })
	defer sc.Close()

	c := NewWSClient(sc.URL(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := c.Connect(ctx); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer func() { _ = c.Close() }()

	// Push a server event. The fake broadcasts whatever is queued.
	sc.pushes <- Response{
		Event: EventAttributeChanged,
		Data:  json.RawMessage(`{"nodeId":"n1","endpointId":1,"cluster":"OnOff","attribute":"OnOff","value":true}`),
	}

	select {
	case ev := <-c.Events():
		if ev.Name != EventAttributeChanged {
			t.Errorf("event name = %s", ev.Name)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("no event received")
	}
}

// A dropped connection must be observable (Disconnected fires) and the same
// client must be redialable — the old code left c.conn set after the read pump
// died, so every later Connect was a silent no-op and the client stayed dead.
func TestWSClientReconnectAfterDrop(t *testing.T) {
	sc := newFakeSidecar(t, func(req Request) any { return []Node{{NodeID: "n1", Online: true}} })
	defer sc.Close()

	c := NewWSClient(sc.URL(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := c.Connect(ctx); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer func() { _ = c.Close() }()

	dropped := c.Disconnected()
	select {
	case <-dropped:
		t.Fatal("Disconnected fired while the connection was healthy")
	default:
	}

	sc.dropCurrent()

	select {
	case <-dropped:
	case <-time.After(3 * time.Second):
		t.Fatal("Disconnected never fired after the sidecar dropped the connection")
	}

	// In-flight semantics: calls fail fast instead of hanging.
	if err := c.Call(ctx, MethodListNodes, nil, nil); err == nil {
		t.Error("expected error on a call issued while disconnected")
	}

	if err := c.Connect(ctx); err != nil {
		t.Fatalf("reconnect: %v", err)
	}
	var nodes []Node
	if err := c.Call(ctx, MethodListNodes, nil, &nodes); err != nil {
		t.Fatalf("listNodes after reconnect: %v", err)
	}
	if len(nodes) != 1 {
		t.Errorf("nodes after reconnect = %+v", nodes)
	}
	if got := sc.acceptCount(); got != 2 {
		t.Errorf("sidecar accepted %d connections, want 2", got)
	}
}

// The heartbeat must tear the connection down when pongs stop coming, so a
// half-open TCP session surfaces as a reconnect instead of eternal silence.
func TestWSClientHeartbeatDropsDeadConnection(t *testing.T) {
	sc := newFakeSidecar(t, func(req Request) any { return nil })
	sc.stall = true // accepts, holds the socket, answers nothing
	defer sc.Close()

	c := NewWSClient(sc.URL(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	c.pingInterval = 50 * time.Millisecond
	c.pingTimeout = 200 * time.Millisecond

	if err := c.Connect(context.Background()); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer func() { _ = c.Close() }()

	dropped := c.Disconnected()
	started := time.Now()

	select {
	case <-dropped:
	case <-time.After(3 * time.Second):
		t.Fatal("heartbeat did not tear down the half-open connection")
	}

	// Nothing errored on this socket — only an unanswered ping could have
	// brought it down, so the elapsed time must cover the ping timeout.
	if elapsed := time.Since(started); elapsed < c.pingTimeout {
		t.Errorf("connection dropped after %v, before the %v ping timeout could expire — "+
			"the drop came from something other than the heartbeat", elapsed, c.pingTimeout)
	}
}

func TestWSClientCallCancelled(t *testing.T) {
	// Sidecar that never replies.
	sc := newFakeSidecar(t, func(req Request) any { return nil })
	// Override the handler to just black-hole requests.
	sc.handler = func(req Request) any { time.Sleep(5 * time.Second); return nil }
	defer sc.Close()

	c := NewWSClient(sc.URL(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err := c.Connect(context.Background()); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer func() { _ = c.Close() }()

	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	err := c.Call(ctx, MethodListNodes, nil, nil)
	if err == nil {
		t.Fatal("expected timeout error")
	}
}
