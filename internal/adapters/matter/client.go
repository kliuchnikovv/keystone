package matter

import (
	"context"
	"encoding/json"
	"errors"
)

// Client is the transport-independent handle the Adapter uses to talk to the
// matter.js sidecar. Splitting the WebSocket transport out behind this
// interface keeps the Adapter unit-testable (mock client) and lets us pick
// or replace the concrete WebSocket library without touching adapter code.
//
// Concrete implementation lives in wsclient.go (added when the sidecar wire
// contract is finalised — see docs/matter-adapter-brief.md §4).
type Client interface {
	// Connect establishes the WebSocket session with the sidecar. Blocks until
	// the connection is ready or ctx is done.
	Connect(ctx context.Context) error

	// Close terminates the WebSocket session. Safe to call multiple times.
	Close() error

	// Call performs one JSON-RPC round trip. result must be a pointer into
	// which the server's `result` field will be json.Unmarshaled; pass nil if
	// the caller doesn't need it.
	Call(ctx context.Context, method string, params any, result any) error

	// Events returns a channel of server-pushed events. The channel lives for
	// the lifetime of the Client, not of one connection: it stays open across
	// reconnects and is never closed, so consumers must select on their own
	// context rather than ranging over it. (Closing it would race the read
	// pump, which may still be delivering a frame when Close lands.)
	Events() <-chan Event

	// Disconnected returns a channel closed when the current connection drops
	// for any reason — remote close, read error, or a failed heartbeat. Callers
	// use it to trigger a reconnect. When no connection is up the returned
	// channel is already closed, so a caller that races Connect sees the
	// "reconnect needed" state rather than blocking forever.
	//
	// Each successful Connect installs a fresh channel; re-read it after
	// reconnecting.
	Disconnected() <-chan struct{}
}

// Event is one decoded server-pushed message. Seq is the sidecar's sequence
// number for it, or 0 if the sidecar doesn't sequence events.
type Event struct {
	Name string
	Data json.RawMessage
	Seq  int64
}

// ErrNotConnected is returned by Client methods when the underlying transport
// is not up. Adapters translate this into their own status reporting.
var ErrNotConnected = errors.New("matter: sidecar not connected")
