#!/usr/bin/env bash
# Build the Matter co-processor firmware inside the esp-matter Docker image.
# Source stays in this repo; only the toolchain is containerized.
# See docs/matter-backlog.md § "Route 3: Docker build image".
set -euo pipefail

IMAGE="${ESP_MATTER_IMAGE:-espressif/esp-matter:release-v1.6_idf_v5.5.5}"
APP="${1:-thread-c6}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="firmware/${APP}"

if [[ ! -d "${REPO_ROOT}/${APP_DIR}" ]]; then
  echo "Unknown app '${APP}' — no ${APP_DIR}/ in this repo." >&2
  exit 1
fi

# brain-s3 keeps a Matter-specific sdkconfig overlay (sdkconfig.defaults.matter)
# that only the Docker build should apply — PlatformIO reads sdkconfig.defaults
# alone and must stay Matter-free. Pass SDKCONFIG_DEFAULTS so idf.py merges the
# overlay on top of the base defaults.
#
# The brain-s3 build also patches a few esp-matter sources inside the
# container. The patches touch only the in-container copy for this build (the
# image itself is `--rm`'d after, so nothing leaks) and live in
# scripts/matter-patches.py, which documents each one:
#
# 1) the pairing command misses `using chip::Transport::PeerAddress;`
#    (an upstream omission in release/v1.6 — the sibling console file has it);
# 2) the controller singleton (130 KB of .bss) has to live in PSRAM, or the
#    S3-Zero runs out of internal RAM inside CHIP's own init;
# 3) the ESP32 NimBLE backend must not tear the connection down when the ATT
#    MTU exchange fails — that killed the commissioning attempt before PASE;
# 4) ... and must not call ble_gap_conn_cancel() on a BLE layer that is
#    already gone (LoadProhibited panic on the S3);
# 5) the ESP32 platform event header uses chip::System::PacketBuffer without
#    including the header that declares it — with esp-matter's BLE controller
#    enabled the file does not compile at all (the thread-c6 build needs this).
BUILD_CMD="idf.py build"
if [[ "${APP}" == "brain-s3" ]]; then
  BUILD_CMD="python3 /project/scripts/matter-patches.py --app brain-s3 && SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.matter' idf.py build"
elif [[ "${APP}" == "thread-c6" ]]; then
  # The thread chip does not build the CHIP stack any more (the Matter Bridge
  # is gone), so it needs none of the esp-matter patches — it only needs IDF
  # 5.5, which the image has.
  BUILD_CMD="idf.py build"
fi

echo "==> Building ${APP} in ${IMAGE}"
# `-w` is overridden by the image's export.sh, so cd explicitly inside the shell.
exec docker run --rm \
  ${KS_TEST_CRASH:+-e KS_TEST_CRASH=1} \
  -v "${REPO_ROOT}:/project" \
  "${IMAGE}" \
  bash -lc "cd /project/${APP_DIR} && ${BUILD_CMD}"
