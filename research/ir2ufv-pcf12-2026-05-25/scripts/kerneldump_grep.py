#!/usr/bin/env python3
"""Scan a Windows kernel-summary dump for .sys module names (UTF-16LE)
and locate any 8-byte kernel addresses that match a given pattern.

This is heuristic — not a real KDD parser — but enough to fingerprint
the faulting driver when WinDbg isn't available.
"""
from __future__ import annotations

import re
import struct
import sys


def find_sys_names(buf: bytes) -> set[str]:
    """Walk every byte; whenever we see ASCII printable wide-char text
    ending in ".sys" (case-insensitive), record it."""
    out: set[str] = set()
    # UTF-16LE pattern: each ASCII char is followed by 0x00.
    # Search ".sys" (0x2e 00 73 00 79 00 73 00) and walk backwards for
    # a contiguous run of wide-ASCII chars (or \ separator).
    needle = b".\x00s\x00y\x00s\x00"
    idx = 0
    while True:
        pos = buf.find(needle, idx)
        if pos < 0:
            break
        idx = pos + len(needle)
        # Walk backwards in 2-byte steps while we see printable wide-ASCII or '\\'.
        start = pos
        while start >= 2:
            lo, hi = buf[start - 2], buf[start - 1]
            if hi != 0:
                break
            if not (0x20 <= lo <= 0x7e):
                break
            start -= 2
        name_bytes = buf[start:pos + len(needle)]
        try:
            name = name_bytes.decode("utf-16le")
        except UnicodeDecodeError:
            continue
        # Strip any leading garbage chars (sometimes back-walk overshoots).
        m = re.search(r"([A-Za-z0-9_.\\\-]+\.sys)$", name, re.IGNORECASE)
        if m:
            out.add(m.group(1))
    return out


def find_kernel_addrs(buf: bytes, fault_addr: int) -> None:
    """Find candidate module bases that could 'contain' fault_addr by
    looking for 8-byte little-endian values that are < fault_addr but
    > (fault_addr - 0x800000) — module sizes max ~8 MB heuristic."""
    candidates = []
    for off in range(0, len(buf) - 8, 8):
        v = struct.unpack_from("<Q", buf, off)[0]
        if (v & 0xFFFFFF0000000000) == (fault_addr & 0xFFFFFF0000000000):
            # Same upper bits — same kernel region.
            if 0 < (fault_addr - v) < 0x800000:
                candidates.append((v, off))
    # Tighten: keep the unique "base candidates" closest just-before the fault.
    candidates.sort()
    seen = set()
    uniq = []
    for v, off in candidates:
        if v in seen:
            continue
        seen.add(v)
        uniq.append((v, off))
    print(f"  near-by 'base' candidates (kernel address < fault_addr by < 8MB), top 10:")
    for v, off in uniq[:10]:
        delta = fault_addr - v
        print(f"    base=0x{v:016x}  delta=+0x{delta:08x}  found@file-offset=0x{off:x}")


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: kerneldump_grep.py <dump> <fault_hex_addr>")
        return 2
    path = sys.argv[1]
    fault_addr = int(sys.argv[2], 16)
    with open(path, "rb") as f:
        buf = f.read()
    print(f"=== {path} (size {len(buf)}) ===")
    print(f"fault addr: 0x{fault_addr:016x}")
    names = find_sys_names(buf)
    print(f"\nfound {len(names)} unique .sys mentions:")
    for n in sorted(names, key=str.lower):
        print(f"  {n}")
    print()
    find_kernel_addrs(buf, fault_addr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
