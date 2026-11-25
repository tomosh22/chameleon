// WP-40 (R-48 item 5, ADR-0043 rule (d)): lambdas of one function that the type records cannot tell apart (Godot's SNAME:
// no captures, the same call operator) are paired by the CODE of their call operators, compared with the old image by the
// unchanged-function comparator of WP-25 (hook_narrowing), under the hypothesis that a new lambda's statics are the old
// lambda's. The old module is the synthetic PE of tests/unit/pe; the new object is made with the WP-06 writer; the plan
// is written by hand (as the planner, which pairs by number, would have written it).
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
constexpr std::uint32_t kOldOp[2] = {0x1100, 0x1140};   // the old call operators of <lambda_0> / <lambda_1> (16 bytes each)
constexpr std::uint32_t kOldSlot[2] = {0x3100, 0x3110}; // their statics (only compared as addresses)
constexpr std::uint32_t kOldLiteral[2] = {0x2980, 0x29C0};  // the string literals each passes, in .rdata

std::string closure(int n) { return "<lambda_" + std::to_string(n) + ">@?0??f@@YAXXZ@"; }
std::string op_name(int n) { return "??R" + closure(n) + "QEBA@XZ"; }
std::string slot_name(int n) { return "?slot@?1???R" + closure(n) + "QEBA@XZ@4HA"; }
std::string literal_name(int n) { return "??_C@_05LIT" + std::to_string(n) + "@lit@"; }

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

// What a lambda's call operator does here: `lea rcx,[rip+literal]` (literal variant) or `mov ecx, id`, then
// `lea rax,[rip+slot]`, `ret`. 16 bytes with the 0xCC filler.
struct Body {
    std::uint32_t id = 0;        // the immediate (mov ecx, id); 0: the literal variant
    std::string literal;         // the literal variant: the text the lambda passes
};

class World {
public:
    // Old lambda n (number n): its body, at kOldOp[n], reaching kOldSlot[n] (and kOldLiteral[n]).
    void old_lambda(int n, const Body& body) {
        std::vector<std::uint8_t> code = encode(body);
        const std::uint32_t rva = kOldOp[n];
        const std::size_t at = cml_test::pe_layout::kTextRaw + (rva - cml_test::pe_layout::kTextRva);
        // Fix the RIP-relative displacements to the old targets.
        std::uint32_t pos = 0;
        if (body.id == 0) {
            put_rel(code, 3, rva, kOldLiteral[n]);
            write_literal(kOldLiteral[n], body.literal);
            pos = 7;
        } else {
            pos = 5;
        }
        put_rel(code, pos + 3, rva, kOldSlot[n]);
        std::memcpy(disk_.data() + at, code.data(), code.size());
    }

    // New lambda n of the patch object.
    void new_lambda(int n, const Body& body) {
        SynthObj::Func f = obj_.function(op_name(n), false, 16);
        std::vector<std::uint8_t> code = encode(body);
        coff::ModelSection& s = obj_.model().sections[f.section - 1];
        for (std::size_t i = 0; i < code.size(); ++i) {
            s.data[i] = static_cast<std::byte>(code[i]);
        }
        const std::uint32_t slot = obj_.symbol(slot_name(n), 0, static_cast<std::int32_t>(data_section()), false, false);
        std::uint32_t pos = 5;
        std::uint32_t literal = 0;
        if (body.id == 0) {
            const std::uint32_t lsec = obj_.section(".rdata", kRdata | coff::scn::LnkComdat, 16, coff::ComdatSelection::Any);
            coff::ModelSection& ls = obj_.model().sections[lsec - 1];
            for (std::size_t i = 0; i < body.literal.size(); ++i) {
                ls.data[i] = static_cast<std::byte>(body.literal[i]);
            }
            literal = obj_.symbol(literal_name(n), 0, static_cast<std::int32_t>(lsec), false, true);
            obj_.reloc(f.section, 3, literal, coff::RelocType::Rel32);
            pos = 7;
            literals_.push_back({literal, lsec, literal_name(n)});
        }
        obj_.reloc(f.section, pos + 3, slot, coff::RelocType::Rel32);
        news_.push_back({n, f, slot});
    }

    // The plan the planner makes: lambda n hooked onto old n, its slot bound to old n's (by number).
    LambdaPairing run(std::uint32_t old_size = 16) {
        built_ = obj_.build();
        BindPlan plan;
        LambdaSet set;
        set.function = "f";
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
            HookEntry h;
            h.new_symbol = op.name;
            h.display = op.name;
            h.old_va = kImageBase + kOldOp[nw.n];
            h.old_rva = kOldOp[nw.n];
            h.old_size = old_size;
            plan.hooks.push_back(h);
            LambdaEntry e;
            e.closure = closure(nw.n);
            e.old_closure = closure(nw.n);
            e.call_operator = op.name;
            e.statics = {slot.name};
            set.lambdas.push_back(e);
        }
        for (const Literal& l : literals_) {
            SymbolDecision d;
            d.symbol_index = l.symbol;
            d.name = l.name;
            d.display = l.name;
            d.type = SymbolType::Data;
            d.defined = true;
            d.section = l.section;
            d.decision = DecisionKind::PatchFresh;
            plan.decisions.push_back(d);
        }
        plan.lambda_sets.push_back(set);
        auto image = pe::Image::parse(cml_test::pe_span(disk_), pe::ImageKind::OnDisk);
        REQUIRE_MESSAGE(image.has_value(), image.error().format());
        NarrowInput in;
        in.object = &built_.obj();
        in.plan = &plan;
        in.old_image = &*image;
        in.load_base = kImageBase;
        in.enabled = false;  // the comparison runs whatever the narrowing option says
        return pair_lambdas_by_code(in);
    }

private:
    struct New {
        int n;
        SynthObj::Func f;
        std::uint32_t slot;
    };
    struct Literal {
        std::uint32_t symbol;
        std::uint32_t section;
        std::string name;
    };
    static std::vector<std::uint8_t> encode(const Body& b) {
        std::vector<std::uint8_t> c;
        if (b.id == 0) {
            c = {0x48, 0x8D, 0x0D, 0, 0, 0, 0};  // lea rcx,[rip+literal]
        } else {
            c = {0xB9, static_cast<std::uint8_t>(b.id), static_cast<std::uint8_t>(b.id >> 8), static_cast<std::uint8_t>(b.id >> 16), static_cast<std::uint8_t>(b.id >> 24)};
        }
        const std::vector<std::uint8_t> tail = {0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3};  // lea rax,[rip+slot]; ret
        c.insert(c.end(), tail.begin(), tail.end());
        while (c.size() < 16) {
            c.push_back(0xCC);
        }
        return c;
    }
    static void put_rel(std::vector<std::uint8_t>& code, std::uint32_t field, std::uint32_t op_rva, std::uint32_t target_rva) {
        const std::int32_t disp = static_cast<std::int32_t>(target_rva) - static_cast<std::int32_t>(op_rva + field + 4);
        std::memcpy(code.data() + field, &disp, 4);
    }
    void write_literal(std::uint32_t rva, const std::string& text) {
        const std::size_t at = cml_test::pe_layout::kRdataRaw + (rva - cml_test::pe_layout::kRdataRva);
        std::memset(disk_.data() + at, 0, 16);
        std::memcpy(disk_.data() + at, text.data(), text.size());
    }
    std::uint32_t data_section() {
        if (data_section_ == 0) {
            data_section_ = obj_.section(".bss", kData, 0x20);
        }
        return data_section_;
    }
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
    std::vector<Literal> literals_;
    Built built_;
};

Body id(std::uint32_t v) { return Body{v, {}}; }
Body text(const std::string& t) { return Body{0, t}; }

}  // namespace

TEST_CASE("lambdas by code (rule (d)): no swap is confirmed by the code, no warning") {
    QuietLogs quiet;
    World w;
    w.old_lambda(0, id(32000));
    w.old_lambda(1, id(33000));
    w.new_lambda(0, id(32000));
    w.new_lambda(1, id(33000));
    const LambdaPairing p = w.run();
    CHECK(p.closure_pairing.empty());
    CHECK(p.warnings.empty());
}

TEST_CASE("lambdas by code (rule (d)): two lambdas that changed places are paired by their code, not by their numbers") {
    QuietLogs quiet;
    World w;
    w.old_lambda(0, id(32000));
    w.old_lambda(1, id(33000));
    w.new_lambda(0, id(33000));  // the swap: the compiler's number 0 is now the 33000 lambda
    w.new_lambda(1, id(32000));
    const LambdaPairing p = w.run();
    REQUIRE(p.closure_pairing.size() == 2);
    CHECK(p.closure_pairing.at(closure(0)) == closure(1));
    CHECK(p.closure_pairing.at(closure(1)) == closure(0));
    CHECK(p.warnings.empty());
}

TEST_CASE("lambdas by code (rule (d)): one lambda edited in a swap is paired with the one old lambda left (step 2)") {
    QuietLogs quiet;
    World w;
    w.old_lambda(0, id(32000));
    w.old_lambda(1, id(33000));
    w.new_lambda(0, id(33000));  // unchanged, now number 0
    w.new_lambda(1, id(32500));  // the old 32000 lambda, edited
    const LambdaPairing p = w.run();
    REQUIRE(p.closure_pairing.size() == 2);
    CHECK(p.closure_pairing.at(closure(0)) == closure(1));
    CHECK(p.closure_pairing.at(closure(1)) == closure(0));
    CHECK(p.warnings.empty());
}

TEST_CASE("lambdas by code (rule (d)): two edited lambdas stay paired by number, with the warning CML3602") {
    QuietLogs quiet;
    World w;
    w.old_lambda(0, id(32000));
    w.old_lambda(1, id(33000));
    w.new_lambda(0, id(32001));
    w.new_lambda(1, id(33001));
    const LambdaPairing p = w.run();
    CHECK(p.closure_pairing.empty());
    REQUIRE(p.warnings.size() == 1);
    CHECK(p.warnings[0].code == diag::kLambdasMatchedByNumber);
    CHECK(p.warnings[0].id() == "CML3602");
    CHECK(p.warnings[0].message.find("Lambdas <lambda_0>, <lambda_1> of 'f' are matched by their numbers") != std::string::npos);
    CHECK(p.warnings[0].message.find("the values of their statics may be exchanged") != std::string::npos);
}

TEST_CASE("lambdas by code (rule (d)): lambdas that differ only in the string literal they pass are told apart by its bytes") {
    // SNAME("alpha") vs SNAME("beta"): the code is byte for byte the same; only the literal each references differs. The
    // comparator compares a fresh read-only constant by its content at the old target, not by its address.
    QuietLogs quiet;
    SUBCASE("in place") {
        World w;
        w.old_lambda(0, text("alpha"));
        w.old_lambda(1, text("beta"));
        w.new_lambda(0, text("alpha"));
        w.new_lambda(1, text("beta"));
        const LambdaPairing p = w.run();
        CHECK(p.closure_pairing.empty());
        CHECK(p.warnings.empty());
    }
    SUBCASE("swapped") {
        World w;
        w.old_lambda(0, text("alpha"));
        w.old_lambda(1, text("beta"));
        w.new_lambda(0, text("beta"));
        w.new_lambda(1, text("alpha"));
        const LambdaPairing p = w.run();
        REQUIRE(p.closure_pairing.size() == 2);
        CHECK(p.closure_pairing.at(closure(0)) == closure(1));
        CHECK(p.warnings.empty());
    }
}

TEST_CASE("lambdas by code (rule (d)): a body that cannot be compared leaves the numbers, with the warning that says why") {
    QuietLogs quiet;
    World w;
    w.old_lambda(0, id(32000));
    w.old_lambda(1, id(33000));
    w.new_lambda(0, id(33000));
    w.new_lambda(1, id(32000));
    const LambdaPairing p = w.run(/*old_size=*/0);  // the PDB gave no size: nothing can be compared
    CHECK(p.closure_pairing.empty());
    REQUIRE(p.warnings.size() == 1);
    CHECK(p.warnings[0].code == diag::kLambdasMatchedByNumber);
    CHECK(p.warnings[0].message.find("cannot be compared") != std::string::npos);
}
