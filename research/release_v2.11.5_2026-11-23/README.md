# v2.11.5 release record — 2026-11-23

- Trigger: prod IW2OHX-13 (v2.11.4) died 2026-11-23 03:12:09 UTC, found at 09:42 CET (~5.5 h down).
  `NodeDebuglog_261123.log`: SIGSEGV, `atoi` <- `InnerProcessHTTPMessage` HTTPcode.c:3810 <-
  `ProcessHTTPMessage`. No OOM, no kernel segfault line, host uptime 74 d; IR2UFV unaffected.
- Cause: `/Node/PortStats` (and `/Node/PortBeacons`) without `?n` -> `strtok_s` returns NULL ->
  `atoi(NULL)`. Upstream 6.0.26.1 (228047d) bug. HTTPPORT 8080 is reachable from LAN, 44.0/9,
  44.128/10, tun0, tunl0 (gw INPUT); no access log for the BPQ web port, so the requester is unknown.
  None of our pollers (station-dashboard, monitor, pr-digi-mcp) request these pages.
- Audit of all 13 `strtok_s(NULL, ...)` sites in HTTPcode.c: only these two dereference unchecked.
  Noted, not changed: the sign-on handlers parse `&input[9]` of the POST body without a length check
  (over-read on a body under 5 bytes, no crash observed).
- Fix: `PortChar ? atoi(PortChar) : 0`. HTTPcode.c joins the overlay (commit 344c835 = upstream
  import, LF-normalised; the fix commit is the 2-line delta). Stand-alone patch for stock LinBPQ:
  `patches/0002-http-port-null/`. `-Wall -Wextra -Wshadow` on HTTPcode.c: 335 before, 335 after.
- Builds: gw `~/linbpq-v2.11.5-prod/` (silent, 5 strings, sha 4302296a) -> IW2OHX-13;
  `~/linbpq-v2.11.5-flexdebug/` (100 strings, sha 557639a2) -> IR2UFV; Windows silent md5 105f0763 -> IW2OHX-4.
  Deploy `/tmp/deploy_v2115.sh 13|ufv`, -4 `C:\Users\iw2ohx\deploy4_v2115.ps1`. MOTD/CTEXT bumped by the scripts.
  Rollbacks `*.pre-v2.11.5-20261123-0956xx` (gw), `LinBPQ.exe.pre-v2.11.5-20261123-095702` (-4).
- Live test 09:56-09:58 CET, every node: `GET /Node/PortStats`, `/Node/PortBeacons`, `?abc`, `?99`
  -> HTTP 200 (`Invalid Port` / beacon page), same PID afterwards. All three report `FlexNet v2.11.5`.
- After restart -14 relinked -4 and IR2UFV within ~1 min and -13 at 09:58:55 (xnet's retry, not ours).
