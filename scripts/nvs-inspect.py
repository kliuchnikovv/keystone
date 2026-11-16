#!/usr/bin/env python3
"""Inspect an ESP-IDF NVS partition image: the entries and the free space.

The NVS partition is a sequence of 4 KB pages; each page starts with a
header whose bitmap says which of its 32-byte entries are in use, and the
entries carry the namespace and the key in plain text.

Usage:
  python3 scripts/nvs-inspect.py firmware/thread-c6/nvs.bin
"""

import struct
import sys

PAGE_SIZE = 4096
HEADER_SIZE = 32
ENTRY_SIZE = 32
NVS_MAGIC = 0xFE

PAGE_TYPES = {0: "uninitialized", 1: "full", 2: "free", 3: "new", 4: "erased"}
PAGE_STATES = {0: "uninitialized", 1: "active", 2: "frozen"}


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    pages = len(data) // PAGE_SIZE
    print("%s: %d bytes, %d pages" % (path, len(data), pages))

    total_entries = 0
    used_pages = 0
    ns_count = {}
    for p in range(pages):
        page = data[p * PAGE_SIZE:(p + 1) * PAGE_SIZE]
        if page[0] != NVS_MAGIC:
            print("page %2d: not an NVS page (magic %02x)" % (p, page[0]))
            continue
        ptype = page[1]
        state = page[3]
        entry_size = page[8]
        entries_per_page = u16(page, 10)
        bitmap = u32(page, 32 - 16)  # the bitmap is at offset 16 in the header
        used = bin(bitmap).count("1")
        total_entries += used
        if used:
            used_pages += 1
        print("page %2d: type=%-13s state=%-13s entry_size=%d entries/page=%d used=%d"
              % (p, PAGE_TYPES.get(ptype, ptype), PAGE_STATES.get(state, state),
                 entry_size, entries_per_page, used))
        # the entries of this page
        for i in range(entries_per_page):
            off = HEADER_SIZE + i * entry_size
            entry = page[off:off + entry_size]
            if entry[0] == 0xFF:  # deleted entry
                continue
            ns = entry[0:16].split(b"\x00")[0].decode("utf-8", "replace")
            key = entry[16:48].split(b"\x00")[0].decode("utf-8", "replace")
            if not ns or not key:
                continue
            ns_count[ns] = ns_count.get(ns, 0) + 1
            vtype = entry[48]
            size = entry[52]
            value = entry[56:56 + min(size, 24)]
            print("        %-16s %-24s type=%-3d size=%-5d %s"
                  % (ns, key, vtype, size, value[:16].hex(" ") if vtype else ""))

    print("\nnamespaces:")
    for ns, n in sorted(ns_count.items()):
        print("  %-20s %d entries" % (ns, n))
    print("total used entries: %d in %d pages" % (total_entries, used_pages))


if __name__ == "__main__":
    main()
