#!/usr/bin/env python3
"""qa_score.py - turn one campaign directory into a scorecard.

Pure stdlib: runs on gw at the end of a campaign and again on macmini.

usage: qa_score.py RUN_DIR [--config qa_config.json] [--baseline OLD/scorecard.json]
                   [--history history.csv]
writes RUN_DIR/scorecard.json and RUN_DIR/SCORECARD.md
"""
import argparse
import csv
import json
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qa_lib as L  # noqa: E402

RANK = {"PASS": 0, "INFO": 0, "SKIP": 0, "WARN": 1, "FAIL": 2}

# check id -> where to look first. The playbook's triage table, condensed.
TRIAGE = {
    "PF": "deploy/config: wrong binary, flavour or cfg invariant. Fix before reading anything else.",
    "LS-01": "link down: peer reachable? keeper (FLEXNETLINK) on LinBPQ<->LinBPQ links; L2 state in peer's L.",
    "LS-02": "session lifecycle: FlexNet_InitSession / reaper in FlexNet_Timer / re-INIT answer; "
             "first rule out peer restarts, pollers and LS-07 stalls. research/link_stability_2026-09-19/.",
    "LS-03": "reaper fired: L2STATE >= 5 rule and grace (QA v2.6.0 B-5); FL Sessions history names the reason.",
    "LS-04": "link quality as the peer sees it: ack latency / stalls (tools/poll_latency.py, ack_latency.py on a capture).",
    "LS-05": "advertisement drain: flex_advertise_check, token bucket, records per frame "
             "(research/link_stability_2026-09-19/PACKED_ADVERTISEMENTS.md).",
    "LS-06": "KISS keeper re-opened the link: flex_port_links_keep; check RF and SABM collision (ROADMAP).",
    "LS-07": "whole-node stall: run tools/sem_stack.py to name the callee "
             "(research/link_stability_2026-09-20/TELNET_SLEEP_FREEZE.md).",
    "RC-01": "missing destinations: learned[] / dest re-election (flex_dest_reelect), split horizon, scope gate.",
    "RC-02": "orphan rows only we hold: stale dest after source withdrew (QA v2.6.0 B-2) or a phantom loop.",
    "RC-03": "FL Routes vs D disagree: via_session_idx vs via_callsign bookkeeping (B-2).",
    "RC-04": "costs inflated: our link cost in flex_dest_cost_here, '+' options, climb guard.",
    "RC-05": "SSID range mismatch: neighbour entry <link SSID>-<INIT max> (B-3), flex_call_in_range.",
    "RC-06": "routes vanish and return: withdrawals / prunes / hold-down (B-1); slice the trace for POISON.",
    "RC-07": "count-to-infinity climb: flex_climb_is_loop floor must persist "
             "(research/link_stability_2026-09-20/DESTINATION_EXCHANGE_CLIMB.md).",
    "RC-08": "route to ourselves: never offer a neighbour its own callsign (flex_expected_rtt).",
    "RC-09": "the reference's cost for us drifts: our link-time / keepalive cadence (spec §6.2-§6.3).",
    "RE-01": "connect failed: read the route: line and the reference D first (four non-bug causes), "
             "then Cmd.c CMDC00 FlexNet branch / flex_find_best_dest.",
    "RE-02": "slow connects: interlink setup after a neighbour restart is normal once; repeated = forwarding.",
    "RE-03": "transit did not move 'extended' or declined: L2 forwarding (flex_l2_*), DIGIFLAG=1, pinned via.",
    "RE-04": "no route line: >8 digis, probe timeout, peer prefers another port, stale path cache.",
    "FD": "flexnetd: ce_proto.c session handling (re-INIT answer, keepalive); journal -t flexnetd.",
    "WD": "flexdebug trace: CE-UNKNOWN = parser gap, L2FWD-DECLINE = forwarding refused, "
          "LEARNED-AGE = removed prune came back.",
}


def lower(v, p, w):
    return "PASS" if v <= p else "WARN" if v <= w else "FAIL"


def higher(v, p, w):
    return "PASS" if v >= p else "WARN" if v >= w else "FAIL"


def median(xs):
    return statistics.median(xs) if xs else None


def p90(xs):
    if not xs:
        return None
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(0.9 * (len(xs) - 1))))]


def load_jsonl(path):
    if not os.path.exists(path):
        return []
    return [json.loads(l) for l in open(path) if l.strip()]


class Scorer:
    def __init__(self, run, cfg):
        self.run, self.cfg, self.th = run, cfg, cfg["thresholds"]
        self.meta = json.load(open(os.path.join(run, "meta.json")))
        self.pf = json.load(open(os.path.join(run, "preflight.json")))
        self.snaps = [s for s in load_jsonl(os.path.join(run, "snaps.jsonl"))
                      if s.get("tag", "S").startswith("S")]
        self.conn = load_jsonl(os.path.join(run, "conn.jsonl"))
        self.pathq = load_jsonl(os.path.join(run, "pathq.jsonl"))
        wp = os.path.join(run, "wire.json")
        self.wire = json.load(open(wp)) if os.path.exists(wp) else {}
        self.checks = []
        self.ref = cfg["reference_node"]
        self.kind = {n: v["kind"] for n, v in cfg["nodes"].items()}
        self.ours = [n for n, k in self.kind.items() if k == "linbpq"]
        dur = (self.meta.get("t_end") or (self.snaps[-1]["t"] if self.snaps else 0)) - (self.meta.get("t0") or 0)
        self.dur_s = max(dur, 1)
        self.norm6h = 21600 / self.dur_s
        self.tables = [self._tables(s) for s in self.snaps]

    def add(self, cid, scope, value, status, detail="", gate=True):
        self.checks.append({"id": cid, "scope": scope, "value": value, "status": status,
                            "detail": detail, "gate": gate})

    def _tables(self, snap):
        t = {}
        for n, v in snap["nodes"].items():
            if self.kind.get(n) == "flexnetd":
                t[n] = L.parse_flexnetd_dests(v.get("destinations", ""))
            elif "D" in v:
                t[n] = L.parse_dests(v["D"])
        return t

    def fl(self, snap, node):
        return L.parse_fl(snap["nodes"].get(node, {}).get("FL", ""))

    # ------------------------------------------------------------ preflight
    def preflight(self):
        exp = self.pf.get("expect_version")
        vers = {n: v.get("flexnet") for n, v in self.pf.get("versions", {}).items()}
        if exp:
            bad = {n: v for n, v in vers.items() if v != exp}
            self.add("PF-01", "all", vers, "FAIL" if bad else "PASS", f"expected {exp}; mismatched {bad}")
        else:
            self.add("PF-01", "all", vers, "PASS" if len(set(vers.values())) == 1 else "WARN",
                     "no --expect-version given; nodes compared with each other")
        for n, b in self.pf.get("binaries", {}).items():
            fl = self.cfg["nodes"][n]["flavour"]
            cnt = b.get("flexnet_strings")
            ok = cnt is not None and ((cnt <= 5) if fl == "silent" else (cnt > 5))
            self.add("PF-02", n, cnt, "PASS" if ok else "FAIL",
                     f"flavour {fl}: 'FlexNet: ' strings {cnt} (silent <= 5); sha256 {b.get('sha256', '')[:12]}")
        cfgs = dict(self.pf.get("cfg", {}))
        cfgs.update(self.pf.get("hosts", {}).get("cfg", {}))
        for n, c in cfgs.items():
            if not c.get("readable", True):
                self.add("PF-03", n, None, "WARN", "cfg not readable")
                continue
            if not c.get("DisconnectOnClose=0"):
                self.add("PF-03", n, "DisconnectOnClose", "FAIL",
                         "telnet close freezes the node 1 s: every poll is a link reset risk")
            else:
                self.add("PF-03", n, "DisconnectOnClose=0", "PASS")
            self.add("PF-03", n, "OnlyVer2point0", "PASS" if c.get("OnlyVer2point0=1") else "WARN",
                     "" if c.get("OnlyVer2point0=1") else "XID at crosslink setup costs one teardown per peer per restart")
        pollers = [l for l in self.pf.get("gw_crontab", []) if re.search(r"dashboard|xnet|linkstab|fl-sampler", l)]
        self.add("PF-04", "gw", len(pollers), "INFO",
                 "pollers active during the campaign (declare them with every number): "
                 + "; ".join(p.split(" cd ")[0] + " " + (p.split("cd ")[1].split(" ")[0] if "cd " in p else "")
                             for p in pollers), gate=False)
        g = self.meta.get("gate", {})
        self.add("PF-05", "all", g, "PASS" if g.get("passed") else "FAIL",
                 f"waited {g.get('waited_s')} s for links >= {self.cfg['timing']['convergence_min_uptime_s']} s up")

    # -------------------------------------------------------- link stability
    def link_rows(self, snap, el):
        fl = self.fl(snap, el["node"])
        return [r for r in fl["links"] if r["call"] == el["peer"] and r["port"] == el["port"]]

    def restarted(self, node, i):
        """Did NODE's process restart between sample i-1 and i? (gw nodes by pid,
        others by every FlexNet link uptime resetting together)."""
        a, b = self.snaps[i - 1], self.snaps[i]
        pa, pb = (a.get("procs") or {}).get(node), (b.get("procs") or {}).get(node)
        if pa and pb:
            return pa["pid"] != pb["pid"]
        if self.kind.get(node) == "linbpq":
            la, lb = self.fl(a, node)["links"], self.fl(b, node)["links"]
            dt = b["t"] - a["t"]
            return bool(lb) and bool(la) and all(r["uptime_s"] < dt for r in lb)
        return False

    def link_stability(self):
        th = self.th
        for el in self.cfg["expected_links"]:
            scope = f"{el['node']}>{el['peer']}:{el['port']}"
            if self.kind[el["node"]] == "flexnetd":
                miss = sum(1 for s in self.snaps
                           if not [r for r in L.parse_xnet_l(s["nodes"].get(el["node"], {}).get("linkstats", ""))
                                   if r["call"] == el["peer"]])
                self.add("LS-01", scope, miss, lower(miss, 0, 1), f"{miss}/{len(self.snaps)} samples without the link")
                continue
            miss = sum(1 for s in self.snaps
                       if not any(r["status"] == "CONNECTED" for r in self.link_rows(s, el)))
            self.add("LS-01", scope, miss, lower(miss, 0, 1), f"{miss}/{len(self.snaps)} samples not CONNECTED")
            # A link that vanishes from FL and returns is ONE restart; a link whose
            # uptime goes backwards is another. linkstab counted only the latter (1 vs 7).
            events, explained = [], []
            present = bool(self.link_rows(self.snaps[0], el))
            for i in range(1, len(self.snaps)):
                ra, rb = self.link_rows(self.snaps[i - 1], el), self.link_rows(self.snaps[i], el)
                dt = self.snaps[i]["t"] - self.snaps[i - 1]["t"]
                ev = None
                if not rb:
                    if present:
                        ev = "vanished"
                    present = False
                elif not present:
                    present = True
                elif ra and rb[0]["uptime_s"] + 120 < ra[0]["uptime_s"] + dt:
                    ev = "uptime reset"
                if ev:
                    why = [n for n in (el["node"], el["peer"]) if self.restarted(n, i)]
                    (explained if why else events).append(f"{self.snaps[i]['utc']} {ev}"
                                                          + (f" (restart of {why})" if why else ""))
            n6 = round(len(events) * self.norm6h, 2)
            p, w = th["LS-02"][el["class"]]
            self.add("LS-02", scope, n6, lower(n6, p, w),
                     f"{len(events)} unexplained in {self.dur_s / 3600:.1f} h (per 6 h: {n6}); "
                     f"explained: {len(explained)}; " + "; ".join(events + explained)[:400])
        for n in self.ours:
            ss = [self.fl(s, n)["sessions"] for s in self.snaps]
            ss = [x for x in ss if x]
            if len(ss) < 2:
                self.add("LS-03", n, None, "SKIP", "no Sessions: line (pre-v2.6.1 build?)")
                continue
            d = ss[-1]["reaped"] - ss[0]["reaped"]
            note = " (counter went backwards: process restart inside the window)" if d < 0 else ""
            self.add("LS-03", n, d, lower(max(d, 0), th["LS-03"]["pass"], th["LS-03"]["warn"]),
                     f"reaped {ss[0]['reaped']} -> {ss[-1]['reaped']}{note}")
        for v in self.cfg["xnet_views"]:
            per_port = {}
            for s in self.snaps:
                for r in L.parse_xnet_l(s["nodes"].get(v["observer"], {}).get("L", "")):
                    if r["call"] == v["of"] and r["flag"] == "F":
                        qr = per_port.setdefault(r["port"], ([], []))
                        if r["qt"] is not None:
                            qr[0].append(r["qt"])
                        if r["rr"] is not None:
                            qr[1].append(r["rr"])
            gate = v.get("gate", True)
            if not per_port:
                self.add("LS-04", f"{v['observer']} sees {v['of']}", None, "FAIL",
                         "no F row for us in the observer's L table", gate=gate)
                continue
            t = self.th["LS-04"][v["class"] if v["class"] in self.th["LS-04"] else "axudp"]
            for port, (qts, rrs) in sorted(per_port.items()):
                if not qts:
                    continue
                q90, rmax = p90(qts), max(rrs) if rrs else 0.0
                st = max(lower(q90, *t["qt"]), lower(rmax, *t["rr"]), key=RANK.get)
                self.add("LS-04", f"{v['observer']} sees {v['of']} (port {port})", {"qt_p90": q90, "rr_max": rmax},
                         st, f"Q/T p90 {q90} (max {max(qts)}), rr+ max {rmax}%", gate=gate)
        for n in self.ours:
            per = {}
            for s in self.snaps:
                for i, p in enumerate(self.fl(s, n)["peers"]):
                    per.setdefault(f"{p['call']}#{i}", []).append(p["queued"])
            for k, qs in per.items():
                share = sum(1 for q in qs if q == 0) / len(qs)
                self.add("LS-05", f"{n}>{k}", round(share, 2),
                         higher(share, self.th["LS-05"]["pass"], self.th["LS-05"]["warn"]),
                         f"Queued==0 in {share:.0%} of samples, max queued {max(qs)}")
            first, last = self.fl(self.snaps[0], n)["kiss"], self.fl(self.snaps[-1], n)["kiss"]
            for a in first:
                b = next((x for x in last if x["call"] == a["call"] and x["port"] == a["port"]), None)
                if b:
                    d = b["opens"] - a["opens"]
                    self.add("LS-06", f"{n}>{a['call']}:{a['port']}", d,
                             lower(max(d, 0), self.th["LS-06"]["pass"], self.th["LS-06"]["warn"]),
                             f"keeper opens {a['opens']} -> {b['opens']}")
        th7 = self.th["LS-07"]
        for n, v in self.cfg["nodes"].items():
            if v["kind"] != "linbpq" or v.get("host") != "gw":
                continue
            path = os.path.join(self.run, f"stalls-{n}.log")
            if not os.path.exists(path) or "sem_watch start" not in open(path).read():
                err = open(path + ".err").read().strip() if os.path.exists(path + ".err") else "not run"
                self.add("LS-07", n, None, "SKIP", f"stall watch unavailable: {err[:160]}", gate=False)
                continue
            holds = [float(m) for m in re.findall(r"HOLD\s+([\d.]+) ms", open(path).read())]
            big = [h for h in holds if h >= th7["fail_ms"]]
            st = "FAIL" if big else lower(len(holds), th7["pass"], th7["warn"])
            self.add("LS-07", n, len(holds), st,
                     f"{len(holds)} holds >= {th7['hold_ms']} ms, {len(big)} >= {th7['fail_ms']} ms, "
                     f"max {max(holds) if holds else 0:.0f} ms")

    # ------------------------------------------------------ routing coherency
    def own_key(self, node, key):
        """Rows a node legitimately does not hold or holds differently: its own
        callsign, the reference's own callsign ((X)Net never advertises itself to
        a neighbour) and the node's local/external calls."""
        calls = [node, self.ref] + self.cfg["nodes"].get(node, {}).get("local_calls", [])
        for c in calls:
            b, s = L.base_ssid(c)
            if key[0] == b and key[1] <= s <= key[2]:
                return True
        return False

    def routing(self):
        th = self.th
        cov, orph, ssid, ratio, outl = {}, {}, {}, {}, {}
        for i, tabs in enumerate(self.tables):
            ref = tabs.get(self.ref, {})
            if not ref:
                continue
            for n in self.ours + [x for x, k in self.kind.items() if k == "flexnetd"]:
                t = tabs.get(n)
                if t is None:
                    continue
                want = [k for k in ref if not self.own_key(n, k)]
                cov.setdefault(n, []).append(sum(1 for k in want if k in t) / max(len(want), 1))
                if self.kind[n] != "linbpq":
                    continue
                others = set().union(*[set(v) for m, v in tabs.items() if m != n])
                orph.setdefault(n, []).append(sorted(f"{k[0]} {k[1]}-{k[2]}" for k in t
                                                     if k not in others and not self.own_key(n, k)))
                rb, nb = {}, {}
                for k in ref:
                    if not self.own_key(n, k):
                        rb.setdefault(k[0], set()).add(k[1:])
                for k in t:
                    if not self.own_key(n, k):
                        nb.setdefault(k[0], set()).add(k[1:])
                ssid.setdefault(n, []).append(sorted(b for b in set(rb) & set(nb) if rb[b] != nb[b]))
                rs, out = [], []
                for k, c in t.items():
                    if k in ref and ref[k] > 0:
                        rs.append(c / ref[k])
                        if c / ref[k] > 3 and c > 20:
                            out.append(f"{k[0]} {k[1]}-{k[2]} {c} vs {ref[k]}")
                ratio.setdefault(n, []).append(median(rs) or 0)
                outl.setdefault(n, []).append(out)
        for n, xs in cov.items():
            cid = "FD-01" if self.kind[n] == "flexnetd" else "RC-01"
            v = round(median(xs), 4)
            self.add(cid, n, v, higher(v, th[cid]["pass"], th[cid]["warn"]),
                     f"median coverage of {self.ref} {v:.1%}, worst sample {min(xs):.1%}")
        for n, xs in orph.items():
            v = median([len(x) for x in xs])
            self.add("RC-02", n, v, lower(v, th["RC-02"]["pass"], th["RC-02"]["warn"]),
                     "last sample: " + ", ".join(xs[-1][:12]))
        for n, xs in ssid.items():
            v = median([len(x) for x in xs])
            self.add("RC-05", n, v, lower(v, th["RC-05"]["pass"], th["RC-05"]["warn"]),
                     "last sample: " + ", ".join(xs[-1][:12]))
        for n, xs in ratio.items():
            v, o = round(median(xs), 2), median([len(x) for x in outl[n]])
            st = max(lower(v, *th["RC-04"]["median"]), lower(o, *th["RC-04"]["outliers"]), key=RANK.get)
            self.add("RC-04", n, {"median_ratio": v, "outliers": o}, st,
                     f"median cost ratio {v}, outliers {o}: " + "; ".join(outl[n][-1][:6]))
        for n in self.ours:
            diffs = []
            for s, tabs in zip(self.snaps, self.tables):
                fl = self.fl(s, n)
                if n in tabs and fl["links"]:
                    diffs.append(abs(sum(r["routes"] for r in fl["links"]) - len(tabs[n])) / max(len(tabs[n]), 1))
            if diffs:
                v = round(median(diffs), 4)
                self.add("RC-03", n, v, lower(v, th["RC-03"]["pass"], th["RC-03"]["warn"]),
                         f"median |sum(FL Routes) - D| / D = {v:.1%}")
            seq = [tabs.get(n) for tabs in self.tables]
            churn = []
            for i in range(1, len(seq) - 1):
                if None in (seq[i - 1], seq[i], seq[i + 1]):
                    continue
                for k in seq[i - 1]:
                    if k not in seq[i] and k in seq[i + 1]:
                        churn.append(f"{self.snaps[i]['utc']} {k[0]} {k[1]}-{k[2]}")
            v = round(len(churn) * self.norm6h, 2)
            self.add("RC-06", n, v, lower(v, th["RC-06"]["pass"], th["RC-06"]["warn"]),
                     f"{len(churn)} vanish-and-return: " + "; ".join(churn[:8]))
            climbs = []
            keys = set().union(*[set(t) for t in seq if t])
            for k in keys:
                cs = [t[k] for t in seq if t and k in t]
                run = best = 0
                for a, b in zip(cs, cs[1:]):
                    run = run + 1 if b > a else 0
                    best = max(best, run)
                if best >= 3 and cs and cs[-1] >= 4 * min(cs):
                    climbs.append(f"{k[0]} {k[1]}-{k[2]} {min(cs)}->{cs[-1]}")
            self.add("RC-07", n, len(climbs), lower(len(climbs), th["RC-07"]["pass"], th["RC-07"]["warn"]),
                     "; ".join(climbs[:8]))
            selfr = sorted({f"{k[0]} {k[1]}-{k[2]} {t[k]}" for t in seq if t for k in t
                            if self.own_key(n, k) and t[k] > 1})
            self.add("RC-08", n, len(selfr), "PASS" if not selfr else "FAIL", "; ".join(selfr[:6]))
        for n in self.ours + [x for x, k in self.kind.items() if k == "flexnetd"]:
            b, s_ = L.base_ssid(n)
            cs = [c for tabs in self.tables for k, c in tabs.get(self.ref, {}).items()
                  if k[0] == b and k[1] <= s_ <= k[2]]
            if cs:
                d = max(cs) - min(cs)
                self.add("RC-09", n, d, lower(d, th["RC-09"]["pass"], th["RC-09"]["warn"]),
                         f"{self.ref} cost for {n}: min {min(cs)} max {max(cs)}")
            else:
                self.add("RC-09", n, None, "WARN", f"{self.ref} has no row covering {n}")

    # ---------------------------------------------------------- reachability
    def nearest_ref(self, utc_s):
        best = self.tables[0].get(self.ref, {}) if self.tables else {}
        for s, tabs in zip(self.snaps, self.tables):
            if s["utc"] <= utc_s:
                best = tabs.get(self.ref, {})
        return best

    def reachability(self):
        th = self.th
        gated, lat, transit = [], {}, []
        for c in self.conn:
            ref = self.nearest_ref(c["utc"])
            c["skip"] = None
            if "error" in c:
                c["skip"] = "harness error: " + c["error"][:80]
            elif c["dest"] != self.ref and not L.covers(ref, c["dest"]) and c.get("via_node") is None \
                    and c["id"].startswith("OB-"):
                c["skip"] = f"{c['dest']} not in {self.ref} D (environment)"
            if c.get("known_issue"):
                self.add("RE-KNOWN", f"{c['node']}>{c['dest']}", c.get("ok"), "INFO",
                         f"{c['known_issue']}: {'NOW PASSES' if c.get('ok') else 'still fails'}", gate=False)
                continue
            if c["skip"] or not c["gate"]:
                continue
            gated.append(c)
            if c.get("ok"):
                lat.setdefault(c["class"], []).append(c["secs"])
            # "Connected" is not evidence that the frames went through the via node;
            # the counter is (transit-test validity rule 2). Multi-hop transit moves
            # 'extended'; an adjacent target on another port moves 'xport'.
            if c.get("via_node") and c.get("via_before") and c.get("via_after") and c.get("expect_counter"):
                k = c["expect_counter"]
                dx = c["via_after"].get(k, 0) - c["via_before"].get(k, 0)
                dd = c["via_after"]["declined"] - c["via_before"]["declined"]
                transit.append((c, c.get("ok") and dx > 0 and dd == 0, f"{k}+{dx}", dd))
        if gated:
            ok = sum(1 for c in gated if c.get("ok"))
            v = round(ok / len(gated), 3)
            fails = [f"{c['node']}>{c['cmd']} ({c['secs']}s: {c['text'][-70:]})" for c in gated if not c.get("ok")]
            self.add("RE-01", "all", v, higher(v, th["RE-01"]["pass"], th["RE-01"]["warn"]),
                     f"{ok}/{len(gated)} first-try; failed: " + " || ".join(fails)[:700])
            for n in sorted({c["node"] for c in gated}):
                mine = [c for c in gated if c["node"] == n]
                k = sum(1 for c in mine if c.get("ok"))
                self.add("RE-01", n, round(k / len(mine), 3), "INFO", f"{k}/{len(mine)}", gate=False)
        skipped = [f"{c['node']}>{c['dest']}: {c['skip']}" for c in self.conn if c.get("skip")]
        if skipped:
            self.add("RE-SKIP", "all", len(skipped), "INFO", "; ".join(skipped)[:500], gate=False)
        for cls, xs in lat.items():
            v = p90(xs)
            self.add("RE-02", cls, v, lower(v, *th["RE-02"].get(cls, th["RE-02"]["axudp"])),
                     f"p90 {v}s, max {max(xs)}s over {len(xs)} connects")
        if transit:
            v = round(sum(1 for t in transit if t[1]) / len(transit), 3)
            det = [f"{t[0]['id']} r{t[0]['round']}: ok={t[0].get('ok')} {t[2]} declined+{t[3]}" for t in transit]
            self.add("RE-03", "all", v, higher(v, th["RE-03"]["pass"], th["RE-03"]["warn"]), "; ".join(det))
        if self.pathq:
            good, bad = 0, []
            for q in self.pathq:
                r = q.get("route")
                if r and L.base_ssid(r[-1])[0] == L.base_ssid(q["dest"])[0] and len(r) - 2 <= 8:
                    good += 1
                else:
                    bad.append(f"{q['node']}>{q['dest']}: {('route ' + ' '.join(r)) if r else 'no route line'}")
            v = round(good / len(self.pathq), 3)
            self.add("RE-04", "all", v, higher(v, th["RE-04"]["pass"], th["RE-04"]["warn"]),
                     f"{good}/{len(self.pathq)} valid; " + "; ".join(bad[:8]))

    # --------------------------------------------------------- wire / flexnetd
    def wire_checks(self):
        w = self.wire.get("IR2UFV")
        if w and "tags" in w:
            t = w["tags"]
            tx, rx = t.get("PATH-REQ-TX", 0), t.get("PATH-REP-RX", 0)
            if tx:
                v = round(rx / tx, 3)
                self.add("WD-01", "IR2UFV", v, higher(v, self.th["WD-01"]["pass"], self.th["WD-01"]["warn"]),
                         f"{rx}/{tx} path probes answered")
            badn = {k: t.get(k, 0) for k in ("CE-UNKNOWN", "L2FWD-DECLINE", "LEARNED-AGE")}
            s = sum(badn.values())
            self.add("WD-02", "IR2UFV", s, lower(s, self.th["WD-02"]["pass"], self.th["WD-02"]["warn"]), str(badn))
            info = {k: t.get(k, 0) for k in ("REINIT", "RESEED", "POISON", "HOLDDOWN", "CLIMB-WITHDRAW",
                                             "CLIMB-SUPPRESS", "L2-SABM-NEW", "PCF-QUIESCE", "PATH-TIMEOUT")}
            self.add("WD-INFO", "IR2UFV", info, "INFO", f"{w['bytes']} log bytes in window", gate=False)
        elif w:
            self.add("WD-01", "IR2UFV", None, "SKIP", w.get("error", ""), gate=False)
        f = self.wire.get("IW2OHX-3")
        if f and f.get("readable"):
            v = f["server_already_sent"]
            self.add("FD-02", "IW2OHX-3", v, lower(v, self.th["FD-02"]["pass"], self.th["FD-02"]["warn"]),
                     f"{v} unanswered re-INIT lines, {f['proactive_quiet']} proactive keepalives (peer quiet 300+ s)")
        elif f:
            self.add("FD-02", "IW2OHX-3", None, "SKIP", "flexnetd journal not readable", gate=False)

    # ---------------------------------------------------------------- output
    def score(self):
        self.preflight()
        if len(self.snaps) >= 2:
            self.link_stability()
            self.routing()
        else:
            self.add("DATA", "all", len(self.snaps), "FAIL", "fewer than 2 post-T0 snapshots")
        self.reachability()
        self.wire_checks()
        worst = max((RANK[c["status"]] for c in self.checks if c["gate"]), default=0)
        verdict = ["GO", "CONDITIONAL", "NO-GO"][worst]
        counts = {s: sum(1 for c in self.checks if c["status"] == s) for s in RANK}
        return {"label": self.meta.get("label"), "profile": self.meta.get("profile"),
                "t0_utc": self.meta.get("t0_utc"), "t_end_utc": self.meta.get("t_end_utc"),
                "hours": round(self.dur_s / 3600, 2), "samples": len(self.snaps),
                "matrix_version": self.pf.get("matrix_version"), "verdict": verdict,
                "counts": counts, "checks": self.checks}


def regressions(card, base):
    old = {(c["id"], c["scope"]): c for c in base["checks"]}
    out = []
    for c in card["checks"]:
        o = old.get((c["id"], c["scope"]))
        if o and RANK[c["status"]] > RANK[o["status"]]:
            out.append({"id": c["id"], "scope": c["scope"], "was": o["status"], "now": c["status"],
                        "was_value": o["value"], "now_value": c["value"]})
    return out


def render(card, regs, base_label):
    L_ = [f"# QA scorecard — {card['label']} ({card['profile']})", "",
          f"**Verdict: {card['verdict']}** · T0 {card['t0_utc']} · {card['hours']} h · "
          f"{card['samples']} snapshots · matrix {card['matrix_version']}", "",
          "| PASS | WARN | FAIL | SKIP | INFO |", "|---|---|---|---|---|",
          "| " + " | ".join(str(card["counts"][k]) for k in ("PASS", "WARN", "FAIL", "SKIP", "INFO")) + " |", ""]
    if base_label is not None:
        L_ += [f"## Regressions vs {base_label}", ""]
        L_ += [f"- **{r['id']} {r['scope']}**: {r['was']} → {r['now']} ({r['was_value']} → {r['now_value']})"
               for r in regs] or ["None."]
        L_.append("")
    L_ += ["## Findings to triage (WARN/FAIL)", ""]
    n = 0
    for c in card["checks"]:
        if c["status"] in ("WARN", "FAIL"):
            n += 1
            hint = TRIAGE.get(c["id"]) or TRIAGE.get(c["id"][:2], "")
            L_ += [f"### F-{n:02d} {c['status']} {c['id']} — {c['scope']}", "",
                   f"- value: `{c['value']}`", f"- evidence: {c['detail']}", f"- look first: {hint}",
                   "- owner (ours / peer / environment / harness): _triage_", ""]
    if not n:
        L_ += ["None.", ""]
    L_ += ["## All checks", "", "| ID | Scope | Status | Value | Detail |", "|---|---|---|---|---|"]
    for c in card["checks"]:
        d = str(c["detail"]).replace("|", "/")[:160]
        L_.append(f"| {c['id']} | {c['scope']} | {c['status']}{'' if c['gate'] else ' ·'} | "
                  f"`{c['value']}` | {d} |")
    L_ += ["", "`·` = informational, not part of the verdict."]
    return "\n".join(L_) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--config", default=os.path.join(HERE, "qa_config.json"))
    ap.add_argument("--baseline")
    ap.add_argument("--history")
    a = ap.parse_args()
    cfg = json.load(open(a.config))
    card = Scorer(a.run, cfg).score()
    regs, base_label = [], None
    if a.baseline:
        base = json.load(open(a.baseline))
        regs, base_label = regressions(card, base), base.get("label")
        card["regressions"] = regs
        card["baseline"] = base_label
    json.dump(card, open(os.path.join(a.run, "scorecard.json"), "w"), indent=1)
    open(os.path.join(a.run, "SCORECARD.md"), "w").write(render(card, regs, base_label))
    if a.history:
        new = not os.path.exists(a.history)
        get = lambda cid, scope="all": next((c["value"] for c in card["checks"]
                                             if c["id"] == cid and c["scope"] == scope), "")
        with open(a.history, "a", newline="") as f:
            w = csv.writer(f)
            if new:
                w.writerow(["label", "profile", "t0_utc", "hours", "verdict", "pass", "warn", "fail",
                            "connect_ok", "transit_ok", "pathq_ok", "restarts_per6h_total", "regressions"])
            rs = sum(c["value"] for c in card["checks"] if c["id"] == "LS-02" and isinstance(c["value"], (int, float)))
            w.writerow([card["label"], card["profile"], card["t0_utc"], card["hours"], card["verdict"],
                        card["counts"]["PASS"], card["counts"]["WARN"], card["counts"]["FAIL"],
                        get("RE-01"), get("RE-03"), get("RE-04"), round(rs, 2), len(regs)])
    print(f"{card['verdict']}  PASS={card['counts']['PASS']} WARN={card['counts']['WARN']} "
          f"FAIL={card['counts']['FAIL']}  -> {os.path.join(a.run, 'SCORECARD.md')}")
    return {"GO": 0, "CONDITIONAL": 1, "NO-GO": 2}[card["verdict"]]


if __name__ == "__main__":
    sys.exit(main())
