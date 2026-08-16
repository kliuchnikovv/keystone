package dirigera

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/coder/websocket"
)

// Event is one hub-pushed message. The hub sends a small variety
// (deviceStateChanged, deviceAdded, deviceRemoved); anything else is
// captured raw so a caller can log for follow-up mapping.
type Event struct {
	Type string          `json:"type"`
	Data json.RawMessage `json:"data,omitempty"`
}

// DeviceStateChanged is the payload of a deviceStateChanged event.
// The hub reports one device id at a time with the attributes that
// actually changed, not the full device row.
type DeviceStateChanged struct {
	ID         string         `json:"id"`
	Type       string         `json:"type,omitempty"`
	DeviceType string         `json:"deviceType,omitempty"`
	Attributes map[string]any `json:"attributes,omitempty"`
}

// WatchEvents opens a WebSocket to the hub and streams every event it
// pushes. The returned channel is closed when ctx is cancelled or a
// reconnect attempt gives up.
//
// The WS URL is derived from the REST base by swapping the scheme
// (https → wss) and appending /v1 — that is what the hub actually
// serves. Auth is a Bearer header on the upgrade handshake.
func (c *Client) WatchEvents(ctx context.Context, log *slog.Logger) (<-chan Event, error) {
	wsURL, err := deriveWSURL(c.BaseURL)
	if err != nil {
		return nil, err
	}
	out := make(chan Event, 32)
	go c.watchLoop(ctx, wsURL, out, log)
	return out, nil
}

func (c *Client) watchLoop(ctx context.Context, wsURL string, out chan<- Event, log *slog.Logger) {
	defer close(out)
	// Reconnect with a modest backoff — the hub is on the LAN, a
	// dropped connection usually recovers in a second or two.
	backoff := time.Second
	const maxBackoff = 30 * time.Second

	for {
		select {
		case <-ctx.Done():
			return
		default:
		}
		conn, resp, err := c.dialWS(ctx, wsURL)
		if err != nil {
			if resp != nil {
				resp.Body.Close()
			}
			log.Warn("dirigera: ws dial failed", "err", err)
			if !waitBackoff(ctx, backoff) {
				return
			}
			backoff *= 2
			if backoff > maxBackoff {
				backoff = maxBackoff
			}
			continue
		}
		backoff = time.Second

		if err := readLoop(ctx, conn, out); err != nil {
			log.Warn("dirigera: ws read loop ended", "err", err)
		}
		_ = conn.Close(websocket.StatusNormalClosure, "reconnecting")
	}
}

func (c *Client) dialWS(ctx context.Context, wsURL string) (*websocket.Conn, *http.Response, error) {
	// Reuse the client's already-verified TLS transport for the WS
	// upgrade so pinning / CA rules apply uniformly.
	return websocket.Dial(ctx, wsURL, &websocket.DialOptions{
		HTTPClient: c.HTTP,
		HTTPHeader: http.Header{
			"Authorization": []string{"Bearer " + c.Token},
		},
	})
}

func readLoop(ctx context.Context, conn *websocket.Conn, out chan<- Event) error {
	for {
		_, data, err := conn.Read(ctx)
		if err != nil {
			return err
		}
		var ev Event
		if err := json.Unmarshal(data, &ev); err != nil {
			// A malformed frame is not fatal — the hub occasionally
			// pushes shapes we don't know about. Skip and keep going.
			continue
		}
		select {
		case out <- ev:
		case <-ctx.Done():
			return ctx.Err()
		}
	}
}

func waitBackoff(ctx context.Context, d time.Duration) bool {
	select {
	case <-ctx.Done():
		return false
	case <-time.After(d):
		return true
	}
}

// deriveWSURL converts an https://<host>:8443 base into a
// wss://<host>:8443/v1 upgrade URL. The hub's WS endpoint sits under
// /v1 by design.
func deriveWSURL(base string) (string, error) {
	u, err := url.Parse(base)
	if err != nil {
		return "", fmt.Errorf("dirigera: parse base URL: %w", err)
	}
	switch u.Scheme {
	case "https":
		u.Scheme = "wss"
	case "http":
		u.Scheme = "ws"
	default:
		return "", fmt.Errorf("dirigera: unsupported base scheme %q for WS", u.Scheme)
	}
	u.Path = strings.TrimRight(u.Path, "/") + "/v1"
	return u.String(), nil
}
