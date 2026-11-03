#pragma once

// From the bind plan's fix-up triples to what is sent to the agent, and the SECREL decision (WP-12, review
// item 1, ADR-0029).
//
// make_fixup_triples() (WP-07) yields (rva, kind, value) with REL32 displacements already computed. The wire
// format (WP-08, ADR-0025) carries the ABSOLUTE target and lets the agent compute and range-check the
// displacement against the real site address, so Rel32 values are converted back:
//     wire.value = patch_base + rva + 4 + n + sext32(displacement)
// which the agent inverts exactly (`int32(value - (site + 4 + n))`).
//
// SecRel32 (IMAGE_REL_AMD64_SECREL against the old module, one per thread-safe function-local static: the
// CRT's `_Init_thread_epoch`) has no wire kind. DECISION: the value is known before the DLL is loaded (the
// offset of the target inside its image section, from the old PDB, plus the addend) and does not depend on
// where the patch lands, so the broker PRE-APPLIES it to the patch DLL on disk, between the link and the
// LoadPatch. No agent change, no wire change, and the site is written while nothing can execute it. The
// writer checks that the four bytes lie inside the raw data of one section of the file. (The alternative, a
// `Write32` fix-up kind in the agent, would add a wire kind, a decoder branch and an agent code path for a
// value that is constant at link time.)

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cml/core/bind_plan.h"
#include "cml/ipc/messages.h"
#include "cml/util/result.h"

namespace cml::core {

struct Write32 {
    std::uint32_t rva = 0;
    std::uint32_t value = 0;
};

struct PatchFixups {
    std::vector<ipc::Fixup> wire;      // Rel32 / Addr64 / SlotWrite, in rva order
    std::vector<Write32> pre_applied;  // SecRel32, in rva order
};

// CML3205 for a value that cannot be represented (never for a plan that make_fixup_triples() accepted).
Result<PatchFixups> to_patch_fixups(std::span<const bind::FixupTriple> triples, std::uint64_t patch_base);

// Writes each value (little-endian, 4 bytes) into the file at the file offset of its RVA. The file must be a
// PE image; every site must lie inside the raw data of one section (CML5310 otherwise; nothing is written
// when any site is bad).
Result<void> preapply_write32(const std::string& dll_path, std::span<const Write32> writes);

}  // namespace cml::core
