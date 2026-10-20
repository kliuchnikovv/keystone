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

echo "==> Building ${APP} in ${IMAGE}"
# `-w` is overridden by the image's export.sh, so cd explicitly inside the shell.
exec docker run --rm \
  ${KS_TEST_CRASH:+-e KS_TEST_CRASH=1} \
  -v "${REPO_ROOT}:/project" \
  "${IMAGE}" \
  bash -lc "cd /project/${APP_DIR} && idf.py build"
