#pragma once

// PatchLoader: address-space reservation and patch loading inside the agent (plan sections 4.6,
// 4.7, 4.10; WP-08).
//
// What it owns
//   * "Owned" address space: blocks reserved with VirtualAlloc(MEM_RESERVE) near a module, so that a
//     patch linked `/FIXED` inside a block can reach the module (and be reached) with REL32.
//   * The patches it loaded: the HMODULE, the image range, and a busy flag.
//
// Loading one patch (LoadPatch), all under the loader's lock until step 3:
//   1. release exactly the image's range back to the OS (the rest of the block is re-reserved at
//      once, so only the image range is open to others);
//   2. LoadLibraryExW(path);
//   3. verify the image sits at the requested base and has the announced size;
//   4. apply fix-ups (REL32 range-checked), then write thunk slots;
//   5. run the named initializers on the calling thread;
// any failure unloads the DLL and puts the range back to the reserved state. Between 1 and 2 the
// range is open to every other thread in the process (ADR-0020: that race is inherent). If
// another thread takes it, LoadLibrary fails; the loader then tries the next candidate (a /FIXED
// DLL cannot move, so each candidate is a different pre-linked file and base), up to 5 times.
//
// Threading: every public function may be called from any thread. One mutex guards the tables and
// is held across steps 1-3 (so two loads, or a load and a reservation scan, cannot disturb each
// other's window); it is NOT held while fix-ups or initializers run - the patch is marked busy
// instead, and other operations on it fail with CML4212.
//
// Allocation: nothing here may be called while heap::alloc_forbidden() (host threads suspended).

#if defined(_WIN32)

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "cml/ipc/messages.h"
#include "cml/proc/address_space.h"
#include "cml/util/result.h"

namespace cml::agent::loader {

struct ReserveParams {
    std::uint64_t module_base = 0;
    std::uint64_t module_size = 0;
    std::uint64_t block_size = 64ull << 20;   // rounded up to 64 KiB; at most 1 GiB
    std::uint64_t reach = 1536ull << 20;      // 0 selects the default
    std::uint32_t block_count = 4;            // 1..64
};

// Test hooks. after_release runs under the loader lock after the image range was released and
// before LoadLibraryExW (this is exactly the race window). `attempt` counts the candidates tried
// so far in one LoadPatch, from 1 (a candidate that is refused before its release counts too).
struct TestHooks {
    void (*after_release)(void* context, std::uint64_t base, std::uint64_t size, std::uint32_t attempt) = nullptr;
    // Invoked right after a patch was placed, while it is marked busy and the loader lock is NOT
    // held (so the hook may call the loader to prove that the busy patch is refused).
    void (*while_busy)(void* context, std::uint64_t image_base) = nullptr;
    void* context = nullptr;
};

class PatchLoader {
public:
    PatchLoader() = default;
    // Releases the address space still owned (reserved but unused). Loaded patches stay loaded:
    // hooks may point into them.
    ~PatchLoader();
    PatchLoader(const PatchLoader&) = delete;
    PatchLoader& operator=(const PatchLoader&) = delete;

    // The loader the agent uses for broker requests. Created on first use and never destroyed: the
    // patches and reservations belong to the process, not to one Agent object.
    static PatchLoader& shared();

    // --- address space ---------------------------------------------------------------------
    // Reserves up to block_count blocks near the module. Succeeds when at least one block was
    // reserved (the caller sees how many); CML4201 when none was, CML4200 for bad parameters.
    Result<std::vector<proc::AddressRange>> reserve_ranges(const ReserveParams& params);

    // Reserves exactly [base, base+size) (64 KiB aligned) and owns it. Used by tests and by
    // callers that need a specific range.
    Result<void> reserve_exact(std::uint64_t base, std::uint64_t size);

    // Gives [base, base+size) of owned, unused address space back to the OS. CML4211 if any part is
    // not owned-and-unused.
    Result<void> release_range(std::uint64_t base, std::uint64_t size);

    // --- patches ---------------------------------------------------------------------------
    // Runs the whole pipeline of the module comment. Never throws; the answer carries status 0 or
    // the CML42xx id, and for failures the diagnostic text.
    ipc::PatchLoaded load_patch(const ipc::LoadPatch& request);

    // Staged variants for patches loaded earlier (ApplyFixups / RunInitializers). On failure the
    // patch is unloaded, as for LoadPatch. CML4210 when no patch is loaded at `image_base`.
    Result<std::uint32_t> apply_fixups(std::uint64_t image_base, std::span<const ipc::Fixup> fixups);
    Result<std::uint32_t> run_initializers(std::uint64_t image_base, std::span<const std::uint32_t> rvas);

    // FreeLibrary the patch at `image_base` and return its range to the reserved state.
    Result<void> unload_patch(std::uint64_t image_base);

    // Handles one broker request (ReserveRange, ReleaseRange, LoadPatch, ApplyFixups,
    // RunInitializers) and returns the answer message. Other message types: Reply with CML5203.
    ipc::Message handle_request(const ipc::Message& request);
    static bool is_loader_request(const ipc::Message& message) noexcept;

    // --- introspection (tests, logs) ---------------------------------------------------------
    std::size_t patch_count() const;
    std::vector<proc::AddressRange> owned_ranges() const;
    bool is_loaded(std::uint64_t image_base) const;

    void set_test_hooks(const TestHooks& hooks);

private:
    struct Patch {
        proc::AddressRange range;      // image range rounded up to 64 KiB
        std::uint32_t image_size = 0;
        HMODULE module = nullptr;
        std::uint64_t patch_id = 0;
        bool busy = false;
    };

    // All of these require mutex_ to be held.
    bool owned_covers(const proc::AddressRange& range) const;
    void owned_add(const proc::AddressRange& range);
    void owned_remove(const proc::AddressRange& range);
    Result<void> detach_range(const proc::AddressRange& range);
    void restore_reservation(const proc::AddressRange& range);
    Result<void> try_load_candidate(const ipc::PatchCandidate& candidate, std::uint32_t attempt,
                                    const std::wstring& wide_path, Patch* out);
    Patch* find_patch(std::uint64_t image_base);
    Result<void> unload_locked(std::size_t index);
    Result<void> lock_patch(std::uint64_t image_base, std::uint64_t* image_size_out);
    void unlock_patch(std::uint64_t image_base);
    void unload_after_failure(std::uint64_t image_base);

    mutable std::mutex mutex_;
    std::vector<proc::AddressRange> owned_;  // sorted by base, merged, never overlapping
    std::vector<Patch> patches_;
    TestHooks hooks_;
};

}  // namespace cml::agent::loader

#endif  // _WIN32
