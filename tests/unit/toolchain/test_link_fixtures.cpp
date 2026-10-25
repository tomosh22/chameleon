// WP-09 acceptance (Windows only; it runs the real compilers and linkers).
//
// For every M1 fixture, in the cell's toolchain:
//   1. detect the linker that built the fixture module (PE + PDB, cross-checked),
//   2. apply the fixture's edits/*.edit (-p1) to a scratch copy of its sources,
//   3. compile the edited TU with the command recorded in the module PDB (WP-05 rewriting),
//   4. anchor the object (WP-06), make binding.obj from a STAND-IN bind plan (test code only: every undefined
//      external is a function thunk when the module PDB says it is code, otherwise a data placeholder),
//   5. link with the mirrored linker AND with the other one where it exists, open the patch PDB, resolve all locators and
//      slots, and check the thunks in the image.
//   Prints: LINK <fixture> linker=<family> ok locators=N/N slots=N/N ms=<t>
// Also: link-time measurements (median of 5 after a warm-up) for the Q2 recommendation, /IGNORE evidence, negative links.
#include <algorithm>
#include <cstdarg>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "cml/coff/anchors.h"
#include "cml/coff/binding.h"
#include "cml/coff/coff.h"
#include "cml/coff/writer.h"
#include "cml/pdb/pdb.h"
#include "cml/pe/pe.h"
#include "cml/toolchain/toolchain.h"
#include "cml/util/hash.h"
#include "doctest.h"
#include "fixture_paths.h"
#include "unified_diff.h"

#ifndef CML_TEST_WORK_DIR
#define CML_TEST_WORK_DIR "."
#endif
#ifndef CML_TEST_FIXTURES_DIR
#define CML_TEST_FIXTURES_DIR "."
#endif
#ifndef CML_TEST_CXX_COMPILER
#define CML_TEST_CXX_COMPILER ""
#endif
#ifndef CML_TEST_MSVC_LINK
#define CML_TEST_MSVC_LINK ""
#endif
#ifndef CML_TEST_LINKER
#define CML_TEST_LINKER ""
#endif

namespace fs = std::filesystem;
namespace coff = cml::coff;
using namespace cml::toolchain;
using cml_test::FixturePaths;

namespace {

constexpr std::uint64_t kBase = 0x180000000ull;

bool is_cl_cell() {
#ifdef CML_TEST_COMPILER_IS_CL
    return true;
#else
    return false;
#endif
}

fs::path work_root() {
    const fs::path p = fs::path(CML_TEST_WORK_DIR) / "link_patch";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

void evidence(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("%s\n", buf);
    std::fflush(stdout);
    std::ofstream f(work_root() / "evidence.txt", std::ios::app);
    f << buf << "\n";
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_bytes(const fs::path& p, const coff::ByteVector& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

const char* cell_compiler() { return is_cl_cell() ? "cl" : "clang-cl"; }

// The linker that built the fixtures in this build tree: CMAKE_LINKER when it names one, else link.exe for cl cells
// and lld-link for clang-cl cells (CMake picks lld-link next to clang-cl).
ToolFamily expected_module_linker() {
    const ToolFamily by_cmake = family_from_path(CML_TEST_LINKER);
    if (by_cmake == ToolFamily::Link || by_cmake == ToolFamily::LldLink) {
        return by_cmake;
    }
    return is_cl_cell() ? ToolFamily::Link : ToolFamily::LldLink;
}

std::vector<std::string> search_dirs_for(ToolFamily family) {
    std::vector<std::string> dirs;
    const std::string msvc = CML_TEST_MSVC_LINK;
    if (family == ToolFamily::Link && !msvc.empty()) {
        dirs.push_back(fs::path(msvc).parent_path().string());
    }
    const std::string linker = CML_TEST_LINKER;
    if (family_from_path(linker) == family) {
        dirs.push_back(fs::path(linker).parent_path().string());
    }
    return dirs;
}

std::vector<std::string> compiler_fallback_dirs() {
    std::vector<std::string> dirs;
    const fs::path cxx = fs::path(CML_TEST_CXX_COMPILER);
    if (cxx.has_parent_path()) {
        dirs.push_back(cxx.parent_path().string());
    }
    return dirs;
}

bool optional_linker(ToolFamily family, ToolInfo& out) {
    ResolveLinkerOptions o;
    o.search_dirs = search_dirs_for(family);
    auto r = resolve_linker(family, o);
    if (!r) {
        return false;
    }
    out = *r;
    return true;
}

// ---- the stand-in bind planner (test code only; WP-07 owns the real one) -----------------------------------------------

struct StandInPlan {
    std::vector<coff::BindSymbol> binds;
    int functions_from_pdb = 0;
    int data_from_pdb = 0;
    int guessed_functions = 0;  // not in the module PDB: classified from the relocations
    int guessed_data = 0;
};

StandInPlan stand_in_plan(const coff::Object& obj, const cml::pdb::PdbIndex& module_pdb) {
    StandInPlan plan;
    std::unordered_set<std::string> defined;
    for (const coff::Symbol& s : obj.symbols()) {
        if ((s.is_external() || s.is_weak_external()) && (s.section_number != 0 || s.is_weak_external() || s.is_common())) {
            defined.insert(std::string(s.name));
        }
        if (s.is_absolute() && s.is_external()) {
            defined.insert(std::string(s.name));
        }
    }
    // Function by relocation context: call/jmp rel32 or an image-relative reference.
    std::unordered_set<std::string> fn_by_reloc;
    for (const coff::Section& sec : obj.sections()) {
        if (sec.relocation_count == 0) {
            continue;
        }
        auto rs = obj.resolve_relocations(sec);
        if (!rs) {
            continue;
        }
        for (const coff::ResolvedRelocation& r : *rs) {
            bool fn = r.reloc.type == coff::RelocType::Addr32Nb;
            if (!fn && sec.is_code() && r.reloc.type == coff::RelocType::Rel32 && r.reloc.virtual_address >= 1 &&
                r.reloc.virtual_address - 1 < sec.data.size()) {
                const auto op = std::to_integer<unsigned>(sec.data[r.reloc.virtual_address - 1]);
                fn = op == 0xE8 || op == 0xE9;
            }
            if (fn) {
                fn_by_reloc.insert(std::string(r.target->name));
            }
        }
    }
    const auto& sections = module_pdb.sections();
    std::unordered_set<std::string> seen;
    for (const coff::Symbol& s : obj.symbols()) {
        if (!s.is_external() || !s.is_undefined()) {
            continue;
        }
        const std::string name(s.name);
        if (defined.count(name) != 0 || name == "__ImageBase" || !seen.insert(name).second) {
            continue;
        }
        bool fn = false;
        auto pubs = module_pdb.find_public(name);
        if (pubs && !pubs->empty()) {
            const cml::pdb::SymbolView& v = pubs->front();
            fn = v.is_function();
            if (!fn && v.section >= 1 && v.section <= sections.size()) {
                fn = (sections[v.section - 1u].characteristics & 0x20000000u) != 0;  // IMAGE_SCN_MEM_EXECUTE
            }
            (fn ? plan.functions_from_pdb : plan.data_from_pdb)++;
        } else {
            fn = s.is_function() || fn_by_reloc.count(name) != 0;
            (fn ? plan.guessed_functions : plan.guessed_data)++;
        }
        plan.binds.push_back({name, fn ? coff::BindKind::Function : coff::BindKind::Data});
    }
    return plan;
}

// Defined external code symbols that the module does not have: "new functions".
std::vector<std::string> new_functions_of(const coff::Object& obj, const cml::pdb::PdbIndex& module_pdb) {
    std::vector<std::string> out;
    for (const coff::Symbol& s : obj.symbols()) {
        if (!s.is_external() || s.section_number <= 0) {
            continue;
        }
        const coff::Section* sec = obj.section_of(s);
        if (sec == nullptr || !sec->is_code()) {
            continue;
        }
        auto pubs = module_pdb.find_public(s.name);
        if (pubs && pubs->empty()) {
            out.emplace_back(s.name);
        }
    }
    return out;
}

// ---- one prepared patch ------------------------------------------------------------------------------------------------

struct Prepared {
    std::string fixture;
    std::string edit;
    std::string source_rel;
    std::string object;            // anchored object
    std::string binding;           // binding.obj ("" when nothing to bind)
    std::vector<PatchObjectInput> objects;
    std::vector<coff::BindSymbol> binds;
    std::vector<std::string> new_functions;
    std::size_t locator_count = 0;
    StandInPlan plan;
    double compile_seconds = 0;
};

// Applies one edit to a scratch copy of the fixture, recompiles the changed TUs, anchors them, makes binding.obj.
// The module PDB (`module_pdb`) must stay open: it classifies the externals.
Prepared prepare(const std::string& fixture, const fs::path& edit_file, const cml::pdb::PdbIndex& module_pdb) {
    Prepared p;
    p.fixture = fixture;
    p.edit = edit_file.stem().string();
    const fs::path fixture_dir = fs::path(CML_TEST_FIXTURES_DIR) / fixture;
    const fs::path scratch = work_root() / fixture / p.edit;
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch / "src", ec);
    fs::copy(fixture_dir, scratch / "src", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    REQUIRE_MESSAGE(!ec, "copy of " << fixture_dir.string() << ": " << ec.message());

    const std::string diff = slurp(edit_file);
    auto applied = cmltest::apply_patch_to_tree(scratch / "src", diff, 1);
    REQUIRE_MESSAGE(applied, applied.error().format());
    REQUIRE_MESSAGE(!applied->files.empty(), "edit " << edit_file.string() << " changed nothing");

    std::vector<coff::OwnedObject> owned;
    std::vector<std::string> object_paths;
    std::unordered_set<std::string> used_tags;
    std::uint32_t index = 0;
    for (const std::string& rel : applied->files) {
        if (fs::path(rel).extension() != ".cpp") {
            continue;
        }
        p.source_rel = rel;
        const std::string original = cml::pdb::normalize_path((fixture_dir / rel).string());
        // The compiland of the module that was built from the original source.
        std::uint32_t found = cml::pdb::kNoCompiland;
        cml::pdb::BuildEnv env;
        for (std::uint32_t ci = 0; ci < module_pdb.compiland_count(); ++ci) {
            if (!module_pdb.compiland(ci).library_name.empty()) {
                continue;
            }
            auto e = module_pdb.build_env(ci);
            if (!e || !e->source_file()) {
                continue;
            }
            if (cml::pdb::normalize_path(*e->source_file(), e->cwd().value_or("")) == original) {
                found = ci;
                env = *e;
                break;
            }
        }
        REQUIRE_MESSAGE(found != cml::pdb::kNoCompiland, "no compiland of the module " << fixture << " was built from " << original);
        auto rec = record_from_build_env(env);
        REQUIRE_MESSAGE(rec, rec.error().format());
        // Same command, edited source: replace the source path wherever the recorded command names it.
        const std::string edited = (scratch / "src" / rel).string();
        for (std::string& tok : rec->args) {
            if (cml::pdb::normalize_path(tok, rec->cwd) == original) {
                tok = edited;
            }
        }
        rec->source = edited;

        CompileRequest req;
        req.record = *rec;
        req.output_object = patch_object_path((scratch / "obj").string(), fixture, cml::hash64(module_pdb.compiland(found).object_name), edited);
        req.fallback_search_dirs = compiler_fallback_dirs();
        CompileOutcome out;
        auto compiled = compile_tu(req, out);
        REQUIRE_MESSAGE(compiled, compiled.error().format() << "\ncommand: " << out.command_line << "\n" << out.output);
        p.compile_seconds += out.seconds;

        auto obj = coff::load_object_file(out.object_path);
        REQUIRE_MESSAGE(obj, obj.error().format());
        auto model = coff::ObjectModel::from_object(obj->object);
        REQUIRE_MESSAGE(model, model.error().format());
        coff::AnchorOptions ao;
        ao.tag = "p" + std::to_string(index);
        ao.used_tags = &used_tags;
        auto locators = coff::add_anchors(*model, ao);
        REQUIRE_MESSAGE(locators, locators.error().format());
        auto bytes = coff::write(*model);
        REQUIRE_MESSAGE(bytes, bytes.error().format());
        const fs::path anchored = scratch / "obj" / (fs::path(rel).stem().string() + ".anchored.obj");
        REQUIRE(write_bytes(anchored, *bytes));

        PatchObjectInput in;
        in.path = anchored.string();
        in.locators = *locators;
        for (const coff::SectionLocator& l : in.locators) {
            p.locator_count += l.kind != coff::LocatorKind::None ? 1 : 0;
        }
        p.objects.push_back(std::move(in));
        owned.push_back(std::move(*obj));
        ++index;
    }
    REQUIRE_MESSAGE(!p.objects.empty(), "edit " << edit_file.string() << " changed no .cpp file");

    // The stand-in bind plan over all edited objects (the planner is per link, like the real one will be).
    std::unordered_set<std::string> seen;
    for (const coff::OwnedObject& o : owned) {
        StandInPlan one = stand_in_plan(o.object, module_pdb);
        for (const coff::BindSymbol& b : one.binds) {
            if (seen.insert(b.name).second) {
                p.binds.push_back(b);
            }
        }
        p.plan.functions_from_pdb += one.functions_from_pdb;
        p.plan.data_from_pdb += one.data_from_pdb;
        p.plan.guessed_functions += one.guessed_functions;
        p.plan.guessed_data += one.guessed_data;
        for (std::string& f : new_functions_of(o.object, module_pdb)) {
            p.new_functions.push_back(std::move(f));
        }
    }
    if (!p.binds.empty()) {
        auto bo = coff::make_binding_object(p.binds);
        REQUIRE_MESSAGE(bo, bo.error().format());
        const fs::path bpath = scratch / "obj" / "binding.obj";
        REQUIRE(write_bytes(bpath, *bo));
        p.binding = bpath.string();
    }
    return p;
}

LinkPatchRequest request_for(const Prepared& p, const ToolInfo& linker, const std::string& leaf) {
    LinkPatchRequest r;
    r.linker = linker;
    r.objects = p.objects;
    r.binding_object = p.binding;
    r.bindings = p.binds;
    r.new_functions = p.new_functions;
    r.out_dir = (work_root() / p.fixture / p.edit / leaf).string();
    r.name = "patch_1";
    r.base = kBase;
    return r;
}

// Everything that can be checked on a linked patch without running it.
struct Verified {
    std::size_t thunks = 0;
};

Verified verify_patch(const Prepared& p, const LinkedPatch& lp) {
    Verified v;
    // locators: every object section with a locator is placed, and placed inside the image
    std::size_t expected_locators = 0;
    for (const PatchObjectInput& o : p.objects) {
        for (const coff::SectionLocator& l : o.locators) {
            expected_locators += l.kind != coff::LocatorKind::None ? 1 : 0;
        }
    }
    CHECK(lp.sections.size() == expected_locators);
    CHECK(lp.base == kBase);
    CHECK(lp.image_size > 0);
    for (const SectionPlacement& sp : lp.sections) {
        CHECK_MESSAGE(sp.section_rva < lp.image_size, "section " << sp.section_index << " of object " << sp.object << " at RVA " << sp.section_rva);
        CHECK(lp.find_section(sp.object, sp.section_index) != nullptr);
    }
    // slots: function bindings have a slot and a thunk, data bindings a placeholder
    std::size_t fn = 0;
    std::size_t data = 0;
    for (const coff::BindSymbol& b : p.binds) {
        (b.kind == coff::BindKind::Function ? fn : data)++;
    }
    CHECK(lp.slot_rvas.size() == fn);
    CHECK(lp.thunk_rvas.size() == fn);
    CHECK(lp.data_rvas.size() == data);
    CHECK(lp.function_rvas.size() == p.new_functions.size());

    // Read the thunks back from the image: FF 25 <rel32> must land exactly on the slot, which must be in a writable section.
    auto image = cml::pe::load_image_file(lp.dll_path);
    REQUIRE_MESSAGE(image, image.error().format());
    for (const auto& kv : lp.thunk_rvas) {
        const std::string& name = kv.first;
        const std::uint32_t thunk_rva = kv.second;
        auto bytes = image->image.read_rva(thunk_rva, 6);
        REQUIRE_MESSAGE(bytes, "thunk " << name << ": " << bytes.error().format());
        CHECK(std::to_integer<unsigned>((*bytes)[0]) == 0xFF);
        CHECK(std::to_integer<unsigned>((*bytes)[1]) == 0x25);
        std::int32_t rel = 0;
        std::memcpy(&rel, bytes->data() + 2, 4);
        const std::int64_t target = static_cast<std::int64_t>(thunk_rva) + 6 + rel;
        const auto slot = lp.slot_rvas.find(name);
        REQUIRE(slot != lp.slot_rvas.end());
        CHECK_MESSAGE(target == static_cast<std::int64_t>(slot->second), "thunk " << name << " jumps to RVA " << target << ", slot is " << slot->second);
        const auto* sec = image->image.section_for_rva(slot->second);
        REQUIRE(sec != nullptr);
        CHECK((sec->characteristics & 0x80000000u) != 0);  // IMAGE_SCN_MEM_WRITE
        ++v.thunks;
    }
    for (const auto& kv : lp.data_rvas) {
        const std::string& name = kv.first;
        const std::uint32_t rva = kv.second;
        const auto* sec = image->image.section_for_rva(rva);
        REQUIRE_MESSAGE(sec != nullptr, "placeholder " << name);
        CHECK((sec->characteristics & 0x80000000u) != 0);
    }
    return v;
}

std::vector<fs::path> edits_of(const std::string& fixture) {
    std::vector<fs::path> out;
    const fs::path dir = fs::path(CML_TEST_FIXTURES_DIR) / fixture / "edits";
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".edit") {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct ModuleUnderTest {
    std::string name;
    std::string exe;
    std::shared_ptr<const cml::pdb::PdbIndex> pdb;
    LinkerDetection detection;
};

ModuleUnderTest open_module(const std::string& fixture) {
    const FixturePaths& paths = FixturePaths::get();
    REQUIRE(paths.loaded());
    ModuleUnderTest m;
    m.name = fixture;
    m.exe = paths.value(fixture + "_exe");
    const std::string pdb_path = paths.value(fixture + "_pdb");
    REQUIRE_MESSAGE((!m.exe.empty() && !pdb_path.empty()), "no exe/pdb for fixture " << fixture);
    auto det = detect_module_linker(m.exe);
    REQUIRE_MESSAGE(det, det.error().format());
    m.detection = *det;
    auto idx = cml::pdb::PdbIndex::open_pdb(pdb_path);
    REQUIRE_MESSAGE(idx, idx.error().format());
    m.pdb = *idx;
    evidence("LINKER-DETECT fixture=%s cell=%s/%s detected=%s from_pdb=%s from_pe=%s version=%u.%u warnings=%zu", fixture.c_str(),
             cell_compiler(), FixturePaths::get().config().c_str(), to_string(m.detection.family), to_string(m.detection.from_pdb),
             to_string(m.detection.from_pe), m.detection.version[0], m.detection.version[1], m.detection.warnings.size());
    for (const std::string& line : m.detection.evidence) {
        evidence("LINKER-DETECT   %s", line.c_str());
    }
    for (const cml::Diag& w : m.detection.warnings) {
        evidence("LINKER-DETECT   warning %s", w.format().c_str());
    }
    return m;
}

void run_fixture(const std::string& fixture) {
    ModuleUnderTest m = open_module(fixture);
    CHECK(m.detection.family == expected_module_linker());
    CHECK(m.detection.warnings.empty());

    ToolInfo mirrored;
    {
        ResolveLinkerOptions o;
        o.search_dirs = search_dirs_for(m.detection.family);
        auto r = resolve_linker(m.detection.family, o);
        REQUIRE_MESSAGE(r, r.error().format());
        mirrored = *r;
    }
    const ToolFamily other_family = m.detection.family == ToolFamily::Link ? ToolFamily::LldLink : ToolFamily::Link;
    ToolInfo other;
    const bool have_other = optional_linker(other_family, other);

    const auto edits = edits_of(fixture);
    REQUIRE_MESSAGE(!edits.empty(), "fixture " << fixture << " has no edits/*.edit");
    for (const fs::path& edit : edits) {
        INFO("fixture " << fixture << " edit " << edit.filename().string());
        Prepared p = prepare(fixture, edit, *m.pdb);
        evidence("PREPARED fixture=%s edit=%s source=%s compiler=%s compile_s=%.2f sections_with_locator=%zu bind=%zu (functions %d+%d guessed, data %d+%d guessed) new_functions=%zu",
                 fixture.c_str(), p.edit.c_str(), p.source_rel.c_str(), cell_compiler(), p.compile_seconds, p.locator_count, p.binds.size(),
                 p.plan.functions_from_pdb, p.plan.guessed_functions, p.plan.data_from_pdb, p.plan.guessed_data, p.new_functions.size());
        if (fixture == "new_function") {
            CHECK_MESSAGE(!p.new_functions.empty(), "the new_function edit must define a function the module does not have");
        }

        struct Run {
            const ToolInfo* tool;
            const char* leaf;
            bool required;
        };
        const Run runs[] = {{&mirrored, "mirrored", true}, {&other, "other", false}};
        for (const Run& run : runs) {
            if (run.tool == &other && !have_other) {
                evidence("LINK-SKIP %s linker=%s not available in this environment", fixture.c_str(), to_string(other_family));
                continue;
            }
            LinkTrace trace;
            auto lp = link_patch(request_for(p, *run.tool, run.leaf), &trace);
            INFO("linker " << to_string(run.tool->family) << " " << run.tool->path << "\ncommand: " << trace.command_line << "\n" << trace.output);
            REQUIRE_MESSAGE(lp, lp.error().format() << "\ncommand: " << trace.command_line << "\n" << trace.output);
            const Verified v = verify_patch(p, *lp);
            const std::size_t slots = lp->slot_rvas.size() + lp->data_rvas.size();
            evidence("LINK %s linker=%s ok locators=%zu/%zu slots=%zu/%zu ms=%.0f", fixture.c_str(), to_string(lp->linker_family),
                     lp->sections.size(), p.locator_count, slots, p.binds.size(), lp->link_ms);
            evidence("LINK-DETAIL %s linker=%s%s version=%s thunks_verified=%zu new_functions=%zu image_size=%u base=0x%llx pdb_read_ms=%.1f warnings=%zu exit=%d",
                     fixture.c_str(), to_string(lp->linker_family), run.tool == &mirrored ? " (mirrored)" : " (other)",
                     lp->linker_version.c_str(), v.thunks, lp->function_rvas.size(), lp->image_size,
                     static_cast<unsigned long long>(lp->base), lp->pdb_read_ms, lp->warnings.size(), trace.exit_code);
            for (const cml::Diag& w : lp->warnings) {
                evidence("LINK-WARNING %s linker=%s %s", fixture.c_str(), to_string(lp->linker_family), w.message.c_str());
            }
        }
    }
}

}  // namespace

TEST_CASE("link patch: fixture basic links under the mirrored linker and the other one") { run_fixture("basic"); }
TEST_CASE("link patch: fixture statics links under the mirrored linker and the other one") { run_fixture("statics"); }
TEST_CASE("link patch: fixture virtuals links under the mirrored linker and the other one") { run_fixture("virtuals"); }
TEST_CASE("link patch: fixture templates links under the mirrored linker and the other one") { run_fixture("templates"); }
TEST_CASE("link patch: fixture lambdas links under the mirrored linker and the other one") { run_fixture("lambdas"); }
TEST_CASE("link patch: fixture new_function links under the mirrored linker and the other one") { run_fixture("new_function"); }
TEST_CASE("link patch: fixture new_global links under the mirrored linker and the other one") { run_fixture("new_global"); }

TEST_CASE("link patch: the user can override the detected linker family") {
    const FixturePaths& paths = FixturePaths::get();
    REQUIRE(paths.loaded());
    const std::string exe = paths.value("basic_exe");
    auto det = detect_module_linker(exe);
    REQUIRE_MESSAGE(det, det.error().format());
    DetectLinkerOptions o;
    o.override_family = det->family == ToolFamily::Link ? ToolFamily::LldLink : ToolFamily::Link;
    auto forced = detect_module_linker(exe, {}, o);
    REQUIRE_MESSAGE(forced, forced.error().format());
    CHECK(forced->overridden);
    CHECK(forced->family == o.override_family);
    CHECK(forced->from_pdb == det->from_pdb);
    // An explicit linker path of the wrong family is refused.
    ToolInfo any;
    if (optional_linker(ToolFamily::Link, any)) {
        ResolveLinkerOptions ro;
        ro.explicit_path = any.path;
        auto wrong = resolve_linker(ToolFamily::LldLink, ro);
        REQUIRE_FALSE(wrong);
        CHECK(wrong.error().code == diag::kLinkerNotFound);
        auto right = resolve_linker(ToolFamily::Link, ro);
        CHECK_MESSAGE(right, right.error().format());
    }
}

TEST_CASE("link patch: failures are diagnosed with the symbol or the file (negative links)") {
    ModuleUnderTest m = open_module("basic");
    const auto edits = edits_of("basic");
    REQUIRE(!edits.empty());
    Prepared p = prepare("basic", edits.front(), *m.pdb);
    ToolInfo mirrored;
    {
        ResolveLinkerOptions ro;
        ro.search_dirs = search_dirs_for(m.detection.family);
        auto r = resolve_linker(m.detection.family, ro);
        REQUIRE_MESSAGE(r, r.error().format());
        mirrored = *r;
    }
    std::vector<ToolInfo> tools = {mirrored};
    ToolInfo other;
    if (optional_linker(m.detection.family == ToolFamily::Link ? ToolFamily::LldLink : ToolFamily::Link, other)) {
        tools.push_back(other);
    }
    for (const ToolInfo& tool : tools) {
        INFO("linker " << to_string(tool.family));
        // 1. no binding.obj: every bound symbol is unresolved
        {
            Prepared q = p;
            q.binding.clear();
            LinkTrace trace;
            auto r = link_patch(request_for(q, tool, std::string("neg_unresolved_") + to_string(tool.family)), &trace);
            REQUIRE_FALSE(r);
            evidence("NEGATIVE unresolved linker=%s -> %s", to_string(tool.family), r.error().format().c_str());
            CHECK_MESSAGE(r.error().code == diag::kUnresolvedSymbol, r.error().format() << "\n" << trace.output);
            if (!p.binds.empty()) {
                CHECK(r.error().message.find("unresolved symbol '") != std::string::npos);
            }
        }
        // 2. a missing object file
        {
            Prepared q = p;
            q.objects[0].path = (work_root() / "does_not_exist.obj").string();
            LinkTrace trace;
            auto r = link_patch(request_for(q, tool, std::string("neg_missing_") + to_string(tool.family)), &trace);
            REQUIRE_FALSE(r);
            evidence("NEGATIVE missing-object linker=%s -> %s", to_string(tool.family), r.error().format().c_str());
            CHECK_MESSAGE(r.error().code == diag::kLinkFailed, r.error().format() << "\n" << trace.output);
        }
        // 3. a re-enabled REF / ICF never reaches the linker
        for (const char* flag : {"/OPT:REF", "/OPT:ICF"}) {
            LinkPatchRequest req = request_for(p, tool, std::string("neg_flag_") + to_string(tool.family));
            req.extra_args = {flag};
            LinkTrace trace;
            auto r = link_patch(req, &trace);
            REQUIRE_FALSE(r);
            CHECK(r.error().code == diag::kForbiddenLinkerOption);
            CHECK(trace.command_line.empty());  // the linker was not started
        }
    }
}

TEST_CASE("link patch: /IGNORE evidence and a long command line through a response file") {
    ModuleUnderTest m = open_module("basic");
    const auto edits = edits_of("basic");
    REQUIRE(!edits.empty());
    Prepared p = prepare("basic", edits.front(), *m.pdb);
    std::vector<ToolInfo> tools;
    ToolInfo a;
    ToolInfo b;
    if (optional_linker(ToolFamily::Link, a)) tools.push_back(a);
    if (optional_linker(ToolFamily::LldLink, b)) tools.push_back(b);
    REQUIRE(!tools.empty());
    for (const ToolInfo& tool : tools) {
        INFO("linker " << to_string(tool.family));
        // Without /IGNORE (the default): how many warnings does the linker print?
        {
            LinkTrace trace;
            auto r = link_patch(request_for(p, tool, std::string("noignore_") + to_string(tool.family)), &trace);
            REQUIRE_MESSAGE(r, r.error().format() << "\n" << trace.output);
            const LinkOutputSummary s = summarize_link_output(trace.output);
            std::size_t lines = 0;
            for (char c : trace.output) lines += c == '\n';
            evidence("LINK-NOIGNORE fixture=basic linker=%s output_lines=%zu warnings=%zu errors=%zu", to_string(tool.family), lines,
                     s.warnings.size(), s.errors.size());
            CHECK(s.warnings.empty());
        }
        // With the plan's /IGNORE list: accepted.
        {
            LinkPatchRequest req = request_for(p, tool, std::string("ignore_") + to_string(tool.family));
            req.ignore_warnings = {4001, 4254, 4281};
            LinkTrace trace;
            auto r = link_patch(req, &trace);
            REQUIRE_MESSAGE(r, r.error().format() << "\n" << trace.output);
            evidence("LINK-IGNORE fixture=basic linker=%s accepted /IGNORE:4001,4254,4281 exit=%d", to_string(tool.family), trace.exit_code);
        }
        // Forced response file: same result, the @file is passed.
        {
            LinkPatchRequest req = request_for(p, tool, std::string("rsp_") + to_string(tool.family));
            req.force_response_file = true;
            LinkTrace trace;
            auto r = link_patch(req, &trace);
            REQUIRE_MESSAGE(r, r.error().format() << "\n" << trace.output);
            CHECK_FALSE(trace.response_file.empty());
            CHECK(fs::exists(trace.response_file));
            CHECK(trace.command_line.find("@") != std::string::npos);
            evidence("LINK-RSP fixture=basic linker=%s response_file=ok command_line_chars=%zu sections=%zu", to_string(tool.family),
                     trace.command_line.size(), r->sections.size());
        }
    }
}

TEST_CASE("link patch: link time of `basic` (median of 5 after a warm-up) for each linker") {
    ModuleUnderTest m = open_module("basic");
    const auto edits = edits_of("basic");
    REQUIRE(!edits.empty());
    Prepared p = prepare("basic", edits.front(), *m.pdb);
    ToolInfo mirrored;
    {
        ResolveLinkerOptions ro;
        ro.search_dirs = search_dirs_for(m.detection.family);
        auto r = resolve_linker(m.detection.family, ro);
        REQUIRE_MESSAGE(r, r.error().format());
        mirrored = *r;
    }
    std::vector<const ToolInfo*> tools = {&mirrored};
    ToolInfo other;
    if (optional_linker(m.detection.family == ToolFamily::Link ? ToolFamily::LldLink : ToolFamily::Link, other)) {
        tools.push_back(&other);
    }
    const bool relwithdebinfo = FixturePaths::get().config() == "RelWithDebInfo";
    for (const ToolInfo* tool : tools) {
        INFO("linker " << to_string(tool->family));
        // The same output path every time: the patch PDB of the previous link is overwritten, which only works when
        // read_linked_patch() released its mapping (ADR-0017).
        const LinkPatchRequest req = request_for(p, *tool, std::string("time_") + to_string(tool->family));
        {
            auto warm = link_patch(req);
            REQUIRE_MESSAGE(warm, warm.error().format());
        }
        std::vector<double> ms;
        for (int i = 0; i < 5; ++i) {
            LinkTrace trace;
            auto r = link_patch(req, &trace);
            REQUIRE_MESSAGE(r, r.error().format() << "\n" << trace.output);
            ms.push_back(r->link_ms);
        }
        std::vector<double> sorted = ms;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[2];
        evidence("LINK-TIME fixture=basic linker=%s%s objects=%s config=%s runs_ms=%.0f,%.0f,%.0f,%.0f,%.0f median_ms=%.0f min_ms=%.0f max_ms=%.0f",
                 to_string(tool->family), tool == &mirrored ? " (mirrored)" : " (other)", cell_compiler(),
                 FixturePaths::get().config().c_str(), ms[0], ms[1], ms[2], ms[3], ms[4], median, sorted.front(), sorted.back());
        if (tool == &mirrored && relwithdebinfo) {
            CHECK_MESSAGE(median < 300.0, "median link time of `basic` is " << median << " ms; the budget is 300 ms");
        }
    }
}
