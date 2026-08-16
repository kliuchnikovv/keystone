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

// CommissionParams carries pairing input.
//
// ProgressID, when non-empty, asks the plugin to publish progress
// updates onto adapter.event with Kind = "commission_progress" and
// CommissionID = ProgressID. The bridge subscribes for the duration of
// the call and fires ports.CommissionRequest.Progress on each match.
// An empty ProgressID means the caller does not want progress and the
// plugin can skip the extra pushes.
type CommissionParams struct {
	Payload    string            `json:"payload,omitempty"`
	WifiSSID   string            `json:"wifiSsid,omitempty"`
	WifiCred   string            `json:"wifiCred,omitempty"`
	Extra      map[string]string `json:"extra,omitempty"`
	ProgressID string            `json:"progressId,omitempty"`
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

// DiscoverCommissionableParams bounds a scan and identifies which
// per-scan channel finds should stream onto. ScanID is minted by the
// bridge before the call and echoed back on every event.
type DiscoverCommissionableParams struct {
	TimeoutMs int64  `json:"timeoutMs"`
	ScanID    string `json:"scanId"`
}

// DiscoverCommissionableResult is the RPC's terminal frame; the actual
// devices flow as adapter.event pushes with Kind = commissionable_found.
// A plugin that ran the scan to completion returns Ended=true so a
// caller can distinguish "window elapsed" from "canceled".
type DiscoverCommissionableResult struct {
	Ended bool `json:"ended"`
}

// StartStreamParams asks the plugin to open a camera session against
// the viewer's SDP offer.
type StartStreamParams struct {
	Ref domain.TransportRef `json:"ref"`
	SDP string              `json:"sdp"`
}

// StartStreamResult carries the session id the camera allocated.
type StartStreamResult struct {
	SessionID int `json:"sessionId"`
}

// AddCandidatesParams forwards viewer ICE candidates to the camera.
type AddCandidatesParams struct {
	SessionID  int      `json:"sessionId"`
	Candidates []string `json:"candidates"`
}

// StopStreamParams ends a session.
type StopStreamParams struct {
	SessionID int `json:"sessionId"`
}

// CameraSignalPayload mirrors ports.CameraSignal for the wire. Kind is
// answer / offer / ice / end; the rest of the fields are populated
// per kind so a caller can decode without an extra RPC.
type CameraSignalPayload struct {
	Kind       string   `json:"kind"`
	SessionID  int      `json:"sessionId"`
	SDP        string   `json:"sdp,omitempty"`
	Candidates []string `json:"candidates,omitempty"`
	Reason     string   `json:"reason,omitempty"`
}

// CommissionableFoundPayload is what the plugin puts in EventPayload
// when Kind = commissionable_found. Mirrors ports.CommissionableDevice
// one-for-one so the bridge can translate without loss.
type CommissionableFoundPayload struct {
	ScanID        string            `json:"scanId"`
	Ref           string            `json:"ref"`
	Name          string            `json:"name,omitempty"`
	Type          domain.DeviceType `json:"type,omitempty"`
	VendorID      int               `json:"vendorId,omitempty"`
	ProductID     int               `json:"productId,omitempty"`
	Discriminator int               `json:"discriminator,omitempty"`
}

// MethodDiscoverCommissionable is optional. A plugin whose transport
// scans for devices ready to pair (Matter, Zigbee via
// permit-join) implements it; anything else returns unsupported. The
// scan runs for the caller's window and streams finds onto
// adapter.event with Kind = commissionable_found until the response
// arrives.
const MethodDiscoverCommissionable = "adapter.discoverCommissionable"

// MethodCameraStart / MethodCameraAddCandidates / MethodCameraStop are
// the three CameraStreamer entry points. A plugin whose transport
// brokers WebRTC (Matter cameras, for now) answers these; anything
// else returns unsupported. Signalling frames from the camera flow
// back on adapter.event as Kind = camera_signal.
const (
	MethodCameraStart         = "adapter.camera.start"
	MethodCameraAddCandidates = "adapter.camera.addCandidates"
	MethodCameraStop          = "adapter.camera.stop"
)

// MethodConfigFlow drives Layer 2 setup wizards. The plugin returns
// the next step to render given the current step id and the data
// the user submitted. The core is a thin proxy — session state
// lives in the plugin.
const MethodConfigFlow = "adapter.configFlow"

// ConfigFlowRequest is what /plugins/{name}/flow POSTs. Step is the
// id the plugin sent last (or "init" on the first call); Data is
// whatever the user submitted on that step's UI.
type ConfigFlowRequest struct {
	Step string         `json:"step"`
	Data map[string]any `json:"data,omitempty"`
}

// ConfigFlowStep is the plugin's reply. Type dispatches the UI:
// info / form / oauth / qr-scan / progress / confirm / pick-device /
// manual-action / error / complete. Not every field is used by every
// type — the plugin sets what applies.
type ConfigFlowStep struct {
	// Type controls the renderer. Must be one of the step types
	// listed in docs/plugin-ui-integration.md §4.3.
	Type string `json:"type"`

	// ID is what the client sends back as ConfigFlowRequest.Step
	// on the next call. Empty when the flow has terminated.
	ID string `json:"id,omitempty"`

	// Next is the id the flow moves to when the user commits this
	// step. Set on form / confirm / manual-action / qr-scan / pick-device.
	Next string `json:"next,omitempty"`

	// Human-facing title and body. Always safe to set.
	Title string `json:"title,omitempty"`
	Body  string `json:"body,omitempty"`

	// form: JSON Schema for the fields; the client renders with the
	// same SchemaForm the plugin config uses.
	Schema string `json:"schema,omitempty"`

	// oauth: browser opens AuthURL; RedirectURI is where the
	// provider redirects. The client captures the code from the
	// redirect and posts it back as {code: "..."} for Next.
	AuthURL     string `json:"authUrl,omitempty"`
	RedirectURI string `json:"redirectUri,omitempty"`
	Provider    string `json:"provider,omitempty"`

	// qr-scan: an optional hint for the client which QR format to
	// expect (e.g. "matter" for MT: strings). Empty means any.
	QRHint string `json:"qrHint,omitempty"`

	// progress: 0..1 progress bar with a message. The client polls
	// (or subscribes, when the push topic lands in a follow-on) to
	// refresh.
	Progress float64 `json:"progress,omitempty"`

	// confirm: a plain accept/decline. Next fires on accept; Cancel
	// (optional) fires on decline.
	Cancel string `json:"cancel,omitempty"`

	// pick-device: list of candidates the user picks from. Each
	// entry's value is what lands in data[Field] for the Next call.
	Field   string          `json:"field,omitempty"`
	Options []ConfigFlowOpt `json:"options,omitempty"`

	// manual-action: instruction plus a "Готово" button that
	// commits to Next.
	Instruction string `json:"instruction,omitempty"`

	// error: red icon, message, optional Retry step id.
	Message string `json:"message,omitempty"`
	Retry   string `json:"retry,omitempty"`

	// complete: message shown to the user before the wizard closes.
}

// ConfigFlowOpt is one entry in a pick-device or select field.
type ConfigFlowOpt struct {
	Label       string `json:"label"`
	Value       string `json:"value"`
	Description string `json:"description,omitempty"`
}

// KindCameraSignal is a bridge-only event kind for one WebRTC
// signalling frame — the camera's SDP answer, its ICE candidates, or
// an "end" marker with a reason.
const KindCameraSignal ports.TransportEventKind = "camera_signal"

// KindCommissionableFound is a bridge-only event kind carrying a
// device advertisement back to a discoverCommissionable caller. The
// scan id is set so multiple concurrent scans stay separate.
const KindCommissionableFound ports.TransportEventKind = "commissionable_found"

// KindCommissionProgress is a bridge-only event kind that carries
// pairing progress back to the caller. It sits alongside the
// ports.TransportEventKind values on the same topic because it uses the
// same transport, but the bridge routes it to a per-call progress
// channel instead of the ports.TransportEvent stream.
const KindCommissionProgress ports.TransportEventKind = "commission_progress"

// EventPayload is what the plugin puts on the adapter.event topic. Each
// field maps to ports.TransportEvent so the bridge can translate with no
// loss. Kind is the primary switch: state_changed, event_fired,
// online, offline, added, removed, adapter_status, or the bridge-only
// commission_progress (see KindCommissionProgress).
//
// CommissionID / Stage / Message are set only for commission_progress
// events; every other kind leaves them empty.
type EventPayload struct {
	Ref     domain.TransportRef      `json:"ref,omitempty"`
	Kind    ports.TransportEventKind `json:"kind"`
	Feature domain.FeatureKey        `json:"feature,omitempty"`
	Key     string                   `json:"key,omitempty"`
	Value   any                      `json:"value,omitempty"`

	CommissionID string `json:"commissionId,omitempty"`
	Stage        string `json:"stage,omitempty"`
	Message      string `json:"message,omitempty"`

	// Set only for commissionable_found. Kept as its own field so a
	// plugin can push finds without borrowing Value's slot.
	Found *CommissionableFoundPayload `json:"found,omitempty"`

	// Set only for camera_signal.
	Signal *CameraSignalPayload `json:"signal,omitempty"`
}
