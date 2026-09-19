# recorder-cam (ESP32-CAM) — stub

Placeholder for **Phase 7**. The recorder is a **read-only** bus listener: it
receives `STATE` events and appends them to an event log on SD with timestamps,
and also logs `boot_id` changes and detected gaps as history annotations. It
**never** sends `COMMAND` (SPEC §2.2) — if it dies, home logic keeps working and
only history is lost.

Not implemented yet. Do not start this before Phase 6 passes (SPEC §9).

Flashing note: the ESP32-CAM has no USB and is flashed via an external FTDI
adapter (SPEC §10).
