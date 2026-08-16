package cli

import "fmt"

// StreamRoot follows the daemon's live event stream at /stream. Each
// line arrives as NDJSON; we print it verbatim so a downstream jq
// pipeline sees exactly what the daemon emits — the CLI is a wire,
// not a transformer.
//
// Ctrl-C stops the stream cleanly (the goroutine reading the body
// exits on the connection close).
func StreamRoot(_ []string) error {
	return GetStream("/stream", func(line []byte) bool {
		fmt.Println(string(line))
		return true
	})
}
