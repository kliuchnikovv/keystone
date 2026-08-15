// Thin wrapper around matter.js that exposes the operations the Go core
// needs. Splits WebSocket layer from matter.js API so the WS dispatcher
// stays independent of the matter.js version churn.
//
// Pinned to @matter/main 0.15.x in package.json. When bumping the pin,
// re-verify the imports below — the controller surface moved substantially
// between 0.15 and 0.18+.

import { EventEmitter } from "node:events";
// StateStream lives in @matter/node, which @matter/main re-exports wholesale.
// Import it from @matter/main so we don't depend on a package that isn't in
// our package.json and only resolves today by npm hoisting.
import { Environment, ServerNode, StateStream } from "@matter/main";
import { MatterModel } from "@matter/model";

// ClientNode is not re-exported from @matter/main in 0.15 — derive its type
// from ServerNode.peers.get() so we don't have to reach into deep paths.
type ClientNode = NonNullable<ReturnType<ServerNode["peers"]["get"]>>;
import type {
    AttrRef,
    AttributeChanged,
    CommissionableDevice,
    CommissionParams,
    CommissionResult,
    CommissioningProgress,
    DeviceEvent,
    DiscoverCommissionableParams,
    WebRtcSignal,
    WebrtcIceParams,
    WebrtcOfferParams,
    WebrtcOfferResult,
    WebrtcStopParams,
    InvokeParams,
    Node,
    NodeLifecycle,
    RemoveNodeParams,
    RemoveNodeResult,
    WriteAttrParams,
} from "./protocol.js";
import { RpcError } from "./protocol.js";
import { ControllerCommissioningFlow } from "@matter/protocol";
import { ChangeNotificationService } from "@matter/node";
import {
    bindSignals,
    ProviderCluster,
    ProviderCommands,
    SessionRegistry,
    WebRtcRequestorBehavior,
} from "./webrtc.js";
import { ManualPairingCodeCodec, QrPairingCodeCodec } from "@matter/types";
import { Millis } from "@matter/general";

export interface MatterController {
    start(): Promise<void>;
    stop(): Promise<void>;

    commission(p: CommissionParams): Promise<CommissionResult>;
    listNodes(): Promise<Node[]>;
    readAttribute(p: AttrRef): Promise<unknown>;
    writeAttribute(p: WriteAttrParams): Promise<void>;
    invokeCommand(p: InvokeParams): Promise<unknown>;
    removeNode(p: RemoveNodeParams): Promise<RemoveNodeResult>;

    // discoverCommissionable listens for devices advertising themselves as
    // ready to pair. Results are also pushed as `commissionableFound` events so
    // a UI can fill its list while the scan is still running.
    discoverCommissionable(p: DiscoverCommissionableParams): Promise<CommissionableDevice[]>;

    // WebRTC signalling for cameras. The sidecar brokers the handshake; the
    // media never passes through it — see webrtc.ts.
    webrtcOffer(p: WebrtcOfferParams): Promise<WebrtcOfferResult>;
    webrtcIce(p: WebrtcIceParams): Promise<void>;
    webrtcStop(p: WebrtcStopParams): Promise<void>;

    // publishSnapshot re-emits the current cached state of every known cluster
    // as `attributeChanged` events. StateStream only delivers *changes* for
    // peers, so without this a freshly connected (or reconnected) keystone has
    // no idea what state devices are in until someone touches them.
    publishSnapshot(nodeId?: string): void;

    on(event: "attributeChanged", listener: (v: AttributeChanged) => void): this;
    on(event: "nodeOnline" | "nodeOffline", listener: (v: NodeLifecycle) => void): this;
    on(event: "commissioningProgress", listener: (v: CommissioningProgress) => void): this;
    on(event: "commissionableFound", listener: (v: CommissionableDevice) => void): this;
    on(event: "deviceEvent", listener: (v: DeviceEvent) => void): this;
    on(event: "webrtcSignal", listener: (v: WebRtcSignal) => void): this;
}

export type ControllerLog = (
    level: "info" | "warn" | "error",
    msg: string,
    meta?: Record<string, unknown>,
) => void;

export interface ControllerOptions {
    // Storage location for fabric state. matter.js reads it from the
    // MATTER_STORAGE_PATH env var, so we set it before constructing the
    // Environment. Passing here is convenience only.
    storagePath: string;
    fabricLabel: string;
    log?: ControllerLog;
}

export function createController(opts: ControllerOptions): MatterController {
    // matter.js reads MATTER_STORAGE_PATH via its VariableService (Unix-env
    // style: MATTER_* -> nested config path). Set it before Environment.default
    // is first touched so StorageBackendDisk lands at the right location.
    if (opts.storagePath && !process.env.MATTER_STORAGE_PATH) {
        process.env.MATTER_STORAGE_PATH = opts.storagePath;
    }

    const log: ControllerLog = opts.log ?? (() => { /* silent by default */ });
    const emitter = new EventEmitter();
    let node: ServerNode | undefined;
    let streamAbort: AbortController | undefined;

    return {
        async start() {
            if (node) return;

            // ServerNode.RootEndpoint bakes in ControllerBehavior in 0.15 —
            // no explicit `.with(ControllerBehavior)` needed.
            // The camera calls Answer/IceCandidates/End on *us*, so the
            // controller has to host the requestor cluster as a server.
            // Without it a camera has nowhere to send its half of the
            // handshake and video never starts.
            node = await ServerNode.create({
                id: "keystone-controller",
                environment: Environment.default,
                type: ServerNode.RootEndpoint.with(WebRtcRequestorBehavior),
            });
            bindSignals(emitter, webrtcSessions);

            node.peers.added.on((client: ClientNode) => {
                emitter.emit("nodeOnline", { nodeId: nodeIdOf(client) });
            });
            node.peers.deleted.on((client: ClientNode) => {
                emitter.emit("nodeOffline", { nodeId: nodeIdOf(client) });
            });

            await node.start();

            // One StateStream for the whole ServerNode replaces the old
            // per-peer bindSubscriptions loop. StateStream emits coalesced
            // property updates for ALL peers (including those restored from
            // persisted fabric on boot), which the `$Changed` observers
            // couldn't do — matter.js 0.17 keeps peer state in a Datasource
            // that per-cluster event registries never see.
            //
            // Supervised: if the generator ends or throws we are silently deaf
            // to every device in the house, with a sidecar that still looks
            // healthy — exactly the failure we just spent a day diagnosing.
            streamAbort = new AbortController();
            void superviseStateStream(node, emitter, streamAbort.signal, log);
            bindDeviceEvents(node, emitter, log);

            for (const client of node.peers) {
                await ensureAutoSubscribe(client, log);
            }
        },

        async stop() {
            streamAbort?.abort();
            streamAbort = undefined;
            const current = node;
            node = undefined;
            emitter.removeAllListeners();
            if (current) {
                await current.close();
            }
        },

        async commission(p: CommissionParams): Promise<CommissionResult> {
            const n = assertStarted(node);

            // Validate before touching any shared state: a rejected setup code
            // must not announce a "discovering" stage for a session that never
            // starts, nor look like an in-flight commission to the next caller.
            const setupCode = decodeSetupCode(p.setupCode);

            // Same for an unknown target: resolve it before anything is
            // announced or reserved.
            const targetRef = p.target ?? "";
            let targetNode: ClientNode | undefined;
            if (targetRef !== "") {
                targetNode = commissionableByRef.get(targetRef);
                if (targetNode === undefined) {
                    throw new RpcError(
                        "not_found",
                        `matter: ${targetRef} is not among the devices found by the last scan — rescan and try again`,
                    );
                }
            }

            // One commission at a time. PASE is a single-session affair on the
            // controller side, and progress reporting keys off a process-wide
            // sink — running two would interleave both users' stages.
            if (commissionInFlight) {
                throw new RpcError("bad_request", "matter: a commissioning session is already in progress");
            }
            commissionInFlight = true;
            progressSink = (stage, message) => {
                emitter.emit("commissioningProgress", { stage, message });
            };

            try {
                reportProgress("discovering", "looking for the device");
                // Only meaningful for a device that still has to join a
                // network; matter.js skips the network steps entirely when the
                // device is already reachable over IP.
                const networkOptions: Record<string, unknown> = {};
                if (p.network?.wifi?.ssid) {
                    networkOptions.wifiNetwork = {
                        wifiSsid: p.network.wifi.ssid,
                        wifiCredentials: p.network.wifi.credentials ?? "",
                    };
                }
                if (p.network?.thread?.operationalDataset) {
                    networkOptions.threadNetwork = {
                        operationalDataset: p.network.thread.operationalDataset,
                        networkName: p.network.thread.networkName,
                    };
                }

                const flowOptions = {
                    ...networkOptions,
                    // Real phase reporting: the flow subclass announces each
                    // commissioning step as matter.js executes it.
                    commissioningFlowImpl: ProgressReportingFlow,
                    // Fires the moment PASE succeeds — the first hard evidence
                    // that we are actually talking to the device.
                    continueCommissioningAfterPase: () => {
                        reportProgress("paired", "secure session established");
                        return true;
                    },
                };

                let client: ClientNode;
                if (targetNode !== undefined) {
                    // Targeted: the user picked one of the devices a scan turned
                    // up, so commission that exact node instead of re-running
                    // discovery and hoping the discriminator is unambiguous.
                    await targetNode.commission({ passcode: passcodeOf(setupCode), ...flowOptions });
                    client = targetNode;
                    commissionableByRef.delete(targetRef);
                } else {
                    client = (await n.peers.commission({ ...setupCode, ...flowOptions })) as unknown as ClientNode;
                }
                const nodeId = nodeIdOf(client);
                reportProgress("interviewing", "reading device capabilities");
                await ensureAutoSubscribe(client, log);
                // Push what the interview already read (OnOff, CurrentLevel, …)
                // so keystone shows real state instead of "unknown" until the
                // user happens to toggle something.
                snapshotOf(client, emitter, log);
                reportProgress("done", "device commissioned");
                return {
                    nodeId,
                    fabricIndex: fabricIndexOf(client),
                };
            } finally {
                progressSink = undefined;
                commissionInFlight = false;
            }
        },

        async listNodes(): Promise<Node[]> {
            const n = assertStarted(node);
            const out: Node[] = [];
            for (const client of n.peers) {
                // `peers` holds commissionable nodes too — anything a discovery
                // run turned up, including our neighbours' devices. Only nodes
                // actually in our fabric are keystone's business; without this
                // filter a scan would conjure phantom devices in the registry.
                if (!isCommissioned(client)) continue;
                out.push({
                    nodeId: nodeIdOf(client),
                    online: true,
                    endpoints: endpointsOf(client),
                    ...basicInfoOf(client),
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
                throw new RpcError(
                    "unsupported",
                    `matter: attribute ${p.cluster}.${p.attribute} not present on endpoint ${p.endpointId}`,
                );
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
                throw new RpcError(
                    "unsupported",
                    `matter: command ${p.cluster}.${p.command} (${cluster}.${command}) not found on endpoint ${p.endpointId}`,
                );
            }
            return await fn(p.args ?? undefined);
        },

        async removeNode(p: RemoveNodeParams): Promise<RemoveNodeResult> {
            const n = assertStarted(node);
            const client = clientFor(n, p.nodeId);

            // Two signals, and they can disagree. `peerAddress` says we hold a
            // fabric on the device; `lifecycle.isCommissioned` is what
            // ClientNode.decommission() checks before asking the device to drop
            // it — and it is derived from lifecycle events, so a node restored
            // from storage can carry a stale value. When it is stale the
            // library quietly degrades to a local delete and the accessory goes
            // on listing us under Apple Home's Connected Services.
            const hasFabric = isCommissioned(client);
            const flagged = Boolean((client as unknown as {
                lifecycle?: { isCommissioned?: boolean };
            }).lifecycle?.isCommissioned);
            log("info", "removing matter node", {
                nodeId: p.nodeId,
                hasPeerAddress: hasFabric,
                lifecycleFlag: flagged,
            });

            if (!hasFabric) {
                // We never held a fabric on it; there is nothing to hand back.
                await client.delete();
                log("info", "matter node removed locally (no fabric held)", { nodeId: p.nodeId });
                return { removed: "decommissioned" };
            }

            try {
                // Bounded: talking to the device means MRP retries, and an
                // accessory that is unplugged would otherwise keep the delete
                // request hanging until the HTTP timeout. The user asked for it
                // gone — waiting a minute to find that out is not acceptable.
                await withTimeout(DECOMMISSION_TIMEOUT_MS, async () => {
                if (flagged) {
                    // The library path: it handles the lifecycle transition and
                    // the local cleanup around the fabric removal, so prefer it
                    // whenever its own precondition holds.
                    await client.decommission();
                } else {
                    // Stale flag: ask the device directly, then clean up
                    // locally the way decommission() would have.
                    await (client as unknown as {
                        act(purpose: string, actor: (agent: {
                            commissioning: { decommission(): Promise<void> };
                        }) => Promise<void>): Promise<void>;
                    }).act("decommission", (agent) => agent.commissioning.decommission());
                    await client.delete();
                }
                });
                log("info", "matter node decommissioned", { nodeId: p.nodeId, viaLibrary: flagged });
                return { removed: "decommissioned" };
            } catch (err) {
                log("warn", "matter node did not accept decommissioning", {
                    nodeId: p.nodeId,
                    err: String(err),
                });
                // The user asked for it gone; it must disappear from keystone
                // whatever the device says. Removing it twice is harmless —
                // delete() on an already-deleted node is a no-op.
                try {
                    await client.delete();
                } catch (deleteErr) {
                    log("warn", "matter node local delete also failed", {
                        nodeId: p.nodeId,
                        err: String(deleteErr),
                    });
                }
                return {
                    removed: "forced",
                    message: `device refused or did not answer decommissioning: ${String(err)}`,
                };
            }
        },

        async discoverCommissionable(p: DiscoverCommissionableParams): Promise<CommissionableDevice[]> {
            const n = assertStarted(node);
            const timeoutMs = Math.min(Math.max(Number(p?.timeoutMs) || DISCOVERY_DEFAULT_MS, 1_000), DISCOVERY_MAX_MS);

            const found = new Map<string, CommissionableDevice>();

            // Devices matter.js already knows about are reported immediately.
            // A scan only forwards what arrives inside its window, so a device
            // discovered by an earlier scan stayed invisible until it happened
            // to re-announce — which is why the first scan came up empty and
            // the second one found it.
            for (const client of n.peers) {
                if (isCommissioned(client)) continue;
                const device = commissionableFrom(client);
                if (device === undefined) continue;
                commissionableByRef.set(device.ref, client);
                found.set(device.ref, device);
                emitter.emit("commissionableFound", device);
            }
            if (found.size > 0) {
                log("info", "reporting already-known commissionable devices", { count: found.size });
            }

            const discovery = n.peers.discover({ timeout: Millis(timeoutMs) });

            discovery.discovered.on((client: ClientNode) => {
                const device = commissionableFrom(client);
                if (device === undefined) return;
                commissionableByRef.set(device.ref, client);
                if (found.has(device.ref)) return; // re-advertisement of a device we already reported
                found.set(device.ref, device);
                emitter.emit("commissionableFound", device);
            });

            try {
                await discovery;
            } catch (err) {
                // A scan that finds nothing is a normal outcome, not a failure;
                // only report an actual discovery error.
                log("warn", "commissionable discovery failed", { err: String(err) });
                throw err;
            }

            log("info", "commissionable discovery finished", { timeoutMs, found: found.size });
            return [...found.values()];
        },

        async webrtcOffer(p: WebrtcOfferParams): Promise<WebrtcOfferResult> {
            const n = assertStarted(node);
            const target = endpointFor(clientFor(n, p.nodeId), p.endpointId);

            // ProvideOffer hands the viewer's SDP to the camera and gets back
            // the session id everything else is keyed by.
            const args: Record<string, unknown> = {
                webRtcSessionId: null, // null asks the camera to allocate one
                sdp: p.sdp,
                streamUsage: 1, // LiveView
                originatingEndpointId: 0,
            };
            if (p.videoStreamId !== undefined) args.videoStreamId = p.videoStreamId;
            if (p.audioStreamId !== undefined) args.audioStreamId = p.audioStreamId;

            const res = (await invokeOn(target, ProviderCluster, ProviderCommands.ProvideOffer, args)) as {
                webRtcSessionId?: number;
                videoStreamId?: number;
                audioStreamId?: number;
            };
            const sessionId = res?.webRtcSessionId;
            if (typeof sessionId !== "number") {
                throw new RpcError("internal", "matter: camera did not return a webrtc session id");
            }
            webrtcSessions.remember(sessionId, p.nodeId, p.endpointId);
            log("info", "webrtc session opened", { nodeId: p.nodeId, sessionId });
            return {
                sessionId,
                videoStreamId: res.videoStreamId,
                audioStreamId: res.audioStreamId,
            };
        },

        async webrtcIce(p: WebrtcIceParams): Promise<void> {
            const n = assertStarted(node);
            const { nodeId, endpointId } = webrtcSessions.lookup(p.sessionId);
            const target = endpointFor(clientFor(n, nodeId), endpointId);
            await invokeOn(target, ProviderCluster, ProviderCommands.ProvideIceCandidates, {
                webRtcSessionId: p.sessionId,
                iceCandidates: p.candidates.map((candidate) => ({ candidate })),
            });
        },

        async webrtcStop(p: WebrtcStopParams): Promise<void> {
            const n = assertStarted(node);
            const { nodeId, endpointId } = webrtcSessions.lookup(p.sessionId);
            const target = endpointFor(clientFor(n, nodeId), endpointId);
            try {
                await invokeOn(target, ProviderCluster, ProviderCommands.EndSession, {
                    webRtcSessionId: p.sessionId,
                    reason: 2, // UserHangup
                });
            } finally {
                // Forget locally even if the camera is unreachable — the viewer
                // has gone either way.
                webrtcSessions.forget(p.sessionId);
            }
        },

        publishSnapshot(nodeId?: string): void {
            const n = assertStarted(node);
            for (const client of n.peers) {
                if (nodeId !== undefined && nodeIdOf(client) !== nodeId) continue;
                snapshotOf(client, emitter, log);
            }
        },

        on(event: string, listener: (...args: unknown[]) => void): MatterController {
            emitter.on(event, listener);
            return this as unknown as MatterController;
        },
    } as MatterController;
}

// bindDeviceEvents forwards Matter *events* — as opposed to attribute changes.
// A button is nothing but its events: CurrentPosition tells you a rocker is
// held, but single/double/long press only exist as events, so without this a
// remote control is undetectable no matter how many attributes we read.
//
// StateStream deliberately ignores them (its change listener handles only
// "update" and "delete"), so we subscribe to the same underlying service
// directly.
function bindDeviceEvents(node: ServerNode, emitter: EventEmitter, log: ControllerLog): void {
    let changes;
    try {
        changes = node.env.get(ChangeNotificationService);
    } catch (err) {
        log("warn", "matter event path unavailable", { err: String(err) });
        return;
    }

    changes.change.on((raw) => {
        if (raw?.kind !== "event") return;
        const change = raw as unknown as Record<string, unknown>;
        try {
            const endpoint = change.endpoint as {
                maybeNumber?: number;
                number?: number;
                owner?: unknown;
            };
            const endpointId = endpoint?.maybeNumber ?? endpoint?.number;
            if (endpointId === undefined) return;

            const peer = nodeOfEndpoint(endpoint);
            if (peer === undefined || peer === node) return; // our own controller

            const clusterKey = (change.behavior as { id?: string } | undefined)?.id;
            const eventName = (change.event as { name?: string } | undefined)?.name;
            if (!clusterKey || !eventName) return;

            emitter.emit("deviceEvent", {
                nodeId: nodeIdOf(peer as ClientNode),
                endpointId,
                cluster: canonicalClusterName(clusterKey),
                event: pascalCase(eventName),
                data: change.value ?? null,
            });
        } catch (err) {
            log("warn", "matter event decode failed", { err: String(err) });
        }
    });
}

// nodeOfEndpoint walks up the endpoint tree to the node that owns it.
function nodeOfEndpoint(endpoint: unknown): unknown {
    let current = endpoint as { owner?: unknown } | undefined;
    for (let i = 0; current !== undefined && i < 16; i++) {
        const owner = (current as { owner?: unknown }).owner;
        if (owner === undefined) return current;
        current = owner as { owner?: unknown };
    }
    return current;
}

// webrtcSessions maps a session id back to the camera that owns it, so ICE and
// teardown reach the right device.
const webrtcSessions = new SessionRegistry();

// invokeOn runs a cluster command on an endpoint. Shared by the WebRTC calls
// and the generic invokeCommand RPC.
async function invokeOn(
    target: unknown,
    cluster: string,
    command: string,
    args: Record<string, unknown>,
): Promise<unknown> {
    const commands = (target as {
        commandsOf(id: string): Record<string, (a?: unknown) => Promise<unknown>>;
    }).commandsOf(cluster);
    const fn = commands?.[command];
    if (typeof fn !== "function") {
        throw new RpcError(
            "unsupported",
            `matter: command ${cluster}.${command} not available on this device`,
        );
    }
    return await fn(args);
}

// How long the device gets to accept decommissioning before we give up and
// remove it locally. Long enough for a couple of MRP retries on a healthy
// network, short enough that a user deleting an unplugged device is not left
// staring at a spinner.
const DECOMMISSION_TIMEOUT_MS = 12_000;

/** Runs work with a deadline; rejects with a readable error when it expires. */
async function withTimeout<T>(ms: number, work: () => Promise<T>): Promise<T> {
    let timer: NodeJS.Timeout | undefined;
    const expiry = new Promise<never>((_, reject) => {
        timer = setTimeout(() => reject(new Error(`timed out after ${ms}ms`)), ms);
    });
    try {
        return await Promise.race([work(), expiry]);
    } finally {
        if (timer) clearTimeout(timer);
    }
}

// --- commissionable discovery ---

const DISCOVERY_DEFAULT_MS = 10_000;
const DISCOVERY_MAX_MS = 60_000;

// Devices the last scans turned up, by ref. Keeps the ClientNode matter.js
// built for each advertisement so a later commission can target it directly
// instead of re-discovering by discriminator (two identical lamps in pairing
// mode are otherwise indistinguishable).
//
// Entries are replaced by later scans and dropped once commissioned. They are
// cheap: a ClientNode for an uncommissioned device holds only its
// advertisement.
const commissionableByRef = new Map<string, ClientNode>();

// isCommissioned distinguishes nodes in our fabric from ones we merely found
// advertising themselves. peerAddress is assigned when a node joins the fabric.
function isCommissioned(client: ClientNode): boolean {
    try {
        return (client as unknown as { peerAddress?: unknown }).peerAddress !== undefined;
    } catch {
        return false;
    }
}

// commissionableFrom projects a discovered node into the wire shape. Everything
// available here comes from the advertisement — no interview has happened, so
// there are no clusters, and notably no passcode: the user still has to supply
// the setup code from the device or its box.
function commissionableFrom(client: ClientNode): CommissionableDevice | undefined {
    let state: Record<string, unknown>;
    try {
        state = (client as unknown as { state: { commissioning: Record<string, unknown> } }).state.commissioning;
    } catch {
        return undefined;
    }
    if (state === undefined) return undefined;

    const num = (v: unknown) => (typeof v === "number" && Number.isFinite(v) ? v : undefined);
    const discriminator = num(state.discriminator);

    // deviceIdentifier is the canonical global ID matter.js derives for the
    // advertisement; fall back to the discriminator, then to the local node id.
    const ref = typeof state.deviceIdentifier === "string" && state.deviceIdentifier !== ""
        ? state.deviceIdentifier
        : discriminator !== undefined
            ? `discriminator:${discriminator}`
            : String((client as unknown as { id?: string }).id ?? "");
    if (ref === "") return undefined;

    const deviceTypeId = num(state.deviceType);

    return {
        ref,
        name: typeof state.deviceName === "string" && state.deviceName !== "" ? state.deviceName : undefined,
        deviceType: deviceTypeId !== undefined ? deviceTypeNamesById.get(deviceTypeId) : undefined,
        vendorId: num(state.vendorId),
        productId: num(state.productId),
        discriminator,
    };
}

// passcodeOf extracts the numeric passcode from decoded setup-code options.
// Targeted commissioning supplies the device itself, so only the secret is
// needed from the code — but it *is* needed: PASE cannot start without it, and
// no advertisement ever carries it.
function passcodeOf(options: SetupCodeOptions): number {
    if ("passcode" in options) return options.passcode;
    const decoded = ManualPairingCodeCodec.decode(options.pairingCode);
    return decoded.passcode;
}

// --- commissioning progress ---
//
// matter.js has no progress observable on CommissioningClient, but it does let
// you supply the commissioning flow implementation. Subclassing it and wrapping
// each step is the supported way to watch the process, and gives us the CSA
// step names as they actually execute — no timers, no guessing.
//
// The sink is process-wide because matter.js constructs the flow itself and
// gives us no place to thread state through. commission() therefore refuses to
// run two sessions at once.
type ProgressSink = (stage: string, message?: string) => void;
let progressSink: ProgressSink | undefined;
let commissionInFlight = false;

function reportProgress(stage: string, message?: string): void {
    progressSink?.(stage, message);
}

// COMMISSIONING_STAGES translates CSA step names into the handful of stages a
// user can act on. Several steps collapse into one stage on purpose: nobody
// needs to see "ArmFailsafe" and "ConfigureRegulatoryInformation" as separate
// events. Unlisted steps report as "configuring".
const COMMISSIONING_STAGES: Record<string, { stage: string; message: string }> = {
    "GetInitialData": { stage: "configuring", message: "reading device information" },
    "GeneralCommissioning.ArmFailsafe": { stage: "configuring", message: "preparing the device" },
    "GeneralCommissioning.ConfigureRegulatoryInformation": { stage: "configuring", message: "preparing the device" },
    "TimeSynchronization.SynchronizeTime": { stage: "configuring", message: "synchronising time" },
    "OperationalCredentials.DeviceAttestation": { stage: "attesting", message: "verifying device certificate" },
    "OperationalCredentials.Certificates": { stage: "provisioning", message: "installing fabric credentials" },
    "AccessControl": { stage: "provisioning", message: "granting access" },
    "NetworkCommissioning.Validate": { stage: "network", message: "checking network settings" },
    "NetworkCommissioning.Wifi": { stage: "network", message: "joining Wi-Fi" },
    "NetworkCommissioning.Thread": { stage: "network", message: "joining Thread" },
    "Reconnect": { stage: "connecting", message: "reconnecting over the operational network" },
    "GeneralCommissioning.Complete": { stage: "finalizing", message: "completing commissioning" },
    "OperationalCredentials.UpdateFabricLabel": { stage: "finalizing", message: "labelling the fabric" },
    "AdditionalLogic.AddDefaultOtaProvider": { stage: "finalizing", message: "configuring updates" },
};

// ProgressReportingFlow wraps every step's logic with a progress report. The
// steps array is populated by the base constructor, so wrapping after super()
// covers all of them. Behaviour is otherwise untouched — we only observe.
class ProgressReportingFlow extends ControllerCommissioningFlow {
    constructor(...args: ConstructorParameters<typeof ControllerCommissioningFlow>) {
        super(...args);
        for (const step of this.commissioningSteps) {
            const inner = step.stepLogic;
            step.stepLogic = () => {
                const mapped = COMMISSIONING_STAGES[step.name] ?? {
                    stage: "configuring",
                    message: step.name,
                };
                reportProgress(mapped.stage, mapped.message);
                return inner.call(step);
            };
        }
    }
}

// decodeSetupCode turns whatever the user pasted into commissioning options.
//
// matter.js only understands the 11/21-digit manual code via its `pairingCode`
// option — a QR payload ("MT:...") throws there — so QR is decoded here and
// passed as explicit passcode + discriminator.
type SetupCodeOptions =
    | { pairingCode: string }
    | { passcode: number; discriminator: number; longDiscriminator: number };

function decodeSetupCode(raw: string): SetupCodeOptions {
    const code = (raw ?? "").trim();
    if (code === "") {
        throw new RpcError("bad_request", "matter: setup code is empty");
    }

    if (code.toUpperCase().startsWith("MT:")) {
        let payloads;
        try {
            payloads = QrPairingCodeCodec.decode(code.toUpperCase());
        } catch (err) {
            throw new RpcError("bad_request", `matter: malformed QR setup code: ${String(err)}`, { cause: err });
        }
        const payload = payloads?.[0];
        if (!payload) {
            throw new RpcError("bad_request", "matter: QR setup code carried no payload");
        }
        // A single QR may describe several devices (concatenated payloads).
        // Commissioning them all is a separate feature; take the first and be
        // explicit rather than silently picking one.
        if (payloads.length > 1) {
            throw new RpcError(
                "bad_request",
                `matter: QR code contains ${payloads.length} devices; multi-device codes are not supported yet`,
            );
        }
        // The QR payload carries the *long* discriminator; matter.js wants it
        // twice — once to find the device (longDiscriminator) and once as part
        // of the commissioning options (discriminator). discoveryCapabilities
        // is deliberately dropped: matter.js models it as a bitmap object while
        // the codec yields a raw number, and it only matters for BLE, which
        // this sidecar does not use.
        return {
            passcode: payload.passcode,
            discriminator: payload.discriminator,
            longDiscriminator: payload.discriminator,
        };
    }

    // Manual codes are commonly written with spaces or dashes on the label.
    const digits = code.replace(/[\s-]/g, "");
    if (!/^\d{11}$|^\d{21}$/.test(digits)) {
        throw new RpcError(
            "bad_request",
            `matter: setup code must be an 11- or 21-digit manual code, or a QR payload starting with "MT:"`,
        );
    }
    // matter.js decodes the manual code itself (passcode + short discriminator).
    return { pairingCode: digits };
}

// --- helpers ---

function clientFor(node: ServerNode, nodeId: string): ClientNode {
    // Peers.get looks up by internal endpoint id, not fabric nodeId. Match by
    // walking peers and comparing the value nodeIdOf returned to keystone.
    for (const client of node.peers) {
        if (nodeIdOf(client) === nodeId) return client;
    }
    throw new RpcError("not_found", `matter: node ${nodeId} not found`);
}

// superviseStateStream keeps exactly one live StateStream consumer running for
// as long as `abort` is unset. The stream is the only realtime path we have —
// if it dies, the sidecar keeps answering RPCs and looks perfectly healthy
// while no device change ever reaches keystone again. So: log loudly, back off,
// restart. Backoff resets once a stream has survived long enough to count as
// working, so a nightly blip doesn't leave us at a 30s retry forever.
const STREAM_RETRY_MIN_MS = 1_000;
const STREAM_RETRY_MAX_MS = 30_000;
const STREAM_HEALTHY_MS = 30_000;

async function superviseStateStream(
    node: ServerNode,
    emitter: EventEmitter,
    abort: AbortSignal,
    log: ControllerLog,
): Promise<void> {
    let backoffMs = STREAM_RETRY_MIN_MS;

    while (!abort.aborted) {
        const startedAt = Date.now();
        try {
            await consumeStateStream(node, emitter, abort);
            if (abort.aborted) return;
            log("warn", "matter state stream ended unexpectedly", { retryInMs: backoffMs });
        } catch (err) {
            if (abort.aborted) return;
            log("error", "matter state stream failed", { err: String(err), retryInMs: backoffMs });
        }

        if (Date.now() - startedAt >= STREAM_HEALTHY_MS) {
            backoffMs = STREAM_RETRY_MIN_MS;
        }
        await delay(backoffMs, abort);
        backoffMs = Math.min(backoffMs * 2, STREAM_RETRY_MAX_MS);

        // A restarted stream starts from scratch and only reports changes from
        // here on, so anything that moved during the outage is invisible.
        // Re-publish cached state to close the gap.
        if (!abort.aborted) {
            for (const client of node.peers) snapshotOf(client, emitter, log);
        }
    }
}

// delay resolves after ms, or immediately when abort fires.
function delay(ms: number, abort: AbortSignal): Promise<void> {
    return new Promise((resolve) => {
        if (abort.aborted) return resolve();
        const timer = setTimeout(() => {
            abort.removeEventListener("abort", onAbort);
            resolve();
        }, ms);
        const onAbort = () => {
            clearTimeout(timer);
            resolve();
        };
        abort.addEventListener("abort", onAbort, { once: true });
    });
}

// consumeStateStream reads the StateStream generator and translates every
// peer property update into an `attributeChanged` event on the emitter. This
// is the ONE realtime path for peer attribute changes in matter.js 0.17 —
// per-cluster `$Changed` observables don't fire for remote peers because peer
// state lives in a Datasource, not in the behavior event registry.
//
// The stream is coalesced by matter.js (DEFAULT_COALESCE_INTERVAL) so a
// physical brightness sweep from the Home app arrives here as a small number
// of merged updates rather than one per LevelControl report frame.
async function consumeStateStream(
    node: ServerNode,
    emitter: EventEmitter,
    abort: AbortSignal,
): Promise<void> {
    const stream = StateStream(node, { abort });
    try {
        for await (const change of stream) {
            if (change.kind !== "update") continue;
            // Drop updates on the controller root itself — those are our own
            // commissioning / general-diagnostics fields, not device state.
            if (change.node === node) continue;

            const peer = change.node as ClientNode;
            const nodeId = nodeIdOf(peer);
            let endpointId: number | undefined;
            try {
                endpointId = (change.endpoint as unknown as { maybeNumber?: number; number?: number }).maybeNumber
                    ?? (change.endpoint as unknown as { number?: number }).number;
            } catch { /* ignore */ }
            if (endpointId === undefined) continue;

            const clusterKey = (change.behavior as unknown as { id?: string }).id;
            if (!clusterKey) continue;
            const cluster = canonicalClusterName(clusterKey);

            for (const [attrKey, value] of Object.entries(change.changes)) {
                emitter.emit("attributeChanged", {
                    nodeId,
                    endpointId,
                    cluster,
                    attribute: pascalCase(attrKey),
                    value,
                });
            }
        }
    } catch (err) {
        if ((err as { name?: string }).name === "AbortError") return;
        // Let superviseStateStream decide — it owns logging and restart.
        throw err;
    }
}

// ensureAutoSubscribe guarantees a peer carries `network.autoSubscribe`.
//
// matter.js already does the right thing on its own: CommissioningClient sets
// autoSubscribe on every node it commissions, the flag is persisted
// (quality "N"), and NetworkClient.startup() re-establishes the subscription
// on every boot — a wildcard `attributes: [{}]` subscription, which is why
// StateStream sees peer changes at all. So this is not the subscribe call
// itself, just a self-heal: a peer that landed in the fabric with the flag off
// (older matter.js, an explicit autoSubscribe:false, a hand-edited store) would
// otherwise stay permanently silent with nothing in the logs to say why.
async function ensureAutoSubscribe(client: ClientNode, log: ControllerLog): Promise<void> {
    const nodeId = nodeIdOf(client);
    try {
        const network = (client as unknown as {
            stateOf(id: string): Record<string, unknown>;
        }).stateOf("network");
        if (network?.autoSubscribe === true) return;

        await (client as unknown as {
            setStateOf(id: string, values: Record<string, unknown>): Promise<void>;
        }).setStateOf("network", { autoSubscribe: true });
        log("warn", "matter peer had autoSubscribe off — enabled it", { nodeId });
    } catch (err) {
        log("warn", "matter peer autoSubscribe check failed", { nodeId, err: String(err) });
    }
}

// snapshotOf emits the cached state of every cluster keystone cares about on
// every endpoint of a peer, as ordinary `attributeChanged` events. Values come
// from the local matter.js cache (filled by the interview and kept fresh by
// subscription reports), so this is cheap and does not hit the network.
//
// Needed because StateStream's initial pass enumerates
// `endpoint.behaviors.supported`, which is the server-side behavior registry
// and stays empty for remote peers — peers therefore produce *changes* only,
// never an opening state.
function snapshotOf(client: ClientNode, emitter: EventEmitter, log: ControllerLog): void {
    const nodeId = nodeIdOf(client);
    let emitted = 0;

    for (const { endpointId, clusters } of endpointsOf(client)) {
        for (const cluster of clusters) {
            if (!SNAPSHOT_CLUSTERS.has(cluster)) continue;
            let state: Record<string, unknown>;
            try {
                const target = endpointFor(client, endpointId) as unknown as {
                    stateOf(id: string): Record<string, unknown>;
                };
                state = target.stateOf(camelCase(cluster));
            } catch {
                continue; // cluster listed in the descriptor but not readable yet
            }
            for (const [attrKey, value] of Object.entries(state ?? {})) {
                if (typeof value === "function") continue;
                emitter.emit("attributeChanged", {
                    nodeId,
                    endpointId,
                    cluster,
                    attribute: pascalCase(attrKey),
                    value,
                });
                emitted++;
            }
        }
    }

    log("info", "matter state snapshot published", { nodeId, attributes: emitted });
}


// Cluster and device-type names come from the CSA data model matter.js already
// ships (141 clusters, 92 device types), not from a table we maintain by hand.
// A hand-written whitelist meant every new device class needed a code change
// before keystone could even see its clusters.
//
// LEGACY_CLUSTER_NAMES covers IDs the current data model dropped: 0x0B04 is
// the Zigbee-derived ElectricalMeasurement, still shipped by plenty of
// certified plugs and still mapped on the keystone side.
const LEGACY_CLUSTER_NAMES: Record<number, string> = {
    0x0B04: "ElectricalMeasurement",
};

const clusterNamesById = new Map<number, string>(Object.entries(LEGACY_CLUSTER_NAMES).map(
    ([id, name]) => [Number(id), name] as [number, string],
));
const clusterNamesByPropertyKey = new Map<string, string>();
const deviceTypeNamesById = new Map<number, string>();

for (const cluster of MatterModel.standard.clusters) {
    if (typeof cluster.id === "number") clusterNamesById.set(cluster.id, cluster.name);
    // matter.js addresses behaviors by camelCase property key ("onOff"); the
    // wire contract with keystone uses the spec's canonical name ("OnOff").
    // Deriving one from the other by upper-casing the first letter breaks on
    // abbreviations (otaSoftwareUpdate → OtaSoftwareUpdate ≠ OtaSoftwareUpdate
    // spelling in the spec), so map the real names instead of guessing.
    clusterNamesByPropertyKey.set(camelCase(cluster.name), cluster.name);
}
for (const deviceType of MatterModel.standard.deviceTypes) {
    if (typeof deviceType.id === "number") deviceTypeNamesById.set(deviceType.id, deviceType.name);
}

// clusterNameFor resolves a numeric cluster id to its canonical name, or
// undefined for ids the data model doesn't know (vendor-specific clusters).
function clusterNameFor(id: number): string | undefined {
    return clusterNamesById.get(id);
}

// canonicalClusterName maps a matter.js behavior id ("onOff") to the spec name
// ("OnOff"), falling back to naive capitalisation for behaviors that aren't
// clusters at all (parts, index, commissioning).
function canonicalClusterName(behaviorId: string): string {
    return clusterNamesByPropertyKey.get(behaviorId) ?? pascalCase(behaviorId);
}

// SNAPSHOT_CLUSTERS limits what publishSnapshot walks. listNodes reports every
// cluster a peer has and lets keystone filter, but a snapshot reads and emits
// actual values — walking AccessControl or OperationalCredentials would push
// ACL entries and fabric descriptors across the wire for nothing.
const SNAPSHOT_CLUSTERS = new Set([
    // Lighting and power
    "OnOff",
    "LevelControl",
    "ColorControl",
    // Environment
    "TemperatureMeasurement",
    "RelativeHumidityMeasurement",
    "OccupancySensing",
    "BooleanState",
    "IlluminanceMeasurement",
    "PressureMeasurement",
    "FlowMeasurement",
    "AirQuality",
    "Pm25ConcentrationMeasurement",
    "Pm10ConcentrationMeasurement",
    "CarbonDioxideConcentrationMeasurement",
    "TotalVolatileOrganicCompoundsConcentrationMeasurement",
    "FormaldehydeConcentrationMeasurement",
    "SmokeCoAlarm",
    // Input
    "Switch",
    // Closures
    "DoorLock",
    "WindowCovering",
    // Climate
    "Thermostat",
    "FanControl",
    // Appliances
    "OperationalState",
    "RvcOperationalState",
    "RvcRunMode",
    "LaundryWasherMode",
    "DishwasherMode",
    "RefrigeratorAndTemperatureControlledCabinetMode",
    "WaterHeaterMode",
    "EnergyEvseMode",
    "TemperatureControl",
    // Media
    "MediaPlayback",
    // Energy
    "ElectricalMeasurement",
    "ElectricalPowerMeasurement",
    "ElectricalEnergyMeasurement",
    "EnergyEvse",
    "PowerSource",
]);

// endpointsOf walks the endpoint tree of a ClientNode and returns the shape
// keystone shows in listNodes. For a commissioned peer, cluster info lives on
// the Descriptor cluster's ServerList attribute (a numeric cluster-id list),
// NOT on behaviors.supported — that registry is for server-side behaviors
// this controller hosts, and stays empty for remote peers.
function endpointsOf(client: ClientNode): Array<{ endpointId: number; deviceType?: string; clusters: string[] }> {
    const out: Array<{ endpointId: number; deviceType?: string; clusters: string[] }> = [];
    const collect = (ep: any) => {
        let num: number | undefined;
        try { num = ep.maybeNumber ?? ep.number; } catch { num = undefined; }

        const clusters: string[] = [];
        let deviceType: string | undefined;

        // Primary source: peer's own Descriptor (works post-interview).
        // ServerList is the numeric cluster-id list; DeviceTypeList carries
        // {deviceType, revision} entries and is the only authoritative answer
        // to "what kind of thing is this endpoint" — inferring the type from
        // the cluster set guesses wrong constantly (a plug and a light both
        // expose OnOff, a thermostat looks like a temperature sensor).
        try {
            const desc = (ep as { stateOf(id: string): Record<string, unknown> }).stateOf("descriptor");

            const serverList = desc?.serverList as unknown;
            if (Array.isArray(serverList)) {
                for (const raw of serverList) {
                    const id = typeof raw === "number" ? raw : Number(raw);
                    if (!Number.isFinite(id)) continue;
                    const name = clusterNameFor(id);
                    if (name) clusters.push(name);
                }
            }

            const deviceTypeList = desc?.deviceTypeList as unknown;
            if (Array.isArray(deviceTypeList)) {
                // An endpoint may declare several device types (a base type
                // plus e.g. a Matter 1.3 electrical-sensor overlay). The first
                // recognised one wins — the list is ordered most-specific first.
                for (const entry of deviceTypeList) {
                    const rawId = (entry as { deviceType?: unknown })?.deviceType ?? entry;
                    const id = typeof rawId === "number" ? rawId : Number(rawId);
                    if (!Number.isFinite(id)) continue;
                    const name = deviceTypeNamesById.get(id);
                    if (name) { deviceType = name; break; }
                }
            }
        } catch { /* endpoint not interviewed yet or descriptor missing */ }

        // Fallback: local behaviors (empty for peers, non-empty for the
        // controller root — cheap safety net).
        if (clusters.length === 0) {
            try {
                const supported = ep.behaviors?.supported ?? {};
                for (const key of Object.keys(supported)) {
                    clusters.push(canonicalClusterName(key));
                }
            } catch { /* ignore */ }
        }

        if (num !== undefined) out.push({ endpointId: num, deviceType, clusters });
        try {
            for (const child of ep.parts ?? []) collect(child);
        } catch { /* ignore */ }
    };
    collect(client);
    return out;
}

// basicInfoOf reads the peer's identity from BasicInformation on endpoint 0.
// Without it every device lands in the UI as "Matter device 3" with no
// manufacturer and no model, and the type heuristics have nothing to work with.
function basicInfoOf(client: ClientNode): {
    vendorName?: string;
    productName?: string;
    vendorId?: number;
    productId?: number;
    nodeLabel?: string;
} {
    try {
        const info = (client as unknown as {
            stateOf(id: string): Record<string, unknown>;
        }).stateOf("basicInformation");
        const str = (v: unknown) => (typeof v === "string" && v !== "" ? v : undefined);
        const num = (v: unknown) => (typeof v === "number" && Number.isFinite(v) ? v : undefined);
        return {
            vendorName: str(info?.vendorName),
            productName: str(info?.productName),
            vendorId: num(info?.vendorId),
            productId: num(info?.productId),
            // NodeLabel is the user-assigned name from whichever ecosystem
            // commissioned the device first — usually the nicest label we have.
            nodeLabel: str(info?.nodeLabel),
        };
    } catch {
        return {};
    }
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
        throw new RpcError("not_found", `matter: endpoint ${endpointId} not found on node ${nodeIdOf(client)}`);
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

// pascalCase is the inverse — used on the outbound wire so events match the
// Go-side (Cluster, Attribute) tables that key on PascalCase.
function pascalCase(name: string): string {
    if (!name) return name;
    return name.charAt(0).toUpperCase() + name.slice(1);
}

function assertStarted(node: ServerNode | undefined): ServerNode {
    // not_ready, not internal: the controller is still coming up and the same
    // call will work shortly, so keystone should retry rather than surface a
    // hard failure to the user.
    if (!node) throw new RpcError("not_ready", "matter controller not started");
    return node;
}

function notImplemented(what: string): Error {
    return new Error(`matter controller: ${what} not yet implemented — see TODO(matter-attr) in controller.ts`);
}
