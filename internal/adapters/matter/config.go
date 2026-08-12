// Package matter is the transport adapter that talks to a matter.js sidecar
// (Node.js process) over a local WebSocket JSON-RPC channel.
//
// See docs/matter-adapter-brief.md for the product context and multi-admin
// commissioning flow via Apple Home.
package matter

import "fmt"

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
