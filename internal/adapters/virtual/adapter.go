// Package virtual provides an in-memory Adapter implementation used for
// tests, demos, and running the engine without any real hardware.
//
// It exposes a small helper API (Inject) so tests and scenarios can push
// state changes and events as if a real device had emitted them.
package virtual

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"sync"
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// Adapter is a fully in-memory adapter with programmable virtual devices.
type Adapter struct {
	log *slog.Logger

	mu      sync.Mutex
	started bool
	devices map[domain.TransportRef]*virtualDevice

	// events fan-out to the current subscriber; nil if no active subscription.
	subMu sync.RWMutex
	subs  map[chan ports.TransportEvent]struct{}
}

type virtualDevice struct {
	ref      domain.TransportRef
	kind     domain.DeviceType
	name     string
	features []domain.Feature
	state    map[stateKey]any
}

type stateKey struct {
	Feature domain.FeatureKey
	Key     domain.StateKey
}

// New builds an empty virtual adapter.
func New(log *slog.Logger) *Adapter {
	if log == nil {
		log = slog.Default()
	}
	return &Adapter{
		log:     log,
		devices: make(map[domain.TransportRef]*virtualDevice),
		subs:    make(map[chan ports.TransportEvent]struct{}),
	}
}

// Kind implements ports.Adapter.
func (a *Adapter) Kind() domain.TransportKind {
	return domain.TransportVirtual
}

// Start implements ports.Adapter.
func (a *Adapter) Start(ctx context.Context) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.started {
		return errors.New("virtual adapter already started")
	}
	a.started = true
	a.log.Info("virtual adapter started")
	return nil
}

// Stop implements ports.Adapter.
func (a *Adapter) Stop(ctx context.Context) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.started = false

	a.subMu.Lock()
	for ch := range a.subs {
		close(ch)
		delete(a.subs, ch)
	}
	a.subMu.Unlock()

	a.log.Info("virtual adapter stopped")
	return nil
}

// Discover implements ports.Adapter. It emits every seeded virtual device once
// and then closes the channel.
func (a *Adapter) Discover(ctx context.Context) (<-chan ports.DiscoveredDevice, error) {
	out := make(chan ports.DiscoveredDevice, len(a.devices))
	a.mu.Lock()
	for _, dev := range a.devices {
		out <- ports.DiscoveredDevice{
			TransportRef: dev.ref,
			Type:         dev.kind,
			Name:         dev.name,
			Manufacturer: "Keystone",
			Model:        "virtual",
			Features:     append([]domain.Feature(nil), dev.features...),
			Metadata:     map[string]string{"kind": "virtual"},
		}
	}
	a.mu.Unlock()
	close(out)
	return out, nil
}

// Commission implements ports.Adapter. In virtual mode, the "setup payload"
// is interpreted as `type:name` (e.g. "plug:Kitchen kettle"). The adapter
// synthesises features based on the requested type.
func (a *Adapter) Commission(ctx context.Context, req ports.CommissionRequest) (domain.TransportRef, error) {
	kind, name := parseVirtualPayload(req.Payload)
	if kind == "" {
		return "", fmt.Errorf("invalid virtual payload %q, expected TYPE:NAME", req.Payload)
	}

	ref := domain.TransportRef(fmt.Sprintf("virt-%d", time.Now().UnixNano()))
	dev := &virtualDevice{
		ref:      ref,
		kind:     kind,
		name:     name,
		features: featuresForType(kind),
		state:    make(map[stateKey]any),
	}
	// Seed defaults so ReadState never returns nothing.
	seedDefaults(dev)

	a.mu.Lock()
	a.devices[ref] = dev
	a.mu.Unlock()
	a.log.Info("virtual device commissioned", "ref", ref, "type", kind, "name", name)
	return ref, nil
}

// ReadState implements ports.Adapter.
func (a *Adapter) ReadState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey) (any, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	dev, ok := a.devices[ref]
	if !ok {
		return nil, fmt.Errorf("virtual device %q not found", ref)
	}
	v, ok := dev.state[stateKey{feature, key}]
	if !ok {
		return nil, fmt.Errorf("state %s.%s not set", feature, key)
	}
	return v, nil
}

// WriteState implements ports.Adapter.
func (a *Adapter) WriteState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey, value any) error {
	a.mu.Lock()
	dev, ok := a.devices[ref]
	if !ok {
		a.mu.Unlock()
		return fmt.Errorf("virtual device %q not found", ref)
	}
	dev.state[stateKey{feature, key}] = value
	a.mu.Unlock()

	a.emit(ports.TransportEvent{
		Ref:     ref,
		Kind:    ports.TransportEventStateChanged,
		Feature: feature,
		Key:     string(key),
		Value:   value,
	})
	return nil
}

// InvokeAction implements ports.Adapter. Actions are translated into state
// writes for the virtual adapter — turn_on = onoff.value=true, etc.
func (a *Adapter) InvokeAction(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, action domain.ActionKey, params map[string]any) error {
	switch action {
	case domain.ActionTurnOn:
		return a.WriteState(ctx, ref, feature, domain.StateOnOff, true)
	case domain.ActionTurnOff:
		return a.WriteState(ctx, ref, feature, domain.StateOnOff, false)
	case domain.ActionToggle:
		cur, _ := a.ReadState(ctx, ref, feature, domain.StateOnOff)
		next := true
		if b, ok := cur.(bool); ok {
			next = !b
		}
		return a.WriteState(ctx, ref, feature, domain.StateOnOff, next)
	case domain.ActionSet:
		if params == nil {
			return errors.New("action set requires params")
		}
		for k, v := range params {
			if err := a.WriteState(ctx, ref, feature, domain.StateKey(k), v); err != nil {
				return err
			}
		}
		return nil
	}
	return fmt.Errorf("virtual adapter: unknown action %q", action)
}

// Subscribe implements ports.Adapter.
func (a *Adapter) Subscribe(ctx context.Context) (<-chan ports.TransportEvent, error) {
	ch := make(chan ports.TransportEvent, 64)
	a.subMu.Lock()
	a.subs[ch] = struct{}{}
	a.subMu.Unlock()

	go func() {
		<-ctx.Done()
		a.subMu.Lock()
		if _, ok := a.subs[ch]; ok {
			delete(a.subs, ch)
			close(ch)
		}
		a.subMu.Unlock()
	}()

	return ch, nil
}

// Decommission implements ports.Adapter.
func (a *Adapter) Decommission(ctx context.Context, ref domain.TransportRef) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if _, ok := a.devices[ref]; !ok {
		return fmt.Errorf("virtual device %q not found", ref)
	}
	delete(a.devices, ref)
	return nil
}

// Inject pushes a synthetic state change from a virtual device. Tests and
// scripted demos call this to simulate real-world events without going
// through the write path.
func (a *Adapter) Inject(ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey, value any) {
	a.mu.Lock()
	dev, ok := a.devices[ref]
	if !ok {
		a.mu.Unlock()
		return
	}
	dev.state[stateKey{feature, key}] = value
	a.mu.Unlock()

	a.emit(ports.TransportEvent{
		Ref:     ref,
		Kind:    ports.TransportEventStateChanged,
		Feature: feature,
		Key:     string(key),
		Value:   value,
	})
}

// InjectEvent pushes a synthetic event from a virtual device.
func (a *Adapter) InjectEvent(ref domain.TransportRef, feature domain.FeatureKey, name domain.EventKey, data map[string]any) {
	a.emit(ports.TransportEvent{
		Ref:     ref,
		Kind:    ports.TransportEventFired,
		Feature: feature,
		Key:     string(name),
		Value:   data,
	})
}

func (a *Adapter) emit(ev ports.TransportEvent) {
	a.subMu.RLock()
	defer a.subMu.RUnlock()
	for ch := range a.subs {
		select {
		case ch <- ev:
		default:
			// Slow subscriber; drop oldest and try again.
			select {
			case <-ch:
			default:
			}
			select {
			case ch <- ev:
			default:
			}
		}
	}
}

// parseVirtualPayload parses "TYPE:NAME".
func parseVirtualPayload(s string) (domain.DeviceType, string) {
	for i := 0; i < len(s); i++ {
		if s[i] == ':' {
			return domain.DeviceType(s[:i]), s[i+1:]
		}
	}
	return "", ""
}

// featuresForType synthesises a feature set for well-known device types so
// virtual devices behave believably.
func featuresForType(t domain.DeviceType) []domain.Feature {
	switch t {
	case domain.DeviceTypeLight:
		return []domain.Feature{
			{Key: domain.FeatureOnOff, States: []domain.StateKey{domain.StateOnOff}, Actions: []domain.ActionKey{domain.ActionTurnOn, domain.ActionTurnOff, domain.ActionToggle}},
			{Key: domain.FeatureBrightness, States: []domain.StateKey{domain.StateLevel}, Actions: []domain.ActionKey{domain.ActionSet}},
			{Key: domain.FeatureColorTemp, States: []domain.StateKey{domain.StateColorTempK}, Actions: []domain.ActionKey{domain.ActionSet}},
		}
	case domain.DeviceTypePlug:
		return []domain.Feature{
			{Key: domain.FeatureOnOff, States: []domain.StateKey{domain.StateOnOff}, Actions: []domain.ActionKey{domain.ActionTurnOn, domain.ActionTurnOff, domain.ActionToggle}},
			{Key: domain.FeaturePowerMeter, States: []domain.StateKey{domain.StatePowerNow, domain.StateEnergyTotal}},
		}
	case domain.DeviceTypeMotion:
		return []domain.Feature{
			{Key: domain.FeatureMotion, States: []domain.StateKey{domain.StateOccupied}, Events: []domain.EventKey{domain.EventMotionDetected}},
			{Key: domain.FeatureBattery, States: []domain.StateKey{domain.StateBatteryLvl}},
		}
	}
	return nil
}

// seedDefaults sets safe initial values for every state a device exposes.
func seedDefaults(dev *virtualDevice) {
	for _, f := range dev.features {
		for _, s := range f.States {
			switch s {
			case domain.StateOnOff:
				dev.state[stateKey{f.Key, s}] = false
			case domain.StateLevel:
				dev.state[stateKey{f.Key, s}] = 100
			case domain.StateColorTempK:
				dev.state[stateKey{f.Key, s}] = 2700 // warm default
			case domain.StatePowerNow:
				dev.state[stateKey{f.Key, s}] = float32(0)
			case domain.StateEnergyTotal:
				dev.state[stateKey{f.Key, s}] = float64(0)
			case domain.StateOccupied:
				dev.state[stateKey{f.Key, s}] = false
			case domain.StateBatteryLvl:
				dev.state[stateKey{f.Key, s}] = 100
			}
		}
	}
}
