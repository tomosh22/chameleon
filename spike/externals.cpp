#include "externals.h"

#include <cstdio>
#include <cstring>
#include <map>

#include "common.h"
#include "obj_tools.h"

namespace spike {

using namespace cml;

const char* to_string(ExtKind k)
{
    switch (k) {
        case ExtKind::Function: return "function";
        case ExtKind::Data: return "data";
        case ExtKind::ImportSlot: return "import-slot";
        case ExtKind::LinkerProvided: return "linker-provided";
        case ExtKind::Weak: return "weak";
        case ExtKind::Common: return "common";
        case ExtKind::Unresolved: return "UNRESOLVED";
    }
    return "?";
}

std::vector<ExternalInfo> classify_externals(const coff::Object& obj, const pdb::PdbIndex& module_pdb,
                                             const pe::Image& module_image)
{
    // How many relocations (outside debug info) name each symbol.
    std::map<std::uint32_t, std::uint32_t> refs;
    for (const coff::Section& sec : obj.sections()) {
        if (!is_anchorable(sec)) {
            continue;
        }
        auto rr = obj.resolve_relocations(sec);
        if (!rr) {
            die("cannot resolve relocations of section %u: %s", sec.index, rr.error().format().c_str());
        }
        for (const coff::ResolvedRelocation& r : *rr) {
            ++refs[r.target->index];
        }
    }

    std::vector<pe::ImportModule> imports;
    bool imports_loaded = false;

    std::vector<ExternalInfo> out;
    std::map<std::string, std::size_t> seen;
    for (const coff::Symbol& s : obj.symbols()) {
        ExternalInfo info;
        info.name = std::string(s.name);
        info.obj_says_function = s.is_function();
        const auto r = refs.find(s.index);
        info.referenced_by = r == refs.end() ? 0 : r->second;

        if (s.is_weak_external()) {
            info.kind = ExtKind::Weak;
            info.note = "weak external (not bound by the spike)";
        } else if (!s.is_external()) {
            continue;
        } else if (s.is_common()) {
            info.kind = ExtKind::Common;
            info.note = "COMMON symbol (unsupported)";
        } else if (!s.is_undefined()) {
            continue;
        } else if (info.name == "__ImageBase") {
            info.kind = ExtKind::LinkerProvided;
            info.note = "defined by the linker in the patch image";
        } else {
            auto pubs = module_pdb.find_public(info.name);
            if (!pubs) {
                die("PDB public lookup failed for '%s': %s", info.name.c_str(), pubs.error().format().c_str());
            }
            if (!pubs->empty() && pubs->front().rva != pdb::kNoRva) {
                const pdb::SymbolView& v = pubs->front();
                info.rva = v.rva;
                info.pdb_section = v.section;
                info.pdb_flags = v.flags;
                bool exec = false;
                if (v.section >= 1 && v.section <= module_pdb.sections().size()) {
                    exec = (module_pdb.sections()[v.section - 1u].characteristics & coff::scn::MemExecute) != 0;
                }
                const bool code = v.is_function() || (v.flags & pdb::kPublicCode) != 0 || exec;
                info.kind = code ? ExtKind::Function : ExtKind::Data;
                char note[160];
                std::snprintf(note, sizeof note, "public rva=0x%x section=%u flags=0x%x exec_section=%d aliases=%zu",
                              v.rva, static_cast<unsigned>(v.section), v.flags, exec ? 1 : 0, pubs->size());
                info.note = note;
            } else if (info.name.rfind("__imp_", 0) == 0) {
                if (!imports_loaded) {
                    auto imp = module_image.imports();
                    if (!imp) {
                        die("cannot read the module's import table: %s", imp.error().format().c_str());
                    }
                    imports = std::move(imp).value();
                    imports_loaded = true;
                }
                const std::string fn = info.name.substr(6);
                info.kind = ExtKind::Unresolved;
                for (const pe::ImportModule& m : imports) {
                    for (const pe::ImportEntry& e : m.entries) {
                        if (!e.by_ordinal && e.name == fn) {
                            info.kind = ExtKind::ImportSlot;
                            info.rva = e.iat_slot_rva;
                            info.note = "IAT slot of " + std::string(m.dll_name) + "!" + fn;
                            break;
                        }
                    }
                    if (info.kind == ExtKind::ImportSlot) {
                        break;
                    }
                }
                if (info.kind == ExtKind::Unresolved) {
                    info.note = "not a public of the module and not in its import table";
                }
            } else {
                info.kind = ExtKind::Unresolved;
                info.note = "not a public of the module's PDB";
            }
        }
        if (seen.count(info.name) != 0) {
            continue;
        }
        seen[info.name] = out.size();
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<PatchWrite> plan_data_retargets(const coff::Object& obj, const std::vector<ExternalInfo>& externals,
                                            const std::unordered_map<std::uint32_t, std::uint32_t>& anchor_rva,
                                            std::uint64_t patch_base, std::uint64_t old_module_base)
{
    std::map<std::string, const ExternalInfo*> by_name;
    for (const ExternalInfo& e : externals) {
        by_name[e.name] = &e;
    }

    std::vector<PatchWrite> writes;
    std::vector<std::string> problems;
    auto problem = [&](const char* fmt, auto... args) {
        char buf[512];
        std::snprintf(buf, sizeof buf, fmt, args...);
        problems.emplace_back(buf);
    };

    for (const coff::Section& sec : obj.sections()) {
        if (!is_anchorable(sec)) {
            continue;  // debug sections are never retargeted
        }
        auto rr = obj.resolve_relocations(sec);
        if (!rr) {
            die("cannot resolve relocations of section %u: %s", sec.index, rr.error().format().c_str());
        }
        for (const coff::ResolvedRelocation& r : *rr) {
            const coff::Symbol& t = *r.target;
            if (!t.is_external() || !t.is_undefined()) {
                continue;
            }
            const auto it = by_name.find(std::string(t.name));
            if (it == by_name.end()) {
                continue;
            }
            const ExternalInfo& ext = *it->second;
            if (ext.kind != ExtKind::Data && ext.kind != ExtKind::ImportSlot) {
                continue;  // functions go through thunks; the linker resolved those references
            }
            const auto a = anchor_rva.find(sec.index);
            if (a == anchor_rva.end()) {
                problem("section %u '%.*s' holds a relocation to '%s' but has no anchor in the patch PDB", sec.index,
                        static_cast<int>(sec.name.size()), sec.name.data(), ext.name.c_str());
                continue;
            }
            const std::uint64_t site_va = patch_base + a->second + r.reloc.virtual_address;
            const std::uint64_t old_va = old_module_base + ext.rva;
            const coff::RelocType type = r.reloc.type;
            char what[400];
            if (coff::is_rel32_family(type)) {
                const int n = static_cast<int>(r.reloc.raw_type) - static_cast<int>(coff::RelocType::Rel32);
                if (std::uint64_t{r.reloc.virtual_address} + 4 > sec.data.size()) {
                    problem("relocation at section %u +0x%x lies outside the section data", sec.index, r.reloc.virtual_address);
                    continue;
                }
                std::int32_t addend = 0;
                std::memcpy(&addend, sec.data.data() + r.reloc.virtual_address, 4);
                const std::int64_t target = static_cast<std::int64_t>(old_va) + addend;
                const std::int64_t rel = target - static_cast<std::int64_t>(site_va + 4 + static_cast<std::uint64_t>(n));
                if (rel < INT32_MIN || rel > INT32_MAX) {
                    problem("REL32 to '%s' at section %u +0x%x is out of range: site=0x%llx target=0x%llx", ext.name.c_str(),
                            sec.index, r.reloc.virtual_address, static_cast<unsigned long long>(site_va),
                            static_cast<unsigned long long>(target));
                    continue;
                }
                const std::int32_t rel32 = static_cast<std::int32_t>(rel);
                PatchWrite w;
                w.va = site_va;
                w.bytes.resize(4);
                std::memcpy(w.bytes.data(), &rel32, 4);
                std::snprintf(what, sizeof what,
                              "%s section %u '%.*s' +0x%x -> %s old=0x%llx addend=%d site=0x%llx rel32=0x%08x",
                              std::string(coff::reloc_type_name(type)).c_str(), sec.index,
                              static_cast<int>(sec.name.size()), sec.name.data(), r.reloc.virtual_address,
                              ext.name.c_str(), static_cast<unsigned long long>(old_va), addend,
                              static_cast<unsigned long long>(site_va), static_cast<unsigned>(rel32));
                w.what = what;
                writes.push_back(std::move(w));
            } else if (type == coff::RelocType::Addr64) {
                if (std::uint64_t{r.reloc.virtual_address} + 8 > sec.data.size()) {
                    problem("relocation at section %u +0x%x lies outside the section data", sec.index, r.reloc.virtual_address);
                    continue;
                }
                std::int64_t addend = 0;
                std::memcpy(&addend, sec.data.data() + r.reloc.virtual_address, 8);
                const std::uint64_t value = old_va + static_cast<std::uint64_t>(addend);
                PatchWrite w;
                w.va = site_va;
                w.bytes.resize(8);
                std::memcpy(w.bytes.data(), &value, 8);
                std::snprintf(what, sizeof what, "ADDR64 section %u '%.*s' +0x%x -> %s old=0x%llx addend=%lld site=0x%llx",
                              sec.index, static_cast<int>(sec.name.size()), sec.name.data(), r.reloc.virtual_address,
                              ext.name.c_str(), static_cast<unsigned long long>(old_va), static_cast<long long>(addend),
                              static_cast<unsigned long long>(site_va));
                w.what = what;
                writes.push_back(std::move(w));
            } else {
                problem("relocation %s to data symbol '%s' (section %u '%.*s' +0x%x) cannot be retargeted: ADDR32NB is only legal into the patch, SECREL/SECTION are TLS/debug only",
                        std::string(coff::reloc_type_name(type)).c_str(), ext.name.c_str(), sec.index,
                        static_cast<int>(sec.name.size()), sec.name.data(), r.reloc.virtual_address);
            }
        }
    }
    if (!problems.empty()) {
        for (const std::string& p : problems) {
            log("retarget problem: %s", p.c_str());
        }
        die("%zu relocation(s) could not be retargeted", problems.size());
    }
    return writes;
}

}  // namespace spike
