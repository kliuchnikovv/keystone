#!/usr/bin/env python3
"""Read a raw ESP-IDF coredump (binary format) without the esp-coredump tool.

The chip writes the dump into flash in the "binary" format
(core_dump_header_t + per-task headers + TCBs + stacks), and esp-coredump
can decode it — but only when the program ELF matches the image that was
running. This script does the part that does not need the ELF: it lists the
tasks, their stacks and the trap frame of the crash.

Usage (inside the esp-matter docker image, which has python3):

    python3 scripts/coredump-inspect.py firmware/thread-c6/coredump.bin

It also checks the bytes of the flashed image (.bin) at the address of the
faulting instruction, so a stale ELF can be spotted.
"""

import struct
import sys


def parse(path):
    d = open(path, "rb").read()
    data_len, version, tasks_num, tcb_sz, mem_segs_num, chip_rev = struct.unpack_from("<6I", d, 0)
    print("data_len 0x%x version 0x%x tasks %d tcb_sz 0x%x mem_segs %d chip_rev %d"
          % (data_len, version, tasks_num, tcb_sz, mem_segs_num, chip_rev))

    off = 24
    tasks = []
    regions = []
    for i in range(tasks_num):
        tcb_addr, stack_start, stack_end = struct.unpack_from("<3I", d, off)
        off += 12
        tcb_off = off
        tcb = d[off:off + tcb_sz]
        off += tcb_sz
        size = stack_end - stack_start
        stk_off = off
        stack = d[off:off + size]
        off += size
        regions.append((tcb_addr, stack_start, stack_end, tcb_off, stk_off))
        names = []
        cur = b""
        for b in tcb:
            if 32 <= b < 127:
                cur += bytes([b])
            else:
                if len(cur) >= 3:
                    names.append(cur.decode())
                cur = b""
        if len(cur) >= 3:
            names.append(cur.decode())
        print("task %2d tcb=0x%08x stack 0x%08x-0x%08x used %5d names=%s"
              % (i, tcb_addr, stack_start, stack_end, size, names))
        tasks.append((tcb_addr, stack_start, stack_end, tcb, stack))

    # The trap frame of the crash is at the bottom of the crashed task's stack.
    # The RISC-V frame is: pc, ra, sp, gp, tp, t0-t2, fp, s1, a0-a7, s2-s11, t3-t6,
    # then mstatus, mtvec, mcause, mtval, mscratch.
    for tcb_addr, stack_start, stack_end, tcb, stack in tasks:
        if len(stack) < 0x84:
            continue
        pc, ra, sp = struct.unpack_from("<3I", stack, 0)
        if not (0x42000000 <= pc < 0x42200000):
            continue
        print("\n--- trap frame in task tcb=0x%08x (stack bottom 0x%08x)" % (tcb_addr, stack_start))
        print("pc 0x%08x  ra 0x%08x  sp 0x%08x" % (pc, ra, sp))
        regs = struct.unpack_from("<32I", stack, 0)
        names = ["pc", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "fp", "s1"]
        names += ["a%d" % i for i in range(8)]
        names += ["s%d" % i for i in range(2, 12)]
        names += ["t3", "t4", "t5", "t6"]
        for n, v in zip(names, regs):
            print("  %-4s 0x%08x" % (n, v))
        mstatus, mtvec, mcause, mtval, mscratch = struct.unpack_from("<5I", stack, 0x80)
        print("  mstatus 0x%08x mtvec 0x%08x mcause %d mtval 0x%08x mscratch 0x%08x"
              % (mstatus, mtvec, mcause, mtval, mscratch))
        causes = {0: "instruction address misaligned", 1: "instruction access fault",
                  2: "illegal instruction", 3: "breakpoint", 4: "load address misaligned",
                  5: "load access fault", 6: "store access fault",
                  7: "store/AMO address misaligned", 8: "ecall", 9: "instruction page fault",
                  11: "environment call", 13: "load page fault", 15: "store page fault"}
        print("  cause: %s" % causes.get(mcause, "?"))
        find_halfword(d, mtval, regions)
        break
    return tasks


def find_halfword(d, value, tasks):
    """Where does the illegal instruction (mtval) sit in the dump?

    An illegal instruction means the CPU jumped somewhere it should not have.
    If the two bytes of mtval are found in a task's stack, that is where it
    jumped to — and the stack is what got corrupted.
    """
    needle = struct.pack("<H", value & 0xFFFF)
    print("\n--- looking for the instruction bytes %s in the dump" % needle.hex(" "))
    i = 0
    hits = []
    while i + 2 <= len(d):
        if d[i:i + 2] == needle:
            hits.append(i)
        i += 1
    if not hits:
        print("not in the dump")
        return
    for tcb_addr, s0, s1, tcb_off, stk_off in tasks:
        for h in hits:
            if tcb_off <= h < tcb_off + 0x150:
                print("  in the TCB of 0x%08x at address 0x%08x" % (tcb_addr, s0 + (h - tcb_off)))
            if stk_off <= h < stk_off + (s1 - s0):
                print("  in the stack of 0x%08x at address 0x%08x" % (tcb_addr, s0 + (h - stk_off)))
    for h in hits:
        print("  file offset 0x%x" % h)


def image_bytes(binpath, addr, count=8):
    d = open(binpath, "rb").read()
    print("\nimage %s: %d bytes, magic %02x, %d segments" % (binpath, len(d), d[0], d[1]))
    off = 24
    while off + 8 <= len(d):
        seg_addr, length = struct.unpack_from("<II", d, off)
        off += 8
        if seg_addr <= addr < seg_addr + length:
            fo = off + (addr - seg_addr)
            print("bytes at 0x%08x in the image: %s" % (addr, d[fo:fo + count].hex(" ")))
            return
        off += length
    print("address 0x%08x is not in the image" % addr)


if __name__ == "__main__":
    tasks = parse(sys.argv[1])
    if len(sys.argv) > 2:
        image_bytes(sys.argv[2], int(sys.argv[3], 0))
