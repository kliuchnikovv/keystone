#!/usr/bin/env python3
"""Emit the Plan 1b test plugin as a .wasm byte array, both as a binary file
and as a C++ `constexpr uint8_t[]` include. The plugin is a minimum-viable
Switch (SPEC §6.1): on_start declares a Switch(on_off) through
`ks_entity_declare`, on_command echoes the incoming value back through
`ks_state_publish`.

Writing wasm by hand got the 86-byte module in the spike wrong twice (missing
type, wrong length byte) — this script removes that whole class of bug.

Usage:
    python3 scripts/gen-test-plugin-wasm.py <out.wasm> <out.h>
"""
import sys
from pathlib import Path


def leb128_u32(n: int) -> bytes:
    out = bytearray()
    while True:
        byte = n & 0x7F
        n >>= 7
        if n:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def name(s: str) -> bytes:
    b = s.encode("ascii")
    return leb128_u32(len(b)) + b


def section(sid: int, payload: bytes) -> bytes:
    return bytes([sid]) + leb128_u32(len(payload)) + payload


def vec(items: list[bytes]) -> bytes:
    return leb128_u32(len(items)) + b"".join(items)


# --- Type section (id 1) ------------------------------------------------------
# type 0: () -> ()                           for on_start, on_stop
# type 1: () -> i32                           for ks_plugin_abi_version
# type 2: (i32, i32) -> i32                   for ks_entity_declare
# type 3: (i32, i32, i32, i32) -> i32         for ks_state_publish
# type 4: (i32, i32, i32, i32, i32) -> ()     for on_command
I32 = 0x7F
FUNC = 0x60
types = vec([
    bytes([FUNC, 0, 0]),
    bytes([FUNC, 0, 1, I32]),
    bytes([FUNC, 2, I32, I32, 1, I32]),
    bytes([FUNC, 4, I32, I32, I32, I32, 1, I32]),
    bytes([FUNC, 5, I32, I32, I32, I32, I32, 0]),
])

# --- Import section (id 2) ----------------------------------------------------
# import 0: env.ks_entity_declare : type 2
# import 1: env.ks_state_publish  : type 3
KIND_FUNC = 0x00
imports = vec([
    name("env") + name("ks_entity_declare") + bytes([KIND_FUNC, 2]),
    name("env") + name("ks_state_publish") + bytes([KIND_FUNC, 3]),
])

# --- Function section (id 3) --------------------------------------------------
# Three locally-defined funcs; their global indices are 2..4 (after two imports).
#   global idx 2 = ks_plugin_abi_version : type 1
#   global idx 3 = on_start              : type 0
#   global idx 4 = on_command            : type 4
functions = vec([bytes([1]), bytes([0]), bytes([4])])

# --- Memory section (id 5): 1 page, no max -----------------------------------
mem = vec([bytes([0x00, 0x01])])

# --- Global section (id 6): mut i32 initialised to 0 -------------------------
globals_ = vec([bytes([I32, 0x01, 0x41, 0x00, 0x0B])])

# --- Export section (id 7) ----------------------------------------------------
KIND_MEM = 0x02
exports = vec([
    name("memory") + bytes([KIND_MEM, 0]),
    name("ks_plugin_abi_version") + bytes([KIND_FUNC, 2]),
    name("on_start") + bytes([KIND_FUNC, 3]),
    name("on_command") + bytes([KIND_FUNC, 4]),
])

# --- Code section (id 10) -----------------------------------------------------
# A function body is `locals_vec` then bytecode, prefixed with its total length.
def body(bytecode: bytes) -> bytes:
    full = bytes([0x00]) + bytecode  # zero local-decl entries
    return leb128_u32(len(full)) + full


# ks_plugin_abi_version: return 1
abi_body = body(bytes([0x41, 0x01, 0x0B]))  # i32.const 1; end

# on_start: ks_entity_declare(ptr=0, len=7); store the returned entity id
# into global 0.
on_start_body = body(bytes([
    0x41, 0x00,        # i32.const 0  (desc ptr)
    0x41, 0x07,        # i32.const 7  (desc len)
    0x10, 0x00,        # call 0       (ks_entity_declare)
    0x24, 0x00,        # global.set 0
    0x0B,              # end
]))

# on_command(entity, attr, val_ptr, len, seq):
#   mem[8] = *val_ptr           (echo back whatever came in)
#   ks_state_publish(entity_global, attr, 8, 1)
#   drop the KsErr result, return.
on_command_body = body(bytes([
    0x41, 0x08,         # i32.const 8        (dst)
    0x20, 0x02,         # local.get 2        (src val_ptr)
    0x2D, 0x00, 0x00,   # i32.load8_u a=0 o=0
    0x3A, 0x00, 0x00,   # i32.store8  a=0 o=0

    0x23, 0x00,         # global.get 0       (entity)
    0x20, 0x01,         # local.get 1        (attr)
    0x41, 0x08,         # i32.const 8        (val_ptr)
    0x41, 0x01,         # i32.const 1        (val_len)
    0x10, 0x01,         # call 1             (ks_state_publish)
    0x1A,               # drop
    0x0B,               # end
]))

code = vec([abi_body, on_start_body, on_command_body])

# --- Data section (id 11) -----------------------------------------------------
# One active data segment in memory 0 starting at offset 0.
# DECLARE payload for a Switch with one Bool attribute `on_off`:
#   entity_id(u16 LE)=0 (manager overrides) | device_type=Switch(0x03) |
#   attr_count=1 | attr { attr_id=kOnOff(0x01), value_type=Bool(0x01),
#                         flags=R|W|Reportable(0x07) }
DECLARE_BYTES = bytes([0x00, 0x00, 0x03, 0x01, 0x01, 0x01, 0x07])
assert len(DECLARE_BYTES) == 7
data_segment = (
    bytes([0x00])              # memory index 0 (and "active, no memidx")
    + bytes([0x41, 0x00, 0x0B])  # offset: i32.const 0; end
    + leb128_u32(len(DECLARE_BYTES))
    + DECLARE_BYTES
)
data = vec([data_segment])

# --- Assemble module ----------------------------------------------------------
module = (
    bytes([0x00, 0x61, 0x73, 0x6D])  # magic
    + bytes([0x01, 0x00, 0x00, 0x00])  # version 1
    + section(1, types)
    + section(2, imports)
    + section(3, functions)
    + section(5, mem)
    + section(6, globals_)
    + section(7, exports)
    + section(10, code)
    + section(11, data)
)


def emit_header(path: Path, module_bytes: bytes) -> None:
    lines = [
        "// Auto-generated by scripts/gen-test-plugin-wasm.py. Do not edit.",
        "// A Switch(on_off) plugin that echoes COMMAND back as STATE.",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace wasm_host {",
        "",
        f"inline constexpr std::size_t kTestPluginWasmLen = {len(module_bytes)};",
        "",
        "inline constexpr std::uint8_t kTestPluginWasm[] = {",
    ]
    for i in range(0, len(module_bytes), 12):
        chunk = module_bytes[i:i + 12]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines += [
        "};",
        "",
        "}  // namespace wasm_host",
        "",
    ]
    path.write_text("\n".join(lines))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    out_wasm = Path(sys.argv[1])
    out_hdr = Path(sys.argv[2])
    out_wasm.parent.mkdir(parents=True, exist_ok=True)
    out_hdr.parent.mkdir(parents=True, exist_ok=True)
    out_wasm.write_bytes(module)
    emit_header(out_hdr, module)
    print(f"wrote {out_wasm} ({len(module)} B) and {out_hdr}")
