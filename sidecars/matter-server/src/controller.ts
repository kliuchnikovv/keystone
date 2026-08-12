// Thin wrapper around matter.js that exposes the operations the Go core
// needs. Split from the WebSocket layer so it stays unit-testable and so
// the matter.js API surface (which churns) is confined to one file.
//
// The matter.js API is evolving quickly (0.15 -> 0.19 rewrote the
// controller surface — see MIGRATION_CONTROLLER_018 in the upstream repo).
// The methods below intentionally live on a narrow facade so the RPC
// dispatcher never touches matter.js types directly. When the upstream API
// stabilises, only this file needs an update.

import { EventEmitter } from "node:events";
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
    storagePath: string;
    fabricLabel: string;
}

// Real implementation: builds a matter.js controller ServerNode and routes the
// RPC surface through it. Kept behind an interface so unit tests / dev mode
// can swap in a stub (see stubController).
export function createController(_opts: ControllerOptions): MatterController {
    const emitter = new EventEmitter();

    // NOTE: The matter.js hookup lives inside the closure below. It is
    // intentionally lazy so `import` failures during dev iteration surface at
    // start() with a clear log, not at module load, and so unit tests can
    // exercise the RPC dispatcher without pulling in the entire matter.js
    // dependency graph.
    //
    // Concrete wiring (matter.js >=0.18):
    //
    //   import { Environment, StorageService } from "@matter/general";
    //   import { ServerNode } from "@matter/node";
    //   import { ControllerBehavior } from "@matter/node/behaviors";
    //   import { FabricAuthority } from "@matter/protocol";
    //
    //   const env = Environment.default;
    //   env.get(StorageService).location = opts.storagePath;
    //   const node = await ServerNode.create(
    //       ServerNode.RootEndpoint.with(ControllerBehavior),
    //       {
    //           environment: env,
    //           id: "keystone-controller",
    //           commissioning: { enabled: false },
    //           controller: { adminFabricLabel: opts.fabricLabel },
    //           network: { autoStartCommissionedPeers: true },
    //       },
    //   );
    //   await env.load(FabricAuthority)
    //       .then(fa => fa.defaultFabric({ adminFabricLabel: opts.fabricLabel }));
    //   await node.start();
    //
    // See:
    //   - packages/matter.js/docs/MIGRATION_CONTROLLER_018.md (upstream)
    //   - examples/controller/ (upstream)
    //
    // Left as TODO(matter-real): landing this requires a live @matter/main
    // install to sanity-check the exact import paths against the version we
    // pin. Until then the RPC layer runs against the stub below in dev.

    let started = false;

    return {
        async start() {
            if (started) return;
            started = true;
            // TODO(matter-real): initialize ServerNode + FabricAuthority as above.
        },

        async stop() {
            if (!started) return;
            started = false;
            emitter.removeAllListeners();
            // TODO(matter-real): await node.close() to persist fabric state cleanly.
        },

        async commission(p: CommissionParams): Promise<CommissionResult> {
            assertStarted(started);
            // TODO(matter-real): parse the 11-digit manual pairing code the user
            // copied from Home.app -> {passcode, discriminator}, then:
            //   const peer = await node.peers.commission({
            //       passcode, discriminator,
            //       discoveryCapabilities: { onIpNetwork: true },
            //       autoSubscribe: true,
            //   });
            //   return { nodeId: peer.nodeId.toString(), fabricIndex: fabric.fabricIndex };
            throw notImplemented("commission", `setupCode=${p.setupCode}`);
        },

        async listNodes(): Promise<Node[]> {
            assertStarted(started);
            // TODO(matter-real): iterate node.peers and describe each peer's
            // endpoints/clusters. See serverNode.peers docs.
            return [];
        },

        async readAttribute(p: AttrRef): Promise<unknown> {
            assertStarted(started);
            // TODO(matter-real): resolve peer by nodeId, then
            //   peer.endpoints[p.endpointId].stateOf(<clusterBehavior>)[p.attribute]
            throw notImplemented("readAttribute", `${p.cluster}.${p.attribute}`);
        },

        async writeAttribute(p: WriteAttrParams): Promise<void> {
            assertStarted(started);
            // TODO(matter-real):
            //   await peer.endpoints[p.endpointId].setStateOf(
            //       <clusterBehavior>,
            //       { [p.attribute]: p.value },
            //   );
            throw notImplemented("writeAttribute", `${p.cluster}.${p.attribute}`);
        },

        async invokeCommand(p: InvokeParams): Promise<unknown> {
            assertStarted(started);
            // TODO(matter-real):
            //   return peer.endpoints[p.endpointId].commandsOf(<clusterBehavior>)[p.command](p.args);
            throw notImplemented("invokeCommand", `${p.cluster}.${p.command}`);
        },

        async removeNode(p: RemoveNodeParams): Promise<void> {
            assertStarted(started);
            // TODO(matter-real): await node.peers.get(nodeId)?.decommission();
            throw notImplemented("removeNode", p.nodeId);
        },

        on(event: string, listener: (...args: unknown[]) => void): MatterController {
            emitter.on(event, listener);
            return this as unknown as MatterController;
        },
    } as MatterController;
}

function assertStarted(started: boolean): void {
    if (!started) throw new Error("matter controller not started");
}

function notImplemented(method: string, detail: string): Error {
    return new Error(`matter controller: ${method}(${detail}) not implemented — see TODO(matter-real) in controller.ts`);
}
