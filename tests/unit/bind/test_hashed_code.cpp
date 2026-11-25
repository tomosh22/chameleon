// WP-46 (F-41.2, ADR-0043 ruling D1): lambdas that cl's legacy lambda processor names by a hash (`<lambda_<32 hex>>`, no
// scope) are paired by that name, whose derivation is not documented. The pipeline proves the pairing by the code of the
// lambdas' functions (prove_hashed_lambdas, the WP-25 comparator with the plan's decisions): every lambda's code must be the
// old one's, except at most one changed lambda (rule (d)); otherwise CML3630. The old module is the synthetic PE of
// tests/unit/pe, the new object is made with the WP-06 writer, and the plan is written by hand as the planner writes it
// (each call operator hooked onto the old one of its name, its static bound to the old one of its name).
#include <cstring>
#include <string>
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
constexpr std::uint32_t kOldOp[3] = {0x1100, 0x1140, 0x1180};   // the old call operators of hash 0 .. 2 (16 bytes each)
constexpr std::uint32_t kOldSlot[3] = {0x3100, 0x3110, 0x3120};  // their statics

const char* const kHashes[3] = {"<lambda_0b7c44cbca6a0337bec371af34c9af94>", "<lambda_24d82191404f736f33e380197c42a713>",
                                "<lambda_d3b4d954b14ab53a4d29b8c79799d6ec>"};
std::string op_name(int n) { return std::string("??R") + kHashes[n] + "@@QEBA@XZ"; }
std::string slot_name(int n) { return std::string("?slot@?1???R") + kHashes[n] + "@@QEBA@XZ@4HA"; }

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

// A lambda's call operator: `mov ecx, id; lea rax,[rip+slot]; ret`, 16 bytes with the 0xCC filler.
std::vector<std::uint8_t> encode(std::uint32_t id) {
    std::vector<std::uint8_t> c = {0xB9, static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(id >> 8), static_cast<std::uint8_t>(id >> 16),
                                   static_cast<std::uint8_t>(id >> 24), 0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3};
    while (c.size() < 16) {
        c.push_back(0xCC);
    }
    return c;
}

class World {
public:
    // The old lambda named by hash n: `id`, at kOldOp[n], reaching kOldSlot[n].
    void old_lambda(int n, std::uint32_t id) {
        std::vector<std::uint8_t> code = encode(id);
        const std::uint32_t rva = kOldOp[n];
        const std::int32_t disp = static_cast<std::int32_t>(kOldSlot[n]) - static_cast<std::int32_t>(rva + 8 + 4);
        std::memcpy(code.data() + 8, &disp, 4);
        std::memcpy(disk_.data() + cml_test::pe_layout::kTextRaw + (rva - cml_test::pe_layout::kTextRva), code.data(), code.size());
    }
    // The patch's lambda named by hash n: `id`.
    void new_lambda(int n, std::uint32_t id) {
        if (data_section_ == 0) {
            data_section_ = obj_.section(".bss", kData, 0x20);
        }
        const SynthObj::Func f = obj_.function(op_name(n), false, 16);
        const std::vector<std::uint8_t> code = encode(id);
        coff::ModelSection& s = obj_.model().sections[f.section - 1];
        for (std::size_t i = 0; i < code.size(); ++i) {
            s.data[i] = static_cast<std::byte>(code[i]);
        }
        const std::uint32_t slot = obj_.symbol(slot_name(n), static_cast<std::uint32_t>(4 * news_.size()), static_cast<std::int32_t>(data_section_), false, false);
        obj_.reloc(f.section, 8, slot, coff::RelocType::Rel32);
        news_.push_back({n, f, slot});
    }

    // The plan: every lambda paired with the old one of its hash name. `unhooked`: that lambda's call operator has no hook.
    // `eliminated`: that lambda was paired by elimination (its name changed with its code; WP-46 round 2).
    std::vector<Diag> run(std::uint32_t old_size = 16, int unhooked = -1, int eliminated = -1) {
        built_ = obj_.build();
        BindPlan plan;
        plan.object_name = "unity_12_cxx.obj";
        for (const New& nw : news_) {
            SymbolDecision op;
            op.symbol_index = nw.f.symbol;
            op.name = op_name(nw.n);
            op.display = op.name;
            op.type = SymbolType::Function;
            op.defined = true;
            op.is_static = true;
            op.section = nw.f.section;
            op.decision = DecisionKind::PatchHooked;
            op.old_address = address(kOldOp[nw.n]);
            plan.decisions.push_back(op);
            SymbolDecision slot;
            slot.symbol_index = nw.slot;
            slot.name = slot_name(nw.n);
            slot.display = slot.name;
            slot.type = SymbolType::Data;
            slot.defined = true;
            slot.is_static = true;
            slot.section = data_section_;
            slot.decision = DecisionKind::BindOldData;
            slot.old_address = address(kOldSlot[nw.n]);
            plan.decisions.push_back(slot);
            if (nw.n != unhooked) {
                HookEntry h;
                h.new_symbol = op.name;
                h.display = op.name;
                h.old_va = kImageBase + kOldOp[nw.n];
                h.old_rva = kOldOp[nw.n];
                h.old_size = old_size;
                plan.hooks.push_back(h);
            }
            HashedLambda hl;
            hl.closure = kHashes[nw.n];
            hl.old_closure = nw.n == eliminated ? std::string("<lambda_00000000000000000000000000000000>") : std::string();
            hl.functions = {op.name};
            hl.statics = {slot.name};
            plan.hashed_lambdas.push_back(hl);
        }
        auto image = pe::Image::parse(cml_test::pe_span(disk_), pe::ImageKind::OnDisk);
        REQUIRE_MESSAGE(image.has_value(), image.error().format());
        NarrowInput in;
        in.object = &built_.obj();
        in.plan = &plan;
        in.old_image = &*image;
        in.load_base = kImageBase;
        in.enabled = false;  // the proof runs whatever the narrowing option says
        return prove_hashed_lambdas(in);
    }

private:
    struct New {
        int n;
        SynthObj::Func f;
        std::uint32_t slot;
    };
    static TargetAddress address(std::uint32_t rva) {
        TargetAddress t;
        t.valid = true;
        t.rva = rva;
        t.va = kImageBase + rva;
        t.module = "old.exe";
        return t;
    }
    cml_test::PeBytes disk_ = cml_test::build_pe_on_disk();
    SynthObj obj_;
    std::uint32_t data_section_ = 0;
    std::vector<New> news_;
    Built built_;
};

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// The three old lambdas of every case.
void old_three(World& w) {
    w.old_lambda(0, 1000);
    w.old_lambda(1, 2000);
    w.old_lambda(2, 3000);
}

}  // namespace

TEST_CASE("hashed lambdas by code (WP-46): an edit elsewhere in the unit leaves every lambda's code as it was: proven") {
    QuietLogs quiet;
    World w;
    old_three(w);
    w.new_lambda(0, 1000);
    w.new_lambda(1, 2000);
    w.new_lambda(2, 3000);
    CHECK(w.run().empty());
}

TEST_CASE("hashed lambdas by code (WP-46): one lambda whose body changed is the one the edit changed: proven (rule (d))") {
    QuietLogs quiet;
    World w;
    old_three(w);
    w.new_lambda(0, 1000);
    w.new_lambda(1, 2001);
    w.new_lambda(2, 3000);
    CHECK(w.run().empty());
}

TEST_CASE("hashed lambdas by code (WP-46): names that moved to other lambdas are refused (CML3630), never hooked crosswise") {
    // If the hash were handed out by position, a lambda added in front and one removed behind would give the unchanged lambdas
    // each other's names: the old lambda named hash 0 would run the code of the lambda now named so, with the other's static.
    QuietLogs quiet;
    World w;
    old_three(w);
    w.new_lambda(0, 2000);
    w.new_lambda(1, 3000);
    w.new_lambda(2, 1000);
    const std::vector<Diag> errors = w.run();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kHashedLambdasUnproven);
    CHECK(contains(errors[0].message, "3 of them changed"));
    for (int n = 0; n < 3; ++n) {
        CHECK(contains(errors[0].message, kHashes[n]));
    }
}

TEST_CASE("hashed lambdas by code (WP-46): two changed lambdas cannot be told from two whose names moved (CML3630)") {
    QuietLogs quiet;
    World w;
    old_three(w);
    w.new_lambda(0, 1001);
    w.new_lambda(1, 2001);
    w.new_lambda(2, 3000);
    const std::vector<Diag> errors = w.run();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kHashedLambdasUnproven);
    CHECK(contains(errors[0].message, "2 of them changed"));
    CHECK(contains(errors[0].message, kHashes[0]));
    CHECK(contains(errors[0].message, kHashes[1]));
    CHECK_FALSE(contains(errors[0].message, kHashes[2]));
}

TEST_CASE("hashed lambdas by code (WP-46): a lambda whose code cannot be compared is not proven (CML3630, with the reason)") {
    QuietLogs quiet;
    SUBCASE("the old size is not known") {
        World w;
        old_three(w);
        w.new_lambda(0, 1000);
        w.new_lambda(1, 2000);
        const std::vector<Diag> errors = w.run(0);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kHashedLambdasUnproven);
        CHECK(contains(errors[0].message, "cannot be compared with the old code"));
    }
    SUBCASE("a function of the lambda is not hooked to an old one") {
        World w;
        old_three(w);
        w.new_lambda(0, 1000);
        w.new_lambda(1, 2000);
        const std::vector<Diag> errors = w.run(16, 1);
        REQUIRE(errors.size() == 1);
        CHECK(contains(errors[0].message, "is not hooked to an old function"));
        CHECK(contains(errors[0].message, kHashes[1]));
    }
}

TEST_CASE("hashed lambdas by code (WP-46 round 2): the lambda paired by elimination is the one changed lambda; every other must be unchanged") {
    QuietLogs quiet;
    SUBCASE("the others are unchanged: proven") {
        World w;
        old_three(w);
        w.new_lambda(0, 1000);
        w.new_lambda(1, 2001);  // the edited lambda (renamed by cl, paired with the old one left)
        w.new_lambda(2, 3000);
        CHECK(w.run(16, -1, 1).empty());
    }
    SUBCASE("another lambda whose name was kept changed too: CML3630") {
        World w;
        old_three(w);
        w.new_lambda(0, 1001);
        w.new_lambda(1, 2001);
        w.new_lambda(2, 3000);
        const std::vector<Diag> errors = w.run(16, -1, 1);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kHashedLambdasUnproven);
        CHECK(contains(errors[0].message, "one lambda was paired by elimination"));
        CHECK(contains(errors[0].message, kHashes[0]));
    }
    SUBCASE("the eliminated lambda's call operator is not hooked: CML3630") {
        World w;
        old_three(w);
        w.new_lambda(0, 1000);
        w.new_lambda(1, 2001);
        w.new_lambda(2, 3000);
        const std::vector<Diag> errors = w.run(16, 1, 1);
        REQUIRE(errors.size() == 1);
        CHECK(contains(errors[0].message, "paired by elimination"));
        CHECK(contains(errors[0].message, "is not hooked to the old one"));
    }
}
