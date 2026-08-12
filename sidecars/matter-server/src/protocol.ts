// Wire protocol between the Go core (keystone) and this sidecar.
// Must stay byte-compatible with internal/adapters/matter/protocol.go.

export interface RpcRequest {
    id: string;
    method: string;
    params?: unknown;
}

export interface RpcSuccess {
    id: string;
    result: unknown;
}

export interface RpcFailure {
    id: string;
    error: { code: number; message: string };
}

export interface ServerEvent {
    event: string;
    data: unknown;
}

export type RpcResponse = RpcSuccess | RpcFailure;
export type OutboundFrame = RpcResponse | ServerEvent;

// --- method names ---
export const Methods = {
    Commission: "commission",
    ListNodes: "listNodes",
    ReadAttribute: "readAttribute",
    WriteAttribute: "writeAttribute",
    InvokeCommand: "invokeCommand",
    RemoveNode: "removeNode",
} as const;

// --- event names ---
export const Events = {
    AttributeChanged: "attributeChanged",
    NodeOnline: "nodeOnline",
    NodeOffline: "nodeOffline",
    CommissioningProgress: "commissioningProgress",
} as const;

// --- method payloads ---

export interface CommissionParams {
    setupCode: string;
}

export interface CommissionResult {
    nodeId: string;
    fabricIndex: number;
}

export interface Node {
    nodeId: string;
    vendorName?: string;
    productName?: string;
    vendorId?: number;
    productId?: number;
    online: boolean;
    endpoints: Endpoint[];
}

export interface Endpoint {
    endpointId: number;
    deviceType?: string;
    clusters: string[];
}

export interface AttrRef {
    nodeId: string;
    endpointId: number;
    cluster: string;
    attribute: string;
}

export interface WriteAttrParams extends AttrRef {
    value: unknown;
}

export interface InvokeParams {
    nodeId: string;
    endpointId: number;
    cluster: string;
    command: string;
    args?: Record<string, unknown>;
}

export interface RemoveNodeParams {
    nodeId: string;
}

// --- event payloads ---

export interface AttributeChanged {
    nodeId: string;
    endpointId: number;
    cluster: string;
    attribute: string;
    value: unknown;
}

export interface NodeLifecycle {
    nodeId: string;
}

export interface CommissioningProgress {
    stage: string;
    message?: string;
}

// --- error codes ---
export const RpcErrorCode = {
    BadRequest: -32602,
    MethodNotFound: -32601,
    Internal: -32603,
    NotFound: -32001,
    NotReady: -32002,
} as const;
