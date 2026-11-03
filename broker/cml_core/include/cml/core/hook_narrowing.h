#pragma once

// Narrowing the hook list to the functions that really changed (WP-12, plan 4.5 + review item 3).
//
// The bind planner hooks every function the patched object defines and the old image also has: for an
// STL-heavy translation unit that is up to 83 functions, nearly all of them untouched instantiations. Each
// extra hook is a chance for a refusal (a 3-byte function, CML4301) that would fail the whole batch, and each
// needs the application's threads stopped. The old copy of an unchanged function keeps running, which is
// correct: it is the same code.
//
// The rule (documented in docs/wp/WP-12.md). A hook is DROPPED only when everything below holds, and KEPT
// ("when in doubt, hook") in every other case:
//
//   1. the function is in the module file (old RVA known), its entry is not currently redirected by an
//      earlier patch (`is_hooked`), and the PDB gave its size;
//   2. the new code is as long as the old code, and the bytes are equal with the 4 (or 8) bytes of every
//      relocation site masked out ("compared normalised for relocations"): the old bytes come from the module
//      file on disk, the new ones from the compiler's object;
//   3. every relocation resolves to the SAME target as in the old code:
//        * a symbol that lives in the old image (bound data / function, a hooked function, __ImageBase): the
//          target address recovered from the old displacement (S = disp + site + 4 + N - A) equals the address
//          the planner resolved for it; SECREL sites hold the planned offset; ADDR64 sites the planned address;
//        * read-only data the patch carries as a fresh copy (string literals, constant tables): the bytes of
//          the new copy equal the bytes at the old target (relocations inside it masked);
//        * anything else (a new function or global, a fresh function, a section symbol, a writable fresh
//          symbol, a relocation type that cannot be checked): the function is kept.
//
// Rule 3 is what the plain "mask relocation sites" comparison lacks: `call helper_a` -> `call helper_b` or a
// string literal edit leaves the masked bytes identical.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/core/bind_plan.h"
#include "cml/pe/pe.h"

namespace cml::core {

enum class NarrowReason : std::uint8_t {
    Unchanged,            // dropped: identical code, identical targets
    BytesDiffer,          // kept
    SizeDiffers,          // kept
    UnknownOldSize,       // kept: the PDB did not give a size
    OldBytesUnavailable,  // kept: not in the module file, or not readable
    AlreadyHooked,        // kept: the old entry currently jumps to an earlier patch
    TargetChanged,        // kept: a relocation resolves differently than in the old code
    TargetNew,            // kept: a relocation refers to something the old image does not have
    Unverifiable,         // kept: a relocation or symbol this check cannot judge
    Disabled,             // kept: narrowing switched off
};

const char* to_string(NarrowReason reason) noexcept;
inline bool is_dropped(NarrowReason reason) noexcept { return reason == NarrowReason::Unchanged; }

struct NarrowDecision {
    std::string symbol;   // decorated name of the patch definition
    std::string display;
    NarrowReason reason = NarrowReason::Unchanged;
    std::string detail;   // e.g. "byte 12 differs", "relocation to 'g' resolves to 0x..., expected 0x..."
};

struct NarrowInput {
    const coff::Object* object = nullptr;
    const bind::BindPlan* plan = nullptr;
    // The module as stored on disk (ImageKind::OnDisk); may be null: nothing can then be compared.
    const pe::Image* old_image = nullptr;
    std::uint64_t load_base = 0;  // where the module is loaded in the process
    // True when the old entry currently redirects to an earlier patch (always kept: the new code may equal the
    // original but the entry must still be re-pointed).
    std::function<bool(std::uint64_t old_va)> is_hooked;
    bool enabled = true;
};

struct NarrowResult {
    std::vector<bind::HookEntry> hooks;  // the hooks to install, in plan order
    std::vector<NarrowDecision> decisions;  // one per plan hook (kept and dropped), in plan order
    std::size_t dropped = 0;
};

// Never fails: whatever cannot be judged is kept. Every decision is logged at trace level.
NarrowResult narrow_hooks(const NarrowInput& input);

}  // namespace cml::core
