#include "cml/core/patch_image.h"

#include <algorithm>
#include <fstream>

#include "cml/core/bind_diag_ids.h"
#include "cml/core/pipeline_diag_ids.h"
#include "cml/pe/pe.h"
#include "cml/util/log.h"

namespace cml::core {

Result<PatchFixups> to_patch_fixups(std::span<const bind::FixupTriple> triples, std::uint64_t patch_base) {
    PatchFixups out;
    out.wire.reserve(triples.size());
    for (const bind::FixupTriple& t : triples) {
        const std::uint64_t site = patch_base + t.rva;
        ipc::Fixup f;
        f.rva = t.rva;
        switch (t.kind) {
            case bind::FixupKind::Rel32:
            case bind::FixupKind::Rel32_1:
            case bind::FixupKind::Rel32_2:
            case bind::FixupKind::Rel32_3:
            case bind::FixupKind::Rel32_4:
            case bind::FixupKind::Rel32_5: {
                const std::uint32_t n = bind::rel32_extra(t.kind);
                const std::int64_t disp = static_cast<std::int32_t>(static_cast<std::uint32_t>(t.value));
                f.kind = ipc::FixupKind::Rel32;
                f.n = static_cast<std::uint8_t>(n);
                f.value = static_cast<std::uint64_t>(static_cast<std::int64_t>(site) + 4 + n + disp);
                out.wire.push_back(f);
                break;
            }
            case bind::FixupKind::Addr64:
                f.kind = ipc::FixupKind::Addr64;
                f.value = t.value;
                out.wire.push_back(f);
                break;
            case bind::FixupKind::Slot:
                f.kind = ipc::FixupKind::SlotWrite;
                f.value = t.value;
                out.wire.push_back(f);
                break;
            case bind::FixupKind::SecRel32:
                if (t.value > 0xFFFFFFFFull) {
                    return Diag::make(bind::diag::kRel32OutOfRange, "a SECREL value does not fit 32 bits at patch rva " + std::to_string(t.rva),
                                      "Hot-restart; this is not expected for a real module.");
                }
                out.pre_applied.push_back(Write32{t.rva, static_cast<std::uint32_t>(t.value)});
                break;
        }
    }
    return out;
}

Result<void> preapply_write32(const std::string& dll_path, std::span<const Write32> writes) {
    if (writes.empty()) {
        return Result<void>();
    }
    Result<pe::OwnedImage> image = pe::load_image_file(dll_path);
    if (!image) {
        return Diag::make(pipeline::diag::kPatchImage, "cannot read the patch DLL '" + dll_path + "': " + image.error().message,
                          "Check that the linker produced the DLL.");
    }
    // Validate every site first: nothing is written when one is bad.
    std::vector<std::uint64_t> offsets;
    offsets.reserve(writes.size());
    for (const Write32& w : writes) {
        Result<pe::ByteSpan> site = image->image.read_rva(w.rva, 4);
        const std::optional<std::uint64_t> offset = image->image.rva_to_offset(w.rva);
        if (!site || !offset) {
            return Diag::make(pipeline::diag::kPatchImage,
                              "the SECREL site at rva 0x" + std::to_string(w.rva) + " is not inside the file data of a section of '" + dll_path + "'",
                              "Report this with the bind plan; the patch cannot be prepared.");
        }
        offsets.push_back(*offset);
    }
    std::fstream file(dll_path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) {
        return Diag::make(pipeline::diag::kPatchImage, "cannot open the patch DLL '" + dll_path + "' for writing",
                          "Check that no other process holds the file.");
    }
    for (std::size_t i = 0; i < writes.size(); ++i) {
        const char bytes[4] = {static_cast<char>(writes[i].value & 0xFF), static_cast<char>((writes[i].value >> 8) & 0xFF),
                               static_cast<char>((writes[i].value >> 16) & 0xFF), static_cast<char>((writes[i].value >> 24) & 0xFF)};
        file.seekp(static_cast<std::streamoff>(offsets[i]));
        file.write(bytes, 4);
        CML_LOG(Trace, "patch image: SECREL rva=0x%x file offset=0x%llx value=0x%x", writes[i].rva, static_cast<unsigned long long>(offsets[i]), writes[i].value);
    }
    file.flush();
    if (!file) {
        return Diag::make(pipeline::diag::kPatchImage, "writing the pre-applied fix-ups to '" + dll_path + "' failed",
                          "Check free disk space and that no other process holds the file.");
    }
    return Result<void>();
}

}  // namespace cml::core
