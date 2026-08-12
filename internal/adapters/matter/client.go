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

	// Events returns a channel of server-pushed events. The channel is closed
	// when Close is called or the underlying connection dies terminally. A
	// re-Connect starts a fresh channel — callers must re-subscribe.
	Events() <-chan Event
}

// Event is one decoded server-pushed message.
type Event struct {
	Name string
	Data json.RawMessage
}

// ErrNotConnected is returned by Client methods when the underlying transport
// is not up. Adapters translate this into their own status reporting.
var ErrNotConnected = errors.New("matter: sidecar not connected")
