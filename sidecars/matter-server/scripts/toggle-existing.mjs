#!/usr/bin/env node
// Toggle the first commissioned node in the fabric on and off.
// Assumes the sidecar is already running against a storage that survived
// a previous commission — i.e. re-uses an existing fabric.
//
// Usage:
//   node scripts/toggle-existing.mjs

import WebSocket from "ws";

const URL = process.env.MATTER_SIDECAR_URL ?? "ws://localhost:5580";
const ws = new WebSocket(URL);
const pending = new Map();
let nextId = 0;

function call(method, params) {
    const id = String(++nextId);
    return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        ws.send(JSON.stringify({ id, method, params }));
    });
}

ws.on("message", (buf) => {
    const frame = JSON.parse(buf.toString());
    if (frame.event) {
        console.log(`[event ${frame.event}]`, JSON.stringify(frame.data));
        return;
    }
    const waiter = pending.get(frame.id);
    if (!waiter) return;
    pending.delete(frame.id);
    if (frame.error) waiter.reject(new Error(`RPC error ${frame.error.code}: ${frame.error.message}`));
    else waiter.resolve(frame.result);
});

ws.on("open", async () => {
    try {
        console.log(`[+] connected to ${URL}`);
        const nodes = await call("listNodes", null);
        console.log(`[+] ${nodes.length} node(s): ${nodes.map(n => n.nodeId).join(", ") || "(none)"}`);
        if (nodes.length === 0) {
            console.log("[!] no commissioned nodes; run commission-interactive.sh first");
            ws.close();
            return;
        }
        // Pick the first (node, endpoint) that exposes an OnOff cluster.
        let target = null;
        let endpointId = 1;
        for (const n of nodes) {
            for (const ep of n.endpoints ?? []) {
                if ((ep.clusters ?? []).includes("onOff")) {
                    target = n.nodeId;
                    endpointId = ep.endpointId;
                    break;
                }
            }
            if (target) break;
        }
        if (!target) {
            console.log("[!] no node exposes an OnOff cluster");
            ws.close();
            return;
        }
        console.log(`[+] target nodeId=${target} endpoint=${endpointId}, sending On`);
        await call("invokeCommand", { nodeId: target, endpointId, cluster: "OnOff", command: "On" });
        await new Promise(r => setTimeout(r, 1500));
        console.log(`[+] sending Off`);
        await call("invokeCommand", { nodeId: target, endpointId, cluster: "OnOff", command: "Off" });
        console.log("[+] done");
        ws.close();
    } catch (err) {
        console.error("[!] failed:", err.message ?? err);
        ws.close();
        process.exit(1);
    }
});

ws.on("close", () => process.exit(0));
ws.on("error", (err) => { console.error("[!] ws error:", err.message); process.exit(1); });
