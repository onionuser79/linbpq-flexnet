#!/usr/bin/env python3
"""conntest.py - timed FlexNet user connects from LinBPQ nodes (v2.6.0 QA).

For every (node, dest): log in, `C dest`, wait up to 90 s for the outcome,
then `B` and close. Prints one JSON line per attempt. Runs on gw in the
pr-digi-mcp venv.
"""
import asyncio, json, re, sys, time
from pr_digi_mcp.config import load_nodes
from pr_digi_mcp.transports import open_transport

OK = re.compile(rb"Connected to|\*\*\* connected to|Link \S+ connected", re.I)
BAD = re.compile(rb"Failure with|Busy from|not in|No Route|Not found|Disconnected from|"
                 rb"link failure|Sorry|unknown|\*\*\* failure|failed|DM received", re.I)

async def attempt(nodes, node, dest):
    rec = {"node": node, "dest": dest, "utc": time.strftime("%H:%M:%SZ", time.gmtime())}
    try:
        async with open_transport(nodes[node], nodes) as xn:
            await xn._send_line(f"C {dest}")
            t0, buf = time.monotonic(), b""
            while time.monotonic() - t0 < 90:
                buf += await xn._read_chunk(timeout=0.5)
                if OK.search(buf) or BAD.search(buf):
                    break
            rec["secs"] = round(time.monotonic() - t0, 1)
            rec["ok"] = bool(OK.search(buf)) and not BAD.search(buf.split(b"onnected to")[0] if b"onnected to" in buf else b"")
            rec["text"] = buf.decode("latin-1", "replace").replace("\r", " | ")[-220:]
            if rec["ok"]:
                # The far end proves itself only by talking: (X)Net acks hop
                # by hop, so "connected" may be the first leg only.
                t1, far = time.monotonic(), b""
                while time.monotonic() - t1 < 20 and len(far) < 40:
                    far += await xn._read_chunk(timeout=0.5)
                tail = buf.split(b"onnected to", 1)[1] if b"onnected to" in buf else b""
                far = (tail + far).strip()
                rec["far_bytes"] = len(far)
                rec["far_secs"] = round(time.monotonic() - t0, 1)
                rec["far"] = far.decode("latin-1", "replace").replace("\r", " | ")[:160]
                await xn._send_line("B")
                await asyncio.sleep(3)
    except Exception as e:
        rec["error"] = repr(e)
    print(json.dumps(rec), flush=True)

async def main():
    nodes = load_nodes()
    for pair in sys.argv[1:]:
        node, dest = pair.split(":")
        await attempt(nodes, node, dest)
        await asyncio.sleep(5)

asyncio.run(main())
