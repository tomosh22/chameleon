#pragma once

// Writing into a loaded patch image: fix-ups, thunk slots and initializers (plan sections 4.6,
// 4.10; WP-08).
//
// Both functions work on any committed image mapping (MEM_IMAGE) that the caller describes with
// an ImageRef, so they can be tested on a DLL loaded by an ordinary LoadLibrary call. They
//   * validate every entry before touching memory (a REL32 that does not fit 32 bits, a site
//     outside the image, a misaligned slot, an unreadable page all fail the whole call with
//     nothing written);
//   * change the protection of only the regions that need it (read-only data, code), write, and
//     restore the exact previous protection, flushing the instruction cache for code regions;
//   * apply_fixups never allocates on its success path: the only state is on the stack, so it may
//     run while allocation is forbidden (heap::set_alloc_forbidden). A failure builds a diagnostic
//     string and therefore allocates. run_initializers allocates a small table and so does not
//     qualify; it runs user code anyway and belongs outside any suspended-threads window.
// Every function may be called from any thread; two threads must not write the same image at once.

#include <cstdint>
#include <span>

#include "cml/ipc/messages.h"
#include "cml/util/result.h"

#if defined(_WIN32)

namespace cml::agent::loader {

// A mapped image: base address and SizeOfImage.
struct ImageRef {
    std::uint64_t base = 0;
    std::uint32_t size = 0;
};

// Applies the fix-ups in two passes: first every Rel32 and Addr64, then every SlotWrite (plan
// order: fix-ups, then slots). Returns the number of fix-ups written. CML4207 (REL32 out of range),
// CML4208 (invalid site or protection failure).
Result<std::uint32_t> apply_fixups(const ImageRef& image, std::span<const ipc::Fixup> fixups);

// Runs the initializers whose table entries (8-byte function pointers, usually in `.CRT$XCU`) are
// at the given RVAs, in the given order, on the calling thread. A null entry is skipped. Entries
// are validated first (inside the image, in an executable region). A fault inside an initializer
// is caught (SEH) and reported; initializers that already ran are not undone. Returns the number
// run. CML4209.
Result<std::uint32_t> run_initializers(const ImageRef& image, std::span<const std::uint32_t> rvas);

}  // namespace cml::agent::loader

#endif  // _WIN32
