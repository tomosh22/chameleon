#pragma once

// Module-list change detection for the agent.
//
// Before WP-48 the agent only polled the process module list on its own thread every 500 ms and turned the difference between two
// snapshots into ModuleLoaded / ModuleUnloaded events: a FreeLibrary + LoadLibrary at the same base between two polls looked like
// no change (R-53). Since WP-48 (ADR-0044) the loader's own notifications (LdrRegisterDllNotification, recorded in a lock-free
// ring, module_ring.h) are the source, in order, and the snapshot is the fallback and the check:
//   * track_modules applies the drained notifications to what the broker was told and returns the events to report;
//   * a ring that overflowed, or a path that could not be resolved, completes the report from the snapshot diff (resync);
//   * a disagreement between the events and the snapshot that persists over two quiet polls is corrected from the snapshot;
//   * the hook bytes the agent wrote decide whether the image at a base is the live, hooked one (ImageHooks, below).
// Without notifications (registration failed) the agent keeps the plain snapshot diff (diff_modules).
//
// Portable so it can be unit-tested anywhere.

#include <cstddef>
#include <cstdint>
#include <functional>
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

// --- the arbiter (ADR-0044 ruling, review round 3): the hook bytes --------------------------------------------------------------
//
// Whether the image the broker knows at a base is the live, hooked one is decided by the hook installer's records, not by timing.
// In the agent the probe is Installer::reconcile_range: in ONE locked section it reads, per record in the module's range, the bytes
// the agent wrote, and forgets exactly the records whose image was replaced (their bytes read the original ones, or the range is no
// longer committed); a record whose bytes are foreign (a breakpoint, another tool's detour) is kept (CML4724). Its verdict:
//   None:    no hook record there: no applied edit can be lost; the list and notification heuristics decide, as before;
//   Present: records are left and none proved a replacement: the image is the live, hooked one. An event that would unload it (an
//            unload, a load at its base, a module list that does not show it or shows something else there) is stale: never
//            reported, and the image's hook records are never forgotten;
//   Absent:  some record proved a replacement (and was forgotten): the image was replaced (or is gone). An unload is reported; a
//            load at that base, even with the same path, size and base, reports the known entry's unload first.
// The probe may forget records as a side effect: only records that prove a replacement, which are dead in any case.
enum class ImageHooks : std::uint8_t { None, Present, Absent };
using HookProbe = std::function<ImageHooks(const ipc::ModuleEntry& module)>;

// How long (ms after the poll that drained it) an unload whose image still carries its hooks is checked again (the loader can send
// the notification a moment before it unmaps the image) before it is dropped as stale. Time, not a poll count: a busy ring can make
// several polls in a few milliseconds (review round 4).
inline constexpr std::uint64_t kHookedUnloadRecheckMs = 2000;

struct ModuleTrackState {
    std::vector<ipc::ModuleEntry> known;  // what the broker was told: the last full ModuleList and every event since
    std::optional<ModuleDelta> suspect;   // a disagreement with the module list seen in one quiet poll (CML4722 when seen again)
    // Bases whose unload notification was seen (reported, or of a module the broker did not know) and at which no load has been
    // reported since. A hint for modules without hook records (ImageHooks::None): a load at such a base is never redundant, even when
    // `known` holds exactly that entry (a module list can lag behind an unload notification and bring the entry back). For a module
    // with hook records the probe decides instead. A new session starts empty.
    std::vector<std::uint64_t> unloads_seen;
    // Unloads whose image still carried its hooks when they were drained: checked again at every poll until `deadline_ms`, reported as
    // soon as the hooks are gone, dropped as stale when they are still there at the deadline. Later events at that base wait for it in
    // `deferred`.
    struct Recheck {
        DrainedEvent event;
        std::uint64_t deadline_ms = 0;
    };
    std::vector<Recheck> rechecks;
    // Events kept for the next poll, in order: a load whose path was cut (longer than a slot) when no module list could be taken to
    // complete it, and later events at a base that has such a load or a pending recheck (the order per base is kept; every other
    // event is processed).
    std::vector<DrainedEvent> deferred;
};

// The first module list of a session: `before` are the notifications drained right before `list` was taken (oldest first). The
// list can lag behind them (still show an image whose unload notification was delivered), so they are applied to it: an unload
// removes its base, unless the probe finds the hooked image there (Present: it is live); a load puts back the list's entry at that
// base (same base and size) if the list has one. Nothing the list does not have is added. Patch images are ignored.
std::vector<ipc::ModuleEntry> apply_before_list(const std::vector<DrainedEvent>& before, const std::vector<ipc::ModuleEntry>& list,
                                                const HookProbe& probe = {});

// A module list for a broker that believes `known`: `snapshot`, except that a known entry whose image is the live, hooked one
// (Present) stays where the snapshot does not show it or shows something else at its base (the snapshot lags).
std::vector<ipc::ModuleEntry> list_with_hooked(const std::vector<ipc::ModuleEntry>& known, const std::vector<ipc::ModuleEntry>& snapshot,
                                               const HookProbe& probe = {});

// A full module list the broker was sent becomes the known list.
void adopt_full_list(ModuleTrackState& state, std::vector<ipc::ModuleEntry> list);

struct ModulePollReport {
    std::vector<ipc::ModuleEvent> events;  // to report, in order
    // 0: every event is the loader's. Otherwise the agent diagnostic (module_event_ids.h) why some of them come from the snapshot
    // diff: kRingOverflow, kPathUnresolved or kListDisagrees.
    std::uint32_t resync_reason = 0;
    std::uint64_t lost = 0;           // notifications the ring dropped since the previous poll
    std::size_t patch_events = 0;     // dropped: the patch loader's own images
    std::size_t redundant = 0;        // dropped: a load of a module the broker knows as it is, an unload of one it does not know
    std::size_t hooked_live = 0;      // not reported: the probe found the known image's hooks in place (stale events and list rows)
    bool need_full_list = false;      // a resync was needed and no snapshot was available: send a full ModuleList
    bool deferred = false;            // events were kept for the next poll (state.deferred)
};

// One poll. `drained` are the notifications taken from the ring, oldest first (pending rechecks, then state.deferred, go before
// them); `lost` how many the ring dropped since the previous poll; `snapshot` the module list taken right after the drain (null
// when it could not be taken); `quiet` that no notification arrived while it was taken; `known_trusted` that the broker has a full
// module list (false while the session's list could not be built or was refused and is to be sent again); `probe` the arbiter
// (empty: ImageHooks::None everywhere). Applies the events to state.known, in order, per base:
//   * an unload of a known base: Present -> rechecked at the next polls for kHookedUnloadRecheckMs (stale if the hooks stay);
//     otherwise reported (with the entry the broker knows). An unload of an unknown base is redundant;
//   * a load at a known base: Present -> redundant (the hooked image is live); Absent -> the known entry's unload (seq 0), then the
//     load; None -> redundant if it is exactly the known entry and no unload at that base was seen since, else unload (seq 0) and
//     load. A load at an unknown base is reported;
//   * a truncated path is completed from the snapshot (same base and size); one that cannot be is a resync (kPathUnresolved);
//     without a snapshot that load and the later events at its base wait for the next poll (state.deferred); the others go on.
// A resync (lost > 0 or an unresolved path) appends the snapshot diff of what is left (unloads, then loads, seq 0; a hooked, live
// known entry stays and is not unloaded) and makes the snapshot the known list. So does an untrusted known list, at once and without
// a diagnostic. Otherwise, with nothing pending and a quiet snapshot, a difference between the known list and the snapshot is only
// remembered; the same difference in the next such poll is reported as a resync (kListDisagrees).
// `now_ms` is the caller's clock (GetTickCount64 in the agent), for the recheck deadline.
ModulePollReport track_modules(ModuleTrackState& state, const std::vector<DrainedEvent>& drained, std::uint64_t lost,
                               const std::vector<ipc::ModuleEntry>* snapshot, bool quiet, bool known_trusted = true,
                               const HookProbe& probe = {}, std::uint64_t now_ms = 0);

// The ModuleEvents a report goes out as: at most `max_events` events and about `max_bytes` of paths per message, in order; one
// message (possibly empty) for a report without events. The resync flag, its reason and the lost count are on the first message
// only, so a report split over several messages is still one resync for the broker (one CML5620).
std::vector<ipc::ModuleEvents> to_messages(const ModulePollReport& report, std::size_t max_events, std::size_t max_bytes);

}  // namespace cml::agent
