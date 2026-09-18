# Phase conclusion — 30-minute soak, 13:29 → 13:59

Same process at both ends (`pid 318965`), so these are real rates and not
a cumulative snapshot spanning a restart.

| Metric | Before | After | Delta | Rate |
|---|---|---|---|---|
| Peer session re-inits | 7 | 16 | **+9** | 0.30/min |
| **Adoptions** (`LEARNED-ADOPT … kept`) | 4 | 13 | **+9** | — |
| Withdrawals (`exp=60000 FIRED`) | 310 | 319 | **+9** | 0.30/min |
| Advertisements FIRED | 3079 | 7408 | +4329 | **144/min** |
| Advertisements SUPPRESSED | 42 | 375 | +333 | 7% of total |
| Probe replies / timeouts | 383 / 37 | 408 / 44 | +25 / +7 | 78% success |
| L2 extended / contracted | 37 / 44 | 78 / 75 | +41 / +31 | balanced |
| Path forwards / relays | 8 / 4 | 10 / 6 | +2 / +2 | — |

## Proven by this soak

**Adoption is 9 out of 9.** Nine peer sessions re-initialised in the
window and nine adopted their learned table. Not one rebuilt from
scratch. That is fix 1 working at every reconnect, not just the one we
happened to catch this morning.

**Withdrawals collapsed ~10x.** Nine restarts produced **nine**
withdrawals. Before the scoped-invalidation fix a single peer's DISC
invalidated every destination from every peer — ~198 each — and the
overnight run logged 2187 in twelve hours (3.0/min) against 0.30/min
here, with *more* restarts per minute.

**L2 forwarding stays balanced under load** (+41 extended, +31
contracted) and **path forwarding keeps working**: six `PATH-FWD` and
four `PATH-REP-RELAY` in the window, all with the `0x21 -> 0x22` header
delta, relaying chains of 6, 9 and 10 hops back to IW2OHX-4.

## The one defect left, now quantified

**Advertisement volume: 144/min, of which only 7% is suppressed.**
PC/Flexnet's token bucket drains one record per 5 s — **12/min** — so we
generate roughly 12x what the slowest peer can absorb, and its queue can
never empty. That is the purely *relative* 10% jitter threshold: a
high-RTT destination jitters past 10% on nearly every update and
re-advertises forever.

This is why PCF's cost to us stays an order of magnitude above its
(X)Net peers' 1/1. It is now the single remaining cause of the link
churn, and the fix has two parts, neither started:

1. an **absolute floor** alongside the percentage, so distant noisy
   destinations go quiet unless they genuinely change;
2. a **hold-down on transitions to infinity**, reusing the existing
   `FLEXNET_POISON_HOLDDOWN`, so a blip does not withdraw and restore.

## Churn that is not ours

Nine re-inits in 30 minutes is high, and the A4 alerts attribute them:
IW2OHX-4 five, IW2OHX-12 three, IW2OHX-14 one. -4 is RAM-only TNC4e
hardware that was physically reset today and has been flapping since;
-12 is PC/Flexnet, which cycles quiet AXIP peers by design. **The
difference now is that their churn no longer costs us the routing
table** — nine restarts, nine adoptions, nine withdrawals.

## State at close of phase

All three links CONNECTED (`-14` 1h43, `-4` 10m, `-12` 12m), all three
monitors collecting (`mon-14` idle at the sampling instant, `mon-4` and
`mon-12` growing), `FLEXNETPATHFORWARD`/`FLEXNETL2TRANSIT`/
`FLEXNETTRANSIT` all YES on IR2UFV and all still default NO in the code.
