package matter

import "encoding/json"

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
)

// Server-pushed event names.
const (
	EventAttributeChanged     = "attributeChanged"
	EventNodeOnline           = "nodeOnline"
	EventNodeOffline          = "nodeOffline"
	EventCommissioningStage   = "commissioningProgress"
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
}

// RPCError is the standard error object.
type RPCError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func (e *RPCError) Error() string {
	if e == nil {
		return ""
	}
	return e.Message
}

// --- Method params / results ---

// CommissionParams carries the 11-digit setup code the user obtained from
// Apple Home's "Turn on Pairing Mode" screen.
type CommissionParams struct {
	SetupCode string `json:"setupCode"`
}

// CommissionResult is what the sidecar reports after joining the fabric.
type CommissionResult struct {
	NodeID       string `json:"nodeId"`
	FabricIndex  int    `json:"fabricIndex"`
}

// Node is a single Matter node reported by listNodes. Endpoints and clusters
// are what the engine maps into keystone Features via mapping.go.
type Node struct {
	NodeID       string     `json:"nodeId"`
	VendorName   string     `json:"vendorName,omitempty"`
	ProductName  string     `json:"productName,omitempty"`
	VendorID     int        `json:"vendorId,omitempty"`
	ProductID    int        `json:"productId,omitempty"`
	Online       bool       `json:"online"`
	Endpoints    []Endpoint `json:"endpoints"`
}

// Endpoint describes one addressable endpoint on a node.
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

// NodeLifecycle is the payload of nodeOnline / nodeOffline.
type NodeLifecycle struct {
	NodeID string `json:"nodeId"`
}

// CommissioningProgress is emitted while a commission() call runs.
type CommissioningProgress struct {
	Stage   string `json:"stage"`
	Message string `json:"message,omitempty"`
}
