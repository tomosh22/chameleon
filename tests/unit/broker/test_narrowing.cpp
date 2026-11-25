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
    // WP-49: `foo` at the start of a plain (non-COMDAT) code section of `section_size` bytes; its extent ends at the next symbol.
    Scenario(std::uint32_t section_size, bool /*plain section*/) {
        foo.section = obj.section(".text$mn", kText, section_size);
        foo.symbol = obj.symbol("?foo@@YAHXZ", 0, static_cast<std::int32_t>(foo.section), true);
    }

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

// ---- WP-36 (ADR-0042): image-relative displacements to old data (Addr32NbDisp) -----------------------------------

namespace {

constexpr std::uint32_t kImgrelDataRva = 0x2980;  // the old data `[__ImageBase + index*4 + disp32]` reaches

// foo: mov eax, [rdi+rax*4 + g_tick@IMGREL + addend] ; ret.  The old linker wrote RVA(g_tick) + addend into the displacement.
// `planned`: the planner proved the site and retargets it (an Addr32NbDisp fix-up); `g_va`: what g_tick is bound to.
void imgrel_load(Scenario& s, std::uint64_t g_va, std::int32_t addend = 0x10, bool planned = true, std::uint32_t old_rva = kImgrelDataRva) {
    s.old_code({0x8B, 0x84, 0x87, 0, 0, 0, 0, 0xC3});
    put32(s.disk, text_off(kFnRva) + 3, old_rva + static_cast<std::uint32_t>(addend));
    const std::uint32_t a = static_cast<std::uint32_t>(addend);
    s.new_code({0x8B, 0x84, 0x87, static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(a >> 8), static_cast<std::uint8_t>(a >> 16),
                static_cast<std::uint8_t>(a >> 24), 0xC3});
    const std::uint32_t g = s.obj.undefined("g_tick", false);
    s.obj.reloc(s.foo.section, 3, g, kAddr32Nb, addend);
    s.hook();
    SymbolDecision& d = s.decision(g, "g_tick", DecisionKind::BindOldData);
    d.old_address.valid = true;
    d.old_address.va = g_va;
    if (planned) {
        Fixup f;
        f.section_index = s.foo.section;
        f.offset = 3;
        f.kind = FixupKind::Addr32NbDisp;
        f.addend = addend;
        f.target_va = g_va;
        s.plan.fixups.push_back(f);
    }
}

}  // namespace

TEST_CASE("narrowing (WP-36): an image-relative displacement to bound data is judged like any bound-data site") {
    QuietLogs quiet;
    SUBCASE("same target: dropped") {
        Scenario s;
        imgrel_load(s, kPreferred + kImgrelDataRva);
        const NarrowResult r = s.run();
        REQUIRE(r.decisions.size() == 1);
        CHECK_MESSAGE(r.decisions[0].reason == NarrowReason::Unchanged, to_string(r.decisions[0].reason) << " " << r.decisions[0].detail);
        CHECK(r.hooks.empty());
    }
    SUBCASE("a negative addend, and the module loaded away from its preferred base") {
        Scenario s;
        s.load_base = 0x7FF600000000ull;
        imgrel_load(s, s.load_base + kImgrelDataRva, -8);
        const NarrowResult r = s.run();
        CHECK_MESSAGE(r.hooks.empty(), to_string(r.decisions[0].reason) << " " << r.decisions[0].detail);
    }
    SUBCASE("the old code reached other data: kept") {
        Scenario s;
        imgrel_load(s, kPreferred + kImgrelDataRva, 0x10, true, kImgrelDataRva + 0x40);
        const NarrowResult r = s.run();
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::TargetChanged);
    }
    SUBCASE("the addend differs from the old code's: kept") {
        Scenario s;
        imgrel_load(s, kPreferred + kImgrelDataRva);
        put32(s.disk, text_off(kFnRva) + 3, kImgrelDataRva + 0x14);  // the old code read the next element
        const NarrowResult r = s.run();
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::TargetChanged);
    }
    SUBCASE("an ADDR32NB the planner did not retarget (a jump-table load, a thunk) is not judged: kept") {
        Scenario s;
        imgrel_load(s, kPreferred + kImgrelDataRva, 0x10, /*planned=*/false);
        const NarrowResult r = s.run();
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::Unverifiable);
    }
    SUBCASE("an ADDR32NB to a bound FUNCTION goes through the patch's thunk: kept") {
        Scenario s;
        s.old_code({0x48, 0x8D, 0x84, 0x0F, 0, 0, 0, 0, 0xC3});  // lea rax, bar[rdi+rcx]
        put32(s.disk, text_off(kFnRva) + 4, kBarRva);
        s.new_code({0x48, 0x8D, 0x84, 0x0F, 0, 0, 0, 0, 0xC3});
        const std::uint32_t bar = s.obj.undefined("bar", true);
        s.obj.reloc(s.foo.section, 4, bar, kAddr32Nb, 0);
        s.hook();
        SymbolDecision& d = s.decision(bar, "bar", DecisionKind::BindOldFunction);
        d.old_address.valid = true;
        d.old_address.va = kPreferred + kBarRva;
        const NarrowResult r = s.run();
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::Unverifiable);
    }
}

TEST_CASE("elision (WP-36): the record of an earlier patch holds the address an image-relative displacement reaches") {
    QuietLogs quiet;
    const auto record = [](std::uint64_t g_va, std::int32_t addend) {
        Scenario first;
        imgrel_load(first, g_va, addend);
        return first.capture();
    };
    const std::shared_ptr<const CodeImage> prior = record(kPreferred + kImgrelDataRva, 0x10);
    REQUIRE(prior->relocs.size() == 1);
    CHECK(prior->relocs[0].offset == 3);
    CHECK(prior->relocs[0].type == static_cast<std::uint16_t>(coff::RelocType::Addr32Nb));
    CHECK(prior->relocs[0].addend == 0x10);
    CHECK(prior->relocs[0].target.kind == CodeTarget::Kind::Address);
    CHECK(prior->relocs[0].target.value == kPreferred + kImgrelDataRva);
    // The site is zeroed in the recorded bytes.
    CHECK(prior->bytes[3] == 0);

    SUBCASE("unchanged in the next patch: dropped") {
        Scenario s;
        imgrel_load(s, kPreferred + kImgrelDataRva, 0x10);
        s.prior = [&](std::uint64_t va) { return entry_is_hooked(va) ? prior : nullptr; };
        const NarrowResult r = s.run(true, entry_is_hooked);
        REQUIRE(r.decisions.size() == 1);
        CHECK_MESSAGE(r.decisions[0].reason == NarrowReason::Unchanged, to_string(r.decisions[0].reason) << " " << r.decisions[0].detail);
        CHECK(r.hooks.empty());
    }
    SUBCASE("another element (addend) or another target: kept") {
        Scenario s;
        imgrel_load(s, kPreferred + kImgrelDataRva, 0x14);
        s.prior = [&](std::uint64_t) { return prior; };
        NarrowResult r = s.run(true, entry_is_hooked);
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::TargetChanged);

        Scenario t;
        imgrel_load(t, kPreferred + kImgrelDataRva + 0x100, 0x10);
        t.prior = [&](std::uint64_t) { return prior; };
        r = t.run(true, entry_is_hooked);
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::TargetChanged);
    }
    SUBCASE("a site the planner did not retarget yields no record: the function is hooked every time") {
        Scenario s;
        imgrel_load(s, kPreferred + kImgrelDataRva, 0x10, /*planned=*/false);
        Built built = s.obj.build();
        NarrowInput in;
        in.object = &built.obj();
        in.plan = &s.plan;
        in.load_base = s.load_base;
        std::string why;
        CHECK_FALSE(capture_code(in, "?foo@@YAHXZ", &why).has_value());
        CHECK(why.find("ADDR32NB") != std::string::npos);
    }
}

// ---- WP-49 (R-46, ADR-0044): jump tables in the function's own code ---------------------------------------------------
//
// The byte layouts are the ones cl writes for a dense `switch` (x64, /Gy, one COMDAT section per function): `lea reg,
// [__ImageBase]` (REL32), the table load `mov r32, [reg + rax*4 + $LNt]` (ADDR32NB to the table's label), `add`, `jmp`, the
// cases, and behind the code, still inside the function (the PDB's size includes it), one ADDR32NB entry per case label.
// tests/unit/broker/test_jump_tables.cpp runs the same rules on objects the cell's compiler really wrote.

namespace {

struct SwitchCode {
    const char* what = "";
    std::vector<std::uint8_t> bytes;
    std::uint32_t image_base_site = 0;  // lea reg, [__ImageBase]: REL32
    std::uint32_t load_site = 0;        // the table load: ADDR32NB to the table
    std::uint32_t table = 0;            // the first entry
    std::vector<std::uint32_t> cases;   // the case label of each entry
    std::uint32_t size() const { return static_cast<std::uint32_t>(bytes.size()); }
};

// cl /Od: int f(int k) { switch (k) { case 0: return 10; case 1: return 20; case 2: return 30; default: return 0; } }
SwitchCode cl_od_switch() {
    SwitchCode c;
    c.what = "cl /Od";
    c.bytes = {0x89, 0x54, 0x24, 0x10,               // 00 mov [rsp+10h], edx
               0x89, 0x4C, 0x24, 0x08,               // 04 mov [rsp+8], ecx
               0x48, 0x83, 0xEC, 0x18,               // 08 sub rsp, 18h
               0x8B, 0x44, 0x24, 0x20,               // 0C mov eax, [rsp+20h]
               0x89, 0x04, 0x24,                     // 10 mov [rsp], eax
               0x83, 0x3C, 0x24, 0x02,               // 13 cmp dword ptr [rsp], 2
               0x77, 0x2C,                           // 17 ja $LN7 (default)
               0x48, 0x63, 0x04, 0x24,               // 19 movsxd rax, dword ptr [rsp]
               0x48, 0x8D, 0x0D, 0, 0, 0, 0,         // 1D lea rcx, [__ImageBase]
               0x8B, 0x84, 0x81, 0, 0, 0, 0,         // 24 mov eax, [rcx+rax*4+$LN9]
               0x48, 0x03, 0xC1,                     // 2B add rax, rcx
               0xFF, 0xE0,                           // 2E jmp rax
               0xB8, 0x0A, 0, 0, 0, 0xEB, 0x10,      // 30 $LN4: mov eax, 10 ; jmp $LN2
               0xB8, 0x14, 0, 0, 0, 0xEB, 0x09,      // 37 $LN5: mov eax, 20 ; jmp $LN2
               0xB8, 0x1E, 0, 0, 0, 0xEB, 0x02,      // 3E $LN6: mov eax, 30 ; jmp $LN2
               0x33, 0xC0,                           // 45 $LN7: xor eax, eax
               0x48, 0x83, 0xC4, 0x18,               // 47 $LN2: add rsp, 18h
               0xC3,                                 // 4B ret
               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // 4C $LN9: DD $LN4, $LN5, $LN6
    c.image_base_site = 0x20;
    c.load_site = 0x27;
    c.table = 0x4C;
    c.cases = {0x30, 0x37, 0x3E};
    return c;
}

// cl /O2: the same switch.
SwitchCode cl_o2_switch() {
    SwitchCode c;
    c.what = "cl /O2";
    c.bytes = {0x83, 0xF9, 0x02,                     // 00 cmp ecx, 2
               0x77, 0x28,                           // 03 ja $LN7
               0x48, 0x63, 0xC1,                     // 05 movsxd rax, ecx
               0x48, 0x8D, 0x15, 0, 0, 0, 0,         // 08 lea rdx, [__ImageBase]
               0x8B, 0x8C, 0x82, 0, 0, 0, 0,         // 0F mov ecx, [rdx+rax*4+$LN9]
               0x48, 0x03, 0xCA,                     // 16 add rcx, rdx
               0xFF, 0xE1,                           // 19 jmp rcx
               0xB8, 0x0A, 0, 0, 0, 0xC3,            // 1B $LN4: mov eax, 10 ; ret
               0xB8, 0x14, 0, 0, 0, 0xC3,            // 21 $LN5: mov eax, 20 ; ret
               0xB8, 0x1E, 0, 0, 0, 0xC3,            // 27 $LN6: mov eax, 30 ; ret
               0x33, 0xC0, 0xC3,                     // 2D $LN7: xor eax, eax ; ret
               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // 30 $LN9: DD $LN4, $LN5, $LN6
    c.image_base_site = 0x0B;
    c.load_site = 0x12;
    c.table = 0x30;
    c.cases = {0x1B, 0x21, 0x27};
    return c;
}

struct TableEdit {
    std::vector<std::uint32_t> new_cases;  // the entries of the NEW table (default: the code's)
    std::vector<std::uint32_t> old_cases;  // the entries the OLD linker wrote, as offsets in the function (default: the same)
    std::uint32_t old_table = 0;           // where the old table load pointed (0: the table)
    bool labels = true;                    // case labels as `$LNn` symbols; false: `foo` + in-place addend (one symbol)
};

TableEdit new_entries(std::vector<std::uint32_t> cases) {
    TableEdit e;
    e.new_cases = std::move(cases);
    return e;
}
TableEdit old_entries(std::vector<std::uint32_t> cases) {
    TableEdit e;
    e.old_cases = std::move(cases);
    return e;
}
TableEdit old_load(std::uint32_t table) {
    TableEdit e;
    e.old_table = table;
    return e;
}
TableEdit one_symbol() {
    TableEdit e;
    e.labels = false;
    return e;
}

// The new object (foo = the switch, its labels, __ImageBase) and the old image at kFnRva, linked from the same code.
void jump_table(Scenario& s, const SwitchCode& c, const TableEdit& e = {}) {
    const std::vector<std::uint32_t> new_cases = e.new_cases.empty() ? c.cases : e.new_cases;
    const std::vector<std::uint32_t> old_cases = e.old_cases.empty() ? c.cases : e.old_cases;
    s.new_code(c.bytes);
    s.old_code(c.bytes);
    put32(s.disk, text_off(kFnRva) + c.image_base_site, static_cast<std::uint32_t>(-static_cast<std::int32_t>(kFnRva + c.image_base_site + 4)));
    put32(s.disk, text_off(kFnRva) + c.load_site, kFnRva + (e.old_table != 0 ? e.old_table : c.table));
    for (std::size_t i = 0; i < old_cases.size(); ++i) {
        put32(s.disk, text_off(kFnRva) + c.table + 4 * i, kFnRva + old_cases[i]);
    }
    const std::uint32_t sec = s.foo.section;
    const std::uint32_t base = s.obj.undefined("__ImageBase", false);
    s.obj.reloc(sec, c.image_base_site, base, kRel32);
    std::vector<std::pair<std::uint32_t, std::string>> labels;
    const auto label = [&](std::uint32_t at, const std::string& name) {
        const std::uint32_t sym = s.obj.symbol(name, at, static_cast<std::int32_t>(sec), false, false);
        labels.emplace_back(sym, name);
        return sym;
    };
    if (e.labels) {
        s.obj.reloc(sec, c.load_site, label(c.table, "$LN9"), kAddr32Nb);
        for (std::size_t i = 0; i < new_cases.size(); ++i) {
            s.obj.reloc(sec, static_cast<std::uint32_t>(c.table + 4 * i), label(new_cases[i], "$LN" + std::to_string(4 + i)), kAddr32Nb);
        }
    } else {
        s.obj.reloc(sec, c.load_site, s.foo.symbol, kAddr32Nb, c.table);
        for (std::size_t i = 0; i < new_cases.size(); ++i) {
            s.obj.reloc(sec, static_cast<std::uint32_t>(c.table + 4 * i), s.foo.symbol, kAddr32Nb, new_cases[i]);
        }
    }
    s.hook(c.size());
    s.decision(base, "__ImageBase", DecisionKind::LinkerProvided);
    for (const auto& [sym, name] : labels) {  // as the planner decides cl's labels: patch-local, never bound
        SymbolDecision& d = s.decision(sym, name, DecisionKind::PatchFresh);
        d.defined = true;
        d.is_static = true;
        d.section = sec;
    }
}

const NarrowDecision& only(const NarrowResult& r) {
    REQUIRE(r.decisions.size() == 1);
    return r.decisions[0];
}

void set_le32(coff::ModelSection& sec, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) {
        sec.data[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}

}  // namespace

TEST_CASE("narrowing (WP-49): an identical jump table in the function's own code is dropped (cl /Od and /O2 layouts)") {
    QuietLogs quiet;
    for (const SwitchCode& c : {cl_od_switch(), cl_o2_switch()}) {
        CAPTURE(c.what);
        {
            Scenario s(c.size());
            jump_table(s, c);
            const NarrowResult r = s.run();
            CHECK_MESSAGE(only(r).reason == NarrowReason::Unchanged, to_string(only(r).reason) << " " << only(r).detail);
            CHECK(r.hooks.empty());
            CHECK(r.dropped == 1);
        }
        {
            // The module loaded away from its preferred base: the entries are RVAs, the comparison is relative to the function.
            Scenario s(c.size());
            s.load_base = 0x7FF600000000ull;
            jump_table(s, c);
            const NarrowResult r = s.run();
            CHECK_MESSAGE(only(r).reason == NarrowReason::Unchanged, to_string(only(r).reason) << " " << only(r).detail);
        }
        {
            // The entries name the function's own symbol with the offset in place (one symbol for every label).
            Scenario s(c.size());
            jump_table(s, c, one_symbol());
            const NarrowResult r = s.run();
            CHECK_MESSAGE(only(r).reason == NarrowReason::Unchanged, to_string(only(r).reason) << " " << only(r).detail);
        }
    }
}

TEST_CASE("narrowing (WP-49): a changed case target, or a table load that reached elsewhere, keeps the hook") {
    QuietLogs quiet;
    for (const SwitchCode& c : {cl_od_switch(), cl_o2_switch()}) {
        CAPTURE(c.what);
        {
            // `case 1:` and `case 2:` exchanged in the source: the same bytes, only two entries of the table differ.
            Scenario s(c.size());
            jump_table(s, c, new_entries({c.cases[0], c.cases[2], c.cases[1]}));
            const NarrowResult r = s.run();
            REQUIRE(r.hooks.size() == 1);
            CHECK(only(r).reason == NarrowReason::TargetChanged);
            CHECK_MESSAGE(only(r).detail.find("jump-table reference at byte " + std::to_string(c.table + 4) + " ") != std::string::npos, only(r).detail);
        }
        {
            Scenario s(c.size());
            jump_table(s, c, old_load(c.table + 4));
            const NarrowResult r = s.run();
            REQUIRE(r.hooks.size() == 1);
            CHECK(only(r).reason == NarrowReason::TargetChanged);
            CHECK_MESSAGE(only(r).detail.find("jump-table reference at byte " + std::to_string(c.load_site) + " ") != std::string::npos, only(r).detail);
        }
        {
            // The last entry of the old table pointed one byte further: kept (every entry is compared, not only the first).
            Scenario s(c.size());
            jump_table(s, c, old_entries({c.cases[0], c.cases[1], c.cases[2] + 1}));
            const NarrowResult r = s.run();
            REQUIRE(r.hooks.size() == 1);
            CHECK(only(r).reason == NarrowReason::TargetChanged);
            CHECK_MESSAGE(only(r).detail.find("jump-table reference at byte " + std::to_string(c.table + 8) + " ") != std::string::npos, only(r).detail);
        }
    }
}

TEST_CASE("narrowing (WP-49): a table whose entries leave the function is not judged") {
    QuietLogs quiet;
    const SwitchCode c = cl_od_switch();
    {
        // The last entry reaches past the end of the function (here: past the end of its COMDAT section).
        Scenario s(c.size());
        jump_table(s, c);
        coff::ModelSection& sec = s.obj.model().sections[s.foo.section - 1];
        sec.relocations.back().symbol_index = s.foo.symbol;
        set_le32(sec, c.table + 8, c.size() + 0x10);
        put32(s.disk, text_off(kFnRva) + c.table + 8, kFnRva + c.size() + 0x10);
        const NarrowResult r = s.run();
        REQUIRE(r.hooks.size() == 1);
        CHECK(only(r).reason == NarrowReason::Unverifiable);
        CHECK_MESSAGE(only(r).detail.find("outside the function") != std::string::npos, only(r).detail);
    }
    {
        // An entry names another function's section: not this function's code, judged as before (no retargeted displacement).
        Scenario s(c.size());
        jump_table(s, c);
        const SynthObj::Func bar = s.obj.function("?bar@@YAHXZ", true, 16);
        s.obj.model().sections[s.foo.section - 1].relocations.back().symbol_index = bar.symbol;
        SymbolDecision& d = s.decision(bar.symbol, "?bar@@YAHXZ", DecisionKind::BindOldFunction);
        d.old_address.valid = true;
        d.old_address.va = kPreferred + kBarRva;
        const NarrowResult r = s.run();
        REQUIRE(r.hooks.size() == 1);
        CHECK(only(r).reason == NarrowReason::Unverifiable);
    }
}

TEST_CASE("narrowing (WP-49): a table that runs to the function end with an ADDR32NB behind it is not judged (its end cannot be told)") {
    QuietLogs quiet;
    const SwitchCode c = cl_o2_switch();
    for (const bool entry_behind : {true, false}) {
        CAPTURE(entry_behind);
        // A plain code section: foo, then bar; foo's extent ends at bar, so the table ends exactly at foo's end.
        Scenario s(c.size() + 8, true);
        jump_table(s, c, one_symbol());
        s.obj.symbol("?bar@@YAHXZ", c.size(), static_cast<std::int32_t>(s.foo.section), true);
        if (entry_behind) {
            s.obj.reloc(s.foo.section, c.size(), s.foo.symbol, kAddr32Nb, c.cases[0]);  // one more entry, outside the bytes compared
        }
        const NarrowResult r = s.run();
        if (entry_behind) {
            REQUIRE(r.hooks.size() == 1);
            CHECK(only(r).reason == NarrowReason::Unverifiable);
            CHECK_MESSAGE(only(r).detail.find("its end cannot be told") != std::string::npos, only(r).detail);
        } else {
            CHECK_MESSAGE(only(r).reason == NarrowReason::Unchanged, to_string(only(r).reason) << " " << only(r).detail);
        }
    }
}

TEST_CASE("narrowing (WP-49): a reference to a place in the function that is not a jump table is not judged (index tables)") {
    QuietLogs quiet;
    const SwitchCode c = cl_o2_switch();
    Scenario s(c.size());
    jump_table(s, c);
    // `mov eax, imm32` of case 0 now holds the RVA of the default label (as `movzx eax, byte ptr [rdx+rax+$LNi]` of a two-level
    // switch names a byte table, which has no relocations to compare).
    s.obj.reloc(s.foo.section, c.cases[0] + 1, s.foo.symbol, kAddr32Nb, 0x2D);
    put32(s.disk, text_off(kFnRva) + c.cases[0] + 1, kFnRva + 0x2D);
    const NarrowResult r = s.run();
    REQUIRE(r.hooks.size() == 1);
    CHECK(only(r).reason == NarrowReason::Unverifiable);
    CHECK_MESSAGE(only(r).detail.find("not the start of a jump table") != std::string::npos, only(r).detail);
}

TEST_CASE("narrowing (WP-49): a clang-cl jump table lives in .rdata and stays unverifiable") {
    QuietLogs quiet;
    // clang-cl -O2 (x64) for a six-case switch: `lea rdx, [rip + table]` (REL32 to the associative .rdata section) and
    // `movslq rcx, [rdx + rcx*4]`; the table holds REL32 entries to .text, relative to the table.
    const std::vector<std::uint8_t> code = {0x89, 0xD0, 0x83, 0xF9, 0x05, 0x77, 0x2A, 0x89, 0xC9, 0x48, 0x8D, 0x15, 0,    0,    0,    0,    0x48, 0x63,
                                            0x0C, 0x8A, 0x48, 0x01, 0xD1, 0xFF, 0xE1, 0x83, 0xC0, 0x03, 0xC3, 0xC1, 0xE0, 0x02, 0xC3, 0x83, 0xC0, 0xF9,
                                            0xC3, 0x83, 0xF0, 0x0B, 0xC3, 0x8D, 0x04, 0x80, 0xC3, 0x0F, 0xAF, 0xC0, 0xC3, 0xF7, 0xD8, 0xC3};
    for (const bool named : {false, true}) {
        CAPTURE(named);
        Scenario s(static_cast<std::uint32_t>(code.size()));
        s.new_code(code);
        s.old_code(code);
        const std::uint32_t rdata = s.obj.section(".rdata", kRdata, 0x18, coff::ComdatSelection::Associative, s.foo.section);
        const std::uint32_t table = s.obj.symbol("$jt0", 0, static_cast<std::int32_t>(rdata), false, false);
        s.obj.reloc(s.foo.section, 0x0C, table, kRel32);
        const std::uint32_t targets[6] = {0x19, 0x31, 0x2D, 0x35, 0x31, 0x45};
        for (std::uint32_t i = 0; i < 6; ++i) {
            s.obj.reloc(rdata, 4 * i, s.foo.symbol, kRel32, targets[i]);
        }
        s.hook(static_cast<std::uint32_t>(code.size()));
        if (named) {
            SymbolDecision& d = s.decision(table, "$jt0", DecisionKind::PatchFresh);  // a fresh constant: it holds pointers
            d.defined = true;
            d.type = SymbolType::Data;
            d.section = rdata;
        }
        const NarrowResult r = s.run();
        REQUIRE(r.hooks.size() == 1);
        CHECK_MESSAGE(only(r).reason == NarrowReason::Unverifiable, to_string(only(r).reason) << " " << only(r).detail);
    }
}

TEST_CASE("elision (WP-49): the record of an earlier patch holds each jump-table target relative to the function") {
    QuietLogs quiet;
    const SwitchCode c = cl_od_switch();
    Scenario first(c.size());
    jump_table(first, c);
    const std::shared_ptr<const CodeImage> prior = first.capture();
    REQUIRE(prior->relocs.size() == 5);
    CHECK(prior->relocs[0].target.kind == CodeTarget::Kind::Address);  // __ImageBase
    CHECK(prior->relocs[1].offset == c.load_site);
    CHECK(prior->relocs[1].target.kind == CodeTarget::Kind::Local);
    CHECK(prior->relocs[1].target.value == c.table);
    for (std::size_t i = 0; i < c.cases.size(); ++i) {
        CHECK(prior->relocs[2 + i].offset == c.table + 4 * i);
        CHECK(prior->relocs[2 + i].target.kind == CodeTarget::Kind::Local);
        CHECK(prior->relocs[2 + i].target.value == c.cases[i]);
    }
    {
        Scenario s(c.size());
        jump_table(s, c);
        s.prior = [&](std::uint64_t va) { return entry_is_hooked(va) ? prior : nullptr; };
        const NarrowResult r = s.run(true, entry_is_hooked);
        CHECK_MESSAGE(only(r).reason == NarrowReason::Unchanged, to_string(only(r).reason) << " " << only(r).detail);
        CHECK(only(r).from_prior);
        CHECK(r.hooks.empty());
    }
    {
        Scenario s(c.size());
        jump_table(s, c, new_entries({c.cases[0], c.cases[2], c.cases[1]}));
        s.prior = [&](std::uint64_t) { return prior; };
        const NarrowResult r = s.run(true, entry_is_hooked);
        REQUIRE(r.hooks.size() == 1);
        CHECK(only(r).reason == NarrowReason::TargetChanged);
        CHECK_MESSAGE(only(r).detail.find("earlier patch") != std::string::npos, only(r).detail);
    }
    {
        // A table that cannot be judged yields no record: the function is hooked every time, as before.
        Scenario s(c.size());
        jump_table(s, c);
        s.obj.reloc(s.foo.section, c.cases[0] + 1, s.foo.symbol, kAddr32Nb, 0x45);
        Built built = s.obj.build();
        NarrowInput in;
        in.object = &built.obj();
        in.plan = &s.plan;
        in.load_base = s.load_base;
        std::string why;
        CHECK_FALSE(capture_code(in, "?foo@@YAHXZ", &why).has_value());
        CHECK_MESSAGE(why.find("not the start of a jump table") != std::string::npos, why);
    }
}

TEST_CASE("narrowing (WP-49, R-50): a relocation that starts before the function and reaches into it is not judged") {
    QuietLogs quiet;
    for (const std::uint32_t site : {6u, 2u}) {
        CAPTURE(site);
        // A plain section: `pre` at 0, foo at 8 (mov eax, 1 ; ret ; int3...). A REL32 at 6 covers foo's first two bytes; one at
        // 2 ends at 6.
        Scenario s(24, true);
        for (coff::ModelSymbol& m : s.obj.model().symbols) {
            if (m.name == "?foo@@YAHXZ") {
                m.value = 8;
            }
        }
        s.obj.symbol("?pre@@YAHXZ", 0, static_cast<std::int32_t>(s.foo.section), true);
        std::vector<std::uint8_t> code = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
        code.resize(16, 0xCC);
        std::vector<std::uint8_t> bytes(8, 0x90);
        bytes.insert(bytes.end(), code.begin(), code.end());
        s.new_code(bytes);
        s.old_code(code);
        const std::uint32_t g = s.obj.undefined("g", false);
        s.obj.reloc(s.foo.section, site, g, kRel32);
        coff::ModelSection& sec = s.obj.model().sections[s.foo.section - 1];
        for (std::uint32_t i = 0; i < 4; ++i) {  // the in-place addend bytes are the code's
            sec.data[site + i] = static_cast<std::byte>(bytes[site + i]);
        }
        s.hook(16);
        for (SymbolDecision& d : s.plan.decisions) {
            if (d.name == "?foo@@YAHXZ") {
                d.value = 8;
            }
        }
        const NarrowResult r = s.run();
        if (site == 6) {
            REQUIRE(r.hooks.size() == 1);
            CHECK(only(r).reason == NarrowReason::Unverifiable);
            CHECK_MESSAGE(only(r).detail.find("before the function reaches into it") != std::string::npos, only(r).detail);
        } else {
            CHECK_MESSAGE(only(r).reason == NarrowReason::Unchanged, to_string(only(r).reason) << " " << only(r).detail);
        }
    }
}
