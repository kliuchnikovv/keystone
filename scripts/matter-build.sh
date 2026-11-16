#!/usr/bin/env bash
# Build the Matter co-processor firmware inside the esp-matter Docker image.
# Source stays in this repo; only the toolchain is containerized.
# See docs/matter-backlog.md § "Route 3: Docker build image".
set -euo pipefail

IMAGE="${ESP_MATTER_IMAGE:-espressif/esp-matter:release-v1.6_idf_v5.5.5}"
APP="${1:-thread-c6}"

# --keep: do not wipe the build dir, so an interrupted build resumes instead
# of starting from scratch (a full Matter build is ~20 min).
if [[ "${2:-}" == "--keep" ]]; then
  export KS_CLEAN=0
  echo "==> keeping the build dir (incremental build)"
fi

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
# 6) the read command prints through the chip-tool DataModelLogger, which links
#    ~420 KB of zap-generated logging for every cluster; the brain prints the TLV
#    itself, so the calls go and the Matter image fits the 0x1F0000 app slots.
BUILD_CMD="idf.py build"
if [[ "${APP}" == "brain-s3" ]]; then
  # brain-s3/build is tmpfs on the Pi (see /etc/fstab) — parallel IO is cheap,
  # keep the default -j.
  #
  # `rm -f sdkconfig` for the same reason as thread-c6: IDF applies
  # sdkconfig.defaults* only when it *creates* sdkconfig, so an existing file
  # keeps its old values and the .matter overlay is silently ignored — the CA
  # bundle would stay the full 69 KB one and the image would not fit the app
  # partition. The file is untracked and is regenerated from the tracked
  # defaults.
  BUILD_CMD="rm -f sdkconfig sdkconfig.old && python3 /project/scripts/matter-patches.py --app brain-s3 && idf.py -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.matter' build"
elif [[ "${APP}" == "thread-c6" ]]; then
  # thread-c6/build lives on the SD card (too big for tmpfs on a 4 GB Pi):
  # a full -j4 ninja saturates SD random IO and freezes SSH. MATTER_JOBS caps
  # parallelism (default 2); override with MATTER_JOBS=4 for a dev box with SSD.
  # `-j` is a ninja flag, not an idf.py flag — pass it after `--`.
  #
  # The platform-event header patch (5) is needed on this chip too: with
  # esp-matter's BLE controller enabled the file does not compile without it.
  # The SDKCONFIG_DEFAULTS overlay turns on BLE + Matter-side lwIP + the bridge
  # data model; the base sdkconfig.defaults keeps the Thread host as it is.
  #
  # The overlay is passed as a cmake define, not as an environment variable:
  # that is the form IDF's own test-suite uses (tools/test_build_system/
  # test_sdkconfig.py), and it survives in the CMake cache — the env var is
  # read only while cmake configures, so it is silently lost whenever the
  # build directory is configured by something else.
  #
  # `rm -f sdkconfig` first: IDF applies sdkconfig.defaults* only when it
  # *creates* sdkconfig. An existing one keeps its old values and the .matter
  # overlay is silently ignored — that is exactly how CONFIG_MBEDTLS_HKDF_C
  # stayed off and the link failed with "undefined reference: mbedtls_hkdf".
  # The file is untracked and is regenerated from the tracked defaults.
  BUILD_CMD="rm -f sdkconfig sdkconfig.old && python3 /project/scripts/matter-patches.py --app thread-c6 && idf.py -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.matter' build -- -j${MATTER_JOBS:-2}"
  # A stale CMake cache can keep a sources list from an earlier layout (the
  # build/ dir once resurrected a deleted .cpp). KS_CLEAN=0 keeps the build dir
  # for a faster incremental build while chasing link errors.
  if [[ "${KS_CLEAN:-1}" != "0" ]]; then
    BUILD_CMD="rm -rf build && ${BUILD_CMD}"
  fi
fi

echo "==> Building ${APP} in ${IMAGE}"
# `-w` is overridden by the image's export.sh, so cd explicitly inside the shell.
exec docker run --rm \
  ${KS_TEST_CRASH:+-e KS_TEST_CRASH=1} \
  -v "${REPO_ROOT}:/project" \
  "${IMAGE}" \
  bash -lc "cd /project/${APP_DIR} && ${BUILD_CMD}"
