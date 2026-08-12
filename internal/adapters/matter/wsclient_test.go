package matter

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
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
