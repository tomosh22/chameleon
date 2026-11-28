#pragma once

// WP-37 (ADR-0042): CodeView type records, read the same way from the three places they live in:
//   * the TPI and IPI streams of a linked PDB (PdbIndex, lazily; see PdbIndex::proc_signature),
//   * the `.debug$T` section of an object compiled with /Z7 (ObjectTypes),
//   * the compiler PDB an object compiled with /Zi refers to through LF_TYPESERVER2 (TypeServerPdb).
// All three are a TypeSource, and function_signature() is the ONE routine that turns a function's type into a canonical
// text. The bind planner compares those texts; it never compares type indices of two sources and never reads a
// signature out of a decorated name.
//
// Everything here parses untrusted input: a malformed record gives "unknown" (FunctionSignature::known == false with a
// CML14xx id and a reason), never a crash or a read outside the record.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cml/pdb/types.h"
#include "cml/util/result.h"

namespace cml::pdb {

// One type record: the bytes after the 2-byte length and the 2-byte leaf kind.
struct TypeRecord {
    std::uint16_t kind = 0;
    const std::uint8_t* body = nullptr;
    std::uint32_t body_len = 0;
};

// A set of type records addressed by index. Types and ids are separate index spaces in a PDB (TPI, IPI) and one space
// in an object. Returned bytes stay valid for as long as the source lives. Implementations are thread-safe.
class TypeSource {
public:
    virtual ~TypeSource() = default;
    virtual bool find_type(std::uint32_t index, TypeRecord& out) const = 0;
    virtual bool find_id(std::uint32_t index, TypeRecord& out) const = 0;
};

// The canonical signature of a function.
//   text   calling convention, function attributes, return type, parameter list and, for a member function, its class,
//          the type of `this` and the `this` adjustment. Every referenced type is rendered structurally (pointer
//          attributes, modifiers, array sizes) down to classes, structs, unions and enums, which are rendered by their
//          unique (decorated) name when the record has one and by their name otherwise. No type index of a record
//          appears in it, so the text of a function is the same in the linked PDB and in an object of the same compiler.
//          Unique names are written without their translation-unit discriminator (without_tu_discriminator, below).
//   class_unique_name   member functions: the unique name of the class exactly as recorded
//          (".?AV<lambda_1>@?1??f@@YAXXZ@`a8f45668" for a closure under cl, without the suffix under clang-cl).
struct FunctionSignature {
    bool known = false;
    std::string text;
    bool member = false;
    std::string class_unique_name;
    std::string class_name;
    // known == false: why (a cml::pdb::diag id, CML1430..1439, and one line of text).
    std::uint32_t diag_id = 0;
    std::string why;
};

// A unique name without the parts that only say WHICH translation unit an internal type belongs to:
//   * cl appends a backtick and eight hex digits to the unique name of a type that is local to a function
//     (".?AV<lambda_1>@?1??f@@YAXXZ@`a8f45668"); the digits are the hash that also names the unit's anonymous
//     namespace ("?A0xa8f45668"), which both compilers derive from the path of the source file;
//   * an anonymous-namespace component "?A0x<hash>@" becomes "?A0x@".
// Signatures are only ever compared between the records of ONE translation unit (its compiland in the old PDB and its
// patch object), where that hash is a constant and tells nothing apart; leaving it out keeps a signature stable when
// the unit is compiled from another path (a build tree that was moved, the unit tests' edited copies). The decorated
// names of symbols never carry the backtick suffix, so the planner needs the closure name in this form as well.
std::string without_tu_discriminator(std::string_view unique_name);

// `index` is a type index (LF_PROCEDURE / LF_MFUNCTION) or, with `index_is_id`, an id index (LF_FUNC_ID / LF_MFUNC_ID):
// what S_GPROC32 / S_LPROC32 and their _ID forms carry.
FunctionSignature function_signature(const TypeSource& types, std::uint32_t index, bool index_is_id);

// The canonical text of one type (tools and tests). nullopt with `why` when it cannot be rendered.
std::optional<std::string> canonical_type(const TypeSource& types, std::uint32_t type_index, std::string* why = nullptr);

// WP-46 (R-54): the name and the unique name of an LF_CLASS / LF_STRUCTURE / LF_INTERFACE / LF_UNION / LF_ENUM record
// (`unique` empty when the record has none). False for any other record and for a malformed one.
bool udt_record_names(const TypeRecord& record, std::string_view& name, std::string_view& unique);

// WP-46 (R-54): a closure class (lambda) of a PDB's TPI stream: its name ("o2_step::<lambda_0>") and its unique name
// without the translation-unit discriminator (".?AV<lambda_0>@?0??o2_step@@YAXXZ@"); `definition` is the index of its
// definition record (not a forward reference), 0 when the stream has none.
struct ClosureClass {
    std::string name;
    std::string unique;
    std::uint32_t definition = 0;
};

// WP-46 (F-41.2 review): the layout of a class or struct definition (not a forward reference) as a canonical text: its size,
// then for every data member its offset and canonical type ("size=8;0:t74;4:t74"). Methods and nested types are read past,
// any other field leaf makes it unreadable. nullopt with `why` when it cannot be read.
std::optional<std::string> udt_layout(const TypeSource& types, std::uint32_t index, std::string* why = nullptr);
// WP-46: the unique name, without the unit's discriminator, of the class or struct a type index names, through LF_MODIFIER
// records (const, volatile) but never through a pointer or reference. Empty for anything else.
std::string class_unique_name_of(const TypeSource& types, std::uint32_t index);
// WP-46 (review): the size of a class / struct / union DEFINITION record (nullopt for a forward reference or another kind).
std::optional<std::uint64_t> udt_definition_size(const TypeRecord& record);
// WP-46 (review): the size in bytes of a type: simple types, pointers, modifiers, enums, arrays, class definitions; a forward
// reference asks `forward` (by its exact unique name, the unit's discriminator included). nullopt when it cannot be known.
std::optional<std::uint64_t> type_size_of(const TypeSource& types, std::uint32_t index,
                                          const std::function<std::optional<std::uint64_t>(const std::string& unique)>& forward);

// ---- objects ---------------------------------------------------------------------------------------------------------

// What LF_TYPESERVER2 says: the compiler PDB that holds the object's types.
struct TypeServerRef {
    PdbId id;              // GUID and age as recorded in the object
    std::string pdb_path;  // as the compiler wrote it (the /Fd file)
};

// What a `.debug$T` section holds.
struct DebugTInfo {
    enum class Kind : std::uint8_t {
        Records,      // the object's own type records (/Z7)
        TypeServer,   // one LF_TYPESERVER2 record: the types are in a compiler PDB (/Zi)
        Precompiled,  // LF_PRECOMP first: indices below `first_own_index` live in the PCH object (/Z7 /Yu)
        Empty,        // no records
    };
    Kind kind = Kind::Empty;
    TypeServerRef server;                // TypeServer
    std::uint32_t first_own_index = 0x1000;  // Precompiled: index of the first record after LF_PRECOMP
    std::string precompiled_object;      // Precompiled: the PCH object the compiler named
};

// Reads the head of a `.debug$T` section. CML1432 when it is not a C13 type section.
Result<DebugTInfo> inspect_debug_t(std::span<const std::uint8_t> section);

// The type records of an object's `.debug$T` (types and ids share one index space). For a Precompiled section the
// object's own records are readable and the PCH's are not (find_* returns false for them, so a signature that needs one
// is "unknown").
class ObjectTypes final : public TypeSource {
public:
    // CML1432: not a type section, or a type-server reference (use TypeServerPdb). CML1431: a record leaves the section.
    static Result<ObjectTypes> parse(std::span<const std::uint8_t> section);

    bool find_type(std::uint32_t index, TypeRecord& out) const override;
    bool find_id(std::uint32_t index, TypeRecord& out) const override { return find_type(index, out); }

    const DebugTInfo& info() const noexcept { return info_; }
    std::uint32_t first_index() const noexcept { return first_index_; }
    std::size_t record_count() const noexcept { return offsets_.size(); }

private:
    std::span<const std::uint8_t> bytes_;
    DebugTInfo info_;
    std::uint32_t first_index_ = 0x1000;
    std::vector<std::uint32_t> offsets_;  // of each record header within bytes_
};

// A compiler PDB opened for its types only (it has no usable DBI stream, so PdbIndex cannot open it). The file stays
// mapped while the object lives: destroy it before the compiler writes the PDB again.
class TypeServerPdb final : public TypeSource {
public:
    ~TypeServerPdb() override;
    TypeServerPdb(const TypeServerPdb&) = delete;
    TypeServerPdb& operator=(const TypeServerPdb&) = delete;

    // CML1400.. when the file cannot be opened or is not a PDB, CML1430 when its TPI stream is unusable.
    static Result<std::unique_ptr<TypeServerPdb>> open(const std::string& pdb_path);

    bool find_type(std::uint32_t index, TypeRecord& out) const override;
    bool find_id(std::uint32_t index, TypeRecord& out) const override;
    // WP-46: calls `visit` for every TPI record in index order (chunks not kept); false when the stream is malformed or its
    // records do not add up to its index range.
    bool walk_types(const std::function<void(std::uint32_t index, const TypeRecord& record)>& visit) const;

    // GUID and age of the PDB info stream (what LF_TYPESERVER2 records).
    const PdbId& id() const noexcept;
    const std::string& path() const noexcept;

    struct Impl;

private:
    explicit TypeServerPdb(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// A data record (S_LDATA32 / S_GDATA32 / S_LTHREAD32 / S_GTHREAD32) declared inside a procedure: a function-local
// static. (ADR-0042, amended: two statics of one name in one procedure, declared in different blocks, have nothing that
// tells them apart but their place in the procedure. The planner compares that place in the old PDB and in the patch
// object, so both are read by the same code: PdbIndex::proc_statics and read_object_procs.)
struct ProcStatic {
    std::string name;                // as recorded: the source name ("names", "$TSS0")
    std::uint32_t type_index = 0;
    bool thread_storage = false;     // S_LTHREAD32 / S_GTHREAD32
    std::uint32_t rva = kNoRva;      // PDB: where the variable is. Object: kNoRva (the record is relocated)
    std::uint32_t offset_field = 0;  // object: offset, within the section, of the record's 32-bit offset (its SECREL site)
    // Where in the procedure it was declared: for every S_BLOCK32 open around the record, outermost first, the index of
    // that block among the blocks of its parent scope. Empty: directly in the procedure.
    std::vector<std::uint32_t> block_path;
    // False when the record is inside a scope that is not a block (an inline site: the static of an inlined function)
    // or nested deeper than the reader follows; its place in the procedure is then not known.
    bool plain_scopes = true;
};

// A procedure record (S_GPROC32 / S_LPROC32 and their _ID forms) of an object's `.debug$S` section. In an object the
// record's section:offset is zero and two relocations (SECREL at `offset_field`, SECTION right after it) name the
// function; the caller resolves them with the COFF reader.
struct ObjectProc {
    std::uint32_t offset_field = 0;  // offset, within the section, of the record's 32-bit code offset
    std::uint32_t type_index = 0;
    bool type_is_id = false;         // *_ID record: `type_index` is an id (LF_FUNC_ID / LF_MFUNC_ID)
    bool global = false;             // S_GPROC32*
    std::string_view name;           // display name, points into the section
    std::vector<ProcStatic> statics; // the data records declared inside it, in the order the compiler emitted them
    // WP-46: the types of its own local variables (S_LOCAL without fIsParam, S_REGREL32, S_BPREL32; none of an inlined call's)
    std::vector<std::uint32_t> local_types;
    bool statics_complete = true;    // false: more than kMaxProcStatics, the rest was dropped
    // WP-60 (R-57): the distinct inlinees (ids of the object's types) of its S_INLINESITE / S_INLINESITE2 records at any depth,
    // in the order of first appearance. false: a site too short to name one, or more than kMaxProcStatics of them.
    std::vector<std::uint32_t> inlinees;
    bool inlinees_complete = true;
};
// A procedure with more function-local statics than this is not followed further (untrusted input).
inline constexpr std::size_t kMaxProcStatics = 4096;

// Walks the symbol subsections of one `.debug$S` section. CML1433 when the section is not C13 or a subsection or
// record leaves it.
Result<std::vector<ObjectProc>> read_object_procs(std::span<const std::uint8_t> section);

// WP-60 (R-57): the source file of an inlined function, as C13 line information records it. DEBUG_S_INLINEELINES names, per
// inlinee id, an entry of the DEBUG_S_FILECHKSMS subsection (the file's checksum: kind 1 MD5, 2 SHA1, 3 SHA256; 0 none).
struct FileChecksum {
    std::uint8_t kind = 0;
    std::vector<std::uint8_t> bytes;
    bool operator==(const FileChecksum& o) const { return kind == o.kind && bytes == o.bytes; }
};
struct C13Inlinees {
    // (inlinee id, its file ids: offsets into the checksum subsection; the first is the file of its lines, the rest the extra files
    // of CV_INLINEE_SOURCE_LINE_SIGNATURE_EX)
    std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> lines;
    std::vector<std::uint8_t> checksums;                         // the first DEBUG_S_FILECHKSMS subsection seen
    std::size_t checksum_tables = 0;                             // how many were seen (an object may have one per section)
};
// Appends the inlinee lines and the file checksum subsection of a run of C13 subsections (a module stream's C13 block, or an
// object's .debug$S section after its 4-byte signature). CML1433 when a subsection leaves the span.
Result<void> read_c13_inlinees(std::span<const std::uint8_t> subsections, C13Inlinees& out);
// The checksums of every file the inlinee's lines name, the file of its lines first, then its extra files. nullopt: no entry
// for it, entries naming different files, more than one checksum subsection, an entry outside the checksum subsection, or any
// of its files recorded without a checksum (kind 0).
std::optional<std::vector<FileChecksum>> inlinee_files(const C13Inlinees& c13, std::uint32_t inlinee);

// Statistics of the lazily read type streams of a PdbIndex / TypeServerPdb (logs, tests, the benchmark).
struct TypeStreamStats {
    bool opened = false;            // the header was read
    bool sparse = false;            // the index-offset table of the hash stream is used (records are read by chunk)
    std::uint32_t chunks = 0;       // chunks the stream is divided into (1 when not sparse)
    std::uint32_t chunks_loaded = 0;
    std::uint64_t record_bytes = 0;  // size of the record area
    std::uint64_t bytes_loaded = 0;  // record bytes walked so far
    std::uint64_t bytes_copied = 0;  // of those, bytes copied because their blocks are not consecutive in the file
    std::uint32_t lookups = 0;
};

}  // namespace cml::pdb
