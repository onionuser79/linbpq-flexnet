# B-1 reply mark — live test of v2.11.2-rc1 on IR2UFV (2026-11-21)

Bug: QA `health-smoke-20261121T1717Z` B-1. Replies built from a received
frame (UA to DISC, DM, FRMR via `L2SWAPADDRESSES`) dropped `IR2UFV*` on a
node user's FlexNet session, because `FlexNet_MarkLocalDigi()` only marked
FLEXNETLOCAL / own-range sources.

Fix: `FlexNet_MarkLocalDigi()` marks MYCALL whenever it is the first digi of
a frame we send and the source is not MYCALL itself.

Deploy: flexdebug build, IR2UFV only, 22:12 local (PID 229271, tty3). Backup
`/home/bpq-ufv/linbpq.v2.11.1.bak-20261121-221235`. WAMPES link deleted/re-added
after the restart. Production -13 and -4 untouched (v2.11.1).

## Before (v2.11.1, QA smoke, 18:41:32 local)

```
PE1FAM -> IW7EAS IW2OHX-14* IR2UFV ctl=DISC
IW7EAS -> PE1FAM IR2UFV IW2OHX-14 ctl=UA        <- IR2UFV unmarked, not relayed
... PE1FAM DISC retries, L2FWD-DECLINE ... dest not in our table x10 (WD-02 FAIL)
```

## After (v2.11.2-rc1, same OB-NL connect via the QA harness `connect_test`, 22:17 local)

```
22:17:07 AXUDP-TX: IW7EAS -> PE1FAM IR2UFV* IW2OHX-14 ctl=SABM(0x3F) len=31
22:17:07 AXUDP-RX: PE1FAM -> IW7EAS IW2OHX-14* IR2UFV ctl=UA(0x73) len=29
22:17:08 AXUDP-RX: PE1FAM -> IW7EAS IW2OHX-14* IR2UFV ctl=DISC(0x53) len=29
22:17:08 AXUDP-TX: IW7EAS -> PE1FAM IR2UFV* IW2OHX-14 ctl=UA(0x73) len=31
```

Connect OK (banner 250 bytes). One DISC, one UA with `IR2UFV*`, no retries;
`L2FWD-DECLINE` lines since deploy: 0.

Unit: `test_local_calls` 71/71 (new `test_user_reply_via_mycall` + changed
cases); mutations "old IsLocalCall condition" (3 FAIL) and "no MYCALL-source
guard" (1 FAIL) both caught; all 26 runners pass. FlexNetCode.c
`-Wall -Wextra -Wshadow`: 51 warnings (baseline 51).

Next: `smoke` on the rc (`v2.11.2-rc1`) — WD-02 must be 0.
