// Package ports declares the interfaces that the domain and service layers
// depend on. Concrete implementations live under internal/adapters (transports),
// internal/storage (persistence), etc. Ports know nothing about wire formats.
package ports

import (
	"context"
	"time"

	"github.com/kliuchnikovv/keystone/internal/domain"
)

// Adapter is the contract every transport must satisfy. It is the single
// integration seam between the engine core and any protocol / cloud / bridge.
type Adapter interface {
	// Kind returns the transport identifier (matter, zigbee, ...).
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
	Payload  string            // e.g. Matter setup code "MT:..."
	WifiSSID string            // for Matter Wi-Fi commissioning
	WifiCred string            // for Matter Wi-Fi commissioning
	Extra    map[string]string // transport-specific

	// Progress, if set, receives real pairing stages as the transport reports
	// them (Matter: PASE established, attestation, credentials, network, …).
	// Adapters that have no progress to report simply never call it.
	//
	// Called from the adapter's event goroutine, so it must not block: send to
	// a buffered channel rather than doing work inline.
	Progress func(stage, message string)
}

// CommissionableDevice is a device advertising itself as ready to pair but not
// yet in any of our fabrics. Everything here comes from the advertisement — the
// device has not been interviewed, so there are no features and no state.
//
// Note that discovery does NOT remove the need for a setup code: the passcode
// is never advertised, so pairing cannot start without the user reading it off
// the device or its box. Discovery only spares them from identifying which
// device is which.
type CommissionableDevice struct {
	// Ref identifies this advertisement. Pass it back in
	// CommissionRequest.Extra["matter.target"] to pair this exact device.
	Ref string
	// Name is the vendor-set advertised name, often empty.
	Name string
	// Type is the advertised device type, or "" when not advertised.
	Type domain.DeviceType
	// VendorID / ProductID identify the model; no vendor-name lookup exists.
	VendorID  int
	ProductID int
	// Discriminator is the device's long discriminator, useful for matching a
	// device against the digits printed next to its QR code.
	Discriminator int
}

// CommissionableDiscoverer is implemented by adapters that can find devices
// which are not yet commissioned. It is deliberately separate from
// ports.Adapter: most transports have no such concept, and Adapter.Discover
// means something different — devices already usable by this transport.
type CommissionableDiscoverer interface {
	// DiscoverCommissionable listens for advertisements for the given window
	// and streams what it finds. The channel is closed when the scan ends.
	DiscoverCommissionable(ctx context.Context, window time.Duration) (<-chan CommissionableDevice, error)
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

	// TransportEventAdapterStatus reports the adapter's own connectivity to
	// its backend (for Matter: the sidecar WebSocket), not any one device's.
	// Ref is empty and Value is a bool: true = usable, false = degraded.
	//
	// Subscribers otherwise cannot tell "quiet house" from "adapter is dead" —
	// both look like an idle channel. Every subscriber receives the current
	// status as soon as it subscribes, so late joiners are not left guessing.
	TransportEventAdapterStatus TransportEventKind = "adapter_status"
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
