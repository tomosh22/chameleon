// cml_lde_probe: runs the hook installer's instruction-length decoder over the first 16 bytes of every
// function of a module and prints what it finds (CI evidence for WP-10; see docs/wp/WP-10.md).
//
//   cml_lde_probe <image.exe|dll> [--quiet]
//
// The functions come from the module's PDB (every S_GPROC32 / S_LPROC32 of every compiland plus the
// function publics); the bytes come from the image file. Output, one line per function:
//
//   LDE <function> lens=2,4,5 unknown=0 plan=padding+2-byte
//
// `lens` are the lengths of the instructions decoded within the first 16 bytes (decoding stops at a
// ret / jmp / int3 and at the end of the 16 bytes). unknown=1 means an instruction the decoder does
// not cover came up before that point: the hook installer would refuse the function with CML4300.
// `plan` is what plan_hook() decides from the bytes around the entry. Then summary lines:
//
//   LDE-SUMMARY functions=N unknown=M ...
//   LDE-PLAN <strategy or CML43xx> count
//   LDE-UNKNOWN <first bytes of the unknown instruction> count      (most frequent first)
//
// Exit code: 0, or 1 when the module or its PDB cannot be read.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cml/pdb/pdb.h"
#include "cml/pe/pe.h"
#include "hook/diag_ids.h"
#include "hook/hook_plan.h"
#include "hook/length_decoder.h"

namespace {

using cml::agent::hook::DecodeStatus;

struct Function {
    std::string name;
    std::uint32_t rva = 0;
};

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

std::string clean(std::string_view name) {
    std::string s(name);
    for (char& c : s) {
        if (c == ' ' || c == '\t') {
            c = '_';
        }
    }
    return s;
}

// Bytes [rva, rva + length) of the image, as many as the section holds (possibly fewer).
std::vector<std::uint8_t> read_clamped(const cml::pe::Image& image, std::uint32_t rva, std::size_t length) {
    for (std::size_t n = length; n > 0; --n) {
        auto span = image.read_rva(rva, n);
        if (span) {
            std::vector<std::uint8_t> out(n);
            std::memcpy(out.data(), span->data(), n);
            return out;
        }
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: cml_lde_probe <image.exe|dll> [--quiet]\n");
        return 2;
    }
    const std::string path = argv[1];
    bool quiet = false;
    for (int i = 2; i < argc; ++i) {
        quiet = quiet || std::strcmp(argv[i], "--quiet") == 0;
    }

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

    // Every function, once per RVA: procs first (they carry the readable names), then publics.
    std::map<std::uint32_t, std::string> by_rva;
    for (std::uint32_t c = 0; c < pdb.compiland_count(); ++c) {
        auto symbols = pdb.compiland_symbols(c);
        if (!symbols) {
            continue;  // a compiland with corrupt symbols is skipped; the count shows
        }
        for (const cml::pdb::SymbolView& s : *symbols) {
            if (cml::pdb::is_proc_kind(s.kind) && s.rva != cml::pdb::kNoRva) {
                by_rva.emplace(s.rva, clean(s.name));
            }
        }
    }
    if (auto publics = pdb.all_publics()) {
        for (const cml::pdb::SymbolView& s : *publics) {
            if (s.is_function() && s.rva != cml::pdb::kNoRva) {
                by_rva.emplace(s.rva, clean(s.name));
            }
        }
    }

    std::size_t decoded_ok = 0, unknown_count = 0, with_flow_end = 0;
    std::map<std::string, std::size_t> plans;
    std::map<std::string, std::size_t> unknown_bytes;
    for (const auto& [rva, name] : by_rva) {
        const std::vector<std::uint8_t> head = read_clamped(owned->image, rva, 16);
        if (head.empty()) {
            continue;  // not backed by file bytes (e.g. in a section without raw data)
        }
        std::string lens;
        bool unknown = false;
        std::size_t at = 0;
        std::string unknown_key;
        while (at < head.size()) {
            const auto d = cml::agent::hook::decode_instruction(head.data() + at, head.size() - at);
            if (d.status == DecodeStatus::Unknown) {
                unknown = true;
                unknown_key = hex_bytes(head.data() + at, std::min<std::size_t>(3, head.size() - at));
                break;
            }
            if (d.status == DecodeStatus::Truncated) {
                break;  // runs past the 16 bytes: fine
            }
            if (!lens.empty()) {
                lens += ',';
            }
            lens += std::to_string(d.length);
            at += d.length;
            if (cml::agent::hook::ends_flow(d.flags)) {
                ++with_flow_end;
                break;
            }
        }

        // What the installer would do with this entry.
        std::vector<std::uint8_t> before;
        for (std::size_t k = cml::agent::hook::kLookBehind; k > 0 && before.empty(); --k) {
            if (rva >= k) {
                before = read_clamped(owned->image, rva - static_cast<std::uint32_t>(k), k);
                if (before.size() != k) {
                    before.clear();
                }
            }
        }
        const std::vector<std::uint8_t> ahead = read_clamped(owned->image, rva, cml::agent::hook::kLookAhead);
        const cml::agent::hook::HookPlan plan =
            cml::agent::hook::plan_hook(before.data(), before.size(), ahead.data(), ahead.size());
        const std::string plan_text = plan.strategy != cml::agent::hook::Strategy::None
                                          ? cml::agent::hook::to_string(plan.strategy)
                                          : "CML" + std::to_string(plan.diag);
        ++plans[plan_text];

        if (unknown) {
            ++unknown_count;
            ++unknown_bytes[unknown_key];
        } else {
            ++decoded_ok;
        }
        if (!quiet) {
            std::printf("LDE %s lens=%s unknown=%d plan=%s\n", name.c_str(), lens.empty() ? "-" : lens.c_str(),
                        unknown ? 1 : 0, plan_text.c_str());
        }
    }

    std::printf("LDE-SUMMARY functions=%zu decoded=%zu unknown=%zu ended_by_ret_or_jmp=%zu\n", by_rva.size(), decoded_ok,
                unknown_count, with_flow_end);
    for (const auto& [text, count] : plans) {
        std::printf("LDE-PLAN %s %zu\n", text.c_str(), count);
    }
    std::vector<std::pair<std::size_t, std::string>> ranked;
    for (const auto& [bytes, count] : unknown_bytes) {
        ranked.emplace_back(count, bytes);
    }
    std::sort(ranked.rbegin(), ranked.rend());
    for (std::size_t i = 0; i < ranked.size() && i < 40; ++i) {
        std::printf("LDE-UNKNOWN %s %zu\n", ranked[i].second.c_str(), ranked[i].first);
    }
    return 0;
}
