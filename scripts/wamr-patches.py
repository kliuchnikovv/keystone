#!/usr/bin/env python3
"""Patch WAMR 2.4.0~1 (the espressif/wasm-micro-runtime managed component) so
it builds against ESP-IDF 6.1 + PlatformIO. This is a SPIKE patch set — each
item sits on a WAMR source the component manager re-downloads whenever the
managed_components/ dir is wiped, so this script is re-applied by hand after
every clean.

What the patches do:
  1. espidf_file.c: pull in <sys/stat.h> and <fcntl.h>, forward-declare the
     POSIX *at* functions newlib doesn't export so the file compiles; it is
     dead code anyway when CONFIG_WAMR_ENABLE_LIBC_WASI=n.
  2. espidf_platform.c: pull in <sys/stat.h>/<sys/types.h> so its ENOSYS
     stubs for fstatat/mkdirat/… see `struct stat` and `mode_t`.
  3. Rename invokeNative_xtensa.s -> .S so PlatformIO/SCons routes the file
     through gcc (as alone does not accept `-mlongcalls` on toolchain 15.2).

Idempotent: a patch already applied is a no-op.
"""
from pathlib import Path
import shutil
import subprocess
import sys

PROJECT_ROOT = Path(__file__).resolve().parents[1]
ROOT = PROJECT_ROOT / "firmware/components/wamr"
MANAGED = (PROJECT_ROOT
           / "firmware/brain-s3/managed_components"
           / "espressif__wasm-micro-runtime")

if not ROOT.exists():
    # Bootstrap: pull WAMR through the IDF Component Manager by adding it to
    # the brain's idf_component.yml and running a short pio pass, then copy
    # the result into firmware/components/wamr. Idempotent.
    if not MANAGED.exists():
        print("WAMR managed component not present; running `pio run` in "
              "firmware/brain-s3 to fetch it (edit idf_component.yml if needed).",
              file=sys.stderr)
        rc = subprocess.call(
            ["pio", "run"], cwd=str(PROJECT_ROOT / "firmware/brain-s3"))
        # The build may fail because of unpatched WAMR (that is literally what
        # this script is here to fix). As long as the component was downloaded,
        # continue.
        if not MANAGED.exists():
            print(f"pio did not download WAMR (exit {rc}); aborting",
                  file=sys.stderr)
            sys.exit(2)
    print(f"vendoring WAMR: {MANAGED} -> {ROOT}")
    shutil.copytree(MANAGED, ROOT)


def patch_contains(path: Path, marker: str) -> bool:
    return marker in path.read_text()


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    if old not in text:
        if new in text:
            print(f"  {path.name}: already patched")
            return
        raise SystemExit(f"{path}: anchor text not found; WAMR upstream may have changed")
    text = text.replace(old, new, 1)
    path.write_text(text)
    print(f"  {path.name}: patched")


# --- 1. espidf_file.c ---------------------------------------------------------
file_c = ROOT / "core/shared/platform/esp-idf/espidf_file.c"
print(file_c.name)
replace_once(
    file_c,
    '#include "platform_api_extension.h"\n'
    '#include "libc_errno.h"\n'
    '#include <unistd.h>\n',
    '/* SPIKE PATCH (wasm-plugin-spike): the file uses `struct stat` and\n'
    ' * POSIX fstatat/mkdirat/renameat/unlinkat/futimens/utimensat. ESP-IDF 6.1\n'
    ' * newlib does not surface them via <unistd.h>; include <sys/stat.h> and\n'
    ' * forward-declare so the file compiles. The symbols are stubbed in\n'
    ' * espidf_platform.c (ENOSYS); harmless with CONFIG_WAMR_ENABLE_LIBC_WASI=n.\n'
    ' */\n'
    '#include <sys/stat.h>\n'
    '#include <sys/types.h>\n'
    '\n'
    '#include "platform_api_extension.h"\n'
    '#include "libc_errno.h"\n'
    '#include <unistd.h>\n'
    '#include <fcntl.h>\n'
    '#include <stdio.h>\n'
    '\n'
    '#ifndef _ESPIDF_POSIX_AT_FWD_\n'
    '#define _ESPIDF_POSIX_AT_FWD_\n'
    'extern int fstatat(int, const char *, struct stat *, int);\n'
    'extern int mkdirat(int, const char *, mode_t);\n'
    'extern int renameat(int, const char *, int, const char *);\n'
    'extern int unlinkat(int, const char *, int);\n'
    'extern int futimens(int, const struct timespec[2]);\n'
    'extern int utimensat(int, const char *, const struct timespec[2], int);\n'
    '#endif\n',
)

# --- 2. espidf_platform.c -----------------------------------------------------
plat_c = ROOT / "core/shared/platform/esp-idf/espidf_platform.c"
print(plat_c.name)
replace_once(
    plat_c,
    '#include "sdkconfig.h"\n'
    '#include "platform_api_vmcore.h"\n'
    '#include "platform_api_extension.h"\n',
    '#include "sdkconfig.h"\n'
    '#include "platform_api_vmcore.h"\n'
    '#include "platform_api_extension.h"\n'
    '/* SPIKE PATCH (wasm-plugin-spike): the file declares `struct stat` params\n'
    ' * and mode_t-taking shims without <sys/stat.h>. ESP-IDF 6.1 newlib does\n'
    ' * not surface them, so WAMR provides ENOSYS stubs here. The declarations\n'
    ' * compile only once the stat/mode_t types are visible. */\n'
    '#include <sys/stat.h>\n'
    '#include <sys/types.h>\n',
)

# --- 3. Rename invokeNative_xtensa.s -> .S ------------------------------------
asm_src = ROOT / "core/iwasm/common/arch/invokeNative_xtensa.s"
asm_dst = ROOT / "core/iwasm/common/arch/invokeNative_xtensa.S"
cmake = ROOT / "core/iwasm/common/iwasm_common.cmake"
print(f"{asm_src.name} -> {asm_dst.name}")
if asm_src.exists():
    asm_dst.write_text(asm_src.read_text())
    asm_src.unlink()
    text = cmake.read_text()
    cmake.write_text(text.replace("invokeNative_xtensa.s", "invokeNative_xtensa.S"))
    print(f"  renamed and updated {cmake.name}")
elif asm_dst.exists():
    print("  already renamed")
else:
    raise SystemExit(f"neither {asm_src} nor {asm_dst} found")

print("\nWAMR patches applied.")
