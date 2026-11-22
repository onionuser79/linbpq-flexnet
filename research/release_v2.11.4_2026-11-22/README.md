# v2.11.3 / v2.11.4 release record — 2026-11-22

- Trigger: RHP-bridge follow-up on IW2OHX-4. (1) a FlexNet caller to an application call
  (`C IW2OHX-8`) was left hanging after BBS `B`; (2) `FL +` did not list the range APPLICATION
  calls -5/-7/-8; (3) Marco asked for an (X)Net-style `LOcals` command, then for the node command
  on every row (`ext pt n`).
- (1) was two defects: rhp-bridge 1.0.2 matched `73 de <caller>` but BPQMail signs off with its
  own call (fixed in bridge 1.0.3, iw2ohx-bpq-ops); and the alias `S` handed direct callers the
  -4 prompt (v2.11.3: Stay dropped for direct APPLCALL connects).
- Live test 2026-11-22 09:43, from IW2OHX-1 (TNC4e): `C IW2OHX-8` → banner, `B` →
  `73 de IW2OHX` → `*** reconnected to IW2OHX-1`.
- v2.11.3 ddd5bc6 live 09:34-09:36, v2.11.4 f76db60 live 09:50-09:51 on IR2UFV (flexdebug),
  IW2OHX-13 and IW2OHX-4 (silent). No QA campaign: Marco went straight to the tag.
- Tag v2.11.4 + GitHub release (v2.11.3 not tagged, included in its notes).
- Announced: BPQMail bulletin `linbpq-flexnet v2.11.4 released`, BPQ.WW #1749 and AX25.WW #1750
  (NNTP POST, from IW2OHX), text in `BULLETIN.txt`.
