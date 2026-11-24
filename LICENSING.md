# Licensing

M-3rd-VAVE is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). If you give someone a firmware built from it, give them its complete source under the same licence.

## Who made what

| | |
| --- | --- |
| The module: `m3rdvave/m3rdvave.c`, `m3rdvave/eng/w3_engine.c`, `gen_tables.py`, `eng/test_*.c`; `tests/`, `build.sh` | James Hampshire |
| The players `m3rdvave/w3_play.inc` (chord generator, arpeggiator, loop recorder) | James Hampshire, from [PRO-FM-78](https://github.com/hamperj/pro-fm-78)'s `profm78_play.inc`, GPL-3.0-only |
| `m3rdvave/eng/fastmath.h` (single-precision maths) | Charles Vestal, from [X0X](https://github.com/charlesvestal/fm1-x0x), GPL-3.0-only |
| `m3rdvave/ref/ppg_data.c` | As extracted by Jacajack from a PPG Wave 2.2's EPROMs, in [Jacajack/avr-ppg](https://github.com/Jacajack/avr-ppg) (ab19d9d), a GPL-3.0 repository |
| The wavetables in `m3rdvave/eng/w3_tables.h` | Generated from that file: the PPG Wave 2.2's waves and wavetable maps (PPG / Wolfgang Palm), band-limited. The software licence does not relicense this data |
| The firmware it runs in (host, platform, tools) | [fm1-dock](https://github.com/joakimeriksson/fm1-dock) (Joakim Eriksson), on [Felucca](https://github.com/hugelton/Felucca) (Leo Kuroshita, Hügelton Instruments) and [SLOOP](https://github.com/isod89/sloop-fm1) (isod89): see fm1-dock's `LICENSING.md` |

Product names used here (M-VAVE, FM-1, Groove Synthesis, 3rd Wave, PPG, Wave 2.2) are their owners' trademarks.
M-3rd-VAVE is not affiliated with or endorsed by them.
