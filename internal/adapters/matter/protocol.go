package matter

import (
	"encoding/json"
	"errors"
	"fmt"
)

// The matter.js sidecar speaks a small JSON-RPC dialect on the WebSocket:
//
//   client -> server:  {"id":"...", "method":"...", "params":{...}}
//   server -> client:  {"id":"...", "result":{...}}
//                 or:  {"id":"...", "error":{"code":..., "message":"..."}}
//                 or:  {"event":"...", "data":{...}}   (server-pushed)
//
// This file declares the wire types. Both ends must stay compatible.

// Method names. Kept as string constants so callers get compiler help.
const (
	MethodCommission     = "commission"
	MethodListNodes      = "listNodes"
	MethodReadAttribute  = "readAttribute"
	MethodWriteAttribute = "writeAttribute"
	MethodInvokeCommand  = "invokeCommand"
	MethodRemoveNode     = "removeNode"
	MethodSubscribe      = "subscribe"

	// MethodDiscoverCommissionable scans for devices advertising themselves as
	// ready to pair. Blocks for the requested window.
	MethodDiscoverCommissionable = "discoverCommissionable"
)

// Server-pushed event names.
const (
	EventAttributeChanged    = "attributeChanged"
	EventNodeOnline          = "nodeOnline"
	EventNodeOffline         = "nodeOffline"
	EventCommissioningStage  = "commissioningProgress"
	EventCommissionableFound = "commissionableFound"
	EventDeviceEvent         = "deviceEvent"
)

// Request is the client-to-server frame.
type Request struct {
	ID     string          `json:"id"`
	Method string          `json:"method"`
	Params json.RawMessage `json:"params,omitempty"`
}

// Response is the server-to-client reply frame. Exactly one of Result / Error
// is populated. Event is set only for server-pushed messages (no id, no
// method, no result — Event and Data instead).
type Response struct {
	ID     string          `json:"id,omitempty"`
	Result json.RawMessage `json:"result,omitempty"`
	Error  *RPCError       `json:"error,omitempty"`

	Event string          `json:"event,omitempty"`
	Data  json.RawMessage `json:"data,omitempty"`
	// Seq numbers server-pushed events within one sidecar process, starting
	// at 1. Zero means the sidecar predates sequencing.
	Seq int64 `json:"seq,omitempty"`
}

// SubscribeParams asks the sidecar to resync this connection.
type SubscribeParams struct {
	// SinceSeq is the highest seq we have already processed. Zero asks for a
	// full snapshot rather than a replay.
	SinceSeq int64 `json:"sinceSeq,omitempty"`
}

// SubscribeResult reports how the sidecar resynced us.
type SubscribeResult struct {
	// Seq is the sidecar's position before it handled our subscribe. We clamp
	// our own counter to it: a value below what we remember means the sidecar
	// process restarted and began numbering again, and without the clamp every
	// subsequent event would look like a duplicate and be dropped.
	Seq int64 `json:"seq"`
	// Replayed counts events resent to us from the sidecar's ring buffer.
	Replayed int `json:"replayed"`
	// Gap is true when the missed range was no longer buffered (or we had no
	// position) and the sidecar published a full snapshot instead.
	Gap bool `json:"gap"`
}

// ErrorKind classifies an RPC failure so callers can branch on the category
// instead of pattern-matching the message text.
type ErrorKind string

const (
	ErrKindBadRequest  ErrorKind = "bad_request" // caller sent something invalid; retrying won't help
	ErrKindNotFound    ErrorKind = "not_found"   // node/endpoint/attribute isn't there
	ErrKindNotReady    ErrorKind = "not_ready"   // controller still starting up
	ErrKindUnreachable ErrorKind = "unreachable" // peer didn't answer (asleep, off-network)
	ErrKindTimeout     ErrorKind = "timeout"     // operation exceeded its deadline
	ErrKindUnsupported ErrorKind = "unsupported" // device lacks this cluster/command
	ErrKindInternal    ErrorKind = "internal"    // sidecar bug or matter.js failure
)

// Sentinel errors for errors.Is. RPCError reports itself as matching the
// sentinel for its kind, so callers write:
//
//	if errors.Is(err, matter.ErrNotFound) { ... }
//
// instead of scanning the message for "not found".
var (
	ErrBadRequest  = errors.New("matter: bad request")
	ErrNotFound    = errors.New("matter: not found")
	ErrNotReady    = errors.New("matter: sidecar not ready")
	ErrUnreachable = errors.New("matter: device unreachable")
	ErrTimeout     = errors.New("matter: timeout")
	ErrUnsupported = errors.New("matter: unsupported operation")
	ErrInternal    = errors.New("matter: internal sidecar error")
)

var sentinelByKind = map[ErrorKind]error{
	ErrKindBadRequest:  ErrBadRequest,
	ErrKindNotFound:    ErrNotFound,
	ErrKindNotReady:    ErrNotReady,
	ErrKindUnreachable: ErrUnreachable,
	ErrKindTimeout:     ErrTimeout,
	ErrKindUnsupported: ErrUnsupported,
	ErrKindInternal:    ErrInternal,
}

// kindByCode derives a kind from the numeric code, for sidecars older than the
// kind field (and for the JSON-RPC codes that have a fixed meaning).
var kindByCode = map[int]ErrorKind{
	RPCCodeBadRequest:    ErrKindBadRequest,
	RPCCodeMethodMissing: ErrKindUnsupported,
	RPCCodeInternal:      ErrKindInternal,
	RPCCodeNotFound:      ErrKindNotFound,
	RPCCodeNotReady:      ErrKindNotReady,
	RPCCodeUnreachable:   ErrKindUnreachable,
	RPCCodeTimeout:       ErrKindTimeout,
}

// Numeric codes on the wire. The -32xxx range below -32000 is reserved by
// JSON-RPC for implementation-defined server errors.
const (
	RPCCodeBadRequest    = -32602
	RPCCodeMethodMissing = -32601
	RPCCodeInternal      = -32603
	RPCCodeNotFound      = -32001
	RPCCodeNotReady      = -32002
	RPCCodeUnreachable   = -32003
	RPCCodeTimeout       = -32004
	RPCCodeUnsupported   = -32005
)

// RPCError is the standard error object.
type RPCError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
	// Kind and Retryable are set by sidecars that speak the taxonomy. Older
	// ones omit them, so read via Kind() / IsRetryable() rather than directly.
	ErrKind   ErrorKind `json:"kind,omitempty"`
	Retryable bool      `json:"retryable,omitempty"`
}

func (e *RPCError) Error() string {
	if e == nil {
		return ""
	}
	if kind := e.Kind(); kind != "" {
		return fmt.Sprintf("%s (%s)", e.Message, kind)
	}
	return e.Message
}

// Kind returns the declared kind, falling back to one derived from the numeric
// code so a sidecar that predates the field still classifies correctly.
func (e *RPCError) Kind() ErrorKind {
	if e == nil {
		return ""
	}
	if e.ErrKind != "" {
		return e.ErrKind
	}
	return kindByCode[e.Code]
}

// IsRetryable reports whether repeating the call could plausibly succeed. The
// sidecar's explicit flag wins; otherwise the kind decides — a sleeping device
// or a controller that is still booting will answer later, a malformed request
// never will.
func (e *RPCError) IsRetryable() bool {
	if e == nil {
		return false
	}
	if e.Retryable {
		return true
	}
	switch e.Kind() {
	case ErrKindNotReady, ErrKindUnreachable, ErrKindTimeout:
		return true
	default:
		return false
	}
}

// Is makes errors.Is(err, ErrNotFound) work through wrapping.
func (e *RPCError) Is(target error) bool {
	if e == nil {
		return false
	}
	return sentinelByKind[e.Kind()] == target
}

// IsRetryable reports whether an error anywhere in the chain came back from the
// sidecar marked as worth retrying. Transport-level failures count too: a call
// that never reached a connected sidecar can be retried once it is back.
func IsRetryable(err error) bool {
	var rpcErr *RPCError
	if errors.As(err, &rpcErr) {
		return rpcErr.IsRetryable()
	}
	return errors.Is(err, ErrSidecarUnavailable) || errors.Is(err, ErrNotConnected)
}

// KindOf extracts the taxonomy kind from anywhere in an error chain, or "" if
// the error didn't originate from the sidecar.
func KindOf(err error) ErrorKind {
	var rpcErr *RPCError
	if errors.As(err, &rpcErr) {
		return rpcErr.Kind()
	}
	return ""
}

// --- Method params / results ---

// CommissionParams carries the setup code the user read off the device (manual
// 11/21-digit code or an "MT:" QR payload) and, optionally, which discovered
// device to pair.
type CommissionParams struct {
	SetupCode string `json:"setupCode"`
	// Target is a ref from a previous discoverCommissionable scan. Empty means
	// "find the device from the setup code", which is what a user typing a code
	// with no prior scan gets.
	Target string `json:"target,omitempty"`
}

// DiscoverCommissionableParams bounds a scan.
type DiscoverCommissionableParams struct {
	TimeoutMs int64 `json:"timeoutMs,omitempty"`
}

// CommissionableDevice is one advertisement reported by a scan. Also the
// payload of the commissionableFound event, pushed while the scan is running.
type CommissionableDevice struct {
	Ref           string `json:"ref"`
	Name          string `json:"name,omitempty"`
	DeviceType    string `json:"deviceType,omitempty"`
	VendorID      int    `json:"vendorId,omitempty"`
	ProductID     int    `json:"productId,omitempty"`
	Discriminator int    `json:"discriminator,omitempty"`
}

// CommissionResult is what the sidecar reports after joining the fabric.
type CommissionResult struct {
	NodeID      string `json:"nodeId"`
	FabricIndex int    `json:"fabricIndex"`
}

// Node is a single Matter node reported by listNodes. Endpoints and clusters
// are what the engine maps into keystone Features via mapping.go.
type Node struct {
	NodeID      string `json:"nodeId"`
	VendorName  string `json:"vendorName,omitempty"`
	ProductName string `json:"productName,omitempty"`
	VendorID    int    `json:"vendorId,omitempty"`
	ProductID   int    `json:"productId,omitempty"`
	// NodeLabel is BasicInformation.NodeLabel — the name the user gave the
	// device in whichever ecosystem commissioned it first (Apple Home, Google
	// Home). Usually friendlier than VendorName + ProductName.
	NodeLabel string     `json:"nodeLabel,omitempty"`
	Online    bool       `json:"online"`
	Endpoints []Endpoint `json:"endpoints"`
}

// Endpoint describes one addressable endpoint on a node. DeviceType is the
// canonical CSA device-type name the sidecar resolved from
// Descriptor.DeviceTypeList ("OnOffLight", "ColorTemperatureLight", …) — empty
// if the endpoint hasn't been interviewed or declares a type we don't know.
type Endpoint struct {
	EndpointID int      `json:"endpointId"`
	DeviceType string   `json:"deviceType,omitempty"`
	Clusters   []string `json:"clusters"`
}

// AttrRef locates a single attribute for read/write operations.
type AttrRef struct {
	NodeID     string `json:"nodeId"`
	EndpointID int    `json:"endpointId"`
	Cluster    string `json:"cluster"`
	Attribute  string `json:"attribute"`
}

// WriteAttrParams is AttrRef plus the desired value.
type WriteAttrParams struct {
	AttrRef
	Value any `json:"value"`
}

// InvokeParams targets a cluster command on a specific endpoint.
type InvokeParams struct {
	NodeID     string         `json:"nodeId"`
	EndpointID int            `json:"endpointId"`
	Cluster    string         `json:"cluster"`
	Command    string         `json:"command"`
	Args       map[string]any `json:"args,omitempty"`
}

// RemoveNodeParams identifies a node to unpair from the local fabric.
type RemoveNodeParams struct {
	NodeID string `json:"nodeId"`
}

// --- Event payloads ---

// AttributeChanged is the payload of the attributeChanged server event.
type AttributeChanged struct {
	NodeID     string `json:"nodeId"`
	EndpointID int    `json:"endpointId"`
	Cluster    string `json:"cluster"`
	Attribute  string `json:"attribute"`
	Value      any    `json:"value"`
}

// DeviceEvent is the payload of the deviceEvent server event: a Matter event
// rather than an attribute change (button press, alarm, cycle completion).
type DeviceEvent struct {
	NodeID     string `json:"nodeId"`
	EndpointID int    `json:"endpointId"`
	Cluster    string `json:"cluster"`
	Event      string `json:"event"`
	Data       any    `json:"data,omitempty"`
}

// NodeLifecycle is the payload of nodeOnline / nodeOffline.
type NodeLifecycle struct {
	NodeID string `json:"nodeId"`
}

// CommissioningProgress is emitted while a commission() call runs.
type CommissioningProgress struct {
	Stage   string `json:"stage"`
	Message string `json:"message,omitempty"`
}
