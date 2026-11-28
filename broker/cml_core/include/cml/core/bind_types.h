#pragma once

// WP-37 (ADR-0042): the signatures of the functions of a patch object, for the bind planner.
//
// The planner tells same-named static functions apart by their canonical signature (cml/pdb/type_records.h). The old
// side is the module PDB (PdbIndex::proc_signature). This is the new side: the object's procedure records in
// `.debug$S` say which type each function has, and the type records come from
//   debugT      the object's own `.debug$T` (compiled with /Z7, the usual rewrite of the patch compile), or
//   typeserver  the compiler PDB named by LF_TYPESERVER2 (compiled with /Zi: a cl translation unit that uses a /Zi
//               precompiled header keeps /Zi and a private compiler PDB, ADR-0026), or
//   unreadable  neither (no debug sections, the compiler PDB is missing or is another build's, a malformed section).
// "unreadable" never guesses: every signature is then unknown and the planner keeps its CML3214.
//
// Nothing is read before the first at(): planning a translation unit without same-named statics costs nothing.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/pdb/index.h"
#include "cml/pdb/type_records.h"

namespace cml::core::bind {

enum class ObjectTypeOrigin : std::uint8_t { NotRead, DebugT, TypeServer, Unreadable };
const char* to_string(ObjectTypeOrigin origin) noexcept;  // "not-read", "debugT", "typeserver", "unreadable"

class ObjectSignatures {
public:
    ObjectSignatures(const coff::Object& object, std::string object_name);
    ~ObjectSignatures();
    ObjectSignatures(const ObjectSignatures&) = delete;
    ObjectSignatures& operator=(const ObjectSignatures&) = delete;

    // The signature of the function whose code starts at `value` in object section `section` (1-based). Unknown (with
    // the reason) when the types are unreadable or the function has no procedure record.
    const pdb::FunctionSignature& at(std::uint32_t section, std::uint32_t value);

    // ADR-0042, amended: the function-local statics of the object's procedures, as `.debug$S` records them (the planner
    // tells same-named statics of one procedure apart by their place in it; the old side is PdbIndex::proc_statics).
    struct LocalStatic {
        pdb::ProcStatic record;
        std::uint32_t section = 0;  // the data symbol the record names through its SECREL relocation; 0: none found
        std::uint32_t value = 0;
    };
    struct ProcStatics {
        std::vector<LocalStatic> statics;  // in the order the compiler emitted them
        bool usable = true;                // false: `why`
        std::string why;
    };
    // The statics declared in the function whose code starts at (section, value). nullptr: the function has no
    // procedure record, or the debug sections are unreadable (detail()).
    const ProcStatics* statics_of(std::uint32_t section, std::uint32_t value);
    // The function, as (section << 32) | offset, in which the data symbol at (section, value) is declared. nullopt: no
    // record of a procedure names it, or records of two procedures do (`several`, when given, says which).
    std::optional<std::uint64_t> function_of_static(std::uint32_t section, std::uint32_t value, bool* several = nullptr);
    // The canonical text of a type of the object (cml::pdb::canonical_type). nullopt with `why` when unreadable.
    std::optional<std::string> type_text(std::uint32_t type_index, std::string* why);
    // WP-46 (F-41.2 review): the functions, as (section << 32) | offset, whose procedure records have a local variable of the
    // class with this unique name (without the unit's discriminator; through modifiers, never through a pointer).
    std::vector<std::uint64_t> functions_with_local(const std::string& unique);
    // WP-46: the layout of the class with this unique name (pdb::udt_layout of its definition record). nullopt with `why`.
    std::optional<std::string> class_layout(const std::string& unique, std::string* why);

    // WP-60 (R-57): what the function whose code starts at (section, value) inlined (its procedure record's inline sites at
    // any depth), each inlinee resolved through the object's types (PdbIndex::resolve_inlinee). nullptr: no procedure record,
    // two of them, the types unreadable, or a site that cannot be read.
    const std::vector<pdb::PdbIndex::Inlinee>* inlinees_of(std::uint32_t section, std::uint32_t value);
    // WP-60: the checksums of the source files of inlinee `id` (an id of the object's types), from the inlinee lines and the file
    // checksums of the object's .debug$S sections (pdb::inlinee_files). nullopt: not recorded, not unambiguously, or a C13
    // section of the object could not be read.
    std::optional<std::vector<pdb::FileChecksum>> inlinee_files(std::uint32_t id);

    // Where the types came from; NotRead before the first at().
    ObjectTypeOrigin origin() const noexcept { return origin_; }
    // Unreadable: why. DebugT / TypeServer: a note ("precompiled types are not resolved", the PDB path).
    const std::string& detail() const noexcept { return detail_; }
    std::size_t proc_records() const noexcept { return procs_.size(); }

private:
    struct Proc {
        std::uint32_t type_index = 0;
        bool type_is_id = false;
        bool conflict = false;  // two records for one address disagree
        bool computed = false;
        pdb::FunctionSignature signature;
        ProcStatics statics;
        std::vector<std::uint32_t> local_types;  // WP-46
        std::vector<std::uint32_t> inlinee_ids;  // WP-60
        bool inlinees_usable = true;
        bool inlinees_resolved = false;
        std::vector<pdb::PdbIndex::Inlinee> inlinees;
    };
    pdb::C13Inlinees c13_;           // WP-60: the inlinee lines and file checksums of every .debug$S section
    std::size_t c13_bad_sections_ = 0;  // WP-60: C13 sections whose line information could not be read

    void load();
    void unreadable(std::uint32_t diag_id, std::string why);

    const coff::Object& object_;
    std::string object_name_;
    bool loaded_ = false;
    ObjectTypeOrigin origin_ = ObjectTypeOrigin::NotRead;
    std::string detail_;
    std::unique_ptr<pdb::ObjectTypes> object_types_;
    std::unique_ptr<pdb::TypeServerPdb> type_server_;
    const pdb::TypeSource* types_ = nullptr;
    std::map<std::uint64_t, Proc> procs_;  // (section << 32) | offset
    static constexpr std::uint64_t kSeveralFunctions = ~0ull;
    std::map<std::uint64_t, std::uint64_t> static_owner_;  // data (section << 32) | offset -> its function, or kSeveralFunctions
    pdb::FunctionSignature unknown_;       // what at() returns when nothing can be known
    pdb::FunctionSignature no_record_;
};

}  // namespace cml::core::bind
