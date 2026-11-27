#pragma once

// Module-list change detection for the agent.
//
// Before WP-48 the agent only polled the process module list on its own thread every 500 ms and turned the difference between two
// snapshots into ModuleLoaded / ModuleUnloaded events: a FreeLibrary + LoadLibrary at the same base between two polls looked like
// no change (R-53). Since WP-48 (ADR-0044) the loader's own notifications (LdrRegisterDllNotification, recorded in a lock-free
// ring, module_ring.h) are the source, in order, and the snapshot is the fallback and the check:
//   * track_modules applies the drained notifications to what the broker was told and returns the events to report;
//   * a ring that overflowed, or a path that could not be resolved, completes the report from the snapshot diff (resync);
//   * a disagreement between the events and the snapshot that persists over two quiet polls is corrected from the snapshot.
// Without notifications (registration failed) the agent keeps the plain snapshot diff (diff_modules).
//
// Portable so it can be unit-tested anywhere.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cml/ipc/messages.h"
#include "module_ring.h"

namespace cml::agent {

struct ModuleDelta {
    std::vector<ipc::ModuleEntry> loaded;    // in `after` only (or at the same base with another image)
    std::vector<ipc::ModuleEntry> unloaded;  // in `before` only
    bool empty() const noexcept { return loaded.empty() && unloaded.empty(); }
};

// Two entries are the same module when base, size and path are all equal; a different image
// mapped at the same base counts as unload + load. Order of the results follows the inputs.
ModuleDelta diff_modules(const std::vector<ipc::ModuleEntry>& before,
                         const std::vector<ipc::ModuleEntry>& after);

// --- WP-48: loader notifications --------------------------------------------------------------------------------------------

// UTF-16 to UTF-8; an unpaired surrogate becomes U+FFFD.
std::string utf8_from_utf16(const char16_t* text, std::size_t units);

// One notification taken from the ring, with its path in UTF-8.
struct DrainedEvent {
    ipc::ModuleEventKind kind = ipc::ModuleEventKind::Loaded;
    std::uint64_t seq = 0;
    ipc::ModuleEntry module;      // when path_truncated, the path is only the start of the real one
    bool path_truncated = false;
    bool patch_image = false;     // the patch loader's own image: never reported
};
DrainedEvent to_drained(const RawModuleEvent& raw);

struct ModuleTrackState {
    std::vector<ipc::ModuleEntry> known;  // what the broker was told: the last full ModuleList and every event since
    std::optional<ModuleDelta> suspect;   // a disagreement with the module list seen in one quiet poll (CML4722 when seen again)
    // Bases whose unload notification was seen (reported or redundant) and at which no load has been reported since. A load at such
    // a base is never redundant, even when `known` holds exactly that entry: `known` can have it back from a module list that still
    // showed the image after its unload notification (a resync, or a list sent again), and the load is then a new image. Kept across
    // resyncs and lists sent again; a new session starts empty.
    std::vector<std::uint64_t> unloads_seen;
    // Events kept for the next poll, in order: a load whose path was cut (longer than a slot) when no module list could be taken
    // to complete it, and every event after it.
    std::vector<DrainedEvent> deferred;
};

// The first module list of a session: `before` are the notifications drained right before `list` was taken (oldest first). The
// list can lag behind them (still show an image whose unload notification was delivered), so they are applied to it: an unload
// removes its base; a load puts back the list's entry at that base (same base and size) if the list has one. Nothing the list
// does not have is added. Patch images are ignored.
std::vector<ipc::ModuleEntry> apply_before_list(const std::vector<DrainedEvent>& before, const std::vector<ipc::ModuleEntry>& list);

struct ModulePollReport {
    std::vector<ipc::ModuleEvent> events;  // to report, in order
    // 0: every event is the loader's. Otherwise the agent diagnostic (module_event_ids.h) why some of them come from the snapshot
    // diff: kRingOverflow, kPathUnresolved or kListDisagrees.
    std::uint32_t resync_reason = 0;
    std::uint64_t lost = 0;           // notifications the ring dropped since the previous poll
    std::size_t patch_events = 0;     // dropped: the patch loader's own images
    std::size_t redundant = 0;        // dropped: a load of a module the broker knows as it is, an unload of one it does not know
    bool need_full_list = false;      // a resync was needed and no snapshot was available: send a full ModuleList
    bool deferred = false;            // events were kept for the next poll (state.deferred): this poll does not cover everything
};

// One poll. `drained` are the notifications taken from the ring, oldest first (state.deferred goes before them); `lost` how many
// the ring dropped since the previous poll; `snapshot` the module list taken right after the drain (null when it could not be
// taken); `quiet` that no notification arrived while it was taken; `known_trusted` that the broker has a full module list (false
// while the session's list could not be built or was refused and is to be sent again). Applies the events to state.known, in order:
//   * an unload of a known base is reported (with the entry the broker knows); of an unknown base it is redundant;
//   * a load is reported; a load of exactly the known entry is redundant (the full list had it) unless an unload at that base was
//     seen since (state.unloads_seen: then the known entry's unload is reported first, seq 0); a load at a base that holds another
//     module reports that module's unload first (seq 0);
//   * a truncated path is completed from the snapshot (same base and size); one that cannot be is a resync (kPathUnresolved);
//     without a snapshot that load and everything after it wait for the next poll (state.deferred).
// A resync (lost > 0 or an unresolved path) appends the snapshot diff of what is left (unloads, then loads, seq 0) and makes the
// snapshot the known list. So does an untrusted known list, at once and without a diagnostic (the broker must learn the modules
// before a request that names one). Otherwise, with no events and a quiet snapshot, a difference between the known list and the
// snapshot is only remembered; the same difference in the next such poll is reported as a resync (kListDisagrees). So transient
// differences (a notification delivered before the module list shows the change, or after it) never reach the broker.
ModulePollReport track_modules(ModuleTrackState& state, const std::vector<DrainedEvent>& drained, std::uint64_t lost,
                               const std::vector<ipc::ModuleEntry>* snapshot, bool quiet, bool known_trusted = true);

// The ModuleEvents a report goes out as: at most `max_events` events and about `max_bytes` of paths per message, in order; one
// message (possibly empty) for a report without events. The resync flag, its reason and the lost count are on the first message
// only, so a report split over several messages is still one resync for the broker (one CML5620).
std::vector<ipc::ModuleEvents> to_messages(const ModulePollReport& report, std::size_t max_events, std::size_t max_bytes);

}  // namespace cml::agent
