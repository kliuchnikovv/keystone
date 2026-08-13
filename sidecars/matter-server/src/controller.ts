// Thin wrapper around matter.js that exposes the operations the Go core
// needs. Splits WebSocket layer from matter.js API so the WS dispatcher
// stays independent of the matter.js version churn.
//
// Pinned to @matter/main 0.15.x in package.json. When bumping the pin,
// re-verify the imports below — the controller surface moved substantially
// between 0.15 and 0.18+.

import { EventEmitter } from "node:events";
import { Environment, ServerNode } from "@matter/main";

// ClientNode is not re-exported from @matter/main in 0.15 — derive its type
// from ServerNode.peers.get() so we don't have to reach into deep paths.
type ClientNode = NonNullable<ReturnType<ServerNode["peers"]["get"]>>;
import type {
    AttrRef,
    AttributeChanged,
    CommissionParams,
    CommissionResult,
    InvokeParams,
    Node,
    NodeLifecycle,
    RemoveNodeParams,
    WriteAttrParams,
} from "./protocol.js";

export interface MatterController {
    start(): Promise<void>;
    stop(): Promise<void>;

    commission(p: CommissionParams): Promise<CommissionResult>;
    listNodes(): Promise<Node[]>;
    readAttribute(p: AttrRef): Promise<unknown>;
    writeAttribute(p: WriteAttrParams): Promise<void>;
    invokeCommand(p: InvokeParams): Promise<unknown>;
    removeNode(p: RemoveNodeParams): Promise<void>;

    on(event: "attributeChanged", listener: (v: AttributeChanged) => void): this;
    on(event: "nodeOnline" | "nodeOffline", listener: (v: NodeLifecycle) => void): this;
}

export interface ControllerOptions {
    // Storage location for fabric state. matter.js reads it from the
    // MATTER_STORAGE_PATH env var, so we set it before constructing the
    // Environment. Passing here is convenience only.
    storagePath: string;
    fabricLabel: string;
}

export function createController(opts: ControllerOptions): MatterController {
    // matter.js reads MATTER_STORAGE_PATH via its VariableService (Unix-env
    // style: MATTER_* -> nested config path). Set it before Environment.default
    // is first touched so StorageBackendDisk lands at the right location.
    if (opts.storagePath && !process.env.MATTER_STORAGE_PATH) {
        process.env.MATTER_STORAGE_PATH = opts.storagePath;
    }

    const emitter = new EventEmitter();
    let node: ServerNode | undefined;

    return {
        async start() {
            if (node) return;

            // ServerNode.RootEndpoint bakes in ControllerBehavior in 0.15 —
            // no explicit `.with(ControllerBehavior)` needed.
            node = await ServerNode.create({
                id: "keystone-controller",
                environment: Environment.default,
            });

            node.peers.added.on((client: ClientNode) => {
                emitter.emit("nodeOnline", { nodeId: nodeIdOf(client) });
            });
            node.peers.deleted.on((client: ClientNode) => {
                emitter.emit("nodeOffline", { nodeId: nodeIdOf(client) });
            });

            await node.start();
        },

        async stop() {
            const current = node;
            node = undefined;
            emitter.removeAllListeners();
            if (current) {
                await current.close();
            }
        },

        async commission(p: CommissionParams): Promise<CommissionResult> {
            const n = assertStarted(node);
            // matter.js 0.15 accepts the raw 11-digit manual pairing code
            // directly — it decodes passcode + discriminator internally.
            const client = (await n.peers.commission({ pairingCode: p.setupCode })) as unknown as ClientNode;
            return {
                nodeId: nodeIdOf(client),
                fabricIndex: fabricIndexOf(client),
            };
        },

        async listNodes(): Promise<Node[]> {
            const n = assertStarted(node);
            const out: Node[] = [];
            for (const client of n.peers) {
                out.push({
                    nodeId: nodeIdOf(client),
                    online: true,
                    endpoints: endpointsOf(client),
                });
            }
            return out;
        },

        async readAttribute(p: AttrRef): Promise<unknown> {
            const n = assertStarted(node);
            const target = endpointFor(clientFor(n, p.nodeId), p.endpointId);
            const cluster = camelCase(p.cluster);
            const attribute = camelCase(p.attribute);
            const state = (target as unknown as {
                stateOf(id: string): Record<string, unknown>;
            }).stateOf(cluster);
            if (!(attribute in state)) {
                throw new Error(`matter: attribute ${p.cluster}.${p.attribute} not present on endpoint ${p.endpointId}`);
            }
            return state[attribute];
        },

        async writeAttribute(p: WriteAttrParams): Promise<void> {
            const n = assertStarted(node);
            const target = endpointFor(clientFor(n, p.nodeId), p.endpointId);
            const cluster = camelCase(p.cluster);
            const attribute = camelCase(p.attribute);
            await (target as unknown as {
                setStateOf(id: string, values: Record<string, unknown>): Promise<void>;
            }).setStateOf(cluster, { [attribute]: p.value });
        },

        async invokeCommand(p: InvokeParams): Promise<unknown> {
            const n = assertStarted(node);
            const target = endpointFor(clientFor(n, p.nodeId), p.endpointId);
            const cluster = camelCase(p.cluster);
            const command = camelCase(p.command);
            // Endpoint.commandsOf gives direct access to cluster commands
            // without having to activate a behavior on the client side.
            const commands = (target as unknown as {
                commandsOf(id: string): Record<string, (args?: unknown) => Promise<unknown>>;
            }).commandsOf(cluster);
            const fn = commands[command];
            if (typeof fn !== "function") {
                throw new Error(`matter: command ${p.cluster}.${p.command} (${cluster}.${command}) not found on endpoint ${p.endpointId}`);
            }
            return await fn(p.args ?? undefined);
        },

        async removeNode(p: RemoveNodeParams): Promise<void> {
            const n = assertStarted(node);
            const client = clientFor(n, p.nodeId);
            await client.delete();
        },

        on(event: string, listener: (...args: unknown[]) => void): MatterController {
            emitter.on(event, listener);
            return this as unknown as MatterController;
        },
    } as MatterController;
}

// --- helpers ---

function clientFor(node: ServerNode, nodeId: string): ClientNode {
    // Peers.get looks up by internal endpoint id, not fabric nodeId. Match by
    // walking peers and comparing the value nodeIdOf returned to keystone.
    for (const client of node.peers) {
        if (nodeIdOf(client) === nodeId) return client;
    }
    throw new Error(`matter: node ${nodeId} not found`);
}

// endpointsOf walks the endpoint tree of a ClientNode and returns the shape
// keystone shows in listNodes. Cluster names come from behaviors.supported —
// keys are matter.js camelCase (onOff, levelControl); listing them raw is more
// useful than trying to normalise here, since the RPC contract asks for the
// canonical PascalCase and the mapping is imperfect anyway.
function endpointsOf(client: ClientNode): Array<{ endpointId: number; clusters: string[] }> {
    const out: Array<{ endpointId: number; clusters: string[] }> = [];
    const collect = (ep: any) => {
        let num: number | undefined;
        try { num = ep.maybeNumber ?? ep.number; } catch { num = undefined; }
        let clusters: string[] = [];
        try {
            const supported = ep.behaviors?.supported ?? {};
            clusters = Object.keys(supported);
        } catch { /* ignore */ }
        if (num !== undefined) out.push({ endpointId: num, clusters });
        try {
            for (const child of ep.parts ?? []) collect(child);
        } catch { /* ignore */ }
    };
    collect(client);
    return out;
}

// endpointFor resolves the target endpoint on a ClientNode. endpointId 0 is
// the root node itself; anything else is a child part. ClientNode extends
// Endpoint, so both cases return an Endpoint compatible with .act().
function endpointFor(client: ClientNode, endpointId: number): { act: ClientNode["act"] } {
    if (endpointId === 0 || client.number === endpointId) {
        return client as unknown as { act: ClientNode["act"] };
    }
    const parts = (client as unknown as { parts: { get(id: number): { act: ClientNode["act"] } | undefined } }).parts;
    const child = parts.get(endpointId);
    if (!child) {
        throw new Error(`matter: endpoint ${endpointId} not found on node ${nodeIdOf(client)}`);
    }
    return child;
}

// nodeIdOf extracts the fabric-scoped node id from a ClientNode's peer
// address. In matter.js 0.17 the public accessor is `peerAddress` — reaching
// into it before the client is fully constructed can throw
// (`Cannot read private member #cachedPeerAddress`), so we swallow that.
function nodeIdOf(client: ClientNode): string {
    let peer: { nodeId?: bigint | number } | undefined;
    try {
        peer = (client as unknown as { peerAddress?: { nodeId?: bigint | number } }).peerAddress;
    } catch {
        peer = undefined;
    }
    const id = peer?.nodeId;
    if (id === undefined || id === null) {
        try { return String(client.number ?? ""); } catch { return ""; }
    }
    return typeof id === "bigint" ? id.toString() : String(id);
}

function fabricIndexOf(client: ClientNode): number {
    try {
        const peer = (client as unknown as { peerAddress?: { fabricIndex?: number } }).peerAddress;
        return peer?.fabricIndex ?? 0;
    } catch {
        return 0;
    }
}

// camelCase converts matter.js wire-style PascalCase names (OnOff, MoveToLevel)
// into the property-style names matter.js uses on behavior instances (onOff,
// moveToLevel). One-char names stay unchanged.
function camelCase(name: string): string {
    if (!name) return name;
    return name.charAt(0).toLowerCase() + name.slice(1);
}

function assertStarted(node: ServerNode | undefined): ServerNode {
    if (!node) throw new Error("matter controller not started");
    return node;
}

function notImplemented(what: string): Error {
    return new Error(`matter controller: ${what} not yet implemented — see TODO(matter-attr) in controller.ts`);
}
