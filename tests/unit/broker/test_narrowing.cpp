// Hook narrowing (WP-12, review item 3): which of the planner's hooks are really needed.
//
// The "old module" is the synthetic PE of tests/unit/pe (its .text is 0xCC filler at RVA 0x1000, .rdata at 0x2000);
// each test writes the old function bytes it wants into that file image, builds the NEW object with the WP-06
// writer (bind tests' SynthObj) and a hand-made BindPlan, and asks narrow_hooks().
#include "cml/core/hook_narrowing.h"

#include <cstring>
#include <string>
#include <vector>

#include "../bind/synth.h"
#include "../pe/pe_builder.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
using namespace cml::core::bind;
using namespace cml_bind_test;

namespace {

constexpr std::uint64_t kPreferred = 0x140000000ull;
constexpr std::uint32_t kFnRva = 0x1100;     // the old function (16 bytes)
constexpr std::uint32_t kBarRva = 0x1200;    // an old function the code calls
constexpr std::uint32_t kConstRva = 0x2980;  // an old constant in .rdata

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

std::size_t text_off(std::uint32_t rva) { return cml_test::pe_layout::kTextRaw + (rva - cml_test::pe_layout::kTextRva); }

void put32(cml_test::PeBytes& b, std::size_t off, std::uint32_t v) { cml_test::w32(b, off, v); }

// One scenario: an old image, a new object with one function `foo`, and the plan describing them.
struct Scenario {
    cml_test::PeBytes disk = cml_test::build_pe_on_disk();
    SynthObj obj;
    BindPlan plan;
    std::uint64_t load_base = kPreferred;
    SynthObj::Func foo;

    explicit Scenario(std::uint32_t size = 16) { foo = obj.function("?foo@@YAHXZ", true, size); }

    void old_code(const std::vector<std::uint8_t>& bytes) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            disk[text_off(kFnRva) + i] = bytes[i];
        }
    }
    void new_code(const std::vector<std::uint8_t>& bytes) {
        coff::ModelSection& s = obj.model().sections[foo.section - 1];
        for (std::byte& b : s.data) {
            b = std::byte{0xCC};  // the old image's filler
        }
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            s.data[i] = static_cast<std::byte>(bytes[i]);
        }
    }
    SymbolDecision& decision(std::uint32_t raw_index, const std::string& name, DecisionKind kind) {
        SymbolDecision d;
        d.symbol_index = raw_index;
        d.name = name;
        d.display = name;
        d.decision = kind;
        plan.decisions.push_back(d);
        return plan.decisions.back();
    }
    void hook(std::uint32_t old_size = 16, std::uint32_t old_rva = kFnRva) {
        SymbolDecision& d = decision(foo.symbol, "?foo@@YAHXZ", DecisionKind::PatchHooked);
        d.defined = true;
        d.type = SymbolType::Function;
        d.section = foo.section;
        d.value = 0;
        d.old_address.valid = true;
        d.old_address.va = load_base + old_rva;
        HookEntry h;
        h.new_symbol = "?foo@@YAHXZ";
        h.display = "foo";
        h.old_va = load_base + old_rva;
        h.old_rva = old_rva;
        h.old_size = old_size;
        plan.hooks.push_back(h);
    }
    // WP-25: what the earlier patch recorded for the entry; set by a test before run().
    std::function<std::shared_ptr<const CodeImage>(std::uint64_t)> prior;

    // The record narrowing makes of this scenario's function (what the broker keeps after the patch was applied).
    std::shared_ptr<const CodeImage> capture() {
        Built built = obj.build();
        keep = std::move(built);
        NarrowInput in;
        in.object = &keep.obj();
        in.plan = &plan;
        in.load_base = load_base;
        std::string why;
        std::optional<CodeImage> image = capture_code(in, "?foo@@YAHXZ", &why);
        REQUIRE_MESSAGE(image.has_value(), why);
        return std::make_shared<const CodeImage>(std::move(*image));
    }

    // WP-25: the data symbols bound to the old image whose initial value this scenario's object changes.
    std::vector<std::string> data_changes() {
        Built built = obj.build();
        keep = std::move(built);
        auto image = pe::Image::parse(cml_test::pe_span(disk), pe::ImageKind::OnDisk);
        REQUIRE_MESSAGE(image.has_value(), image.error().format());
        NarrowInput in;
        in.object = &keep.obj();
        in.plan = &plan;
        in.old_image = &*image;
        in.load_base = load_base;
        return bound_data_changes(in);
    }

    NarrowResult run(bool enabled = true, std::function<bool(std::uint64_t)> hooked = {}) {
        Built built = obj.build();
        keep = std::move(built);
        auto image = pe::Image::parse(cml_test::pe_span(disk), pe::ImageKind::OnDisk);
        REQUIRE_MESSAGE(image.has_value(), image.error().format());
        NarrowInput in;
        in.object = &keep.obj();
        in.plan = &plan;
        in.old_image = &*image;
        in.load_base = load_base;
        in.is_hooked = std::move(hooked);
        in.prior_code = prior;
        in.enabled = enabled;
        return narrow_hooks(in);
    }
    Built keep;
};

}  // namespace

TEST_CASE("narrowing: identical code and identical call target is dropped") {
    QuietLogs quiet;
    Scenario s;
    // call bar; ret.  The old displacement points at bar; the new object relocates against bar.
    s.old_code({0xE8, 0, 0, 0, 0, 0xC3});
    const std::int32_t disp = static_cast<std::int32_t>(kBarRva) - static_cast<std::int32_t>(kFnRva + 1 + 4);
    put32(s.disk, text_off(kFnRva) + 1, static_cast<std::uint32_t>(disp));
    s.new_code({0xE8, 0, 0, 0, 0, 0xC3});
    const std::uint32_t bar = s.obj.undefined("bar", true);
    s.obj.reloc(s.foo.section, 1, bar, kRel32, 0);
    s.hook();
    SymbolDecision& d = s.decision(bar, "bar", DecisionKind::BindOldFunction);
    d.old_address.valid = true;
    d.old_address.va = kPreferred + kBarRva;
    const NarrowResult r = s.run();
    REQUIRE(r.decisions.size() == 1);
    CHECK_MESSAGE(r.decisions[0].reason == NarrowReason::Unchanged, to_string(r.decisions[0].reason) << " " << r.decisions[0].detail);
    CHECK(r.hooks.empty());
    CHECK(r.dropped == 1);
}

TEST_CASE("narrowing: a different instruction byte keeps the hook") {
    QuietLogs quiet;
    Scenario s;
    s.old_code({0xB8, 1, 0, 0, 0, 0xC3});  // mov eax, 1 ; ret
    s.new_code({0xB8, 2, 0, 0, 0, 0xC3});  // mov eax, 2 ; ret
    s.hook();
    const NarrowResult r = s.run();
    REQUIRE(r.hooks.size() == 1);
    CHECK(r.decisions[0].reason == NarrowReason::BytesDiffer);
    CHECK(r.decisions[0].detail.find("byte 1") != std::string::npos);
}

TEST_CASE("narrowing: identical bytes are dropped even when the function is all instructions") {
    QuietLogs quiet;
    Scenario s;
    s.old_code({0xB8, 1, 0, 0, 0, 0xC3});
    s.new_code({0xB8, 1, 0, 0, 0, 0xC3});
    s.hook(16);
    CHECK(s.run().hooks.empty());
}

TEST_CASE("narrowing: a call that now goes to another function keeps the hook (masked bytes alone would miss it)") {
    QuietLogs quiet;
    Scenario s;
    s.old_code({0xE8, 0, 0, 0, 0, 0xC3});
    const std::int32_t disp = static_cast<std::int32_t>(kBarRva) - static_cast<std::int32_t>(kFnRva + 1 + 4);
    put32(s.disk, text_off(kFnRva) + 1, static_cast<std::uint32_t>(disp));
    s.new_code({0xE8, 0, 0, 0, 0, 0xC3});
    const std::uint32_t baz = s.obj.undefined("baz", true);
    s.obj.reloc(s.foo.section, 1, baz, kRel32, 0);
    s.hook();
    SymbolDecision& d = s.decision(baz, "baz", DecisionKind::BindOldFunction);
    d.old_address.valid = true;
    d.old_address.va = kPreferred + 0x1240;  // not what the old code called
    const NarrowResult r = s.run();
    REQUIRE(r.hooks.size() == 1);
    CHECK(r.decisions[0].reason == NarrowReason::TargetChanged);
}

TEST_CASE("narrowing: the in-place addend is part of the target (REL32 with A = -1, clang style)") {
    QuietLogs quiet;
    Scenario s;
    // cmp dword ptr [rip+x], 0 : 83 3D <rel32> 00. clang's REL32 carries A = -1; the old displacement is relative to
    // the end of the instruction, i.e. 1 byte after the field.
    s.old_code({0x83, 0x3D, 0, 0, 0, 0, 0x00, 0xC3});
    const std::uint32_t g_rva = 0x2980;
    const std::int32_t disp = static_cast<std::int32_t>(g_rva) - static_cast<std::int32_t>(kFnRva + 2 + 4 + 1);
    put32(s.disk, text_off(kFnRva) + 2, static_cast<std::uint32_t>(disp));
    s.new_code({0x83, 0x3D, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xC3});  // A = -1 stored in place
    const std::uint32_t g = s.obj.undefined("g_tick", false);
    s.obj.reloc(s.foo.section, 2, g, kRel32, -1);
    s.hook();
    SymbolDecision& d = s.decision(g, "g_tick", DecisionKind::BindOldData);
    d.old_address.valid = true;
    d.old_address.va = kPreferred + g_rva;
    const NarrowResult r = s.run();
    CHECK_MESSAGE(r.decisions[0].reason == NarrowReason::Unchanged, to_string(r.decisions[0].reason) << " " << r.decisions[0].detail);
}

TEST_CASE("narrowing: a reference to a symbol that does not exist in the old image keeps the hook") {
    QuietLogs quiet;
    Scenario s;
    s.old_code({0xE8, 0, 0, 0, 0, 0xC3});
    s.new_code({0xE8, 0, 0, 0, 0, 0xC3});
    const std::uint32_t helper = s.obj.undefined("new_helper", true);
    s.obj.reloc(s.foo.section, 1, helper, kRel32, 0);
    s.hook();
    s.decision(helper, "new_helper", DecisionKind::PatchNewFunction);
    const NarrowResult r = s.run();
    REQUIRE(r.hooks.size() == 1);
    CHECK(r.decisions[0].reason == NarrowReason::TargetNew);
}

TEST_CASE("narrowing: size, unknown size, missing old bytes, already hooked, disabled") {
    QuietLogs quiet;
    SUBCASE("the new function is longer") {
        Scenario s;
        s.old_code({0xC3});
        s.hook(8);  // old size 8, new section 16 bytes
        CHECK(s.run().decisions[0].reason == NarrowReason::SizeDiffers);
    }
    SUBCASE("the PDB gave no size") {
        Scenario s;
        s.hook(0);
        CHECK(s.run().decisions[0].reason == NarrowReason::UnknownOldSize);
    }
    SUBCASE("old entry not in the module file (defined by an earlier patch)") {
        Scenario s;
        s.hook(16, bind::kNoRva);
        CHECK(s.run().decisions[0].reason == NarrowReason::OldBytesUnavailable);
    }
    SUBCASE("old bytes outside the file's sections") {
        Scenario s;
        s.hook(16, 0x9000);
        CHECK(s.run().decisions[0].reason == NarrowReason::OldBytesUnavailable);
    }
    SUBCASE("an entry that currently redirects to an earlier patch is always kept, even when the code equals the original") {
        Scenario s;
        s.old_code({0xC3});
        s.new_code({0xC3});
        s.hook();
        const NarrowResult r = s.run(true, [](std::uint64_t va) { return va == kPreferred + kFnRva; });
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::AlreadyHooked);
    }
    SUBCASE("narrowing switched off hooks everything") {
        Scenario s;
        s.old_code({0xC3});
        s.new_code({0xC3});
        s.hook();
        const NarrowResult r = s.run(false);
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::Disabled);
    }
}

TEST_CASE("narrowing: a string literal or table the patch carries as a fresh copy is compared by content") {
    QuietLogs quiet;
    const auto build = [](bool same, Scenario& s) {
        // lea rax, [rip+literal]; ret
        s.old_code({0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3});
        const std::int32_t disp = static_cast<std::int32_t>(kConstRva) - static_cast<std::int32_t>(kFnRva + 3 + 4);
        put32(s.disk, text_off(kFnRva) + 3, static_cast<std::uint32_t>(disp));
        const char old_text[8] = {'h', 'e', 'l', 'l', 'o', 0, 0, 0};
        std::memcpy(s.disk.data() + cml_test::rdata_off(kConstRva), old_text, 8);
        s.new_code({0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3});
        const std::uint32_t sec = s.obj.section(".rdata$r", kRdata | coff::scn::LnkComdat, 8, coff::ComdatSelection::Any);
        const std::uint32_t lit = s.obj.symbol("??_C@_05literal@", 0, static_cast<std::int32_t>(sec), false, true);
        coff::ModelSection& ms = s.obj.model().sections[sec - 1];
        const char new_text[8] = {'h', 'e', 'l', same ? 'l' : 'p', 'o', 0, 0, 0};
        for (int i = 0; i < 8; ++i) {
            ms.data[static_cast<std::size_t>(i)] = static_cast<std::byte>(new_text[i]);
        }
        s.obj.reloc(s.foo.section, 3, lit, kRel32, 0);
        s.hook();
        SymbolDecision& d = s.decision(lit, "??_C@_05literal@", DecisionKind::PatchFresh);
        d.defined = true;
        d.type = SymbolType::Data;
        d.section = sec;
        d.value = 0;
    };
    SUBCASE("same literal") {
        Scenario s;
        build(true, s);
        const NarrowResult r = s.run();
        CHECK_MESSAGE(r.decisions[0].reason == NarrowReason::Unchanged, to_string(r.decisions[0].reason) << " " << r.decisions[0].detail);
    }
    SUBCASE("edited literal") {
        Scenario s;
        build(false, s);
        const NarrowResult r = s.run();
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::TargetChanged);
        CHECK(r.decisions[0].detail.find("differs") != std::string::npos);
    }
}

TEST_CASE("narrowing: ADDR64 and SECREL sites are judged against the planned values; load base differs from the preferred one") {
    QuietLogs quiet;
    SUBCASE("ADDR64 to old data, loaded at another base") {
        Scenario s;
        s.load_base = 0x7FF600000000ull;
        s.old_code({0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xC3});  // movabs rax, imm64
        // The file holds the value for the PREFERRED base; at run time the loader adds (load - preferred).
        const std::uint64_t data_rva = 0x3000;
        const std::uint64_t file_value = kPreferred + data_rva;
        for (int i = 0; i < 8; ++i) {
            s.disk[text_off(kFnRva) + 2 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(file_value >> (8 * i));
        }
        s.new_code({0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xC3});
        const std::uint32_t g = s.obj.undefined("g_data", false);
        s.obj.reloc(s.foo.section, 2, g, kAddr64, 0);
        s.hook();
        SymbolDecision& d = s.decision(g, "g_data", DecisionKind::BindOldData);
        d.old_address.valid = true;
        d.old_address.va = s.load_base + data_rva;
        CHECK(s.run().hooks.empty());
        d.old_address.va += 8;  // now it points elsewhere
        CHECK(s.run().hooks.size() == 1);
    }
    SUBCASE("SECREL to the thread-safe-static epoch") {
        Scenario s;
        s.old_code({0x8B, 0x82, 0, 0, 0, 0, 0xC3});  // mov eax, [rdx+secrel]
        put32(s.disk, text_off(kFnRva) + 2, 0x24);
        s.new_code({0x8B, 0x82, 0, 0, 0, 0, 0xC3});
        const std::uint32_t epoch = s.obj.undefined("_Init_thread_epoch", false);
        s.obj.reloc(s.foo.section, 2, epoch, coff::RelocType::SecRel, 0);
        s.hook();
        SymbolDecision& d = s.decision(epoch, "_Init_thread_epoch", DecisionKind::BindOldData);
        d.old_address.valid = true;
        d.old_address.va = kPreferred + 0x5000;
        Fixup f;
        f.section_index = s.foo.section;
        f.offset = 2;
        f.kind = FixupKind::SecRel32;
        f.target_section_offset = 0x24;
        f.addend = 0;
        s.plan.fixups.push_back(f);
        CHECK(s.run().hooks.empty());
        s.plan.fixups[0].target_section_offset = 0x28;
        CHECK(s.run().hooks.size() == 1);
    }
}

TEST_CASE("narrowing: a constant that holds pointers (a vtable) is never judged equal") {
    QuietLogs quiet;
    Scenario s;
    s.old_code({0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3});
    const std::int32_t disp = static_cast<std::int32_t>(kConstRva) - static_cast<std::int32_t>(kFnRva + 3 + 4);
    put32(s.disk, text_off(kFnRva) + 3, static_cast<std::uint32_t>(disp));
    s.new_code({0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3});
    const std::uint32_t sec = s.obj.section(".rdata", kRdata | coff::scn::LnkComdat, 16, coff::ComdatSelection::Any);
    const std::uint32_t vt = s.obj.symbol("??_7Foo@@6B@", 0, static_cast<std::int32_t>(sec), false, true);
    const std::uint32_t target = s.obj.undefined("Foo::f", true);
    s.obj.reloc(sec, 8, target, kAddr64, 0);
    s.obj.reloc(s.foo.section, 3, vt, kRel32, 0);
    s.hook();
    SymbolDecision& d = s.decision(vt, "??_7Foo@@6B@", DecisionKind::PatchFresh);
    d.defined = true;
    d.type = SymbolType::Data;
    d.section = sec;
    const NarrowResult r = s.run();
    REQUIRE(r.hooks.size() == 1);
    CHECK(r.decisions[0].reason == NarrowReason::Unverifiable);
}

// ---- WP-25: functions an earlier patch hooked are compared with that patch's code --------------------------------------

namespace {

constexpr std::uint64_t kBarVa = kPreferred + kBarRva;

// `call bar; ret` with the displacement field holding the in-place addend `addend` (clang writes -4, cl 0).
void call_bar(Scenario& s, std::uint64_t bar_va, std::int32_t addend = 0, std::uint8_t first_byte = 0xE8) {
    const std::uint32_t a = static_cast<std::uint32_t>(addend);
    s.new_code({first_byte, static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(a >> 8), static_cast<std::uint8_t>(a >> 16), static_cast<std::uint8_t>(a >> 24), 0xC3});
    const std::uint32_t bar = s.obj.undefined("bar", true);
    s.obj.reloc(s.foo.section, 1, bar, kRel32, addend);
    s.hook();
    SymbolDecision& d = s.decision(bar, "bar", DecisionKind::BindOldFunction);
    d.old_address.valid = true;
    d.old_address.va = bar_va;
}

bool entry_is_hooked(std::uint64_t va) { return va == kPreferred + kFnRva; }

// The first scenario's record, the way the broker keeps it after patch 1.
std::shared_ptr<const CodeImage> first_patch(std::uint64_t bar_va = kBarVa, std::int32_t addend = 0) {
    Scenario first;
    call_bar(first, bar_va, addend);
    return first.capture();
}

}  // namespace

TEST_CASE("elision: a function the earlier patch hooked and that is unchanged is dropped") {
    QuietLogs quiet;
    const std::shared_ptr<const CodeImage> prior = first_patch();
    Scenario s;
    call_bar(s, kBarVa);
    s.prior = [&](std::uint64_t va) { return entry_is_hooked(va) ? prior : nullptr; };
    const NarrowResult r = s.run(true, entry_is_hooked);
    REQUIRE(r.decisions.size() == 1);
    CHECK_MESSAGE(r.decisions[0].reason == NarrowReason::Unchanged, to_string(r.decisions[0].reason) << " " << r.decisions[0].detail);
    CHECK(r.hooks.empty());
    CHECK(r.dropped == 1);
}

TEST_CASE("elision: an already hooked function whose bytes changed keeps its hook") {
    QuietLogs quiet;
    const std::shared_ptr<const CodeImage> prior = first_patch();
    Scenario s;
    call_bar(s, kBarVa, 0, 0xE9);  // jmp instead of call
    s.prior = [&](std::uint64_t) { return prior; };
    const NarrowResult r = s.run(true, entry_is_hooked);
    REQUIRE(r.hooks.size() == 1);
    CHECK(r.decisions[0].reason == NarrowReason::BytesDiffer);
    CHECK(r.decisions[0].detail.find("earlier patch") != std::string::npos);
}

TEST_CASE("elision: a different length keeps the hook") {
    QuietLogs quiet;
    Scenario first(24);
    first.new_code({0xB8, 1, 0, 0, 0, 0xC3});
    first.hook(24);
    const std::shared_ptr<const CodeImage> prior = first.capture();
    Scenario s;
    s.new_code({0xB8, 1, 0, 0, 0, 0xC3});
    s.hook();
    s.prior = [&](std::uint64_t) { return prior; };
    const NarrowResult r = s.run(true, entry_is_hooked);
    REQUIRE(r.hooks.size() == 1);
    CHECK(r.decisions[0].reason == NarrowReason::SizeDiffers);
}

TEST_CASE("elision: a relocation retargeted between the two patches keeps the hook") {
    QuietLogs quiet;
    const std::shared_ptr<const CodeImage> prior = first_patch();
    Scenario s;
    call_bar(s, kBarVa + 0x10);  // identical bytes, but `bar` now resolves elsewhere
    s.prior = [&](std::uint64_t) { return prior; };
    const NarrowResult r = s.run(true, entry_is_hooked);
    REQUIRE(r.hooks.size() == 1);
    CHECK(r.decisions[0].reason == NarrowReason::TargetChanged);
}

TEST_CASE("elision: the in-place addend is part of the target") {
    QuietLogs quiet;
    const std::shared_ptr<const CodeImage> prior = first_patch(kBarVa, -4);
    Scenario s;
    call_bar(s, kBarVa, 0);
    s.prior = [&](std::uint64_t) { return prior; };
    const NarrowResult r = s.run(true, entry_is_hooked);
    REQUIRE(r.hooks.size() == 1);
    CHECK(r.decisions[0].reason == NarrowReason::TargetChanged);
}

TEST_CASE("elision: a string literal is compared by content between patches") {
    QuietLogs quiet;
    const auto build = [](bool same, Scenario& s) {
        s.new_code({0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3});
        const std::uint32_t sec = s.obj.section(".rdata$r", kRdata | coff::scn::LnkComdat, 8, coff::ComdatSelection::Any);
        const std::uint32_t lit = s.obj.symbol("??_C@_05literal@", 0, static_cast<std::int32_t>(sec), false, true);
        coff::ModelSection& ms = s.obj.model().sections[sec - 1];
        const char text[8] = {'h', 'e', 'l', same ? 'l' : 'p', 'o', 0, 0, 0};
        for (int i = 0; i < 8; ++i) {
            ms.data[static_cast<std::size_t>(i)] = static_cast<std::byte>(text[i]);
        }
        s.obj.reloc(s.foo.section, 3, lit, kRel32, 0);
        s.hook();
        SymbolDecision& d = s.decision(lit, "??_C@_05literal@", DecisionKind::PatchFresh);
        d.defined = true;
        d.type = SymbolType::Data;
        d.section = sec;
        d.value = 0;
    };
    Scenario first;
    build(true, first);
    const std::shared_ptr<const CodeImage> prior = first.capture();
    REQUIRE(prior->relocs.size() == 1);
    CHECK(prior->relocs[0].target.kind == CodeTarget::Kind::Constant);
    SUBCASE("same literal") {
        Scenario s;
        build(true, s);
        s.prior = [&](std::uint64_t) { return prior; };
        const NarrowResult r = s.run(true, entry_is_hooked);
        CHECK_MESSAGE(r.decisions[0].reason == NarrowReason::Unchanged, to_string(r.decisions[0].reason) << " " << r.decisions[0].detail);
    }
    SUBCASE("edited literal") {
        Scenario s;
        build(false, s);
        s.prior = [&](std::uint64_t) { return prior; };
        const NarrowResult r = s.run(true, entry_is_hooked);
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::TargetChanged);
    }
}

TEST_CASE("elision: a reference to a symbol the earlier patch introduced is compared by address") {
    QuietLogs quiet;
    // Patch 1 defines `new_helper` and calls it (NewSymbol); once laid out it lives at kHelperVa.
    constexpr std::uint64_t kHelperVa = 0x7FF500001000ull;
    Scenario first;
    first.new_code({0xE8, 0, 0, 0, 0, 0xC3});
    const std::uint32_t helper = first.obj.undefined("new_helper", true);
    first.obj.reloc(first.foo.section, 1, helper, kRel32, 0);
    first.hook();
    first.decision(helper, "new_helper", DecisionKind::PatchNewFunction);
    const std::shared_ptr<const CodeImage> prior_raw = first.capture();
    REQUIRE(prior_raw->relocs.size() == 1);
    CHECK(prior_raw->relocs[0].target.kind == CodeTarget::Kind::NewSymbol);
    CodeImage unresolved = *prior_raw;
    CHECK_FALSE(finalize_code(unresolved, [](const std::string&) { return std::optional<std::uint64_t>(); }));
    CodeImage laid_out = *prior_raw;
    CHECK(finalize_code(laid_out, [&](const std::string& name) -> std::optional<std::uint64_t> {
        return name == "new_helper" ? std::optional<std::uint64_t>(kHelperVa) : std::nullopt;
    }));
    REQUIRE(laid_out.relocs.size() == 1);
    CHECK(laid_out.relocs[0].target.kind == CodeTarget::Kind::Address);
    CHECK(laid_out.relocs[0].target.value == kHelperVa);
    const std::shared_ptr<const CodeImage> prior = std::make_shared<const CodeImage>(laid_out);

    SUBCASE("patch 2 binds it through the registry to the same address: dropped") {
        Scenario s;
        call_bar(s, kHelperVa);
        s.prior = [&](std::uint64_t) { return prior; };
        CHECK(s.run(true, entry_is_hooked).hooks.empty());
    }
    SUBCASE("it resolves elsewhere: kept") {
        Scenario s;
        call_bar(s, kHelperVa + 0x40);
        s.prior = [&](std::uint64_t) { return prior; };
        const NarrowResult r = s.run(true, entry_is_hooked);
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::TargetChanged);
    }
    SUBCASE("patch 2 introduces another new symbol: kept") {
        Scenario s;
        s.new_code({0xE8, 0, 0, 0, 0, 0xC3});
        const std::uint32_t other = s.obj.undefined("other_helper", true);
        s.obj.reloc(s.foo.section, 1, other, kRel32, 0);
        s.hook();
        s.decision(other, "other_helper", DecisionKind::PatchNewFunction);
        s.prior = [&](std::uint64_t) { return prior; };
        const NarrowResult r = s.run(true, entry_is_hooked);
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::TargetNew);
    }
}

TEST_CASE("elision: a function an earlier patch introduced (no old entry in the module file) is compared with that patch") {
    QuietLogs quiet;
    const std::shared_ptr<const CodeImage> prior = first_patch();
    const std::uint64_t entry = 0x7FF500002000ull;  // the function's address in patch 1
    SUBCASE("unchanged") {
        Scenario s;
        call_bar(s, kBarVa);
        s.plan.hooks[0].old_rva = bind::kNoRva;
        s.plan.hooks[0].old_va = entry;
        s.plan.hooks[0].old_size = 0;
        s.prior = [&](std::uint64_t va) { return va == entry ? prior : nullptr; };
        CHECK(s.run().hooks.empty());
    }
    SUBCASE("nothing recorded: kept as before") {
        Scenario s;
        call_bar(s, kBarVa);
        s.plan.hooks[0].old_rva = bind::kNoRva;
        s.plan.hooks[0].old_va = entry;
        s.prior = [](std::uint64_t) { return std::shared_ptr<const CodeImage>(); };
        const NarrowResult r = s.run();
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::OldBytesUnavailable);
    }
}

TEST_CASE("elision: nothing recorded for a hooked entry keeps it; narrowing off keeps it too") {
    QuietLogs quiet;
    const std::shared_ptr<const CodeImage> prior = first_patch();
    SUBCASE("no record") {
        Scenario s;
        call_bar(s, kBarVa);
        s.prior = [](std::uint64_t) { return std::shared_ptr<const CodeImage>(); };
        const NarrowResult r = s.run(true, entry_is_hooked);
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::AlreadyHooked);
    }
    SUBCASE("narrowing off") {
        Scenario s;
        call_bar(s, kBarVa);
        s.prior = [&](std::uint64_t) { return prior; };
        const NarrowResult r = s.run(false, entry_is_hooked);
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::Disabled);
    }
}

TEST_CASE("elision: a function that cannot be recorded (pointer-holding constant) yields no record") {
    QuietLogs quiet;
    Scenario s;
    s.new_code({0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3});
    const std::uint32_t sec = s.obj.section(".rdata", kRdata | coff::scn::LnkComdat, 16, coff::ComdatSelection::Any);
    const std::uint32_t vt = s.obj.symbol("??_7Foo@@6B@", 0, static_cast<std::int32_t>(sec), false, true);
    const std::uint32_t target = s.obj.undefined("Foo::f", true);
    s.obj.reloc(sec, 8, target, kAddr64, 0);
    s.obj.reloc(s.foo.section, 3, vt, kRel32, 0);
    s.hook();
    SymbolDecision& d = s.decision(vt, "??_7Foo@@6B@", DecisionKind::PatchFresh);
    d.defined = true;
    d.type = SymbolType::Data;
    d.section = sec;
    Built built = s.obj.build();
    s.keep = std::move(built);
    NarrowInput in;
    in.object = &s.keep.obj();
    in.plan = &s.plan;
    in.load_base = kPreferred;
    std::string why;
    CHECK_FALSE(capture_code(in, "?foo@@YAHXZ", &why).has_value());
    CHECK(why.find("holds pointers") != std::string::npos);
}

// ---- WP-25 review: callers that still call the earlier patch's copy of a callee, counts, bound data ----------------------

namespace {

constexpr std::uint32_t kInnerRva = 0x1180;  // inside the synthetic module's .text
constexpr std::uint64_t kInnerVa = kPreferred + kInnerRva;

// Two functions in one object: `?foo@@YAHXZ` (outer) calls `?inner@@YAHXZ`, both hooked by the planner. The old module's inner
// is `mov eax, 1 ; ret` at kInnerRva; `inner_code` is what this object's inner holds.
struct Chain {
    Scenario s;
    SynthObj::Func inner;

    explicit Chain(const std::vector<std::uint8_t>& inner_code) {
        inner = s.obj.function("?inner@@YAHXZ", true, 16);
        coff::ModelSection& is = s.obj.model().sections[inner.section - 1];
        for (std::byte& b : is.data) {
            b = std::byte{0xCC};
        }
        for (std::size_t i = 0; i < inner_code.size(); ++i) {
            is.data[i] = static_cast<std::byte>(inner_code[i]);
        }
        s.new_code({0xE8, 0, 0, 0, 0, 0xC3});
        s.obj.reloc(s.foo.section, 1, inner.symbol, kRel32, 0);
        s.hook();  // outer
        SymbolDecision& d = s.decision(inner.symbol, "?inner@@YAHXZ", DecisionKind::PatchHooked);
        d.defined = true;
        d.type = SymbolType::Function;
        d.section = inner.section;
        d.value = 0;
        d.old_address.valid = true;
        d.old_address.va = kInnerVa;
        d.old_address.rva = kInnerRva;
        HookEntry h;
        h.new_symbol = "?inner@@YAHXZ";
        h.display = "inner";
        h.old_va = kInnerVa;
        h.old_rva = kInnerRva;
        h.old_size = 16;
        s.plan.hooks.push_back(h);
        const std::vector<std::uint8_t> old_inner = {0xB8, 1, 0, 0, 0, 0xC3};
        for (std::size_t i = 0; i < old_inner.size(); ++i) {
            s.disk[text_off(kInnerRva) + i] = old_inner[i];
        }
    }
};

const std::vector<std::uint8_t> kInnerAsOld = {0xB8, 1, 0, 0, 0, 0xC3};
const std::vector<std::uint8_t> kInnerChanged = {0xB8, 7, 0, 0, 0, 0xC3};

}  // namespace

TEST_CASE("elision: a caller whose earlier-patch copy calls a callee that is hooked again is hooked again too") {
    QuietLogs quiet;
    // Patch 1 changed `outer` only: its code calls patch 1's own copy of `inner`.
    Chain first(kInnerAsOld);
    const std::shared_ptr<const CodeImage> prior = first.s.capture();
    REQUIRE(prior->relocs.size() == 1);
    CHECK(prior->relocs[0].target.local_callee == "?inner@@YAHXZ");

    SUBCASE("patch 2 changes inner: outer must be hooked again, or it goes on calling the stale copy") {
        Chain second(kInnerChanged);
        second.s.prior = [&](std::uint64_t va) { return entry_is_hooked(va) ? prior : nullptr; };
        const NarrowResult r = second.s.run(true, entry_is_hooked);
        REQUIRE(r.decisions.size() == 2);
        CHECK(r.decisions[1].reason == NarrowReason::BytesDiffer);  // inner
        CHECK_MESSAGE(r.decisions[0].reason == NarrowReason::TargetChanged, to_string(r.decisions[0].reason));
        CHECK(r.decisions[0].detail.find("inner") != std::string::npos);
        CHECK(r.hooks.size() == 2);
        CHECK(r.dropped == 0);
    }
    SUBCASE("patch 2 leaves inner alone: outer stays dropped") {
        Chain second(kInnerAsOld);
        second.s.prior = [&](std::uint64_t va) { return entry_is_hooked(va) ? prior : nullptr; };
        const NarrowResult r = second.s.run(true, entry_is_hooked);
        REQUIRE(r.decisions.size() == 2);
        CHECK(r.decisions[0].reason == NarrowReason::Unchanged);
        CHECK(r.decisions[1].reason == NarrowReason::Unchanged);
        CHECK(r.hooks.empty());
        CHECK(r.planned == 2);
        CHECK(r.dropped == 2);
    }
}

TEST_CASE("elision: counts are per old entry: plan hooks that share an entry count once") {
    QuietLogs quiet;
    Scenario s;
    s.old_code({0xB8, 1, 0, 0, 0, 0xC3});
    s.new_code({0xB8, 1, 0, 0, 0, 0xC3});
    s.hook();  // foo: identical to the old code, dropped
    HookEntry twin = s.plan.hooks[0];
    twin.new_symbol = "?twin@@YAHXZ";  // no definition in the object: cannot be judged, kept
    twin.display = "twin";
    s.plan.hooks.push_back(twin);
    const NarrowResult r = s.run();
    REQUIRE(r.decisions.size() == 2);
    CHECK(r.decisions[0].reason == NarrowReason::Unchanged);
    CHECK(r.decisions[1].reason == NarrowReason::Unverifiable);
    CHECK(r.hooks.size() == 1);
    CHECK(r.planned == 1);
    CHECK(r.dropped == 0);
    CHECK(r.planned - r.dropped == r.hooks.size());

    SUBCASE("two different entries, one dropped") {
        Scenario t;
        t.old_code({0xB8, 1, 0, 0, 0, 0xC3});
        t.new_code({0xB8, 1, 0, 0, 0, 0xC3});
        t.hook();
        HookEntry other = t.plan.hooks[0];
        other.new_symbol = "?other@@YAHXZ";
        other.old_va += 0x100;
        t.plan.hooks.push_back(other);
        const NarrowResult two = t.run();
        CHECK(two.planned == 2);
        CHECK(two.dropped == 1);
        CHECK(two.hooks.size() == 1);
    }
}

TEST_CASE("elision: data the old image owns whose initial value changed is reported; equal data and uninitialised data are not") {
    QuietLogs quiet;
    const auto build = [](Scenario& s, bool same) {
        const std::uint32_t sec = s.obj.section(".rdata$g", kRdata | coff::scn::LnkComdat, 8, coff::ComdatSelection::Any);
        const std::uint32_t g = s.obj.symbol("?g_const@@3HB", 0, static_cast<std::int32_t>(sec), false, true);
        coff::ModelSection& ms = s.obj.model().sections[sec - 1];
        for (int i = 0; i < 8; ++i) {
            ms.data[static_cast<std::size_t>(i)] = static_cast<std::byte>(i == 3 && !same ? 9 : i);
        }
        std::uint8_t old_data[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        std::memcpy(s.disk.data() + cml_test::rdata_off(kConstRva), old_data, 8);
        SymbolDecision& d = s.decision(g, "?g_const@@3HB", DecisionKind::BindOldData);
        d.defined = true;
        d.type = SymbolType::Data;
        d.section = sec;
        d.value = 0;
        d.old_address.valid = true;
        d.old_address.va = kPreferred + kConstRva;
        d.old_address.rva = kConstRva;
        d.display = "g_const";
    };
    SUBCASE("changed") {
        Scenario s;
        build(s, false);
        const std::vector<std::string> changed = s.data_changes();
        REQUIRE(changed.size() == 1);
        CHECK(changed[0] == "g_const");
    }
    SUBCASE("equal") {
        Scenario s;
        build(s, true);
        CHECK(s.data_changes().empty());
    }
    SUBCASE("the old bytes cannot be read: not reported") {
        Scenario s;
        build(s, false);
        s.plan.decisions.back().old_address.rva = 0x9000;  // outside the file's sections
        CHECK(s.data_changes().empty());
    }
}
