package manager

import (
	"context"
	"sync"
	"time"
)

// DefaultLogBufferSize is how many recent log lines the manager keeps
// per plugin. Deep enough to cover a normal boot sequence, shallow
// enough to bound memory for long-running plugins.
const DefaultLogBufferSize = 500

// LogLine is one line of captured child output.
type LogLine struct {
	Time   time.Time `json:"time"`
	Plugin string    `json:"plugin"`
	Stream string    `json:"stream"` // stdout / stderr / stderr:<sidecar>
	Line   string    `json:"line"`
}

// logBuffer is a bounded ring of the most recent lines plus a set of
// live followers. Push is called from a per-plugin log pump goroutine.
type logBuffer struct {
	mu        sync.Mutex
	ring      []LogLine
	cap       int
	next      int
	full      bool
	followers map[chan LogLine]struct{}
}

func newLogBuffer(capacity int) *logBuffer {
	return &logBuffer{
		ring:      make([]LogLine, capacity),
		cap:       capacity,
		followers: make(map[chan LogLine]struct{}),
	}
}

func (b *logBuffer) push(l LogLine) {
	b.mu.Lock()
	b.ring[b.next] = l
	b.next = (b.next + 1) % b.cap
	if b.next == 0 {
		b.full = true
	}
	// Broadcast to followers without holding the lock for long. A
	// follower that has stopped reading gets its line dropped so a
	// hung client cannot back-pressure the pump.
	for ch := range b.followers {
		select {
		case ch <- l:
		default:
		}
	}
	b.mu.Unlock()
}

// tail returns the last n lines in order (oldest first).
func (b *logBuffer) tail(n int) []LogLine {
	b.mu.Lock()
	defer b.mu.Unlock()

	total := b.cap
	if !b.full {
		total = b.next
	}
	if n <= 0 || n > total {
		n = total
	}
	out := make([]LogLine, 0, n)
	start := (b.next - n + b.cap) % b.cap
	for i := 0; i < n; i++ {
		out = append(out, b.ring[(start+i)%b.cap])
	}
	return out
}

// subscribe returns a channel that receives every subsequent push, and a
// cancel that removes the follower.
func (b *logBuffer) subscribe() (chan LogLine, func()) {
	ch := make(chan LogLine, 64)
	b.mu.Lock()
	b.followers[ch] = struct{}{}
	b.mu.Unlock()
	return ch, func() {
		b.mu.Lock()
		delete(b.followers, ch)
		b.mu.Unlock()
		close(ch)
	}
}

// Logs returns the last tail lines a plugin's supervisors have emitted.
// tail <= 0 means "everything buffered". Unknown plugin returns nil.
func (m *Manager) Logs(name string, tail int) []LogLine {
	m.logsMu.RLock()
	buf, ok := m.logs[name]
	m.logsMu.RUnlock()
	if !ok {
		return nil
	}
	return buf.tail(tail)
}

// Follow returns a channel that emits every new log line for a plugin
// until ctx is cancelled. Returns an error if the plugin is unknown.
func (m *Manager) Follow(ctx context.Context, name string) (<-chan LogLine, error) {
	m.logsMu.RLock()
	buf, ok := m.logs[name]
	m.logsMu.RUnlock()
	if !ok {
		return nil, ErrPluginNotFound
	}
	src, cancel := buf.subscribe()
	out := make(chan LogLine, 64)
	go func() {
		defer close(out)
		defer cancel()
		for {
			select {
			case <-ctx.Done():
				return
			case l, ok := <-src:
				if !ok {
					return
				}
				select {
				case out <- l:
				case <-ctx.Done():
					return
				}
			}
		}
	}()
	return out, nil
}

// bufferFor lazily creates the buffer for a plugin.
func (m *Manager) bufferFor(name string) *logBuffer {
	m.logsMu.Lock()
	buf, ok := m.logs[name]
	if !ok {
		buf = newLogBuffer(DefaultLogBufferSize)
		m.logs[name] = buf
	}
	m.logsMu.Unlock()
	return buf
}

// captureLine returns a per-plugin-and-stream sink that both stores the
// line in the ring buffer (for /plugins/{name}/logs) and forwards to
// Options.OnPluginLog. Passed as OnStdout/OnStderr to every child
// process the manager supervises.
func (m *Manager) captureLine(pluginName, stream string) func(string) {
	buf := m.bufferFor(pluginName)
	return func(line string) {
		buf.push(LogLine{
			Time:   time.Now().UTC(),
			Plugin: pluginName,
			Stream: stream,
			Line:   line,
		})
		if m.opts.OnPluginLog != nil {
			m.opts.OnPluginLog(pluginName, stream, line)
		}
	}
}
