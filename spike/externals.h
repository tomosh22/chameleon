#pragma once

// Classification of the patch object's undefined externals against the running module (its PDB
// publics, its PE import table), and the retarget plan. Portable: no Windows API.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/pdb/pdb.h"
#include "cml/pe/pe.h"

namespace spike {

enum class ExtKind {
    Function,        // public with the code/function flag (or in an executable section): thunk + slot
    Data,            // public in a data section: 8-byte placeholder, references retargeted
    ImportSlot,      // __imp_<name> found in the module's import table: placeholder -> IAT slot
    LinkerProvided,  // __ImageBase: the linker defines it, nothing to bind
    Weak,            // weak external: reported, not bound
    Common,          // COMMON symbol: unsupported, reported
    Unresolved,      // not found anywhere: reported, the spike fails
};

const char* to_string(ExtKind kind);

struct ExternalInfo {
    std::string name;
    ExtKind kind = ExtKind::Unresolved;
    std::uint32_t rva = 0;            // RVA in the old module (Function, Data, ImportSlot)
    std::uint16_t pdb_section = 0;
    std::uint32_t pdb_flags = 0;      // S_PUB32 flags
    bool obj_says_function = false;   // DTYPE_FUNCTION on the undefined symbol in the object
    std::uint32_t referenced_by = 0;  // number of relocations naming this symbol
    std::string note;
};

// Every undefined external symbol of `object`, classified. The reported order is symbol-table order.
std::vector<ExternalInfo> classify_externals(const cml::coff::Object& object, const cml::pdb::PdbIndex& module_pdb,
                                             const cml::pe::Image& module_image);

struct PatchWrite {
    std::uint64_t va = 0;
    std::vector<std::uint8_t> bytes;
    std::string what;
};

// For every relocation of the ORIGINAL (pre-anchor) object whose target is an undefined external of
// kind Data or ImportSlot: the fix-up bytes, using the formula in plan section 4.6.
//   anchor_rva  section index -> RVA of __cml_anchor_<n> in the patch image
// Dies (with every problem listed) if a relocation cannot be retargeted.
std::vector<PatchWrite> plan_data_retargets(const cml::coff::Object& original, const std::vector<ExternalInfo>& externals,
                                            const std::unordered_map<std::uint32_t, std::uint32_t>& anchor_rva,
                                            std::uint64_t patch_base, std::uint64_t old_module_base);

}  // namespace spike
