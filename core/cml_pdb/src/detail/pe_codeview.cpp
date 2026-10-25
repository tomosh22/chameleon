// Minimal, bounds-checked extraction of the CodeView (RSDS) debug record from a PE image on disk.
//
// TEMPORARY: WP-02 (core/cml_pe) owns the real PE reader. Once it has merged, this file is to be
// replaced by a call into cml_pe's debug-directory API; the public entry points
// (read_codeview_ref / read_codeview_ref_from_image in cml/pdb/index.h) stay the same.
#include <cstring>
#include <string>

#include "cml/pdb/diag_ids.h"
#include "cml/pdb/index.h"
#include "detail/le.h"
#include "detail/mapped_file.h"

namespace cml::pdb {

namespace {

using detail::rd16;
using detail::rd32;

Diag bad_pe(const std::string& what) {
    return Diag::make(diag::kBadPeImage, "The PE image is malformed: " + what + ".",
                      "Check that the file is a complete, unmodified .exe or .dll.");
}

}  // namespace

Result<CodeViewRef> read_codeview_ref_from_image(const void* data_in, std::size_t size) {
    const auto* data = static_cast<const std::uint8_t*>(data_in);
    if (data == nullptr || size < 0x40 || data[0] != 'M' || data[1] != 'Z') {
        return bad_pe("missing MZ header");
    }
    const std::uint32_t nt = rd32(data + 0x3C);
    if (nt > size || size - nt < 24 || std::memcmp(data + nt, "PE\0\0", 4) != 0) {
        return bad_pe("missing PE signature");
    }
    const std::uint32_t coff = nt + 4;
    const std::uint16_t section_count = rd16(data + coff + 2);
    const std::uint16_t opt_size = rd16(data + coff + 16);
    const std::uint32_t opt = coff + 20;
    if (opt > size || size - opt < opt_size || opt_size < 2) {
        return bad_pe("optional header outside the file");
    }
    const std::uint16_t magic = rd16(data + opt);
    std::uint32_t dir_base;
    if (magic == 0x20B) {
        dir_base = opt + 112;  // PE32+: data directories start at 112
    } else if (magic == 0x10B) {
        dir_base = opt + 96;  // PE32
    } else {
        return bad_pe("unknown optional header magic");
    }
    // Debug is data directory 6; need NumberOfRvaAndSizes and the entry itself.
    const std::uint32_t nrva_off = dir_base - 4;
    if (static_cast<std::uint64_t>(nrva_off) + 4 > static_cast<std::uint64_t>(opt) + opt_size) {
        return bad_pe("optional header too small for data directories");
    }
    const std::uint32_t dir_count = rd32(data + nrva_off);
    if (dir_count <= 6 || static_cast<std::uint64_t>(dir_base) + 7 * 8 > static_cast<std::uint64_t>(opt) + opt_size) {
        return Diag::make(diag::kNoCodeViewRecord, "The PE image has no debug directory.",
                          "Link with /DEBUG so the image records its PDB.");
    }
    const std::uint32_t debug_rva = rd32(data + dir_base + 6 * 8);
    const std::uint32_t debug_size = rd32(data + dir_base + 6 * 8 + 4);
    if (debug_rva == 0 || debug_size == 0) {
        return Diag::make(diag::kNoCodeViewRecord, "The PE image has an empty debug directory.",
                          "Link with /DEBUG so the image records its PDB.");
    }

    // RVA -> file offset through the section table.
    const std::uint32_t sections = opt + opt_size;
    if (static_cast<std::uint64_t>(sections) + static_cast<std::uint64_t>(section_count) * 40 > size) {
        return bad_pe("section table outside the file");
    }
    std::uint64_t dir_file_off = 0;
    std::uint64_t dir_raw_avail = 0;  // bytes of the directory that are really in the file
    bool found = false;
    bool in_virtual_tail = false;
    for (std::uint16_t i = 0; i < section_count; ++i) {
        const std::uint8_t* s = data + sections + static_cast<std::size_t>(i) * 40;
        const std::uint32_t vsize = rd32(s + 8);
        const std::uint32_t vaddr = rd32(s + 12);
        const std::uint32_t rawsize = rd32(s + 16);
        const std::uint32_t rawptr = rd32(s + 20);
        const std::uint32_t span = vsize > rawsize ? vsize : rawsize;
        if (debug_rva >= vaddr && debug_rva - vaddr < span) {
            // Only bytes below SizeOfRawData exist in the file; the rest of the section is
            // zero-filled by the loader and has no file offset.
            if (debug_rva - vaddr >= rawsize) {
                in_virtual_tail = true;
                continue;
            }
            dir_file_off = static_cast<std::uint64_t>(rawptr) + (debug_rva - vaddr);
            dir_raw_avail = rawsize - (debug_rva - vaddr);
            found = true;
            break;
        }
    }
    if (!found) {
        if (in_virtual_tail) {
            return bad_pe("the debug directory is in a section's virtual-only tail (past SizeOfRawData), not in the file");
        }
        return bad_pe("debug directory RVA does not lie in any section");
    }
    std::uint32_t entries = debug_size / 28;
    if (static_cast<std::uint64_t>(entries) * 28 > dir_raw_avail) {
        return bad_pe("the debug directory extends past the section's data in the file");
    }
    for (std::uint32_t e = 0; e < entries; ++e) {
        const std::uint64_t eo = dir_file_off + static_cast<std::uint64_t>(e) * 28;
        if (eo + 28 > size) {
            return bad_pe("debug directory entry outside the file");
        }
        const std::uint8_t* ent = data + eo;
        const std::uint32_t type = rd32(ent + 12);
        const std::uint32_t data_size = rd32(ent + 16);
        const std::uint32_t raw_ptr = rd32(ent + 24);
        if (type != 2 /* IMAGE_DEBUG_TYPE_CODEVIEW */) {
            continue;
        }
        if (data_size < 24 || static_cast<std::uint64_t>(raw_ptr) + data_size > size) {
            return bad_pe("CodeView record outside the file");
        }
        const std::uint8_t* cv = data + raw_ptr;
        if (std::memcmp(cv, "RSDS", 4) != 0) {
            continue;  // NB10 (PDB 2.0) and others are not supported
        }
        CodeViewRef ref;
        std::memcpy(ref.id.guid.data(), cv + 4, 16);
        ref.id.age = rd32(cv + 20);
        const char* path = reinterpret_cast<const char*>(cv + 24);
        const std::size_t max_len = data_size - 24;
        const void* nul = std::memchr(path, 0, max_len);
        const std::size_t len = nul != nullptr ? static_cast<std::size_t>(static_cast<const char*>(nul) - path) : max_len;
        ref.pdb_path.assign(path, len);
        return ref;
    }
    return Diag::make(diag::kNoCodeViewRecord, "The PE image has no CodeView (RSDS) debug record.",
                      "Link with /DEBUG so the image records its PDB; PDB 2.0 (NB10) records are not supported.");
}

Result<CodeViewRef> read_codeview_ref(const std::string& pe_path) {
    auto mapped = detail::MappedFile::open(pe_path);
    if (!mapped) {
        return mapped.error();
    }
    return read_codeview_ref_from_image((*mapped)->data(), static_cast<std::size_t>((*mapped)->size()));
}

}  // namespace cml::pdb
