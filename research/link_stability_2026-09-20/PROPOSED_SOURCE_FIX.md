# Proposed source fix — don't sleep under the global semaphore

**Not applied.** `DisconnectOnClose=0` already removes the freeze for
this station's traffic (see [`TELNET_SLEEP_FREEZE.md`](TELNET_SLEEP_FREEZE.md)).
This is the general fix, written down so the decision is on record.

## What it changes

`TelnetPoll` has two `Sleep(1000)` calls that run on the main BPQ thread
under the global `Semaphore`. Each one stops all AX.25 processing for a
second. Release the lock around them — exactly the pattern `LinBPQ.c`
already uses around `InitializeTNCEmulator` and `AGWAPIInit`.

```diff
--- a/TelnetV6.c
+++ b/TelnetV6.c
@@ TelnetPoll()
 				if (sockptr->Signon[0] || sockptr->ClientSession)		// Outward Connect
 				{
-					Sleep(1000);
+					/* Sleeping here stalls every port: the main loop holds
+					   &Semaphore across TIMERINTERRUPT(). A peer whose retry
+					   budget is under a second drops the link. */
+					FreeSemaphore(&Semaphore);
+					Sleep(1000);
+					GetSemaphore(&Semaphore, 2);
 					DataSocket_Disconnect(TNC, sockptr);
 					return;
 				}
 
 				if (TCP->DisconnectOnClose)
 				{
-					Sleep(1000);
+					FreeSemaphore(&Semaphore);
+					Sleep(1000);
+					GetSemaphore(&Semaphore, 2);
 					DataSocket_Disconnect(TNC, sockptr);
 				}
```

`Sleep(100)` in the `RelayMode` branch just above is the same shape but
a tenth of the cost; leave it or treat it the same way.

## Why not just delete the sleep

It is presumably there to let the last bytes reach the client before the
socket closes. Releasing the lock keeps that behaviour and costs
nothing, whereas removing it risks truncating the goodbye text.

## Cost, and why it is not applied here

`TelnetV6.c` is upstream and **not currently in the overlay**. The whole
rebase conflict surface today is five files — `Cmd.c`, `L2Code.c`,
`asmstrucs.h`, `bpqaxip.c`, `makefile` — and the weekly upstream watcher
is tuned to exactly those. Adding a sixth file for a change that a
config directive already neutralises is a poor trade.

**Better path: send it to G8BPQ.** It is an upstream defect, it affects
any LinBPQ node that serves telnet and carries AX.25 links, and it is
not specific to FlexNet.

## If it is applied anyway

- `TelnetV6.c` is **CRLF with tab indentation**. Preserve both or the
  rebase diff becomes unreadable — the same trap as the v2.1.40 rebase.
- Verify `Semaphore` is declared visible in `TelnetV6.c`; it is an
  extern in the BPQ headers, but check rather than assume.
- Re-verify with `tools/sem_watch.py`: the number to see is **zero holds
  per hour** with `DisconnectOnClose=1` restored.
