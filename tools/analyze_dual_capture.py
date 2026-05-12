#!/usr/bin/env python3
"""
analyze_dual_capture.py — compare two xnet_agent JSON captures of FlexNet
                          traffic from neighbouring hops.

Use with a dual-port capture across a suspected forwarder: run
xnet_agent.py on each of two adjacent nodes on the port that faces
the middle node, then feed the two JSON outputs into this script.

For each capture:
  - summary: total frames, PID histogram, callsign-pair histogram
  - CE/CF frames isolated, byte-0 classified (type 6/7 = PATH_REQ/REP,
    others = D-table compact / dest broadcast variants)
  - D-table snapshot count + last snapshot size
  - L3RTT events

Cross-cut:
  - Did the same QSO-keyed type-6/7 frame appear on both sides? (forwarding
    evidence)
  - Were D queries visible as outbound CE frames or just local cache
    lookups?
"""
import json, re, sys
from collections import Counter, defaultdict

def fb(f):
    """First payload byte from hex_lines"""
    for hl in f.get("hex_lines") or []:
        m = re.match(r"\s*[0-9A-Fa-f]{4}\s+([0-9A-Fa-f]{2})", hl)
        if m:
            return int(m.group(1), 16)
    return None

def payload_bytes(f, n=80):
    out = bytearray()
    for hl in f.get("hex_lines") or []:
        m = re.match(r"\s*[0-9A-Fa-f]{4}\s+((?:[0-9A-Fa-f]{2}\s+)+)", hl)
        if not m: continue
        for tok in m.group(1).split():
            try: out.append(int(tok, 16))
            except: pass
        if len(out) >= n: break
    return bytes(out[:n])

def summarize(label, path):
    print(f"\n=== {label} ({path}) ===")
    try:
        data = json.load(open(path))
    except Exception as e:
        print(f"  ERROR: {e}")
        return None

    frames = data.get("monitor_frames", []) or []
    print(f"  frames captured        : {len(frames)}")
    print(f"  D snapshots            : {len(data.get('d_snapshots', []) or [])}")
    print(f"  L snapshots            : {len(data.get('l_snapshots', []) or [])}")
    print(f"  L3RTT events           : {len(data.get('l3rtt_events', []) or [])}")

    pid_hist = Counter()
    pair_hist = Counter()
    type67 = []
    ce_others = Counter()

    for f in frames:
        pid = f.get("pid") or "??"
        pid_hist[pid] += 1
        pair = f"{f.get('from','?'):11s} -> {f.get('to','?'):11s}"
        pair_hist[pair] += 1
        if pid in ("CE", "CF"):
            b = fb(f)
            if b in (0x36, 0x37):
                type67.append((f, b))
            else:
                ce_others[f"0x{b:02X}" if b is not None else "??"] += 1

    print(f"\n  PID histogram          :")
    for pid, n in pid_hist.most_common():
        print(f"    {pid:>3s}  {n}")

    print(f"\n  top callsign pairs (first 10):")
    for p, n in pair_hist.most_common(10):
        print(f"    {p}  {n}")

    print(f"\n  CE byte0 distribution (non-type6/7):")
    for b, n in ce_others.most_common():
        print(f"    {b}  {n}")

    print(f"\n  CE type-6/7 frames     : {len(type67)}")
    for f, b in type67:
        ts = f["timestamp"].split("T")[1][:8]
        kind = "REQ" if b == 0x36 else "REP"
        asc = payload_bytes(f, 70).replace(b"\r", b"<CR>")
        print(f"    {ts}  {f['from']:11s} -> {f['to']:11s}  "
              f"{kind} len={f.get('length')}  {asc!r}")

    # D-table size from last snapshot
    snaps = data.get("d_snapshots") or []
    if snaps:
        last = snaps[-1]
        records = last.get("records") or []
        print(f"\n  Last D snapshot (t+{last.get('elapsed_s','?')}s):")
        print(f"    records: {len(records)}")
        # Show first ~10 entries
        for r in records[:10]:
            print(f"      {r}")

    return data


def crosscut(a, b):
    print("\n=== CROSS-CUT: forwarding evidence ===")
    if not a or not b:
        print("  (one or both captures missing — skipping)")
        return
    # Extract type-6/7 QSOs from each
    def t67_qsos(data):
        out = []
        for f in data.get("monitor_frames", []) or []:
            if f.get("pid") not in ("CE", "CF"): continue
            byte0 = None
            for hl in f.get("hex_lines") or []:
                m = re.match(r"\s*[0-9A-Fa-f]{4}\s+([0-9A-Fa-f]{2})", hl)
                if m:
                    byte0 = int(m.group(1), 16)
                    break
            if byte0 not in (0x36, 0x37): continue
            pl = payload_bytes(f, 80)
            # QSO field = 5 ASCII chars bytes 2-6 (right-justified, top bit of
            # byte0 may indicate TRACE kind)
            qso = pl[2:7] if len(pl) >= 7 else b""
            out.append((qso, byte0, f["from"], f["to"],
                        f["timestamp"].split("T")[1][:8]))
        return out

    qa = t67_qsos(a)
    qb = t67_qsos(b)
    print(f"  type-6/7 on side A: {len(qa)}")
    for q in qa: print(f"    A  {q[4]}  {q[0]!r}  {'REQ' if q[1]==0x36 else 'REP'}  {q[2]}->{q[3]}")
    print(f"  type-6/7 on side B: {len(qb)}")
    for q in qb: print(f"    B  {q[4]}  {q[0]!r}  {'REQ' if q[1]==0x36 else 'REP'}  {q[2]}->{q[3]}")

    set_a = {q[0] for q in qa}
    set_b = {q[0] for q in qb}
    shared = set_a & set_b
    print(f"\n  QSO-keys shared (forwarded across IW2OHX-12): {len(shared)}")
    for s in shared:
        print(f"    qso={s!r}")


def main():
    if len(sys.argv) < 3:
        print("usage: analyze_dual_capture.py <side_a.json> <side_b.json>")
        sys.exit(1)
    a = summarize("IW2OHX-14 port 1 (faces -12)", sys.argv[1])
    b = summarize("IW2OHX-4 port 3 (faces -12)", sys.argv[2])
    crosscut(a, b)

if __name__ == "__main__":
    main()
