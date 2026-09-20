#!/usr/bin/env python3
"""Read a Windows minidump and print the module list + exception info.

Minidump format reference: MSDN MINIDUMP_HEADER, MINIDUMP_DIRECTORY,
MINIDUMP_MODULE_LIST_STREAM, MINIDUMP_EXCEPTION_STREAM.

Just enough parsing to identify the faulting driver from a 0xD1 / 0x1E
bugcheck dump.
"""
from __future__ import annotations

import struct
import sys

STREAM_MODULE_LIST = 4
STREAM_EXCEPTION = 6
STREAM_SYSTEM_INFO = 7
STREAM_MISC_INFO = 15


def read_utf16_at(buf: bytes, rva: int) -> str:
    """MINIDUMP_STRING: ULONG Length (bytes), WCHAR[] Buffer (no NUL)."""
    if rva == 0 or rva + 4 > len(buf):
        return ""
    (length,) = struct.unpack_from("<I", buf, rva)
    return buf[rva + 4 : rva + 4 + length].decode("utf-16le", errors="replace")


def parse(path: str) -> None:
    with open(path, "rb") as f:
        buf = f.read()
    if buf[:4] != b"MDMP":
        raise SystemExit(f"{path}: not a minidump (magic={buf[:4]!r})")

    # MINIDUMP_HEADER
    sig, ver, num_streams, dir_rva, *_ = struct.unpack_from("<4sIIIIIIQ", buf, 0)
    print(f"--- {path} ---")
    print(f"streams: {num_streams}")

    # Walk directory.
    streams: dict[int, tuple[int, int]] = {}
    for i in range(num_streams):
        off = dir_rva + i * 12
        stype, dsize, drva = struct.unpack_from("<III", buf, off)
        streams[stype] = (dsize, drva)

    # Exception stream → bugcheck code + faulting address.
    if STREAM_EXCEPTION in streams:
        _dsize, drva = streams[STREAM_EXCEPTION]
        thread_id, _align, exc_code, exc_flags, exc_record, exc_addr = struct.unpack_from(
            "<IIIIQQ", buf, drva
        )
        # ExceptionInformation: ULONG NumberParameters, ULONG __, ULONG64[15]
        np = struct.unpack_from("<I", buf, drva + 28)[0]
        params = struct.unpack_from(f"<{np}Q", buf, drva + 32)
        print(f"\nEXCEPTION_RECORD")
        print(f"  ExceptionCode: 0x{exc_code:08x}")
        print(f"  ExceptionFlags: 0x{exc_flags:08x}")
        print(f"  ExceptionAddress: 0x{exc_addr:016x}")
        print(f"  Parameters ({np}):")
        for i, p in enumerate(params):
            print(f"    [{i}] 0x{p:016x}")

    # Module list.
    if STREAM_MODULE_LIST in streams:
        _dsize, drva = streams[STREAM_MODULE_LIST]
        n_modules = struct.unpack_from("<I", buf, drva)[0]
        print(f"\nMODULE_LIST: {n_modules} modules")
        modules = []
        for i in range(n_modules):
            # MINIDUMP_MODULE is 108 bytes
            mo = drva + 4 + i * 108
            base, size, csum, ts, name_rva = struct.unpack_from("<QIIII", buf, mo)
            name = read_utf16_at(buf, name_rva)
            modules.append((base, size, name))
        modules.sort()

        # Print modules with their base + size range.
        for base, size, name in modules:
            end = base + size
            short = name.rsplit("\\", 1)[-1]
            print(f"  base=0x{base:016x} end=0x{end:016x} size={size:>8d}  {short}")

        # If we have the exception address, find which module contains it.
        if STREAM_EXCEPTION in streams:
            print(f"\n=> Faulting address 0x{exc_addr:016x}")
            hit = None
            for base, size, name in modules:
                if base <= exc_addr < base + size:
                    hit = (base, size, name)
                    break
            if hit:
                base, size, name = hit
                off = exc_addr - base
                print(f"=> hits module: {name.rsplit(chr(92), 1)[-1]}  "
                      f"(base 0x{base:x}, offset +0x{off:x})")
            else:
                print("=> no module contains that address")
            # Also check parameter[1] (often the "first chance" address for 0xD1)
            if STREAM_EXCEPTION in streams and len(params) >= 4:
                cand = params[3] if exc_code == 0xD1 else params[1]
                print(f"\n=> Likely 'faulting-instruction' candidate (param ~addr): 0x{cand:016x}")
                hit2 = None
                for base, size, name in modules:
                    if base <= cand < base + size:
                        hit2 = (base, size, name)
                        break
                if hit2:
                    base, size, name = hit2
                    print(f"=> hits module: {name.rsplit(chr(92), 1)[-1]}  "
                          f"(base 0x{base:x}, offset +0x{cand-base:x})")
                else:
                    print("=> no module contains that address")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        parse(p)
        print()
