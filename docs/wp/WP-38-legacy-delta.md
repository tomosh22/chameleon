# WP-38 appendix: legacy encodings that main accepted and the decoder now refuses

Generated, not written by hand: both decoders (main at 755a455 and this branch) were run over every combination of
26 prefix forms, the four legacy maps (one-byte, `0F`, `0F 38`, `0F 3A`), 256 opcodes and 256 ModRM bytes (random
bytes behind them), and Zydis v4.1.1 was asked about each combination main accepted.

* main accepted 5,898,112 of the combinations; this branch refuses 4,174,116 of those.
* **Zydis decodes none of the 4,174,116** (`delta_real=0`). Every one is a byte sequence that is not an instruction.
* Wherever both decoders say Ok the length is the same (0 differences).

The list is by opcode. For each of the forms `none` (no mandatory prefix; REX, 67 and segment prefixes change
nothing), `66`, `F3`, `F2` (the last F2 / F3 prefix decides, a 66 next to it is ignored) and `lock` it says how much
of the opcode went from Ok to Unknown: `all` (every ModRM byte), `mem` (every memory form), `reg` (every register
form), or counts out of the 192 memory and 64 register ModRM bytes. An opcode or form that is not listed did not
change. Why each is not an instruction:

* `lock: all` / `lock: reg` / counts: F0 is #UD unless the instruction is one of add, or, adc, sbb, and, sub, xor,
  not, neg, inc, dec, xchg, xadd, cmpxchg, cmpxchg8b/16b, bts, btr, btc with a memory destination (SDM, LOCK).
* `0F 38` and `0F 3A`: main accepted every opcode under every prefix; only the listed forms of the documented
  opcodes exist (SDM appendix A, tables A-4 and A-5).
* `0F` with `F3` / `F2` / `66` / `none`: the SSE opcodes exist only with the mandatory prefixes the SDM names
  (`0F 28` has no F3 form, `0F 6C` only a 66 form, `0F B8` only F3, ...).
* `mem` or `reg` alone: the operand form does not exist (lea with a register, movnt* to a register, movmskps from
  memory, the shift-by-immediate groups with memory, movbe with a register, ...).
* `D9`..`DF`, `0F 01`, `0F AE`, `0F C7`, `8C`, `8E`, `0F 71`..`73`: the empty slots of the x87 escapes and of the
  opcode groups, and the segment registers that do not exist (6, 7) or cannot be loaded (cs).

```
00..01 | lock: reg
02..05 | lock: all
08..09 | lock: reg
0A..0D | lock: all
10..11 | lock: reg
12..15 | lock: all
18..19 | lock: reg
1A..1D | lock: all
20..21 | lock: reg
22..25 | lock: all
28..29 | lock: reg
2A..2D | lock: all
30..31 | lock: reg
32..35 | lock: all
38..3D | lock: all
50..5F | lock: all
63 | lock: all
68..6B | lock: all
70..7F | lock: all
80..81 | lock: mem 24/192, reg 64/64
83 | lock: mem 24/192, reg 64/64
84..85 | lock: all
86..87 | lock: reg
88..8B | lock: all
8C | none: mem 48/192, reg 16/64; 66: mem 48/192, reg 16/64; F3: mem 48/192, reg 16/64; F2: mem 48/192, reg 16/64; lock: all
8D | none: reg; 66: reg; F3: reg; F2: reg; lock: all
8E | none: mem 72/192, reg 24/64; 66: mem 72/192, reg 24/64; F3: mem 72/192, reg 24/64; F2: mem 72/192, reg 24/64; lock: all
8F | lock: mem 24/192, reg 8/64
90..99 | lock: all
9B..9F | lock: all
A4..C3 | lock: all
C6..C7 | lock: mem 24/192, reg 8/64
C9 | lock: all
CC..CD | lock: all
D0..D3 | lock: all
D7..D8 | lock: all
D9 | none: mem 24/192, reg 12/64; 66: mem 24/192, reg 12/64; F3: mem 24/192, reg 12/64; F2: mem 24/192, reg 12/64; lock: all
DA | none: mem 0/192, reg 31/64; 66: mem 0/192, reg 31/64; F3: mem 0/192, reg 31/64; F2: mem 0/192, reg 31/64; lock: all
DB | none: mem 48/192, reg 11/64; 66: mem 48/192, reg 11/64; F3: mem 48/192, reg 11/64; F2: mem 48/192, reg 11/64; lock: all
DC | lock: all
DD | none: mem 24/192, reg 16/64; 66: mem 24/192, reg 16/64; F3: mem 24/192, reg 16/64; F2: mem 24/192, reg 16/64; lock: all
DE | none: mem 0/192, reg 7/64; 66: mem 0/192, reg 7/64; F3: mem 0/192, reg 7/64; F2: mem 0/192, reg 7/64; lock: all
DF | none: mem 0/192, reg 15/64; 66: mem 0/192, reg 15/64; F3: mem 0/192, reg 15/64; F2: mem 0/192, reg 15/64; lock: all
E0..E3 | lock: all
E8..E9 | lock: all
EB | lock: all
F5 | lock: all
F6..F7 | lock: mem 144/192, reg 64/64
F8..F9 | lock: all
FC..FD | lock: all
FE | lock: mem 0/192, reg 16/64
FF | lock: mem 72/192, reg 40/64
0F 01 | none: mem 24/192, reg 10/64; 66: mem 24/192, reg 32/64; F3: mem 0/192, reg 26/64; F2: mem 24/192, reg 31/64; lock: all
0F 05 | lock: all
0F 0B | lock: all
0F 0D | lock: all
0F 10..11 | lock: all
0F 12 | 66: reg; lock: all
0F 13 | none: reg; 66: reg; F3: all; F2: all; lock: all
0F 14..15 | F3: all; F2: all; lock: all
0F 16 | 66: reg; F2: all; lock: all
0F 17 | none: reg; 66: reg; F3: all; F2: all; lock: all
0F 18..1F | lock: all
0F 28..29 | F3: all; F2: all; lock: all
0F 2A | lock: all
0F 2B | none: reg; 66: reg; F3: reg; F2: reg; lock: all
0F 2C..2D | lock: all
0F 2E..2F | F3: all; F2: all; lock: all
0F 31 | lock: all
0F 40..4F | lock: all
0F 50 | none: mem; 66: mem; F3: all; F2: all; lock: all
0F 51 | lock: all
0F 52..53 | 66: all; F2: all; lock: all
0F 54..57 | F3: all; F2: all; lock: all
0F 58..5A | lock: all
0F 5B | F2: all; lock: all
0F 5C..5F | lock: all
0F 60..6B | F3: all; F2: all; lock: all
0F 6C..6D | none: all; F3: all; F2: all; lock: all
0F 6E | F3: all; F2: all; lock: all
0F 6F | F2: all; lock: all
0F 70 | lock: all
0F 71..72 | none: mem 192/192, reg 40/64; 66: mem 192/192, reg 40/64; F3: all; F2: all; lock: all
0F 73 | none: mem 192/192, reg 48/64; 66: mem 192/192, reg 32/64; F3: all; F2: all; lock: all
0F 74..76 | F3: all; F2: all; lock: all
0F 77 | 66: all; F3: all; F2: all; lock: all
0F 7C..7D | none: all; F3: all; lock: all
0F 7E..7F | F2: all; lock: all
0F 80..9F | lock: all
0F A2..A5 | lock: all
0F AB | lock: reg
0F AC..AD | lock: all
0F AE | none: mem 0/192, reg 40/64; 66: mem 144/192, reg 48/64; F3: mem 144/192, reg 8/64; F2: mem 192/192, reg 56/64; lock: all
0F AF | lock: all
0F B0..B1 | lock: reg
0F B3 | lock: reg
0F B6..B7 | lock: all
0F B8 | none: all; 66: all; F2: all; lock: all
0F BA | lock: mem 24/192, reg 32/64
0F BB | lock: reg
0F BC..BF | lock: all
0F C0..C1 | lock: reg
0F C2 | lock: all
0F C3 | none: reg; 66: all; F3: all; F2: all; lock: all
0F C4 | F3: all; F2: all; lock: all
0F C5 | none: mem; 66: mem; F3: all; F2: all; lock: all
0F C6 | F3: all; F2: all; lock: all
0F C7 | none: mem 48/192, reg 48/64; 66: mem 144/192, reg 48/64; F3: mem 144/192, reg 48/64; F2: mem 168/192, reg 64/64; lock: mem 168/192, reg 64/64
0F C8..CF | lock: all
0F D0 | none: all; F3: all; lock: all
0F D1..D5 | F3: all; F2: all; lock: all
0F D6 | none: all; F3: mem; F2: mem; lock: all
0F D7 | none: mem; 66: mem; F3: all; F2: all; lock: all
0F D8..E5 | F3: all; F2: all; lock: all
0F E6 | none: all; lock: all
0F E7 | none: reg; 66: reg; F3: all; F2: all; lock: all
0F E8..EF | F3: all; F2: all; lock: all
0F F0 | none: all; 66: all; F3: all; F2: reg; lock: all
0F F1..F6 | F3: all; F2: all; lock: all
0F F7 | none: mem; 66: mem; F3: all; F2: all; lock: all
0F F8..FE | F3: all; F2: all; lock: all
0F 38 00..0B | F3: all; F2: all; lock: all
0F 38 0C..0F | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 10 | none: all; F3: all; F2: all; lock: all
0F 38 11..13 | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 14..15 | none: all; F3: all; F2: all; lock: all
0F 38 16 | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 17 | none: all; F3: all; F2: all; lock: all
0F 38 18..1B | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 1C..1E | F3: all; F2: all; lock: all
0F 38 1F | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 20..25 | none: all; F3: all; F2: all; lock: all
0F 38 26..27 | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 28..29 | none: all; F3: all; F2: all; lock: all
0F 38 2A | none: all; 66: reg; F3: all; F2: all; lock: all
0F 38 2B | none: all; F3: all; F2: all; lock: all
0F 38 2C..2F | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 30..35 | none: all; F3: all; F2: all; lock: all
0F 38 36 | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 37..41 | none: all; F3: all; F2: all; lock: all
0F 38 42..7F | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 80..82 | none: all; 66: reg; F3: all; F2: all; lock: all
0F 38 83..C7 | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 C8..CD | 66: all; F3: all; F2: all; lock: all
0F 38 CE | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 CF | none: all; F3: all; F2: all; lock: all
0F 38 D0..D7 | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 D8 | none: all; 66: all; F3: mem 96/192, reg 64/64; F2: all; lock: all
0F 38 D9..DA | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 DB | none: all; F3: all; F2: all; lock: all
0F 38 DC | none: all; F2: all; lock: all
0F 38 DD..DF | none: all; F3: reg; F2: all; lock: all
0F 38 E0..EF | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 F0..F1 | none: reg; 66: reg; F3: all; lock: all
0F 38 F2..F4 | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 F5 | none: all; 66: reg; F3: all; F2: all; lock: all
0F 38 F6 | none: reg; F2: all; lock: all
0F 38 F7 | none: all; 66: all; F3: all; F2: all; lock: all
0F 38 F8 | none: all; 66: reg; F3: reg; F2: reg; lock: all
0F 38 F9 | none: reg; 66: all; F3: all; F2: all; lock: all
0F 38 FA..FB | none: all; 66: all; F3: mem; F2: all; lock: all
0F 38 FC | none: reg; 66: reg; F3: reg; F2: reg; lock: all
0F 38 FD..FF | none: all; 66: all; F3: all; F2: all; lock: all
0F 3A 00..07 | none: all; 66: all; F3: all; F2: all; lock: all
0F 3A 08..0E | none: all; F3: all; F2: all; lock: all
0F 3A 0F | F3: all; F2: all; lock: all
0F 3A 10..13 | none: all; 66: all; F3: all; F2: all; lock: all
0F 3A 14..17 | none: all; F3: all; F2: all; lock: all
0F 3A 18..1F | none: all; 66: all; F3: all; F2: all; lock: all
0F 3A 20..22 | none: all; F3: all; F2: all; lock: all
0F 3A 23..3F | none: all; 66: all; F3: all; F2: all; lock: all
0F 3A 40..42 | none: all; F3: all; F2: all; lock: all
0F 3A 43 | none: all; 66: all; F3: all; F2: all; lock: all
0F 3A 44 | none: all; F3: all; F2: all; lock: all
0F 3A 45..5F | none: all; 66: all; F3: all; F2: all; lock: all
0F 3A 60..63 | none: all; F3: all; F2: all; lock: all
0F 3A 64..CB | none: all; 66: all; F3: all; F2: all; lock: all
0F 3A CC | 66: all; F3: all; F2: all; lock: all
0F 3A CD | none: all; 66: all; F3: all; F2: all; lock: all
0F 3A CE..CF | none: all; F3: all; F2: all; lock: all
0F 3A D0..DE | none: all; 66: all; F3: all; F2: all; lock: all
0F 3A DF | none: all; F3: all; F2: all; lock: all
0F 3A E0..EF | none: all; 66: all; F3: all; F2: all; lock: all
0F 3A F0 | none: all; 66: all; F3: mem 192/192, reg 63/64; F2: all; lock: all
0F 3A F1..FF | none: all; 66: all; F3: all; F2: all; lock: all
```
