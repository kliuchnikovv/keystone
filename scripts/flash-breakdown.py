#!/usr/bin/env python3
"""Where did the flash go? Sum the linked image size per library.

Reads an ESP-IDF linker map (.map) and adds up the size of every section
that ended up in the ELF, grouped by the archive or object it came from.
That answers "which library is eating my OTA slot" without guessing.

Usage:
    scripts/flash-breakdown.py [firmware/thread-c6/build/keystone_thread_c6.map]
"""

import collections
import pathlib
import re
import sys

# ".text.foo" on one line, then "  0xADDR  0xSIZE  path/libX.a(obj.o)" on the next.
SECTION = re.compile(r"^ (\.[\w.]+)$")
ENTRY = re.compile(r"^\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S+)")


def main() -> None:
    if len(sys.argv) > 1:
        mapfile = pathlib.Path(sys.argv[1])
    else:
        mapfile = pathlib.Path("firmware/thread-c6/build/keystone_thread_c6.map")
    if not mapfile.exists():
        sys.exit(f"no such map file: {mapfile}")

    per_file: collections.Counter = collections.Counter()
    per_lib: collections.Counter = collections.Counter()
    total = 0
    for line in mapfile.read_text(errors="replace").splitlines():
        m = ENTRY.match(line)
        if not m:
            continue
        size = int(m.group(1), 16)
        src = m.group(2)
        per_file[src] += size
        lib = src.split("(")[0]
        per_lib[lib] += size
        total += size

    print(f"{total:>10}  bytes  (total of the sections listed in {mapfile.name})")
    print("\nby library:")
    for lib, size in per_lib.most_common(18):
        print(f"{size:>10}  {lib}")
    print("\nby object:")
    for src, size in per_file.most_common(20):
        print(f"{size:>10}  {src}")


if __name__ == "__main__":
    main()
