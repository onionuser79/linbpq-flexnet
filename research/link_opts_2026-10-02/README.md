# v2.4.0 per-link routing options — IR2UFV field test, 2026-10-02

IR2UFV peers: (X)Net `IW2OHX-14` (sends ~185 destinations) and PC/Flexnet
`IW2OHX-12` (sends only itself). Each option set = one restart with the
MAP suffix changed (`ufv24.sh opts`), a capture of `udp port 10075`, and
a sysop `FL` 230 s later (`runall.sh`). `cerecs.py` lists the compact CE
records in a capture; only records at/after the restart time count — the
first seconds hold the previous process's queue.

| Run | -14 | -12 | Expected | Observed |
|---|---|---|---|---|
| 0 | `F` | `F` | v2.3.1 behaviour | Advert -14=1, -12=185; no options section |
| 1 | `F+` | `F>)` | penalty in D and on the wire; -12 not advertised; -12 hidden from non-sysop | `D`: `IW2OHX (14-14) T=2001`; 120 records to -12 all ≥2000 + own; to -14 own only; AX.25 user via -14 sees no -12 row |
| 2 | `F!` | `F` | -12 gets the -14 neighbour only | to -12: `IW2OHX 14-14` + own; Advert -12=1 |
| 3 | `F-` | `F` | -12 gets what is behind -14, not -14 | 190 records to -12, none `IW2OHX 14-14`; Advert -12=184 |
| 4 | `F` | `F=` | -12 gets own records only; -14 still told IW2OHX-12 | to -12: 3 own records; to -14: own + `IW2OHX 12-12`; Advert -12=0 |
| 5 | `F` | `F*` | warning + default | console `unknown link option in 'F*' - link uses default policy`; Advert -12=185 incl. `IW2OHX 14-14` |

`matrix.txt` is the raw output of runs 2-5. (Its "to 44.134.24.4" lines
filtered on the wrong source address and are empty; the corrected decode
is in the table above — IR2UFV sends to -14 from 192.168.1.202.)

Not field-tested: the live option-change path (`flex_link_opts_changed`).
LinBPQ has no command that re-reads an AXIP port's MAP lines, so on this
platform options change only with a restart.
