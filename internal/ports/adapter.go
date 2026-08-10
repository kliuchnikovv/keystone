// Package ports declares the interfaces that the domain and service layers
// depend on. Concrete implementations live under internal/adapters (transports),
// internal/storage (persistence), etc. Ports know nothing about wire formats.
package ports

import (
	"context"

	"github.com/keystone/keystone/internal/domain"
)

// Adapter is the contract every transport must satisfy. It is the single
// integration seam between the engine core and any protocol / cloud / bridge.
type Adapter interface {
	// Kind returns the transport identifier (matter, zigbee, dirigera, ...).
	Kind() domain.TransportKind

	// Start opens connections to the underlying transport (sidecar, broker,
	// HTTP API). Must be non-blocking and return once the adapter is ready to
	// accept operations.
	Start(ctx context.Context) error

	// Stop tears the adapter down gracefully.
	Stop(ctx context.Context) error

	// Discover streams devices the transport currently sees.
	Discover(ctx context.Context) (<-chan DiscoveredDevice, error)

	// Commission adds a new device to the transport (pair/join/OTA).
	Commission(ctx context.Context, req CommissionRequest) (domain.TransportRef, error)

	// ReadState fetches the current value of a state attribute.
	ReadState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey) (any, error)

	// WriteState requests a change to a settable state attribute.
	WriteState(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, key domain.StateKey, value any) error

	// InvokeAction runs a command against a feature.
	InvokeAction(ctx context.Context, ref domain.TransportRef, feature domain.FeatureKey, action domain.ActionKey, params map[string]any) error

	// Subscribe returns a channel that emits every state change / event / device
	// lifecycle message from the transport. The channel is closed when the
	// context is cancelled or Stop is called.
	Subscribe(ctx context.Context) (<-chan TransportEvent, error)

	// Decommission removes a device from the transport.
	Decommission(ctx context.Context, ref domain.TransportRef) error
}

// DiscoveredDevice describes a device the transport has found but the engine
// has not yet decided to add to the registry.
type DiscoveredDevice struct {
	TransportRef domain.TransportRef
	Type         domain.DeviceType
	Name         string
	Manufacturer string
	Model        string
	Features     []domain.Feature
	Metadata     map[string]string
}

// CommissionRequest carries pairing parameters. Concrete fields depend on the
// transport — for Matter it's a setup payload, for Zigbee typically nothing
// (permit-join is a mode).
type CommissionRequest struct {
	Payload   string            // e.g. Matter setup code "MT:..."
	WifiSSID  string            // for Matter Wi-Fi commissioning
	WifiCred  string            // for Matter Wi-Fi commissioning
	Extra     map[string]string // transport-specific
}

// TransportEventKind classifies what happened in a TransportEvent.
type TransportEventKind string

const (
	TransportEventStateChanged TransportEventKind = "state_changed"
	TransportEventFired        TransportEventKind = "event_fired"
	TransportEventOnline       TransportEventKind = "online"
	TransportEventOffline      TransportEventKind = "offline"
	TransportEventAdded        TransportEventKind = "added"
	TransportEventRemoved      TransportEventKind = "removed"
)

// TransportEvent is the unified message emitted by every adapter's Subscribe
// channel. Ingress code maps this into domain.StateSnapshot or domain.Event
// and publishes onto the internal event bus.
type TransportEvent struct {
	Ref     domain.TransportRef
	Kind    TransportEventKind
	Feature domain.FeatureKey
	Key     string // state key or event key
	Value   any
}
