// Package eventbus provides an in-process pub/sub. Subscribers receive their
// own buffered channel; overflow is handled by drop-oldest with a warning log
// so a slow subscriber cannot backpressure the whole bus.
package eventbus

import (
	"context"
	"log/slog"
	"sync"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

// defaultBufferSize is the buffer per subscriber channel. Sized to handle
// bursts (e.g. dozens of state changes from a scene switch) without dropping.
const defaultBufferSize = 128

// Bus is a simple fan-out event bus with two typed subscriptions.
type Bus struct {
	log *slog.Logger

	mu          sync.RWMutex
	stateSubs   map[chan domain.StateSnapshot]struct{}
	eventSubs   map[chan domain.Event]struct{}
	closed      bool
}

// New returns a ready-to-use Bus.
func New(log *slog.Logger) *Bus {
	if log == nil {
		log = slog.Default()
	}
	return &Bus{
		log:       log,
		stateSubs: make(map[chan domain.StateSnapshot]struct{}),
		eventSubs: make(map[chan domain.Event]struct{}),
	}
}

// PublishState fans out a state snapshot to every current subscriber.
func (b *Bus) PublishState(ctx context.Context, s domain.StateSnapshot) error {
	b.mu.RLock()
	defer b.mu.RUnlock()

	if b.closed {
		return nil
	}

	for ch := range b.stateSubs {
		select {
		case ch <- s:
		default:
			// Drop-oldest: read one to make room.
			select {
			case <-ch:
				b.log.Warn("state subscriber slow, dropped oldest",
					"device_id", s.DeviceID)
			default:
			}
			select {
			case ch <- s:
			default:
				b.log.Error("state subscriber still full, dropping current",
					"device_id", s.DeviceID)
			}
		}
	}
	return nil
}

// PublishEvent fans out an event to every current subscriber.
func (b *Bus) PublishEvent(ctx context.Context, e domain.Event) error {
	b.mu.RLock()
	defer b.mu.RUnlock()

	if b.closed {
		return nil
	}

	for ch := range b.eventSubs {
		select {
		case ch <- e:
		default:
			select {
			case <-ch:
				b.log.Warn("event subscriber slow, dropped oldest",
					"device_id", e.DeviceID, "event", e.Name)
			default:
			}
			select {
			case ch <- e:
			default:
				b.log.Error("event subscriber still full, dropping current",
					"device_id", e.DeviceID, "event", e.Name)
			}
		}
	}
	return nil
}

// SubscribeStates returns a channel that receives every published state
// snapshot until the context is cancelled.
func (b *Bus) SubscribeStates(ctx context.Context) <-chan domain.StateSnapshot {
	ch := make(chan domain.StateSnapshot, defaultBufferSize)
	b.mu.Lock()
	b.stateSubs[ch] = struct{}{}
	b.mu.Unlock()

	go func() {
		<-ctx.Done()
		b.mu.Lock()
		// Guard against races with Close() which may have already closed the
		// channel and removed it from the map.
		if _, still := b.stateSubs[ch]; still {
			delete(b.stateSubs, ch)
			close(ch)
		}
		b.mu.Unlock()
	}()

	return ch
}

// SubscribeEvents returns a channel that receives every published event until
// the context is cancelled.
func (b *Bus) SubscribeEvents(ctx context.Context) <-chan domain.Event {
	ch := make(chan domain.Event, defaultBufferSize)
	b.mu.Lock()
	b.eventSubs[ch] = struct{}{}
	b.mu.Unlock()

	go func() {
		<-ctx.Done()
		b.mu.Lock()
		if _, still := b.eventSubs[ch]; still {
			delete(b.eventSubs, ch)
			close(ch)
		}
		b.mu.Unlock()
	}()

	return ch
}

// Close blocks new subscribers and drops all existing ones. Safe to call more
// than once.
func (b *Bus) Close() {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.closed {
		return
	}
	b.closed = true
	for ch := range b.stateSubs {
		close(ch)
		delete(b.stateSubs, ch)
	}
	for ch := range b.eventSubs {
		close(ch)
		delete(b.eventSubs, ch)
	}
}
