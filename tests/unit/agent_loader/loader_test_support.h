#pragma once

// Helpers shared by the unit.agent_loader tests: the prototype patch DLL, per-base copies of it,
// the "host" module the patches are placed near (this test exe), and memory inspection.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cml/ipc/messages.h"
#include "loader/patch_loader.h"

namespace loadertest {

// Where things are in the prototype DLL (all RVAs), read from its export table and init table.
struct ProtoLayout {
    std::uint64_t proto_base = 0;  // the /BASE it was linked at
    std::uint32_t image_size = 0;  // SizeOfImage
    std::uint32_t rva_code_call = 0;  // REL32 site is at +5
    std::uint32_t rva_data = 0;       // 256 bytes
    std::uint32_t rva_rodata = 0;     // 64 bytes, read-only page
    std::uint32_t rva_slot_a = 0;
    std::uint32_t rva_slot_b = 0;
    std::uint32_t rva_inits = 0;       // 3 pointers
    std::uint32_t rva_init_fault = 0;  // 1 pointer
    std::uint32_t rva_init_overflow = 0;  // 1 pointer
    std::uint32_t rva_log = 0;
    std::uint32_t rva_log_len = 0;
    std::uint32_t rva_answer = 0;
    std::uint32_t init_fn_rva[3] = {};
    std::uint32_t fault_fn_rva = 0;
    std::uint32_t overflow_fn_rva = 0;
};

const ProtoLayout& proto();
std::string proto_path();

// Per-process scratch directory (created on first use); remove_work_dir() deletes what it can.
std::string work_dir();
void remove_work_dir();

// Writes a copy of the prototype with ImageBase = `base` as <work_dir>/<tag>.dll and returns the
// path. `base` must be 64 KiB aligned.
std::string make_variant(std::uint64_t base, const std::string& tag);

// The module the patches are placed near: this executable.
std::uint64_t host_base();
std::uint64_t host_size();
// A function in the host that patch code can call through a REL32.
std::uint64_t host_target_address();
constexpr int kHostTargetResult = 0x5AFE;

std::uint64_t stride();  // image size rounded up to 64 KiB: the bump step between patches

// Fix-ups that touch every kind and every kind of page (code, read-only, writable): REL32 n=0 at
// the code stub, REL32 n=1..5 and ADDR64 in cml_data, ADDR64 in cml_rodata and in the read-only
// initializer tables (they must be re-pointed after the rebase), two thunk slots.
std::vector<cml::ipc::Fixup> standard_fixups(std::uint64_t base, std::uint64_t target);
std::vector<std::uint32_t> init_entry_rvas();  // cml_inits[0..2]
std::vector<std::uint32_t> fault_entry_rvas();  // cml_init_fault[0]
std::vector<std::uint32_t> overflow_entry_rvas();  // cml_init_overflow[0]

// One-candidate LoadPatch for `base` using a fresh variant file; fix-ups are standard_fixups unless
// `with_fixups` is false; initializers are the three in order unless `with_inits` is false.
cml::ipc::LoadPatch make_request(std::uint64_t base, const std::string& tag, bool with_fixups = true,
                                 bool with_inits = true);

// Verifies what standard_fixups should have produced in the image at `base` (reads the image
// memory, and calls the code stub). Returns an empty string when everything is right, else what
// is wrong.
std::string verify_standard(std::uint64_t base, std::uint64_t target);

MEMORY_BASIC_INFORMATION query(std::uint64_t address);
bool is_module_loaded_at(std::uint64_t base);  // MEM_IMAGE at `base`
std::string initializer_log(std::uint64_t base);

}  // namespace loadertest

// RAII owner of a loader plus a few reserved blocks; unloads what the test loaded.
struct LoaderSession {
    cml::agent::loader::PatchLoader loader;
    std::vector<cml::proc::AddressRange> blocks;
    std::vector<std::uint64_t> loaded;

    // Reserves `count` blocks of `block_size` near the host; REQUIREs that all of them were found.
    explicit LoaderSession(unsigned count, std::uint64_t block_size = 1ull << 20);
    ~LoaderSession();

    // Loads and remembers the base for cleanup. Returns the answer.
    cml::ipc::PatchLoaded load(const cml::ipc::LoadPatch& request);
};
