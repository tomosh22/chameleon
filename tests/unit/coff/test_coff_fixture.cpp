// Acceptance tests: cml_coff on the objects cl / clang-cl actually produce from fixtures `basic`
// and `bigobj` (paths come from the file written by CMake; see tests/unit/coff/CMakeLists.txt).
//
// Two sources of fixture objects are used:
//   * the fixture's own objects, compiled with the cell's configuration (Debug or
//     RelWithDebInfo /O2 /Ob0), and
//   * `cov_extras`: tests/fixtures/basic/extras.cpp compiled by the unit-test build with /Od in
//     every configuration, because an optimising compiler legitimately folds most of the
//     constructs extras.cpp exists to produce (clang-cl /O2 /Ob0 removes the inline and template
//     COMDATs, the static data, the string literal and the EH personality; see docs/wp/WP-02.md).
//
// Compiler differences that are asserted explicitly (cl vs clang-cl):
//   * true IMAGE_SYM_CLASS_WEAK_EXTERNAL for __attribute__((weak)): clang only;
//   * .CRT$XCU points straight at the ??__E... initialiser (cl) or at a static
//     _GLOBAL__sub_I_<file> function that calls it (clang);
//   * clang also emits .pdata/.xdata for leaf functions; cl does not.
#include "cml/coff/coff.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "fixture_paths.h"

using namespace cml::coff;
using cml_test::FixturePaths;

namespace {

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}
bool istarts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && FixturePaths::iequals(std::string(s.substr(0, prefix.size())), std::string(prefix));
}

bool config_is_debug() {
    return FixturePaths::get().config() == "Debug";
}

// Resolves an object path from the paths file by stem: matches "<stem>.obj" (Visual Studio
// generator) and "<stem>.cpp.obj" (Ninja).
std::string object_path(const std::string& list_key, const std::string& stem) {
    std::string p = FixturePaths::get().object(list_key, stem + ".obj");
    if (p.empty()) {
        p = FixturePaths::get().object(list_key, stem + ".cpp.obj");
    }
    return p;
}

const Object& load(const char* list_key, const char* stem) {
    static std::map<std::string, OwnedObject> cache;
    const std::string path = object_path(list_key, stem);
    REQUIRE_MESSAGE(!path.empty(), "object '" << stem << "' not found in " << list_key << ": " << FixturePaths::get().value(list_key));
    auto it = cache.find(path);
    if (it == cache.end()) {
        auto r = load_object_file(path);
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
        it = cache.emplace(path, std::move(r).value()).first;
    }
    return it->second.object;
}

const Symbol& need(const Object& o, std::string_view name) {
    const Symbol* s = o.find_symbol(name);
    REQUIRE_MESSAGE(s != nullptr, "symbol not found: " << name);
    return *s;
}

const Symbol* find_prefix(const Object& o, std::string_view prefix) {
    for (const Symbol& s : o.symbols()) {
        if (starts_with(s.name, prefix)) {
            return &s;
        }
    }
    return nullptr;
}

std::vector<ResolvedRelocation> resolved(const Object& o, const Section& s) {
    auto r = o.resolve_relocations(s);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    return std::move(r).value();
}

bool has_rel32_to(const Object& o, const Section& s, std::string_view target_name) {
    for (const ResolvedRelocation& r : resolved(o, s)) {
        if (is_rel32_family(r.reloc.type) && r.target->name == target_name) {
            return true;
        }
    }
    return false;
}

std::uint32_t read_u32(ByteSpan data, std::size_t off) {
    REQUIRE(off + 4 <= data.size());
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[off + i])) << (8 * i);
    }
    return v;
}

bool contains_bytes(ByteSpan data, std::string_view text) {
    if (text.size() > data.size()) {
        return false;
    }
    for (std::size_t i = 0; i + text.size() <= data.size(); ++i) {
        bool same = true;
        for (std::size_t k = 0; k < text.size() && same; ++k) {
            same = static_cast<char>(std::to_integer<std::uint8_t>(data[i + k])) == text[k];
        }
        if (same) {
            return true;
        }
    }
    return false;
}


// The symbol (reached through a REL32-family relocation from any section) that holds `text` at its
// address. cl without /GF names string literals $SG<n> (static, in a plain .rdata); with /GF or
// clang they are ??_C@_... COMDATs (selection Any).
const Symbol* find_string_symbol(const Object& o, std::string_view text) {
    for (const Section& sec : o.sections()) {
        if (!sec.is_code()) {
            continue;
        }
        for (const ResolvedRelocation& r : resolved(o, sec)) {
            if (!is_rel32_family(r.reloc.type)) {
                continue;
            }
            const Section* ts = o.section_of(*r.target);
            if (ts == nullptr || r.target->value + text.size() > ts->data.size()) {
                continue;
            }
            if (contains_bytes(ts->data.subspan(r.target->value, text.size()), text)) {
                return r.target;
            }
        }
    }
    return nullptr;
}

void check_string_symbol(const Object& o, std::string_view text) {
    const Symbol* lit = find_string_symbol(o, text);
    REQUIRE_MESSAGE(lit != nullptr, "no symbol referenced by a REL32 relocation holds \"" << text << "\"");
    const Section* sec = o.section_of(*lit);
    REQUIRE(sec != nullptr);
    CHECK_MESSAGE((starts_with(sec->name, ".rdata") || starts_with(sec->name, ".data")), "literal in " << sec->name);
    CHECK((sec->characteristics & scn::MemWrite) == 0u);
    if (sec->is_comdat()) {
        const auto c = o.comdat(*sec);
        REQUIRE(c.has_value());
        CHECK(c->selection == ComdatSelection::Any);
        REQUIRE(c->leader != nullptr);
        CHECK(c->leader->name == lit->name);
    }
}

// Facts about the static-CRT default-lib directive, shared by all objects.
void check_default_lib(const Object& o) {
    bool found = false;
    for (const Directive& d : o.directives()) {
        if (d.kind == DirectiveKind::DefaultLib && istarts_with(d.value, "libcmt")) {
            found = true;
        }
    }
    CHECK_MESSAGE(found, "no /DEFAULTLIB:LIBCMT[D] directive (the project uses the static CRT)");
}

}  // namespace

TEST_CASE("fixtures: the generated paths file lists every object") {
    const FixturePaths& p = FixturePaths::get();
    REQUIRE_MESSAGE(p.loaded(), "cannot read " << CML_TEST_PATHS_FILE);
    CHECK_FALSE(p.config().empty());
    for (const char* stem : {"main", "logic", "extras"}) {
        CHECK_MESSAGE(!object_path("basic_objs", stem).empty(), "missing basic object " << stem);
    }
    CHECK_MESSAGE(!object_path("cov_extras_objs", "extras").empty(), "missing cov_extras object");
    CHECK_MESSAGE(!object_path("bigobj_objs", "bigobj_funcs").empty(), "missing bigobj_funcs object");
    CHECK_MESSAGE(!object_path("bigobj_objs", "main").empty(), "missing bigobj main object");
}

TEST_CASE("fixture basic: logic.obj defines compute_value and references g_tick / fixture_note") {
    const Object& o = load("basic_objs", "logic");
    CHECK_FALSE(o.is_bigobj());
    CHECK(o.machine() == Machine::Amd64);

    const Symbol& fn = need(o, "?compute_value@@YAHXZ");
    CHECK_FALSE(fn.is_undefined());
    CHECK(fn.is_external());
    CHECK(fn.is_function());
    const Section* text = o.section_of(fn);
    REQUIRE(text != nullptr);
    CHECK(text->is_code());
    CHECK_MESSAGE(starts_with(text->name, ".text"), "section name " << text->name);

    // cl: RIP-relative accesses use REL32 or REL32_1..REL32_4 (the addend is folded into the type);
    // clang-cl uses REL32 with the addend in the instruction stream. Both are "the REL32 family".
    const Symbol& g_tick = need(o, "?g_tick@@3HA");
    const Symbol& note = need(o, "?fixture_note@@YAXPEBD@Z");
    CHECK(g_tick.is_undefined());
    CHECK(g_tick.is_external());
    CHECK(note.is_undefined());
    CHECK(note.is_external());
    CHECK(has_rel32_to(o, *text, "?g_tick@@3HA"));
    CHECK(has_rel32_to(o, *text, "?fixture_note@@YAXPEBD@Z"));

    // The string literal passed to fixture_note.
    check_string_symbol(o, "logic-start");

    check_default_lib(o);
}

TEST_CASE("fixture basic: main.obj defines g_tick, fixture_note and main") {
    const Object& o = load("basic_objs", "main");
    const Symbol& g_tick = need(o, "?g_tick@@3HA");
    CHECK_FALSE(g_tick.is_undefined());
    CHECK(g_tick.is_external());
    const Section* data = o.section_of(g_tick);
    REQUIRE(data != nullptr);
    CHECK((data->characteristics & scn::MemWrite) != 0u);
    CHECK((data->characteristics & (scn::CntInitializedData | scn::CntUninitializedData)) != 0u);

    const Symbol& note = need(o, "?fixture_note@@YAXPEBD@Z");
    CHECK_FALSE(note.is_undefined());
    CHECK(note.is_function());
    const Symbol& entry = need(o, "main");
    CHECK_FALSE(entry.is_undefined());
    CHECK(entry.is_function());

    const Symbol& compute = need(o, "?compute_value@@YAHXZ");
    CHECK(compute.is_undefined());
    const Symbol& extras = need(o, "?extras_value@@YAHXZ");
    CHECK(extras.is_undefined());
    const Symbol& sleep = need(o, "__imp_Sleep");
    CHECK(sleep.is_undefined());
}

namespace {

// extras.obj constructs. `full` also requires the constructs an optimising compiler may fold.
void check_extras(const Object& o, bool full) {
    CHECK_FALSE(o.is_bigobj());
    CHECK(o.machine() == Machine::Amd64);
    check_default_lib(o);

    // ---- __imp_ reference ----
    const Symbol& tick = need(o, "__imp_GetTickCount64");
    CHECK(tick.is_undefined());
    CHECK(tick.is_external());
    const Symbol& extras_value = need(o, "?extras_value@@YAHXZ");
    REQUIRE_FALSE(extras_value.is_undefined());
    const Section* ev_sec = o.section_of(extras_value);
    REQUIRE(ev_sec != nullptr);
    CHECK(has_rel32_to(o, *ev_sec, "__imp_GetTickCount64"));

    // ---- /alternatename (drectve) and weak externals ----
    bool alt = false;
    for (const Directive& d : o.directives()) {
        if (d.kind == DirectiveKind::AlternateName && d.has_pair && d.lhs == "fixture_optional_hook" &&
            d.rhs == "fixture_optional_hook_default") {
            alt = true;
        }
    }
    CHECK_MESSAGE(alt, "no /alternatename:fixture_optional_hook=fixture_optional_hook_default directive");
    const Symbol& hook = need(o, "fixture_optional_hook");
    CHECK(hook.is_undefined());
    CHECK(hook.is_external());  // the alternate name is NOT a weak external symbol; it only lives in .drectve
    const Symbol& hook_default = need(o, "fixture_optional_hook_default");
    CHECK_FALSE(hook_default.is_undefined());
    CHECK(hook_default.is_function());

    const Symbol& weak = need(o, "fixture_weak_symbol");
#if defined(__clang__)
    // clang: __attribute__((weak)) is a real IMAGE_SYM_CLASS_WEAK_EXTERNAL whose aux points at a
    // defined ".weak.<name>.default.*" symbol, searched as an alias (characteristics 3).
    CHECK(weak.is_weak_external());
    CHECK(weak.is_undefined());
    const auto wa = o.weak_external(weak);
    REQUIRE(wa.has_value());
    CHECK(wa->characteristics == static_cast<std::uint32_t>(WeakCharacteristics::SearchAlias));
    const Symbol* fallback = o.weak_external_default(weak);
    REQUIRE(fallback != nullptr);
    CHECK_FALSE(fallback->is_undefined());
    CHECK(fallback->is_external());
    CHECK_MESSAGE(starts_with(fallback->name, ".weak.fixture_weak_symbol.default."), "default symbol " << fallback->name);
#else
    // cl: an ordinary external definition; cl emits no weak external symbol for this construct.
    CHECK_FALSE(weak.is_weak_external());
    CHECK_FALSE(weak.is_undefined());
    CHECK(weak.is_function());
#endif

    // ---- .pdata / .xdata ----
    std::size_t pdata_sections = 0;
    std::size_t xdata_sections = 0;
    for (const Section& s : o.sections()) {
        if (starts_with(s.name, ".pdata")) {
            ++pdata_sections;
            CHECK(s.relocation_count % 3u == 0u);
            CHECK(s.relocation_count >= 3u);
            const auto rel = resolved(o, s);
            for (std::size_t i = 0; i < rel.size(); ++i) {
                CHECK_MESSAGE(rel[i].reloc.type == RelocType::Addr32Nb,
                              ".pdata relocation " << i << " is " << reloc_type_name(rel[i].reloc.type));
                const Section* tsec = o.section_of(*rel[i].target);
                REQUIRE(tsec != nullptr);
                if (i % 3 == 2) {
                    CHECK_MESSAGE(starts_with(tsec->name, ".xdata"), "unwind target in " << tsec->name);
                } else {
                    CHECK_MESSAGE(tsec->is_code(), "begin/end target in " << tsec->name);
                }
            }
        } else if (starts_with(s.name, ".xdata")) {
            ++xdata_sections;
            for (const ResolvedRelocation& r : resolved(o, s)) {
                CHECK_MESSAGE(r.reloc.type == RelocType::Addr32Nb, ".xdata relocation is " << reloc_type_name(r.reloc.type));
            }
        }
    }
    CHECK(pdata_sections > 0u);
    CHECK(xdata_sections > 0u);

    // extras_value always needs unwind data: its COMDAT/section has .pdata and .xdata children.
    // clang marks them Associative to the function section; cl does too with /Gy.
    const auto children = o.associative_children(*ev_sec);
    bool child_pdata = false;
    bool child_xdata = false;
    for (const Section* c : children) {
        child_pdata = child_pdata || starts_with(c->name, ".pdata");
        child_xdata = child_xdata || starts_with(c->name, ".xdata");
    }
    CHECK_MESSAGE((child_pdata && child_xdata), "extras_value section has " << children.size() << " associative children");

    // ---- .CRT$XCU dynamic initialiser ----
    std::size_t xcu_found = 0;
    for (const Section& s : o.sections()) {
        if (s.name != ".CRT$XCU") {
            continue;
        }
        ++xcu_found;
        const auto rel = resolved(o, s);
        REQUIRE(rel.size() >= 1u);
        CHECK(rel[0].reloc.type == RelocType::Addr64);
        std::string_view target = rel[0].target->name;
        if (starts_with(target, "_GLOBAL__sub_I_")) {
            // clang: a static wrapper function that calls the ??__E initialiser.
            const Section* wrapper = o.section_of(*rel[0].target);
            REQUIRE(wrapper != nullptr);
            bool calls_init = false;
            for (const ResolvedRelocation& r : resolved(o, *wrapper)) {
                calls_init = calls_init || (is_rel32_family(r.reloc.type) && starts_with(r.target->name, "??__E"));
            }
            // An optimising build may have inlined the initialiser into the wrapper.
            CHECK_MESSAGE((calls_init || !full), target << " does not call a ??__E initialiser");
            CHECK(wrapper->is_code());
        } else {
            CHECK_MESSAGE(starts_with(target, "??__E"), "XCU target " << target);
        }
    }
    CHECK(xcu_found >= 1u);

    // Dynamic initialiser data: static, writable, defined in this object, in every configuration.
    const Symbol* dyn = find_prefix(o, "?s_dynamic@");
    REQUIRE(dyn != nullptr);
    CHECK(dyn->is_static());
    const Section* dyn_sec = o.section_of(*dyn);
    REQUIRE(dyn_sec != nullptr);
    CHECK((dyn_sec->characteristics & scn::MemWrite) != 0u);

    if (!full) {
        return;
    }

    // ---- COMDAT inline function and template instantiations (selection Any) ----
    for (const char* name : {"?inline_twice@@YAHH@Z", "??$add_one@H@@YAHH@Z", "??$add_one@_J@@YA_J_J@Z"}) {
        const Symbol& f = need(o, name);
        CHECK(f.is_external());
        CHECK(f.is_function());
        const Section* sec = o.section_of(f);
        REQUIRE(sec != nullptr);
        CHECK(sec->is_code());
        CHECK(sec->is_comdat());
        const auto c = o.comdat(*sec);
        REQUIRE_MESSAGE(c.has_value(), name << " section has no section-definition aux");
        CHECK_MESSAGE(c->selection == ComdatSelection::Any, name << " selection " << static_cast<int>(c->selection));
        REQUIRE(c->leader != nullptr);
        CHECK(c->leader->name == name);
        CHECK(c->section_symbol != nullptr);
    }

    // ---- C++ EH personality referenced from .xdata ----
    bool personality = false;
    for (const Section& s : o.sections()) {
        if (!starts_with(s.name, ".xdata")) {
            continue;
        }
        for (const ResolvedRelocation& r : resolved(o, s)) {
            if (starts_with(r.target->name, "__CxxFrameHandler")) {
                personality = true;
                CHECK(r.target->is_undefined());
            }
        }
    }
    CHECK_MESSAGE(personality, "no .xdata relocation to __CxxFrameHandler*");

    // ---- string literal ----
    check_string_symbol(o, "extras-string-literal");

    // ---- static data (.data) and const table (.rdata) ----
    const Symbol* counter = find_prefix(o, "?s_counter@");
    REQUIRE(counter != nullptr);
    CHECK(counter->is_static());
    const Section* counter_sec = o.section_of(*counter);
    REQUIRE(counter_sec != nullptr);
    CHECK_MESSAGE(starts_with(counter_sec->name, ".data"), "s_counter in " << counter_sec->name);
    CHECK_FALSE(counter_sec->is_comdat());
    CHECK(read_u32(counter_sec->data, counter->value) == 5u);

    const Symbol* table = find_prefix(o, "?k_table@");
    REQUIRE(table != nullptr);
    CHECK(table->is_static());
    const Section* table_sec = o.section_of(*table);
    REQUIRE(table_sec != nullptr);
    CHECK_MESSAGE(starts_with(table_sec->name, ".rdata"), "k_table in " << table_sec->name);
    CHECK((table_sec->characteristics & scn::MemWrite) == 0u);
    CHECK(read_u32(table_sec->data, table->value + 0) == 2u);
    CHECK(read_u32(table_sec->data, table->value + 4) == 3u);
    CHECK(read_u32(table_sec->data, table->value + 8) == 5u);
    CHECK(read_u32(table_sec->data, table->value + 12) == 7u);
}

}  // namespace

TEST_CASE("fixture basic: extras.cpp at /Od covers every construct (cov_extras)") {
    check_extras(load("cov_extras_objs", "extras"), /*full=*/true);
}

TEST_CASE("fixture basic: extras.obj as built by this configuration") {
    // Debug keeps every construct; RelWithDebInfo may legitimately fold some (see file comment).
    check_extras(load("basic_objs", "extras"), /*full=*/config_is_debug());
}

TEST_CASE("fixture bigobj: bigobj_funcs.obj parses as /bigobj with 32-bit section numbers") {
    const Object& o = load("bigobj_objs", "bigobj_funcs");
    CHECK(o.is_bigobj());
    CHECK(o.header().header_size == 56u);
    CHECK(o.header().symbol_record_size == 20u);
    CHECK(o.header().bigobj_version == 2u);
    CHECK(o.machine() == Machine::Amd64);
    CHECK(o.sections().size() == static_cast<std::size_t>(o.header().section_count));

    const Symbol& first = need(o, "bigobj_fn_1");
    CHECK_FALSE(first.is_undefined());
    CHECK(first.is_function());
    REQUIRE(o.section_of(first) != nullptr);

    const Symbol& sum = need(o, "bigobj_sum");
    CHECK_FALSE(sum.is_undefined());
    const Section* sum_sec = o.section_of(sum);
    REQUIRE(sum_sec != nullptr);
    CHECK(has_rel32_to(o, *sum_sec, "bigobj_fn_1"));
    CHECK(has_rel32_to(o, *sum_sec, "bigobj_fn_51"));

    const Symbol& sink = need(o, "?g_bigobj_sink@@3HA");
    CHECK(sink.is_undefined());

    // Every defined symbol's section number must resolve to a section (no 16-bit truncation).
    std::uint32_t max_section = 0;
    for (const Symbol& s : o.symbols()) {
        if (s.section_number > 0) {
            REQUIRE(o.section_of(s) != nullptr);
            max_section = std::max(max_section, static_cast<std::uint32_t>(s.section_number));
        }
    }
    CHECK(max_section > 0u);
#if defined(__clang__)
    {
        // clang-cl: LLVM only switches to bigobj when the section count needs it. The fixture has
        // 66000 functions, so the object crosses the 65279-section limit in every configuration and
        // has symbols whose section numbers do not fit in 16 bits.
        CHECK_MESSAGE(o.header().section_count > 65279u, "section count " << o.header().section_count);
        CHECK_MESSAGE(max_section > 32767u, "max defined-symbol section number " << max_section);
        // The associated .xdata/.pdata sections are numbered after all the .text sections, so the
        // very last section has a section-definition symbol with a 32-bit section number.
        const Section& last_section = o.sections().back();
        bool last_has_symbol = false;
        for (const Symbol& s : o.symbols()) {
            last_has_symbol = last_has_symbol || (s.section_number == static_cast<std::int32_t>(last_section.index));
        }
        CHECK(last_has_symbol);
        CHECK(last_section.index > 65279u);
    }
#endif
    for (const Section& s : o.sections()) {
        if (s.name == ".xdata" || s.name == ".pdata") {
            // Associated unwind/exception data of a function in a high-numbered section.
            const auto c = o.comdat(s);
            if (c.has_value()) {
                CHECK(c->selection == ComdatSelection::Associative);
                CHECK(c->associated_section >= 1u);
                CHECK(c->associated_section <= o.header().section_count);
            }
        }
    }
}

TEST_CASE("fixture bigobj: main.obj") {
    const Object& o = load("bigobj_objs", "main");
#if defined(__clang__)
    CHECK_FALSE(o.is_bigobj());  // clang-cl ignores /bigobj and only switches format when needed
#else
    CHECK(o.is_bigobj());        // cl: /bigobj applies to every TU of the target
#endif
    const Symbol& sum = need(o, "bigobj_sum");
    CHECK(sum.is_undefined());
    const Symbol& sink = need(o, "?g_bigobj_sink@@3HA");
    CHECK_FALSE(sink.is_undefined());
}

