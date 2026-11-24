// cml_lde_diff: the hook installer's length decoder against the reference disassembler (WP-38, ADR-0042 points 3
// and 4). Built only in a tree configured with CML_ZYDIS_DIR (.github/workflows/fuzz.yml); lde_reference.h has the
// comparison rule and what counts as an exception.
//
//   cml_lde_diff sweep <image.exe|dll>... [--min-instructions N] [--min-vex N] [--min-evex N]
//       A linear sweep from every function entry of each module (the functions come from the module's PDB: every
//       S_GPROC32 / S_LPROC32 and the function publics; the bytes from the image file). Each function is decoded
//       instruction by instruction to its end (the PDB's code length, else the next function entry) and every
//       position is compared. Where the decoder says Unknown the reference's length carries the sweep on, so one
//       uncovered instruction does not hide the rest of a function.
//         WP38-DIFF-MODULE <file> functions=F instructions=N vex=V evex=E unknown=U exceptions=X mismatches=M
//         WP38-DIFF instructions=N mismatches=M modules=K functions=F vex=V evex=E unknown=U exceptions=X
//       `instructions` counts the positions where the decoder said Ok and the reference was asked. `unknown` counts the
//       positions the decoder refused although the reference decodes them (what the decoder does not cover; the most
//       frequent ones are listed as WP38-DIFF-UNKNOWN). --min-* turn a sweep that saw too little into a failure.
//
//   cml_lde_diff raw <file>... [--min-instructions N] [--min-vex N] [--min-evex N]
//       The same sweep over files that are nothing but code (a .text section dumped with objcopy, for hosts without
//       PE and PDB files): from offset 0, and where neither decoder knows the bytes one byte further.
//
//   cml_lde_diff enumerate [--seed N]
//       A structured walk over the encoding space instead of real code: every legacy opcode of the four legacy maps
//       under a set of prefix combinations, every VEX and EVEX map / pp / opcode with every ModRM byte and varied
//       payload bits, and prefixes in front of VEX / EVEX. Deterministic for a seed.
//         WP38-ENUM-EXCEPTIONS (...) DECODING_ERROR=a BAD_REGISTER=b INVALID_MASK=c ...
//         WP38-ENUM cases=N ok=A agree=B exceptions=X mismatches=M unknown_reference_ok=U
//
// Exit code: 0; 1 a module or its PDB cannot be read; 2 bad command line; 3 at least one mismatch; 4 a --min-* bound.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cml/pdb/pdb.h"
#include "cml/pe/pe.h"
#include "lde_reference.h"

namespace {

using cml::agent::hook::DecodeStatus;
using cml::fuzz::lde::Comparison;
using cml::fuzz::lde::Reference;
using cml::fuzz::lde::Verdict;

std::string hex_bytes(const std::uint8_t* bytes, std::size_t count) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            s += ' ';
        }
        s += kDigits[bytes[i] >> 4];
        s += kDigits[bytes[i] & 15];
    }
    return s;
}

struct Totals {
    std::size_t functions = 0;
    std::size_t instructions = 0;
    std::size_t vex = 0;
    std::size_t evex = 0;
    std::size_t unknown = 0;
    std::size_t exceptions = 0;
    std::size_t mismatches = 0;
};

std::size_t g_mismatches_printed = 0;

void print_mismatch(const char* where, const Comparison& c, const std::uint8_t* bytes, std::size_t available) {
    if (g_mismatches_printed++ >= 100) {
        return;
    }
    std::printf("WP38-MISMATCH %s: %s; ours=%s/%u reference=%s/%u bytes=%s\n", where, c.why,
                c.ours.status == DecodeStatus::Ok ? "ok" : c.ours.status == DecodeStatus::Unknown ? "unknown" : "truncated",
                static_cast<unsigned>(c.ours.length), c.reference_ok ? "ok" : cml::fuzz::lde::status_name(c.reference_status),
                c.reference_length, hex_bytes(bytes, std::min<std::size_t>(available, 16)).c_str());
}

// ---- sweep ----------------------------------------------------------------------------------------------------------

struct Function {
    std::string name;
    std::uint32_t size = 0;  // 0 = not known
};

// One run of code: instruction by instruction until its end, a mismatch, or bytes neither decoder knows (then
// `resync` decides between stopping and trying one byte further).
void sweep_range(const Reference& reference, const std::uint8_t* code, std::size_t length, const std::string& where, bool resync,
                 Totals& t, std::map<std::string, std::size_t>& unknown_kinds) {
    std::size_t at = 0;
    while (at < length) {
        const Comparison c = reference.compare(code + at, length - at);
        if (c.verdict == Verdict::Mismatch) {
            ++t.mismatches;
            const std::string here = where + "+0x" + std::to_string(at);
            print_mismatch(here.c_str(), c, code + at, length - at);
            if (!resync) {
                return;
            }
            at += c.reference_ok ? c.reference_length : 1;
            continue;
        }
        if (c.ours.status == DecodeStatus::Ok) {
            ++t.instructions;
            if (c.vector_encoded) {
                const std::uint8_t* p = code + at;
                while (*p != 0xC4 && *p != 0xC5 && *p != 0x62) {
                    ++p;
                }
                ++(*p == 0x62 ? t.evex : t.vex);
            }
            if (c.verdict == Verdict::Exception) {
                ++t.exceptions;
            }
            at += c.ours.length;
            continue;
        }
        if (c.ours.status == DecodeStatus::Unknown && c.reference_ok) {
            ++t.unknown;
            ++unknown_kinds[hex_bytes(code + at, std::min<std::size_t>(c.reference_length, 4))];
            at += c.reference_length;
            continue;
        }
        if (!resync) {
            return;  // the function's bytes end inside an instruction, or neither decoder knows what this is
        }
        ++at;
    }
}

int sweep_module(const Reference& reference, const std::string& path, Totals& grand, std::map<std::string, std::size_t>& unknown_kinds) {
    auto owned = cml::pe::load_image_file(path);
    if (!owned) {
        std::fprintf(stderr, "%s\n", owned.error().format().c_str());
        return 1;
    }
    auto index = cml::pdb::PdbIndex::open_for_pe(path);
    if (!index) {
        std::fprintf(stderr, "%s\n", index.error().format().c_str());
        return 1;
    }
    const cml::pdb::PdbIndex& pdb = **index;
    const cml::pe::Image& image = owned->image;

    std::map<std::uint32_t, Function> by_rva;
    for (std::uint32_t c = 0; c < pdb.compiland_count(); ++c) {
        auto symbols = pdb.compiland_symbols(c);
        if (!symbols) {
            continue;
        }
        for (const cml::pdb::SymbolView& s : *symbols) {
            if (cml::pdb::is_proc_kind(s.kind) && s.rva != cml::pdb::kNoRva) {
                by_rva.emplace(s.rva, Function{std::string(s.name), s.size});
            }
        }
    }
    if (auto publics = pdb.all_publics()) {
        for (const cml::pdb::SymbolView& s : *publics) {
            if (s.is_function() && s.rva != cml::pdb::kNoRva) {
                by_rva.emplace(s.rva, Function{std::string(s.name), 0});
            }
        }
    }

    Totals t;
    for (auto it = by_rva.begin(); it != by_rva.end(); ++it) {
        const std::uint32_t entry = it->first;
        const cml::pe::SectionHeader* section = image.section_for_rva(entry);
        if (section == nullptr) {
            continue;
        }
        // The bytes of the function: to its recorded end, else to the next entry; never beyond the section's file data.
        const std::uint32_t section_end = section->virtual_address + std::min(section->virtual_size, section->size_of_raw_data);
        std::uint32_t end = section_end;
        const auto next = std::next(it);
        if (it->second.size != 0) {
            end = static_cast<std::uint32_t>(std::min<std::uint64_t>(section_end, std::uint64_t{entry} + it->second.size));
        } else if (next != by_rva.end()) {
            end = std::min(section_end, next->first);
        }
        if (end <= entry) {
            continue;
        }
        auto span = image.read_rva(entry, end - entry);
        if (!span) {
            continue;
        }
        ++t.functions;
        sweep_range(reference, reinterpret_cast<const std::uint8_t*>(span->data()), span->size(), path + " " + it->second.name, false, t,
                    unknown_kinds);
    }
    std::printf("WP38-DIFF-MODULE %s functions=%zu instructions=%zu vex=%zu evex=%zu unknown=%zu exceptions=%zu mismatches=%zu\n",
                path.c_str(), t.functions, t.instructions, t.vex, t.evex, t.unknown, t.exceptions, t.mismatches);
    grand.functions += t.functions;
    grand.instructions += t.instructions;
    grand.vex += t.vex;
    grand.evex += t.evex;
    grand.unknown += t.unknown;
    grand.exceptions += t.exceptions;
    grand.mismatches += t.mismatches;
    return 0;
}

int sweep_raw(const Reference& reference, const std::string& path, Totals& grand, std::map<std::string, std::size_t>& unknown_kinds) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cml_lde_diff: cannot open %s\n", path.c_str());
        return 1;
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::vector<std::uint8_t> bytes(raw.begin(), raw.end());
    Totals t;
    t.functions = 1;
    sweep_range(reference, bytes.data(), bytes.size(), path, true, t, unknown_kinds);
    std::printf("WP38-DIFF-MODULE %s functions=%zu instructions=%zu vex=%zu evex=%zu unknown=%zu exceptions=%zu mismatches=%zu\n",
                path.c_str(), t.functions, t.instructions, t.vex, t.evex, t.unknown, t.exceptions, t.mismatches);
    grand.functions += t.functions;
    grand.instructions += t.instructions;
    grand.vex += t.vex;
    grand.evex += t.evex;
    grand.unknown += t.unknown;
    grand.exceptions += t.exceptions;
    grand.mismatches += t.mismatches;
    return 0;
}

// ---- enumerate ------------------------------------------------------------------------------------------------------

struct Rng {  // xorshift64*: the same sequence on every platform
    std::uint64_t state;
    std::uint64_t next() noexcept {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }
};

struct EnumTotals {
    std::size_t cases = 0;
    std::size_t ok = 0;
    std::size_t agree = 0;
    std::size_t exceptions = 0;
    std::size_t mismatches = 0;
    std::size_t unknown_reference_ok = 0;
    std::map<std::string, std::size_t> exception_status;  // what Zydis said about the encodings counted as exceptions
};

void enumerate_case(const Reference& reference, const std::uint8_t* bytes, std::size_t size, EnumTotals& t) {
    ++t.cases;
    const Comparison c = reference.compare(bytes, size);
    if (c.ours.status == DecodeStatus::Ok) {
        ++t.ok;
    } else if (c.ours.status == DecodeStatus::Unknown && c.reference_ok) {
        ++t.unknown_reference_ok;
    }
    switch (c.verdict) {
        case Verdict::Agree: ++t.agree; break;
        case Verdict::Exception:
            ++t.exceptions;
            ++t.exception_status[cml::fuzz::lde::status_name(c.reference_status)];
            break;
        case Verdict::Mismatch:
            ++t.mismatches;
            print_mismatch("enumerate", c, bytes, size);
            break;
        case Verdict::NotCompared: break;
    }
}

int enumerate(const Reference& reference, std::uint64_t seed) {
    Rng rng{seed * 0x9E3779B97F4A7C15ull + 0x1234567ull};
    EnumTotals t;
    std::uint8_t buffer[32];
    const auto fill_from = [&](std::size_t from) {
        for (std::size_t i = from; i < sizeof(buffer); ++i) {
            buffer[i] = static_cast<std::uint8_t>(rng.next() >> 32);
        }
    };

    // 1. Legacy maps: prefix set x map x opcode x ModRM byte, the rest random (SIB, displacement, immediates).
    static const std::vector<std::vector<std::uint8_t>> kPrefixes = {
        {},           {0x66},       {0xF2},       {0xF3},       {0xF0},             {0x67},       {0x48},       {0x41},
        {0x4F},       {0x40},       {0x2E},       {0x64},       {0x65},             {0x66, 0x48}, {0xF3, 0x48}, {0xF2, 0x48},
        {0xF0, 0x48}, {0x66, 0xF2}, {0x66, 0xF3}, {0xF2, 0x66}, {0xF3, 0x66},       {0xF0, 0x66}, {0xF0, 0xF2}, {0xF0, 0xF3},
        {0xF2, 0xF3}, {0xF3, 0xF2}, {0x48, 0x66}, {0x48, 0xF3}, {0x67, 0x65},       {0x65, 0x2E}, {0x66, 0x66}, {0x67, 0xF0, 0x48},
        {0x66, 0x2E}, {0x3E},       {0x26, 0x36}, {0xF2, 0x41}, {0x66, 0x67, 0x48}, {0x64, 0xF0}, {0x4C, 0x48}, {0x66, 0x4D},
    };
    for (const std::vector<std::uint8_t>& prefixes : kPrefixes) {
        for (unsigned map = 0; map < 4; ++map) {
            for (unsigned opcode = 0; opcode < 256; ++opcode) {
                for (unsigned modrm = 0; modrm < 256; ++modrm) {
                    for (unsigned repeat = 0; repeat < 2; ++repeat) {
                        std::size_t i = 0;
                        for (const std::uint8_t p : prefixes) {
                            buffer[i++] = p;
                        }
                        if (map >= 1) {
                            buffer[i++] = 0x0F;
                        }
                        if (map == 2) {
                            buffer[i++] = 0x38;
                        }
                        if (map == 3) {
                            buffer[i++] = 0x3A;
                        }
                        buffer[i++] = static_cast<std::uint8_t>(opcode);
                        buffer[i++] = static_cast<std::uint8_t>(modrm);
                        fill_from(i);
                        // Most cases see all 32 bytes; some a window that may cut the instruction.
                        const std::size_t size = repeat == 0 ? sizeof(buffer) : 1 + (rng.next() >> 33) % 18;
                        enumerate_case(reference, buffer, size, t);
                    }
                }
            }
        }
    }

    // 2. VEX (every 5-bit map number) and EVEX (every 3-bit map number): pp x opcode x ModRM byte, the other payload
    //    bits random, sometimes a segment or address-size prefix in front, sometimes a reserved payload bit flipped.
    for (unsigned evex = 0; evex < 2; ++evex) {
        const unsigned maps = evex != 0 ? 8u : 32u;
        const unsigned repeats = evex != 0 ? 24u : 12u;
        for (unsigned map = 0; map < maps; ++map) {
            for (unsigned pp = 0; pp < 4; ++pp) {
                for (unsigned opcode = 0; opcode < 256; ++opcode) {
                    for (unsigned modrm = 0; modrm < 256; ++modrm) {
                        for (unsigned repeat = 0; repeat < repeats; ++repeat) {
                            std::uint64_t r = rng.next();
                            std::size_t i = 0;
                            if ((r & 0x1F) == 0) {
                                buffer[i++] = 0x67;
                            }
                            if ((r & 0x3E0) == 0) {
                                buffer[i++] = static_cast<std::uint8_t>((r & 0x400) != 0 ? 0x65 : 0x2E);
                            }
                            r >>= 11;
                            if (evex != 0) {
                                buffer[i++] = 0x62;
                                std::uint8_t p0 = static_cast<std::uint8_t>(((r & 0xF) << 4) | map);  // R X B R' 0 m m m
                                if (((r >> 4) & 0x3F) == 0) {
                                    p0 |= 0x08;
                                }
                                r >>= 10;
                                std::uint8_t p1 = static_cast<std::uint8_t>(((r & 0x1F) << 3) | 0x04 | pp);  // W vvvv 1 pp
                                if (((r >> 5) & 0x3F) == 0) {
                                    p1 &= static_cast<std::uint8_t>(~0x04u);
                                }
                                r >>= 11;
                                buffer[i++] = p0;
                                buffer[i++] = p1;
                                buffer[i++] = static_cast<std::uint8_t>(r);  // z L'L b V' aaa
                            } else if (map == 1 && (r & 1) != 0) {
                                buffer[i++] = 0xC5;
                                buffer[i++] = static_cast<std::uint8_t>(((r >> 1) & 0xFC) | pp);  // R vvvv L pp
                            } else {
                                buffer[i++] = 0xC4;
                                buffer[i++] = static_cast<std::uint8_t>((((r >> 1) & 7) << 5) | map);  // R X B m-mmmm
                                buffer[i++] = static_cast<std::uint8_t>(((r >> 4) & 0xFC) | pp);       // W vvvv L pp
                            }
                            buffer[i++] = static_cast<std::uint8_t>(opcode);
                            buffer[i++] = static_cast<std::uint8_t>(modrm);
                            fill_from(i);
                            const std::size_t size = repeat != 1 ? sizeof(buffer) : 1 + (rng.next() >> 33) % 18;
                            enumerate_case(reference, buffer, size, t);
                        }
                    }
                }
            }
        }
    }

    // 2b. The same maps behind a long run of the prefixes that are allowed in front of VEX / EVEX, so that the
    //     instruction ends near, at or beyond the 15-byte limit (found by the fuzzer: docs/wp/WP-38.md).
    static const std::uint8_t kAllowedPrefixes[] = {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x67};
    for (unsigned evex = 0; evex < 2; ++evex) {
        for (unsigned map = 1; map < 4; ++map) {
            for (unsigned pp = 0; pp < 4; ++pp) {
                for (unsigned opcode = 0; opcode < 256; ++opcode) {
                    for (unsigned modrm = 0; modrm < 256; ++modrm) {
                        std::uint64_t r = rng.next();
                        std::size_t i = 0;
                        const unsigned run = 5 + static_cast<unsigned>(r % 8);  // 5..12 prefixes
                        r >>= 3;
                        for (unsigned k = 0; k < run; ++k) {
                            buffer[i++] = kAllowedPrefixes[(rng.next() >> 40) % sizeof(kAllowedPrefixes)];
                        }
                        if (evex != 0) {
                            buffer[i++] = 0x62;
                            buffer[i++] = static_cast<std::uint8_t>(((r & 0xF) << 4) | map);
                            buffer[i++] = static_cast<std::uint8_t>((((r >> 4) & 0x1F) << 3) | 0x04 | pp);
                            buffer[i++] = static_cast<std::uint8_t>(r >> 9);
                        } else {
                            buffer[i++] = 0xC4;
                            buffer[i++] = static_cast<std::uint8_t>(((r & 7) << 5) | map);
                            buffer[i++] = static_cast<std::uint8_t>(((r >> 3) & 0xFC) | pp);
                        }
                        buffer[i++] = static_cast<std::uint8_t>(opcode);
                        buffer[i++] = static_cast<std::uint8_t>(modrm);
                        fill_from(i);
                        enumerate_case(reference, buffer, sizeof(buffer), t);
                    }
                }
            }
        }
    }

    // 3. Any prefix byte, REX included, in front of VEX / EVEX (and of everything else): random prefix runs.
    static const std::uint8_t kPrefixBytes[] = {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3,
                                                0x40, 0x41, 0x44, 0x48, 0x4C, 0x4F};
    static const std::uint8_t kLeads[] = {0xC4, 0xC5, 0x62, 0x0F, 0x8B, 0x01, 0xE8, 0x90};
    for (unsigned n = 0; n < 6000000; ++n) {
        std::uint64_t r = rng.next();
        const unsigned count = 1 + static_cast<unsigned>(r & 3) + ((r & 0xFF00) == 0 ? 12u : 0u);
        r >>= 16;
        std::size_t i = 0;
        for (unsigned k = 0; k < count; ++k) {
            buffer[i++] = kPrefixBytes[(rng.next() >> 40) % sizeof(kPrefixBytes)];
        }
        buffer[i++] = kLeads[r % sizeof(kLeads)];
        fill_from(i);
        enumerate_case(reference, buffer, sizeof(buffer), t);
    }

    // 4. Plain random bytes.
    for (unsigned n = 0; n < 20000000; ++n) {
        fill_from(0);
        enumerate_case(reference, buffer, 1 + (rng.next() >> 33) % sizeof(buffer), t);
    }

    std::string by_status;
    for (const auto& [status, count] : t.exception_status) {
        by_status += " " + status + "=" + std::to_string(count);
    }
    std::printf("WP38-ENUM-EXCEPTIONS (VEX/EVEX encodings the reference rejects, by its status)%s\n", by_status.c_str());
    std::printf("WP38-ENUM cases=%zu ok=%zu agree=%zu exceptions=%zu mismatches=%zu unknown_reference_ok=%zu\n", t.cases, t.ok,
                t.agree, t.exceptions, t.mismatches, t.unknown_reference_ok);
    return t.mismatches != 0 ? 3 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Reference reference;
    if (argc >= 2 && std::strcmp(argv[1], "enumerate") == 0) {
        std::uint64_t seed = 1;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
                seed = std::strtoull(argv[++i], nullptr, 10);
            } else {
                std::fprintf(stderr, "cml_lde_diff: unknown argument %s\n", argv[i]);
                return 2;
            }
        }
        return enumerate(reference, seed);
    }
    const bool raw = argc >= 3 && std::strcmp(argv[1], "raw") == 0;
    if (raw || (argc >= 3 && std::strcmp(argv[1], "sweep") == 0)) {
        std::vector<std::string> modules;
        std::size_t min_instructions = 0, min_vex = 0, min_evex = 0;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--min-instructions") == 0 && i + 1 < argc) {
                min_instructions = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
            } else if (std::strcmp(argv[i], "--min-vex") == 0 && i + 1 < argc) {
                min_vex = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
            } else if (std::strcmp(argv[i], "--min-evex") == 0 && i + 1 < argc) {
                min_evex = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
            } else if (argv[i][0] == '-' && argv[i][1] == '-') {
                std::fprintf(stderr, "cml_lde_diff: unknown argument %s\n", argv[i]);
                return 2;
            } else {
                modules.emplace_back(argv[i]);
            }
        }
        Totals grand;
        std::map<std::string, std::size_t> unknown_kinds;
        for (const std::string& module : modules) {
            if (const int rc = raw ? sweep_raw(reference, module, grand, unknown_kinds) : sweep_module(reference, module, grand, unknown_kinds);
                rc != 0) {
                return rc;
            }
        }
        std::vector<std::pair<std::size_t, std::string>> ranked;
        for (const auto& [bytes, count] : unknown_kinds) {
            ranked.emplace_back(count, bytes);
        }
        std::sort(ranked.rbegin(), ranked.rend());
        for (std::size_t i = 0; i < ranked.size() && i < 40; ++i) {
            std::printf("WP38-DIFF-UNKNOWN %s %zu\n", ranked[i].second.c_str(), ranked[i].first);
        }
        std::printf("WP38-DIFF instructions=%zu mismatches=%zu modules=%zu functions=%zu vex=%zu evex=%zu unknown=%zu exceptions=%zu\n",
                    grand.instructions, grand.mismatches, modules.size(), grand.functions, grand.vex, grand.evex, grand.unknown,
                    grand.exceptions);
        if (grand.mismatches != 0) {
            return 3;
        }
        if (grand.instructions < min_instructions || grand.vex < min_vex || grand.evex < min_evex) {
            std::fprintf(stderr, "cml_lde_diff: the sweep saw fewer instructions than required (%zu/%zu, VEX %zu/%zu, EVEX %zu/%zu)\n",
                         grand.instructions, min_instructions, grand.vex, min_vex, grand.evex, min_evex);
            return 4;
        }
        return 0;
    }
    std::fprintf(stderr, "usage: cml_lde_diff sweep <image>... [--min-instructions N] [--min-vex N] [--min-evex N]\n"
                         "       cml_lde_diff raw <code file>... [--min-instructions N] [--min-vex N] [--min-evex N]\n"
                         "       cml_lde_diff enumerate [--seed N]\n");
    return 2;
}
