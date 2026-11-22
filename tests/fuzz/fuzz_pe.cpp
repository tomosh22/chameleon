// Fuzz target: the PE32+ reader (WP-28). Input = a PE file (on-disk layout); the same bytes are also parsed as an in-memory
// (mapped) image. Every directory reader runs, and read_rva / read_cstring_rva / the address mapping are probed at the
// RVAs the image names.
#include <optional>
#include <vector>

#include "cml/pe/pe.h"
#include "fuzz_common.h"

namespace {

void walk(const cml::pe::Image& image) {
    using namespace cml;
    std::uint64_t acc = image.sections().size() + image.optional_header().size_of_image;
    if (const auto exports = image.exports(); exports) {
        acc += exports->entries.size();
        for (const pe::Export& e : exports->entries) {
            acc += e.name.size() + e.forwarder.size();
        }
        if (!exports->entries.empty() && exports->entries.front().has_name) {
            acc += exports->find_by_name(exports->entries.front().name) != nullptr ? 1 : 0;
        }
        acc += exports->find_by_ordinal(exports->ordinal_base) != nullptr ? 1 : 0;
    }
    if (const auto imports = image.imports(); imports) {
        for (const pe::ImportModule& m : *imports) {
            acc += m.dll_name.size() + m.entries.size();
            for (const pe::ImportEntry& e : m.entries) {
                acc += e.name.size();
            }
        }
        acc += pe::find_import(*imports, "kernel32.dll", "ExitProcess") != nullptr ? 1 : 0;
    }
    if (const auto debug = image.debug_entries(); debug) {
        acc += debug->size();
    }
    if (const auto cv = image.codeview(); cv) {
        acc += cv->pdb_path.size() + cv->age;
    }
    if (const auto ex = image.exception_table(); ex) {
        acc += ex->entries.size();
        if (!ex->entries.empty()) {
            acc += ex->find(ex->entries.front().begin_rva) != nullptr ? 1 : 0;
            acc += ex->find(0xFFFFFFFFu) != nullptr ? 1 : 0;
        }
    }
    if (const auto lc = image.load_config(); lc) {
        acc += lc->size;
    }
    acc += image.has_tls() ? 1 : 0;
    if (const auto tls = image.tls(); tls) {
        acc += tls->size_of_zero_fill;
    }
    for (const pe::SectionHeader& s : image.sections()) {
        const std::uint32_t probes[] = {s.virtual_address, s.virtual_address + s.virtual_size, s.virtual_address + s.virtual_size - 1,
                                        s.virtual_address + 0x10u};
        for (const std::uint32_t rva : probes) {
            if (const std::optional<std::uint64_t> off = image.rva_to_offset(rva)) {
                acc += *off;
                acc += image.offset_to_rva(*off).value_or(0);
            }
            if (const auto span = image.read_rva(rva, 16); span) {
                acc += span->size();
            }
            if (const auto str = image.read_cstring_rva(rva); str) {
                acc += str->size();
            }
        }
        acc += image.read_rva(s.virtual_address, 0xFFFFFFFFFFull) ? 1 : 0;
    }
    acc += image.find_section(".text") != nullptr ? 1 : 0;
    acc += image.section_for_rva(0x1000) != nullptr ? 1 : 0;
    acc += image.data_directory(pe::dir::Count - 1).size;
    cml::fuzz::use(acc);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace cml;
    const pe::ByteSpan bytes(reinterpret_cast<const std::byte*>(data), size);
    for (const pe::ImageKind kind : {pe::ImageKind::OnDisk, pe::ImageKind::InMemory}) {
        Result<pe::Image> image = pe::Image::parse(bytes, kind);
        if (image) {
            walk(*image);
        } else {
            fuzz::use(image.error().code);
        }
    }
    return 0;
}
