// Sidecar entry point. Boots a matter.js controller and exposes a small
// JSON-RPC surface over WebSocket for the Go core to drive.
//
// Configuration is read from env vars so docker-compose / systemd can
// override without touching code:
//
//   KEYSTONE_MATTER_HOST      default 0.0.0.0
//   KEYSTONE_MATTER_PORT      default 5580
//   KEYSTONE_MATTER_STORAGE   default ./matter-data
//   KEYSTONE_MATTER_LABEL     fabric label, default "Keystone"

// The storage path must be set BEFORE any @matter/* import: NodeJsEnvironment
// resolves storage.path when Environment.default is first constructed, which
// happens as a side-effect of the very first matter.js import.
const STORAGE = process.env.KEYSTONE_MATTER_STORAGE ?? "./matter-data";
if (!process.env.MATTER_STORAGE_PATH) process.env.MATTER_STORAGE_PATH = STORAGE;

const { createController } = await import("./controller.js");
const { startWsServer } = await import("./wsServer.js");

const HOST = process.env.KEYSTONE_MATTER_HOST ?? "0.0.0.0";
const PORT = Number(process.env.KEYSTONE_MATTER_PORT ?? 5580);
const LABEL = process.env.KEYSTONE_MATTER_LABEL ?? "Keystone";

const log = (
    level: "info" | "warn" | "error",
    msg: string,
    meta?: Record<string, unknown>,
) => {
    const entry = { ts: new Date().toISOString(), level, msg, ...(meta ?? {}) };
    const line = JSON.stringify(entry);
    if (level === "error") console.error(line);
    else console.log(line);
};

async function main(): Promise<void> {
    log("info", "matter sidecar starting", { host: HOST, port: PORT, storage: STORAGE });

    const controller = createController({ storagePath: STORAGE, fabricLabel: LABEL });
    await controller.start();

    const server = startWsServer({ host: HOST, port: PORT, controller, log });

    const shutdown = async (signal: string) => {
        log("info", "shutting down", { signal });
        try {
            await server.close();
            await controller.stop();
        } catch (err) {
            log("error", "shutdown failed", { err: String(err) });
        }
        process.exit(0);
    };

    process.on("SIGINT", () => void shutdown("SIGINT"));
    process.on("SIGTERM", () => void shutdown("SIGTERM"));
    process.on("unhandledRejection", (err) => log("error", "unhandled rejection", { err: String(err) }));
}

main().catch((err) => {
    log("error", "fatal", { err: String(err) });
    process.exit(1);
});

export {};
