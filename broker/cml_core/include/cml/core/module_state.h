#pragma once

// Everything the broker keeps per enabled module of a process (WP-12): the PDB index, the symbols earlier
// patches made canonical (plan step 9), the hooks currently installed, the address-space reservation, and
// what the pipeline caches between reloads (the linker, the module's file).
//
// Portable; each member documents its own locking.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cml/core/hook_narrowing.h"
#include "cml/core/module_index.h"
#include "cml/core/symbol_registry.h"
#include "cml/ipc/messages.h"
#include "cml/pe/pe.h"
#include "cml/toolchain/tool_info.h"

namespace cml::core {

// Thread-safe SymbolRegistry: new functions and globals of applied patches. Patch N+1 of a translation unit
// resolves its references to what patch N introduced through this (plan 4.5, resolution step 1).
class SharedRegistry final : public SymbolRegistry {
public:
    std::optional<SymbolAddress> find(std::string_view decorated_name) const override;
    void add(std::string name, SymbolAddress address);
    std::size_t size() const;

private:
    mutable std::mutex mutex_;
    InMemorySymbolRegistry registry_;
};

// Address space the agent reserved near the module (ReserveRange). Patches are placed one after another.
class Reservation {
public:
    bool reserved() const noexcept { return reserved_; }
    void set_blocks(std::vector<ipc::AddressBlock> blocks);
    // The 64 KiB aligned base for the next patch of at most `max_size` bytes, or 0 when no block has room.
    std::uint64_t next_base(std::uint64_t max_size) const;
    // Marks [base, base + size) used (plus a 64 KiB gap).
    void consume(std::uint64_t base, std::uint64_t size);
    const std::vector<ipc::AddressBlock>& blocks() const noexcept { return blocks_; }
    std::uint64_t remaining() const;

private:
    bool reserved_ = false;
    std::vector<ipc::AddressBlock> blocks_;
    std::vector<std::uint64_t> next_;  // per block: first free address
};

struct ModuleState {
    std::string path;  // full path of the module file
    std::string name;  // file name ("fixture_basic.exe")
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::shared_ptr<ModuleIndex> index;
    SharedRegistry registry;

    // Guards everything below.
    std::mutex mutex;
    std::map<std::uint64_t, std::uint64_t> hooked;  // old entry VA -> VA it currently jumps to
    Reservation reservation;
    std::optional<toolchain::ToolInfo> linker;      // resolved once per option set, mirrors the module's linker
    std::string linker_key;                         // the `linker` / `linker_path` options `linker` was resolved with
    std::shared_ptr<const pe::OwnedImage> image;    // the module file on disk (old code bytes, narrowing)
    std::uint64_t generation = 0;                   // patches applied to this module
    std::uint64_t patch_counter = 0;                // names patch files (also counts failed attempts)

    bool is_hooked(std::uint64_t old_va);

    // WP-25: what the patches' functions look like, keyed by the address of the patch code (the new entry of a hook, or the
    // address of a function a patch introduced). `code_running_at(entry)` is the record of the code that runs when `entry`
    // is called: the hook target when the entry is hooked, else the entry itself. Records are never removed: the patch DLLs
    // stay loaded, and a revert that re-points a hook only changes which record applies.
    std::map<std::uint64_t, std::shared_ptr<const CodeImage>> code;
    void record_code(std::uint64_t patch_entry, std::shared_ptr<const CodeImage> image);  // takes `mutex`
    std::shared_ptr<const CodeImage> code_running_at(std::uint64_t entry);               // takes `mutex`
};

}  // namespace cml::core
