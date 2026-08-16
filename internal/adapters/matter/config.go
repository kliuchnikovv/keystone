// Package matter is the transport adapter that talks to a matter.js sidecar
// (Node.js process) over a local WebSocket JSON-RPC channel.
//
// See docs/matter-adapter-brief.md for the product context and multi-admin
// commissioning flow via Apple Home.
package matter

import (
	"fmt"
	"strings"
)

// Config parameters the matter.js sidecar location. In the default deployment
// the sidecar runs on the same host as keystone, so localhost is fine.
type Config struct {
	Host string
	Port int
}

// DefaultConfig points at the sidecar's default ws://localhost:5580 endpoint.
func DefaultConfig() Config {
	return Config{Host: "localhost", Port: 5580}
}

// URL builds the ws:// endpoint the client will dial.
func (c Config) URL() string {
	return fmt.Sprintf("ws://%s:%d", c.Host, c.Port)
}

// ParseSidecarURL accepts either a bare host:port or a full ws:// URL and
// returns a Config the adapter can dial. Empty schemes and wss:// are both
// tolerated — the ws:// scheme is implicit for a local sidecar.
func ParseSidecarURL(raw string) (Config, error) {
	s := strings.TrimSpace(raw)
	s = strings.TrimPrefix(s, "ws://")
	s = strings.TrimPrefix(s, "wss://")
	host, portStr, ok := strings.Cut(s, ":")
	if !ok || host == "" || portStr == "" {
		return Config{}, fmt.Errorf("expected host:port or ws://host:port, got %q", raw)
	}
	var port int
	if _, err := fmt.Sscanf(portStr, "%d", &port); err != nil || port <= 0 {
		return Config{}, fmt.Errorf("invalid port in %q", raw)
	}
	return Config{Host: host, Port: port}, nil
}
