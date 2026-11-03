// The CodeView (RSDS) debug record of a PE image: which PDB (GUID, age, path) the image was linked against.
//
// WP-18 / FU-05: this used to be a private, bounds-checked reader of the PE headers (written before WP-02). It is now a thin
// translation of cml_pe's reader, so the repository has ONE PE parser. The behaviour that follows from that, decided here:
//
//   * what cml_pe calls "no RSDS record" (no debug directory, an empty one, or only NB10 / other CodeView signatures) stays
//     CML1408 (kNoCodeViewRecord): there is nothing to look up;
//   * everything cml_pe finds malformed becomes CML1409 (kBadPeImage), with cml_pe's own message appended. That now also
//     covers a PDB path that is not NUL-terminated inside its record (the old reader returned the bytes up to the end of
//     the record: a linker never writes that, so it is treated as corruption) and PE32 images (x86 is out of scope, CML1303);
//   * the public ids and signatures (read_codeview_ref, read_codeview_ref_from_image) are unchanged.
#include <string>

#include "cml/pdb/diag_ids.h"
#include "cml/pdb/index.h"
#include "cml/pe/pe.h"
#include "detail/mapped_file.h"

namespace cml::pdb {

namespace {

// cml_pe's diagnostic without its "What to do" sentence: "CML1312: debug directory: RVA ...".
std::string describe(const Diag& d) {
    return d.id() + ": " + d.message;
}

Diag bad_pe(const std::string& what) {
    return Diag::make(diag::kBadPeImage, "The PE image is malformed: " + what + ".",
                      "Check that the file is a complete, unmodified .exe or .dll.");
}

constexpr std::uint32_t kPeNoRsds = 1313;  // cml_pe: debug directory present but no RSDS CodeView record

}  // namespace

Result<CodeViewRef> read_codeview_ref_from_image(const void* data_in, std::size_t size) {
    if (data_in == nullptr || size == 0) {
        return bad_pe("missing MZ header");
    }
    const pe::ByteSpan bytes(static_cast<const std::byte*>(data_in), size);
    Result<pe::Image> image = pe::Image::parse(bytes, pe::ImageKind::OnDisk);
    if (!image) {
        return bad_pe(describe(image.error()));
    }
    Result<pe::CodeViewInfo> info = image->codeview();
    if (!info) {
        if (info.error().code == kPeNoRsds) {
            return Diag::make(diag::kNoCodeViewRecord, "The PE image has no CodeView (RSDS) debug record.",
                              "Link with /DEBUG so the image records its PDB; PDB 2.0 (NB10) records are not supported.");
        }
        return bad_pe(describe(info.error()));
    }
    if (!info->present) {
        return Diag::make(diag::kNoCodeViewRecord, "The PE image has no CodeView (RSDS) debug record.",
                          "Link with /DEBUG so the image records its PDB.");
    }
    CodeViewRef ref;
    ref.id.guid[0] = static_cast<std::uint8_t>(info->guid.data1);
    ref.id.guid[1] = static_cast<std::uint8_t>(info->guid.data1 >> 8);
    ref.id.guid[2] = static_cast<std::uint8_t>(info->guid.data1 >> 16);
    ref.id.guid[3] = static_cast<std::uint8_t>(info->guid.data1 >> 24);
    ref.id.guid[4] = static_cast<std::uint8_t>(info->guid.data2);
    ref.id.guid[5] = static_cast<std::uint8_t>(info->guid.data2 >> 8);
    ref.id.guid[6] = static_cast<std::uint8_t>(info->guid.data3);
    ref.id.guid[7] = static_cast<std::uint8_t>(info->guid.data3 >> 8);
    for (std::size_t i = 0; i < 8; ++i) {
        ref.id.guid[8 + i] = info->guid.data4[i];
    }
    ref.id.age = info->age;
    ref.pdb_path = std::string(info->pdb_path);
    return ref;
}

Result<CodeViewRef> read_codeview_ref(const std::string& pe_path) {
    auto mapped = detail::MappedFile::open(pe_path);
    if (!mapped) {
        return mapped.error();
    }
    return read_codeview_ref_from_image((*mapped)->data(), static_cast<std::size_t>((*mapped)->size()));
}

}  // namespace cml::pdb
