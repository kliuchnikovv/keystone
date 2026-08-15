// WebRTC signalling for Matter 1.4 cameras.
//
// Matter carries only the *signalling* for camera video: SDP offers/answers and
// ICE candidates travel as cluster commands, while the media itself flows over
// an ordinary WebRTC peer connection that Matter never touches.
//
// So this sidecar is a signalling broker, not a media endpoint. It never
// decodes a frame: the browser's RTCPeerConnection talks directly to the
// camera, which keeps a whole WebRTC stack (ICE, DTLS-SRTP, codecs) out of
// Node. Both are on the same LAN, which is exactly the case ICE handles best.
//
// Two clusters are involved, in opposite directions:
//   - WebRtcTransportProvider (0x0553) lives on the camera. We invoke
//     ProvideOffer / ProvideIceCandidates / EndSession on it.
//   - WebRtcTransportRequestor (0x0554) lives on us. The camera invokes
//     Answer / IceCandidates / End on it, which is why the controller has to
//     host it as a server cluster.
//
// matter.js 0.17 ships no behavior for either, but it does ship the cluster
// definitions, so the requestor behavior is built from its definition below.

import { EventEmitter } from "node:events";
import { ClusterBehavior } from "@matter/node";
import { WebRtcTransportRequestor } from "@matter/types/clusters/web-rtc-transport-requestor";
import { RpcError } from "./protocol.js";
import type { WebRtcSignal } from "./protocol.js";

const RequestorBase = ClusterBehavior.for(WebRtcTransportRequestor.Cluster);

// signalSink is process-wide because matter.js instantiates behaviors itself
// and gives us no place to thread state through. There is one controller per
// sidecar, so one sink is enough.
let signalSink: ((signal: WebRtcSignal) => void) | undefined;

export function setSignalSink(sink: (signal: WebRtcSignal) => void): void {
    signalSink = sink;
}

function emitSignal(signal: WebRtcSignal): void {
    signalSink?.(signal);
}

/**
 * The requestor cluster the camera calls back into. Every handler does the same
 * thing: hand the payload to whoever is watching this session, so it can reach
 * the browser that owns the peer connection.
 */
export class WebRtcRequestorBehavior extends RequestorBase {
    // The camera offers a stream (used when we solicit rather than offer).
    offer(request: { webRtcSessionId: number; sdp: string }) {
        emitSignal({
            kind: "offer",
            sessionId: request.webRtcSessionId,
            sdp: request.sdp,
        });
    }

    // The camera answers the offer we sent.
    answer(request: { webRtcSessionId: number; sdp: string }) {
        emitSignal({
            kind: "answer",
            sessionId: request.webRtcSessionId,
            sdp: request.sdp,
        });
    }

    iceCandidates(request: { webRtcSessionId: number; iceCandidates: Array<{ candidate: string }> }) {
        emitSignal({
            kind: "ice",
            sessionId: request.webRtcSessionId,
            candidates: (request.iceCandidates ?? []).map((c) => c.candidate ?? String(c)),
        });
    }

    end(request: { webRtcSessionId: number; reason?: number }) {
        emitSignal({
            kind: "end",
            sessionId: request.webRtcSessionId,
            reason: reasonLabel(request.reason),
        });
    }
}

// Reason codes from the spec's WebRTCEndReasonEnum. Anything unrecognised is
// reported numerically rather than flattened to "unknown", so a spec addition
// stays visible.
const END_REASONS: Record<number, string> = {
    0: "ice_failed",
    1: "ice_timeout",
    2: "user_hangup",
    3: "user_busy",
    4: "replaced",
    5: "no_user_media",
    6: "invalid_sdp",
    7: "internal_error",
};

function reasonLabel(reason?: number): string | undefined {
    if (reason === undefined) return undefined;
    return END_REASONS[reason] ?? String(reason);
}

/**
 * Command names on the camera's provider cluster, in the camelCase matter.js
 * uses for behavior properties.
 */
export const ProviderCommands = {
    SolicitOffer: "solicitOffer",
    ProvideOffer: "provideOffer",
    ProvideAnswer: "provideAnswer",
    ProvideIceCandidates: "provideIceCandidates",
    EndSession: "endSession",
} as const;

export const ProviderCluster = "webRtcTransportProvider";

/**
 * Tracks which node each session belongs to, so ICE candidates and teardown
 * reach the right camera. Sessions are short-lived; a stale entry costs
 * nothing but is cleaned up on end.
 */
export class SessionRegistry {
    #byId = new Map<number, { nodeId: string; endpointId: number }>();

    remember(sessionId: number, nodeId: string, endpointId: number): void {
        this.#byId.set(sessionId, { nodeId, endpointId });
    }

    lookup(sessionId: number): { nodeId: string; endpointId: number } {
        const entry = this.#byId.get(sessionId);
        if (entry === undefined) {
            throw new RpcError("not_found", `matter: webrtc session ${sessionId} is not active`);
        }
        return entry;
    }

    forget(sessionId: number): void {
        this.#byId.delete(sessionId);
    }
}

/**
 * emitterBridge wires the module-level sink to a controller's EventEmitter.
 * Kept here so controller.ts does not have to know about the sink at all.
 */
export function bindSignals(emitter: EventEmitter, sessions: SessionRegistry): void {
    setSignalSink((signal) => {
        if (signal.kind === "end") {
            sessions.forget(signal.sessionId);
        }
        emitter.emit("webrtcSignal", signal);
    });
}
