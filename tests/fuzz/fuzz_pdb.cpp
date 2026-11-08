// Fuzz target: the PDB reader, cml_pdb (WP-28, ADR-0014 "diagnose, never crash").
// Input = the bytes of a .pdb file. cml_pdb maps files, so the input is written to one scratch file per process (overwritten
// for every input) and opened with PdbIndex::open_pdb. When it opens, every query of the index runs once, and
// load_all() forces every compiland's symbol stream through the record walkers. The first 4 KiB, if they look like a PE
// image, are additionally given to the CodeView reference reader.
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
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
    static const ScratchCleanup cleanup;
    std::ofstream out(scratch_path(), std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    return static_cast<bool>(out);
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
    return 0;
}
