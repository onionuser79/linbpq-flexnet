"""qa_lib.py - parsers and collectors shared by qa_run.py and qa_score.py.

Parsers are pure stdlib so the scorer runs anywhere (macmini included).
Collectors import pr_digi_mcp lazily: they only run on iw2ohx-gw inside the
pr-digi-mcp venv, where nodes.yaml and the credentials live.
"""
import asyncio
import hashlib
import os
import re
import subprocess
import time

INFINITY = 60000

# ---------------------------------------------------------------- parsers

_FL_LINK = re.compile(
    r"^(?P<call>[A-Z0-9]+(?:-\d+)?)\s+(?P<port>\d+)\s+(?P<status>[A-Z]+)\s+"
    r"(?P<lt>\S+)\s+(?:(?P<qt>\d+|-)\s+)?(?P<ka>\d+)\s+(?P<uptime>(?:\d+d\s+)?\d+:\d+(?::\d+)?)\s+"
    r"(?P<routes>\d+)\s*$")
_FL_SESS = re.compile(r"Sessions:\s+started=(\d+)\s+closed=(\d+)\s+reaped=(\d+)\s+slot-moves=(\d+)")
_FL_KISS = re.compile(r"^\s+(?P<call>[A-Z0-9]+(?:-\d+)?)\s+port\s+(?P<port>\d+)\s+\S+\s+"
                      r"(?P<state>\S+)\s+opens=(?P<opens>\d+)")
_FL_L2 = re.compile(r"extended=(\d+)\s+contracted=(\d+)\s+declined=(\d+)")
_FL_XPORT = re.compile(r"cross-port \S+: frames=(\d+)")
_FL_PATH = re.compile(r"Path forwarding \S+: forwarded=(\d+) declined=(\d+) replies-relayed=(\d+)")
_FL_PEER = re.compile(r"^(?P<call>[A-Z0-9]+(?:-\d+)?)\s+(?P<family>xnet|PCF|BPQ|\S+)\s+"
                      r"(?P<learned>\d+)\s+(?P<direct>\d+)\s+(?P<advert>\d+)\s+"
                      r"(?P<queued>\d+)\s+(?P<tokens>[\d.]+)\s*$")
_DEST = re.compile(r"\b([A-Z0-9]{2,6})\s+(\d+)-(\d+)\s+(\d+)(!?)")
_XL_HEAD = re.compile(r"^\s*(?P<port>\d+):(?P<call>[A-Z0-9]+(?:-\d+)?)\s+(?P<dst>\d*)\s*\+?"
                      r"(?P<flag>[QIF])\s+(?P<qt>\S+)\s+(?P<rtt>\S+)\s+(?P<tx>\S+)\s+(?P<rest>.*)$")
_ROUTE = re.compile(r"route:\s*(.+)$", re.M)
_VERSION = re.compile(r"FlexNet\s+(v\d+\.\d+\.\d+[\w.-]*)", re.I)


def uptime_s(text):
    """'2d 02:37' (days, HH:MM) or '00:50:41' (HH:MM:SS) -> seconds."""
    text = text.strip()
    m = re.match(r"(\d+)d\s+(\d+):(\d+)$", text)
    if m:
        return int(m[1]) * 86400 + int(m[2]) * 3600 + int(m[3]) * 60
    parts = [int(p) for p in text.split(":")]
    while len(parts) < 3:
        parts.insert(0, 0)
    return parts[0] * 3600 + parts[1] * 60 + parts[2]


def parse_fl(text):
    """LinBPQ sysop FL -> dict. Link rows and peer rows share session order."""
    out = {"links": [], "peers": [], "kiss": [], "sessions": None, "l2fwd": None, "pathfwd": None}
    in_peers = False
    for line in text.replace("\r", "\n").splitlines():
        if line.startswith("Peer ") and "Family" in line:
            in_peers = True
            continue
        m = _FL_LINK.match(line)
        if m and not in_peers:
            out["links"].append({"call": m["call"], "port": int(m["port"]), "status": m["status"],
                                 "lt": m["lt"], "qt": int(m["qt"]) if m["qt"] not in (None, "-") else None,
                                 "ka": int(m["ka"]), "uptime_s": uptime_s(m["uptime"]),
                                 "routes": int(m["routes"])})
            continue
        m = _FL_PEER.match(line)
        if m and in_peers:
            out["peers"].append({"call": m["call"], "family": m["family"], "learned": int(m["learned"]),
                                 "direct": int(m["direct"]), "advert": int(m["advert"]),
                                 "queued": int(m["queued"]), "tokens": float(m["tokens"])})
            continue
        m = _FL_KISS.match(line)
        if m:
            out["kiss"].append({"call": m["call"], "port": int(m["port"]), "state": m["state"],
                                "opens": int(m["opens"])})
            continue
        m = _FL_SESS.search(line)
        if m:
            out["sessions"] = dict(zip(("started", "closed", "reaped", "slot_moves"), map(int, m.groups())))
            continue
        m = _FL_L2.search(line)
        if m:
            out["l2fwd"] = dict(zip(("extended", "contracted", "declined"), map(int, m.groups())))
            continue
        m = _FL_XPORT.search(line)
        if m and out["l2fwd"] is not None:
            out["l2fwd"]["xport"] = int(m[1])
            continue
        m = _FL_PATH.search(line)
        if m:
            out["pathfwd"] = dict(zip(("forwarded", "declined", "relayed"), map(int, m.groups())))
    return out


def parse_dests(text):
    """BPQ or (X)Net D table -> {(call, lo, hi): cost}. Infinity rows dropped."""
    out = {}
    for call, lo, hi, cost, _ in _DEST.findall(text or ""):
        c = int(cost)
        if c < INFINITY:
            out[(call, int(lo), int(hi))] = c
    return out


def parse_xnet_l(text):
    """(X)Net L (or flexnetd linkstats) -> list of link rows."""
    rows = []
    for line in (text or "").replace("\r", "\n").splitlines():
        m = _XL_HEAD.match(line)
        if not m:
            continue
        rest = m["rest"].split()
        rr = None
        if len(rest) >= 2:
            try:
                rr = float(rest[-2])
            except ValueError:
                rr = None
        qt = int(m["qt"]) if m["qt"].isdigit() else None
        rows.append({"port": int(m["port"]), "call": m["call"], "flag": m["flag"],
                     "dst": int(m["dst"]) if m["dst"] else None, "qt": qt, "rtt": m["rtt"],
                     "rr": rr, "connect": " ".join(rest[:-5]) if len(rest) > 5 else ""})
    return rows


def parse_flexnetd_dests(text):
    """flexnetd 'destinations' file -> {(call, lo, hi): cost}."""
    out = {}
    for line in (text or "").splitlines()[1:]:
        p = line.split()
        if len(p) >= 3 and "-" in p[1] and p[2].isdigit():
            lo, hi = p[1].split("-", 1)
            out[(p[0], int(lo), int(hi))] = int(p[2])
    return out


def parse_route(text):
    """'*** route: A B ... DEST' -> list of calls, or None."""
    m = _ROUTE.findall(text or "")
    return m[-1].split() if m else None


def parse_version(text):
    m = _VERSION.search(text or "")
    return m[1] if m else None


def base_ssid(call):
    if "-" in call:
        b, s = call.rsplit("-", 1)
        return b, int(s) if s.isdigit() else 0
    return call, 0


def covers(dests, call):
    """True if a destination row covers CALL-SSID."""
    b, s = base_ssid(call)
    return any(k[0] == b and k[1] <= s <= k[2] for k in dests)

# -------------------------------------------------------- local (gw) probes


def sh(cmd, timeout=30):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout, r.stderr


def gw_process(dirpath):
    """pid + start epoch of the linbpq under dirpath, or None."""
    rc, out, _ = sh(f"pgrep -f '^{dirpath}/linbpq'")
    pids = out.split()
    if rc != 0 or not pids:
        return None
    pid = pids[0]
    try:
        start_ticks = int(open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()[19])
        btime = next(int(l.split()[1]) for l in open("/proc/stat") if l.startswith("btime"))
        start = btime + start_ticks / os.sysconf("SC_CLK_TCK")
    except (OSError, StopIteration, ValueError, IndexError):
        start = None
    return {"pid": int(pid), "start": start}


def gw_binary_facts(dirpath):
    path = f"{dirpath}/linbpq"
    facts = {"path": path}
    try:
        facts["sha256"] = hashlib.sha256(open(path, "rb").read()).hexdigest()
    except OSError as e:
        facts["error"] = repr(e)
        return facts
    _, out, _ = sh(f"strings '{path}' | grep -c 'FlexNet: ' || true")
    facts["flexnet_strings"] = int(out.strip() or 0)
    return facts


def gw_cfg_invariants(dirpath):
    _, out, _ = sh(f"sudo -n cat '{dirpath}/bpq32.cfg'")
    lines = [l.split(";")[0].strip() for l in out.splitlines()]
    return {"DisconnectOnClose=0": any(re.fullmatch(r"DisconnectOnClose\s*=\s*0", l, re.I) for l in lines),
            "OnlyVer2point0=1": any(re.fullmatch(r"OnlyVer2point0\s*=\s*1", l, re.I) for l in lines),
            "readable": bool(out)}


def gw_flexnetd_state(state_dir):
    st = {}
    for name in ("linkstats", "destinations"):
        try:
            st[name] = open(os.path.join(state_dir, name)).read()
        except OSError as e:
            st[name] = ""
            st[f"{name}_error"] = repr(e)
    return st

# ------------------------------------------------------- remote collectors


async def _session(cfg_all, call, fn, timeout):
    from pr_digi_mcp.transports import open_transport
    try:
        async with open_transport(cfg_all[call], cfg_all) as xn:
            return await asyncio.wait_for(fn(xn), timeout)
    except Exception as e:  # one node failing must not sink the snapshot
        return {"error": repr(e)}


async def snap_node(cfg_all, call, kind, timeout):
    async def linbpq(xn):
        await xn.elevate_sys()
        # "FL +": v2.9.0+ prints only the link overview for a bare FL; the
        # FlexNet detail parse_fl reads needs "+". Older builds ignore it.
        return {"FL": await xn.run_command("FL +", idle_ms=800, max_wait_s=20),
                "D": await xn.run_command("D", idle_ms=1000, max_wait_s=25)}

    async def xnet(xn):
        return {"L": await xn.run_command("L", idle_ms=800, max_wait_s=20),
                "D": await xn.run_command("D", idle_ms=1000, max_wait_s=25)}
    return await _session(cfg_all, call, linbpq if kind == "linbpq" else xnet, timeout)


async def run_cmds(cfg_all, call, cmds, sys_needed, timeout):
    async def fn(xn):
        if sys_needed:
            await xn.elevate_sys()
        return {c: await xn.run_command(c, idle_ms=800, max_wait_s=20) for c in cmds}
    return await _session(cfg_all, call, fn, timeout)


_OK = re.compile(rb"Connected to|\*\*\* connected to|Link \S+ connected", re.I)
_BAD = re.compile(rb"Failure with|Busy from|not in|No Route|Not found|Disconnected from|"
                  rb"link failure|Sorry|unknown|\*\*\* failure|failed|DM received", re.I)


async def connect_test(cfg_all, node, cmd, connect_timeout, banner_wait):
    """Send CMD (a C command), success = far-end bytes after 'connected'.

    (X)Net acknowledges hop by hop, so 'connected' alone may be the first leg;
    the banner is the proof (v2.6.0 QA rule)."""
    async def fn(xn):
        rec = {}
        await xn._send_line(cmd)
        t0, buf = time.monotonic(), b""
        while time.monotonic() - t0 < connect_timeout:
            buf += await xn._read_chunk(timeout=0.5)
            if _OK.search(buf) or _BAD.search(buf):
                break
        rec["secs"] = round(time.monotonic() - t0, 1)
        head = buf.split(b"onnected to")[0] if b"onnected to" in buf else buf
        rec["connected"] = bool(_OK.search(buf)) and not _BAD.search(head)
        far = b""
        if rec["connected"]:
            t1 = time.monotonic()
            far = buf.split(b"onnected to", 1)[1] if b"onnected to" in buf else b""
            while time.monotonic() - t1 < banner_wait and len(far.strip()) < 40:
                far += await xn._read_chunk(timeout=0.5)
            await xn._send_line("B")
            await asyncio.sleep(3)
        rec["far_bytes"] = len(far.strip())
        rec["ok"] = rec["connected"] and rec["far_bytes"] > 0
        rec["text"] = buf.decode("latin-1", "replace").replace("\r", " | ")[-240:]
        rec["far"] = far.decode("latin-1", "replace").replace("\r", " | ")[:160]
        return rec
    return await _session(cfg_all, node, fn, connect_timeout + banner_wait + 30)


async def path_query(cfg_all, node, dest, wait_s):
    async def fn(xn):
        await xn._send_line(f"D {dest}")
        t0, buf = time.monotonic(), b""
        while time.monotonic() - t0 < wait_s:
            buf += await xn._read_chunk(timeout=0.5)
            if b"route:" in buf and buf.endswith((b"\r", b"\n")):
                break
        text = buf.decode("latin-1", "replace")
        return {"secs": round(time.monotonic() - t0, 1), "route": parse_route(text),
                "text": text.replace("\r", " | ")[-300:]}
    return await _session(cfg_all, node, fn, wait_s + 40)
