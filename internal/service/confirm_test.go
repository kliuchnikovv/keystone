package service

import (
	"context"
	"io"
	"log/slog"
	"testing"
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
	"github.com/kliuchnikovv/keystone/internal/registry"
)

// recordingBus collects published events so a test can assert on what the
// system said out loud.
type recordingBus struct {
	events chan domain.Event
}

func newRecordingBus() *recordingBus {
	return &recordingBus{events: make(chan domain.Event, 8)}
}

func (b *recordingBus) PublishState(context.Context, domain.StateSnapshot) error { return nil }
func (b *recordingBus) PublishEvent(_ context.Context, e domain.Event) error {
	select {
	case b.events <- e:
	default:
	}
	return nil
}
func (b *recordingBus) SubscribeStates(context.Context) <-chan domain.StateSnapshot { return nil }
func (b *recordingBus) SubscribeEvents(context.Context) <-chan domain.Event         { return nil }

// waitForNotConfirmed returns the first not-confirmed event, or nil if none
// arrived in time.
func (b *recordingBus) waitForNotConfirmed(d time.Duration) *domain.Event {
	deadline := time.After(d)
	for {
		select {
		case e := <-b.events:
			if e.Name == domain.EventNotConfirmed {
				return &e
			}
		case <-deadline:
			return nil
		}
	}
}

// confirmTestService builds a service whose confirmations fire fast enough for
// a test to wait on them.
func confirmTestService(t *testing.T, bus ports.EventBus) (*DeviceService, domain.DeviceID) {
	t.Helper()
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	reg := registry.New()
	svc := NewDeviceService(log, reg, bus, []ports.Adapter{&fakeAdapter{}})
	svc.confirms = newConfirmations(log, bus, 30*time.Millisecond)
	t.Cleanup(svc.StopConfirmations)

	d := &domain.Device{
		ID:           domain.DeviceID("dev-1"),
		Type:         domain.DeviceTypeLight,
		Name:         "lamp",
		Transport:    domain.TransportMatter,
		TransportRef: "n1",
		Features:     []domain.Feature{{Key: domain.FeatureBrightness}},
		CreatedAt:    time.Now().UTC(),
	}
	if err := reg.Add(d); err != nil {
		t.Fatalf("add device: %v", err)
	}
	return svc, d.ID
}

// The whole point of the tracker: the adapter accepted the write, so the call
// returned nil — but the device never reported the new value, which is what a
// write to a read-only attribute looks like. That must not pass as success.
func TestWriteWithoutADeviceReportIsAnnounced(t *testing.T) {
	bus := newRecordingBus()
	svc, id := confirmTestService(t, bus)

	if err := svc.WriteState(context.Background(), id, domain.FeatureBrightness, domain.StateLevel, 60); err != nil {
		t.Fatalf("write: %v", err)
	}

	ev := bus.waitForNotConfirmed(time.Second)
	if ev == nil {
		t.Fatal("silent failure went unreported: expected a not_confirmed event")
	}
	if ev.DeviceID != id || ev.Feature != domain.FeatureBrightness {
		t.Fatalf("event points at the wrong control: %+v", ev)
	}
	if got := ev.Data["state"]; got != string(domain.StateLevel) {
		t.Fatalf("state = %v, want %q", got, domain.StateLevel)
	}
}

// The device did report back, so nothing is wrong and nothing should be said.
// A tracker that cries wolf on working hardware is worse than none.
func TestWriteConfirmedByTheDeviceStaysQuiet(t *testing.T) {
	bus := newRecordingBus()
	svc, id := confirmTestService(t, bus)

	if err := svc.WriteState(context.Background(), id, domain.FeatureBrightness, domain.StateLevel, 60); err != nil {
		t.Fatalf("write: %v", err)
	}
	// What IngressLoop does when the subscription report arrives.
	svc.confirms.observe(id, domain.FeatureBrightness, domain.StateLevel)

	if ev := bus.waitForNotConfirmed(150 * time.Millisecond); ev != nil {
		t.Fatalf("reported a failure on a device that answered: %+v", ev)
	}
}

// Matter reports on change. Asking a lamp already at 60 to go to 60 produces
// no report at all, and treating that silence as a fault would flag every
// repeated command.
func TestAskingForTheCurrentValueIsNotAFailure(t *testing.T) {
	bus := newRecordingBus()
	svc, id := confirmTestService(t, bus)
	svc.registry.PutState(domain.StateSnapshot{
		DeviceID:  id,
		Feature:   domain.FeatureBrightness,
		Key:       domain.StateLevel,
		Value:     60,
		UpdatedAt: time.Now().UTC(),
	})

	if err := svc.WriteState(context.Background(), id, domain.FeatureBrightness, domain.StateLevel, 60); err != nil {
		t.Fatalf("write: %v", err)
	}

	if ev := bus.waitForNotConfirmed(150 * time.Millisecond); ev != nil {
		t.Fatalf("flagged a no-op as a failure: %+v", ev)
	}
}

// An action has no value to compare against, so it is always tracked — via the
// state it is supposed to change.
func TestActionsAreTrackedThroughTheStateTheyChange(t *testing.T) {
	bus := newRecordingBus()
	svc, id := confirmTestService(t, bus)

	if err := svc.InvokeAction(context.Background(), id, domain.FeatureOnOff, domain.ActionTurnOn, nil); err != nil {
		t.Fatalf("invoke: %v", err)
	}

	ev := bus.waitForNotConfirmed(time.Second)
	if ev == nil {
		t.Fatal("turn_on that changed nothing went unreported")
	}
	if got := ev.Data["state"]; got != string(domain.StateOnOff) {
		t.Fatalf("state = %v, want %q", got, domain.StateOnOff)
	}
}

// A slider produces a burst of writes; only the last one is worth waiting for,
// and each of them must not leave its own timer behind.
func TestOnlyTheLatestIntentionIsTracked(t *testing.T) {
	bus := newRecordingBus()
	svc, id := confirmTestService(t, bus)

	for _, v := range []int{10, 20, 30} {
		if err := svc.WriteState(context.Background(), id, domain.FeatureBrightness, domain.StateLevel, v); err != nil {
			t.Fatalf("write: %v", err)
		}
	}
	if ev := bus.waitForNotConfirmed(time.Second); ev == nil {
		t.Fatal("expected one not_confirmed event")
	}
	if ev := bus.waitForNotConfirmed(150 * time.Millisecond); ev != nil {
		t.Fatalf("three writes to one control produced more than one alarm: %+v", ev)
	}
}

// Values round-trip through the transport's own units — 30% becomes level 76
// and comes back as 29%. Treating that as "not what I asked for" would train
// everyone to ignore the real alarms.
func TestAlreadyAtToleratesUnitRoundTripping(t *testing.T) {
	cases := []struct {
		name    string
		current any
		target  any
		want    bool
	}{
		{"exact int", 60, 60, true},
		{"rounding drift", 29, 30, true},
		{"real change", 30, 60, false},
		{"bool match", true, true, true},
		{"bool differs", false, true, false},
		{"string match", "cool", "cool", true},
		{"string differs", "warm", "cool", false},
		{"unknown types", struct{}{}, struct{}{}, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := alreadyAt(domain.StateSnapshot{Value: tc.current}, tc.target)
			if got != tc.want {
				t.Fatalf("alreadyAt(%v, %v) = %v, want %v", tc.current, tc.target, got, tc.want)
			}
		})
	}
}
