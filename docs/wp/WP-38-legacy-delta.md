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

## Function entries: main next to this branch

`LDE-OUTCOME` of `cml_lde_probe` (`plan_hook()` over every function entry of the module's PDB) for every fixture
module in every `ci` cell. main: ci run 45411803431 (755a455). This branch: ci run 45433089557 (5e49806). Read from the
`ctest-junit.xml` of each cell's `logs-*` artifact. `cml_lde_avx_module` is new in this branch.

In all 104 module x cell pairs the number of functions is the same on both sides, `refused_unknown_opcode` and
`refused_other` are 0 on both sides, and `refused` goes from a total of 13844 (all of it `refused_vex`) to
0: every function that was refused is planned now, nothing that was planned is refused.

| cell | module | functions | main: padding / five_byte / refused | head: padding / five_byte / refused | refused_vex | head: unknown_opcode / other |
|---|---|---|---|---|---|---|
| windows-2022 / clang-cl / Debug | basic (`tool.lde_probe_basic`) | 5115 | 5064 / 50 / 1 | 5065 / 50 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | avx_module | 3649 | (new) | 3532 / 117 / 0 | 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | basic | 5115 | 5064 / 50 / 1 | 5065 / 50 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | templates | 5247 | 5196 / 50 / 1 | 5197 / 50 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | virtuals | 5256 | 5205 / 50 / 1 | 5206 / 50 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | statics | 5247 | 5196 / 50 / 1 | 5197 / 50 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | lambdas | 5276 | 5225 / 50 / 1 | 5226 / 50 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | optimised | 4982 | 4931 / 50 / 1 | 4932 / 50 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | tls | 5521 | 5470 / 50 / 1 | 5471 / 50 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | pch | 5139 | 5088 / 50 / 1 | 5089 / 50 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | bigobj | 69603 | 69554 / 49 / 0 | 69554 / 49 / 0 | 0 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | elision | 5249 | 5198 / 50 / 1 | 5199 / 50 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | hot_restart | 5109 | 5058 / 50 / 1 | 5059 / 50 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / Debug | new_function | 5243 | 5192 / 50 / 1 | 5193 / 50 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | basic (`tool.lde_probe_basic`) | 4905 | 4659 / 34 / 212 | 4871 / 34 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | avx_module | 3484 | (new) | 3402 / 82 / 0 | 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | basic | 4905 | 4659 / 34 / 212 | 4871 / 34 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | templates | 5003 | 4757 / 34 / 212 | 4969 / 34 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | virtuals | 5010 | 4764 / 34 / 212 | 4976 / 34 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | statics | 5002 | 4756 / 34 / 212 | 4968 / 34 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | lambdas | 5030 | 4784 / 34 / 212 | 4996 / 34 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | optimised | 4780 | 4534 / 34 / 212 | 4746 / 34 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | tls | 5273 | 5027 / 34 / 212 | 5239 / 34 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | pch | 4927 | 4681 / 34 / 212 | 4893 / 34 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | bigobj | 69439 | 69401 / 33 / 5 | 69406 / 33 / 0 | 5 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | elision | 5005 | 4759 / 34 / 212 | 4971 / 34 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | hot_restart | 4923 | 4677 / 34 / 212 | 4889 / 34 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / clang-cl / RelWithDebInfo | new_function | 4999 | 4753 / 34 / 212 | 4965 / 34 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | basic (`tool.lde_probe_basic`) | 5206 | 5173 / 32 / 1 | 5174 / 32 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | avx_module | 3711 | (new) | 3679 / 32 / 0 | 0 | 0 / 0 |
| windows-2022 / msvc / Debug | basic | 5206 | 5173 / 32 / 1 | 5174 / 32 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | templates | 5343 | 5310 / 32 / 1 | 5311 / 32 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | virtuals | 5354 | 5321 / 32 / 1 | 5322 / 32 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | statics | 5344 | 5311 / 32 / 1 | 5312 / 32 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | lambdas | 5374 | 5341 / 32 / 1 | 5342 / 32 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | optimised | 5274 | 5241 / 32 / 1 | 5242 / 32 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | tls | 5609 | 5576 / 32 / 1 | 5577 / 32 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | pch | 5233 | 5200 / 32 / 1 | 5201 / 32 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | bigobj | 3865 | 3833 / 32 / 0 | 3833 / 32 / 0 | 0 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | elision | 5345 | 5312 / 32 / 1 | 5313 / 32 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | hot_restart | 5205 | 5172 / 32 / 1 | 5173 / 32 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / msvc / Debug | new_function | 5339 | 5306 / 32 / 1 | 5307 / 32 / 0 | 1 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | basic (`tool.lde_probe_basic`) | 4922 | 4693 / 17 / 212 | 4905 / 17 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | avx_module | 3474 | (new) | 3457 / 17 / 0 | 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | basic | 4922 | 4693 / 17 / 212 | 4905 / 17 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | templates | 5034 | 4805 / 17 / 212 | 5017 / 17 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | virtuals | 5043 | 4814 / 17 / 212 | 5026 / 17 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | statics | 5034 | 4805 / 17 / 212 | 5017 / 17 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | lambdas | 5069 | 4840 / 17 / 212 | 5052 / 17 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | optimised | 5033 | 4804 / 17 / 212 | 5016 / 17 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | tls | 5301 | 5072 / 17 / 212 | 5284 / 17 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | pch | 4946 | 4717 / 17 / 212 | 4929 / 17 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | bigobj | 3628 | 3606 / 17 / 5 | 3611 / 17 / 0 | 5 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | elision | 5036 | 4807 / 17 / 212 | 5019 / 17 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | hot_restart | 4920 | 4691 / 17 / 212 | 4903 / 17 / 0 | 212 -> 0 | 0 / 0 |
| windows-2022 / msvc / RelWithDebInfo | new_function | 5030 | 4801 / 17 / 212 | 5013 / 17 / 0 | 212 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | basic (`tool.lde_probe_basic`) | 6034 | 5974 / 58 / 2 | 5976 / 58 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | avx_module | 3642 | (new) | 3519 / 123 / 0 | 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | basic | 6034 | 5974 / 58 / 2 | 5976 / 58 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | templates | 6169 | 6109 / 58 / 2 | 6111 / 58 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | virtuals | 6178 | 6118 / 58 / 2 | 6120 / 58 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | statics | 6169 | 6109 / 58 / 2 | 6111 / 58 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | lambdas | 6198 | 6138 / 58 / 2 | 6140 / 58 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | optimised | 5896 | 5836 / 58 / 2 | 5838 / 58 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | tls | 6424 | 6364 / 58 / 2 | 6366 / 58 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | pch | 6060 | 6000 / 58 / 2 | 6002 / 58 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | bigobj | 69596 | 69539 / 57 / 0 | 69539 / 57 / 0 | 0 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | elision | 6171 | 6111 / 58 / 2 | 6113 / 58 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | hot_restart | 6028 | 5968 / 58 / 2 | 5970 / 58 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / Debug | new_function | 6165 | 6105 / 58 / 2 | 6107 / 58 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | basic (`tool.lde_probe_basic`) | 5823 | 5420 / 42 / 361 | 5781 / 42 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | avx_module | 3474 | (new) | 3391 / 83 / 0 | 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | basic | 5823 | 5420 / 42 / 361 | 5781 / 42 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | templates | 5921 | 5519 / 41 / 361 | 5880 / 41 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | virtuals | 5928 | 5526 / 41 / 361 | 5887 / 41 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | statics | 5920 | 5518 / 41 / 361 | 5879 / 41 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | lambdas | 5948 | 5546 / 41 / 361 | 5907 / 41 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | optimised | 5689 | 5287 / 41 / 361 | 5648 / 41 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | tls | 6174 | 5772 / 41 / 361 | 6133 / 41 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | pch | 5846 | 5444 / 41 / 361 | 5805 / 41 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | bigobj | 69430 | 69385 / 40 / 5 | 69390 / 40 / 0 | 5 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | elision | 5923 | 5521 / 41 / 361 | 5882 / 41 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | hot_restart | 5841 | 5438 / 42 / 361 | 5799 / 42 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / clang-cl / RelWithDebInfo | new_function | 5917 | 5515 / 41 / 361 | 5876 / 41 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | basic (`tool.lde_probe_basic`) | 6114 | 6080 / 32 / 2 | 6082 / 32 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | avx_module | 3696 | (new) | 3664 / 32 / 0 | 0 | 0 / 0 |
| windows-2025 / msvc / Debug | basic | 6114 | 6080 / 32 / 2 | 6082 / 32 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | templates | 6254 | 6220 / 32 / 2 | 6222 / 32 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | virtuals | 6265 | 6231 / 32 / 2 | 6233 / 32 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | statics | 6255 | 6221 / 32 / 2 | 6223 / 32 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | lambdas | 6285 | 6251 / 32 / 2 | 6253 / 32 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | optimised | 6186 | 6152 / 32 / 2 | 6154 / 32 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | tls | 6502 | 6468 / 32 / 2 | 6470 / 32 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | pch | 6143 | 6109 / 32 / 2 | 6111 / 32 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | bigobj | 3850 | 3818 / 32 / 0 | 3818 / 32 / 0 | 0 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | elision | 6256 | 6222 / 32 / 2 | 6224 / 32 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | hot_restart | 6112 | 6078 / 32 / 2 | 6080 / 32 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / msvc / Debug | new_function | 6250 | 6216 / 32 / 2 | 6218 / 32 / 0 | 2 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | basic (`tool.lde_probe_basic`) | 5828 | 5451 / 16 / 361 | 5812 / 16 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | avx_module | 3456 | (new) | 3441 / 15 / 0 | 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | basic | 5828 | 5451 / 16 / 361 | 5812 / 16 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | templates | 5942 | 5565 / 16 / 361 | 5926 / 16 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | virtuals | 5951 | 5574 / 16 / 361 | 5935 / 16 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | statics | 5942 | 5565 / 16 / 361 | 5926 / 16 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | lambdas | 5977 | 5600 / 16 / 361 | 5961 / 16 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | optimised | 5940 | 5563 / 16 / 361 | 5924 / 16 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | tls | 6193 | 5816 / 16 / 361 | 6177 / 16 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | pch | 5854 | 5477 / 16 / 361 | 5838 / 16 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | bigobj | 3610 | 3590 / 15 / 5 | 3595 / 15 / 0 | 5 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | elision | 5944 | 5567 / 16 / 361 | 5928 / 16 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | hot_restart | 5826 | 5448 / 17 / 361 | 5809 / 17 / 0 | 361 -> 0 | 0 / 0 |
| windows-2025 / msvc / RelWithDebInfo | new_function | 5938 | 5561 / 16 / 361 | 5922 / 16 / 0 | 361 -> 0 | 0 / 0 |
