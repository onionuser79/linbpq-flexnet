#!/usr/bin/env python3
"""
linkstab.py — long-run link-stability watch for a linbpq-flexnet node.

Answers the two questions a `quad-watch` sample cannot, because both
need a fine time base:

  1. **How stable is each FlexNet peer session, and who pays for it?**
     `FL` is polled once a minute, so a session restart is located to
     the minute and the per-peer advertisement counters give a *rate*
     rather than a cumulative total. Advertisement rate is the metric
     that matters: a peer whose token bucket drains slower than we
     queue can never converge, and the measured symptom is its link
     cycling.

  2. **Is the destination table actually usable?** Every round a rotating
     sample of destinations from `D *` is connected to for real, and the
     outcome is recorded next to the link state at that moment. "The
     table says reachable" and "a connect succeeds" are different
     claims, and the whole point of this watch is that they have been
     disagreeing.

Everything is read-only apart from the connect probes, which connect and
immediately drop. Nothing is written to node config or routing.

Credentials come from the environment so this file stays publishable:
    LS_USER_UFV / LS_PW_UFV      LinBPQ telnet login on the node under test

Output (all under --out):
    fl.jsonl          one object per `FL` poll
    connects.jsonl    one object per connect probe
    events.log        session restarts, connect failures, rate alerts
    watch.log         human-readable running transcript

Usage:
    linkstab.py --out /tmp/linkstab --hours 24
    linkstab.py --out /tmp/linkstab --fl-interval 60 --connect-interval 1800
"""

import argparse
import json
import os
import re
import socket
import sys
import time
import traceback
from datetime import datetime, timezone

RECV_BUF = 8192

# `FL` link row:  IW2OHX-14  2  CONNECTED  0.0s  6  00:16:51  107
FL_LINK = re.compile(
    r"^(?P<call>[A-Z0-9]+(?:-\d+)?)\s+(?P<port>\d+)\s+(?P<status>[A-Z]+)\s+"
    r"(?P<lt>[\d.]+)s\s+(?P<ka>\d+)\s+(?P<up>\d+:\d\d:\d\d)\s+(?P<routes>\d+)"
)
# `FL` peer row:  IW2OHX-14  xnet  109  1  3  0  2.00
FL_PEER = re.compile(
    r"^(?P<call>[A-Z0-9]+(?:-\d+)?)\s+(?P<family>xnet|PCF|BPQ)\s+"
    r"(?P<learned>\d+)\s+(?P<direct>\d+)\s+(?P<advert>\d+)\s+"
    r"(?P<queued>\d+)\s+(?P<tokens>[\d.]+)"
)
FL_L2 = re.compile(
    r"L2 forwarding (?P<state>ON|OFF).*?extended=(?P<ext>\d+)\s+"
    r"contracted=(?P<con>\d+)\s+declined=(?P<dec>\d+)"
)
# `D *` packs three entries per line, each "CALL lo-hi rtt[!]", with the
# SSID range in its own column rather than attached to the callsign:
#   VA3BAL 7-7      37!     VA3BAL 8-8       6!     VA3BAL 9-9      37!
# So this is scanned across the whole reply, never line-anchored.
DEST_ENTRY = re.compile(
    r"\b(?P<call>[A-Z][A-Z0-9]{2,5})\s+(?P<lo>\d{1,2})-(?P<hi>\d{1,2})\s+"
    r"(?P<rtt>\d+)(?P<mark>!?)"
)

# Debug-log line, when the node is a `flexdebug` build:
#   15:17:59 FlexNet: ADVERT-CHECK peer=IW2OHX-4 dest=DB0NU-0/0 exp=22
#            last=-1 delta=22 FIRED
LOG_ADVERT = re.compile(
    r"ADVERT-CHECK peer=(?P<peer>\S+) dest=(?P<dest>\S+) exp=(?P<exp>-?\d+) "
    r"last=(?P<last>-?\d+) delta=(?P<delta>-?\d+) (?P<verdict>FIRED|SUPPRESSED)"
)
LOG_TAGS = re.compile(r"FlexNet: (?P<tag>[A-Z0-9][A-Z0-9-]+)")
RTT_INFINITY = 60000

CONNECT_OK = re.compile(r"connected to\s+(?P<call>\S+)", re.IGNORECASE)
CONNECT_BAD = re.compile(
    r"(no route|failure with|busy from|invalid|unable|rejected|downlink denied)",
    re.IGNORECASE,
)


def now():
    return datetime.now(timezone.utc)


def iso(dt=None):
    return (dt or now()).strftime("%Y-%m-%dT%H:%M:%SZ")


def uptime_seconds(text):
    """'01:46:16' -> 6376. Returns None for anything unparseable."""
    try:
        h, m, s = (int(p) for p in text.split(":"))
    except (ValueError, AttributeError):
        return None
    return h * 3600 + m * 60 + s


def strip_iac(data):
    """Drop telnet IAC negotiation triplets."""
    out = bytearray()
    i = 0
    while i < len(data):
        if data[i] == 0xFF and i + 2 < len(data):
            i += 3
            continue
        out.append(data[i])
        i += 1
    return bytes(out)


class NodeSession:
    """A short-lived LinBPQ telnet session.

    Deliberately not kept open between polls: a wedged socket on a node
    that is itself cycling links is the classic way a watch goes quiet
    without dying, and this watch has to survive 24 hours unattended.
    """

    def __init__(self, host, port, user, password, connect_timeout=20):
        self.host, self.port = host, port
        self.user, self.password = user, password
        self.connect_timeout = connect_timeout
        self.sock = None

    def __enter__(self):
        self.sock = socket.create_connection((self.host, self.port),
                                             timeout=self.connect_timeout)
        self.sock.settimeout(5.0)
        self.read(silence=1.0, max_wait=6.0)          # banner
        self.send(self.user)
        self.read(silence=0.8, max_wait=5.0)
        self.send(self.password)
        self.read(silence=1.0, max_wait=8.0)
        return self

    def __exit__(self, *exc):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        return False

    def send(self, line):
        self.sock.sendall((line + "\r").encode("latin-1", errors="replace"))

    def read(self, silence=1.5, max_wait=20.0):
        """Read until the node has been quiet for `silence` seconds."""
        end = time.time() + max_wait
        buf = b""
        last = time.time()
        self.sock.settimeout(0.3)
        while time.time() < end:
            try:
                chunk = self.sock.recv(RECV_BUF)
            except socket.timeout:
                if buf and time.time() - last > silence:
                    break
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            last = time.time()
        return strip_iac(buf).decode("latin-1", errors="replace").replace("\r", "")

    def command(self, cmd, silence=1.5, max_wait=20.0):
        self.send(cmd)
        return self.read(silence=silence, max_wait=max_wait)


def parse_fl(text):
    links, peers, l2 = {}, {}, None
    for raw in text.splitlines():
        line = raw.strip()
        m = FL_LINK.match(line)
        if m and m.group("status") in ("CONNECTED", "PENDING", "DISCONNECTED",
                                       "SETUP", "CLOSING"):
            links[m.group("call")] = {
                "port": int(m.group("port")),
                "status": m.group("status"),
                "link_time": float(m.group("lt")),
                "keepalive": int(m.group("ka")),
                "uptime": m.group("up"),
                "uptime_s": uptime_seconds(m.group("up")),
                "routes": int(m.group("routes")),
            }
            continue
        m = FL_PEER.match(line)
        if m:
            peers[m.group("call")] = {
                "family": m.group("family"),
                "learned": int(m.group("learned")),
                "direct": int(m.group("direct")),
                "advert": int(m.group("advert")),
                "queued": int(m.group("queued")),
                "tokens": float(m.group("tokens")),
            }
            continue
        m = FL_L2.search(line)
        if m:
            l2 = {"state": m.group("state"), "extended": int(m.group("ext")),
                  "contracted": int(m.group("con")),
                  "declined": int(m.group("dec"))}
    return {"links": links, "peers": peers, "l2": l2}


def parse_dests(text):
    """Parse a `D *` reply into connectable destinations.

    A destination is advertised as a callsign plus an SSID *range*; the
    low end is the one to connect to, and a 0-0 range means the bare
    callsign. Entries are de-duplicated by the connect target, because
    several SSID ranges of one callsign are one node to probe.
    """
    out, seen = [], set()
    for m in DEST_ENTRY.finditer(text):
        lo, hi = int(m.group("lo")), int(m.group("hi"))
        call = m.group("call")
        target = call if lo == 0 else f"{call}-{lo}"
        if target in seen:
            continue
        seen.add(target)
        out.append({"call": target, "base": call, "ssids": f"{lo}-{hi}",
                    "rtt": int(m.group("rtt")),
                    "path_cached": m.group("mark") == "!"})
    return out


class DebugLogTail:
    """Incremental reader for the `flexdebug` wire log.

    `FL`'s Advert column is the number of destinations currently
    advertised to a peer, not how many advertisements went out — so it
    cannot measure volume, which is the quantity under investigation.
    The log can, but only if it is read as a stream: re-grepping a file
    that grows past 100 MB in a day would dominate the poll cycle.

    A file that shrank was rotated or the node restarted; the offset
    resets and the interval is reported as a gap rather than as a
    negative count.
    """

    def __init__(self, path):
        self.path = path
        self.offset = None

    def prime(self):
        try:
            self.offset = os.path.getsize(self.path)
        except OSError:
            self.offset = None

    def read_new(self, max_bytes=40 * 1024 * 1024):
        """Return (counters, rotated). Counters are per-interval."""
        counters = {
            "fired": 0, "suppressed": 0, "withdrawals": 0, "restorations": 0,
            "per_peer_fired": {}, "per_peer_suppressed": {}, "tags": {},
            "bytes": 0,
        }
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return counters, False
        if self.offset is None:
            self.offset = size
            return counters, False
        rotated = size < self.offset
        if rotated:
            self.offset = 0
        if size == self.offset:
            return counters, rotated
        try:
            with open(self.path, "rb") as fh:
                fh.seek(self.offset)
                chunk = fh.read(min(size - self.offset, max_bytes))
        except OSError:
            return counters, rotated
        self.offset += len(chunk)
        counters["bytes"] = len(chunk)
        text = chunk.decode("latin-1", errors="replace")

        for m in LOG_TAGS.finditer(text):
            tag = m.group("tag")
            counters["tags"][tag] = counters["tags"].get(tag, 0) + 1
        for m in LOG_ADVERT.finditer(text):
            peer, verdict = m.group("peer"), m.group("verdict")
            if verdict == "FIRED":
                counters["fired"] += 1
                counters["per_peer_fired"][peer] = \
                    counters["per_peer_fired"].get(peer, 0) + 1
                if int(m.group("exp")) >= RTT_INFINITY:
                    counters["withdrawals"] += 1
                elif int(m.group("last")) >= RTT_INFINITY:
                    counters["restorations"] += 1
            else:
                counters["suppressed"] += 1
                counters["per_peer_suppressed"][peer] = \
                    counters["per_peer_suppressed"].get(peer, 0) + 1
        return counters, rotated


class Watch:
    def __init__(self, args):
        self.args = args
        os.makedirs(args.out, exist_ok=True)
        self.fl_fh = open(os.path.join(args.out, "fl.jsonl"), "a", buffering=1)
        self.cx_fh = open(os.path.join(args.out, "connects.jsonl"), "a",
                          buffering=1)
        self.ev_fh = open(os.path.join(args.out, "events.log"), "a",
                          buffering=1)
        self.log_fh = open(os.path.join(args.out, "watch.log"), "a",
                           buffering=1)
        self.prev_links = {}
        self.prev_peers = {}
        self.prev_sample_at = None
        self.dest_cursor = 0
        self.dest_pool = []
        self.restarts = {}
        self.n_fl = 0
        self.dbg = DebugLogTail(args.debug_log) if args.debug_log else None
        if self.dbg:
            self.dbg.prime()

    # ---------------------------------------------------------- logging
    def log(self, msg):
        line = f"{iso()} {msg}"
        print(line, flush=True)
        self.log_fh.write(line + "\n")

    def event(self, kind, msg):
        line = f"{iso()} {kind} {msg}"
        self.ev_fh.write(line + "\n")
        self.log(f"  ! {kind} {msg}")

    # ------------------------------------------------------------- polls
    def session(self):
        return NodeSession(self.args.host, self.args.port,
                           self.args.user, self.args.password)

    def poll_fl(self):
        with self.session() as s:
            text = s.command("FL", silence=1.5, max_wait=25.0)
        sample = parse_fl(text)
        if not sample["links"] and not sample["peers"]:
            self.event("POLL_EMPTY", "FL returned nothing parseable "
                                     "(NOT recorded as data)")
            return None

        at = now()
        elapsed = ((at - self.prev_sample_at).total_seconds()
                   if self.prev_sample_at else None)
        rec = {"ts": iso(at), "kind": "fl", **sample}

        # Advertisement rate per peer. A cumulative counter that went
        # backwards means the process restarted, not that we un-sent
        # anything — report it as a restart marker and drop the delta.
        rates = {}
        for call, peer in sample["peers"].items():
            prev = self.prev_peers.get(call)
            if prev and elapsed and elapsed > 0:
                delta = peer["advert"] - prev["advert"]
                if delta < 0:
                    self.event("COUNTER_RESET",
                               f"{call}: advert {prev['advert']} -> "
                               f"{peer['advert']} (process or session restart)")
                else:
                    rates[call] = round(delta * 60.0 / elapsed, 1)
        rec["advert_routes_per_min"] = rates

        # True advertisement volume, from the wire log if we have one.
        fire_rate = None
        if self.dbg:
            counters, rotated = self.dbg.read_new()
            if rotated:
                self.event("LOG_ROTATED",
                           f"{self.args.debug_log} shrank; interval dropped")
            rec["log"] = counters
            if elapsed and elapsed > 0 and counters["bytes"]:
                per_min = 60.0 / elapsed
                fire_rate = round(counters["fired"] * per_min, 1)
                rec["fired_per_min"] = fire_rate
                rec["suppressed_per_min"] = round(
                    counters["suppressed"] * per_min, 1)
                rec["fired_per_min_by_peer"] = {
                    p: round(n * per_min, 1)
                    for p, n in counters["per_peer_fired"].items()
                }
                # PC/Flexnet drains one record per 5 s. Anything above
                # that for a PCF peer is backlog we are creating.
                for peer, n in counters["per_peer_fired"].items():
                    rate = n * per_min
                    if (sample["peers"].get(peer, {}).get("family") == "PCF"
                            and rate > self.args.pcf_drain_per_min):
                        self.event("PCF_OVERRUN",
                                   f"{peer}: {rate:.0f} adverts/min against a "
                                   f"{self.args.pcf_drain_per_min}/min drain")
        rec["fired_per_min_total"] = fire_rate

        # Session restarts, located to the poll interval.
        for call, link in sample["links"].items():
            prev = self.prev_links.get(call)
            cur_up = link["uptime_s"]
            if prev and prev["uptime_s"] is not None and cur_up is not None:
                if cur_up < prev["uptime_s"]:
                    self.restarts[call] = self.restarts.get(call, 0) + 1
                    self.event("SESSION_RESTART",
                               f"{call}: uptime {prev['uptime']} -> "
                               f"{link['uptime']} "
                               f"(total {self.restarts[call]} this run)")
            if prev and prev["status"] != link["status"]:
                self.event("LINK_STATE",
                           f"{call}: {prev['status']} -> {link['status']}")

        # A peer whose queue never empties cannot converge; that is the
        # condition the advertisement-volume work is trying to clear.
        for call, peer in sample["peers"].items():
            if peer["queued"] > self.args.queue_alert:
                self.event("QUEUE_DEEP",
                           f"{call}: {peer['queued']} records queued, "
                           f"tokens={peer['tokens']}")

        self.fl_fh.write(json.dumps(rec) + "\n")
        self.prev_links = sample["links"]
        self.prev_peers = sample["peers"]
        self.prev_sample_at = at
        self.n_fl += 1

        if self.n_fl % self.args.summary_every == 1:
            parts = []
            for call in sorted(sample["links"]):
                link = sample["links"][call]
                peer = sample["peers"].get(call, {})
                fired = (rec.get("fired_per_min_by_peer") or {}).get(call, "-")
                parts.append(f"{call} {link['status'][:4]} up={link['uptime']} "
                             f"adv={peer.get('advert', '-')} "
                             f"fired/min={fired} q={peer.get('queued', '-')}")
            self.log("FL  " + " | ".join(parts))
        return sample

    def refresh_dest_pool(self):
        with self.session() as s:
            text = s.command("D *", silence=2.5, max_wait=40.0)
        dests = parse_dests(text)
        if dests:
            self.dest_pool = dests
            self.log(f"dest pool refreshed: {len(dests)} destinations, "
                     f"{sum(1 for d in dests if d['path_cached'])} with a "
                     f"cached path")
        else:
            self.event("DEST_EMPTY", "D * returned no parseable rows")
        return dests

    def probe_connect(self, dest, link_state):
        """Connect to one destination and drop straight away.

        The socket is closed rather than the session being walked back
        out: from inside a connected session the node CLI is no longer
        listening, and closing the telnet socket makes LinBPQ tear the
        AX.25 session down properly.
        """
        started = now()
        outcome, detail = "TIMEOUT", ""
        try:
            with self.session() as s:
                text = s.command(f"C {dest}", silence=2.0,
                                 max_wait=self.args.connect_timeout)
                detail = " ".join(text.split())[-240:]
                if CONNECT_OK.search(text):
                    outcome = "CONNECTED"
                elif CONNECT_BAD.search(text):
                    outcome = "REFUSED"
                elif text.strip():
                    outcome = "NO_ANSWER"
        except (OSError, socket.timeout) as exc:
            outcome, detail = "ERROR", str(exc)
        took = (now() - started).total_seconds()

        rec = {"ts": iso(started), "kind": "connect", "dest": dest,
               "outcome": outcome, "seconds": round(took, 1),
               "detail": detail, "links": link_state}
        self.cx_fh.write(json.dumps(rec) + "\n")
        return outcome, took

    def connect_round(self, link_state):
        if not self.dest_pool:
            self.refresh_dest_pool()
        if not self.dest_pool:
            return
        # Stride across the pool instead of taking six adjacent entries.
        # `D *` comes out grouped by callsign prefix, so consecutive picks
        # are all the same region reached the same way — six Canadian
        # nodes behind one gateway is one measurement, not six.
        n = len(self.dest_pool)
        stride = max(1, n // max(1, self.args.connect_batch))
        picks = []
        for k in range(min(self.args.connect_batch, n)):
            picks.append(self.dest_pool[(self.dest_cursor + k * stride) % n])
        self.dest_cursor = (self.dest_cursor + 1) % n

        results = []
        for dest in picks:
            call = dest["call"]
            outcome, took = self.probe_connect(call, link_state)
            results.append(f"{call}={outcome}({took:.0f}s)"
                           f"{'!' if dest['path_cached'] else ''}")
            if outcome != "CONNECTED":
                self.event("CONNECT_FAIL",
                           f"{call} rtt={dest['rtt']} "
                           f"path_cached={dest['path_cached']} -> {outcome}")
            time.sleep(self.args.connect_gap)
        self.log("CONNECT  " + "  ".join(results))

    # -------------------------------------------------------------- main
    def run(self):
        deadline = time.time() + self.args.hours * 3600
        next_connect = time.time() + self.args.connect_delay
        next_pool = 0.0
        self.log(f"linkstab starting: host={self.args.host}:{self.args.port} "
                 f"hours={self.args.hours} fl-interval={self.args.fl_interval}s "
                 f"connect-interval={self.args.connect_interval}s "
                 f"batch={self.args.connect_batch}")
        while time.time() < deadline:
            cycle_started = time.time()
            link_state = None
            try:
                sample = self.poll_fl()
                if sample:
                    link_state = {c: {"status": v["status"],
                                      "uptime": v["uptime"]}
                                  for c, v in sample["links"].items()}
            except Exception:                      # never die on one poll
                self.event("POLL_ERROR",
                           traceback.format_exc(limit=2).replace("\n", " | "))

            if time.time() >= next_pool:
                try:
                    self.refresh_dest_pool()
                    next_pool = time.time() + self.args.pool_interval
                except Exception:
                    self.event("POOL_ERROR",
                               traceback.format_exc(limit=2).replace("\n", " | "))
                    next_pool = time.time() + 300

            if time.time() >= next_connect:
                try:
                    self.connect_round(link_state)
                except Exception:
                    self.event("CONNECT_ERROR",
                               traceback.format_exc(limit=2).replace("\n", " | "))
                next_connect = time.time() + self.args.connect_interval

            sleep_for = self.args.fl_interval - (time.time() - cycle_started)
            if sleep_for > 0:
                time.sleep(sleep_for)
        self.log(f"linkstab finished after {self.args.hours}h: "
                 f"{self.n_fl} FL polls, restarts={self.restarts}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2525)
    ap.add_argument("--out", default="/tmp/linkstab")
    ap.add_argument("--hours", type=float, default=24.0)
    ap.add_argument("--fl-interval", type=int, default=60)
    ap.add_argument("--connect-interval", type=int, default=1800)
    ap.add_argument("--connect-delay", type=int, default=120,
                    help="wait this long before the first connect round")
    ap.add_argument("--connect-batch", type=int, default=6)
    ap.add_argument("--connect-gap", type=int, default=10,
                    help="seconds between connect probes in a round")
    ap.add_argument("--connect-timeout", type=int, default=60)
    ap.add_argument("--pool-interval", type=int, default=3600)
    ap.add_argument("--queue-alert", type=int, default=100)
    ap.add_argument("--summary-every", type=int, default=5)
    ap.add_argument("--debug-log", default="/tmp/flexnet_axudp.log",
                    help="flexdebug wire log to tail for advertisement "
                         "volume; empty string disables")
    ap.add_argument("--pcf-drain-per-min", type=float, default=12.0,
                    help="PC/Flexnet token-bucket drain rate, one per 5 s")
    args = ap.parse_args()

    args.user = os.environ.get("LS_USER_UFV")
    args.password = os.environ.get("LS_PW_UFV")
    if not args.user or not args.password:
        print("set LS_USER_UFV and LS_PW_UFV in the environment",
              file=sys.stderr)
        return 2
    Watch(args).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
