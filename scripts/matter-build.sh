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
# The brain-s3 build also patches one esp-matter source inside the container:
# esp_matter_controller_pairing_command.cpp in release/v1.6 uses bare
# `PeerAddress` but forgets `using chip::Transport::PeerAddress;` at file
# scope — the sibling file esp_matter_controller_console.cpp has exactly that
# using-declaration, so this is clearly an upstream omission. The patch adds
# the missing line only to the in-container copy for this build (the image
# itself is `--rm`'d after, so nothing leaks). thread-c6 does not build the
# controller and never hits this.
BUILD_CMD="idf.py build"
if [[ "${APP}" == "brain-s3" ]]; then
  PATCH='sed -i "/^using namespace chip::Controller;/a using chip::Transport::PeerAddress;" /opt/espressif/esp-matter/components/esp_matter_controller/commands/esp_matter_controller_pairing_command.cpp'
  BUILD_CMD="${PATCH} && SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.matter' idf.py build"
fi

echo "==> Building ${APP} in ${IMAGE}"
# `-w` is overridden by the image's export.sh, so cd explicitly inside the shell.
exec docker run --rm \
  ${KS_TEST_CRASH:+-e KS_TEST_CRASH=1} \
  -v "${REPO_ROOT}:/project" \
  "${IMAGE}" \
  bash -lc "cd /project/${APP_DIR} && ${BUILD_CMD}"
