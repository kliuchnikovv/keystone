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

# How many compile jobs the machine can afford.
#
# The job count is what actually bounds the build (see the docker run below
# for why the memory limit is not enough on its own).
#
# Measured on this Raspberry Pi 5: the build needs ~2.2 GB with one job, and
# every extra job adds about that much. So the machine needs roughly 1.5 GB
# (everything else) + 2.2 GB per job: a 4 GB Pi gets one job, a 16 GB
# workstation gets four.
if [[ -z "${MATTER_JOBS:-}" ]]; then
  total_mb="$(awk '/MemTotal/ {printf "%d", $2/1024}' /proc/meminfo 2>/dev/null || echo 0)"
  if [[ "${total_mb:-0}" -lt 6000 ]]; then
    MATTER_JOBS=1
  elif [[ "${total_mb}" -lt 9000 ]]; then
    MATTER_JOBS=2
  elif [[ "${total_mb}" -lt 14000 ]]; then
    MATTER_JOBS=3
  else
    MATTER_JOBS=4
  fi
  echo "==> ${total_mb} MB of RAM, building with ${MATTER_JOBS} job(s)"
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
  # brain-s3/build is tmpfs on the Pi (see /etc/fstab) — parallel IO is cheap.
  # The scarce resource is still the memory (see the docker run below), so the
  # job count is capped like thread-c6's.
  #
  # `rm -f sdkconfig` for the same reason as thread-c6: IDF applies
  # sdkconfig.defaults* only when it *creates* sdkconfig, so an existing file
  # keeps its old values and the .matter overlay is silently ignored — the CA
  # bundle would stay the full 69 KB one and the image would not fit the app
  # partition. The file is untracked and is regenerated from the tracked
  # defaults.
  # The `-j` flag is a ninja flag, not an idf.py flag — pass it after `--`.
  # brain-s3's build is on tmpfs (fast IO), but the *memory* is the scarce
  # resource: the Matter stack's controller singleton and the CHIP objects are
  # what eat it. The job count is picked by the available memory (above).
  BUILD_CMD="rm -f sdkconfig sdkconfig.old && python3 /project/scripts/matter-patches.py --app brain-s3 && idf.py -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.matter' build -- -j${MATTER_JOBS}"
elif [[ "${APP}" == "thread-c6" ]]; then
  # thread-c6/build lives on the SD card (too big for tmpfs on a 4 GB Pi):
  # a full -j4 ninja saturates SD random IO and freezes SSH. MATTER_JOBS caps
  # the parallelism (picked by the available memory, see above).
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
  BUILD_CMD="rm -f sdkconfig sdkconfig.old && python3 /project/scripts/matter-patches.py --app thread-c6 && idf.py -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.matter' build -- -j${MATTER_JOBS}"
  # A stale CMake cache can keep a sources list from an earlier layout (the
  # build/ dir once resurrected a deleted .cpp). KS_CLEAN=0 keeps the build dir
  # for a faster incremental build while chasing link errors.
  if [[ "${KS_CLEAN:-1}" != "0" ]]; then
    BUILD_CMD="rm -rf build && ${BUILD_CMD}"
  fi
fi

echo "==> Building ${APP} in ${IMAGE}"
# The build is bounded on purpose, and the numbers matter on a small machine
# (a Raspberry Pi 5 with 4 GB of RAM).
#
# A Matter build compiles ~1700 translation units of CHIP and links ~400 KB of
# objects. Measured on this Pi: *one* job peaks at ~3.6 GB of the host's
# memory. Two jobs push it over: the kernel then compresses pages into zram
# (the Pi's swap *is* RAM), which is so CPU-heavy that the machine stops
# answering; the interactive session (ssh/mosh) dies first, and then systemd's
# hardware watchdog reboots the Pi (RuntimeWatchdogUSec=1min,
# RebootWatchdogUSec=2min) — so there is no log of the failure, the machine is
# simply restarted.
#
# The job count above is what actually bounds it. The memory limit is a second
# line of defence, but only where the kernel has the memory controller: this
# Pi's cmdline carries `cgroup_disable=memory`, so Docker prints "Your kernel
# does not support memory limit capabilities" and *discards* the limit. Where
# the controller is missing, the job count is the only lever — do not lower
# MATTER_MEM expecting it to help here.
#
#   MATTER_JOBS   how many compile jobs (default: by available memory)
#   MATTER_MEM    how much RAM the build may use (default: two thirds of what
#                 is available; needs the memory cgroup controller to apply)
#   MATTER_CPUS   how many CPUs the build may use (default 2)
#
# On a bigger machine (8 GB and more) raise MATTER_JOBS and MATTER_MEM:
#   MATTER_JOBS=4 MATTER_MEM=4g ./scripts/matter-build.sh thread-c6
if [[ -z "${MATTER_MEM:-}" ]]; then
  # Two thirds of what is free, and never more than the machine has: the build
  # has to leave room for the kernel, the session and the other services.
  avail_mb="$(awk '/MemAvailable/ {printf "%d", $2/1024}' /proc/meminfo 2>/dev/null || echo 0)"
  mem_mb=$(( avail_mb * 2 / 3 ))
  [[ "${mem_mb}" -lt 1200 ]] && mem_mb=1200
  [[ "${mem_mb}" -gt 2600 ]] && mem_mb=2600
  MATTER_MEM="${mem_mb}m"
fi
DOCKER_MEM="${MATTER_MEM}"

# Whether the kernel can enforce the memory limit at all: without the memory
# cgroup controller Docker discards it, and the job count is all we have.
mem_cgroup=0
if [[ -w /sys/fs/cgroup/cgroup.subtree_control ]] && grep -qw memory /sys/fs/cgroup/cgroup.controllers 2>/dev/null; then
  mem_cgroup=1
fi

DOCKER_MEM_ARGS=()
if [[ "${mem_cgroup}" -eq 1 ]]; then
  DOCKER_MEM_ARGS=(--memory "${DOCKER_MEM}" --memory-swap "${DOCKER_MEM}")
else
  echo "==> note: this kernel has no memory cgroup controller (cgroup_disable=memory?),"
  echo "    so the memory limit does not apply — the job count is what bounds the build."
fi

exec docker run --rm \
  ${KS_TEST_CRASH:+-e KS_TEST_CRASH=1} \
  "${DOCKER_MEM_ARGS[@]}" \
  --cpus "${MATTER_CPUS:-2}" \
  -v "${REPO_ROOT}:/project" \
  "${IMAGE}" \
  bash -lc "cd /project/${APP_DIR} && ${BUILD_CMD}"
