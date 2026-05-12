#!/usr/bin/env python3
"""
xnet_agent.py — FlexNet protocol research agent for (X)Net nodes
Connects via telnet, authenticates, elevates to SysOp, then:
  - Takes a full baseline snapshot (L, L*, D, U, P)
  - Runs monitor +2 for a configurable duration
  - Takes periodic D+L snapshots every 5 minutes
  - Parses L3RTT frames and destination table records on the fly
  - Saves everything to a structured JSON log

Usage:
  python3 xnet_agent.py --host your.node.example.org --port 23 \
      --user USERNAME --password LOGINPASS --syspass SYSPASSWORD \
      --duration 3600 --output flexnet_capture.json

  Or use a config file:
  python3 xnet_agent.py --config xnet.conf

Config file format (INI):
  [xnet]
  host     = your.node.example.org
  port     = 23
  user     = USERNAME
  password = LOGINPASS
  syspass  = SYSPASSWORD
  duration = 3600
  output   = flexnet_capture.json
"""

import socket
import re
import json
import time
import argparse
import configparser
import sys
import os
from datetime import datetime, timezone


# ── tunables ────────────────────────────────────────────────────────────────
RECV_BUF       = 4096
RECV_TIMEOUT   = 8.0      # seconds to wait for prompt after a command
MONITOR_POLL   = 0.2      # polling interval while in monitor mode
SNAPSHOT_EVERY = 300      # seconds between periodic D+L snapshots
PROMPT         = "=>"     # Xnet node prompt (may be preceded by callsign)
ENCODING       = "latin-1"

# ── regex patterns ───────────────────────────────────────────────────────────
RE_SYS_CHALLENGE = re.compile(
    r"[Pp]assword\s+characters?\s*[:\-]?\s*([\d\s,]+)", re.IGNORECASE
)
RE_SYS_CHALLENGE_ALT = re.compile(
    r"[Cc]har(?:acter)?s?\s+([\d](?:\s*[,\s]\s*[\d])*)", re.IGNORECASE
)
RE_L3RTT = re.compile(
    r"L3RTT:\s+(\d+)\s+"        # counter
    r"(\S+)\s+"                  # val1
    r"(\S+)\s+"                  # val2
    r"(\S+)"                     # node_serial (may be multi-field, captured loosely)
)
RE_M_VALUE   = re.compile(r"\$M(\d+)")
RE_N_VALUE   = re.compile(r"\$N")
RE_LT        = re.compile(r"\bLT\s+(\d+)\b")
RE_L3_HDR    = re.compile(
    r"L3 fm (\S+)\s+to (\S+)\s+LT (\d+)\s+(\S+)\s+IN=(\d+)\s+ID=(\d+)"
    r"\s+S\((\d+)\)\s+R\((\d+)\)"
)
RE_DEST_RECORD = re.compile(
    r"^([A-Z0-9]+-?\d*)\s+"     # callsign-ssid
    r"(\d+)/(\d+)"              # RTT / SSID_max
    r"(?:\s+(\d+)\[(\d+)\])?"   # optional hops[quality]
    r"(?:\s+'([^']+)')?"        # optional 'ALIAS'
    r"\s*$"
)
RE_FRAME_HDR = re.compile(
    r"^\d+:fm\s+(\S+)\s+to\s+(\S+)\s+ctl\s+(\S+)"
    r"(?:\s+pid\s+([0-9A-Fa-f]+))?"
    r"(?:\s+\[(\d+)\])?"
)
RE_LINK_ROW = re.compile(
    r"^(\S+)\s+"                 # callsign
    r"(\d+|\-{3})\s*"           # RTT or ---
    r"(/\s*(\d+|\-{3}))?\s*"    # optional /RTT_reverse
    r"(.*)"                      # rest (port, via, etc.)
)


# ── utility ──────────────────────────────────────────────────────────────────
def ts():
    return datetime.now(timezone.utc).isoformat()

def log(msg):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


# ── telnet socket wrapper ────────────────────────────────────────────────────
class XnetSession:
    def __init__(self, host, port=23):
        self.host = host
        self.port = port
        self.sock = None
        self._buf = b""

    def connect(self):
        log(f"Connecting to {self.host}:{self.port}")
        self.sock = socket.create_connection((self.host, self.port), timeout=30)
        self.sock.settimeout(RECV_TIMEOUT)
        log("Connected")

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass

    def _recv_raw(self, timeout=None):
        """Read available bytes, stripping telnet IAC sequences."""
        if timeout is not None:
            self.sock.settimeout(timeout)
        data = b""
        try:
            while True:
                chunk = self.sock.recv(RECV_BUF)
                if not chunk:
                    break
                data += chunk
                # small delay then check if more is coming
                self.sock.settimeout(0.15)
        except socket.timeout:
            pass
        finally:
            self.sock.settimeout(RECV_TIMEOUT)
        # strip telnet IAC negotiation bytes (0xFF sequences)
        clean = bytearray()
        i = 0
        while i < len(data):
            b = data[i]
            if b == 0xFF and i + 2 < len(data):
                i += 3  # IAC + cmd + option
            else:
                clean.append(b)
                i += 1
        return bytes(clean)

    def read_until(self, marker, timeout=None):
        """Read until marker string appears. Returns full accumulated text."""
        deadline = time.time() + (timeout or RECV_TIMEOUT)
        self.sock.settimeout(0.3)
        accumulated = self._buf.decode(ENCODING, errors="replace")
        self._buf = b""
        while time.time() < deadline:
            try:
                chunk = self.sock.recv(RECV_BUF)
                if chunk:
                    # strip IAC
                    clean = bytearray()
                    i = 0
                    while i < len(chunk):
                        bv = chunk[i]
                        if bv == 0xFF and i + 2 < len(chunk):
                            i += 3
                        else:
                            clean.append(bv)
                            i += 1
                    accumulated += bytes(clean).decode(ENCODING, errors="replace")
                    if marker in accumulated:
                        return accumulated
            except socket.timeout:
                pass
        return accumulated  # return what we have even if marker not found

    def send(self, cmd):
        """Send a command followed by CR."""
        data = (cmd + "\r").encode(ENCODING)
        self.sock.sendall(data)

    def cmd(self, command, wait_prompt=True, timeout=None):
        """Send command and optionally wait for the => prompt. Returns response text."""
        log(f"  > {command}")
        self.send(command)
        if wait_prompt:
            resp = self.read_until(PROMPT, timeout=timeout or RECV_TIMEOUT)
        else:
            time.sleep(0.5)
            resp = self._recv_raw(timeout=1.0).decode(ENCODING, errors="replace")
        return resp

    def read_monitor_chunk(self, timeout=MONITOR_POLL):
        """Non-blocking read during monitor mode."""
        self.sock.settimeout(timeout)
        try:
            raw = self.sock.recv(RECV_BUF)
            # strip IAC
            clean = bytearray()
            i = 0
            while i < len(raw):
                bv = raw[i]
                if bv == 0xFF and i + 2 < len(raw):
                    i += 3
                else:
                    clean.append(bv)
                    i += 1
            return bytes(clean).decode(ENCODING, errors="replace")
        except socket.timeout:
            return ""
        except Exception:
            return ""


# ── SYS authentication ───────────────────────────────────────────────────────
def solve_sys_challenge(challenge_text, syspass):
    """
    Xnet asks for specific character positions from the sysop password.
    Example challenge: "Password characters: 3 7 2"
    We return the characters at those 1-based positions concatenated.
    """
    # Try primary pattern first, then alternate
    m = RE_SYS_CHALLENGE.search(challenge_text)
    if not m:
        m = RE_SYS_CHALLENGE_ALT.search(challenge_text)
    if not m:
        log(f"WARNING: Could not parse SYS challenge from: {challenge_text!r}")
        log("         Attempting to extract any digit sequence...")
        digits = re.findall(r"\b(\d+)\b", challenge_text)
        if not digits:
            return None
        positions = [int(d) for d in digits if 1 <= int(d) <= len(syspass)]
    else:
        positions = [int(x) for x in re.findall(r"\d+", m.group(1))]

    log(f"  SYS challenge positions: {positions}")
    response = ""
    for pos in positions:
        if 1 <= pos <= len(syspass):
            response += syspass[pos - 1]
        else:
            log(f"  WARNING: position {pos} out of range for syspass length {len(syspass)}")
            response += "?"
    log(f"  SYS response: {'*' * len(response)} ({len(response)} chars)")
    return response


def authenticate(session, user, password, syspass, debug=False):
    """
    Handle login + SYS privilege elevation.

    Login flow observed:
      server -> banner + "Login: "
      client -> username
      server -> "Password: "
      client -> password
      server -> node prompt "=>"
      client -> "SYS"
      server -> challenge "Give characters N M P of your password: "
      client -> syspass[N-1] + syspass[M-1] + syspass[P-1]
      server -> "=>" (elevated)
    """

    def dlog(msg):
        if debug:
            log(f"  [DBG] {msg}")

    # ── step 1: read banner, wait for Login prompt ────────────────────────
    log("Waiting for login prompt...")
    banner = session.read_until("Login:", timeout=20)
    log(f"  Received {len(banner)} chars")
    dlog(f"banner={banner!r}")

    if "Login:" not in banner:
        # maybe the node uses a different prompt — try "login" lowercase or just ":"
        log("  'Login:' not found — checking for alternative login prompts...")
        if re.search(r"[Ll]ogin|[Uu]ser", banner):
            log("  Found login keyword, proceeding")
        else:
            log(f"  WARNING: unexpected banner, last 200 chars: {banner[-200:]!r}")
            log("  Trying to send username anyway...")

    # ── step 2: send username ─────────────────────────────────────────────
    log(f"  Sending username: {user}")
    session.send(user)

    # ── step 3: wait for Password prompt ─────────────────────────────────
    log("  Waiting for password prompt...")
    resp = session.read_until(":", timeout=10)
    dlog(f"after-user={resp!r}")

    if re.search(r"[Pp]ass", resp):
        log("  Got password prompt — sending login password")
        session.send(password)
    else:
        log(f"  WARNING: expected password prompt, got: {resp[-200:]!r}")
        log("  Sending password anyway...")
        session.send(password)

    # ── step 4: wait for node prompt ─────────────────────────────────────
    log("  Waiting for node prompt (=>)...")
    resp = session.read_until(PROMPT, timeout=30)
    dlog(f"after-password={resp!r}")

    if PROMPT in resp:
        log("  Logged in — node prompt received")
    else:
        log(f"  WARNING: node prompt not found. Last 300 chars: {resp[-300:]!r}")
        # Check if we're at a different prompt / menu
        if re.search(r"[Mm]enu|[Ss]elect|[Cc]hoice|\?", resp):
            log("  Looks like an interactive menu — check --debug output")
        log("  Attempting to continue with SYS elevation anyway...")

    # ── step 5: send SYS command ──────────────────────────────────────────
    log("Requesting SYS elevation...")
    session.send("SYS")

    # The challenge may end with ":" or with a digit sequence and newline
    # Read until we get something that looks like a challenge or another prompt
    challenge = session.read_until(":", timeout=15)
    dlog(f"sys-challenge={challenge!r}")
    log(f"  SYS challenge raw: {challenge.strip()[-300:]!r}")

    # ── step 6: solve challenge ───────────────────────────────────────────
    response = solve_sys_challenge(challenge, syspass)
    if response is None:
        log("ERROR: could not solve SYS challenge.")
        log("       Run with --debug and paste the output so the regex can be fixed.")
        return False

    # ── step 7: send response, wait for elevated prompt ───────────────────
    log(f"  Sending SYS response ({len(response)} chars)")
    session.send(response)
    result = session.read_until(PROMPT, timeout=15)
    dlog(f"sys-result={result!r}")
    log(f"  SYS result: {result.strip()[-200:]!r}")

    if PROMPT in result:
        log("  SYS elevation successful — at node prompt")
        return True
    elif re.search(r"[Ee]rror|[Ww]rong|[Ff]ail|[Dd]enied|[Ii]ncorrect", result, re.IGNORECASE):
        log("ERROR: SYS challenge response rejected — check syspass")
        return False
    else:
        log("  WARNING: unclear SYS result — continuing (may not be elevated)")
        return True


# ── parsers ──────────────────────────────────────────────────────────────────
def parse_dest_record(line):
    m = RE_DEST_RECORD.match(line.strip())
    if not m:
        return None
    return {
        "callsign":  m.group(1),
        "rtt_100ms": int(m.group(2)),
        "ssid_max":  int(m.group(3)),
        "f3_outer":  int(m.group(4)) if m.group(4) is not None else None,
        "f3_inner":  int(m.group(5)) if m.group(5) is not None else None,
        "alias":     m.group(6),
        "is_infinity": int(m.group(2)) >= 60000,
    }

def parse_d_table(text):
    records = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("=") or line.startswith("D") or "Dest" in line:
            continue
        r = parse_dest_record(line)
        if r:
            records.append(r)
    return records

def parse_l3rtt_payload(text):
    result = {}
    m = RE_L3RTT.search(text)
    if m:
        result["counter"]     = m.group(1)
        result["val1"]        = m.group(2)
        result["val2"]        = m.group(3)
        result["node_serial"] = m.group(4)
    mm = RE_M_VALUE.search(text)
    if mm:
        result["m_rtt_100ms"] = int(mm.group(1))
    result["has_n_token"] = bool(RE_N_VALUE.search(text))
    return result

def parse_l3_header(line):
    m = RE_L3_HDR.search(line)
    if not m:
        return None
    return {
        "from":   m.group(1),
        "to":     m.group(2),
        "lt":     int(m.group(3)),
        "type":   m.group(4),
        "in":     int(m.group(5)),
        "id":     int(m.group(6)),
        "s":      int(m.group(7)),
        "r":      int(m.group(8)),
    }

def parse_frame_header(line):
    m = RE_FRAME_HDR.match(line.strip())
    if not m:
        return None
    return {
        "from":    m.group(1),
        "to":      m.group(2),
        "ctl":     m.group(3),
        "pid":     m.group(4),
        "length":  int(m.group(5)) if m.group(5) else None,
    }


# ── monitor stream parser (state machine) ────────────────────────────────────
class MonitorParser:
    """
    Accumulates raw monitor text and emits structured frame events.
    Xnet monitor output pattern:
      <port>:fm <FROM> to <TO> ctl <CTL> [pid <PID>] [<len>]
      [L3 fm ... LT ... I --- IN=.. ID=.. S(..) R(..)]
      [hex+ascii dump lines]
      [plain text lines for D-table etc]
    """
    def __init__(self):
        self.frames = []
        self._pending = None   # current frame being assembled
        self._hex_lines = []
        self._text_lines = []
        self._stats = {
            "total_frames": 0,
            "l3rtt_frames": 0,
            "dtable_frames": 0,
            "rr_frames": 0,
            "sabm_frames": 0,
            "ua_frames": 0,
            "disc_frames": 0,
            "other_frames": 0,
        }

    def _flush_pending(self):
        if self._pending is None:
            return
        frame = self._pending.copy()
        frame["hex_lines"]  = self._hex_lines[:]
        frame["text_lines"] = self._text_lines[:]

        # classify and parse payload
        ctl = frame.get("ctl", "")
        text_blob = "\n".join(self._text_lines)
        hex_blob  = "\n".join(self._hex_lines)

        if ctl.upper().startswith("I"):
            frame["frame_class"] = "I"
            if "L3RTT" in text_blob:
                frame["frame_type"] = "L3RTT"
                frame["l3rtt"] = parse_l3rtt_payload(text_blob)
                frame["l3_header"] = parse_l3_header(text_blob)
                self._stats["l3rtt_frames"] += 1
            else:
                dest_records = [parse_dest_record(l)
                                for l in self._text_lines
                                if parse_dest_record(l)]
                if dest_records:
                    frame["frame_type"] = "D_TABLE"
                    frame["dest_records"] = dest_records
                    self._stats["dtable_frames"] += 1
                else:
                    frame["frame_type"] = "I_OTHER"
                    self._stats["other_frames"] += 1
        elif ctl.upper().startswith("RR"):
            frame["frame_class"] = "S"
            frame["frame_type"]  = "RR"
            self._stats["rr_frames"] += 1
        elif ctl.upper().startswith("SABM"):
            frame["frame_class"] = "U"
            frame["frame_type"]  = "SABM"
            self._stats["sabm_frames"] += 1
        elif ctl.upper().startswith("UA"):
            frame["frame_class"] = "U"
            frame["frame_type"]  = "UA"
            self._stats["ua_frames"] += 1
        elif ctl.upper().startswith("DISC"):
            frame["frame_class"] = "U"
            frame["frame_type"]  = "DISC"
            self._stats["disc_frames"] += 1
        else:
            frame["frame_class"] = "?"
            frame["frame_type"]  = "OTHER"
            self._stats["other_frames"] += 1

        self._stats["total_frames"] += 1
        self.frames.append(frame)
        self._pending = None
        self._hex_lines = []
        self._text_lines = []

    def feed(self, text):
        """Feed a chunk of monitor output. Returns list of newly completed frames."""
        before = len(self.frames)
        for line in text.splitlines():
            stripped = line.strip()
            if not stripped:
                continue
            # new frame header?
            hdr = parse_frame_header(stripped)
            if hdr:
                self._flush_pending()
                self._pending = {
                    "timestamp": ts(),
                    **hdr,
                }
            elif self._pending is not None:
                # hex dump line: "0000 XX XX XX ..."
                if re.match(r"^[0-9A-Fa-f]{4}\s+[0-9A-Fa-f]{2}", stripped):
                    self._hex_lines.append(stripped)
                    # extract ASCII portion (after the 16 hex bytes)
                    ascii_m = re.search(r"(?:[0-9A-Fa-f]{2}\s+){1,16}(.+)$", stripped)
                    if ascii_m:
                        self._text_lines.append(ascii_m.group(1).strip())
                else:
                    # plain text line (L3 header, dest record, etc.)
                    self._text_lines.append(stripped)
        return self.frames[before:]

    def stats(self):
        return self._stats.copy()


# ── snapshot helpers ──────────────────────────────────────────────────────────
def take_snapshot(session, label="snapshot"):
    log(f"Taking snapshot: {label}")
    snap = {"timestamp": ts(), "label": label}

    snap["L"]      = session.cmd("L",   timeout=15)
    snap["L_star"] = session.cmd("L *", timeout=15)
    snap["D"]      = session.cmd("D",   timeout=30)
    snap["U"]      = session.cmd("U",   timeout=10)
    snap["P"]      = session.cmd("P",   timeout=10)

    # parse D-table
    snap["d_table_parsed"] = parse_d_table(snap["D"])
    log(f"  D-table: {len(snap['d_table_parsed'])} entries")

    return snap


# ── main ──────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Xnet FlexNet research agent"
    )
    parser.add_argument("--config",   help="INI config file")
    parser.add_argument("--host",     help="Xnet telnet host")
    parser.add_argument("--port",     type=int, default=23)
    parser.add_argument("--user",     help="Login username")
    parser.add_argument("--password", help="Login password")
    parser.add_argument("--syspass",  help="SYS sysop password (full string)")
    parser.add_argument("--duration", type=int, default=3600,
                        help="Monitor duration in seconds (default 3600)")
    parser.add_argument("--output",   default="flexnet_capture.json",
                        help="Output JSON file")
    parser.add_argument("--snapshot-interval", type=int, default=SNAPSHOT_EVERY,
                        help="Seconds between periodic D+L snapshots")
    parser.add_argument("--monitor-cmd", default="monitor +2",
                        help="Monitor command sent to xnet (default 'monitor +2'; "
                             "use 'monitor' for all ports, or 'monitor +N' for a "
                             "specific port)")
    parser.add_argument("--debug", action="store_true",
                        help="Print raw telnet exchange for auth debugging")
    args = parser.parse_args()

    # load config file if provided
    cfg = {}
    if args.config:
        cp = configparser.ConfigParser()
        cp.read(args.config)
        if "xnet" in cp:
            cfg = dict(cp["xnet"])

    host     = args.host     or cfg.get("host")
    port     = args.port     or int(cfg.get("port", 23))
    user     = args.user     or cfg.get("user")
    password = args.password or cfg.get("password")
    syspass  = args.syspass  or cfg.get("syspass")
    duration = args.duration or int(cfg.get("duration", 3600))
    outfile  = args.output   or cfg.get("output", "flexnet_capture.json")
    snap_interval = args.snapshot_interval or int(cfg.get("snapshot_interval", SNAPSHOT_EVERY))

    if not all([host, user, password, syspass]):
        print("ERROR: host, user, password and syspass are all required.")
        print("       Use --config or individual --flags.")
        sys.exit(1)

    log(f"=== Xnet FlexNet Research Agent ===")
    log(f"Target : {host}:{port}")
    log(f"Monitor: {duration}s  ({duration//60}m)")
    log(f"Output : {outfile}")
    log(f"Snapshots every {snap_interval}s")

    capture = {
        "meta": {
            "host": host,
            "port": port,
            "start_time": ts(),
            "duration_requested": duration,
            "agent_version": "1.1",
        },
        "baseline": None,
        "snapshots": [],
        "monitor_frames": [],
        "monitor_stats": {},
        "end_time": None,
    }

    session = XnetSession(host, port)
    try:
        session.connect()
        ok = authenticate(session, user, password, syspass, debug=args.debug)
        if not ok:
            log("Authentication failed — aborting")
            sys.exit(1)

        # baseline snapshot
        capture["baseline"] = take_snapshot(session, label="baseline")

        # start monitor
        log(f"Starting {args.monitor_cmd!r} for {duration}s...")
        session.send(args.monitor_cmd)
        time.sleep(1.0)
        # read any immediate output / echo
        session._recv_raw(timeout=1.0)

        parser_m = MonitorParser()
        monitor_start    = time.time()
        last_snapshot    = monitor_start
        total_chars      = 0
        live_frame_count = 0

        while True:
            elapsed = time.time() - monitor_start
            if elapsed >= duration:
                break

            chunk = session.read_monitor_chunk(timeout=MONITOR_POLL)
            if chunk:
                total_chars += len(chunk)
                new_frames = parser_m.feed(chunk)
                live_frame_count += len(new_frames)
                for f in new_frames:
                    capture["monitor_frames"].append(f)
                    ft = f.get("frame_type", "?")
                    fr = f.get("from", "?")
                    to = f.get("to", "?")
                    if ft == "L3RTT":
                        rtt = f.get("l3rtt", {}).get("m_rtt_100ms", "?")
                        lt  = f.get("l3_header", {}) or {}
                        log(f"  L3RTT  {fr}->{to}  LT={lt.get('lt','?')}  $M={rtt}")
                    elif ft == "D_TABLE":
                        n = len(f.get("dest_records", []))
                        log(f"  D-TABLE {fr}->{to}  {n} records")
                    elif ft in ("SABM", "DISC"):
                        log(f"  {ft:6} {fr}->{to}")

            # periodic snapshot
            if time.time() - last_snapshot >= snap_interval:
                log(f"--- Periodic snapshot at {elapsed:.0f}s elapsed ---")
                # stop monitor briefly
                session.send("")          # send empty line
                time.sleep(0.3)
                snap = take_snapshot(session, label=f"t+{int(elapsed)}s")
                capture["snapshots"].append(snap)
                last_snapshot = time.time()
                # restart monitor
                session.send(args.monitor_cmd)
                time.sleep(0.5)
                session._recv_raw(timeout=0.5)

            if int(elapsed) % 60 == 0 and chunk == "":
                stats = parser_m.stats()
                log(f"  Progress: {elapsed:.0f}s/{duration}s  "
                    f"frames={stats['total_frames']}  "
                    f"chars={total_chars}")

        # stop monitor
        log("Stopping monitor...")
        session.send("")
        time.sleep(0.5)
        session.read_until(PROMPT, timeout=5)

        # final snapshot
        capture["snapshots"].append(
            take_snapshot(session, label="final")
        )

        capture["monitor_stats"] = parser_m.stats()
        capture["end_time"] = ts()

        # print summary
        stats = parser_m.stats()
        log("=== Capture complete ===")
        log(f"  Total frames : {stats['total_frames']}")
        log(f"  L3RTT        : {stats['l3rtt_frames']}")
        log(f"  D-table      : {stats['dtable_frames']}")
        log(f"  RR           : {stats['rr_frames']}")
        log(f"  SABM/UA/DISC : {stats['sabm_frames']}/{stats['ua_frames']}/{stats['disc_frames']}")
        log(f"  Other        : {stats['other_frames']}")
        log(f"  Chars read   : {total_chars}")
        log(f"  Snapshots    : {len(capture['snapshots'])}")

    except KeyboardInterrupt:
        log("Interrupted by user — saving partial capture")
        capture["end_time"] = ts()
        if "parser_m" in locals():
            capture["monitor_stats"] = parser_m.stats()
    except Exception as e:
        log(f"ERROR: {e}")
        import traceback; traceback.print_exc()
        capture["end_time"] = ts()
        capture["error"] = str(e)
        if "parser_m" in locals():
            capture["monitor_stats"] = parser_m.stats()
    finally:
        session.close()

    # save output
    log(f"Writing {outfile} ...")
    with open(outfile, "w") as f:
        json.dump(capture, f, indent=2, ensure_ascii=False)
    size = os.path.getsize(outfile)
    log(f"Saved {outfile} ({size:,} bytes)")

    # quick analysis hints
    if capture.get("monitor_frames"):
        analyse_capture(capture)


def analyse_capture(capture):
    """Print quick analysis to stdout after capture completes."""
    frames = capture["monitor_frames"]
    log("\n=== Quick analysis ===")

    # L3RTT: LT field distribution
    lt_by_direction = {}
    for f in frames:
        if f.get("frame_type") == "L3RTT":
            key = f"{f['from']}->{f['to']}"
            lt  = (f.get("l3_header") or {}).get("lt", "?")
            lt_by_direction.setdefault(key, []).append(lt)

    log("L3RTT LT field by direction:")
    for direction, lts in lt_by_direction.items():
        unique = sorted(set(lts))
        log(f"  {direction}: LT values seen = {unique}  (n={len(lts)})")

    # $M RTT distribution per link
    rtt_by_link = {}
    for f in frames:
        if f.get("frame_type") == "L3RTT":
            key = f"{f['from']}->{f['to']}"
            rtt = (f.get("l3rtt") or {}).get("m_rtt_100ms")
            if rtt is not None:
                rtt_by_link.setdefault(key, []).append(rtt)

    log("$M RTT values per link (100ms units):")
    for link, rtts in rtt_by_link.items():
        mn, mx, avg = min(rtts), max(rtts), sum(rtts)/len(rtts)
        log(f"  {link}: min={mn} max={mx} avg={avg:.1f} n={len(rtts)}")

    # D-table: destinations seen across all frames, infinity count
    all_dest = {}
    for f in frames:
        if f.get("frame_type") == "D_TABLE":
            for r in f.get("dest_records", []):
                cs = r["callsign"]
                if cs not in all_dest:
                    all_dest[cs] = r
    log(f"Unique destinations seen in D-table frames: {len(all_dest)}")
    infinity = [cs for cs, r in all_dest.items() if r.get("is_infinity")]
    log(f"  Infinity (RTT>=60000): {infinity}")

    # f3 field distribution
    f3_vals = {}
    for f in frames:
        if f.get("frame_type") == "D_TABLE":
            for r in f.get("dest_records", []):
                outer = r.get("f3_outer")
                inner = r.get("f3_inner")
                if inner is not None:
                    key = f"{outer}[{inner}]"
                    f3_vals[key] = f3_vals.get(key, 0) + 1
    if f3_vals:
        log(f"f3 field value distribution: {dict(sorted(f3_vals.items()))}")

    # polling cycle timing
    sabm_times = [f["timestamp"] for f in frames if f.get("frame_type") == "SABM"]
    if len(sabm_times) >= 2:
        from datetime import datetime
        dts = []
        for i in range(1, len(sabm_times)):
            t1 = datetime.fromisoformat(sabm_times[i-1])
            t2 = datetime.fromisoformat(sabm_times[i])
            dts.append((t2 - t1).total_seconds())
        avg_cycle = sum(dts) / len(dts)
        log(f"Polling cycle interval: avg={avg_cycle:.1f}s  "
            f"min={min(dts):.1f}s  max={max(dts):.1f}s  (n={len(dts)} cycles)")


if __name__ == "__main__":
    main()
