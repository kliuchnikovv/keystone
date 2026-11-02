#!/usr/bin/env bash
# Build the Keystone relay (Go) into build/.
#
#   scripts/build-relay.sh              # for this machine
#   scripts/build-relay.sh linux/amd64  # for a server
#
# The relay is one static binary; the container build (relay.Dockerfile) does
# the same thing inside Docker.
set -euo pipefail

cd "$(dirname "$0")/.."
target="${1:-native}"

case "$target" in
  native)         go build -trimpath -ldflags="-s -w" -o build/relay ./relay ;;
  linux/amd64)    CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -ldflags="-s -w" -o build/relay-linux-amd64 ./relay ;;
  linux/arm64)    CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -ldflags="-s -w" -o build/relay-linux-arm64 ./relay ;;
  linux/arm/v7)   CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 go build -trimpath -ldflags="-s -w" -o build/relay-linux-armv7 ./relay ;;
  *) echo "unknown target: $target (native, linux/amd64, linux/arm64, linux/arm/v7)" >&2; exit 2 ;;
esac

echo "built: build/relay*"
