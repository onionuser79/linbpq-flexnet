# The path reply that breaks the transit it is meant to enable

**2026-09-17, 21:40.** Marco reported a connect through IR2UFV failing.
The cause turned out to be the opposite of every earlier hypothesis, and
it inverts the conclusion recorded a few hours before it.

## The two attempts, side by side

Same node, same command, same destination, eighteen minutes apart.

```
21:14:37  PATH-REP-TX -> origin=IW2OHX-4 target=DB0ALG hops=11
          [IR2UFV IW2OHX-14 IR3UHU-2 IZ3LSV-14 IR3UHF OE7XGR
           OE2XZR OE3XVR CS5LX-8 DB0NU DB0ALG]
          => *** DB0ALG (0-8) T=29     (no route line)
          => link setup (4)... *** link failure with DB0ALG

21:32:16  PATH-REQ-NOANSWER: target=DB0ALG reachable via IW2OHX-14
          but no cached chain (path_len=0 age=1789673536s)
          => link setup (4)... *** connected to DB0ALG
          L2FWD IW7EAS-2->DB0ALG via IW2OHX-14 (appended, digis now 3)
```

**The attempt we answered failed. The attempt we ignored succeeded.**

## Why

An 11-callsign chain is 10 digipeaters plus the destination. **AX.25
allows 8 digipeaters.** -4 received a path it cannot express as an
address field, so it neither displayed a route nor built a connect — the
`*** route:` line was missing for exactly that reason.

Staying silent instead leaves -4 to do what (X)Net does by default: send
the frame with the two-digi chain `IW2OHX-4* IR2UFV` and let each
transit node append its own next hop. The chain never exceeds 3 entries,
and the connect completes across **ten** hops.

That is the whole point of the mechanism captured earlier in
`FLEXNET_L2_FORWARDING.md`: hop-by-hop rewriting means **no node ever
needs the full path**. Answering with one is not merely unnecessary, it
actively replaces a working mechanism with an impossible one.

## The degradation, explained

This is why transit "worked earlier and stopped later", and why the
counters froze:

1. Fresh start: path cache empty → we stay silent → transit works.
2. Our own background PATH_REQ probe runs
   (`PATH-REQ-TX -> next=IW2OHX-14 target=DB0ALG-0`) and receives
   `PATH-REP-RX hops=10`.
3. Cache now populated → the next peer request is answered with 11 hops
   → every connect to that destination fails from then on.

**Our own probe poisons our own transit.** It degrades per-destination,
as each one gets probed, which is exactly the slow partial failure that
made this hard to see: some destinations still work, and the node looks
healthy.

## The fix

Answer a PATH_REQ only with a chain the asking peer can actually carry —
at most `PORTMAXDIGIS` (and never more than AX.25's 8) digipeaters after
removing the final destination. Otherwise **stay silent**, which is
already the correct and tested behaviour.

The existing code has the right instinct for the wrong case. It refuses
to answer a *truncated* chain:

> NEVER answer with a truncated chain. [...] A wrong chain is worse than
> no chain.

That reasoning is sound and should stay. What is missing is the
symmetrical guard: **a chain that is too long is also a wrong chain.**

Two other defects found in the same log:

* `age=1789673536s` in the NOANSWER line is `now - 0` — `path_updated`
  is read before it is ever set. Harmless today because `path_len > 0`
  is checked first, but it is an uninitialised read and it makes the log
  lie.
* The comment above the `D`-command fallback claims "our background
  PATH_REQ probes do not receive replies, so the cached-path branch
  stays empty in practice". The log shows **185 `PATH_REP from
  IW2OHX-14`** and 39 from IW2OHX-12. Replies arrive in volume; the
  comment is stale and reasoning from it would send the next reader the
  wrong way.

## Fixed in v2.2.0-rc5

```c
int reply_digis = n_reply - 1;
int digi_cap = (LINK->LINKPORT && LINK->LINKPORT->PORTMAXDIGIS)
                   ? LINK->LINKPORT->PORTMAXDIGIS
                   : FLEXNET_L2_MAX_DIGIS;
if (digi_cap > FLEXNET_L2_MAX_DIGIS) digi_cap = FLEXNET_L2_MAX_DIGIS;
if (reply_digis > digi_cap) { /* PATH-REQ-TOOLONG, stay silent */ }
```

Capped on the answering port's `PORTMAXDIGIS`, bounded by the AX.25
ceiling. We cannot know the *asking* peer's limit, so the protocol
ceiling is the only defensible bound. `FLEXNET_L2_MAX_DIGIS` moved up
beside the path-cache constants so the L2 rewriter and the path answer
share one definition instead of two that can drift.

The `age` lie is fixed too, and that one is already proven — the same
log file holds both sides of it:

```
21:34:44  age=1789673684s    <- before
21:47:50  age=-1s            <- after
```

## What validation still owes

The pre-fix causation test was **stopped before it could mislead us**:
it was armed to expect a *failed* connect, so with the fix deployed it
would have reported "hypothesis WRONG" for the right reason and the
wrong conclusion. Scheduled experiments encode an expected outcome, and
changing the code underneath one invalidates it.

`/tmp/validate-fix.sh` replaces it and inverts the logic: wait for the
first `PATH-REQ-TOOLONG` (the guard firing on live traffic), then
connect from -4 to *that* destination. Two hardenings, both learned the
hard way in the first attempt:

1. **`C <target> IR2UFV`, not `C <target>`.** Left to itself -4 prefers
   PC/Flexnet: a manual retest of DB0ALG showed `link setup (3)` and
   `extended=0`, i.e. it succeeded without a single frame crossing us.
   A pass like that proves nothing.
2. **The verdict requires `extended` to have grown.** "Connected" alone
   cannot distinguish our transit from somebody else's path, so a
   connect that does not move the counter is reported INCONCLUSIVE
   rather than as a pass.

It cannot fire until some cached chain actually exceeds 8 digis and a
peer asks about it. A 4h timeout reports the hop-count histogram
instead, so a quiet result is still readable.

Standing evidence meanwhile: two failures with an 11-hop answer, one
success with silence, and the arithmetic (10 digis > 8).

---

# Part 2 — one symptom, several causes

**Same evening, 22:00–22:30.** The path-length guard was necessary and
not sufficient. Chasing the still-missing `route:` line turned up two
more defects and one behaviour nobody has explained yet.

## The 21:14 failure had TWO causes, not one

Counting `L2FWD-DECLINE ... no live session for it (via_session_idx=-1)`
by minute:

```
  1  17:55      <- hours before any of tonight's changes
 23  21:14      <- Marco's failing DB0ALG connect
 32  21:15      <- same
  5  22:16
```

So during that failure the node was *both* answering an uncarryable
11-hop path *and* refusing to forward the frames. Fixing only the
visible cause would have left the other in place — and the 21:32 success
that made the path fix look complete happened simply because the table
had been batch-refreshed by then.

A destination restored from cache, or not yet refreshed by a CE compact
batch, carries `via_session_idx = -1` while `via_callsign` is perfectly
good. `flex_session_for_call()` now resolves the index from the callsign
so it heals on first use. **`L2FWD-HEAL` has not been observed firing
yet** — the window is short, and connects attempted inside it are noisy
for other reasons (see below).

## Replies must be anchored on the asker

Every working type-7 on the wire starts its chain with the node that
asked. Frames replying to probes *we* originated:

```
'7' 'C' "   51" "IR2UFV IW2OHX-14 IW2OHX-12 IQ2LB-6"
'7' '$' "   41" "IR2UFV IW2OHX-14 HB9ON-15 HB9ON-10"
'7' '"' "   45" "IR2UFV IW2OHX-14 IGATE"
```

IR2UFV first — us, the originator. PC/Flexnet's answers render on -4 the
same way, asker first: `IW2OHX-4 IW2OHX-12 IW2OHX-14 HB9ON-15
VE3MCH-8 VA3BAL-8`. We were starting at our own callsign, so -4 received
a chain that did not begin with itself, could not anchor it, and bounced
a type-7 straight back at us:

```
22:07:44  PATH-REP-TX -> origin=IW2OHX-4 target=IR3UHU-1 hops=4
                         [IR2UFV IW2OHX-14 IR3UHU-2 IR3UHU-1]
22:07:44  PATH-REP-DROP: unsolicited qso=0 origin=IR2UFV hops=3
```

`hops=3` is our own chain with our callsign stripped. The target was
three digis deep, well inside the limit, so length was not this failure.

Now sent as `[asker, us, ...cached chain...]`, and the digi count
excludes both endpoints (`n_reply - 2`). **The route line still does not
appear**, so this is at best necessary-but-not-sufficient, and it is
deployed unvalidated.

## Unexplained: `D` on (X)Net sends a type-7, not a type-6

The thing that should be chased first tomorrow. Typing `D IR3UHU-1` on
-4 produced no inbound type-6 at all. Instead:

```
22:15:12  PATH-REP-DROP: unsolicited qso=2 origin=IW2OHX-4 hops=4
22:15:28  PATH-REP-DROP: unsolicited qso=2 origin=IW2OHX-4 hops=4
```

-4 sent us a **type-7 carrying its own partial chain**, origin itself.
Our handler matches inbound type-7 against our pending-probe table by
QSO id, finds no match — our own qso=2 was a different, timed-out probe
— and drops it.

If (X)Net's `D` works by sending a partial type-7 outward for each hop
to **extend and return**, then answering its type-6 is beside the point
and the whole responder path is modelled wrongly. That would also
explain why a correctly-anchored, correctly-sized reply still renders
nothing: we are answering a question -4 is not asking.

What settles it: a capture of `D <dest>` on -4 for a destination reached
via **PC/Flexnet**, where the route line demonstrably does appear. The
frames -12 exchanges in that case are the specification.

## Two testing traps hit while chasing this

* **Post-restart windows are not a test bed.** At 75s after a restart a
  pinned `C DB0FAA IR2UFV` failed with `extended=1 contracted=0` while
  the table was still converging; at full convergence the identical
  command connected with `extended=9 contracted=9 declined=0`. Wait for
  convergence before concluding anything.
* **An unpinned connect can succeed via PC/Flexnet and prove nothing.**
  Even pinned, watch which port -4 reports: `link setup (3)` is the -12
  link, `link setup (4)` is ours. Reaching us *through* -12 is normal
  (`IW2OHX-4* IW2OHX-12* IR2UFV`), so the port alone is not the test —
  the `extended`/`contracted` delta is.
