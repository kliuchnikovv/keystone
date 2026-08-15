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

    const controller = createController({ storagePath: STORAGE, fabricLabel: LABEL, log });
    await controller.start();

    const server = startWsServer({ host: HOST, port: PORT, controller, log });

    // Each shutdown step gets its own deadline. Unbounded, a wedged matter.js
    // close (a peer that won't finish a session teardown) hangs the process
    // until systemd/docker escalates to SIGKILL — and a SIGKILL mid-write is
    // exactly how the fabric store on disk ends up inconsistent. Better to
    // abandon a slow step and exit deliberately.
    const SHUTDOWN_STEP_MS = 5_000;

    const withDeadline = async (name: string, step: Promise<unknown>): Promise<void> => {
        let timer: NodeJS.Timeout | undefined;
        const expired = Symbol("expired");
        const deadline = new Promise<typeof expired>((resolve) => {
            timer = setTimeout(() => resolve(expired), SHUTDOWN_STEP_MS);
        });
        try {
            const result = await Promise.race([step.then(() => "done" as const), deadline]);
            if (result === expired) {
                log("warn", "shutdown step timed out, continuing", { step: name, timeoutMs: SHUTDOWN_STEP_MS });
            }
        } catch (err) {
            log("error", "shutdown step failed", { step: name, err: String(err) });
        } finally {
            if (timer) clearTimeout(timer);
        }
    };

    let shuttingDown = false;
    const shutdown = async (signal: string) => {
        // A second Ctrl-C (or SIGTERM after SIGINT) should not start a parallel
        // teardown of the same resources.
        if (shuttingDown) {
            log("warn", "shutdown already in progress, forcing exit", { signal });
            process.exit(1);
        }
        shuttingDown = true;

        log("info", "shutting down", { signal });
        // Stop accepting work before tearing down the controller, so no RPC
        // lands on a half-closed matter.js node.
        await withDeadline("ws-server", server.close());
        await withDeadline("controller", controller.stop());
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
