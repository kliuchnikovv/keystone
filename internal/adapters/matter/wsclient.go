package matter

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
)

// WSClient is the production Client — a WebSocket JSON-RPC client for the
// matter.js sidecar. It multiplexes concurrent Call() invocations over the
// single duplex connection by assigning each request a unique id and routing
// responses through per-call channels.
//
// Server-pushed events (attributeChanged, nodeOnline, etc.) are delivered on
// a single buffered channel exposed via Events().
type WSClient struct {
	url string
	log *slog.Logger

	// dial timeout for Connect.
	dialTimeout time.Duration

	// Underlying connection. Set on Connect, cleared on Close.
	connMu sync.RWMutex
	conn   *websocket.Conn

	// Write serialisation. websocket.Conn is not safe for concurrent Writes,
	// so every outbound frame goes through this mutex.
	writeMu sync.Mutex

	// Pending Call() invocations, keyed by request id -> response channel.
	pendingMu sync.Mutex
	pending   map[string]chan *Response

	// Monotonic id source for Call().
	nextID atomic.Uint64

	// events is the queue of server-pushed events. Buffered so a slow
	// consumer does not block the read loop indefinitely; overflow is
	// logged and dropped.
	events chan Event

	// Cancel signal for the read pump; closed by Close.
	closeCh chan struct{}

	// closed is set by Close so subsequent Call() invocations short-circuit
	// with ErrNotConnected instead of blocking on a dead connection.
	closed atomic.Bool
}

// NewWSClient builds a WS client for the given ws:// URL. Nothing is dialled
// until Connect is called.
func NewWSClient(url string, log *slog.Logger) *WSClient {
	if log == nil {
		log = slog.Default()
	}
	return &WSClient{
		url:         url,
		log:         log,
		dialTimeout: 5 * time.Second,
		pending:     make(map[string]chan *Response),
		events:      make(chan Event, 128),
		closeCh:     make(chan struct{}),
	}
}

// Connect implements Client. It dials the sidecar and starts the read pump.
// Safe to call once; a second call while already connected is a no-op.
func (c *WSClient) Connect(ctx context.Context) error {
	c.connMu.Lock()
	defer c.connMu.Unlock()
	if c.conn != nil {
		return nil
	}
	if c.closed.Load() {
		return errors.New("matter ws client: already closed")
	}

	dialCtx, cancel := context.WithTimeout(ctx, c.dialTimeout)
	defer cancel()

	conn, _, err := websocket.Dial(dialCtx, c.url, nil)
	if err != nil {
		return fmt.Errorf("dial %s: %w", c.url, err)
	}
	// Disable the 32 KiB default read cap — matter.js listNodes responses can
	// easily grow past that once a fabric has several peers.
	conn.SetReadLimit(1 << 20) // 1 MiB
	c.conn = conn

	go c.readPump(conn)
	c.log.Info("matter ws client connected", "url", c.url)
	return nil
}

// Close implements Client. Idempotent — safe to call from any goroutine.
func (c *WSClient) Close() error {
	if !c.closed.CompareAndSwap(false, true) {
		return nil
	}
	c.connMu.Lock()
	conn := c.conn
	c.conn = nil
	c.connMu.Unlock()

	close(c.closeCh)
	if conn != nil {
		_ = conn.Close(websocket.StatusNormalClosure, "client shutting down")
	}

	// Fail every in-flight Call() so callers do not block forever.
	c.pendingMu.Lock()
	for id, ch := range c.pending {
		close(ch)
		delete(c.pending, id)
	}
	c.pendingMu.Unlock()

	// Drain the events channel so a slow subscriber does not keep it open;
	// close it so consumers can range.
	close(c.events)
	return nil
}

// Call implements Client. Sends a JSON-RPC request and waits for the matching
// response. Blocks until the reply arrives, ctx is done, or the connection
// dies. result may be nil if the caller does not care about the payload.
func (c *WSClient) Call(ctx context.Context, method string, params any, result any) error {
	if c.closed.Load() {
		return ErrNotConnected
	}
	c.connMu.RLock()
	conn := c.conn
	c.connMu.RUnlock()
	if conn == nil {
		return ErrNotConnected
	}

	id := strconv.FormatUint(c.nextID.Add(1), 10)
	replyCh := make(chan *Response, 1)

	c.pendingMu.Lock()
	c.pending[id] = replyCh
	c.pendingMu.Unlock()

	// Ensure the pending entry is torn down on every exit path.
	defer func() {
		c.pendingMu.Lock()
		delete(c.pending, id)
		c.pendingMu.Unlock()
	}()

	frame := Request{ID: id, Method: method}
	if params != nil {
		raw, err := json.Marshal(params)
		if err != nil {
			return fmt.Errorf("marshal params: %w", err)
		}
		frame.Params = raw
	}
	if err := c.writeJSON(ctx, conn, frame); err != nil {
		return fmt.Errorf("write %s: %w", method, err)
	}

	select {
	case <-ctx.Done():
		return ctx.Err()
	case resp, ok := <-replyCh:
		if !ok {
			return ErrNotConnected
		}
		if resp.Error != nil {
			return resp.Error
		}
		if result == nil || len(resp.Result) == 0 {
			return nil
		}
		if err := json.Unmarshal(resp.Result, result); err != nil {
			return fmt.Errorf("unmarshal %s result: %w", method, err)
		}
		return nil
	}
}

// Events implements Client.
func (c *WSClient) Events() <-chan Event { return c.events }

// --- internals ---

func (c *WSClient) writeJSON(ctx context.Context, conn *websocket.Conn, v any) error {
	payload, err := json.Marshal(v)
	if err != nil {
		return err
	}
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	return conn.Write(ctx, websocket.MessageText, payload)
}

// readPump drains frames from the sidecar and routes each one to either a
// pending Call() waiter or the events channel. Exits when the connection
// closes for any reason.
func (c *WSClient) readPump(conn *websocket.Conn) {
	defer func() {
		// Wake up any Call() still waiting on a channel — no more replies
		// are coming.
		c.pendingMu.Lock()
		for id, ch := range c.pending {
			close(ch)
			delete(c.pending, id)
		}
		c.pendingMu.Unlock()
		c.log.Warn("matter ws read pump exited")
	}()

	for {
		select {
		case <-c.closeCh:
			return
		default:
		}
		_, payload, err := conn.Read(context.Background())
		if err != nil {
			if !c.closed.Load() {
				c.log.Warn("matter ws read failed", "err", err)
			}
			return
		}
		c.dispatch(payload)
	}
}

// dispatch parses one frame and forwards it to a Call() waiter or the events
// channel. Malformed frames are logged and dropped.
func (c *WSClient) dispatch(payload []byte) {
	var resp Response
	if err := json.Unmarshal(payload, &resp); err != nil {
		c.log.Warn("matter ws: bad frame", "err", err)
		return
	}

	// Server-pushed event: no id, has event field.
	if resp.ID == "" && resp.Event != "" {
		select {
		case c.events <- Event{Name: resp.Event, Data: resp.Data}:
		default:
			c.log.Warn("matter ws: event channel full, dropping", "event", resp.Event)
		}
		return
	}

	// Response to a Call().
	c.pendingMu.Lock()
	ch, ok := c.pending[resp.ID]
	c.pendingMu.Unlock()
	if !ok {
		c.log.Warn("matter ws: no waiter for id", "id", resp.ID)
		return
	}
	select {
	case ch <- &resp:
	default:
		// Buffer is 1 and we own the sender side; this arm is only hit if the
		// sidecar sends a duplicate reply for the same id.
		c.log.Warn("matter ws: duplicate reply", "id", resp.ID)
	}
}
