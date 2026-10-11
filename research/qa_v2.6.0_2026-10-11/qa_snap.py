#!/usr/bin/env python3
"""qa_snap.py - one raw snapshot of every FlexNet node (v2.6.0 QA).

Runs on gw inside the pr-digi-mcp venv (needs nodes.yaml + credentials).
One session per node; LinBPQ: FL + D, (X)Net: L + D. Appends one JSON line
to the output file so repeated runs form a time series.
"""
import asyncio, json, sys, time
from pr_digi_mcp.config import load_nodes
from pr_digi_mcp.transports import open_transport

LINBPQ = ["IW2OHX-13", "IR2UFV", "IW2OHX-12"]
XNET = ["IW2OHX-14", "IW2OHX-4", "IQ2LB"]

async def snap(call, nodes):
    cfg = nodes[call]
    out = {}
    try:
        async with open_transport(cfg, nodes) as xn:
            if call in LINBPQ:
                await xn.elevate_sys()
                out["FL"] = await xn.run_command("FL", idle_ms=800, max_wait_s=20)
                out["D"] = await xn.run_command("D", idle_ms=1000, max_wait_s=25)
            else:
                out["L"] = await xn.run_command("L", idle_ms=800, max_wait_s=20)
                out["D"] = await xn.run_command("D", idle_ms=1000, max_wait_s=25)
    except Exception as e:
        out["error"] = repr(e)
    return call, out

async def main():
    nodes = load_nodes()
    res = await asyncio.gather(*(asyncio.wait_for(snap(c, nodes), 120)
                                 for c in LINBPQ + XNET), return_exceptions=True)
    rec = {"t": time.time(), "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    for r in res:
        if isinstance(r, Exception):
            continue
        rec[r[0]] = r[1]
    with open(sys.argv[1], "a") as f:
        f.write(json.dumps(rec) + "\n")

asyncio.run(main())
