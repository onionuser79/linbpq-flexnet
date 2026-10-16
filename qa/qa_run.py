#!/usr/bin/env python3
"""qa_run.py - one FlexNet QA campaign, on a fixed schedule.

Runs on iw2ohx-gw in the pr-digi-mcp venv (see qa_launch.sh). Every campaign
of the same profile performs the same actions at the same offsets from T0:

  preflight -> convergence gate -> T0 -> snapshots / connects / path queries
  at the profile's offsets -> wire audit -> qa_score.py

T0 is the moment the convergence gate passes, never the launch time, so a
campaign started right after a deploy measures a settled mesh, not the
re-learning phase (a post-restart window is not a test bed).

usage: qa_run.py --profile smoke|standard|soak --label LABEL --out DIR
                 [--expect-version vX.Y.Z] [--hosts-json FILE] [--force]
"""
import argparse
import asyncio
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qa_lib as L  # noqa: E402

TOOLS = os.path.expanduser("~/xnet_investigation_agent/linbpq-flexnet/tools")


def ino(path):
    """Inode of path, or None. logrotate's copy creates a new <log>.1 inode
    on every rotation, while copytruncate keeps the live log's inode."""
    try:
        return os.stat(path).st_ino
    except OSError:
        return None


def utc(t=None):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t if t is not None else time.time()))


class Campaign:
    def __init__(self, args, cfg):
        from pr_digi_mcp.config import load_nodes
        self.args, self.cfg = args, cfg
        self.prof = cfg["profiles"][args.profile]
        self.tm = cfg["timing"]
        self.nodes_yaml = load_nodes()
        self.out = args.out
        os.makedirs(self.out, exist_ok=True)
        self.t0 = None
        self.log_f = open(os.path.join(self.out, "run.log"), "a", buffering=1)

    def log(self, msg):
        self.log_f.write(f"{utc()} {msg}\n")

    def append(self, name, rec):
        with open(os.path.join(self.out, name), "a") as f:
            f.write(json.dumps(rec) + "\n")

    def nodes(self, kind):
        return [n for n, v in self.cfg["nodes"].items() if v["kind"] == kind]

    # ------------------------------------------------------------ snapshot
    async def snapshot(self, tag):
        t = time.time()
        rec = {"t": t, "utc": utc(t), "tag": tag,
               "t_rel_min": round((t - self.t0) / 60, 1) if self.t0 else None}
        calls = self.nodes("linbpq") + self.nodes("xnet")
        res = await asyncio.gather(*(L.snap_node(self.nodes_yaml, c, self.cfg["nodes"][c]["kind"],
                                                 self.tm["node_timeout_s"]) for c in calls))
        rec["nodes"] = dict(zip(calls, res))
        for c in self.nodes("flexnetd"):
            rec["nodes"][c] = L.gw_flexnetd_state(self.cfg["nodes"][c]["state_dir"])
        rec["procs"] = {c: L.gw_process(v["dir"]) for c, v in self.cfg["nodes"].items()
                        if v["kind"] == "linbpq" and v.get("host") == "gw"}
        self.append("snaps.jsonl", rec)
        bad = [c for c, v in rec["nodes"].items() if "error" in v]
        self.log(f"snapshot {tag} done in {time.time() - t:.0f}s errors={bad}")
        return rec

    # ----------------------------------------------------------- preflight
    async def preflight(self):
        pf = {"utc": utc(), "label": self.args.label, "profile": self.args.profile,
              "matrix_version": self.cfg["matrix_version"], "expect_version": self.args.expect_version}
        pf["versions"] = {}
        for c in self.nodes("linbpq"):
            r = await L.run_cmds(self.nodes_yaml, c, ["V"], False, self.tm["node_timeout_s"])
            pf["versions"][c] = {"raw": r.get("V", r.get("error", ""))[-400:],
                                 "flexnet": L.parse_version(r.get("V", ""))}
        pf["binaries"] = {c: L.gw_binary_facts(v["dir"]) for c, v in self.cfg["nodes"].items()
                          if v["kind"] == "linbpq" and v.get("host") == "gw"}
        pf["cfg"] = {c: L.gw_cfg_invariants(v["dir"]) for c, v in self.cfg["nodes"].items()
                     if v["kind"] == "linbpq" and v.get("host") == "gw"}
        _, cron, _ = L.sh("crontab -l 2>/dev/null | grep -v '^#' | grep -v '^$' || true")
        pf["gw_crontab"] = cron.splitlines()
        if self.args.hosts_json and os.path.exists(self.args.hosts_json):
            pf["hosts"] = json.load(open(self.args.hosts_json))
        if os.path.exists(self.cfg["nodes"]["IR2UFV"].get("debug_log", "")):
            pf["debug_log_offset"] = os.path.getsize(self.cfg["nodes"]["IR2UFV"]["debug_log"])
        json.dump(pf, open(os.path.join(self.out, "preflight.json"), "w"), indent=1)
        self.log(f"preflight versions={ {k: v['flexnet'] for k, v in pf['versions'].items()} }")
        return pf

    def link_state(self, snap):
        """Expected links -> (connected?, uptime_s)."""
        st = {}
        for el in self.cfg["expected_links"]:
            node = snap["nodes"].get(el["node"], {})
            key = f"{el['node']}>{el['peer']}:{el['port']}"
            if self.cfg["nodes"][el["node"]]["kind"] == "flexnetd":
                rows = [r for r in L.parse_xnet_l(node.get("linkstats", "")) if r["call"] == el["peer"]]
                st[key] = (bool(rows) and rows[0]["qt"] is not None, None)
                continue
            fl = L.parse_fl(node.get("FL", ""))
            rows = [r for r in fl["links"] if r["call"] == el["peer"] and r["port"] == el["port"]]
            st[key] = (bool(rows) and rows[0]["status"] == "CONNECTED",
                       rows[0]["uptime_s"] if rows else None)
        return st

    async def convergence_gate(self):
        t_start = time.time()
        while True:
            snap = await self.snapshot("pre")
            st = self.link_state(snap)
            down = [k for k, (ok, _) in st.items() if not ok]
            young = [k for k, (ok, up) in st.items() if ok and up is not None
                     and up < self.tm["convergence_min_uptime_s"]]
            self.log(f"gate: down={down} young={young}")
            if not down and not young:
                return {"passed": True, "waited_s": round(time.time() - t_start)}
            if time.time() - t_start > self.tm["convergence_max_wait_min"] * 60:
                return {"passed": False, "down": down, "young": young,
                        "waited_s": round(time.time() - t_start)}
            await asyncio.sleep(self.tm["convergence_poll_min"] * 60)

    # ------------------------------------------------------------ actions
    async def fl_counters(self, node):
        r = await L.run_cmds(self.nodes_yaml, node, ["FL"], True, self.tm["node_timeout_s"])
        fl = L.parse_fl(r.get("FL", ""))
        return fl["l2fwd"], fl["pathfwd"]

    async def connect_round(self, rnd):
        gap = self.tm["gap_between_tests_s"]
        for t in self.cfg["outbound"]:
            for node in t["from"]:
                rec = {"round": rnd, "id": t["id"], "node": node, "dest": t["dest"], "cmd": f"C {t['dest']}",
                       "class": t["class"], "gate": t["gate"], "known_issue": t.get("known_issue"),
                       "utc": utc()}
                rec.update(await L.connect_test(self.nodes_yaml, node, rec["cmd"],
                                                self.tm["connect_timeout_s"], self.tm["banner_wait_s"]))
                self.append("conn.jsonl", rec)
                await asyncio.sleep(gap)
        for t in self.cfg["transit"]:
            rec = {"round": rnd, "id": t["id"], "node": t["from"], "dest": t["cmd"].split()[1],
                   "cmd": t["cmd"], "via_node": t["via_node"], "expect_counter": t.get("expect_counter"),
                   "class": t["class"], "gate": t["gate"],
                   "utc": utc()}
            if t["via_node"]:
                rec["via_before"] = (await self.fl_counters(t["via_node"]))[0]
            rec.update(await L.connect_test(self.nodes_yaml, t["from"], t["cmd"],
                                            self.tm["connect_timeout_s"], self.tm["banner_wait_s"]))
            if t["via_node"]:
                rec["via_after"] = (await self.fl_counters(t["via_node"]))[0]
            self.append("conn.jsonl", rec)
            await asyncio.sleep(gap)
        self.log(f"connect round {rnd} done")

    async def pathq_round(self, rnd):
        pq = self.cfg["path_queries"]
        for node in pq["from"]:
            for dest in pq["dests"]:
                rec = {"round": rnd, "node": node, "dest": dest, "utc": utc()}
                rec.update(await L.path_query(self.nodes_yaml, node, dest, pq["wait_s"]))
                self.append("pathq.jsonl", rec)
                await asyncio.sleep(self.tm["gap_between_tests_s"])
        self.log(f"path-query round {rnd} done")

    # ---------------------------------------------------------- side jobs
    def start_stall_watch(self, seconds):
        procs = []
        th = self.cfg["thresholds"]["LS-07"]
        for c, v in self.cfg["nodes"].items():
            if v["kind"] != "linbpq" or v.get("host") != "gw":
                continue
            p = L.gw_process(v["dir"])
            if not p:
                continue
            out = os.path.join(self.out, f"stalls-{c}.log")
            cmd = ["sudo", "-n", "python3", os.path.join(TOOLS, "sem_watch.py"), str(p["pid"]),
                   f"{v['dir']}/linbpq", out, str(seconds), str(th["hold_ms"]), "100"]
            procs.append(subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                          stderr=open(out + ".err", "w")))
        return procs

    def wire_audit(self, pf, t_end):
        res = {}
        log = self.cfg["nodes"]["IR2UFV"].get("debug_log")
        off = pf.get("debug_log_offset")
        if log and off is not None and os.path.exists(log):
            size = os.path.getsize(log)
            # gw rotates the trace nightly (copytruncate + delaycompress), so
            # a window spanning one rotation is <log>.1 from T0 + all of <log>.
            # Size alone cannot tell: the new file may already exceed T0's offset.
            rot = log + ".1"
            parts = [(log, off)]
            if ino(rot) != pf.get("debug_log_rot_ino", ino(rot)):
                parts = [(rot, off), (log, 0)] if os.path.getsize(rot) >= off else None
            elif size < off:
                parts = None
            if parts is None:
                res["IR2UFV"] = {"error": "debug log shrank: reboot or >1 rotation in the window"}
            else:
                tags, nbytes = {}, 0
                for path, start in parts:
                    nbytes += os.path.getsize(path) - start
                    with open(path, "rb") as f:
                        f.seek(start)
                        for raw in f:
                            p = raw.split(b" ", 2)
                            if len(p) >= 2 and p[0][:1].isdigit():
                                tag = p[1].rstrip(b":").decode("latin-1")
                                tags[tag] = tags.get(tag, 0) + 1
                res["IR2UFV"] = {"bytes": nbytes, "tags": tags, "rotated": len(parts) > 1}
        _, out, err = L.sh(f"journalctl -t flexnetd --since @{int(self.t0)} --until @{int(t_end)} "
                           f"--no-pager -o cat 2>&1", timeout=120)
        res["IW2OHX-3"] = {"server_already_sent": out.count("server already sent"),
                           "proactive_quiet": out.count("proactive keepalive"),
                           "lines": out.count("\n"), "readable": "No journal" not in out and bool(out)}
        return res

    # ---------------------------------------------------------------- run
    async def run(self):
        meta = {"label": self.args.label, "profile": self.args.profile, "launched": utc(),
                "argv": sys.argv, "host": os.uname().nodename, "pid": os.getpid(),
                "schedule": self.prof}
        pf = await self.preflight()
        gate = await self.convergence_gate()
        meta["gate"] = gate
        if not gate["passed"] and not self.args.force:
            meta["aborted"] = "convergence gate failed"
            json.dump(meta, open(os.path.join(self.out, "meta.json"), "w"), indent=1)
            self.log("ABORT: convergence gate failed")
            return 3
        self.t0 = time.time()
        meta["t0"], meta["t0_utc"] = self.t0, utc(self.t0)
        json.dump(meta, open(os.path.join(self.out, "meta.json"), "w"), indent=1)
        pf["debug_log_offset"] = (os.path.getsize(self.cfg["nodes"]["IR2UFV"]["debug_log"])
                                  if os.path.exists(self.cfg["nodes"]["IR2UFV"].get("debug_log", ""))
                                  else None)
        pf["debug_log_rot_ino"] = ino(self.cfg["nodes"]["IR2UFV"].get("debug_log", "") + ".1")
        json.dump(pf, open(os.path.join(self.out, "preflight.json"), "w"), indent=1)
        dur = self.prof["duration_min"] * 60
        stalls = self.start_stall_watch(dur) if self.prof.get("stall_watch") else []
        self.log(f"T0 {utc(self.t0)} profile={self.args.profile} duration={dur}s")

        async def at(offset_min, coro_fn, *a):
            await asyncio.sleep(max(0.0, self.t0 + offset_min * 60 - time.time()))
            await coro_fn(*a)

        jobs = []
        every = self.prof["snapshot_every_min"]
        for k in range(self.prof["duration_min"] // every + 1):
            jobs.append(at(k * every, self.snapshot, f"S{k}"))
        for i, m in enumerate(self.prof["connect_at_min"]):
            jobs.append(at(m, self.connect_round, i))
        for i, m in enumerate(self.prof["pathq_at_min"]):
            jobs.append(at(m, self.pathq_round, i))
        await asyncio.gather(*jobs)
        t_end = time.time()
        for p in stalls:
            try:
                p.wait(timeout=120)
            except subprocess.TimeoutExpired:
                p.kill()
        wire = self.wire_audit(pf, t_end)
        json.dump(wire, open(os.path.join(self.out, "wire.json"), "w"), indent=1)
        meta["t_end"], meta["t_end_utc"] = t_end, utc(t_end)
        json.dump(meta, open(os.path.join(self.out, "meta.json"), "w"), indent=1)
        self.log("campaign complete; scoring")
        rc = subprocess.call([sys.executable, os.path.join(HERE, "qa_score.py"), self.out,
                              "--config", os.path.join(HERE, "qa_config.json")])
        self.log(f"score rc={rc}")
        open(os.path.join(self.out, "DONE"), "w").write(utc() + "\n")
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", required=True, choices=["smoke", "standard", "soak"])
    ap.add_argument("--label", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--expect-version")
    ap.add_argument("--hosts-json")
    ap.add_argument("--force", action="store_true", help="run even if the convergence gate fails")
    ap.add_argument("--config", default=os.path.join(HERE, "qa_config.json"))
    args = ap.parse_args()
    cfg = json.load(open(args.config))
    sys.exit(asyncio.run(Campaign(args, cfg).run()))


if __name__ == "__main__":
    main()
