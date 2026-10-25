#include "length_decoder.h"

#include <array>

namespace cml::agent::hook {

namespace {

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

// Opcode groups: the ModRM.reg field selects the instruction, which changes validity and the
// immediate. Stored in bits 12..14.
constexpr unsigned kGroupShift = 12;
constexpr std::uint32_t kGroupMask = 7u << kGroupShift;
constexpr std::uint32_t G_F6 = 1u << kGroupShift;    // test Eb,Ib has an imm8 (reg 0,1); others none
constexpr std::uint32_t G_F7 = 2u << kGroupShift;    // test Ev,Iz has an immediate (reg 0,1); others none
constexpr std::uint32_t G_REG0 = 3u << kGroupShift;  // only reg == 0 is covered (mov Eb/Ev,imm; pop Ev)
constexpr std::uint32_t G_FE = 4u << kGroupShift;    // inc/dec Eb: reg 0,1
constexpr std::uint32_t G_FF = 5u << kGroupShift;    // inc, dec, call, jmp, push: reg 0,1,2,4,6
constexpr std::uint32_t G_BA = 6u << kGroupShift;    // bt/bts/btr/btc Ev,Ib: reg 4..7

struct Tables {
    std::array<std::uint32_t, 256> one{};  // one-byte opcode map
    std::array<std::uint32_t, 256> two{};  // 0F xx map
};

constexpr void fill(std::array<std::uint32_t, 256>& table, unsigned lo, unsigned hi, std::uint32_t attributes) {
    for (unsigned i = lo; i <= hi; ++i) {
        table[i] = V | attributes;
    }
}

constexpr Tables build_tables() {
    Tables t;
    auto& one = t.one;
    auto& two = t.two;

    // 00..3F: the eight ALU operations (add or adc sbb and sub xor cmp), each as
    // Eb,Gb / Ev,Gv / Gb,Eb / Gv,Ev / AL,Ib / eAX,Iz. The remaining slots of those rows are
    // prefixes (26 2E 36 3E, handled before the lookup), the 0F escape or invalid in 64-bit mode.
    for (unsigned base = 0; base < 0x40; base += 8) {
        fill(one, base, base + 3, M);
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
    fill(one, 0x80, 0x80, M | I8);     // group 1 Eb,Ib
    fill(one, 0x81, 0x81, M | IZ);     // group 1 Ev,Iz
    fill(one, 0x83, 0x83, M | I8);     // group 1 Ev,Ib
    fill(one, 0x84, 0x8E, M);          // test xchg mov (88..8B) mov sreg (8C, 8E) lea (8D)
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
    fill(one, 0xD8, 0xDF, M);          // x87 escape: always a ModRM byte
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

    // 0F xx
    fill(two, 0x05, 0x05, 0);          // syscall
    fill(two, 0x0B, 0x0B, TRAP);       // ud2
    fill(two, 0x0D, 0x0D, M);          // prefetchw
    fill(two, 0x01, 0x01, M);          // group 7 (xgetbv, rdtscp, ... all have ModRM)
    fill(two, 0x10, 0x17, M);          // movups movss movlps unpck movhps ...
    fill(two, 0x18, 0x1F, M);          // prefetch hints, nop Ev (0F 1F), endbr64 (F3 0F 1E FA)
    fill(two, 0x28, 0x2F, M);          // movaps cvt* ucomiss comiss
    fill(two, 0x31, 0x31, 0);          // rdtsc
    fill(two, 0x40, 0x4F, M);          // cmovcc
    fill(two, 0x50, 0x6F, M);          // SSE / MMX
    fill(two, 0x70, 0x73, M | I8);     // pshufw/pshufd/..., shift-by-immediate groups
    fill(two, 0x74, 0x76, M);          // pcmpeq
    fill(two, 0x77, 0x77, 0);          // emms
    fill(two, 0x7C, 0x7F, M);          // haddps, movd/movq, movdqa/movdqu
    fill(two, 0x80, 0x8F, I32 | JCC);  // jcc rel32
    fill(two, 0x90, 0x9F, M);          // setcc
    fill(two, 0xA2, 0xA2, 0);          // cpuid
    fill(two, 0xA3, 0xA3, M);          // bt
    fill(two, 0xA4, 0xA4, M | I8);     // shld imm8
    fill(two, 0xA5, 0xA5, M);
    fill(two, 0xAB, 0xAB, M);          // bts
    fill(two, 0xAC, 0xAC, M | I8);     // shrd imm8
    fill(two, 0xAD, 0xAF, M);          // shrd cl, group 15 (fences, ldmxcsr), imul
    fill(two, 0xB0, 0xB1, M);          // cmpxchg
    fill(two, 0xB3, 0xB3, M);          // btr
    fill(two, 0xB6, 0xB8, M);          // movzx, popcnt
    fill(two, 0xBA, 0xBA, M | I8 | G_BA);
    fill(two, 0xBB, 0xBF, M);          // btc, bsf, bsr, movsx
    fill(two, 0xC0, 0xC1, M);          // xadd
    fill(two, 0xC2, 0xC2, M | I8);     // cmpps
    fill(two, 0xC3, 0xC3, M);          // movnti
    fill(two, 0xC4, 0xC6, M | I8);     // pinsrw, pextrw, shufps
    fill(two, 0xC7, 0xC7, M);          // group 9 (cmpxchg8b/16b, rdrand)
    fill(two, 0xC8, 0xCF, 0);          // bswap
    fill(two, 0xD0, 0xFE, M);          // MMX / SSE2 integer
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
    return DecodeResult{DecodeStatus::Unknown, 0, 0};
}
constexpr DecodeResult truncated() noexcept {
    return DecodeResult{DecodeStatus::Truncated, 0, 0};
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
    std::uint8_t rex = 0;
    unsigned prefix_count = 0;
    for (;;) {
        if (i >= available) {
            return truncated();
        }
        const std::uint8_t b = code[i];
        if (is_legacy_prefix(b)) {
            operand_size = operand_size || b == 0x66;
            rex = 0;
        } else if ((b & 0xF0) == 0x40) {
            rex = b;
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
    const std::uint8_t opcode = code[i++];
    if (opcode == 0x0F) {
        if (i >= available) {
            return truncated();
        }
        const std::uint8_t second = code[i++];
        if (second == 0x38 || second == 0x3A) {
            // Three-byte maps: every defined opcode has a ModRM byte; the 0F 3A map also has an
            // imm8 on all of them.
            if (i >= available) {
                return truncated();
            }
            ++i;
            attributes = V | M | (second == 0x3A ? I8 : 0u);
        } else {
            attributes = kTables.two[second];
        }
    } else {
        attributes = kTables.one[opcode];
    }
    if ((attributes & V) == 0) {
        return unknown();
    }
    // 66 + rel32 is a rel16 branch on AMD and ignored on Intel; do not guess.
    if (operand_size && (attributes & I32) != 0 && (attributes & (CALL | JMP | JCC)) != 0) {
        return unknown();
    }

    std::uint8_t flags = 0;
    std::size_t immediate = 0;

    // ModRM, SIB, displacement (64-bit addressing; a 67 prefix selects 32-bit addressing, which has
    // the same ModRM/SIB/displacement layout).
    if ((attributes & M) != 0) {
        if (i >= available) {
            return truncated();
        }
        const std::uint8_t modrm = code[i++];
        const unsigned mod = modrm >> 6;
        const unsigned reg = (modrm >> 3) & 7u;
        const unsigned rm = modrm & 7u;

        std::size_t displacement = 0;
        if (mod != 3) {
            unsigned sib_base = 0;
            if (rm == 4) {
                if (i >= available) {
                    return truncated();
                }
                sib_base = code[i++] & 7u;
            }
            if (mod == 0) {
                if (rm == 5) {
                    displacement = 4;  // [rip + disp32]
                    flags = static_cast<std::uint8_t>(flags | kInsnRipRel);
                } else if (rm == 4 && sib_base == 5) {
                    displacement = 4;  // [index*scale + disp32], no base
                }
            } else if (mod == 1) {
                displacement = 1;
            } else {
                displacement = 4;
            }
        }
        i += displacement;

        switch (attributes & kGroupMask) {
            case 0:
                break;
            case G_F6:
                if (reg <= 1) {
                    immediate += 1;
                }
                break;
            case G_F7:
                if (reg <= 1) {
                    immediate += (operand_size && !rex_w) ? 2 : 4;
                }
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
                break;
            case G_FF:
                if (reg == 2) {
                    flags = static_cast<std::uint8_t>(flags | kInsnCall);
                } else if (reg == 4) {
                    flags = static_cast<std::uint8_t>(flags | kInsnJmp);
                } else if (reg != 0 && reg != 1 && reg != 6) {
                    return unknown();  // far call/jmp (3, 5) and the invalid slot 7
                }
                break;
            case G_BA:
                if (reg < 4) {
                    return unknown();
                }
                break;
            default:
                return unknown();
        }
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

    if (i > 15) {
        return unknown();
    }
    if (i > available) {
        return truncated();
    }

    if ((attributes & CALL) != 0) flags = static_cast<std::uint8_t>(flags | kInsnCall);
    if ((attributes & JMP) != 0) flags = static_cast<std::uint8_t>(flags | kInsnJmp);
    if ((attributes & JCC) != 0) flags = static_cast<std::uint8_t>(flags | kInsnJcc);
    if ((attributes & RET) != 0) flags = static_cast<std::uint8_t>(flags | kInsnRet);
    if ((attributes & TRAP) != 0) flags = static_cast<std::uint8_t>(flags | kInsnTrap);

    return DecodeResult{DecodeStatus::Ok, static_cast<std::uint8_t>(i), flags};
}

}  // namespace cml::agent::hook
