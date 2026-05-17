#!/usr/bin/env python3
"""
parse_hex.py — Phase 2 parser for `mo -i +N` captures (frame header + hex
payload, no timestamps).

Each frame in the raw file looks like:

    PORT:fm SRC to DST [via DIGI*] ctl CTL [pid PID] [PLEN]
    0000 32 20 20 20 ... 2 ........
    0010 20 20 20 ...
    ...
    [blank line]

We assemble the hex payload back into bytes for every frame, then classify
CE frames by their first byte (the type discriminator `'0'..'7'`).

For compact-record CE frames (type 3) we further parse the callsign /
SSID / RTT triples per §1.6 of the FlexNet skill.

Output: writes `<input>.h.json` next to each input file.
"""

from __future__ import annotations
import argparse
import json
import re
import sys
from pathlib import Path
from collections import Counter, defaultdict


FRAME_HEADER_RE = re.compile(
    r"^(?P<port>\d+):"
    r"fm\s+(?P<src>\S+)\s+to\s+(?P<dst>\S+)"
    r"(?:\s+via\s+(?P<digis>.+?))?"
    r"\s+ctl\s+(?P<ctl>\S+)"
    r"(?:\s+pid\s+(?P<pid>\S+))?"
    r"(?:\s+\[(?P<plen>\d+)\])?\s*$"
)
HEX_ROW_RE = re.compile(r"^[0-9A-Fa-f]{4}\s+([0-9A-Fa-f ]+?)(?:\s{2,}.*)?$")


def parse_hex_row(row: str) -> bytes:
    """Extract bytes from a single hex-dump row."""
    m = HEX_ROW_RE.match(row)
    if not m:
        return b""
    hex_part = m.group(1).strip()
    # tolerate odd remnants by splitting on whitespace
    out = bytearray()
    for tok in hex_part.split():
        if len(tok) == 2 and all(c in "0123456789abcdefABCDEF" for c in tok):
            out.append(int(tok, 16))
    return bytes(out)


CE_TYPE_NAMES = {
    0x30: "ce-init",
    0x31: "ce-lt-or-stat",  # link-time or 1n status family
    0x32: "ce-ka",
    0x33: "ce-compact-or-token",  # records or 3+/3- tokens
    0x34: "ce-seqnum",
    0x35: "ce-reserved",
    0x36: "ce-pathreq",
    0x37: "ce-pathrep",
}


def classify_ce(payload: bytes) -> dict:
    """Refine classification using payload content."""
    if not payload:
        return {"kind": "ce-empty"}
    t = payload[0]
    name = CE_TYPE_NAMES.get(t, f"ce-unk-0x{t:02x}")
    extra = {}
    if t == 0x31 and len(payload) >= 2 and payload[-1] == 0x0D:
        body = payload[1:-1].decode("latin-1", "replace")
        # type-1 "1NNN\r" link-time
        if body.isdigit():
            extra["lt_value"] = int(body)
            extra["kind_refined"] = "ce-lt"
        else:
            extra["kind_refined"] = "ce-1x"
            extra["body"] = body
    elif t == 0x33:
        # type-3: either token (3+ / 3-) or compact records
        if len(payload) == 3 and payload[1] in (0x2B, 0x2D) and payload[2] == 0x0D:
            extra["kind_refined"] = "ce-tok-plus" if payload[1] == 0x2B else "ce-tok-minus"
        else:
            # parse compact records
            records, trailing_minus = parse_compact_records(payload[1:])
            extra["kind_refined"] = "ce-compact-batch"
            extra["records"] = records
            extra["trailing_minus"] = trailing_minus
            extra["record_count"] = len(records)
    elif t == 0x32:
        # KA payload size variant per peer family
        if len(payload) == 241:
            extra["kind_refined"] = "ce-ka-xnet"
        elif len(payload) == 201:
            extra["kind_refined"] = "ce-ka-pcf"
        else:
            extra["kind_refined"] = f"ce-ka-{len(payload)}"
    elif t == 0x30 and len(payload) >= 5:
        # init: byte 1 = 0x30 + max_ssid
        extra["max_ssid"] = payload[1] - 0x30
        extra["caps"] = bytes(payload[2:4]).decode("latin-1", "replace")
        extra["kind_refined"] = "ce-init"
    elif t == 0x36 and len(payload) >= 7:
        extra["kind_refined"] = "ce-pathreq"
        extra["body"] = payload[1:].decode("latin-1", "replace").rstrip("\r")
    elif t == 0x37 and len(payload) >= 7:
        extra["kind_refined"] = "ce-pathrep"
        extra["body"] = payload[1:].decode("latin-1", "replace").rstrip("\r")
    return {"kind": name, **extra}


def parse_compact_records(body: bytes) -> tuple[list, bool]:
    """Parse a sequence of compact records.

    Wire format per record (per FlexNet skill §1.6):
      CALL(6) SSID_LO(1) SSID_HI(1) RTT_decimal+ ' '
    With an optional '?' indirect prefix immediately before RTT digits.

    A trailing '-' just before the final '\r' marks the whole batch as
    withdrawal (RTT=60000 effectively).
    """
    # Strip trailing \r
    if body.endswith(b"\r"):
        body = body[:-1]
    trailing_minus = False
    if body.endswith(b"-"):
        trailing_minus = True
        body = body[:-1]
    records = []
    text = body.decode("latin-1", "replace")
    # records are space-separated; each starts with 6-char callsign + 2 SSID chars
    # then RTT (with optional '?') then trailing space (already split).
    tokens = text.split(" ")
    # tokens can be re-joined: each "record" needs the callsign field
    # which is 6 chars; we walk tokens in order, but it's simpler to
    # tokenise differently — every record is exactly: 8 chars (call+ssid)
    # followed by [?]+digits. Let's just walk the raw string with a
    # position index.
    s = text
    i = 0
    while i < len(s):
        # skip whitespace
        while i < len(s) and s[i] == " ":
            i += 1
        if i + 8 > len(s):
            break
        call_field = s[i:i + 6]   # space-padded callsign
        ssid_lo_c = s[i + 6]
        ssid_hi_c = s[i + 7]
        i += 8
        # optional '?' indirect prefix
        indirect = False
        if i < len(s) and s[i] == "?":
            indirect = True
            i += 1
        # RTT decimal digits
        rtt_start = i
        while i < len(s) and s[i].isdigit():
            i += 1
        if rtt_start == i:
            break
        rtt = int(s[rtt_start:i])
        records.append({
            "call": call_field.strip(),
            "ssid_lo": ord(ssid_lo_c) - 0x30 if ssid_lo_c else None,
            "ssid_hi": ord(ssid_hi_c) - 0x30 if ssid_hi_c else None,
            "rtt": rtt,
            "indirect": indirect,
        })
    return records, trailing_minus


def parse_capture(path: Path) -> dict:
    frames = []
    cur_header = None
    cur_payload = bytearray()
    n_seen = 0

    def flush():
        nonlocal cur_header, cur_payload, n_seen
        if cur_header is None:
            return
        n_seen += 1
        payload = bytes(cur_payload)
        # The script's xnet hex header shows [PLEN] — keep both for sanity
        rec = {
            "idx": n_seen,
            "port": int(cur_header["port"]),
            "src": cur_header["src"],
            "dst": cur_header["dst"],
            "digis": (cur_header.get("digis") or "").split(),
            "ctl": cur_header["ctl"],
            "pid": (cur_header.get("pid") or "").upper(),
            "plen_hdr": int(cur_header["plen"]) if cur_header.get("plen") else None,
            "plen_actual": len(payload),
            "hex": payload.hex(),
        }
        if rec["pid"] == "CE":
            rec.update(classify_ce(payload))
        elif rec["pid"] == "CF":
            # NetROM-compat L3 — keep first 6 bytes for tagging
            if payload.startswith(b"L3RTT:"):
                rec["kind"] = "cf-l3rtt"
            else:
                rec["kind"] = "cf"
        elif rec["pid"] == "F0":
            rec["kind"] = "f0"
        else:
            rec["kind"] = "ctrl"
        frames.append(rec)
        cur_header = None
        cur_payload = bytearray()

    with path.open("r", encoding="utf-8", errors="replace") as fp:
        for ln in fp:
            ln_stripped = ln.rstrip("\r\n")
            if not ln_stripped:
                # blank line ends a frame
                flush()
                continue
            m = FRAME_HEADER_RE.match(ln_stripped)
            if m:
                # close any prior frame
                flush()
                cur_header = m.groupdict()
                continue
            # try to interpret as hex row (continuing current frame)
            if cur_header is not None:
                chunk = parse_hex_row(ln_stripped)
                if chunk:
                    cur_payload.extend(chunk)
    flush()
    return {"input": str(path), "frame_count": len(frames), "frames": frames}


def inventory(parsed: dict) -> dict:
    by_kind = Counter()
    by_kind_refined = Counter()
    by_pair = Counter()
    callsigns = Counter()
    indirect_count = 0
    direct_count = 0
    for f in parsed["frames"]:
        kind = f.get("kind", "?")
        by_kind[kind] += 1
        if "kind_refined" in f:
            by_kind_refined[f["kind_refined"]] += 1
        by_pair[f"{f['src']}->{f['dst']}"] += 1
        for r in f.get("records", []) or []:
            cs = r["call"]
            if cs:
                callsigns[cs] += 1
                if r["indirect"]:
                    indirect_count += 1
                else:
                    direct_count += 1
    return {
        "by_kind": dict(by_kind.most_common()),
        "by_kind_refined": dict(by_kind_refined.most_common()),
        "by_pair": dict(by_pair.most_common()),
        "advertised_callsigns": dict(callsigns.most_common()),
        "indirect_records": indirect_count,
        "direct_records": direct_count,
        "unique_callsigns_advertised": len(callsigns),
    }


def transit_analysis(parsed_a: dict, parsed_b: dict) -> dict:
    """Set algebra on advertised destinations between two ports."""
    set_a = set()
    set_b = set()
    rtt_a = {}
    rtt_b = {}
    for f in parsed_a["frames"]:
        for r in f.get("records", []) or []:
            if r["call"]:
                set_a.add(r["call"])
                rtt_a.setdefault(r["call"], []).append(
                    (r["rtt"], r["indirect"], r["ssid_lo"], r["ssid_hi"])
                )
    for f in parsed_b["frames"]:
        for r in f.get("records", []) or []:
            if r["call"]:
                set_b.add(r["call"])
                rtt_b.setdefault(r["call"], []).append(
                    (r["rtt"], r["indirect"], r["ssid_lo"], r["ssid_hi"])
                )
    only_a = sorted(set_a - set_b)
    only_b = sorted(set_b - set_a)
    both = sorted(set_a & set_b)
    return {
        "set_a_count": len(set_a),
        "set_b_count": len(set_b),
        "only_a": only_a,
        "only_b": only_b,
        "both": both,
        "rtt_samples_a": {k: rtt_a[k][:5] for k in both[:30]},
        "rtt_samples_b": {k: rtt_b[k][:5] for k in both[:30]},
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+", help="*_raw.txt files (mo -i hex format)")
    ap.add_argument("--transit", action="store_true",
                    help="If two inputs, compare advertised dest sets")
    args = ap.parse_args()

    parsed = {}
    for p in args.inputs:
        path = Path(p)
        ps = parse_capture(path)
        ps["inventory"] = inventory(ps)
        out = path.with_suffix(".h.json")
        with out.open("w") as fp:
            json.dump(ps, fp, default=str, indent=2)
        parsed[path.name] = ps
        inv = ps["inventory"]
        print(f"[{path.name}] frames={ps['frame_count']} → {out.name}")
        print(f"   by_kind: {dict(list(inv['by_kind'].items())[:8])}")
        print(f"   by_kind_refined: {dict(list(inv['by_kind_refined'].items())[:8])}")
        print(f"   unique callsigns advertised: {inv['unique_callsigns_advertised']}")
        print(f"   direct records: {inv['direct_records']}  indirect (`?`): {inv['indirect_records']}")

    if args.transit and len(args.inputs) == 2:
        a, b = parsed.values()
        ta = transit_analysis(a, b)
        print(f"\n[transit] |set_A|={ta['set_a_count']}  |set_B|={ta['set_b_count']}")
        print(f"  both ports ({len(ta['both'])}):  {ta['both'][:20]}")
        print(f"  only in A  ({len(ta['only_a'])}):  {ta['only_a'][:20]}")
        print(f"  only in B  ({len(ta['only_b'])}):  {ta['only_b'][:20]}")
        # show RTT for a few in 'both'
        print("\n  RTT comparison (call: A_samples vs B_samples) for first 8 in 'both':")
        for cs in ta['both'][:8]:
            a_s = ta['rtt_samples_a'].get(cs)
            b_s = ta['rtt_samples_b'].get(cs)
            print(f"    {cs:8s}  A={a_s}  B={b_s}")


if __name__ == "__main__":
    main()
