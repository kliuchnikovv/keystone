#!/usr/bin/env node
// Print the endpoint / cluster shape of every commissioned node as
// matter.js currently sees it. Useful when invokeCommand or readAttribute
// fail with "cluster X not present on endpoint Y" — the output shows what
// keys are actually available so the wire mapping can be corrected.
//
// Usage:  node scripts/describe.mjs

import WebSocket from "ws";

const ws = new WebSocket(process.env.MATTER_SIDECAR_URL ?? "ws://localhost:5580");
ws.on("open", () => ws.send(JSON.stringify({ id: "1", method: "listNodes" })));
ws.on("message", (buf) => {
    const frame = JSON.parse(buf.toString());
    if (frame.event) return;
    if (frame.error) {
        console.error("RPC error:", frame.error);
    } else {
        for (const node of frame.result) {
            console.log(`node ${node.nodeId} (online=${node.online})`);
            for (const ep of node.endpoints) {
                console.log(`  endpoint#${ep.endpointId}`);
                for (const c of ep.clusters) console.log(`    - ${c}`);
            }
        }
    }
    ws.close();
});
ws.on("close", () => process.exit(0));
ws.on("error", (err) => { console.error(err.message); process.exit(1); });
