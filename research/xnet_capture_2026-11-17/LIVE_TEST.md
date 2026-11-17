# (X)Net capture session — 2026-11-17 (v2.11 cycle, input for v2.12)

Target link: (X)Net IW2OHX-14 port 14 <-> linbpq-flexnet IR2UFV (v2.10.1),
AXUDP. Capture on iw2ohx-gw: `tcpdump -i any 'udp and net 44.134.24.0/24'`
(one file also carries -14 <-> IW2OHX-13, the second neighbour, so it works
as a dual-port capture). Full capture 19:29-19:52 UTC:
`xcap-14-20261117.pcap` (local only, gitignored: it carries third-party
traffic). `snapN.pcap` below = mid-run copies of the same file. Times UTC (node logs show local CET = UTC+1).
Commands sent with pr-digi-gw at SYS level; nothing saved on -14 (runtime
changes are lost unless saved explicitly).

Baseline 19:29: -14 `D IR2UFV` = `IR2UFV (0-8) T=1`, route `IW2OHX-14 IR2UFV`;
-13 `IR2UFV (0-8) T=2` via -14; IR2UFV 127 routes from -14, Parser unknown=0.

## Step 1 — `ro flexnet reset 14 IR2UFV` (19:30:04)

-14 replied `Link to 14:IR2UFV resetted.` On the wire (snap1.pcap):

```
19:30:06.071 IW2OHX-14>IR2UFV I55 pid=CE '0>%!\r'          <- the "reset frame"
19:30:06.093 IR2UFV>IW2OHX-14 I56 pid=CE '08%!\r'          init answer
                                         '2' + 240 spaces   (type 2)
                                         '3IR2UFV081 \r'    own record
                                         '3-\r'
19:30:06.097 IW2OHX-14>IR2UFV I66/I76/I06 '3...'           full table again
19:30:06.098 IW2OHX-14>IR2UFV '10\r', '2'+spaces           link-time exchange restarts
```

Findings:
- The FlexNet routing-reset frame is **an ordinary type-0 init**
  (`'0' MAXSSID '%' '!' CR`, MAXSSID `>` = 14) sent **inside the
  established AX.25 session**: no DISC/SABM, I-frame numbering continues.
  So "send a routing reset" = send an unsolicited init (PROTOCOL_SPEC §5.3.4
  describes this as something a node SHOULD NOT do unprompted; a sysop
  reset is the deliberate case).
- (X)Net answers our re-init with its full table and restarts the
  link-time measurement.
- linbpq-flexnet v2.10.1 already handles it: IR2UFV session log
  `peer re-INIT answered`, FlexNet uptime not restarted, 127 routes kept,
  Parser unknown=0 malformed=0. The *handle* half of v2.12 `FLRESET`
  exists already (v2.6.2 peer re-INIT fix); only *send* is new.
- -14 sent IW2OHX-13 nothing about IR2UFV during the reset.

## Step 2a — `ro flexnet del 14 IR2UFV` (19:31:13)

```
19:31:14.726 IW2OHX-14>IR2UFV I14 '0>%!\r'          init again (reset first)
19:31:14.750 IR2UFV>IW2OHX-14    '08%!\r' '2..' '3IR2UFV081 \r' '3-\r'
19:31:14.848 IW2OHX-14>IR2UFV    DISC+ (x2), DM+
19:31:14.866 IR2UFV>IW2OHX-14    UA+, DM+
19:31:25.878 IW2OHX-14>IW2OHX-13 I70 '3IR2UFV080 \r'  <- IR2UFV 0-8 cost 0
```

Findings:
- Deleting a link = reset (init) + DISC.
- Towards the other neighbour (-13) -14 sent **cost `0`**, not `60000`,
  11 s after the DISC. No `60000` record for IR2UFV at that time.
- -14 kept `IR2UFV (0-8)` in its own `D` with no route line, T rising
  (T=4 at 19:31:30, T=5 at 19:34).
- **linbpq-flexnet kept a phantom**: -13 still showed `IR2UFV T=2 route
  IW2OHX-13 IW2OHX-14 IR2UFV` minutes later. Cause: the record parser
  keeps cost 0 as 0 and `FlexNetCode.c` skips every cost-0 record as an
  (X)Net "refresh marker" (`rtt0-skips`), so this record changed nothing.

### After the delete: count-to-infinity through the wider mesh

-14 -> -13 IR2UFV 0-8 records after the cost-0 record (snap3.pcap):

```
19:31:25  0      19:33:10  10     19:35:37  19
19:32:07  6      19:34:13  14     19:35:58  22
```

-13 followed (T=22 via -14 at 19:36). -14 `D IR2UFV *` at 19:36:

```
*** IR2UFV (0-8) T=19
    IW2OHX-4    -22
    IW2OHX-1    -22
    IGATE       -24
    IGATEB      -24
    DK0WUE      -23
    IR3UHU-2     25
    IR1UAW-10  -331
    IW2OHX-13   -22
    HB9ON-15     19
    IW8PGT-15   -22
    N2MH-3       19
```

Read as: positive = cost that neighbour currently offers; negative = cost
-14 advertises to that neighbour, shown negative where split horizon
applies (that neighbour is not the next hop). Three remote neighbours
(HB9ON-15, N2MH-3, IR3UHU-2) still held the old route and offered it back,
so -14 re-learned IR2UFV through them and the cost climbs lap by lap.
The cost-0 record did not stop the loop.

This is also the reference output for v2.11 `D CALL *`.

Ghost climb continued (snap4.pcap, -14 -> -13): 38 (19:38) 64 (19:40)
94 (19:42) 106 (19:43) 154 (19:45); -14 `D IR2UFV *` 19:46 = T=174, offered
by HB9ON-15 174, IR3UHU-2 196, N2MH-3 196. No `60000` record for IR2UFV in
15 min. Watch stopped (would need ~1 h more to reach 60000 at this rate).

### Cost 0 is not a post-init refresh round

linbpq-flexnet skips every cost-0 record on the belief (inherited from
flexnetd v0.7.5 `dtable.c`) that (X)Net re-sends its table with cost 0
about 20 s after a session init. Not seen here: after the 19:30:06 re-init
-14 sent IR2UFV 9 record frames (19:30:06-19:31:05), all non-zero. The only
cost-0 record in the capture is the one sent to -13 right after losing
IR2UFV. IR2UFV's `rtt0-skips` counter stood at 132 after 3h45m of session,
consistent with single cost-0 records arriving whenever (X)Net loses a
destination elsewhere (the trace shows many single-record batches
`entries=1 skipped=1`, but `skipped` also counts unchanged costs, so the
trace cannot isolate them). No historical pcap is kept to check further.

## Step 2b — `ro flexnet add + 14 IR2UFV` (19:46:38)

```
19:46:46.171 IW2OHX-14>IR2UFV SABM+          (X)Net opens the link itself
19:46:46.192 IR2UFV>IW2OHX-14 UA+
19:46:46.204 IR2UFV  '08%!\r' '2...'
19:46:46.205 -14     '1600\r' '0>%!\r'       link time 600 first, then init
19:46:46.240 IR2UFV  '15\r' '3IR2UFV081 \r' '3-\r'
19:47:07/28 -14      full table, costs ~300 higher than before
                     (e.g. DB0GW 307; earlier records ~7-34)
```

- `ro fl list` / `L` row after link-up: `14:IR2UFV  0+F 303 600/5 ...` —
  the option character is printed in front of the protocol letter.
- `D IR2UFV *` at 19:48: `IR2UFV 2001` — `+` applies to the routes as
  received (1 + 2000), confirming PROTOCOL_SPEC §14.6's earlier note.
- Side effect: the looping ghost (IR3UHU-2 357) still undercut the
  penalised direct link (2001), so -14 kept IR2UFV on the ghost path
  and -13 followed it (T=281). A `+` link makes a count-to-infinity ghost
  live longer, because the ghost must climb past 2000 before the real
  link wins.
- The ~300 rise in advertised costs coincides with the new link's
  initial measurement (own 600, Q/T 303). Recorded only, not explained.

## Step 3 — several option characters (19:49)

After `ro flexnet del 14 IR2UFV`:

| Command | Reply |
|---|---|
| `ro flexnet add +) 14 IR2UFV` | `Port missing.` |
| `ro flexnet add + ) 14 IR2UFV` | `Port missing.` |
| `ro flexnet add )+ 14 IR2UFV` | `Port missing.` |
| `ro flexnet add ) 14 IR2UFV` (control) | `Link to 14:IR2UFV added` |

(X)Net 1.39 accepts **exactly one** option character per link.
linbpq-flexnet accepts several (`F>+` etc.) — an extension, harmless,
but it has no (X)Net equivalent.

## Restore

`ro flexnet del 14 IR2UFV`, `ro flexnet add 14 IR2UFV` (no option) —
the original state. Nothing saved on -14. Verified 19:52: `14:IR2UFV 1 F`,
-14 `D IR2UFV` T=1 direct (ghost gone), -13 T=2 via -14, IR2UFV 127 routes,
Parser 0/0.

## Fix verified live on IR2UFV (branch v2.11, flexdebug build, 21:01 CET)

`flex_rtt0_rewrite()`: a cost-0 record for a destination we hold is
rewritten to a withdrawal (failover, else infinity + poison onward); for
an unknown destination it is dropped. Deployed to IR2UFV only (backup
`linbpq.pre-v2.11-rtt0-*`); production -13 still on v2.10.1, which made
a side-by-side control possible.

Trace (CET): `21:04:16 RTT0-WITHDRAW: IR0EQ (2-2) from session 0`, single
record, 84 s into the new -14 session (not an init round).

| Node | `D IR0EQ` right after |
|---|---|
| -14 (sender) | `D IR0EQ *` empty — destination gone |
| IR2UFV (fix) | `no matching destinations` |
| -13 (v2.10.1) | `IR0EQ (2-2) T=39 route IW2OHX-13 IW2OHX-14 IR0EQ-2` — phantom |

IR2UFV `FL +`: `rtt0-skips=0 rtt0-withdrawn=1`, Parser 0/0, -14 127 routes.
WAMPES link re-added after the restart (`sudo /tcp/bin/cnet -c "flexnet
link delete/add ir2ufv"`).

Evidence for the reading of cost 0, from three sources: today's single
records after a link loss (IR2UFV, I0OJJ x2, IR0EQ); mid-session
all-zero batches in IR2UFV's trace this morning (08:31, 09:29-09:34,
11:50 CET, no INIT nearby — matches IGATE links flapping on -14); and
WAMPES, which also withdraws with cost 0 (research/wampes_2026-11-17).
The April 2026 flexnetd "zero round ~20 s after init" (flexnetd 8ce26ae)
fits the same reading (a lost subtree) and is not treated separately.

## Reference output: (X)Net `D <call> *` (for v2.11 `D CALL *`)

After step 2b (`+` link, 19:48 UTC):

```
*** IR2UFV (0-8) T=357
    IW2OHX-4   -281
    IW2OHX-1   -281
    IGATE      -282
    IGATEB     -282
    IR2UFV     2001
    DK0WUE     -282
    IR3UHU-2    357
    IR1UAW-10  -593
    IW2OHX-13  -281
    HB9ON-15   -402
    IW8PGT-15  -281
    N2MH-3     -281
```

After the restore (19:52 UTC):

```
*** IR2UFV (0-8) T=1
    IW2OHX-4     -2
    IW2OHX-1     -2
    IGATE        -3
    IGATEB       -3
    IR2UFV        1
    DK0WUE       -3
    IR3UHU-2     -2
    IR1UAW-10  -314
    IW2OHX-13    -2
    HB9ON-15     -2
    IW8PGT-15    -2
    N2MH-3       -2
```

Layout: 4 spaces, neighbour `%-9s`, value `%6d`. Positive = the
neighbour's offer as received (`+` penalty included: 1 + 2000 = 2001);
negative = what -14 last advertised to a neighbour that offers nothing.
The negative values track T plus that neighbour's link time (IR1UAW-10,
link Q/T 313: -314 at T=1; IGATE/DK0WUE, Q/T 2: -3), consistent with the
~300 rise seen in step 2b: (X)Net appears to advertise its cost plus its
link time to the receiving neighbour. Recorded, not relied on.
