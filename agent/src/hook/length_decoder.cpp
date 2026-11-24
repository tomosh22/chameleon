#include "length_decoder.h"

#include <array>

namespace cml::agent::hook {

namespace {

// ---- legacy maps (one-byte, 0F, 0F 38, 0F 3A) ---------------------------------------------------------------------
//
// Opcode attributes. An opcode with attribute 0 is "not covered": decode_instruction() reports
// Unknown. Everything the tables do not name explicitly is therefore refused.
constexpr std::uint32_t V = 1u << 0;      // valid / covered
constexpr std::uint32_t M = 1u << 1;      // has a ModRM byte (plus SIB and displacement as it dictates)
constexpr std::uint32_t I8 = 1u << 2;     // 8-bit immediate (also rel8)
constexpr std::uint32_t I16 = 1u << 3;    // 16-bit immediate (ret imm16)
constexpr std::uint32_t IZ = 1u << 4;     // 32-bit immediate; 16-bit with a 66 prefix and no REX.W
constexpr std::uint32_t IV = 1u << 5;     // mov r, imm: 64-bit with REX.W, else 16-bit with 66, else 32-bit
constexpr std::uint32_t I32 = 1u << 6;    // always 32 bits (rel32)
constexpr std::uint32_t CALL = 1u << 7;
constexpr std::uint32_t JMP = 1u << 8;
constexpr std::uint32_t JCC = 1u << 9;
constexpr std::uint32_t RET = 1u << 10;
constexpr std::uint32_t TRAP = 1u << 11;

// Opcode groups: the ModRM byte (and sometimes the prefix) selects the instruction, which changes validity and the
// immediate. Stored in bits 12..16.
constexpr unsigned kGroupShift = 12;
constexpr std::uint32_t kGroupMask = 31u << kGroupShift;
constexpr std::uint32_t G_F6 = 1u << kGroupShift;      // test Eb,Ib has an imm8 (reg 0,1); others none
constexpr std::uint32_t G_F7 = 2u << kGroupShift;      // test Ev,Iz has an immediate (reg 0,1); others none
constexpr std::uint32_t G_REG0 = 3u << kGroupShift;    // only reg == 0 is covered (mov Eb/Ev,imm; pop Ev)
constexpr std::uint32_t G_FE = 4u << kGroupShift;      // inc/dec Eb: reg 0,1
constexpr std::uint32_t G_FF = 5u << kGroupShift;      // inc, dec, call, jmp, push: reg 0,1,2,4,6
constexpr std::uint32_t G_BA = 6u << kGroupShift;      // bt/bts/btr/btc Ev,Ib: reg 4..7
constexpr std::uint32_t G_ALU = 7u << kGroupShift;     // group 1 (80, 81, 83): all eight; cmp (reg 7) takes no lock
constexpr std::uint32_t G_X87 = 8u << kGroupShift;     // D8..DF: the defined slots of each escape opcode
constexpr std::uint32_t G_8C = 9u << kGroupShift;      // mov r/m, sreg: reg 0..5
constexpr std::uint32_t G_8E = 10u << kGroupShift;     // mov sreg, r/m: reg 0, 2..5 (cs cannot be loaded)
constexpr std::uint32_t G_0F01 = 11u << kGroupShift;   // group 7
constexpr std::uint32_t G_0FAE = 12u << kGroupShift;   // group 15
constexpr std::uint32_t G_0FC7 = 13u << kGroupShift;   // group 9
constexpr std::uint32_t G_SH71 = 14u << kGroupShift;   // groups 12, 13 (0F 71, 0F 72): reg 2, 4, 6, register form
constexpr std::uint32_t G_SH73 = 15u << kGroupShift;   // group 14 (0F 73): reg 2, 6; reg 3, 7 with 66 only
constexpr std::uint32_t G_66MEM = 16u << kGroupShift;  // 0F 12, 0F 16: the 66 form (movlpd, movhpd) is memory only
constexpr std::uint32_t G_D6 = 17u << kGroupShift;     // 0F D6: the F3 / F2 forms (movq2dq, movdq2q) are register only
constexpr std::uint32_t G_MOVBE = 18u << kGroupShift;  // 0F 38 F0/F1: movbe (no prefix, 66) is memory only; crc32 (F2)
constexpr std::uint32_t G_38F6 = 19u << kGroupShift;   // 0F 38 F6: wrss (no prefix) is memory only; adcx, adox
constexpr std::uint32_t G_38D8 = 20u << kGroupShift;   // F3 0F 38 D8: the wide Key Locker group, reg 0..3
constexpr std::uint32_t G_F3MEM = 21u << kGroupShift;  // 0F 38 DD..DF: the F3 form (Key Locker) is memory only
constexpr std::uint32_t G_C0 = 22u << kGroupShift;     // F3 0F 3A F0: only ModRM C0 (hreset)

constexpr std::uint32_t MEM = 1u << 17;   // memory operand only: ModRM.mod == 3 is not an instruction
constexpr std::uint32_t REG = 1u << 18;   // register operand only: ModRM.mod must be 3
constexpr std::uint32_t LOCK = 1u << 19;  // takes a lock prefix (F0) when its destination is memory

// Mandatory prefixes (SSE and later): when any of these four is set, the opcode exists only in the listed forms. The
// form is selected the way the processor does it: the last F2 / F3 prefix, else 66 if present, else none. Without any
// of the four bits the opcode does not care (the integer instructions: 66, F2 and F3 are operand-size, rep or ignored).
constexpr unsigned kPrefixShift = 20;
constexpr std::uint32_t NP = 1u << kPrefixShift;         // no mandatory prefix
constexpr std::uint32_t P66 = 2u << kPrefixShift;
constexpr std::uint32_t PF3 = 4u << kPrefixShift;
constexpr std::uint32_t PF2 = 8u << kPrefixShift;
constexpr std::uint32_t kPrefixFormMask = 15u << kPrefixShift;
constexpr std::uint32_t PS_PD = NP | P66;                // packed single / packed double, or MMX / SSE2 integer
constexpr std::uint32_t ALL4 = NP | P66 | PF3 | PF2;     // ps, pd, ss, sd

// ---- VEX and EVEX maps 1..3 (ADR-0042, WP-38) -----------------------------------------------------------------------
//
// One byte per opcode: bits 0..3 say for which values of the `pp` field (0 none, 1 = 66, 2 = F3, 3 = F2) the opcode is
// an instruction, bit 4 that an 8-bit immediate follows (every opcode of map 3, a few of map 1), bit 5 that there is
// no ModRM byte (VEX 0F 77, vzeroupper / vzeroall, is the only one). 0 = not covered = Unknown. The length does not
// depend on VEX.L, VEX.W, vvvv, the mask or the broadcast bit, so they are not looked at: an encoding that is #UD only
// because of one of those fields still gets the length of its opcode. What the tables name are the instructions of
// AVX, AVX2, FMA, F16C, BMI1, BMI2, AVX-VNNI (and INT8, INT16), AVX-IFMA, AVX-NE-CONVERT, VAES, VPCLMULQDQ, GFNI,
// SHA512, SM3, SM4 and the opmask (k-register) instructions under VEX, and AVX-512 F, CD, BW, DQ, VL, IFMA, VBMI,
// VBMI2, VNNI, BITALG, VPOPCNTDQ, BF16, VP2INTERSECT, VAES, VPCLMULQDQ and GFNI under EVEX. FMA4, XOP, AMX, the Xeon
// Phi sets, AVX512-FP16 (EVEX maps 5 and 6, and its few map-3 forms) and APX (EVEX map 4) are not covered.
constexpr std::uint8_t kVexNone = 1u << 0;
constexpr std::uint8_t kVex66 = 1u << 1;
constexpr std::uint8_t kVexF3 = 1u << 2;
constexpr std::uint8_t kVexF2 = 1u << 3;
constexpr std::uint8_t kVexImm8 = 1u << 4;
constexpr std::uint8_t kVexNoModRm = 1u << 5;
constexpr std::uint8_t kVexPsPd = kVexNone | kVex66;
constexpr std::uint8_t kVexAll4 = kVexNone | kVex66 | kVexF3 | kVexF2;

using VexMap = std::array<std::uint8_t, 256>;

struct Tables {
    std::array<std::uint32_t, 256> one{};    // one-byte opcode map
    std::array<std::uint32_t, 256> two{};    // 0F xx
    std::array<std::uint32_t, 256> map38{};  // 0F 38 xx
    std::array<std::uint32_t, 256> map3a{};  // 0F 3A xx
    std::array<VexMap, 3> vex{};             // C4 / C5, maps 1 (0F), 2 (0F 38), 3 (0F 3A)
    std::array<VexMap, 3> evex{};            // 62, the same three maps
    // x87 (D8..DF): which ModRM.reg values are instructions with a memory operand, and which of the 64 register
    // forms (ModRM C0..FF, bit = ModRM - C0) are.
    std::array<std::uint8_t, 8> x87_memory{};
    std::array<std::uint64_t, 8> x87_register{};
};

constexpr void fill(std::array<std::uint32_t, 256>& table, unsigned lo, unsigned hi, std::uint32_t attributes) {
    for (unsigned i = lo; i <= hi; ++i) {
        table[i] = V | attributes;
    }
}

constexpr void vfill(VexMap& table, unsigned lo, unsigned hi, std::uint8_t attributes) {
    for (unsigned i = lo; i <= hi; ++i) {
        table[i] = attributes;
    }
}

constexpr std::uint64_t bits(unsigned lo, unsigned hi) {  // ModRM bytes lo..hi (C0..FF) as a mask
    std::uint64_t mask = 0;
    for (unsigned i = lo; i <= hi; ++i) {
        mask |= std::uint64_t{1} << (i - 0xC0);
    }
    return mask;
}

constexpr void build_legacy(Tables& t) {
    auto& one = t.one;
    auto& two = t.two;

    // 00..3F: the eight ALU operations (add or adc sbb and sub xor cmp), each as
    // Eb,Gb / Ev,Gv / Gb,Eb / Gv,Ev / AL,Ib / eAX,Iz. The remaining slots of those rows are
    // prefixes (26 2E 36 3E, handled before the lookup), the 0F escape or invalid in 64-bit mode.
    for (unsigned base = 0; base < 0x40; base += 8) {
        fill(one, base, base + 1, M | (base == 0x38 ? 0u : LOCK));  // to memory: lockable, except cmp
        fill(one, base + 2, base + 3, M);
        fill(one, base + 4, base + 4, I8);
        fill(one, base + 5, base + 5, IZ);
    }
    fill(one, 0x50, 0x5F, 0);          // push / pop r64
    fill(one, 0x63, 0x63, M);          // movsxd
    fill(one, 0x68, 0x68, IZ);         // push imm32
    fill(one, 0x69, 0x69, M | IZ);     // imul r, r/m, imm32
    fill(one, 0x6A, 0x6A, I8);         // push imm8
    fill(one, 0x6B, 0x6B, M | I8);     // imul r, r/m, imm8
    fill(one, 0x70, 0x7F, I8 | JCC);   // jcc rel8
    fill(one, 0x80, 0x80, M | I8 | G_ALU);  // group 1 Eb,Ib
    fill(one, 0x81, 0x81, M | IZ | G_ALU);  // group 1 Ev,Iz
    fill(one, 0x83, 0x83, M | I8 | G_ALU);  // group 1 Ev,Ib
    fill(one, 0x84, 0x85, M);          // test
    fill(one, 0x86, 0x87, M | LOCK);   // xchg
    fill(one, 0x88, 0x8B, M);          // mov
    fill(one, 0x8C, 0x8C, M | G_8C);   // mov r/m, sreg
    fill(one, 0x8D, 0x8D, M | MEM);    // lea
    fill(one, 0x8E, 0x8E, M | G_8E);   // mov sreg, r/m
    fill(one, 0x8F, 0x8F, M | G_REG0); // pop Ev
    fill(one, 0x90, 0x99, 0);          // nop, xchg r,rax, cwde, cdq
    fill(one, 0x9B, 0x9F, 0);          // fwait, pushf, popf, sahf, lahf
    fill(one, 0xA4, 0xA7, 0);          // movs, cmps (with or without rep)
    fill(one, 0xA8, 0xA8, I8);         // test al, imm8
    fill(one, 0xA9, 0xA9, IZ);         // test eax, imm32
    fill(one, 0xAA, 0xAF, 0);          // stos, lods, scas
    fill(one, 0xB0, 0xB7, I8);         // mov r8, imm8
    fill(one, 0xB8, 0xBF, IV);         // mov r, imm16/32/64
    fill(one, 0xC0, 0xC1, M | I8);     // shift group, imm8
    fill(one, 0xC2, 0xC2, I16 | RET);  // ret imm16
    fill(one, 0xC3, 0xC3, RET);        // ret
    fill(one, 0xC6, 0xC6, M | I8 | G_REG0);  // mov Eb, imm8
    fill(one, 0xC7, 0xC7, M | IZ | G_REG0);  // mov Ev, imm32
    fill(one, 0xC9, 0xC9, 0);          // leave
    fill(one, 0xCC, 0xCC, TRAP);       // int3
    fill(one, 0xCD, 0xCD, I8 | TRAP);  // int imm8 (int 29h is __fastfail)
    fill(one, 0xD0, 0xD3, M);          // shift group by 1 / cl
    fill(one, 0xD7, 0xD7, 0);          // xlat
    fill(one, 0xD8, 0xDF, M | G_X87);  // x87 escape: always a ModRM byte
    fill(one, 0xE0, 0xE3, I8 | JCC);   // loopne loope loop jrcxz
    fill(one, 0xE8, 0xE8, I32 | CALL); // call rel32
    fill(one, 0xE9, 0xE9, I32 | JMP);  // jmp rel32
    fill(one, 0xEB, 0xEB, I8 | JMP);   // jmp rel8
    fill(one, 0xF5, 0xF5, 0);          // cmc
    fill(one, 0xF6, 0xF6, M | G_F6);   // test not neg mul imul div idiv Eb
    fill(one, 0xF7, 0xF7, M | G_F7);   // same, Ev
    fill(one, 0xF8, 0xF9, 0);          // clc stc
    fill(one, 0xFC, 0xFD, 0);          // cld std
    fill(one, 0xFE, 0xFE, M | G_FE);
    fill(one, 0xFF, 0xFF, M | G_FF);

    // x87, SDM volume 2 appendix A.5 (tables A-7 to A-22), plus the aliases every processor executes although the
    // tables leave their slots empty (fstp1 D9 D8.., fcom2 / fcomp3 DC D0.., fxch4 DD C8.., fcomp5 DE D0.., ffreep /
    // fxch7 / fstp8 / fstp9 DF C0.., and the 8087 / 287 no-ops DB E0, E1, E4): WP-10's decoder accepted them, some
    // compilers emit ffreep, and they are two bytes like every register form.
    t.x87_memory = {0xFF, 0xFD, 0xFF, 0xAF, 0xFF, 0xDF, 0xFF, 0xFF};  // D9 /1, DB /4 /6 and DD /5 are undefined
    t.x87_register[0] = bits(0xC0, 0xFF);                                                    // D8
    t.x87_register[1] = bits(0xC0, 0xD0) | bits(0xD8, 0xE1) | bits(0xE4, 0xE5) | bits(0xE8, 0xEE) | bits(0xF0, 0xFF);  // D9
    t.x87_register[2] = bits(0xC0, 0xDF) | bits(0xE9, 0xE9);                                 // DA: fcmovcc, fucompp
    t.x87_register[3] = bits(0xC0, 0xE4) | bits(0xE8, 0xF7);                                 // DB: fcmovncc, fnclex, fninit, f(u)comi
    t.x87_register[4] = bits(0xC0, 0xFF);                                                    // DC
    t.x87_register[5] = bits(0xC0, 0xEF);                                                    // DD: ffree, fst, fstp, fucom(p)
    t.x87_register[6] = bits(0xC0, 0xD7) | bits(0xD9, 0xD9) | bits(0xE0, 0xFF);              // DE
    t.x87_register[7] = bits(0xC0, 0xE0) | bits(0xE8, 0xF7);                                 // DF: fnstsw ax, f(u)comip

    // 0F xx
    fill(two, 0x01, 0x01, M | G_0F01);       // group 7 (xgetbv, rdtscp, ...)
    fill(two, 0x05, 0x05, 0);                // syscall
    fill(two, 0x0B, 0x0B, TRAP);             // ud2
    fill(two, 0x0D, 0x0D, M);                // prefetchw
    fill(two, 0x10, 0x11, M | ALL4);         // movups movupd movss movsd
    fill(two, 0x12, 0x12, M | ALL4 | G_66MEM);          // movlps/movhlps movlpd movsldup movddup
    fill(two, 0x13, 0x13, M | PS_PD | MEM);  // movlps / movlpd store
    fill(two, 0x14, 0x15, M | PS_PD);        // unpcklp, unpckhp
    fill(two, 0x16, 0x16, M | PS_PD | PF3 | G_66MEM);   // movhps/movlhps movhpd movshdup
    fill(two, 0x17, 0x17, M | PS_PD | MEM);  // movhps / movhpd store
    fill(two, 0x18, 0x1F, M);                // prefetch hints, nop Ev (0F 1F), endbr64 (F3 0F 1E FA)
    fill(two, 0x28, 0x29, M | PS_PD);        // movaps movapd
    fill(two, 0x2A, 0x2A, M | ALL4);         // cvtpi2ps cvtpi2pd cvtsi2ss cvtsi2sd
    fill(two, 0x2B, 0x2B, M | ALL4 | MEM);   // movntps movntpd; movntss movntsd (SSE4a)
    fill(two, 0x2C, 0x2D, M | ALL4);         // cvt(t)ps2pi ... cvt(t)sd2si
    fill(two, 0x2E, 0x2F, M | PS_PD);        // ucomis comis
    fill(two, 0x31, 0x31, 0);                // rdtsc
    fill(two, 0x40, 0x4F, M);                // cmovcc
    fill(two, 0x50, 0x50, M | PS_PD | REG);  // movmskps movmskpd
    fill(two, 0x51, 0x51, M | ALL4);         // sqrt
    fill(two, 0x52, 0x53, M | NP | PF3);     // rsqrt, rcp (ps, ss)
    fill(two, 0x54, 0x57, M | PS_PD);        // and andn or xor
    fill(two, 0x58, 0x5A, M | ALL4);         // add mul cvt
    fill(two, 0x5B, 0x5B, M | PS_PD | PF3);  // cvtdq2ps cvtps2dq cvttps2dq
    fill(two, 0x5C, 0x5F, M | ALL4);         // sub min div max
    fill(two, 0x60, 0x6B, M | PS_PD);        // punpck, packs, pcmpgt (MMX and SSE2)
    fill(two, 0x6C, 0x6D, M | P66);          // punpcklqdq punpckhqdq
    fill(two, 0x6E, 0x6E, M | PS_PD);        // movd / movq
    fill(two, 0x6F, 0x6F, M | PS_PD | PF3);  // movq mm, movdqa, movdqu
    fill(two, 0x70, 0x70, M | I8 | ALL4);    // pshufw pshufd pshufhw pshuflw
    fill(two, 0x71, 0x72, M | I8 | PS_PD | G_SH71);  // shift-by-immediate groups 12, 13
    fill(two, 0x73, 0x73, M | I8 | PS_PD | G_SH73);  // group 14
    fill(two, 0x74, 0x76, M | PS_PD);        // pcmpeq
    fill(two, 0x77, 0x77, NP);               // emms
    fill(two, 0x7C, 0x7D, M | P66 | PF2);    // haddp, hsubp
    fill(two, 0x7E, 0x7F, M | PS_PD | PF3);  // movd/movq, movq, movdqa/movdqu stores
    fill(two, 0x80, 0x8F, I32 | JCC);        // jcc rel32
    fill(two, 0x90, 0x9F, M);                // setcc
    fill(two, 0xA2, 0xA2, 0);                // cpuid
    fill(two, 0xA3, 0xA3, M);                // bt
    fill(two, 0xA4, 0xA4, M | I8);           // shld imm8
    fill(two, 0xA5, 0xA5, M);
    fill(two, 0xAB, 0xAB, M | LOCK);         // bts
    fill(two, 0xAC, 0xAC, M | I8);           // shrd imm8
    fill(two, 0xAD, 0xAD, M);                // shrd cl
    fill(two, 0xAE, 0xAE, M | G_0FAE);       // group 15 (fences, ldmxcsr, fxsave, ...)
    fill(two, 0xAF, 0xAF, M);                // imul
    fill(two, 0xB0, 0xB1, M | LOCK);         // cmpxchg
    fill(two, 0xB3, 0xB3, M | LOCK);         // btr
    fill(two, 0xB6, 0xB7, M);                // movzx
    fill(two, 0xB8, 0xB8, M | PF3);          // popcnt
    fill(two, 0xBA, 0xBA, M | I8 | G_BA);
    fill(two, 0xBB, 0xBB, M | LOCK);         // btc
    fill(two, 0xBC, 0xBF, M);                // bsf/tzcnt, bsr/lzcnt, movsx
    fill(two, 0xC0, 0xC1, M | LOCK);         // xadd
    fill(two, 0xC2, 0xC2, M | I8 | ALL4);    // cmpps cmppd cmpss cmpsd
    fill(two, 0xC3, 0xC3, M | NP | MEM);     // movnti
    fill(two, 0xC4, 0xC4, M | I8 | PS_PD);   // pinsrw
    fill(two, 0xC5, 0xC5, M | I8 | PS_PD | REG);  // pextrw
    fill(two, 0xC6, 0xC6, M | I8 | PS_PD);   // shufps shufpd
    fill(two, 0xC7, 0xC7, M | G_0FC7);       // group 9 (cmpxchg8b/16b, rdrand, ...)
    fill(two, 0xC8, 0xCF, 0);                // bswap
    fill(two, 0xD0, 0xD0, M | P66 | PF2);    // addsubpd addsubps
    fill(two, 0xD1, 0xD5, M | PS_PD);        // psrl, paddq, pmullw
    fill(two, 0xD6, 0xD6, M | P66 | PF3 | PF2 | G_D6);  // movq, movq2dq, movdq2q
    fill(two, 0xD7, 0xD7, M | PS_PD | REG);  // pmovmskb
    fill(two, 0xD8, 0xE5, M | PS_PD);        // psubus, pminub, pand, paddus, pmaxub, pandn, pavg, psra, pmulh
    fill(two, 0xE6, 0xE6, M | P66 | PF3 | PF2);  // cvttpd2dq cvtdq2pd cvtpd2dq
    fill(two, 0xE7, 0xE7, M | PS_PD | MEM);  // movntq movntdq
    fill(two, 0xE8, 0xEF, M | PS_PD);        // psubs, pminsw, por, padds, pmaxsw, pxor
    fill(two, 0xF0, 0xF0, M | PF2 | MEM);    // lddqu
    fill(two, 0xF1, 0xF6, M | PS_PD);        // psll, pmuludq, pmaddwd, psadbw
    fill(two, 0xF7, 0xF7, M | PS_PD | REG);  // maskmovq maskmovdqu
    fill(two, 0xF8, 0xFE, M | PS_PD);        // psub, padd

    // 0F 38 xx: every opcode has a ModRM byte and no immediate.
    auto& m38 = t.map38;
    fill(m38, 0x00, 0x0B, M | PS_PD);        // pshufb phadd pmaddubsw phsub psign pmulhrsw (MMX and SSE)
    fill(m38, 0x10, 0x10, M | P66);          // pblendvb
    fill(m38, 0x14, 0x15, M | P66);          // blendvps blendvpd
    fill(m38, 0x17, 0x17, M | P66);          // ptest
    fill(m38, 0x1C, 0x1E, M | PS_PD);        // pabsb pabsw pabsd
    fill(m38, 0x20, 0x25, M | P66);          // pmovsx
    fill(m38, 0x28, 0x29, M | P66);          // pmuldq pcmpeqq
    fill(m38, 0x2A, 0x2A, M | P66 | MEM);    // movntdqa
    fill(m38, 0x2B, 0x2B, M | P66);          // packusdw
    fill(m38, 0x30, 0x35, M | P66);          // pmovzx
    fill(m38, 0x37, 0x41, M | P66);          // pcmpgtq, pmin/pmax, pmulld, phminposuw
    fill(m38, 0x80, 0x82, M | P66 | MEM);    // invept invvpid invpcid
    fill(m38, 0xC8, 0xCD, M | NP);           // sha1nexte sha1msg1 sha1msg2 sha256rnds2 sha256msg1 sha256msg2
    fill(m38, 0xCF, 0xCF, M | P66);          // gf2p8mulb
    fill(m38, 0xD8, 0xD8, M | PF3 | MEM | G_38D8);     // aesencwide128kl aesdecwide128kl aesencwide256kl aesdecwide256kl
    fill(m38, 0xDB, 0xDB, M | P66);          // aesimc
    fill(m38, 0xDC, 0xDC, M | P66 | PF3);    // aesenc; loadiwkey / aesenc128kl (Key Locker)
    fill(m38, 0xDD, 0xDF, M | P66 | PF3 | G_F3MEM);    // aesenclast aesdec aesdeclast; aesdec128kl aesenc256kl aesdec256kl
    fill(m38, 0xF0, 0xF1, M | PS_PD | PF2 | G_MOVBE);  // movbe; crc32
    fill(m38, 0xF5, 0xF5, M | P66 | MEM);    // wruss
    fill(m38, 0xF6, 0xF6, M | PS_PD | PF3 | G_38F6);   // wrss; adcx; adox
    fill(m38, 0xF8, 0xF8, M | P66 | PF3 | PF2 | MEM);  // movdir64b enqcmds enqcmd
    fill(m38, 0xF9, 0xF9, M | NP | MEM);     // movdiri
    fill(m38, 0xFA, 0xFB, M | PF3 | REG);    // encodekey128 encodekey256
    fill(m38, 0xFC, 0xFC, M | ALL4 | MEM);   // aadd aand axor aor (RAO-INT)

    // 0F 3A xx: every opcode has a ModRM byte and an 8-bit immediate.
    auto& m3a = t.map3a;
    fill(m3a, 0x08, 0x0E, M | I8 | P66);     // roundps roundpd roundss roundsd blendps blendpd pblendw
    fill(m3a, 0x0F, 0x0F, M | I8 | PS_PD);   // palignr
    fill(m3a, 0x14, 0x17, M | I8 | P66);     // pextrb pextrw pextrd/q extractps
    fill(m3a, 0x20, 0x22, M | I8 | P66);     // pinsrb insertps pinsrd/q
    fill(m3a, 0x40, 0x42, M | I8 | P66);     // dpps dppd mpsadbw
    fill(m3a, 0x44, 0x44, M | I8 | P66);     // pclmulqdq
    fill(m3a, 0x60, 0x63, M | I8 | P66);     // pcmpestrm pcmpestri pcmpistrm pcmpistri
    fill(m3a, 0xCC, 0xCC, M | I8 | NP);      // sha1rnds4
    fill(m3a, 0xCE, 0xCF, M | I8 | P66);     // gf2p8affineqb gf2p8affineinvqb
    fill(m3a, 0xDF, 0xDF, M | I8 | P66);     // aeskeygenassist
    fill(m3a, 0xF0, 0xF0, M | I8 | PF3 | G_C0);  // hreset
}

constexpr void build_vex(Tables& t) {
    // VEX map 1 (0F): the SSE / SSE2 instructions in their AVX form, and the opmask instructions.
    VexMap& v1 = t.vex[0];
    vfill(v1, 0x10, 0x12, kVexAll4);                          // vmovups/pd/ss/sd; vmovlps, vmovlpd, vmovsldup, vmovddup
    vfill(v1, 0x13, 0x15, kVexPsPd);                          // vmovlp store, vunpcklp, vunpckhp
    vfill(v1, 0x16, 0x16, kVexPsPd | kVexF3);                 // vmovhps, vmovhpd, vmovshdup
    vfill(v1, 0x17, 0x17, kVexPsPd);                          // vmovhp store
    vfill(v1, 0x28, 0x29, kVexPsPd);                          // vmovaps vmovapd
    vfill(v1, 0x2A, 0x2A, kVexF3 | kVexF2);                   // vcvtsi2ss vcvtsi2sd
    vfill(v1, 0x2B, 0x2B, kVexPsPd);                          // vmovntps vmovntpd
    vfill(v1, 0x2C, 0x2D, kVexF3 | kVexF2);                   // vcvt(t)ss2si vcvt(t)sd2si
    vfill(v1, 0x2E, 0x2F, kVexPsPd);                          // vucomis vcomis
    vfill(v1, 0x41, 0x42, kVexPsPd);                          // kand kandn
    vfill(v1, 0x44, 0x47, kVexPsPd);                          // knot kor kxnor kxor
    vfill(v1, 0x4A, 0x4B, kVexPsPd);                          // kadd kunpck
    vfill(v1, 0x50, 0x50, kVexPsPd);                          // vmovmskps vmovmskpd
    vfill(v1, 0x51, 0x51, kVexAll4);                          // vsqrt
    vfill(v1, 0x52, 0x53, kVexNone | kVexF3);                 // vrsqrt vrcp (ps, ss)
    vfill(v1, 0x54, 0x57, kVexPsPd);                          // vand vandn vor vxor
    vfill(v1, 0x58, 0x5A, kVexAll4);                          // vadd vmul vcvt
    vfill(v1, 0x5B, 0x5B, kVexPsPd | kVexF3);                 // vcvtdq2ps vcvtps2dq vcvttps2dq
    vfill(v1, 0x5C, 0x5F, kVexAll4);                          // vsub vmin vdiv vmax
    vfill(v1, 0x60, 0x6E, kVex66);                            // vpunpck, vpacks, vpcmpgt, vmovd/q
    vfill(v1, 0x6F, 0x6F, kVex66 | kVexF3);                   // vmovdqa vmovdqu
    vfill(v1, 0x70, 0x70, kVex66 | kVexF3 | kVexF2 | kVexImm8);  // vpshufd vpshufhw vpshuflw
    vfill(v1, 0x71, 0x73, kVex66 | kVexImm8);                 // shift-by-immediate groups
    vfill(v1, 0x74, 0x76, kVex66);                            // vpcmpeq
    vfill(v1, 0x77, 0x77, kVexNone | kVexNoModRm);            // vzeroupper, vzeroall: no ModRM
    vfill(v1, 0x7C, 0x7D, kVex66 | kVexF2);                   // vhaddp vhsubp
    vfill(v1, 0x7E, 0x7F, kVex66 | kVexF3);                   // vmovd/q, vmovq, vmovdqa/u stores
    vfill(v1, 0x90, 0x91, kVexPsPd);                          // kmov k, k/m; kmov m, k
    vfill(v1, 0x92, 0x93, kVexPsPd | kVexF2);                 // kmov k, r; kmov r, k
    vfill(v1, 0x98, 0x99, kVexPsPd);                          // kortest ktest
    vfill(v1, 0xAE, 0xAE, kVexNone);                          // vldmxcsr vstmxcsr
    vfill(v1, 0xC2, 0xC2, kVexAll4 | kVexImm8);               // vcmp
    vfill(v1, 0xC4, 0xC5, kVex66 | kVexImm8);                 // vpinsrw vpextrw
    vfill(v1, 0xC6, 0xC6, kVexPsPd | kVexImm8);               // vshufps vshufpd
    vfill(v1, 0xD0, 0xD0, kVex66 | kVexF2);                   // vaddsubpd vaddsubps
    vfill(v1, 0xD1, 0xE5, kVex66);                            // integer SSE2 (vpsrl ... vpmulhw), vmovq, vpmovmskb
    vfill(v1, 0xE6, 0xE6, kVex66 | kVexF3 | kVexF2);          // vcvttpd2dq vcvtdq2pd vcvtpd2dq
    vfill(v1, 0xE7, 0xEF, kVex66);                            // vmovntdq, vpsubs ... vpxor
    vfill(v1, 0xF0, 0xF0, kVexF2);                            // vlddqu
    vfill(v1, 0xF1, 0xFE, kVex66);                            // vpsll ... vpaddd, vmaskmovdqu

    // VEX map 2 (0F 38).
    VexMap& v2 = t.vex[1];
    vfill(v2, 0x00, 0x0F, kVex66);                            // SSSE3 (00..0B), vpermilps/pd, vtestps/pd
    vfill(v2, 0x13, 0x13, kVex66);                            // vcvtph2ps
    vfill(v2, 0x16, 0x1A, kVex66);                            // vpermps vptest vbroadcastss/sd/f128
    vfill(v2, 0x1C, 0x1E, kVex66);                            // vpabs
    vfill(v2, 0x20, 0x25, kVex66);                            // vpmovsx
    vfill(v2, 0x28, 0x2F, kVex66);                            // vpmuldq vpcmpeqq vmovntdqa vpackusdw vmaskmovps/pd
    vfill(v2, 0x30, 0x41, kVex66);                            // vpmovzx vpermd vpcmpgtq vpmin/vpmax vpmulld vphminposuw
    vfill(v2, 0x45, 0x47, kVex66);                            // vpsrlv vpsrav vpsllv
    vfill(v2, 0x50, 0x51, kVexAll4);                          // vpdpbusd(s) (AVX-VNNI); vpdpbuud(s) vpdpbsud(s) vpdpbssd(s) (INT8)
    vfill(v2, 0x52, 0x53, kVex66);                            // vpdpwssd(s)
    vfill(v2, 0x58, 0x5A, kVex66);                            // vpbroadcastd/q vbroadcasti128
    vfill(v2, 0x72, 0x72, kVexF3);                            // vcvtneps2bf16 (AVX-NE-CONVERT)
    vfill(v2, 0x78, 0x79, kVex66);                            // vpbroadcastb/w
    vfill(v2, 0x8C, 0x8C, kVex66);                            // vpmaskmov load
    vfill(v2, 0x8E, 0x8E, kVex66);                            // vpmaskmov store
    vfill(v2, 0x90, 0x93, kVex66);                            // gathers
    vfill(v2, 0x96, 0x9F, kVex66);                            // FMA 132
    vfill(v2, 0xA6, 0xAF, kVex66);                            // FMA 213
    vfill(v2, 0xB0, 0xB0, kVexAll4);                          // vcvtneoph2ps vcvtneeph2ps vcvtneebf162ps vcvtneobf162ps
    vfill(v2, 0xB1, 0xB1, kVex66 | kVexF3);                   // vbcstnesh2ps vbcstnebf162ps
    vfill(v2, 0xB4, 0xB5, kVex66);                            // vpmadd52luq vpmadd52huq (AVX-IFMA)
    vfill(v2, 0xB6, 0xBF, kVex66);                            // FMA 231
    vfill(v2, 0xCB, 0xCD, kVexF2);                            // vsha512rnds2 vsha512msg1 vsha512msg2
    vfill(v2, 0xCF, 0xCF, kVex66);                            // vgf2p8mulb
    vfill(v2, 0xD2, 0xD3, kVexNone | kVex66 | kVexF3);        // vpdpwuud(s) vpdpwusd(s) vpdpwsud(s) (AVX-VNNI-INT16)
    vfill(v2, 0xDA, 0xDA, kVexAll4);                          // vsm3msg1 vsm3msg2 vsm4key4 vsm4rnds4
    vfill(v2, 0xDB, 0xDF, kVex66);                            // vaesimc vaesenc vaesenclast vaesdec vaesdeclast
    vfill(v2, 0xF2, 0xF3, kVexNone);                          // andn; blsr / blsmsk / blsi
    vfill(v2, 0xF5, 0xF5, kVexNone | kVexF3 | kVexF2);        // bzhi pext pdep
    vfill(v2, 0xF6, 0xF6, kVexF2);                            // mulx
    vfill(v2, 0xF7, 0xF7, kVexAll4);                          // bextr shlx sarx shrx

    // VEX map 3 (0F 3A): every opcode has an 8-bit immediate (for the /is4 forms it holds a register number).
    VexMap& v3 = t.vex[2];
    const std::uint8_t i66 = kVex66 | kVexImm8;
    vfill(v3, 0x00, 0x02, i66);                               // vpermq vpermpd vpblendd
    vfill(v3, 0x04, 0x06, i66);                               // vpermilps vpermilpd vperm2f128
    vfill(v3, 0x08, 0x0F, i66);                               // vround vblendp vpblendw vpalignr
    vfill(v3, 0x14, 0x19, i66);                               // vpextr vextractps vinsertf128 vextractf128
    vfill(v3, 0x1D, 0x1D, i66);                               // vcvtps2ph
    vfill(v3, 0x20, 0x22, i66);                               // vpinsrb vinsertps vpinsrd/q
    vfill(v3, 0x30, 0x33, i66);                               // kshiftr kshiftl
    vfill(v3, 0x38, 0x39, i66);                               // vinserti128 vextracti128
    vfill(v3, 0x40, 0x42, i66);                               // vdpps vdppd vmpsadbw
    vfill(v3, 0x44, 0x44, i66);                               // vpclmulqdq
    vfill(v3, 0x46, 0x46, i66);                               // vperm2i128
    vfill(v3, 0x4A, 0x4C, i66);                               // vblendvps vblendvpd vpblendvb (/is4)
    vfill(v3, 0x60, 0x63, i66);                               // vpcmpestrm vpcmpestri vpcmpistrm vpcmpistri
    vfill(v3, 0xCE, 0xCF, i66);                               // vgf2p8affineqb vgf2p8affineinvqb
    vfill(v3, 0xDE, 0xDF, i66);                               // vsm3rnds2 vaeskeygenassist
    vfill(v3, 0xF0, 0xF0, kVexF2 | kVexImm8);                 // rorx

    // EVEX map 1 (0F).
    VexMap& e1 = t.evex[0];
    vfill(e1, 0x10, 0x12, kVexAll4);
    vfill(e1, 0x13, 0x15, kVexPsPd);
    vfill(e1, 0x16, 0x16, kVexPsPd | kVexF3);
    vfill(e1, 0x17, 0x17, kVexPsPd);
    vfill(e1, 0x28, 0x29, kVexPsPd);
    vfill(e1, 0x2A, 0x2A, kVexF3 | kVexF2);
    vfill(e1, 0x2B, 0x2B, kVexPsPd);
    vfill(e1, 0x2C, 0x2D, kVexF3 | kVexF2);
    vfill(e1, 0x2E, 0x2F, kVexPsPd);
    vfill(e1, 0x51, 0x51, kVexAll4);
    vfill(e1, 0x54, 0x57, kVexPsPd);
    vfill(e1, 0x58, 0x5A, kVexAll4);
    vfill(e1, 0x5B, 0x5B, kVexPsPd | kVexF3);
    vfill(e1, 0x5C, 0x5F, kVexAll4);
    vfill(e1, 0x60, 0x6E, kVex66);
    vfill(e1, 0x6F, 0x6F, kVex66 | kVexF3 | kVexF2);          // vmovdqa32/64, vmovdqu32/64, vmovdqu8/16
    vfill(e1, 0x70, 0x70, kVex66 | kVexF3 | kVexF2 | kVexImm8);
    vfill(e1, 0x71, 0x73, kVex66 | kVexImm8);                 // shifts and rotates by immediate
    vfill(e1, 0x74, 0x76, kVex66);
    vfill(e1, 0x78, 0x79, kVexAll4);                          // vcvt(t)ps/pd2udq, 2uqq, vcvt(t)ss/sd2usi
    vfill(e1, 0x7A, 0x7B, kVex66 | kVexF3 | kVexF2);          // vcvt(t)ps/pd2qq, vcvtu(dq|qq)2p(s|d), vcvtusi2ss/sd
    vfill(e1, 0x7E, 0x7E, kVex66 | kVexF3);
    vfill(e1, 0x7F, 0x7F, kVex66 | kVexF3 | kVexF2);
    vfill(e1, 0xC2, 0xC2, kVexAll4 | kVexImm8);
    vfill(e1, 0xC4, 0xC5, kVex66 | kVexImm8);
    vfill(e1, 0xC6, 0xC6, kVexPsPd | kVexImm8);
    vfill(e1, 0xD1, 0xD6, kVex66);
    vfill(e1, 0xD8, 0xE5, kVex66);
    vfill(e1, 0xE6, 0xE6, kVex66 | kVexF3 | kVexF2);
    vfill(e1, 0xE7, 0xEF, kVex66);
    vfill(e1, 0xF1, 0xF6, kVex66);
    vfill(e1, 0xF8, 0xFE, kVex66);

    // EVEX map 2 (0F 38). The F3 forms are the down-converting moves (vpmov*), the mask moves and vptestnm.
    VexMap& e2 = t.evex[1];
    vfill(e2, 0x00, 0x00, kVex66);                            // vpshufb
    vfill(e2, 0x04, 0x04, kVex66);                            // vpmaddubsw
    vfill(e2, 0x0B, 0x0D, kVex66);                            // vpmulhrsw vpermilps vpermilpd
    vfill(e2, 0x10, 0x15, kVex66 | kVexF3);                   // vpsrlvw vpsravw vpsllvw vcvtph2ps vprorv vprolv; vpmovus*
    vfill(e2, 0x16, 0x16, kVex66);                            // vpermps/pd
    vfill(e2, 0x18, 0x1F, kVex66);                            // vbroadcast*, vpabs
    vfill(e2, 0x20, 0x2A, kVex66 | kVexF3);                   // vpmovsx*, vptestm, vpmuldq, vpcmpeqq, vmovntdqa; vpmovs*, vptestnm, vpmovm2*, vpmov*2m, vpbroadcastmb2q
    vfill(e2, 0x2B, 0x2D, kVex66);                            // vpackusdw vscalef
    vfill(e2, 0x30, 0x35, kVex66 | kVexF3);                   // vpmovzx*; vpmov* (truncating)
    vfill(e2, 0x36, 0x37, kVex66);                            // vpermd/q vpcmpgtq
    vfill(e2, 0x38, 0x3A, kVex66 | kVexF3);                   // vpminsb vpminsd/q vpminuw; vpmovm2d/q, vpmovd2m/q2m, vpbroadcastmw2d
    vfill(e2, 0x3B, 0x40, kVex66);                            // vpminu, vpmax, vpmulld/q
    vfill(e2, 0x42, 0x47, kVex66);                            // vgetexp vplzcnt vpsrlv vpsrav vpsllv
    vfill(e2, 0x4C, 0x4F, kVex66);                            // vrcp14 vrsqrt14
    vfill(e2, 0x50, 0x51, kVex66);                            // vpdpbusd(s)
    vfill(e2, 0x52, 0x52, kVex66 | kVexF3);                   // vpdpwssd; vdpbf16ps
    vfill(e2, 0x53, 0x55, kVex66);                            // vpdpwssds vpopcnt
    vfill(e2, 0x58, 0x5B, kVex66);                            // vpbroadcastd/q, vbroadcasti*
    vfill(e2, 0x62, 0x66, kVex66);                            // vpexpandb/w vpcompressb/w vpblendm vblendm
    vfill(e2, 0x68, 0x68, kVexF2);                            // vp2intersect
    vfill(e2, 0x70, 0x71, kVex66);                            // vpshldv
    vfill(e2, 0x72, 0x72, kVex66 | kVexF3 | kVexF2);          // vpshrdvw; vcvtneps2bf16; vcvtne2ps2bf16
    vfill(e2, 0x73, 0x73, kVex66);                            // vpshrdv
    vfill(e2, 0x75, 0x7F, kVex66);                            // vpermi2*, vpbroadcast*, vpermt2*
    vfill(e2, 0x83, 0x83, kVex66);                            // vpmultishiftqb
    vfill(e2, 0x88, 0x8B, kVex66);                            // vexpand vpexpand vcompress vpcompress
    vfill(e2, 0x8D, 0x8D, kVex66);                            // vpermb/w
    vfill(e2, 0x8F, 0x8F, kVex66);                            // vpshufbitqmb
    vfill(e2, 0x90, 0x93, kVex66);                            // gathers
    vfill(e2, 0x96, 0x9F, kVex66);                            // FMA 132
    vfill(e2, 0xA0, 0xA3, kVex66);                            // scatters
    vfill(e2, 0xA6, 0xAF, kVex66);                            // FMA 213
    vfill(e2, 0xB4, 0xBF, kVex66);                            // vpmadd52luq vpmadd52huq, FMA 231
    vfill(e2, 0xC4, 0xC4, kVex66);                            // vpconflict
    vfill(e2, 0xCF, 0xCF, kVex66);                            // vgf2p8mulb
    vfill(e2, 0xDC, 0xDF, kVex66);                            // vaesenc vaesenclast vaesdec vaesdeclast

    // EVEX map 3 (0F 3A): every opcode has an 8-bit immediate.
    VexMap& e3 = t.evex[2];
    vfill(e3, 0x00, 0x01, i66);                               // vpermq vpermpd
    vfill(e3, 0x03, 0x05, i66);                               // valign vpermilps vpermilpd
    vfill(e3, 0x08, 0x0B, i66);                               // vrndscale
    vfill(e3, 0x0F, 0x0F, i66);                               // vpalignr
    vfill(e3, 0x14, 0x1B, i66);                               // vpextr vextractps vinsertf* vextractf*
    vfill(e3, 0x1D, 0x1F, i66);                               // vcvtps2ph vpcmpu vpcmp
    vfill(e3, 0x20, 0x23, i66);                               // vpinsr vinsertps vshuff*
    vfill(e3, 0x25, 0x27, i66);                               // vpternlog vgetmant
    vfill(e3, 0x38, 0x3B, i66);                               // vinserti* vextracti*
    vfill(e3, 0x3E, 0x3F, i66);                               // vpcmpub/uw vpcmpb/w
    vfill(e3, 0x42, 0x44, i66);                               // vdbpsadbw vshufi* vpclmulqdq
    vfill(e3, 0x50, 0x51, i66);                               // vrange
    vfill(e3, 0x54, 0x57, i66);                               // vfixupimm vreduce
    vfill(e3, 0x66, 0x67, i66);                               // vfpclass
    vfill(e3, 0x70, 0x73, i66);                               // vpshld vpshrd
    vfill(e3, 0xCE, 0xCF, i66);                               // vgf2p8affineqb vgf2p8affineinvqb
}

constexpr Tables build_tables() {
    Tables t;
    build_legacy(t);
    build_vex(t);
    return t;
}

constexpr Tables kTables = build_tables();

constexpr bool is_legacy_prefix(std::uint8_t b) noexcept {
    switch (b) {
        case 0x26: case 0x2E: case 0x36: case 0x3E:  // segment overrides (ignored in 64-bit mode)
        case 0x64: case 0x65:                        // fs, gs
        case 0x66: case 0x67:                        // operand size, address size
        case 0xF0: case 0xF2: case 0xF3:             // lock, repne, rep
            return true;
        default:
            return false;
    }
}

constexpr DecodeResult unknown() noexcept {
    return DecodeResult{DecodeStatus::Unknown, 0, 0, 0, 0};
}
constexpr DecodeResult truncated() noexcept {
    return DecodeResult{DecodeStatus::Truncated, 0, 0, 0, 0};
}

// ModRM, SIB and the displacement they call for (64-bit addressing; a 67 prefix selects 32-bit addressing, which has
// the same layout; a VSIB or an EVEX compressed displacement does not change it either).
struct ModRm {
    unsigned mod = 0;
    unsigned reg = 0;
    unsigned rm = 0;
    std::size_t disp_offset = 0;
    std::size_t disp_size = 0;
    bool rip_relative = false;
};

// Reads the ModRM byte at code[i] and the SIB byte if there is one, and leaves `i` behind the displacement (whose
// bytes are not read: the caller compares `i` with `available` at the end). False when the ModRM or SIB byte lies
// beyond `available`.
bool read_modrm(const std::uint8_t* code, std::size_t available, std::size_t& i, ModRm& out) noexcept {
    if (i >= available) {
        return false;
    }
    const std::uint8_t modrm = code[i++];
    out.mod = modrm >> 6;
    out.reg = (modrm >> 3) & 7u;
    out.rm = modrm & 7u;
    std::size_t displacement = 0;
    if (out.mod != 3) {
        unsigned sib_base = 0;
        if (out.rm == 4) {
            if (i >= available) {
                return false;
            }
            sib_base = code[i++] & 7u;
        }
        if (out.mod == 0) {
            if (out.rm == 5) {
                displacement = 4;  // [rip + disp32]
                out.rip_relative = true;
            } else if (out.rm == 4 && sib_base == 5) {
                displacement = 4;  // [index*scale + disp32], no base
            }
        } else if (out.mod == 1) {
            displacement = 1;
        } else {
            displacement = 4;
        }
    }
    if (displacement != 0) {
        out.disp_offset = i;
        out.disp_size = displacement;
    }
    i += displacement;
    return true;
}

DecodeResult finish(std::size_t length, std::size_t available, std::uint8_t flags, const ModRm& modrm) noexcept {
    if (length > 15) {
        return unknown();
    }
    if (length > available) {
        return truncated();
    }
    if (modrm.rip_relative) {
        flags = static_cast<std::uint8_t>(flags | kInsnRipRel);
    }
    return DecodeResult{DecodeStatus::Ok, static_cast<std::uint8_t>(length), flags,
                        static_cast<std::uint8_t>(modrm.disp_offset), static_cast<std::uint8_t>(modrm.disp_size)};
}

// A VEX (C4, C5) or EVEX (62) instruction; code[i] is that byte. In 64-bit mode these three bytes are never LES, LDS
// or BOUND, whatever follows them.
DecodeResult decode_vex(const std::uint8_t* code, std::size_t available, std::size_t i, std::uint8_t prefix_flags) noexcept {
    const std::uint8_t lead = code[i++];
    unsigned map = 1;
    unsigned pp = 0;
    bool evex = false;
    unsigned vector_length = 0;  // EVEX L'L
    if (lead == 0xC5) {
        // C5: R vvvv L pp. Always map 1.
        if (i >= available) {
            return truncated();
        }
        pp = code[i++] & 3u;
    } else if (lead == 0xC4) {
        // C4: R X B m-mmmm, then W vvvv L pp.
        if (i >= available) {
            return truncated();
        }
        map = code[i++] & 0x1Fu;
        if (map < 1 || map > 3) {
            return unknown();
        }
        if (i >= available) {
            return truncated();
        }
        pp = code[i++] & 3u;
    } else {
        // 62: R X B R' 0 0 m m, then W vvvv 1 pp, then z L'L b V' aaa. The two zero bits and the one bit are fixed in
        // the encodings this decoder covers (they select the maps 4..7 and the APX forms otherwise).
        evex = true;
        if (i >= available) {
            return truncated();
        }
        const std::uint8_t p0 = code[i++];
        map = p0 & 3u;
        if ((p0 & 0x0Cu) != 0 || map == 0) {
            return unknown();
        }
        if (i >= available) {
            return truncated();
        }
        const std::uint8_t p1 = code[i++];
        if ((p1 & 0x04u) == 0) {
            return unknown();
        }
        pp = p1 & 3u;
        if (i >= available) {
            return truncated();
        }
        const std::uint8_t p2 = code[i++];
        vector_length = (p2 >> 5) & 3u;
        const bool broadcast_or_rounding = (p2 & 0x10u) != 0;
        if ((p2 & 0x80u) != 0 && (p2 & 7u) == 0) {
            return unknown();  // zeroing without a mask register
        }
        if (vector_length == 3 && !broadcast_or_rounding) {
            return unknown();  // L'L = 3 exists only as a rounding mode
        }
    }
    if (i >= available) {
        return truncated();
    }
    const std::uint8_t opcode = code[i++];
    const std::uint8_t attributes = (evex ? kTables.evex : kTables.vex)[map - 1][opcode];
    if ((attributes & (1u << pp)) == 0) {
        return unknown();
    }

    ModRm modrm;
    if ((attributes & kVexNoModRm) == 0) {
        if (!read_modrm(code, available, i, modrm)) {
            return truncated();
        }
        if (evex && vector_length == 3 && modrm.mod != 3) {
            return unknown();  // a rounding mode needs register operands
        }
    }
    if ((attributes & kVexImm8) != 0) {
        i += 1;
    }
    return finish(i, available, prefix_flags, modrm);
}

}  // namespace

DecodeResult decode_instruction(const std::uint8_t* code, std::size_t available) noexcept {
    if (code == nullptr || available == 0) {
        return truncated();
    }

    // Prefixes. A REX prefix only counts when it is the last prefix: a legacy prefix after it
    // makes the processor ignore the REX.
    std::size_t i = 0;
    bool operand_size = false;
    bool lock = false;
    bool rex_seen = false;
    std::uint8_t last_rep = 0;      // the last F2 / F3 prefix: it selects the form of an SSE opcode
    std::uint8_t prefix_flags = 0;  // kInsnAddr32, kInsnSegFsGs
    std::uint8_t rex = 0;
    unsigned prefix_count = 0;
    for (;;) {
        if (i >= available) {
            return truncated();
        }
        const std::uint8_t b = code[i];
        if (is_legacy_prefix(b)) {
            operand_size = operand_size || b == 0x66;
            lock = lock || b == 0xF0;
            if (b == 0xF2 || b == 0xF3) {
                last_rep = b;
            } else if (b == 0x67) {
                prefix_flags = static_cast<std::uint8_t>(prefix_flags | kInsnAddr32);
            } else if (b == 0x64 || b == 0x65) {
                prefix_flags = static_cast<std::uint8_t>(prefix_flags | kInsnSegFsGs);
            }
            rex = 0;
        } else if ((b & 0xF0) == 0x40) {
            rex = b;
            rex_seen = true;
        } else {
            break;
        }
        ++i;
        if (++prefix_count >= 15) {
            return unknown();  // no instruction is 15 bytes of prefixes plus an opcode
        }
    }
    const bool rex_w = (rex & 0x08) != 0;

    // Opcode.
    std::uint32_t attributes = 0;
    const std::uint8_t opcode = code[i];
    if (opcode == 0xC4 || opcode == 0xC5 || opcode == 0x62) {
        // VEX / EVEX. 66, F2, F3, lock or a REX in front of one is #UD (SDM volume 2, 2.3.2 and 2.7.1); a segment
        // override or 67 is an ordinary prefix of the instruction.
        if (operand_size || lock || last_rep != 0 || rex_seen) {
            return unknown();
        }
        return decode_vex(code, available, i, prefix_flags);
    }
    ++i;
    if (opcode == 0x0F) {
        if (i >= available) {
            return truncated();
        }
        const std::uint8_t second = code[i++];
        if (second == 0x38 || second == 0x3A) {
            if (i >= available) {
                return truncated();
            }
            attributes = (second == 0x38 ? kTables.map38 : kTables.map3a)[code[i++]];
        } else {
            attributes = kTables.two[second];
        }
    } else {
        attributes = kTables.one[opcode];
    }
    if ((attributes & V) == 0) {
        return unknown();
    }
    // The form of an opcode that exists only with certain mandatory prefixes.
    const std::uint32_t form = last_rep == 0xF3 ? PF3 : last_rep == 0xF2 ? PF2 : operand_size ? P66 : NP;
    if ((attributes & kPrefixFormMask) != 0 && (attributes & form) == 0) {
        return unknown();
    }
    // 66 + rel32 is a rel16 branch on AMD and ignored on Intel; do not guess.
    if (operand_size && (attributes & I32) != 0 && (attributes & (CALL | JMP | JCC)) != 0) {
        return unknown();
    }

    std::uint8_t flags = prefix_flags;
    std::size_t immediate = 0;
    ModRm modrm;
    bool lockable = false;

    if ((attributes & M) != 0) {
        if (!read_modrm(code, available, i, modrm)) {
            return truncated();
        }
        const unsigned reg = modrm.reg;
        const bool memory = modrm.mod != 3;
        if ((memory && (attributes & REG) != 0) || (!memory && (attributes & MEM) != 0)) {
            return unknown();
        }
        lockable = memory && (attributes & LOCK) != 0;

        switch (attributes & kGroupMask) {
            case 0:
                break;
            case G_F6:
                if (reg <= 1) {
                    immediate += 1;
                }
                lockable = memory && (reg == 2 || reg == 3);  // not, neg
                break;
            case G_F7:
                if (reg <= 1) {
                    immediate += (operand_size && !rex_w) ? 2 : 4;
                }
                lockable = memory && (reg == 2 || reg == 3);
                break;
            case G_REG0:
                if (reg != 0) {
                    return unknown();  // xabort, xbegin, XOP...
                }
                break;
            case G_FE:
                if (reg > 1) {
                    return unknown();
                }
                lockable = memory;  // inc, dec
                break;
            case G_FF:
                if (reg == 2) {
                    flags = static_cast<std::uint8_t>(flags | kInsnCall);
                } else if (reg == 4) {
                    flags = static_cast<std::uint8_t>(flags | kInsnJmp);
                } else if (reg != 0 && reg != 1 && reg != 6) {
                    return unknown();  // far call/jmp (3, 5) and the invalid slot 7
                }
                lockable = memory && reg <= 1;
                break;
            case G_BA:
                if (reg < 4) {
                    return unknown();
                }
                lockable = memory && reg != 4;  // bts, btr, btc; not bt
                break;
            case G_ALU:
                lockable = memory && reg != 7;  // not cmp
                break;
            case G_X87: {
                const unsigned escape = opcode - 0xD8u;
                const bool defined = memory ? ((kTables.x87_memory[escape] >> reg) & 1u) != 0
                                            : ((kTables.x87_register[escape] >> ((reg << 3) | modrm.rm)) & 1u) != 0;
                if (!defined) {
                    return unknown();
                }
                break;
            }
            case G_8C:
                if (reg > 5) {
                    return unknown();
                }
                break;
            case G_8E:
                if (reg > 5 || reg == 1) {
                    return unknown();
                }
                break;
            case G_0F01: {
                if (memory) {
                    // sgdt sidt lgdt lidt smsw - lmsw invlpg; F3 /5 is rstorssp
                    if (reg == 5 && form != PF3) {
                        return unknown();
                    }
                    break;
                }
                if (reg == 4 || reg == 6) {
                    break;  // smsw / lmsw with a register
                }
                // The register forms are instructions on their own, named by the whole ModRM byte.
                const unsigned byte = 0xC0u | (reg << 3) | modrm.rm;
                // Whatever the prefix: the SVM instructions (D8..DF), swapgs rdtscp (F8 F9), clzero rdpru (FC FD).
                bool defined = (byte >= 0xD8 && byte <= 0xDF) || byte == 0xF8 || byte == 0xF9 || byte == 0xFC || byte == 0xFD;
                if (form == NP) {
                    // enclv vmcall vmlaunch vmresume vmxoff pconfig wrmsrns pbndkb (C0..C7), monitor mwait clac stac
                    // (C8..CB), encls (CF), xgetbv xsetbv (D0 D1), vmfunc xend xtest enclu (D4..D7), serialize (E8),
                    // rdpkru wrpkru (EE EF), monitorx mwaitx (FA FB), invlpgb tlbsync (FE FF).
                    defined = defined || (byte >= 0xC0 && byte <= 0xCB) || byte == 0xCF || byte == 0xD0 || byte == 0xD1 ||
                              (byte >= 0xD4 && byte <= 0xD7) || byte == 0xE8 || byte == 0xEE || byte == 0xEF ||
                              byte == 0xFA || byte == 0xFB || byte == 0xFE || byte == 0xFF;
                } else if (form == P66) {
                    defined = defined || (byte >= 0xCC && byte <= 0xCF);  // tdcall seamret seamops seamcall
                } else if (form == PF3) {
                    // wrmsrlist (C6), setssbsy (E8), saveprevssp (EA), uiret testui clui stui (EC..EF), mcommit (FA),
                    // rmpadjust psmash (FE FF)
                    defined = defined || byte == 0xC6 || byte == 0xE8 || byte == 0xEA || (byte >= 0xEC && byte <= 0xEF) ||
                              byte == 0xFA || byte == 0xFE || byte == 0xFF;
                } else {
                    // rdmsrlist (C6), xsusldtrk xresldtrk (E8 E9), rmpupdate pvalidate (FE FF)
                    defined = defined || byte == 0xC6 || byte == 0xE8 || byte == 0xE9 || byte == 0xFE || byte == 0xFF;
                }
                if (!defined) {
                    return unknown();
                }
                break;
            }
            case G_0FAE: {
                bool defined = false;
                if (memory) {
                    // No prefix: fxsave fxrstor ldmxcsr stmxcsr xsave xrstor xsaveopt clflush. 66: clwb (6),
                    // clflushopt (7). F3: ptwrite (4), clrssbsy (6).
                    defined = form == NP || (form == P66 && reg >= 6) || (form == PF3 && (reg == 4 || reg == 6));
                } else if (form == NP) {
                    defined = reg >= 5;  // lfence (5), mfence (6), sfence (7)
                } else if (form == PF3) {
                    defined = reg <= 6;  // rdfsbase rdgsbase wrfsbase wrgsbase (0..3), ptwrite, incssp, umonitor
                } else {
                    defined = reg == 6 || (form == P66 && reg == 7);  // 66: tpause, pcommit; F2: umwait
                }
                if (!defined) {
                    return unknown();
                }
                break;
            }
            case G_0FC7:
                if (memory) {
                    // cmpxchg8b / cmpxchg16b (1); xrstors (3), xsavec (4), xsaves (5), vmptrld (6), vmptrst (7) without
                    // a prefix; vmclear (66 /6), vmxon (F3 /6)
                    if (!(reg == 1 || (form == NP && reg >= 3) || (reg == 6 && (form == P66 || form == PF3)))) {
                        return unknown();
                    }
                    lockable = reg == 1;
                } else if (!((reg == 6 || reg == 7) && form != PF2)) {
                    return unknown();  // rdrand (6), rdseed (7); F3: senduipi (6), rdpid (7)
                }
                break;
            case G_SH71:
                if (memory || !(reg == 2 || reg == 4 || reg == 6)) {
                    return unknown();
                }
                break;
            case G_SH73:
                if (memory || !(reg == 2 || reg == 6 || (form == P66 && (reg == 3 || reg == 7)))) {
                    return unknown();
                }
                break;
            case G_66MEM:
                if (form == P66 && !memory) {
                    return unknown();
                }
                break;
            case G_D6:
                if (form != P66 && memory) {
                    return unknown();
                }
                break;
            case G_MOVBE:
                if (form != PF2 && !memory) {
                    return unknown();
                }
                break;
            case G_38F6:
                if (form == NP && !memory) {
                    return unknown();
                }
                break;
            case G_38D8:
                if (reg > 3) {
                    return unknown();
                }
                break;
            case G_F3MEM:
                if (form == PF3 && !memory) {
                    return unknown();
                }
                break;
            case G_C0:
                if (memory || reg != 0 || modrm.rm != 0) {
                    return unknown();
                }
                break;
            default:
                return unknown();
        }
    }
    // A lock prefix is #UD except on the read-modify-write instructions with a memory destination.
    if (lock && !lockable) {
        return unknown();
    }

    // Immediates.
    if ((attributes & I8) != 0) {
        immediate += 1;
    }
    if ((attributes & I16) != 0) {
        immediate += 2;
    }
    if ((attributes & IZ) != 0) {
        immediate += (operand_size && !rex_w) ? 2 : 4;
    }
    if ((attributes & IV) != 0) {
        immediate += rex_w ? 8 : (operand_size ? 2 : 4);
    }
    if ((attributes & I32) != 0) {
        immediate += 4;
    }
    i += immediate;

    if ((attributes & CALL) != 0) flags = static_cast<std::uint8_t>(flags | kInsnCall);
    if ((attributes & JMP) != 0) flags = static_cast<std::uint8_t>(flags | kInsnJmp);
    if ((attributes & JCC) != 0) flags = static_cast<std::uint8_t>(flags | kInsnJcc);
    if ((attributes & RET) != 0) flags = static_cast<std::uint8_t>(flags | kInsnRet);
    if ((attributes & TRAP) != 0) flags = static_cast<std::uint8_t>(flags | kInsnTrap);

    return finish(i, available, flags, modrm);
}

}  // namespace cml::agent::hook
