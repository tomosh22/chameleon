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
#include "cml/core/source_baseline.h"
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
    // Registers `name`; returns what it resolved to before (nullopt: it was not registered). WP-22: restore() is the undo.
    std::optional<SymbolAddress> replace(std::string name, SymbolAddress address);
    void restore(const std::string& name, const std::optional<SymbolAddress>& previous);
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
    // Undoes consume(base, size) when it was the newest one of its block (nothing was placed after it): the patch was never
    // loaded, so its address space is free again. Anything else is left alone.
    void release_tail(std::uint64_t base, std::uint64_t size);
    const std::vector<ipc::AddressBlock>& blocks() const noexcept { return blocks_; }
    std::uint64_t remaining() const;

private:
    bool reserved_ = false;
    std::vector<ipc::AddressBlock> blocks_;
    std::vector<std::uint64_t> next_;  // per block: first free address
};

// A function hooked by a patch (WP-22: kept so a revert can put the previous patch's jump back).
struct AppliedHook {
    std::uint64_t old_entry = 0;
    std::uint64_t new_entry = 0;
    std::string name;
};

// A registry entry a patch added, and what the name resolved to before.
struct RegistryUndo {
    std::string name;
    std::optional<SymbolAddress> previous;
};

// One patch that is live in the process (WP-22). Its DLL stays loaded for the rest of the session.
struct AppliedPatch {
    std::uint64_t number = 0;  // the patch counter value that names its files
    std::uint64_t base = 0;
    std::string dll_path;
    std::vector<AppliedHook> hooks;
    std::vector<RegistryUndo> registry;  // in the order the entries were added
};

// The patches one reload applied to the module: the unit a `revert` with scope `last` undoes (a reload that
// recompiles three translation units makes three patches; undoing one of them would leave a mixed state).
struct PatchBatch {
    std::vector<AppliedPatch> patches;
    std::uint64_t generation_before = 0;                 // the module's generation before the batch
    std::uint64_t reload_id = 0;                         // the reload command that made it (ReloadService numbers them 1, 2, ...)
    std::map<std::string, FileStamp> baseline_before;    // the source baseline the batch replaced
};

// WP-50 (R-47): the copy of a module file the reloads compare with, and its read. Owned by the ModuleState and shared with the
// background reader (ReloadService), which holds this slot, not the state, while it reads: a disable drops the state (joining its
// index, releasing its PDB) at once, on the disabling thread, and a read still in flight then stores into a released slot.
struct ModuleImageRead {
    std::shared_ptr<const pe::OwnedImage> image;  // null when the file could not be read, or (background) after release()
    std::optional<Diag> error;                    // why it could not be read
    bool read_now = false;                        // this call read the file
    double ms = 0;                                // how long the read took
    double mib = 0;
};
class ModuleImageSlot {
public:
    // The copy, read once and kept. `reading` makes a second caller wait for a read in flight instead of reading the file again
    // (two copies of a large executable would cross the broker's memory budget). A background read neither reads nor stores
    // after release(); any other read always reads, and stores unless released.
    ModuleImageRead ensure(const std::string& path, bool background);
    // Drops the copy; later background reads store nothing (a WP-45 re-enable is a new state with a new slot).
    void release();
    std::shared_ptr<const pe::OwnedImage> stored() const;
    bool released() const;
    std::uint64_t reads() const;  // times ensure() read the file (evidence, tests)
    void forget_for_testing();    // drops the copy without releasing (the next read reads again)

private:
    std::mutex reading_;        // one reader of the file at a time; taken before mutex_
    mutable std::mutex mutex_;  // guards the members below; never held while reading
    std::shared_ptr<const pe::OwnedImage> image_;
    bool released_ = false;
    std::uint64_t reads_ = 0;
};

struct ModuleState {
    std::string path;  // full path of the module file
    std::string name;  // file name ("fixture_basic.exe")
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    // WP-45: which instance of this module (path) in its process this state is, from 1: a module freed and loaded again, or
    // disabled and enabled again, is a new instance. Names the patch work directory, so that no patch of a new instance is written
    // to the path of an earlier instance's patch DLL, which stays loaded (F-42.2). Set before the state is shared.
    std::uint64_t instance = 1;
    std::shared_ptr<ModuleIndex> index;
    SharedRegistry registry;

    // Guards everything below.
    std::mutex mutex;
    std::map<std::uint64_t, std::uint64_t> hooked;  // old entry VA -> VA it currently jumps to
    Reservation reservation;
    std::optional<toolchain::ToolInfo> linker;      // resolved once per option set, mirrors the module's linker
    std::string linker_key;                         // the `linker` / `linker_path` options `linker` was resolved with
    std::uint64_t generation = 0;                   // patches applied to this module
    std::uint64_t patch_counter = 0;                // names patch files (also counts failed attempts; never goes back)
    std::vector<PatchBatch> batches;                // live patches, oldest first (WP-22)
    // WP-45: the baseline of an earlier instance this one was given to re-apply (ReloadService). Kept so that this instance, should
    // it go away before the re-apply ran, hands the same edits on to the next one.
    std::optional<BaselineSnapshot> carried_in;

    bool is_hooked(std::uint64_t old_va);

    // WP-50 (R-47): the module file on disk (old code bytes, narrowing), read once per state and kept (ModuleImageSlot). The read
    // happens in the background once the index is Ready (ReloadService, `background` true), or in the plan stage of the first
    // reload if that came first; a reload keeps what it got for all of its units (Run::module_image in pipeline.cpp). Not under
    // `mutex`: the slot has its own locks.
    using ImageRead = ModuleImageRead;
    const std::shared_ptr<ModuleImageSlot> image_slot = std::make_shared<ModuleImageSlot>();
    ImageRead ensure_image(bool background) { return image_slot->ensure(path, background); }
    void release_image() { image_slot->release(); }

    // WP-55 (R-62): the bind planner's verdicts on the inline sites of this module image's procedures (WP-51), kept for the
    // reloads of this state and dropped with it. Not under `mutex`: the cache has its own lock.
    bind::InlineVerdictCache inline_verdicts;

    // WP-25: what the patches' functions look like, keyed by the address of the patch code (the new entry of a hook, or the
    // address of a function a patch introduced). `code_running_at(entry)` is the record of the code that runs when `entry`
    // is called: the hook target when the entry is hooked, else the entry itself. Records are never removed: the patch DLLs
    // stay loaded, and a revert that re-points a hook only changes which record applies.
    std::map<std::uint64_t, std::shared_ptr<const CodeImage>> code;
    void record_code(std::uint64_t patch_entry, std::shared_ptr<const CodeImage> record);  // takes `mutex`
    std::shared_ptr<const CodeImage> code_running_at(std::uint64_t entry);               // takes `mutex`
    // Drops the records nothing reaches: those that are no value of `hooked` and no function a patch introduced that is still
    // canonical in the registry and not hooked since. Call after `hooked` / `registry` changed (a new patch superseded a hook, a
    // revert undid one, the module went away). `prune_code_locked` needs `mutex` held.
    void prune_code();
    void prune_code_locked();
};

}  // namespace cml::core
