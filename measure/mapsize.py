#!/usr/bin/env python3
"""Attribute flash and RAM bytes in a GNU ld map to (archive, object).

Flash: output sections below 0x20000000, plus the load image of RAM sections
that carry a load address (.data-like). RAM: output sections at or above
0x20000000. Non-ALLOC sections (.debug*, .comment, ...) are skipped.

usage: mapsize.py zephyr.map [regex-on-archive(object) ...]
"""
import re
import sys
from collections import defaultdict

SKIP = (".debug", ".comment", ".ARM.attributes", ".stab", ".symtab", ".strtab",
        ".shstrtab", "/DISCARD/", ".gnu.attributes", ".last_section")

out_re = re.compile(r"^(\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)(\s+load address 0x([0-9a-fA-F]+))?\s*$")
out_name_only = re.compile(r"^(\S+)\s*$")
out_cont = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)(\s+load address 0x([0-9a-fA-F]+))?\s*$")
in_re = re.compile(r"^ (\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$")
in_name_only = re.compile(r"^ (\S+)\s*$")
in_cont = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$")


def add(sizes, cur, size, owner):
    name, vma, has_lma = cur
    if size == 0 or name.startswith(SKIP):
        return
    owner = owner.strip()
    if vma >= 0x20000000:
        sizes[owner][1] += size
        if has_lma:
            sizes[owner][0] += size
    elif vma != 0 or name in ("rom_start", "text"):
        sizes[owner][0] += size


def parse(path):
    sizes = defaultdict(lambda: [0, 0])  # owner -> [flash, ram]
    cur = None  # (output section, vma, has load address)
    pending_out = pending_in = None
    started = False
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not started:
                started = line.startswith("Linker script and memory map")
                continue
            # A name alone on its line continues on the next one; anything
            # else on that next line is a fresh entry and is parsed as such.
            if pending_out is not None:
                m = out_cont.match(line)
                p, pending_out = pending_out, None
                if m:
                    cur = (p, int(m.group(1), 16), m.group(3) is not None)
                    continue
            if pending_in is not None:
                m = in_cont.match(line)
                pending_in = None
                if m:
                    if cur:
                        add(sizes, cur, int(m.group(2), 16), m.group(3))
                    continue
            if not line.startswith(" "):
                m = out_re.match(line)
                if m:
                    cur = (m.group(1), int(m.group(2), 16), m.group(4) is not None)
                    continue
                m = out_name_only.match(line)
                if m and not line.startswith("LOAD"):
                    pending_out = m.group(1)
                continue
            m = in_re.match(line)
            if m and cur:
                add(sizes, cur, int(m.group(3), 16), m.group(4))
                continue
            m = in_name_only.match(line)
            if m:
                pending_in = m.group(1)
    return sizes


def main():
    sizes = parse(sys.argv[1])
    print(f"TOTAL flash {sum(v[0] for v in sizes.values())} "
          f"RAM {sum(v[1] for v in sizes.values())}")
    for p in sys.argv[2:]:
        r = re.compile(p)
        sel = {k: v for k, v in sizes.items() if r.search(k)}
        print(f"\n== {p}: flash {sum(v[0] for v in sel.values())} "
              f"RAM {sum(v[1] for v in sel.values())} ({len(sel)} objects)")
        for k, v in sorted(sel.items(), key=lambda kv: -(kv[1][0] + kv[1][1])):
            print(f"  {v[0]:7d} {v[1]:7d}  {re.sub(r'^.*/', '', k)}")


if __name__ == "__main__":
    main()
