package dirigera

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"sync"

	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// Adapter implements ports.Adapter on top of a DIRIGERA HTTP client.
//
// Realtime updates are intentionally out of scope for v0: DIRIGERA's
// WebSocket stream needs a separate goroutine and a small routing
// table, and its absence does not prevent commissioning, control, or
// state reads. Subscribe currently emits only the adapter status
// change on Start/Stop so subscribers can tell "hub down" from "quiet".
type Adapter struct {
	client *Client
	log    *slog.Logger

	mu       sync.Mutex
	events   chan ports.TransportEvent
	started  bool
	stopping chan struct{}
}

// New builds an Adapter around a preconfigured Client.
func New(client *Client, log *slog.Logger) *Adapter {
	return &Adapter{client: client, log: log}
}

// Kind returns the transport identifier. The manifest declares the
// same string in spec.capabilities as "transport.dirigera".
func (a *Adapter) Kind() domain.TransportKind { return "dirigera" }

// Start opens the event channel and announces the adapter is up.
// Idempotent — a second Start reuses the existing channel.
func (a *Adapter) Start(ctx context.Context) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.started {
		return nil
	}
	a.events = make(chan ports.TransportEvent, 32)
	a.stopping = make(chan struct{})
	a.started = true
	// Cheap health probe: a single ListDevices confirms the token and
	// TLS pinning both work. A failure here is not fatal — the hub may
	// come online later — so we publish adapter_status=false and let
	// the core mark the transport degraded rather than losing the
	// plugin process.
	if _, err := a.client.ListDevices(ctx); err != nil {
		a.log.Warn("dirigera: initial probe failed — hub reachable later?", "err", err)
		a.publish(ctx, ports.TransportEvent{Kind: ports.TransportEventAdapterStatus, Value: false})
		return nil
	}
	a.publish(ctx, ports.TransportEvent{Kind: ports.TransportEventAdapterStatus, Value: true})
	return nil
}

// Stop closes the event channel. Subsequent Start reopens it, so a
// caller can restart the adapter through the sidecar surface.
func (a *Adapter) Stop(_ context.Context) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.started {
		return nil
	}
	close(a.stopping)
	close(a.events)
	a.started = false
	return nil
}

// Discover reports every device the hub currently knows about.
func (a *Adapter) Discover(ctx context.Context) (<-chan ports.DiscoveredDevice, error) {
	devices, err := a.client.ListDevices(ctx)
	if err != nil {
		return nil, err
	}
	out := make(chan ports.DiscoveredDevice, len(devices))
	for _, d := range devices {
		ref, dt, name, features := ToDiscovered(d)
		out <- ports.DiscoveredDevice{
			TransportRef: ref,
			Type:         dt,
			Name:         name,
			Manufacturer: "IKEA",
			Features:     features,
		}
	}
	close(out)
	return out, nil
}

// Commission is not supported over HTTP: DIRIGERA pairs devices via a
// physical join button on the hub itself. Commission is reserved for
// the token pairing flow, which is triggered elsewhere.
func (a *Adapter) Commission(_ context.Context, _ ports.CommissionRequest) (domain.TransportRef, error) {
	return "", errors.New("dirigera: pair devices via the hub button, not the /commission endpoint")
}

// ReadState fetches one attribute for a feature.
func (a *Adapter) ReadState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey) (any, error) {
	d, err := a.client.GetDevice(ctx, string(ref))
	if err != nil {
		return nil, err
	}
	attr, ok := attributeFor(feature, key)
	if !ok {
		return nil, fmt.Errorf("dirigera: no attribute mapping for %s/%s", feature, key)
	}
	v, ok := d.Attributes[attr]
	if !ok {
		return nil, fmt.Errorf("dirigera: device %q missing attribute %q", ref, attr)
	}
	return v, nil
}

// WriteState patches one attribute.
func (a *Adapter) WriteState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey, value any) error {
	attr, ok := attributeFor(feature, key)
	if !ok {
		return fmt.Errorf("dirigera: no attribute mapping for %s/%s", feature, key)
	}
	return a.client.SetAttributes(ctx, string(ref), map[string]any{attr: value})
}

// InvokeAction handles the small set of imperative operations.
func (a *Adapter) InvokeAction(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, action domain.ActionKey, _ map[string]any) error {
	switch {
	case feature == domain.FeatureOnOff && action == domain.ActionTurnOn:
		return a.client.SetAttributes(ctx, string(ref), map[string]any{"isOn": true})
	case feature == domain.FeatureOnOff && action == domain.ActionTurnOff:
		return a.client.SetAttributes(ctx, string(ref), map[string]any{"isOn": false})
	case feature == domain.FeatureOnOff && action == domain.ActionToggle:
		d, err := a.client.GetDevice(ctx, string(ref))
		if err != nil {
			return err
		}
		cur, _ := d.Attributes["isOn"].(bool)
		return a.client.SetAttributes(ctx, string(ref), map[string]any{"isOn": !cur})
	}
	return fmt.Errorf("dirigera: unsupported action %s/%s", feature, action)
}

// Subscribe returns the adapter's event channel. Realtime updates
// require a WebSocket subscription that this v0 does not open — the
// channel will still emit adapter_status changes so the core can tell
// a healthy quiet hub from a lost one.
func (a *Adapter) Subscribe(_ context.Context) (<-chan ports.TransportEvent, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.started {
		return nil, errors.New("dirigera: Start before Subscribe")
	}
	return a.events, nil
}

// Decommission unpairs a device from the hub.
func (a *Adapter) Decommission(ctx context.Context, ref domain.TransportRef) error {
	return a.client.RemoveDevice(ctx, string(ref))
}

func (a *Adapter) publish(ctx context.Context, ev ports.TransportEvent) {
	select {
	case a.events <- ev:
	case <-ctx.Done():
	case <-a.stopping:
	default:
		// Ring is full — the subscriber is slow. Dropping keeps us live
		// rather than blocking a health probe.
	}
}

// attributeFor maps (feature, key) → the DIRIGERA JSON attribute name.
// A missing mapping is not fatal; callers surface it as unsupported so
// the operator gets a specific error, not a silent no-op.
func attributeFor(feature domain.FeatureKey, key domain.StateKey) (string, bool) {
	switch {
	case feature == domain.FeatureOnOff && (key == domain.StateOnOff || strings.EqualFold(string(key), "isOn")):
		return "isOn", true
	case feature == domain.FeatureBrightness && key == domain.StateLevel:
		return "lightLevel", true
	}
	return "", false
}
