// Fuzz target: the PDB reader, cml_pdb (WP-28, ADR-0014 "diagnose, never crash").
// Input = the bytes of a .pdb file. cml_pdb maps files, so the input is written to one scratch file per process (overwritten
// for every input) and opened with PdbIndex::open_pdb. When it opens, every query of the index runs once, and
// load_all() forces every compiland's symbol stream through the record walkers. The first 4 KiB, if they look like a PE
// image, are additionally given to the CodeView reference reader.
//
// WP-37: the type reader. Every proc of the opened index is asked for its canonical signature (TPI / IPI streams, read
// lazily through the type-index offset table), the same file is opened as a type-server PDB, and the raw input is also
// taken as the bytes of an object's `.debug$T` section (type records, signatures of the first indices) and of a
// `.debug$S` section (procedure records).
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "cml/pdb/pdb.h"
#include "fuzz_common.h"

#if defined(_WIN32)
#include <process.h>
#define CML_FUZZ_GETPID _getpid
#else
#include <unistd.h>
#define CML_FUZZ_GETPID getpid
#endif

namespace {

const std::string& scratch_path() {
    static const std::string path = [] {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec) {
            dir = ".";
        }
        return (dir / ("cml_fuzz_pdb_" + std::to_string(CML_FUZZ_GETPID()) + ".pdb")).string();
    }();
    return path;
}

// Removes the scratch file when the process ends normally (the replay run; a fuzzing run that is stopped leaves it in %TEMP%).
struct ScratchCleanup {
    ~ScratchCleanup() {
        std::error_code ec;
        std::filesystem::remove(scratch_path(), ec);
    }
};

bool write_scratch(const std::uint8_t* data, std::size_t size) {
    // The path first: statics die in reverse order of construction, and the cleanup's destructor reads the path. (WP-37: it
    // used to be constructed after the cleanup; the sanitizer build of the replay reported the use after free at exit.)
    const std::string& path = scratch_path();
    static const ScratchCleanup cleanup;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    return static_cast<bool>(out);
}

// Signatures of the indices [0x1000, 0x1000 + count) of a type source, as types and as ids, plus the plain type text.
std::uint64_t exercise_types(const cml::pdb::TypeSource& types, std::uint32_t first, std::uint32_t count) {
    std::uint64_t acc = 0;
    cml::pdb::TypeRecord record;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t index = first + i;
        acc += types.find_type(index, record) ? record.kind + record.body_len : 0u;
        acc += types.find_id(index, record) ? record.kind + record.body_len : 0u;
        for (const bool as_id : {false, true}) {
            const cml::pdb::FunctionSignature s = cml::pdb::function_signature(types, index, as_id);
            acc += s.known ? s.text.size() + s.class_unique_name.size() : s.why.size() + s.diag_id;
        }
        std::string why;
        if (const auto text = cml::pdb::canonical_type(types, index, &why)) {
            acc += text->size();
        }
    }
    return acc;
}

void exercise(const cml::pdb::PdbIndex& idx) {
    std::uint64_t acc = idx.compiland_count() + idx.file_size() + idx.machine() + idx.dbi_age() + idx.info_age();
    for (std::uint32_t i = 0; i < idx.compiland_count(); ++i) {
        const auto& c = idx.compiland(i);
        acc += c.cwd().value_or(std::string_view()).size();
        acc += idx.compiland_files(i).size();
        acc += idx.compiland_files_normalized(i).size();
        if (const auto syms = idx.compiland_symbols(i)) {
            acc += syms->size();
            // WP-37: the signature of every proc (the first 256 of a compiland bound the time of one input).
            std::size_t asked = 0;
            for (const cml::pdb::SymbolView& v : *syms) {
                if (cml::pdb::is_proc_kind(v.kind) && asked++ < 256) {
                    const cml::pdb::FunctionSignature s = idx.proc_signature(v);
                    acc += s.known ? s.text.size() + s.class_unique_name.size() : s.why.size() + s.diag_id;
                    // ADR-0042, amended: its function-local statics and the blocks they sit in.
                    bool complete = true;
                    if (const auto statics = idx.proc_statics(v, &complete)) {
                        for (const cml::pdb::ProcStatic& local : *statics) {
                            acc += local.name.size() + local.type_index + local.rva + local.block_path.size() + (local.plain_scopes ? 1u : 0u);
                        }
                        acc += complete ? 1u : 0u;
                    } else {
                        acc += statics.error().code;
                    }
                }
            }
        }
        if (const auto env = idx.build_env(i)) {
            acc += env->pairs.size() + env->cwd().value_or(std::string_view()).size();
        }
    }
    acc += idx.compilands_for_file("c:\\proj\\src\\logic.h").size();
    acc += idx.compilands_by_object_name("logic.cpp.obj").size();
    acc += idx.compilands_by_object_stem("logic").size();
    acc += idx.find_public("main") ? 1 : 0;
    acc += idx.find_global("g_tick") ? 1 : 0;
    acc += idx.find_global_data("g_tick") ? 1 : 0;
    acc += idx.publics_at_rva(0x1100) ? 1 : 0;
    acc += idx.public_containing_rva(0x1100) ? 1 : 0;
    acc += idx.find_proc_by_rva(0x1105) ? 1 : 0;
    acc += idx.compiland_for_rva(0x1100).value_or(0);
    acc += idx.section_offset_from_rva(0x1100).has_value() ? 1 : 0;
    acc += idx.rva_from_section_offset(1, 0x10).value_or(0);
    acc += idx.contribution_count();
    if (idx.contribution_count() != 0) {
        acc += idx.contribution(0).rva;
    }
    acc += idx.public_count();
    if (const auto all = idx.all_publics()) {
        acc += all->size();
    }
    acc += idx.load_all(2) ? 1 : 0;
    // WP-37: the type streams by index (the low indices, and a few around every power of two up to 2^20: the reader
    // finds the chunk of an index through the offset table, so far indices take another path than near ones).
    acc += exercise_types(idx.types(), 0x0FF8, 72);
    for (std::uint32_t shift = 13; shift <= 20; ++shift) {
        acc += exercise_types(idx.types(), (1u << shift) - 2, 4);
    }
    for (const bool tpi : {true, false}) {
        const cml::pdb::TypeStreamStats st = idx.type_stream_stats(tpi);
        acc += st.chunks + st.chunks_loaded + st.bytes_loaded + st.lookups;
    }
    cml::fuzz::use(acc);
}

// WP-37: the input as the sections of an object.
void exercise_object_sections(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    std::uint64_t acc = 0;
    if (const auto info = cml::pdb::inspect_debug_t(bytes)) {
        acc += static_cast<std::uint64_t>(info->kind) + info->first_own_index + info->server.pdb_path.size() + info->precompiled_object.size();
    } else {
        acc += info.error().code;
    }
    if (const auto types = cml::pdb::ObjectTypes::parse(bytes)) {
        const std::uint32_t count = static_cast<std::uint32_t>(types->record_count() < 96 ? types->record_count() : 96);
        acc += exercise_types(*types, types->first_index(), count + 2);
        acc += exercise_types(*types, 0x0FFE, 4);
    } else {
        acc += types.error().code;
    }
    if (const auto procs = cml::pdb::read_object_procs(bytes)) {
        for (const cml::pdb::ObjectProc& p : *procs) {
            acc += p.offset_field + p.type_index + p.name.size() + (p.type_is_id ? 1u : 0u) + (p.global ? 2u : 0u);
            for (const cml::pdb::ProcStatic& local : p.statics) {
                acc += local.name.size() + local.type_index + local.offset_field + local.block_path.size() + (local.plain_scopes ? 1u : 0u);
            }
            acc += p.statics_complete ? 1u : 0u;
        }
    } else {
        acc += procs.error().code;
    }
    cml::fuzz::use(acc);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace cml;
    if (size >= 2 && data[0] == 'M' && data[1] == 'Z') {
        // A PE image rather than a PDB: the reference reader of the matching logic.
        fuzz::use(pdb::read_codeview_ref_from_image(data, size) ? 1 : 0);
    }
    fuzz::use(pdb::is_valid_pdb_image(data, size) ? 1 : 0);
    exercise_object_sections(data, size);
    if (!write_scratch(data, size)) {
        return 0;
    }
    pdb::OpenOptions options;
    options.use_cache = false;
    {
        Result<std::shared_ptr<const pdb::PdbIndex>> idx = pdb::PdbIndex::open_pdb(scratch_path(), options);
        if (idx) {
            exercise(**idx);
        } else {
            fuzz::use(idx.error().code);
        }
    }
    // Eager variant: publics and every compiland are parsed while opening.
    options.preload_all_symbols = true;
    options.threads = 1;
    {
        Result<std::shared_ptr<const pdb::PdbIndex>> idx = pdb::PdbIndex::open_pdb(scratch_path(), options);
        fuzz::use(idx ? 1 : 0);
    }
    // WP-37: the same file as the compiler PDB of a /Zi object (opened for its types only; no DBI stream needed).
    {
        Result<std::unique_ptr<pdb::TypeServerPdb>> server = pdb::TypeServerPdb::open(scratch_path());
        if (server) {
            fuzz::use(exercise_types(**server, 0x0FF8, 72) + (*server)->id().age);
        } else {
            fuzz::use(server.error().code);
        }
    }
    return 0;
}
