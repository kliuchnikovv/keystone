# WIP — Matter accessory + bridge back on thread-c6 (F2 Stage A)

Short-lived status file. Delete once Stage A is on main.

**Branch:** `matter/bridge-on-thread-c6` (local, not pushed, not committed).
**Goal:** `docs/matter-backlog.md § 10 F2 Stage A` — minimal Matter accessory
on thread-c6 (root endpoint + aggregator Device Type 0x000E) next to the
existing Thread host, so Apple Home can add the hub as one accessory.
Controller stays on brain-s3.

## What is on disk, not committed

- `firmware/thread-c6/CMakeLists.txt` — conditional esp-matter wiring when
  `ESP_MATTER_PATH` is set; `set(IDF_TARGET esp32c6)` added so the device
  HAL / overlays resolve on a from-scratch configure.
- `firmware/thread-c6/main/CMakeLists.txt` — conditional
  `REQUIRES esp_matter esp_matter_bridge app_bridge` + `-DCHIP_HAVE_CONFIG_H`.
- `firmware/thread-c6/main/matter_accessory.cpp` — new file; builds node +
  aggregator + `esp_matter::start(event_cb)`. Gated on `CHIP_HAVE_CONFIG_H`
  (so a plain `idf.py build` without the Matter Docker image produces a
  working Thread host).
- `firmware/thread-c6/main/main.cpp` — `matter_accessory_start()` call after
  `wifi_bringup_connect()`, under `#ifdef CHIP_HAVE_CONFIG_H`.
- `firmware/thread-c6/main/idf_component.yml` — `espressif/esp_rcp_update`
  added (transitive dependency of CHIP on an OpenThread host path; the C6
  has native 802.15.4, nothing calls into it, but the link has to resolve).
- `firmware/thread-c6/sdkconfig.defaults.matter` — new overlay: BT + NimBLE,
  IPv6 autoconfig / num_addresses=6 / LwIP route+GW hooks, HKDF, bridge
  endpoint counts (22/1), OTA Requestor off (pulls `esp_rcp_update` extras
  we don't need yet).
- `scripts/matter-build.sh` — thread-c6 branch now calls
  `matter-patches.py --app thread-c6` and uses
  `SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.matter'`.

## Current blocker — linker errors on a from-scratch build

Build gets through compilation (1771/1781) and dies at the ELF link with
two problems — see `/tmp/.../scratchpad/clean_build3.log:2717-2723` while
the scratchpad lives, or reproduce with `scripts/matter-build.sh thread-c6`:

1. **Duplicate symbols** `lwip_hook_ip6_route` and `lwip_hook_nd6_get_gw` —
   both `libopenthread_br.a` (ESP-IDF) and `libCHIP.a`
   (`src/platform/ESP32/route_hook/ESP32RouteTable.c`) define them. Our
   overlay turns on `CONFIG_LWIP_HOOK_IP6_ROUTE_DEFAULT=y` /
   `CONFIG_LWIP_HOOK_ND6_GET_GW_DEFAULT=y` (needed by OpenThread BR), CHIP
   registers its own on top.

   **Fix direction:** find the CHIP-side knob that disables its route hook
   and put it in `sdkconfig.defaults.matter`.
   `grep -r 'ROUTE_HOOK\|RouteTable' /home/kliuchnikovv/esp/esp-matter/components/esp_matter/Kconfig
   /home/kliuchnikovv/esp/esp-matter/connectedhomeip/connectedhomeip/config/esp32/components/chip/Kconfig`
   should name it. The historical build (pre-`b9e8897`) did not have this
   duplicate because OpenThread BR was off then — CHIP owned the hook
   alone. Now BR is on, so CHIP's copy must be off, not BR's.

2. **Undefined reference** `mbedtls_hkdf` — CHIP uses it, mbedTLS was built
   without HKDF. Our overlay sets `CONFIG_MBEDTLS_HKDF_C=y`, so the
   question is whether the overlay actually loaded.

   **Check:** `grep -E '^Sequence of SDKCONFIG files' firmware/thread-c6/build/log/idf_py_stdout_output_*`
   should list `sdkconfig.defaults;sdkconfig.defaults.matter;sdkconfig.defaults.esp32c6`.
   If only the first file is there, the `SDKCONFIG_DEFAULTS=` env var did
   not make it into the Docker shell — then merge the Matter settings
   into base `sdkconfig.defaults` (ugly but reliable: the extra bits are
   harmless in a non-Matter build, they just add BT and a few lwIP knobs).

## Other notes from the session

- `matter_accessory.cpp:57` has an unused-result warning on
  `mgr.OpenBasicCommissioningWindow(...)` — wrap with `(void)` or branch on
  the result when touching that file.
- The CMake cache can hold on to a stale sources list from before
  `b9e8897` (we saw `keystone_bridged_device.cpp.obj` reappear in a
  build/). Full clean before every measurement:
  `docker run --rm -v $PWD:/project espressif/esp-matter:release-v1.6_idf_v5.5.5
   bash -c "rm -rf /project/firmware/thread-c6/build"`.
- Baseline measurement (Thread host only, no Matter linked, hardware):
  static 199 236 B (194.6 KiB), heap free 224 488 B (219.2 KiB), largest
  free block 204 800 B. See `docs/matter-backlog.md § 10` for the F2
  budget argument.

## When Stage A actually completes

1. Flash the thread-c6 image (`scripts/thread-dataset-probe.sh` finds it by
   MAC `AC:EB:E6:DA:FC:08`; no `erase_flash`).
2. `heap` on `thread>` — compare against the baseline above; log the
   number next to the base one in `docs/matter-backlog.md § 10`.
3. The aggregator is empty; Apple Home can add the hub but will see no
   bridged endpoints yet. That is Stage A done.

Stages B..E (restore `on_bus_forward`, bridged endpoints for chip 0x01 then
0x03, setup code/QR, app UX) are each their own commit; keep this file
until Stage A lands.
