#!/usr/bin/env node
// Manual end-to-end tester: commissions a Matter device via the sidecar,
// then attempts On -> pause -> Off to confirm read/write survive the WS
// round trip.
//
// Usage:
//   SETUP_CODE=12345678901 node scripts/commission-and-toggle.mjs
//
// The setup code may be given with or without dashes.

import WebSocket from "ws";

const URL = process.env.MATTER_SIDECAR_URL ?? "ws://localhost:5580";
const rawCode = process.env.SETUP_CODE;
if (!rawCode) {
    console.error("SETUP_CODE env var required (11 digits, dashes optional)");
    process.exit(2);
}
const setupCode = rawCode.replace(/[^0-9]/g, "");
if (setupCode.length < 11) {
    console.error(`SETUP_CODE too short after stripping dashes: got ${setupCode.length} chars, need 11+`);
    process.exit(2);
}

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
        console.log(`[+] commissioning with setup code ${setupCode}...`);
        const before = await call("listNodes", null);
        console.log(`    before: ${before.length} nodes`);

        const commissioned = await call("commission", { setupCode });
        console.log(`[+] commissioned: nodeId=${commissioned.nodeId} fabricIndex=${commissioned.fabricIndex}`);

        const after = await call("listNodes", null);
        console.log(`    after: ${after.length} nodes -> ${after.map(n => n.nodeId).join(", ")}`);

        const target = commissioned.nodeId;
        console.log(`[+] turning ON`);
        await call("invokeCommand", { nodeId: target, endpointId: 1, cluster: "OnOff", command: "On" });

        await new Promise(r => setTimeout(r, 1500));

        console.log(`[+] turning OFF`);
        await call("invokeCommand", { nodeId: target, endpointId: 1, cluster: "OnOff", command: "Off" });

        console.log(`[+] done, closing`);
        ws.close();
    } catch (err) {
        console.error("[!] failed:", err.message ?? err);
        ws.close();
        process.exit(1);
    }
});

ws.on("close", () => process.exit(0));
ws.on("error", (err) => { console.error("[!] ws error:", err.message); process.exit(1); });
