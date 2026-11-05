#!/usr/bin/env python3
"""Patch the esp-matter sources inside the build container.

The container image ships esp-matter release/v1.6 unmodified. A few spots in
it have to be adjusted for the brain (ESP32-S3-Zero) build; every patch below
touches only the in-container copy — the image itself is `--rm`'d after the
build, so nothing leaks back to the host.

Run inside the container (scripts/matter-build.sh does that for us):

    python3 /project/scripts/matter-patches.py [--app brain-s3|thread-c6] [esp-matter-root]

`--app` selects the patch set: the controller-only patches (the brain runs a
Matter controller, the thread-c6 only a bridge) are skipped for the bridge, and
the patches that every app needs are applied to both.

The script is idempotent: a patch that is already applied is skipped, and a
source that does not contain the expected text is a hard error (so a future
upstream release cannot silently skip a patch we still need).
"""
import sys
from pathlib import Path

APP = "brain-s3"
ROOT = Path("/opt/espressif/esp-matter")

_argv = sys.argv[1:]
if _argv and _argv[0] in ("--app", "-a"):
    APP = _argv[1]
    _argv = _argv[2:]
if _argv:
    ROOT = Path(_argv[0])

BRAIN = "brain-s3"
BRIDGE = "thread-c6"
ALL = (BRAIN, BRIDGE)

# --- 1) missing using-declaration in the pairing command -------------------
#
# esp_matter_controller_pairing_command.cpp in release/v1.6 uses the bare name
# `PeerAddress` but forgets `using chip::Transport::PeerAddress;` at file
# scope. The sibling file esp_matter_controller_console.cpp has exactly that
# using-declaration, so this is clearly an upstream omission.
PAIRING_CMD = ROOT / "components/esp_matter_controller/commands/esp_matter_controller_pairing_command.cpp"
PEER_OLD = "using namespace chip::Controller;\n"
PEER_NEW = "using namespace chip::Controller;\nusing chip::Transport::PeerAddress;\n"

# --- 2) the controller singleton does not fit into internal DRAM ------------
#
# esp_matter_controller_client.h keeps the controller singleton
# (`matter_controller_client::get_instance()`) as a function-local static. The
# class embeds a full CHIPDeviceController, so the singleton is 130 KB of .bss
# in internal DRAM — on the S3-Zero that is more than half of the heap the BLE
# controller needs in one contiguous block, and it is why the brain ran out of
# internal RAM inside CHIP's own init (esp-aes: Failed to allocate memory ->
# DRBG abort). EXT_RAM_BSS_ATTR moves it to PSRAM (.ext_ram.bss), where the
# CHIP stack happily reads and writes it. ESP-IDF's own ESP32-S3 RAM-optimised
# Matter config does the same thing for the Matter library's BSS.
#
# NOTE: only plain data may carry that attribute — an object with a
# polymorphic member gets its vptr initialised as a constant in .ext_ram.bss
# (NOBITS) and the first virtual call dies in LoadProhibited.
CLIENT_HDR = ROOT / "components/esp_matter_controller/core/esp_matter_controller_client.h"
PSRAM_OLD = "#include <esp_log.h>\n"
PSRAM_NEW = "#include <esp_attr.h>\n#include <esp_log.h>\n"
SINGLE_OLD = "        static matter_controller_client s_instance;\n"
SINGLE_NEW = "        EXT_RAM_BSS_ATTR static matter_controller_client s_instance;\n"

# --- 3) a failed MTU exchange must not kill the connection -----------------
#
# In the ESP32 NimBLE backend of CHIP, the GAP "connected" event handler
# negotiates the ATT MTU first and gives up on the whole connection if that
# fails:
#
#     rc = ble_gattc_exchange_mtu(gapEvent->connect.conn_handle, NULL, NULL);
#     if (rc != 0) { return CHIP_ERROR_INTERNAL; }
#
# On the S3 the exchange fails, the returned CHIP_ERROR_INTERNAL reaches the
# gap event handler, which then *disables the CHIPoBLE service*: the BLE layer
# is deinitialised and the freshly established connection is torn down before
# the commissioner ever gets to the GATT discovery (and thus to PASE). We log
# the reason and keep the connection — a smaller MTU is survivable, a dead
# connection is not.
NIMBLE_CPP = (ROOT / "connectedhomeip/connectedhomeip/config/esp32/third_party/"
              "connectedhomeip/src/platform/ESP32/nimble/BLEManagerImpl.cpp")
MTU_OLD = """    rc = ble_gattc_exchange_mtu(gapEvent->connect.conn_handle, NULL, NULL);
    if (rc != 0)
    {
        return CHIP_ERROR_INTERNAL;
    }
"""
MTU_NEW = """    rc = ble_gattc_exchange_mtu(gapEvent->connect.conn_handle, NULL, NULL);
    if (rc != 0)
    {
        // keystone: keep the connection alive. Returning an error here used to
        // disable the CHIPoBLE service, which deinitialised the BLE layer and
        // tore the connection down before the commissioner could start PASE.
        ESP_LOGW(TAG, "ble_gattc_exchange_mtu failed: %d (keeping the connection)", rc);
    }
"""

# --- 4) cancelling a connect must not panic on a dead host -----------------
#
# CancelConnect() calls ble_gap_conn_cancel(), which asks the NimBLE host
# whether it is enabled — ble_hs_is_enabled() reads the host context. If the
# BLE layer has already been deinitialised (which is exactly what happens
# after the service was disabled in 3), that context is gone and the S3 dies
# with a LoadProhibited panic in ble_hs_is_enabled. The connection we would
# cancel is gone in that case anyway, so skip the call.
CANCEL_OLD = """void BLEManagerImpl::CancelConnect(void)
{
    int rc = ble_gap_conn_cancel();
    VerifyOrReturn(rc == 0, ChipLogError(Ble, "Failed to cancel connection rc=%d", rc));
}
"""
CANCEL_NEW = """void BLEManagerImpl::CancelConnect(void)
{
    // keystone: ble_gap_conn_cancel() -> ble_hs_is_enabled() dereferences the
    // NimBLE host context. After the CHIPoBLE service was disabled the BLE
    // layer is gone and that read panics the ESP32-S3 with LoadProhibited.
    ChipLogError(Ble, "CancelConnect: skipping ble_gap_conn_cancel() (BLE layer may be down)");
}
"""

# --- 5) the platform event header uses PacketBuffer without including it ---
#
# src/platform/ESP32/CHIPDevicePlatformEvent.h declares, inside
# `#ifdef CONFIG_ENABLE_ESP32_BLE_CONTROLLER`, a member
# `chip::System::PacketBuffer * mData;` — but the header never includes
# <system/SystemPacketBuffer.h>, so the type is incomplete at that point.
# Upstream survives because nothing defines CONFIG_ENABLE_ESP32_BLE_CONTROLLER
# there; esp-matter does (the C6 and the S3 run the new ESP32 BLE controller),
# and then every translation unit that pulls the platform event header in
# before PacketBuffer fails to compile:
#
#     CHIPDevicePlatformEvent.h:118:27: error: 'PacketBuffer' in namespace
#     'chip::System' does not name a type
#
# The fix is the obvious one: include the header that declares the class.
PLATFORM_EVENT_H = (ROOT / "connectedhomeip/connectedhomeip/config/esp32/third_party/"
                    "connectedhomeip/src/platform/ESP32/CHIPDevicePlatformEvent.h")
PACKETBUF_OLD = "#include <esp_event.h>\n"
PACKETBUF_NEW = "#include <esp_event.h>\n#include <system/SystemPacketBuffer.h>\n"


def patch(path, old, new, what, apps=ALL):
    if APP not in apps:
        print(f"  skip  {what} (not needed by {APP})")
        return
    text = path.read_text(encoding="utf-8")
    if new in text:
        print(f"  skip  {what} (already patched)")
        return
    if old not in text:
        raise SystemExit(f"  FAIL  {what}: expected text not found in {path}")
    if text.count(old) != 1:
        raise SystemExit(f"  FAIL  {what}: expected text is not unique in {path}")
    path.write_text(text.replace(old, new), encoding="utf-8")
    print(f"  patch {what}")


def main():
    print(f"== patching esp-matter sources in {ROOT} for {APP}")
    patch(PAIRING_CMD, PEER_OLD, PEER_NEW, "pairing command: using chip::Transport::PeerAddress", (BRAIN,))
    patch(CLIENT_HDR, PSRAM_OLD, PSRAM_NEW, "controller header: include esp_attr.h", (BRAIN,))
    patch(CLIENT_HDR, SINGLE_OLD, SINGLE_NEW, "controller header: singleton in PSRAM", (BRAIN,))
    patch(NIMBLE_CPP, MTU_OLD, MTU_NEW, "nimble backend: survive a failed MTU exchange", (BRAIN,))
    patch(NIMBLE_CPP, CANCEL_OLD, CANCEL_NEW, "nimble backend: no ble_gap_conn_cancel on a dead host", (BRAIN,))
    patch(PLATFORM_EVENT_H, PACKETBUF_OLD, PACKETBUF_NEW, "platform event header: include system/SystemPacketBuffer.h")


if __name__ == "__main__":
    main()
