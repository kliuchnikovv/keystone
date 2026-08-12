// Small WebSocket JSON-RPC server. One handler per method; server-pushed
// events are broadcast to every connected client.

import { WebSocketServer, type WebSocket } from "ws";
import {
    Events,
    Methods,
    RpcErrorCode,
    type OutboundFrame,
    type RpcRequest,
    type ServerEvent,
} from "./protocol.js";
import type { MatterController } from "./controller.js";

export interface ServerOptions {
    host: string;
    port: number;
    controller: MatterController;
    log: (level: "info" | "warn" | "error", msg: string, meta?: Record<string, unknown>) => void;
}

export function startWsServer(opts: ServerOptions): { close: () => Promise<void> } {
    const wss = new WebSocketServer({ host: opts.host, port: opts.port });
    const clients = new Set<WebSocket>();

    const handlers: Record<string, (params: unknown) => Promise<unknown>> = {
        [Methods.Commission]: (p) => opts.controller.commission(p as never),
        [Methods.ListNodes]: () => opts.controller.listNodes(),
        [Methods.ReadAttribute]: (p) => opts.controller.readAttribute(p as never),
        [Methods.WriteAttribute]: (p) => opts.controller.writeAttribute(p as never).then(() => null),
        [Methods.InvokeCommand]: (p) => opts.controller.invokeCommand(p as never),
        [Methods.RemoveNode]: (p) => opts.controller.removeNode(p as never).then(() => null),
    };

    wss.on("connection", (ws) => {
        clients.add(ws);
        opts.log("info", "ws client connected", { clients: clients.size });

        ws.on("message", async (buf) => {
            const req = parseRequest(buf.toString(), opts.log);
            if (!req) {
                sendFrame(ws, {
                    id: "",
                    error: { code: RpcErrorCode.BadRequest, message: "invalid json frame" },
                });
                return;
            }
            const handler = handlers[req.method];
            if (!handler) {
                sendFrame(ws, {
                    id: req.id,
                    error: { code: RpcErrorCode.MethodNotFound, message: `unknown method ${req.method}` },
                });
                return;
            }
            try {
                const result = await handler(req.params);
                sendFrame(ws, { id: req.id, result: result ?? null });
            } catch (err) {
                opts.log("warn", "rpc handler failed", { method: req.method, err: String(err) });
                sendFrame(ws, {
                    id: req.id,
                    error: { code: RpcErrorCode.Internal, message: err instanceof Error ? err.message : String(err) },
                });
            }
        });

        ws.on("close", () => {
            clients.delete(ws);
            opts.log("info", "ws client disconnected", { clients: clients.size });
        });
    });

    // Fan out matter.js events to every connected WS client.
    const forward = (event: string) => (data: unknown) => {
        broadcast(clients, { event, data });
    };
    opts.controller.on(Events.AttributeChanged, forward(Events.AttributeChanged));
    opts.controller.on(Events.NodeOnline, forward(Events.NodeOnline));
    opts.controller.on(Events.NodeOffline, forward(Events.NodeOffline));

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

function sendFrame(ws: WebSocket, frame: OutboundFrame): void {
    if (ws.readyState === ws.OPEN) {
        ws.send(JSON.stringify(frame));
    }
}

function broadcast(clients: Set<WebSocket>, event: ServerEvent): void {
    const payload = JSON.stringify(event);
    for (const ws of clients) {
        if (ws.readyState === ws.OPEN) ws.send(payload);
    }
}
