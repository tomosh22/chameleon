// cml_spike: WP-03 end-to-end spike (throwaway). One sub-command per pipeline step so that the CI
// log shows each step on its own; spike/run_spike.py drives them. See spike/README.md.
//
//   cml_spike anchor   <in.obj> <out.obj>
//   cml_spike bind     <orig.obj> <module.exe> <out-binding.obj> <out-externals.txt>
//   cml_spike reserve  <pid> <module-file-name> <size> <out-file>              (Windows)
//   cml_spike load     <pid> <patch.dll> <base>                                (Windows)
//   cml_spike retarget --pid N --exe <module.exe> --obj <orig.obj> --dll <patch.dll>   (Windows)
//                      [--dry-run --exe-base H --patch-base H]                 (any host)
//   cml_spike hook     <pid> <module.exe> <patch.dll> [--symbol <decorated>]   (Windows)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/pdb/pdb.h"
#include "cml/pe/pe.h"
#include "common.h"
#include "externals.h"
#include "obj_tools.h"
#ifdef _WIN32
#include "win_remote.h"
#endif

using namespace spike;

namespace {

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;
    bool has(const std::string& k) const { return opt.count(k) != 0; }
    std::string get(const std::string& k, const std::string& def = {}) const
    {
        const auto it = opt.find(k);
        return it == opt.end() ? def : it->second;
    }
};

Args parse_args(int argc, char** argv, int first)
{
    Args a;
    for (int i = first; i < argc; ++i) {
        const std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            if (s == "--dry-run") {
                a.opt[s.substr(2)] = "1";
            } else if (i + 1 < argc) {
                a.opt[s.substr(2)] = argv[++i];
            } else {
                die("option %s needs a value", s.c_str());
            }
        } else {
            a.pos.push_back(s);
        }
    }
    return a;
}

[[maybe_unused]] std::uint64_t parse_u64(const std::string& s)
{
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    if (end == s.c_str() || *end != '\0') {
        die("'%s' is not a number", s.c_str());
    }
    return v;
}

[[maybe_unused]] std::string basename_of(const std::string& path)
{
    const std::size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        die("cannot open '%s' for writing", path.c_str());
    }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) {
        die("write to '%s' failed", path.c_str());
    }
}

cml::coff::OwnedObject load_obj(const std::string& path)
{
    auto r = cml::coff::load_object_file(path);
    if (!r) {
        die("cannot load object '%s': %s", path.c_str(), r.error().format().c_str());
    }
    return std::move(r).value();
}

std::shared_ptr<const cml::pdb::PdbIndex> open_pdb_for(const std::string& image_path)
{
    auto r = cml::pdb::PdbIndex::open_for_pe(image_path);
    if (!r) {
        die("cannot open the PDB of '%s': %s", image_path.c_str(), r.error().format().c_str());
    }
    log("opened PDB %s (id %s, %u compilands)", (*r)->path().c_str(), (*r)->id().to_string().c_str(),
        (*r)->compiland_count());
    return *r;
}

cml::pe::OwnedImage load_image(const std::string& path)
{
    auto r = cml::pe::load_image_file(path);
    if (!r) {
        die("cannot load image '%s': %s", path.c_str(), r.error().format().c_str());
    }
    return std::move(r).value();
}

void print_externals(const std::vector<ExternalInfo>& externals)
{
    log("externals of the patch object: %zu", externals.size());
    for (const ExternalInfo& e : externals) {
        log("EXTERNAL name=%s kind=%s rva=0x%x obj_function=%d refs=%u %s", e.name.c_str(), to_string(e.kind), e.rva,
            e.obj_says_function ? 1 : 0, e.referenced_by, e.note.c_str());
    }
}

// ---------------------------------------------------------------------------------------------

int cmd_anchor(const Args& a)
{
    if (a.pos.size() != 2) {
        die("usage: anchor <in.obj> <out.obj>");
    }
    auto in = load_obj(a.pos[0]);
    log("anchor: %s machine=0x%04x sections=%u symbols=%u bigobj=%d", a.pos[0].c_str(), in.object.header().machine,
        in.object.header().section_count, in.object.header().symbol_count, in.object.is_bigobj() ? 1 : 0);
    const AnchorOutput out = add_anchors(in.object);
    write_file(a.pos[1], out.bytes);

    // Verify the rewritten object with the independent reader.
    auto re = cml::coff::Object::parse(cml::coff::ByteSpan(reinterpret_cast<const std::byte*>(out.bytes.data()), out.bytes.size()));
    if (!re) {
        die("the rewritten object does not parse: %s", re.error().format().c_str());
    }
    for (const AnchorInfo& ai : out.anchors) {
        const cml::coff::Symbol* ext = re->find_symbol(anchor_name(ai.section));
        const cml::coff::Symbol* sta = re->find_symbol(static_anchor_name(ai.section));
        if (ext == nullptr || sta == nullptr || ext->section_number != static_cast<std::int32_t>(ai.section) ||
            sta->section_number != static_cast<std::int32_t>(ai.section) || ext->value != 0 || sta->value != 0 ||
            !ext->is_external() || !sta->is_static()) {
            die("verification failed for the anchors of section %u", ai.section);
        }
    }
    // Relocation indices must be unchanged for the original symbols.
    for (const cml::coff::Symbol& s : in.object.symbols()) {
        const cml::coff::Symbol* t = re->symbol_at(s.index);
        if (t == nullptr || t->name != s.name || t->section_number != s.section_number || t->value != s.value) {
            die("verification failed: original symbol %u ('%.*s') changed", s.index, static_cast<int>(s.name.size()),
                s.name.data());
        }
    }
    log("anchor: wrote %s (%zu bytes, %zu anchored sections); original symbol indices verified unchanged", a.pos[1].c_str(),
        out.bytes.size(), out.anchors.size());
    return 0;
}

int cmd_bind(const Args& a)
{
    if (a.pos.size() != 4) {
        die("usage: bind <orig.obj> <module.exe> <out-binding.obj> <out-externals.txt>");
    }
    auto obj = load_obj(a.pos[0]);
    auto image = load_image(a.pos[1]);
    auto pdb = open_pdb_for(a.pos[1]);
    const std::vector<ExternalInfo> externals = classify_externals(obj.object, *pdb, image.image);
    print_externals(externals);

    std::vector<BindEntry> entries;
    std::string table;
    std::size_t unresolved = 0;
    for (const ExternalInfo& e : externals) {
        table += e.name + " | " + to_string(e.kind) + " | rva=" + std::to_string(e.rva) + " | " + e.note + "\n";
        switch (e.kind) {
            case ExtKind::Function:
                entries.push_back({e.name, true});
                break;
            case ExtKind::Data:
            case ExtKind::ImportSlot:
                entries.push_back({e.name, false});
                break;
            case ExtKind::LinkerProvided:
                break;
            case ExtKind::Weak:
            case ExtKind::Common:
            case ExtKind::Unresolved:
                ++unresolved;
                log("UNBOUND external '%s' (%s): %s", e.name.c_str(), to_string(e.kind), e.note.c_str());
                break;
        }
        if (e.obj_says_function != (e.kind == ExtKind::Function) &&
            (e.kind == ExtKind::Function || e.kind == ExtKind::Data)) {
            log("NOTE classification mismatch for '%s': PDB says %s, the object's symbol type says %s", e.name.c_str(),
                to_string(e.kind), e.obj_says_function ? "function" : "data");
        }
    }
    if (unresolved != 0) {
        die("%zu external(s) cannot be bound (see UNBOUND lines above)", unresolved);
    }
    const std::vector<std::uint8_t> bytes = emit_binding_obj(entries);
    write_file(a.pos[2], bytes);
    std::ofstream t(a.pos[3], std::ios::trunc);
    t << table;

    auto re = cml::coff::Object::parse(cml::coff::ByteSpan(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (!re) {
        die("the binding object does not parse: %s", re.error().format().c_str());
    }
    std::size_t nf = 0;
    for (const BindEntry& e : entries) {
        nf += e.is_function ? 1 : 0;
        if (re->find_symbol(e.name) == nullptr) {
            die("verification failed: binding object lacks symbol '%s'", e.name.c_str());
        }
    }
    log("bind: wrote %s (%zu bytes): %zu thunk(s)+slot(s), %zu data placeholder(s); sections=%u symbols=%u", a.pos[2].c_str(),
        bytes.size(), nf, entries.size() - nf, re->header().section_count, re->header().symbol_count);
    return 0;
}

#ifdef _WIN32

int cmd_reserve(const Args& a)
{
    if (a.pos.size() != 4) {
        die("usage: reserve <pid> <module-file-name> <size> <out-file>");
    }
    const auto pid = static_cast<std::uint32_t>(parse_u64(a.pos[0]));
    const RemoteModule mod = require_module(pid, a.pos[1]);
    log("target module %s base=%s size=0x%llx path=%s", mod.name.c_str(), hex64(mod.base).c_str(),
        static_cast<unsigned long long>(mod.size), mod.path.c_str());
    HANDLE h = open_target(pid);
    const std::uint64_t size = parse_u64(a.pos[2]);
    const std::uint64_t base = reserve_near(h, mod.base, mod.size, size, 0x60000000ull);  // +-1.5 GB
    CloseHandle(h);
    std::ofstream f(a.pos[3], std::ios::trunc);
    f << hex64(base) << "\n";
    log("RESERVED base=%s size=0x%llx", hex64(base).c_str(), static_cast<unsigned long long>(size));
    return 0;
}

int cmd_load(const Args& a)
{
    if (a.pos.size() != 3) {
        die("usage: load <pid> <patch.dll> <base>");
    }
    const auto pid = static_cast<std::uint32_t>(parse_u64(a.pos[0]));
    const std::string dll = a.pos[1];
    const std::uint64_t base = parse_u64(a.pos[2]);
    HANDLE h = open_target(pid);

    // Everything that allocates in the target happens before the reservation is released.
    const std::uint64_t remote_path = alloc_remote_string(h, to_wide(dll));
    log("remote path buffer at %s: %s", hex64(remote_path).c_str(), dll.c_str());

    const DWORD rel = release_reservation(h, base);
    log("released reservation %s (error %lu)", hex64(base).c_str(), rel);
    if (rel != 0) {
        die("the reservation at %s was not found in the target (error %lu)", hex64(base).c_str(), rel);
    }
    const DWORD code = remote_load_library(h, remote_path);
    free_remote(h, remote_path);
    log("remote LoadLibraryW returned 0x%08lx (low 32 bits of the HMODULE; 0 = failure)", code);

    RemoteModule m;
    if (!find_module(pid, basename_of(dll), &m)) {
        die("LoadLibraryW did not load '%s' (thread exit code 0x%08lx). /FIXED at %s could not be honoured or the image is invalid",
            dll.c_str(), code, hex64(base).c_str());
    }
    log("patch module %s loaded at %s size=0x%llx (requested /BASE %s)", m.name.c_str(), hex64(m.base).c_str(),
        static_cast<unsigned long long>(m.size), hex64(base).c_str());
    if (m.base != base) {
        die("patch module loaded at %s, NOT at the /FIXED base %s", hex64(m.base).c_str(), hex64(base).c_str());
    }
    log("LOADED-AT-FIXED-BASE %s", hex64(m.base).c_str());
    CloseHandle(h);
    return 0;
}

#endif  // _WIN32

int cmd_retarget(const Args& a)
{
    const std::string exe = a.get("exe");
    const std::string objp = a.get("obj");
    const std::string dll = a.get("dll");
    if (exe.empty() || objp.empty() || dll.empty()) {
        die("usage: retarget --pid N --exe <module.exe> --obj <orig.obj> --dll <patch.dll> [--dry-run --exe-base H --patch-base H]");
    }
    const bool dry = a.has("dry-run");
    std::uint64_t exe_base = 0;
    std::uint64_t patch_base = 0;
#ifdef _WIN32
    HANDLE h = nullptr;
#endif
    if (dry) {
        exe_base = parse_u64(a.get("exe-base", "0"));
        patch_base = parse_u64(a.get("patch-base", "0"));
    } else {
#ifdef _WIN32
        const auto pid = static_cast<std::uint32_t>(parse_u64(a.get("pid", "0")));
        exe_base = require_module(pid, basename_of(exe)).base;
        patch_base = require_module(pid, basename_of(dll)).base;
        h = open_target(pid);
#else
        die("retargeting a live process is Windows only; use --dry-run");
#endif
    }
    log("retarget: old module base %s, patch base %s%s", hex64(exe_base).c_str(), hex64(patch_base).c_str(),
        dry ? " (dry run)" : "");

    auto obj = load_obj(objp);
    auto image = load_image(exe);
    auto old_pdb = open_pdb_for(exe);
    auto patch_pdb = open_pdb_for(dll);
    const std::vector<ExternalInfo> externals = classify_externals(obj.object, *old_pdb, image.image);
    print_externals(externals);

    // ---- anchors: where did each object section land in the patch image? ----
    std::unordered_map<std::uint32_t, std::uint32_t> anchor_rva;
    std::size_t static_in_publics = 0;
    std::size_t anchorable_sections = 0;
    for (const cml::coff::Section& sec : obj.object.sections()) {
        if (!is_anchorable(sec)) {
            continue;
        }
        ++anchorable_sections;
        auto ext = patch_pdb->find_public(anchor_name(sec.index));
        if (!ext) {
            die("PDB lookup failed: %s", ext.error().format().c_str());
        }
        auto sta = patch_pdb->find_public(static_anchor_name(sec.index));
        if (!sta) {
            die("PDB lookup failed: %s", sta.error().format().c_str());
        }
        static_in_publics += sta->size();
        if (ext->empty()) {
            log("ANCHOR section %u '%.*s': %s NOT FOUND in the patch PDB publics (section dropped by the linker?)", sec.index,
                static_cast<int>(sec.name.size()), sec.name.data(), anchor_name(sec.index).c_str());
            continue;
        }
        anchor_rva[sec.index] = ext->front().rva;
        log("ANCHOR section %u '%.*s'%s -> %s rva=0x%x (public, pdb section %u offset 0x%x); static anchor in publics: %s",
            sec.index, static_cast<int>(sec.name.size()), sec.name.data(), sec.is_comdat() ? " [COMDAT]" : "",
            anchor_name(sec.index).c_str(), ext->front().rva, static_cast<unsigned>(ext->front().section),
            ext->front().offset, sta->empty() ? "no" : "yes");
    }
    // Do the static anchors show up as module symbols?
    std::size_t static_in_modules = 0;
    std::size_t module_symbol_total = 0;
    for (std::uint32_t ci = 0; ci < patch_pdb->compiland_count(); ++ci) {
        const cml::pdb::Compiland& c = patch_pdb->compiland(ci);
        if (c.object_name.empty() || c.object_name.front() == '*' || c.object_name.rfind("Import:", 0) == 0) {
            continue;
        }
        auto syms = patch_pdb->compiland_symbols(ci);
        if (!syms) {
            log("module symbols of compiland %u unreadable: %s", ci, syms.error().format().c_str());
            continue;
        }
        std::size_t shown = 0;
        for (const cml::pdb::SymbolView& s : *syms) {
            ++module_symbol_total;
            if (s.name.rfind("__cml_sanchor_", 0) == 0) {
                ++static_in_modules;
            }
            if (shown < 12 && (s.kind != cml::pdb::SymbolKind::Udt)) {
                log("  module symbol [%u] %.*s kind=%s rva=%s", ci, static_cast<int>(s.name.size()), s.name.data(),
                    cml::pdb::to_string(s.kind), s.rva == cml::pdb::kNoRva ? "-" : hex64(s.rva).c_str());
                ++shown;
            }
        }
        log("compiland %u '%.*s': %zu module symbols", ci, static_cast<int>(c.object_name.size()), c.object_name.data(),
            syms->size());
    }
    log("ANCHOR-SOURCE external anchors: found in PDB publics (%zu of them used); static anchors: %zu in publics, %zu in module symbols (of %zu)",
        anchor_rva.size(), static_in_publics, static_in_modules, module_symbol_total);
    // Asserted (ADR-0020): every anchorable section has its external anchor in the publics, and the
    // static probe anchors are NOT visible anywhere in the PDB.
    if (anchor_rva.size() != anchorable_sections) {
        die("ASSERT FAILED: %zu of %zu anchorable sections have an external anchor in the patch PDB publics", anchor_rva.size(),
            anchorable_sections);
    }
    if (static_in_publics != 0 || static_in_modules != 0) {
        die("ASSERT FAILED (unexpected evidence): static anchors ARE visible in the PDB (%zu in publics, %zu in module symbols); ADR-0020 item 1 must be revised",
            static_in_publics, static_in_modules);
    }
    log("ASSERT-OK anchors: %zu/%zu external anchors found in publics; static anchors found: 0 in publics, 0 in module symbols",
        anchor_rva.size(), anchorable_sections);

    // Cross-check: a function COMDAT's leader symbol sits at offset 0 of its section, so its public
    // RVA must equal the section's anchor RVA.
    std::size_t crosschecked = 0;
    for (const cml::coff::Symbol& s : obj.object.symbols()) {
        if (!s.is_external() || s.section_number <= 0 || s.value != 0) {
            continue;
        }
        const auto it = anchor_rva.find(static_cast<std::uint32_t>(s.section_number));
        if (it == anchor_rva.end()) {
            continue;
        }
        auto pub = patch_pdb->find_public(s.name);
        if (!pub || pub->empty()) {
            continue;
        }
        if (pub->front().rva != it->second) {
            die("anchor cross-check failed: '%.*s' (offset 0 of section %d) is at rva 0x%x but the section anchor is at 0x%x",
                static_cast<int>(s.name.size()), s.name.data(), s.section_number, pub->front().rva, it->second);
        }
        ++crosschecked;
        log("anchor cross-check ok: %.*s rva=0x%x == anchor of section %d", static_cast<int>(s.name.size()), s.name.data(),
            it->second, s.section_number);
    }
    log("anchor cross-check: %zu symbol(s) at section offset 0 agree with their anchors", crosschecked);
    if (crosschecked == 0) {
        die("ASSERT FAILED: the anchor cross-check compared no symbol with its anchor");
    }

    // ---- plan ----
    std::vector<PatchWrite> writes = plan_data_retargets(obj.object, externals, anchor_rva, patch_base, exe_base);
    std::size_t reloc_writes = writes.size();
    for (const ExternalInfo& e : externals) {
        if (e.kind != ExtKind::Function) {
            continue;
        }
        const std::string slot = slot_name(e.name);
        auto pub = patch_pdb->find_public(slot);
        if (!pub || pub->empty()) {
            die("slot symbol '%s' not found in the patch PDB publics", slot.c_str());
        }
        PatchWrite w;
        w.va = patch_base + pub->front().rva;
        const std::uint64_t target = exe_base + e.rva;
        w.bytes.resize(8);
        std::memcpy(w.bytes.data(), &target, 8);
        char what[400];
        std::snprintf(what, sizeof what, "SLOT %s rva=0x%x <- old function %s (rva 0x%x)", slot.c_str(),
                      pub->front().rva, hex64(target).c_str(), e.rva);
        w.what = what;
        writes.push_back(std::move(w));
    }
    log("retarget plan: %zu data relocation fix-up(s), %zu function slot(s)", reloc_writes, writes.size() - reloc_writes);

    for (const PatchWrite& w : writes) {
#ifdef _WIN32
        if (!dry) {
            const std::vector<std::uint8_t> before = read_remote(h, w.va, w.bytes.size());
            write_remote(h, w.va, w.bytes.data(), w.bytes.size());
            const std::vector<std::uint8_t> after = read_remote(h, w.va, w.bytes.size());
            if (after != w.bytes) {
                die("read-back mismatch at %s", hex64(w.va).c_str());
            }
            log("RETARGET %s | before=[%s] after=[%s]", w.what.c_str(), hex_bytes(before.data(), before.size()).c_str(),
                hex_bytes(after.data(), after.size()).c_str());
            if (w.is_rel32) {
                // Independent check: decode the displacement from the TARGET's memory and compare the
                // address the instruction will really access with the old module's symbol address
                // (module base + RVA from the OLD PDB) and with the placeholder in the patch.
                std::int32_t disp = 0;
                std::memcpy(&disp, after.data(), 4);
                const std::int64_t decoded = static_cast<std::int64_t>(w.va) + 4 + w.rel_n + disp;  // site + 4 + N + disp
                const std::int64_t accessed = decoded - w.addend;  // the in-place addend is the size of a trailing immediate
                auto op = old_pdb->find_public(w.symbol);
                auto pp = patch_pdb->find_public(w.symbol);
                if (!op || op->empty() || !pp || pp->empty()) {
                    die("ASSERT FAILED: '%s' is not a public of both the old module and the patch", w.symbol.c_str());
                }
                const std::uint64_t old_addr = exe_base + op->front().rva;
                const std::uint64_t placeholder = patch_base + pp->front().rva;
                const std::vector<std::uint8_t> old_val = read_remote(h, old_addr, 4);
                const std::vector<std::uint8_t> ph_val = read_remote(h, placeholder, 4);
                log("RETARGET-CHECK %s: site=%s disp=%d site+4+N+disp=%s accessed=%s | OLD address=%s (value [%s]) | patch placeholder=%s (value [%s])",
                    w.symbol.c_str(), hex64(w.va).c_str(), disp, hex64(static_cast<std::uint64_t>(decoded)).c_str(),
                    hex64(static_cast<std::uint64_t>(accessed)).c_str(), hex64(old_addr).c_str(),
                    hex_bytes(old_val.data(), 4).c_str(), hex64(placeholder).c_str(), hex_bytes(ph_val.data(), 4).c_str());
                if (static_cast<std::uint64_t>(accessed) != old_addr) {
                    die("ASSERT FAILED: the patched site accesses %s, not the old %s",
                        hex64(static_cast<std::uint64_t>(accessed)).c_str(), hex64(old_addr).c_str());
                }
                if (static_cast<std::uint64_t>(accessed) == placeholder || old_addr == placeholder) {
                    die("ASSERT FAILED: the patched site still accesses the placeholder");
                }
                if (old_addr != w.old_va) {
                    die("ASSERT FAILED: planned old address %s differs from module base + PDB rva %s", hex64(w.old_va).c_str(),
                        hex64(old_addr).c_str());
                }
                std::uint32_t pv = 0;
                std::memcpy(&pv, ph_val.data(), 4);
                if (pv != 0) {
                    die("ASSERT FAILED: the placeholder is not zero (%u)", pv);
                }
                log("ASSERT-OK retarget: decoded target %s == OLD %s, != placeholder %s", hex64(static_cast<std::uint64_t>(accessed)).c_str(),
                    w.symbol.c_str(), hex64(placeholder).c_str());
            }
            continue;
        }
#endif
        log("RETARGET (dry) %s | write=[%s] at %s", w.what.c_str(), hex_bytes(w.bytes.data(), w.bytes.size()).c_str(),
            hex64(w.va).c_str());
    }
#ifdef _WIN32
    if (h != nullptr) {
        CloseHandle(h);
    }
#endif
    log("retarget: done (%zu writes)", writes.size());
    return 0;
}

#ifdef _WIN32

int cmd_hook(const Args& a)
{
    if (a.pos.size() != 3) {
        die("usage: hook <pid> <module.exe> <patch.dll> [--symbol <decorated>]");
    }
    const auto pid = static_cast<std::uint32_t>(parse_u64(a.pos[0]));
    const std::string exe = a.pos[1];
    const std::string dll = a.pos[2];
    const std::string symbol = a.get("symbol", "?compute_value@@YAHXZ");

    const RemoteModule exe_mod = require_module(pid, basename_of(exe));
    const RemoteModule patch_mod = require_module(pid, basename_of(dll));
    auto old_pdb = open_pdb_for(exe);
    auto patch_pdb = open_pdb_for(dll);
    auto old_pub = old_pdb->find_public(symbol);
    auto new_pub = patch_pdb->find_public(symbol);
    if (!old_pub || old_pub->empty()) {
        die("'%s' is not a public of the old module", symbol.c_str());
    }
    if (!new_pub || new_pub->empty()) {
        die("'%s' is not a public of the patch module", symbol.c_str());
    }
    const std::uint64_t old_entry = exe_mod.base + old_pub->front().rva;
    const std::uint64_t new_entry = patch_mod.base + new_pub->front().rva;
    log("hook: %s old=%s (module %s + 0x%x, size %u) new=%s (module %s + 0x%x)", symbol.c_str(), hex64(old_entry).c_str(),
        exe_mod.name.c_str(), old_pub->front().rva, old_pub->front().size, hex64(new_entry).c_str(), patch_mod.name.c_str(),
        new_pub->front().rva);

    HANDLE h = open_target(pid);
    // Evidence for WP-10: padding in front of the entry (/FUNCTIONPADMIN + /hotpatch) and the first bytes.
    const std::vector<std::uint8_t> before = read_remote(h, old_entry - 16, 32);
    log("PADDING bytes at entry-16 .. entry+16: [%s | %s]", hex_bytes(before.data(), 16).c_str(),
        hex_bytes(before.data() + 16, 16).c_str());
    bool all_cc = true;
    bool all_nop = true;
    for (int i = 10; i < 16; ++i) {
        all_cc = all_cc && before[static_cast<std::size_t>(i)] == 0xCC;
        all_nop = all_nop && before[static_cast<std::size_t>(i)] == 0x90;
    }
    log("PADDING the 6 bytes before the entry are %s", all_cc ? "all 0xCC" : all_nop ? "all 0x90" : "NOT uniform padding");
    if (!all_cc) {
        die("ASSERT FAILED: the 6 bytes before the entry are not all 0xCC (padding expectation for WP-10)");
    }
    log("ASSERT-OK padding: >= 6 bytes of 0xCC precede the old entry (this is alignment and/or /FUNCTIONPADMIN, not distinguishable here)");
    if (before[16] == 0xE9 || (before[16] == 0xFF && before[17] == 0x25)) {
        die("the old entry starts with a jmp (%02x %02x): an incremental-link thunk? the fixture must link with /INCREMENTAL:NO",
            before[16], before[17]);
    }
    const std::int64_t rel = static_cast<std::int64_t>(new_entry) - static_cast<std::int64_t>(old_entry + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        die("jump from %s to %s is out of rel32 range", hex64(old_entry).c_str(), hex64(new_entry).c_str());
    }
    const auto rel32 = static_cast<std::int32_t>(rel);
    std::uint8_t jmp[5];
    jmp[0] = 0xE9;
    std::memcpy(jmp + 1, &rel32, 4);

    bool done = false;
    for (int attempt = 1; attempt <= 100 && !done; ++attempt) {
        SuspendedThreads threads = suspend_all(pid);
        const std::vector<std::uint64_t> rips = thread_rips(threads);
        bool hit = false;
        for (std::size_t i = 0; i < rips.size(); ++i) {
            const bool in_window = rips[i] >= old_entry && rips[i] < old_entry + sizeof jmp;
            log("  attempt %d: thread %lu rip=%s%s", attempt, threads.ids[i], hex64(rips[i]).c_str(),
                in_window ? "  <-- INSIDE the 5-byte window" : "");
            hit = hit || in_window;
        }
        if (hit) {
            resume_all(threads);
            Sleep(2);
            continue;
        }
        write_remote(h, old_entry, jmp, sizeof jmp);  // VirtualProtectEx, write, restore, FlushInstructionCache
        resume_all(threads);
        done = true;
        log("suspended %zu thread(s), wrote the jump, flushed the instruction cache, resumed", rips.size());
    }
    if (!done) {
        die("could not find a moment when no thread was inside the entry window");
    }
    const std::vector<std::uint8_t> after = read_remote(h, old_entry, 16);
    log("HOOK installed: old entry %s -> new entry %s  bytes=[%s] rel32=%d", hex64(old_entry).c_str(),
        hex64(new_entry).c_str(), hex_bytes(after.data(), 8).c_str(), static_cast<int>(rel32));
    if (std::memcmp(after.data(), jmp, sizeof jmp) != 0) {
        die("read-back of the hook failed");
    }
    CloseHandle(h);
    return 0;
}

// Resets the fixture's g_tick to 0 so that the patched compute_value() takes its
// `g_tick == 0` branch: it must read the OLD module's g_tick (retargeted REL32) and call the old
// fixture_note() through the thunk and slot, which prints "NOTE logic-start".
int cmd_exercise(const Args& a)
{
    if (a.pos.size() != 2) {
        die("usage: exercise <pid> <module.exe>");
    }
    const auto pid = static_cast<std::uint32_t>(parse_u64(a.pos[0]));
    const RemoteModule mod = require_module(pid, basename_of(a.pos[1]));
    auto pdb = open_pdb_for(a.pos[1]);
    auto pub = pdb->find_public("?g_tick@@3HA");
    if (!pub || pub->empty()) {
        die("?g_tick@@3HA is not a public of the module");
    }
    const std::uint64_t va = mod.base + pub->front().rva;
    HANDLE h = open_target(pid);
    const std::vector<std::uint8_t> before = read_remote(h, va, 4);
    const std::int32_t zero = 0;
    SuspendedThreads threads = suspend_all(pid);
    write_remote(h, va, &zero, sizeof zero);
    resume_all(threads);
    log("EXERCISE g_tick at %s: before=[%s], set to 0 so the patched code calls fixture_note() through its thunk",
        hex64(va).c_str(), hex_bytes(before.data(), 4).c_str());
    CloseHandle(h);
    return 0;
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: cml_spike <anchor|bind|reserve|load|retarget|hook> ...\n");
        return 2;
    }
    const std::string cmd = argv[1];
    const Args a = parse_args(argc, argv, 2);
    if (cmd == "anchor") {
        return cmd_anchor(a);
    }
    if (cmd == "bind") {
        return cmd_bind(a);
    }
    if (cmd == "retarget") {
        return cmd_retarget(a);
    }
#ifdef _WIN32
    if (cmd == "reserve") {
        return cmd_reserve(a);
    }
    if (cmd == "load") {
        return cmd_load(a);
    }
    if (cmd == "hook") {
        return cmd_hook(a);
    }
    if (cmd == "exercise") {
        return cmd_exercise(a);
    }
#endif
    die("unknown or unsupported command '%s'", cmd.c_str());
}
