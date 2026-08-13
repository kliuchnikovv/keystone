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
                    // Endpoint / cluster walking is done lazily by readAttribute
                    // & friends. Returning the ids is enough for keystone to
                    // register the device; full cluster enumeration lands with
                    // the attribute wiring below.
                    endpoints: [],
                });
            }
            return out;
        },

        async readAttribute(p: AttrRef): Promise<unknown> {
            const n = assertStarted(node);
            const client = clientFor(n, p.nodeId);
            // TODO(matter-attr): map (cluster, attribute) -> behavior factory
            // from @matter/main/clusters, resolve the endpoint on the client
            // tree, then:
            //   return ep.stateOf(behavior.id)[camelCase(p.attribute)];
            throw notImplemented(`readAttribute(${p.cluster}.${p.attribute}) on ${client.number}`);
        },

        async writeAttribute(p: WriteAttrParams): Promise<void> {
            const n = assertStarted(node);
            const client = clientFor(n, p.nodeId);
            // TODO(matter-attr): same mapping as readAttribute, then:
            //   await ep.setStateOf(behavior.id, { [camelCase(p.attribute)]: p.value });
            throw notImplemented(`writeAttribute(${p.cluster}.${p.attribute}) on ${client.number}`);
        },

        async invokeCommand(p: InvokeParams): Promise<unknown> {
            const n = assertStarted(node);
            const client = clientFor(n, p.nodeId);
            const cluster = camelCase(p.cluster);
            const command = camelCase(p.command);
            // ClientNode.act runs a callback inside an ActionContext. The
            // agent exposes every behavior as a property, and each cluster
            // behavior exposes its commands as methods.
            //
            // We route the whole call through act() so matter.js manages the
            // context/transaction for us. If the cluster or command name is
            // wrong, the agent lookup throws a descriptive error.
            return await client.act(async (agent: Record<string, any>) => {
                const beh = agent[cluster];
                if (!beh) {
                    throw new Error(`matter: cluster ${p.cluster} (${cluster}) not present on endpoint ${p.endpointId}`);
                }
                const fn = beh[command];
                if (typeof fn !== "function") {
                    throw new Error(`matter: command ${p.cluster}.${p.command} (${cluster}.${command}) not found`);
                }
                return await fn.call(beh, p.args ?? {});
            });
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
    // ClientNodes.get accepts string | number | PeerAddress.
    const client = node.peers.get(nodeId) as ClientNode | undefined;
    if (!client) {
        throw new Error(`matter: node ${nodeId} not found`);
    }
    return client;
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
