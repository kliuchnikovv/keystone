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

	// Heartbeat. WS sits on TCP, and a half-open TCP connection (router
	// dropped the session, cable pulled) is invisible to a blocked Read —
	// without a ping we only notice at the next Call() timeout, which for a
	// push-only stream may be never.
	pingInterval time.Duration
	pingTimeout  time.Duration

	// Underlying connection. Set on Connect, cleared when the read pump exits
	// (drop) or by Close (shutdown). conn == nil means "reconnectable".
	connMu sync.RWMutex
	conn   *websocket.Conn
	// connCancel tears down the per-connection goroutines (read pump,
	// heartbeat). Replaced on every Connect.
	connCancel context.CancelFunc
	// disconnected is closed once when the current connection drops. Replaced
	// on every Connect; nil before the first one.
	disconnected chan struct{}

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
		url:          url,
		log:          log,
		dialTimeout:  5 * time.Second,
		pingInterval: 10 * time.Second,
		pingTimeout:  30 * time.Second,
		pending:      make(map[string]chan *Response),
		events:       make(chan Event, 128),
	}
}

// Connect implements Client. It dials the sidecar and starts the read pump
// plus the heartbeat. A call while already connected is a no-op; a call after
// the connection dropped dials afresh, so the same WSClient survives an
// arbitrary number of sidecar restarts. Only Close is terminal.
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

	// Per-connection scope: cancelled when this connection dies, so the read
	// pump and heartbeat never outlive it (and Read is cancellable, which it
	// wasn't while the pump used context.Background()).
	connCtx, connCancel := context.WithCancel(context.Background())
	done := make(chan struct{})

	c.conn = conn
	c.connCancel = connCancel
	c.disconnected = done

	go c.readPump(connCtx, conn, done)
	go c.heartbeat(connCtx, conn)
	c.log.Info("matter ws client connected", "url", c.url)
	return nil
}

// Disconnected implements Client.
func (c *WSClient) Disconnected() <-chan struct{} {
	c.connMu.RLock()
	defer c.connMu.RUnlock()
	if c.disconnected == nil || c.conn == nil {
		return closedChan
	}
	return c.disconnected
}

// closedChan is handed out by Disconnected when nothing is connected — an
// already-closed channel reads as "you need to reconnect", which is exactly
// the state.
var closedChan = func() chan struct{} {
	ch := make(chan struct{})
	close(ch)
	return ch
}()

// dropConnection tears down the current connection's state and signals
// Disconnected waiters. Idempotent per connection: only the goroutine that
// still sees `conn` installed performs the teardown, so a read error racing a
// heartbeat failure closes the channel once.
func (c *WSClient) dropConnection(conn *websocket.Conn, done chan struct{}) {
	c.connMu.Lock()
	owned := c.conn == conn
	if owned {
		c.conn = nil
		c.connCancel = nil
		c.disconnected = nil
	}
	c.connMu.Unlock()

	// CloseNow, not Close: the graceful path waits for the peer's close frame,
	// and a peer that stopped reading (half-open socket, wedged sidecar) never
	// sends one — the wait would stall the teardown for seconds.
	_ = conn.CloseNow()

	// Wake every in-flight Call() — no more replies are coming on this socket.
	c.pendingMu.Lock()
	for id, ch := range c.pending {
		close(ch)
		delete(c.pending, id)
	}
	c.pendingMu.Unlock()

	// Exactly one caller owns the teardown, so this close can't double-fire.
	if owned {
		close(done)
	}
}

// Close implements Client. Idempotent — safe to call from any goroutine.
func (c *WSClient) Close() error {
	if !c.closed.CompareAndSwap(false, true) {
		return nil
	}
	c.connMu.Lock()
	conn := c.conn
	cancel := c.connCancel
	done := c.disconnected
	c.conn = nil
	c.connCancel = nil
	c.disconnected = nil
	c.connMu.Unlock()

	if cancel != nil {
		cancel()
	}
	if conn != nil {
		_ = conn.Close(websocket.StatusNormalClosure, "client shutting down")
	}
	if done != nil {
		close(done)
	}

	// Fail every in-flight Call() so callers do not block forever.
	c.pendingMu.Lock()
	for id, ch := range c.pending {
		close(ch)
		delete(c.pending, id)
	}
	c.pendingMu.Unlock()

	// c.events is deliberately left open: the read pump may still be inside
	// dispatch, and closing underneath it would panic. Consumers exit on their
	// own context (see Client.Events docs), and the channel is garbage once
	// the client is dropped.
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
// closes for any reason, signalling Disconnected waiters on the way out so the
// adapter can start reconnecting.
func (c *WSClient) readPump(ctx context.Context, conn *websocket.Conn, done chan struct{}) {
	defer c.dropConnection(conn, done)

	for {
		_, payload, err := conn.Read(ctx)
		if err != nil {
			if !c.closed.Load() && ctx.Err() == nil {
				c.log.Warn("matter ws read failed — connection dropped", "err", err)
			}
			return
		}
		c.dispatch(payload)
	}
}

// heartbeat pings the sidecar on an interval and kills the connection if a
// pong doesn't come back in time. Without it a half-open TCP session (NAT
// timeout, router reboot, unplugged cable) leaves Read blocked forever: the
// sidecar looks connected, no events arrive, and nothing ever retries.
func (c *WSClient) heartbeat(ctx context.Context, conn *websocket.Conn) {
	ticker := time.NewTicker(c.pingInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}

		pingCtx, cancel := context.WithTimeout(ctx, c.pingTimeout)
		err := conn.Ping(pingCtx)
		cancel()
		if err == nil {
			continue
		}
		if ctx.Err() != nil || c.closed.Load() {
			return
		}
		c.log.Warn("matter ws heartbeat failed — forcing reconnect",
			"err", err, "timeout", c.pingTimeout.String())
		// CloseNow rather than a graceful close: the peer is by definition not
		// answering, so a close handshake would block too. This wakes the
		// blocked Read and readPump runs the teardown.
		_ = conn.CloseNow()
		return
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
		case c.events <- Event{Name: resp.Event, Data: resp.Data, Seq: resp.Seq}:
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
