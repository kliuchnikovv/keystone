// Small WebSocket JSON-RPC server. One handler per method; server-pushed
// events are broadcast to every connected client.

import { WebSocketServer, type WebSocket } from "ws";
import {
    Events,
    Methods,
    RpcErrorCode,
    toRpcErrorBody,
    type OutboundFrame,
    type RpcRequest,
    type ServerEvent,
    type SubscribeParams,
    type SubscribeResult,
} from "./protocol.js";
import type { MatterController } from "./controller.js";

export interface ServerOptions {
    host: string;
    port: number;
    controller: MatterController;
    log: (level: "info" | "warn" | "error", msg: string, meta?: Record<string, unknown>) => void;
}

// How many recent events stay replayable. A colour animation from the Home app
// can produce a few hundred coalesced updates a minute, so this covers roughly
// a minute of heavy traffic — long enough for a reconnect, short enough to stay
// bounded.
const EVENT_BUFFER_SIZE = 512;

export function startWsServer(opts: ServerOptions): { close: () => Promise<void> } {
    const wss = new WebSocketServer({ host: opts.host, port: opts.port });
    const clients = new Set<WebSocket>();

    // Ring buffer of recently broadcast events, oldest first. A client that
    // reconnects within this window gets the exact events it missed instead of
    // a full snapshot — and, more importantly, instead of silently missing
    // them, which is what happened before seq existed.
    const buffer: ServerEvent[] = [];
    let lastSeq = 0;

    const record = (event: string, data: unknown): ServerEvent => {
        const frame: ServerEvent = { event, data, seq: ++lastSeq };
        buffer.push(frame);
        if (buffer.length > EVENT_BUFFER_SIZE) buffer.shift();
        return frame;
    };

    // subscribe re-syncs a client after (re)connect. It runs to completion
    // synchronously: Node's single-threaded event loop guarantees no live event
    // can be broadcast between the replay writes and the client going live, so
    // there is no window in which an event is lost or reordered.
    const subscribe = (params: unknown, ws: WebSocket): SubscribeResult => {
        const sinceSeq = Number((params as SubscribeParams | undefined)?.sinceSeq ?? 0);
        // Position the server occupied before this call — the client clamps to
        // it, which is how it detects that we restarted and renumbered.
        const seqBefore = lastSeq;

        const oldestBuffered = buffer.length > 0 ? buffer[0].seq : lastSeq + 1;
        const canReplay =
            Number.isFinite(sinceSeq) &&
            sinceSeq > 0 &&
            sinceSeq <= lastSeq && // ahead of us => we restarted; not a replay case
            sinceSeq >= oldestBuffered - 1; // everything after sinceSeq is still buffered

        if (canReplay) {
            let replayed = 0;
            for (const frame of buffer) {
                if (frame.seq <= sinceSeq) continue;
                sendFrame(ws, opts.log, frame);
                replayed++;
            }
            opts.log("info", "client resubscribed with replay", { sinceSeq, replayed, seq: seqBefore });
            return { seq: seqBefore, replayed, gap: false };
        }

        // Either a first connect, a gap too large to replay, or a client that
        // is ahead of us because we restarted. All three need current truth
        // rather than history.
        opts.log("info", "client resubscribed with snapshot", {
            sinceSeq,
            seq: seqBefore,
            oldestBuffered,
            reason: sinceSeq <= 0 ? "no position" : sinceSeq > lastSeq ? "server restarted" : "buffer overrun",
        });
        try {
            opts.controller.publishSnapshot();
        } catch (err) {
            opts.log("warn", "snapshot on subscribe failed", { err: String(err) });
        }
        return { seq: seqBefore, replayed: 0, gap: true };
    };

    const handlers: Record<string, (params: unknown) => Promise<unknown>> = {
        [Methods.Commission]: (p) => opts.controller.commission(p as never),
        [Methods.ListNodes]: () => opts.controller.listNodes(),
        [Methods.ReadAttribute]: (p) => opts.controller.readAttribute(p as never),
        [Methods.WriteAttribute]: (p) => opts.controller.writeAttribute(p as never).then(() => null),
        [Methods.InvokeCommand]: (p) => opts.controller.invokeCommand(p as never),
        [Methods.RemoveNode]: (p) => opts.controller.removeNode(p as never).then(() => null),
        [Methods.DiscoverCommissionable]: (p) => opts.controller.discoverCommissionable((p ?? {}) as never),
        [Methods.WebrtcOffer]: (p) => opts.controller.webrtcOffer(p as never),
        [Methods.WebrtcIce]: (p) => opts.controller.webrtcIce(p as never).then(() => null),
        [Methods.WebrtcStop]: (p) => opts.controller.webrtcStop(p as never).then(() => null),
    };

    wss.on("connection", (ws) => {
        clients.add(ws);
        opts.log("info", "ws client connected", { clients: clients.size });

        ws.on("message", async (buf) => {
            const req = parseRequest(buf.toString(), opts.log);
            if (!req) {
                sendFrame(ws, opts.log, {
                    id: "",
                    error: {
                        code: RpcErrorCode.BadRequest,
                        message: "invalid json frame",
                        kind: "bad_request",
                        retryable: false,
                    },
                });
                return;
            }
            // subscribe is the one method whose reply depends on which socket
            // asked, so it can't go through the params-only handler table.
            if (req.method === Methods.Subscribe) {
                sendFrame(ws, opts.log, { id: req.id, result: subscribe(req.params, ws) });
                return;
            }

            const handler = handlers[req.method];
            if (!handler) {
                sendFrame(ws, opts.log, {
                    id: req.id,
                    error: {
                        code: RpcErrorCode.MethodNotFound,
                        message: `unknown method ${req.method}`,
                        kind: "unsupported",
                        retryable: false,
                    },
                });
                return;
            }
            try {
                const result = await handler(req.params);
                sendFrame(ws, opts.log, { id: req.id, result: result ?? null });
            } catch (err) {
                const body = toRpcErrorBody(err);
                opts.log("warn", "rpc handler failed", {
                    method: req.method,
                    kind: body.kind,
                    retryable: body.retryable,
                    err: body.message,
                });
                sendFrame(ws, opts.log, { id: req.id, error: body });
            }
        });

        ws.on("close", () => {
            clients.delete(ws);
            opts.log("info", "ws client disconnected", { clients: clients.size });
        });
    });

    // Fan out matter.js events to every connected WS client, numbering and
    // buffering each one on the way out.
    const forward = (event: string) => (data: unknown) => {
        broadcast(clients, record(event, data), opts.log);
    };
    opts.controller.on(Events.AttributeChanged, forward(Events.AttributeChanged));
    opts.controller.on(Events.NodeOnline, forward(Events.NodeOnline));
    opts.controller.on(Events.NodeOffline, forward(Events.NodeOffline));
    opts.controller.on(Events.CommissioningProgress, forward(Events.CommissioningProgress));
    opts.controller.on(Events.CommissionableFound, forward(Events.CommissionableFound));
    opts.controller.on(Events.DeviceEvent, forward(Events.DeviceEvent));
    opts.controller.on(Events.WebrtcSignal, forward(Events.WebrtcSignal));

    opts.log("info", "ws server listening", { host: opts.host, port: opts.port });

    return {
        close: () =>
            new Promise((resolve) => {
                for (const ws of clients) ws.close();
                wss.close(() => resolve());
            }),
    };
}

function parseRequest(raw: string, log: ServerOptions["log"]): RpcRequest | null {
    try {
        const obj = JSON.parse(raw) as RpcRequest;
        if (typeof obj?.id !== "string" || typeof obj?.method !== "string") {
            return null;
        }
        return obj;
    } catch (err) {
        log("warn", "malformed json", { err: String(err) });
        return null;
    }
}

/**
 * Matter attribute values routinely contain BigInt — node ids, EUI64 addresses,
 * 64-bit counters — and JSON.stringify throws on them outright. Unhandled, that
 * single throw travelled up through the emitter into the StateStream consumer
 * and killed the whole realtime path on its first event, over and over.
 *
 * Values inside the safe-integer range become numbers so the Go side keeps
 * parsing them as numbers; anything larger becomes a string, because silently
 * rounding a 64-bit id is worse than changing its type.
 */
function jsonReplacer(_key: string, value: unknown): unknown {
    if (typeof value !== "bigint") return value;
    return value >= BigInt(Number.MIN_SAFE_INTEGER) && value <= BigInt(Number.MAX_SAFE_INTEGER)
        ? Number(value)
        : value.toString();
}

/**
 * encodeFrame never throws. A frame that cannot be serialised is dropped with a
 * log line: losing one update is bad, but taking down the connection that
 * carries every other device's state is far worse.
 */
function encodeFrame(frame: unknown, log: ServerOptions["log"]): string | undefined {
    try {
        return JSON.stringify(frame, jsonReplacer);
    } catch (err) {
        log("error", "dropping unserialisable frame", { err: String(err) });
        return undefined;
    }
}

function sendFrame(ws: WebSocket, log: ServerOptions["log"], frame: OutboundFrame): void {
    if (ws.readyState !== ws.OPEN) return;
    const payload = encodeFrame(frame, log);
    if (payload !== undefined) ws.send(payload);
}

function broadcast(clients: Set<WebSocket>, event: ServerEvent, log: ServerOptions["log"]): void {
    const payload = encodeFrame(event, log);
    if (payload === undefined) return;
    for (const ws of clients) {
        if (ws.readyState === ws.OPEN) ws.send(payload);
    }
}
