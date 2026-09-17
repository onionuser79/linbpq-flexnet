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

## Confirming it

`/tmp/causation-test.sh` is armed on gw: it waits for the probe to
repopulate DB0ALG's cache after the 21:27 restart, then immediately
reruns the identical connect from -4. Same command, opposite cache
state, one variable. Verdict lands in `/tmp/causation-test.log`.

Until that returns, the mechanism is strongly evidenced (two failures
with an answer, one success without, plus the arithmetic) but not yet
proven by a controlled flip.
