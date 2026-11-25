// WP-40 (second high review, ADR-0043 ruling): a bitmask guard `$S<n>` (cl /Zc:threadSafeInit-, clang-cl
// -fno-threadsafe-statics) hands out its bits to the function's dynamically initialised statics in the order they are
// reached. The old bitmask fits the patch only while those statics are the same set in the same order, which the
// records cannot show (a static made constant-initialised keeps its record) and an unchanged function proves:
// prove_bitmask_guards compares the function's code with the old one (the WP-25 comparator, under the plan's
// decisions) and refuses with CML3600 otherwise. The old module is the synthetic PE of tests/unit/pe; the new object
// is made with the WP-06 writer; the plan is written by hand, as the planner writes it (settle_guards).
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../pe/pe_builder.h"
#include "cml/core/bind_diag_ids.h"
#include "cml/core/hook_narrowing.h"
#include "cml/util/log.h"
#include "doctest.h"
#include "synth.h"

using namespace cml;
using namespace cml::core;
using namespace cml::core::bind;
using namespace cml_bind_test;

namespace {

constexpr std::uint64_t kImageBase = 0x140000000ull;
constexpr std::uint32_t kOldF = 0x1100;     // the old f() (32 bytes)
constexpr std::uint32_t kOldBits = 0x3100;  // its bitmask
constexpr std::uint32_t kOldA = 0x3104;     // its statics a (block 1) and b (block 2)
constexpr std::uint32_t kOldB = 0x3108;

const char* const kFunction = "?f@@YAHXZ";
const char* const kBits = "?$S1@?1??f@@YAHXZ@4IA";
const char* const kA = "?a@?1??f@@YAHXZ@4HA";
const char* const kB = "?b@?2??f@@YAHXZ@4HA";

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

// What f() does here, for each of its statics in order: `or byte [rax], bit` (when the static is dynamically initialised:
// bit != 0) and `lea rcx|rdx,[rip+static]`, after `lea rax,[rip+bitmask]`; then `ret`, 0xCC up to 32 bytes.
struct Use {
    char name;          // 'a' or 'b'
    std::uint8_t bit;   // 0: constant-initialised (no bit)
};
struct Code {
    std::vector<std::uint8_t> bytes;
    std::vector<std::pair<std::uint32_t, char>> fields;  // (offset of a rel32, 'm' bitmask / 'a' / 'b')
};
Code encode(const std::vector<Use>& uses) {
    Code c;
    c.bytes = {0x48, 0x8D, 0x05, 0, 0, 0, 0};  // lea rax,[rip+bitmask]
    c.fields.push_back({3, 'm'});
    for (const Use& u : uses) {
        if (u.bit != 0) {
            const std::vector<std::uint8_t> set_bit = {0x80, 0x08, u.bit};  // or byte ptr [rax], bit
            c.bytes.insert(c.bytes.end(), set_bit.begin(), set_bit.end());
        }
        const std::uint8_t modrm = u.name == 'a' ? std::uint8_t{0x0D} : std::uint8_t{0x15};  // rcx / rdx
        const std::uint32_t at = static_cast<std::uint32_t>(c.bytes.size());
        const std::vector<std::uint8_t> lea = {0x48, 0x8D, modrm, 0, 0, 0, 0};
        c.bytes.insert(c.bytes.end(), lea.begin(), lea.end());
        c.fields.push_back({at + 3, u.name});
    }
    c.bytes.push_back(0xC3);
    while (c.bytes.size() < 32) {
        c.bytes.push_back(0xCC);
    }
    return c;
}

TargetAddress address(std::uint32_t rva) {
    TargetAddress t;
    t.valid = true;
    t.rva = rva;
    t.va = kImageBase + rva;
    t.module = "old.exe";
    return t;
}

class World {
public:
    void old_function(const std::vector<Use>& uses) {
        const Code c = encode(uses);
        std::vector<std::uint8_t> code = c.bytes;
        for (const auto& [field, what] : c.fields) {
            const std::uint32_t target = what == 'm' ? kOldBits : what == 'a' ? kOldA : kOldB;
            const std::int32_t disp = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(kOldF + field + 4);
            std::memcpy(code.data() + field, &disp, 4);
        }
        const std::size_t at = cml_test::pe_layout::kTextRaw + (kOldF - cml_test::pe_layout::kTextRva);
        std::memcpy(disk_.data() + at, code.data(), code.size());
    }

    // The patch's f(), and the plan: f hooked onto the old f(), the bitmask and the statics bound to the old ones.
    std::vector<Diag> run(const std::vector<Use>& uses, bool hooked = true, const std::string& function = kFunction) {
        const Code c = encode(uses);
        SynthObj::Func f = obj_.function(kFunction, false, 32);
        coff::ModelSection& s = obj_.model().sections[f.section - 1];
        for (std::size_t i = 0; i < c.bytes.size(); ++i) {
            s.data[i] = static_cast<std::byte>(c.bytes[i]);
        }
        const std::uint32_t bss = obj_.section(".bss", kData, 0x10);
        std::vector<std::pair<std::string, std::uint32_t>> data = {{kBits, 0}};
        for (const Use& u : uses) {
            data.push_back({u.name == 'a' ? kA : kB, u.name == 'a' ? 4u : 8u});
        }
        std::vector<std::uint32_t> index;
        for (const auto& [name, value] : data) {
            index.push_back(obj_.symbol(name, value, static_cast<std::int32_t>(bss), false, false));
        }
        for (const auto& [field, what] : c.fields) {
            std::uint32_t symbol = index[0];
            for (std::size_t k = 1; k < data.size(); ++k) {
                if (data[k].first == (what == 'a' ? kA : what == 'b' ? kB : "")) {
                    symbol = index[k];
                }
            }
            obj_.reloc(f.section, field, symbol, coff::RelocType::Rel32);
        }
        built_ = obj_.build();

        BindPlan plan;
        SymbolDecision fd;
        fd.symbol_index = f.symbol;
        fd.name = kFunction;
        fd.display = "f";
        fd.type = SymbolType::Function;
        fd.defined = true;
        fd.section = f.section;
        fd.decision = DecisionKind::PatchHooked;
        fd.old_address = address(kOldF);
        plan.decisions.push_back(fd);
        for (std::size_t k = 0; k < data.size(); ++k) {
            SymbolDecision d;
            d.symbol_index = index[k];
            d.name = data[k].first;
            d.display = d.name;
            d.type = SymbolType::Data;
            d.defined = true;
            d.is_static = true;
            d.section = bss;
            d.decision = DecisionKind::BindOldData;
            d.old_address = address(data[k].first == kBits ? kOldBits : data[k].first == kA ? kOldA : kOldB);
            plan.decisions.push_back(d);
        }
        if (hooked) {
            HookEntry h;
            h.new_symbol = kFunction;
            h.display = "f";
            h.old_va = kImageBase + kOldF;
            h.old_rva = kOldF;
            h.old_size = 32;
            plan.hooks.push_back(h);
        }
        plan.bitmask_guards.push_back({kBits, function});
        auto image = pe::Image::parse(cml_test::pe_span(disk_), pe::ImageKind::OnDisk);
        REQUIRE_MESSAGE(image.has_value(), image.error().format());
        NarrowInput in;
        in.object = &built_.obj();
        in.plan = &plan;
        in.old_image = &*image;
        in.load_base = kImageBase;
        in.enabled = false;  // the proof runs whatever the narrowing option says
        return prove_bitmask_guards(in);
    }

private:
    cml_test::PeBytes disk_ = cml_test::build_pe_on_disk();
    SynthObj obj_;
    Built built_;
};

}  // namespace

TEST_CASE("bitmask guards: f() unchanged: its bits are handed out as before, the old bitmask is kept") {
    QuietLogs quiet;
    World w;
    w.old_function({{'a', 1}, {'b', 2}});
    const std::vector<Diag> errors = w.run({{'a', 1}, {'b', 2}});
    CHECK(errors.empty());
}

TEST_CASE("bitmask guards: a static made constant-initialised shifts the bits of the rest: CML3600") {
    // `static int a = make();` became `static int a = 0;`: a keeps its record (and is bound), but takes no bit any more,
    // and b now takes bit 0, which the running program set for a: b would never be constructed.
    QuietLogs quiet;
    World w;
    w.old_function({{'a', 1}, {'b', 2}});
    const std::vector<Diag> errors = w.run({{'a', 0}, {'b', 1}});
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kGuardMismatch);
    CHECK(errors[0].id() == "CML3600");
    CHECK(errors[0].message.find("the code of 'f' changed") != std::string::npos);
}

TEST_CASE("bitmask guards: a static removed in front of another shifts its bit: CML3600") {
    QuietLogs quiet;
    World w;
    w.old_function({{'a', 1}, {'b', 2}});
    const std::vector<Diag> errors = w.run({{'b', 1}});
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kGuardMismatch);
}

TEST_CASE("bitmask guards: nothing to compare (the function is not the old version of one, or its name is unreadable): CML3600") {
    QuietLogs quiet;
    SUBCASE("not hooked") {
        World w;
        w.old_function({{'a', 1}, {'b', 2}});
        const std::vector<Diag> errors = w.run({{'a', 1}, {'b', 2}}, false);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kGuardMismatch);
        CHECK(errors[0].message.find("is not the old version of a function") != std::string::npos);
    }
    SUBCASE("no function name") {
        World w;
        w.old_function({{'a', 1}, {'b', 2}});
        const std::vector<Diag> errors = w.run({{'a', 1}, {'b', 2}}, true, "");
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kGuardMismatch);
    }
}
