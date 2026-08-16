// Package bridge implements ports.Adapter on top of a sidecar plugin.
//
// The wire vocabulary here IS the plugin adapter contract: a plugin that
// answers the methods below and pushes events onto the adapter.event topic
// slots into keystone's service layer with no core changes. When the
// contract moves out of internal/ (once we have a second in-tree plugin
// and stability guarantees to make), the types in this file become the
// stable public surface — everything else in bridge/ is core-side glue.
package bridge

import (
	"github.com/kliuchnikovv/keystone/internal/domain"
	"github.com/kliuchnikovv/keystone/internal/ports"
)

// Method names. Kept as constants so plugin authors and the bridge stay in
// lockstep and typos surface at compile time.
const (
	MethodStart        = "adapter.start"
	MethodStop         = "adapter.stop"
	MethodDiscover     = "adapter.discover"
	MethodCommission   = "adapter.commission"
	MethodReadState    = "adapter.readState"
	MethodWriteState   = "adapter.writeState"
	MethodInvoke       = "adapter.invokeAction"
	MethodDecommission = "adapter.decommission"
)

// TopicEvent is the single topic the plugin publishes onto for state
// changes, events, online/offline and adapter status. Bridge subscribes and
// translates each push into a ports.TransportEvent.
//
// One topic keeps the plugin contract small; kind lives inside the
// payload. If a plugin wants finer-grained subscriptions later (e.g. only
// state changes for a specific ref), a Filter over structured sub-topics
// like "adapter.event.state.<ref>" is a compatible extension.
const TopicEvent = "adapter.event"

// DiscoverResult is what a plugin returns for adapter.discover. Snapshot
// semantics — no streaming yet. A plugin whose transport genuinely
// streams (BLE scans, mDNS churn) can publish a "discovered" event
// separately once we agree on the topic.
type DiscoverResult struct {
	Devices []DiscoveredDevice `json:"devices"`
}

// DiscoveredDevice mirrors ports.DiscoveredDevice one-for-one, on the wire.
// domain types are aliased through so a change on either side surfaces at
// compile time.
type DiscoveredDevice struct {
	TransportRef domain.TransportRef `json:"transportRef"`
	Type         domain.DeviceType   `json:"type"`
	Name         string              `json:"name"`
	Manufacturer string              `json:"manufacturer,omitempty"`
	Model        string              `json:"model,omitempty"`
	Features     []domain.Feature    `json:"features,omitempty"`
	Metadata     map[string]string   `json:"metadata,omitempty"`
}

// CommissionParams carries pairing input. Progress reporting is not yet
// modelled; adapters that need it will push to a per-session topic once
// the contract picks one.
type CommissionParams struct {
	Payload  string            `json:"payload,omitempty"`
	WifiSSID string            `json:"wifiSsid,omitempty"`
	WifiCred string            `json:"wifiCred,omitempty"`
	Extra    map[string]string `json:"extra,omitempty"`
}

// CommissionResult is what the plugin returns after joining the device.
type CommissionResult struct {
	Ref domain.TransportRef `json:"ref"`
}

// AttrRef locates a single attribute on a device for read/write/invoke.
type AttrRef struct {
	Ref     domain.TransportRef `json:"ref"`
	Feature domain.FeatureKey   `json:"feature"`
	Key     string              `json:"key,omitempty"` // state key for read/write; empty on invoke
}

// ReadStateParams asks for one attribute value.
type ReadStateParams struct {
	AttrRef
}

// ReadStateResult carries the value the plugin read.
type ReadStateResult struct {
	Value any `json:"value"`
}

// WriteStateParams requests a change.
type WriteStateParams struct {
	AttrRef
	Value any `json:"value"`
}

// InvokeParams runs a command against a feature.
type InvokeParams struct {
	Ref     domain.TransportRef `json:"ref"`
	Feature domain.FeatureKey   `json:"feature"`
	Action  domain.ActionKey    `json:"action"`
	Args    map[string]any      `json:"args,omitempty"`
}

// DecommissionParams unpairs a device.
type DecommissionParams struct {
	Ref domain.TransportRef `json:"ref"`
}

// EventPayload is what the plugin puts on the adapter.event topic. Each
// field maps to ports.TransportEvent so the bridge can translate with no
// loss. Kind is the primary switch: state_changed, event_fired,
// online, offline, added, removed, adapter_status.
type EventPayload struct {
	Ref     domain.TransportRef      `json:"ref,omitempty"`
	Kind    ports.TransportEventKind `json:"kind"`
	Feature domain.FeatureKey        `json:"feature,omitempty"`
	Key     string                   `json:"key,omitempty"`
	Value   any                      `json:"value,omitempty"`
}
