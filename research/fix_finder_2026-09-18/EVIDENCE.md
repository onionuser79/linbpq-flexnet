# Fix finder — `D` / `C` evidence from all three peers

**2026-09-18, IR2UFV running v2.2.0-rc5 + the day's fixes.** Three
per-link monitors running (`/tmp/mon-14`, `/tmp/mon-4`, `/tmp/mon-12`,
one capture per peer so a frame never has to be attributed after the
fact), plus `quad-watch` and the debug console.

## The one case that works end to end

From IW2OHX-4, destination 8 hops away:

```
D DB0FFL
*** DB0FFL (0-11) T=13
*** route: IW2OHX-4 IR2UFV IW2OHX-14 IR3UHU-2 IZ3LSV-14 IR3UHF OE7XGR OE2XZR DB0FFL

C DB0FFL
link setup (4)...
*** connected to DB0FFL
```

Route renders, connect succeeds, port 4 is the IR2UFV link, and the
chain is asker-anchored with 7 digis — inside AX.25's 8. This is the
reference for "correct".

## Four distinct failure modes, only one of which is ours

**1. Chain longer than 8 digipeaters — unfixable by design.**
`CS5LX`: our probe resolved `hops=13`, i.e. 12 digis. `PATH-REQ-TOOLONG`
suppresses the answer, correctly, so `D CS5LX` shows `T=13` with no
route line. `W2KPQ` is the same at 9 digis. AX.25 cannot express these
paths; nothing to fix in the answer. Only forwarding the traversal
(below) reaches such destinations.

**2. Our own probe times out — 11% of the time.**
Current process: **261 replies, 31 timeouts, 292 probes sent** (89%
success). The two destinations tested from -12 both landed in the
failing 11%:

```
11:23:17 PATH-REQ-TX -> next=IW2OHX-14 target=DB0DLG-6
11:23:17 PATH-DEFER: parked DB0DLG for peer slot 2
11:23:33 PATH-TIMEOUT: qso=18 target=DB0DLG (16s elapsed)
```

The same destination answered `hops=9` in 0 s at 11:01, so it is
intermittent rather than structural. **This is the one that is ours**,
and the deferred answer added today only helps when the probe *does*
reply — see below.

**3. IW2OHX-4 reboots and loses everything via us.**
Caught live mid-test. -4's own table went from ~120 destinations via
IR2UFV to none, and:

```
4:IR2UFV       F   -   -/-     -   - - -    -    -    -/-    -.-    -
MH: 4:IR2UFV   18.09.26 10:16:36          (an hour stale)
```

Monitor 2 showed the cause unambiguously — every frame outbound, nothing
back:

```
11:16:46 Out 192.168.1.202.10075 > 192.168.1.203.10075: UDP, length 17
11:16:49 Out ...                                         length 17
11:16:52 Out ...                                         length 17
```

-4 is a RAM-only (X)Net on TNC4e hardware: a reboot reverts its port
config, including the AXIP map for IR2UFV. Every one of its links was
seconds-to-minutes old and its telnet was refusing logins
(`no password prompt after username`). Recovery is already automated
(`tnc4e-recover.py --auto` driven by `iw2ohx-monitor`); it came back on
its own ~15 minutes later. **Not a FlexNet defect, and not ours** — but
it invalidates any measurement taken across it, which is why the first
-4 test run showed everything failing on port 1.

**4. Destinations -4 reaches better elsewhere.** `C CS5LX` reports
`link setup (1)` — port 1, not our link — and fails on -4's own path.
Picking test destinations from `D < IR2UFV` is necessary but not
sufficient: check which port the connect actually uses before blaming
IR2UFV.

## From IW2OHX-14: nothing to test, and that is correct

```
D < IR2UFV
IR2UFV  0-8      1
```

One entry: ourselves. -14 taught us everything it knows, so after the A3
destination-level split-horizon fix we advertise nothing back to it. The
`D <` filter does work — on -4 the same command listed ~120 rows — so
this is the fix behaving as designed, not a broken table.

## From IW2OHX-12

10 destinations via IR2UFV, `T` values present, **no route lines**, both
tested destinations hit failure mode 2 (probe timeout). The chained
connect result was truncated by the test window and is not reported
either way rather than guessed.

## What to fix next, and why it is the right fix

Answering from our own path cache is a shortcut. A real router does not
cache-and-answer: **it forwards the traversal**, as captured from
PC/Flexnet this morning (`TYPE6_IS_A_TRAVERSAL.md`) — insert its next
hop before the target, bump the hop counter, pass the type-6 on, and let
the node adjacent to the target reply.

Forwarding fixes modes 1 and 2 together:

* a probe timeout stops mattering, because we are no longer the one who
  has to know the path;
* an over-long chain stops mattering for *discovery*, because no single
  node ever has to express the whole path.

The deferred answer added today (`PATH-DEFER` / `PATH-DEFER-REPLAY`) is
worth keeping either way — it closes the window where the probe replies
milliseconds after we have already answered with silence — but it is a
patch on the shortcut, not a replacement for forwarding.
