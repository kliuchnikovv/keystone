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
    error: RpcErrorBody;
}

export interface ServerEvent {
    event: string;
    data: unknown;
    /**
     * Monotonic per-process sequence number, starting at 1. Lets a client
     * detect loss and ask for a replay after a reconnect.
     *
     * The counter resets when the sidecar restarts, so a client must treat a
     * `seq` lower than what it has already seen as "the server restarted",
     * not as a duplicate — see SubscribeResult.seq.
     */
    seq: number;
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
    Subscribe: "subscribe",
    DiscoverCommissionable: "discoverCommissionable",
    WebrtcOffer: "webrtcOffer",
    WebrtcIce: "webrtcIce",
    WebrtcStop: "webrtcStop",
} as const;

// --- event names ---
export const Events = {
    AttributeChanged: "attributeChanged",
    NodeOnline: "nodeOnline",
    NodeOffline: "nodeOffline",
    CommissioningProgress: "commissioningProgress",
    CommissionableFound: "commissionableFound",
    DeviceEvent: "deviceEvent",
    WebrtcSignal: "webrtcSignal",
} as const;

// --- method payloads ---

export interface CommissionParams {
    setupCode: string;
    /**
     * Ref of a device previously reported by discoverCommissionable. When set,
     * commissioning targets exactly that device and only the passcode is taken
     * from `setupCode`.
     *
     * Discovery never removes the need for a setup code: the passcode is not
     * advertised, so PASE cannot start without it. Targeting only saves the
     * user from disambiguating identical devices.
     */
    target?: string;
}

export interface DiscoverCommissionableParams {
    /** How long to listen for advertisements. Defaults to 10s, capped at 60s. */
    timeoutMs?: number;
}

/**
 * A device advertising itself as ready to be commissioned. Everything here
 * comes from the mDNS/BLE advertisement — the device has not been interviewed,
 * so there are no clusters or features yet.
 */
export interface CommissionableDevice {
    /** Stable handle for this advertisement; pass back as CommissionParams.target. */
    ref: string;
    /** Advertised device name, if the vendor set one. */
    name?: string;
    /** Resolved CSA device-type name ("OnOffLight"), when advertised and known. */
    deviceType?: string;
    vendorId?: number;
    productId?: number;
    discriminator?: number;
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
    /** User-assigned name from the ecosystem that commissioned the device. */
    nodeLabel?: string;
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

/**
 * How the node actually left.
 *
 * `decommissioned` — the device dropped our fabric and no longer lists us among
 * its connected services. `forced` — the device was unreachable, so it was only
 * forgotten locally and still carries our fabric; the user has to factory-reset
 * it to clean that up.
 */
export interface RemoveNodeResult {
    removed: "decommissioned" | "forced";
    message?: string;
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

/**
 * A Matter event (as opposed to an attribute change): a button press, an alarm
 * firing, a wash cycle finishing. Carries the cluster's own payload untouched.
 */
export interface DeviceEvent {
    nodeId: string;
    endpointId: number;
    cluster: string;
    event: string;
    data: unknown;
}

export interface SubscribeParams {
    /**
     * Highest seq the client has already processed. Omit (or 0) on a first
     * connect to ask for a full snapshot instead of a replay.
     */
    sinceSeq?: number;
}

export interface SubscribeResult {
    /**
     * The server's sequence position *before* this call did anything.
     *
     * A client must clamp its own counter to this value: if it is lower than
     * what the client remembers, the sidecar process restarted and its
     * numbering began again. Without the clamp the client would mistake every
     * new event for a duplicate and go silent.
     */
    seq: number;
    /** How many buffered events were resent on this socket. */
    replayed: number;
    /**
     * True when the requested position had already fallen out of the ring
     * buffer (or no position was given). A full snapshot was published
     * instead — the events follow this response's own frame order.
     */
    gap: boolean;
}

/**
 * Signalling relayed from a camera. Matter carries only this handshake — the
 * video itself flows over a WebRTC peer connection between the camera and
 * whoever holds the browser session, never through this sidecar.
 */
export type WebRtcSignal =
    | { kind: "offer"; sessionId: number; sdp: string }
    | { kind: "answer"; sessionId: number; sdp: string }
    | { kind: "ice"; sessionId: number; candidates: string[] }
    | { kind: "end"; sessionId: number; reason?: string };

export interface WebrtcOfferParams {
    nodeId: string;
    endpointId: number;
    /** SDP offer produced by the viewer's RTCPeerConnection. */
    sdp: string;
    /** Existing stream ids to reuse; omit to let the camera allocate. */
    videoStreamId?: number;
    audioStreamId?: number;
}

export interface WebrtcOfferResult {
    sessionId: number;
    videoStreamId?: number;
    audioStreamId?: number;
}

export interface WebrtcIceParams {
    sessionId: number;
    candidates: string[];
}

export interface WebrtcStopParams {
    sessionId: number;
}

export interface CommissioningProgress {
    stage: string;
    message?: string;
}

// --- errors ---
export const RpcErrorCode = {
    BadRequest: -32602,
    MethodNotFound: -32601,
    Internal: -32603,
    NotFound: -32001,
    NotReady: -32002,
    Unreachable: -32003,
    Timeout: -32004,
    Unsupported: -32005,
} as const;

/**
 * Error category. Keystone branches on this instead of matching message text,
 * so the wording of an error stays a presentation detail.
 */
export type ErrorKind =
    | "bad_request"
    | "not_found"
    | "not_ready"
    | "unreachable"
    | "timeout"
    | "unsupported"
    | "internal";

export interface RpcErrorBody {
    code: number;
    message: string;
    kind: ErrorKind;
    /** True when repeating the identical call could plausibly succeed. */
    retryable: boolean;
}

const ERROR_SHAPE: Record<ErrorKind, { code: number; retryable: boolean }> = {
    bad_request: { code: RpcErrorCode.BadRequest, retryable: false },
    not_found: { code: RpcErrorCode.NotFound, retryable: false },
    not_ready: { code: RpcErrorCode.NotReady, retryable: true },
    unreachable: { code: RpcErrorCode.Unreachable, retryable: true },
    timeout: { code: RpcErrorCode.Timeout, retryable: true },
    unsupported: { code: RpcErrorCode.Unsupported, retryable: false },
    internal: { code: RpcErrorCode.Internal, retryable: false },
};

/**
 * An error carrying its taxonomy. Throw this from controller methods; the WS
 * dispatcher turns it into the wire error body verbatim.
 */
export class RpcError extends Error {
    readonly kind: ErrorKind;
    readonly code: number;
    readonly retryable: boolean;

    constructor(kind: ErrorKind, message: string, opts?: { retryable?: boolean; cause?: unknown }) {
        super(message, opts?.cause !== undefined ? { cause: opts.cause } : undefined);
        this.name = "RpcError";
        this.kind = kind;
        this.code = ERROR_SHAPE[kind].code;
        this.retryable = opts?.retryable ?? ERROR_SHAPE[kind].retryable;
    }

    body(): RpcErrorBody {
        return { code: this.code, message: this.message, kind: this.kind, retryable: this.retryable };
    }
}

/**
 * Classify an arbitrary thrown value. Anything that isn't already an RpcError
 * came from matter.js or the runtime, so we inspect it for the failure modes
 * worth retrying — an unreachable or sleeping device is by far the most common,
 * and reporting it as "internal" would make keystone give up on a device that
 * is merely asleep.
 */
export function toRpcErrorBody(err: unknown): RpcErrorBody {
    if (err instanceof RpcError) return err.body();

    const message = err instanceof Error ? err.message : String(err);
    const name = err instanceof Error ? err.name : "";
    const haystack = `${name} ${message}`.toLowerCase();

    let kind: ErrorKind = "internal";
    if (haystack.includes("timeout") || haystack.includes("timed out")) {
        kind = "timeout";
    } else if (
        haystack.includes("unreachable") ||
        haystack.includes("no route") ||
        haystack.includes("channel") ||
        haystack.includes("disconnect") ||
        haystack.includes("session")
    ) {
        kind = "unreachable";
    }

    const shape = ERROR_SHAPE[kind];
    return { code: shape.code, message, kind, retryable: shape.retryable };
}
