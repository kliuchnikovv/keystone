#!/bin/sh
# Build the Keystone plugin examples into .wasm files.
#
# The toolchain is clang with the wasm32 target, WITHOUT libc and WITHOUT
# WASI: a plugin is freestanding, it gets everything from the hub through
# the host imports in the SDK header.
#
# Requirements:
#   - clang >= 15 with the wasm32 target and wasm-ld
#     (Debian/Ubuntu: apt install clang lld; on macOS: brew install llvm)
#   - the plugin SDK header, plugins/sdk/keystone.h
#
# Usage:
#   scripts/build-plugins.sh [output-dir]
#
# The output lands in output-dir/<name>.wasm. The firmware's bundled copy
# is regenerated with scripts/wasm-to-header.py (see plugins/README.md).
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/plugins/build}"
SDK="$ROOT/plugins/sdk"

if ! command -v clang >/dev/null 2>&1; then
  echo "clang not found; install clang + lld (wasm32 target)" >&2
  exit 1
fi

mkdir -p "$OUT"

# The freestanding wasm flags:
#   --target=wasm32   the wasm target
#   -nostdlib         no libc, no compiler-rt — the plugin has nothing but
#                     the SDK header
#   -Wl,--no-entry    wasm-ld must not look for a _start
#   -Wl,--export-dynamic  keep the plugin's own exports visible
#   -Wl,--allow-undefined  the host imports are resolved by the hub at
#                     runtime, not by the linker
#   -Wl,--export=ks_plugin_abi_version  the hub refuses a plugin that does
#                     not export it
#   -Wl,--export=memory  the host reads the plugin's linear memory
build_one() {
  src="$1"
  name="$2"
  shift 2
  echo "building $name.wasm from $(basename "$src")"
  clang --target=wasm32 -nostdlib -O2 -I "$SDK" \
        -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined \
        -Wl,--export=ks_plugin_abi_version -Wl,--export=memory \
        "$@" \
        -o "$OUT/$name.wasm" "$src"
  ls -l "$OUT/$name.wasm"
}

# The well-behaved plugin: a Switch(on_off) that echoes COMMANDs as STATEs.
build_one "$ROOT/plugins/test-switch/test-switch.c" test-switch

# The misbehaving one: the same shape, but on_command never returns. It
# exists to prove the callback watchdog: the hub must survive it, fault
# the plugin and withdraw its entity (design §10).
build_one "$ROOT/plugins/test-switch/test-switch.c" test-switch-crash \
  -DKS_PLUGIN_CRASH=1

echo
echo "built into $OUT"
