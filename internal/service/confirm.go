package service

import (
	"context"
	"log/slog"
	"sync"
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// confirmTimeout is how long a device gets to report the change we asked for.
// Matter subscription reports normally arrive well inside a second; five
// seconds leaves room for a sleepy device without leaving a person staring at
// a control that has silently done nothing.
const confirmTimeout = 5 * time.Second

// confirmations watches whether a command we sent actually changed anything.
//
// The protocol cannot answer this on its own. A write to a read-only attribute
// is accepted, a colour command on a lamp that is off is discarded by spec, a
// command missing a mandatory argument never leaves the stack — and every one
// of those looks like success from the caller's side. The only trustworthy
// signal is the device reporting the new state back, which it does anyway
// because we subscribe to it.
//
// So: after each state-changing call we expect a report, and if none arrives we
// say so out loud instead of leaving "nothing happened" to be discovered by a
// human with a lamp in their hand.
type confirmations struct {
	log     *slog.Logger
	bus     ports.EventBus
	timeout time.Duration

	mu      sync.Mutex
	pending map[confirmKey]*time.Timer
}

type confirmKey struct {
	device  domain.DeviceID
	feature domain.FeatureKey
	state   domain.StateKey
}

func newConfirmations(log *slog.Logger, bus ports.EventBus, timeout time.Duration) *confirmations {
	if timeout <= 0 {
		timeout = confirmTimeout
	}
	return &confirmations{
		log:     log,
		bus:     bus,
		timeout: timeout,
		pending: make(map[confirmKey]*time.Timer),
	}
}

// expect records that a device should report (feature, state) shortly. what
// describes the command in the words the user would use, for the message.
//
// A second command for the same key replaces the first: only the latest
// intention is worth reporting on, and a slider produces a burst of them.
func (c *confirmations) expect(device domain.DeviceID, feature domain.FeatureKey, state domain.StateKey, what string) {
	if c == nil || c.bus == nil {
		return
	}
	key := confirmKey{device: device, feature: feature, state: state}

	c.mu.Lock()
	defer c.mu.Unlock()
	if timer, ok := c.pending[key]; ok {
		timer.Stop()
	}
	c.pending[key] = time.AfterFunc(c.timeout, func() {
		c.mu.Lock()
		delete(c.pending, key)
		c.mu.Unlock()
		c.report(key, what)
	})
}

// observe clears an expectation because the device reported the state. Called
// for every device report, so it must stay cheap when nothing is pending.
func (c *confirmations) observe(device domain.DeviceID, feature domain.FeatureKey, state domain.StateKey) {
	if c == nil {
		return
	}
	key := confirmKey{device: device, feature: feature, state: state}

	c.mu.Lock()
	timer, ok := c.pending[key]
	if ok {
		delete(c.pending, key)
	}
	c.mu.Unlock()

	if ok {
		timer.Stop()
	}
}

// report announces that the device never confirmed the change.
//
// Deliberately an event rather than an error return: by the time we know, the
// call has long since returned successfully — which is exactly the trap. The
// UI shows it next to the control and rules can react to it.
func (c *confirmations) report(key confirmKey, what string) {
	c.log.Warn("device did not confirm the change",
		"device_id", key.device, "feature", key.feature, "state", key.state,
		"what", what, "waited", c.timeout.String())

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	_ = c.bus.PublishEvent(ctx, domain.Event{
		DeviceID: key.device,
		Feature:  key.feature,
		Name:     domain.EventNotConfirmed,
		Data: map[string]any{
			"state":     string(key.state),
			"what":      what,
			"waited_ms": c.timeout.Milliseconds(),
		},
		At: time.Now().UTC(),
	})
}

// stop cancels every outstanding expectation. Used on shutdown so a pending
// timer cannot fire into a closed bus.
func (c *confirmations) stop() {
	if c == nil {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	for key, timer := range c.pending {
		timer.Stop()
		delete(c.pending, key)
	}
}

// alreadyAt reports whether the device is already in the state being requested,
// in which case Matter sends no report and waiting for one would raise a false
// alarm. Numbers are compared with a tolerance because values round-trip
// through the transport's own units: 30% becomes level 76 and comes back as
// 29%, and an alarm on that would train everyone to ignore the real ones.
func alreadyAt(current domain.StateSnapshot, target any) bool {
	switch want := target.(type) {
	case bool:
		got, ok := current.Value.(bool)
		return ok && got == want
	case string:
		got, ok := current.Value.(string)
		return ok && got == want
	default:
		wantF, okWant := asFloat(target)
		gotF, okGot := asFloat(current.Value)
		if !okWant || !okGot {
			return false
		}
		diff := wantF - gotF
		if diff < 0 {
			diff = -diff
		}
		return diff <= 2
	}
}

func asFloat(v any) (float64, bool) {
	switch n := v.(type) {
	case int:
		return float64(n), true
	case int64:
		return float64(n), true
	case float32:
		return float64(n), true
	case float64:
		return n, true
	default:
		return 0, false
	}
}
