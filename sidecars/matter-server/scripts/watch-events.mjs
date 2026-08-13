#!/usr/bin/env node
// Live-tail attribute-change events pushed by the sidecar.
// Connect, print every server event as it arrives, exit on Ctrl-C.
//
// Usage:  node scripts/watch-events.mjs

import WebSocket from "ws";

const ws = new WebSocket(process.env.MATTER_SIDECAR_URL ?? "ws://localhost:5580");
ws.on("open", () => {
    console.log(`[+] watching ${ws.url}, Ctrl-C to stop`);
    // Fire a listNodes so the connection isn't idle and any startup-time
    // events land while we're already attached.
    ws.send(JSON.stringify({ id: "1", method: "listNodes" }));
});
ws.on("message", (buf) => {
    const frame = JSON.parse(buf.toString());
    if (frame.event) {
        const ts = new Date().toISOString().slice(11, 23);
        console.log(`${ts}  ${frame.event}  ${JSON.stringify(frame.data)}`);
    } else if (frame.result !== undefined) {
        console.log(`[+] initial listNodes: ${JSON.stringify(frame.result)}`);
    }
});
ws.on("close", () => process.exit(0));
ws.on("error", (err) => { console.error("[!]", err.message); process.exit(1); });
process.on("SIGINT", () => ws.close());
