#include "module_diff.h"

#include <algorithm>
#include <map>
#include <utility>

#include "module_event_ids.h"

namespace cml::agent {

namespace {

using ByBase = std::map<std::uint64_t, const ipc::ModuleEntry*>;

ByBase index_by_base(const std::vector<ipc::ModuleEntry>& modules) {
    ByBase index;
    for (const ipc::ModuleEntry& module : modules) {
        index[module.base] = &module;
    }
    return index;
}

bool same_module(const ipc::ModuleEntry& a, const ipc::ModuleEntry& b) {
    return a.base == b.base && a.size == b.size && a.path == b.path;
}

bool same_entries(const std::vector<ipc::ModuleEntry>& a, const std::vector<ipc::ModuleEntry>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), same_module);
}

bool same_delta(const ModuleDelta& a, const ModuleDelta& b) {
    return same_entries(a.loaded, b.loaded) && same_entries(a.unloaded, b.unloaded);
}

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

ipc::ModuleEvent make_event(ipc::ModuleEventKind kind, std::uint64_t seq, const ipc::ModuleEntry& module) {
    ipc::ModuleEvent event;
    event.kind = kind;
    event.seq = seq;
    event.module = module;
    return event;
}

ImageHooks probe_of(const HookProbe& probe, const ipc::ModuleEntry& module) {
    return probe ? probe(module) : ImageHooks::None;
}

// The entries of `known` whose image is the live, hooked one (Present) at a base where `snapshot` has nothing or something else:
// the snapshot lags behind them, they stay.
std::vector<ipc::ModuleEntry> hooked_missing(const std::vector<ipc::ModuleEntry>& known, const std::vector<ipc::ModuleEntry>& snapshot,
                                             const HookProbe& probe) {
    std::vector<ipc::ModuleEntry> kept;
    if (!probe) {
        return kept;
    }
    for (const ipc::ModuleEntry& entry : diff_modules(known, snapshot).unloaded) {
        if (probe(entry) == ImageHooks::Present) {
            kept.push_back(entry);
        }
    }
    return kept;
}

bool base_in(const std::vector<ipc::ModuleEntry>& entries, std::uint64_t base) {
    return std::any_of(entries.begin(), entries.end(), [base](const ipc::ModuleEntry& m) { return m.base == base; });
}

// `snapshot`, with the `kept` entries in place of whatever the snapshot has at their bases.
std::vector<ipc::ModuleEntry> merge_kept(const std::vector<ipc::ModuleEntry>& snapshot, const std::vector<ipc::ModuleEntry>& kept) {
    std::vector<ipc::ModuleEntry> out;
    out.reserve(snapshot.size() + kept.size());
    for (const ipc::ModuleEntry& m : snapshot) {
        if (!base_in(kept, m.base)) {
            out.push_back(m);
        }
    }
    out.insert(out.end(), kept.begin(), kept.end());
    return out;
}

// Reports the difference between the known list and `snapshot` (unloads first, then loads, both seq 0) and makes the snapshot the
// known list, except for known entries whose image is the live, hooked one: they are neither unloaded nor replaced.
void append_snapshot_diff(ModulePollReport& report, ModuleTrackState& state, const std::vector<ipc::ModuleEntry>& snapshot,
                          const HookProbe& probe) {
    const std::vector<ipc::ModuleEntry> kept = hooked_missing(state.known, snapshot, probe);
    const ModuleDelta delta = diff_modules(state.known, snapshot);
    for (const ipc::ModuleEntry& module : delta.unloaded) {
        if (base_in(kept, module.base)) {
            ++report.hooked_live;
            continue;
        }
        report.events.push_back(make_event(ipc::ModuleEventKind::Unloaded, 0, module));
    }
    for (const ipc::ModuleEntry& module : delta.loaded) {
        if (!base_in(kept, module.base)) {
            report.events.push_back(make_event(ipc::ModuleEventKind::Loaded, 0, module));
        }
    }
    state.known = merge_kept(snapshot, kept);
}

}  // namespace

ModuleDelta diff_modules(const std::vector<ipc::ModuleEntry>& before,
                         const std::vector<ipc::ModuleEntry>& after) {
    const ByBase old_index = index_by_base(before);
    const ByBase new_index = index_by_base(after);
    ModuleDelta delta;
    for (const ipc::ModuleEntry& module : before) {
        const auto it = new_index.find(module.base);
        if (it == new_index.end() || !same_module(module, *it->second)) {
            delta.unloaded.push_back(module);
        }
    }
    for (const ipc::ModuleEntry& module : after) {
        const auto it = old_index.find(module.base);
        if (it == old_index.end() || !same_module(module, *it->second)) {
            delta.loaded.push_back(module);
        }
    }
    return delta;
}

std::string utf8_from_utf16(const char16_t* text, std::size_t units) {
    std::string out;
    out.reserve(units);
    for (std::size_t i = 0; i < units; ++i) {
        const std::uint32_t unit = text[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < units && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            append_utf8(out, 0x10000 + ((unit - 0xD800) << 10) + (static_cast<std::uint32_t>(text[i + 1]) - 0xDC00));
            ++i;
        } else if (unit >= 0xD800 && unit <= 0xDFFF) {
            append_utf8(out, 0xFFFD);
        } else {
            append_utf8(out, unit);
        }
    }
    return out;
}

DrainedEvent to_drained(const RawModuleEvent& raw) {
    DrainedEvent event;
    event.kind = raw.kind == kRawEventUnloaded ? ipc::ModuleEventKind::Unloaded : ipc::ModuleEventKind::Loaded;
    event.seq = raw.seq;
    event.module.base = raw.base;
    event.module.size = raw.size;
    const std::size_t units = std::min<std::size_t>(raw.path_units, kRawPathUnits);
    event.module.path = utf8_from_utf16(raw.path, units);
    event.path_truncated = (raw.flags & kRawPathTruncated) != 0;
    event.patch_image = (raw.flags & kRawPatchImage) != 0;
    return event;
}

std::vector<ipc::ModuleEntry> apply_before_list(const std::vector<DrainedEvent>& before, const std::vector<ipc::ModuleEntry>& list,
                                                const HookProbe& probe) {
    std::vector<ipc::ModuleEntry> out = list;
    for (const DrainedEvent& event : before) {
        if (event.patch_image) {
            continue;
        }
        const std::uint64_t base = event.module.base;
        const auto here = [base](const ipc::ModuleEntry& m) { return m.base == base; };
        if (event.kind == ipc::ModuleEventKind::Unloaded) {
            const auto at = std::find_if(out.begin(), out.end(), here);
            if (at != out.end() && probe_of(probe, *at) == ImageHooks::Present) {
                continue;  // the image at that base is the live, hooked one: this unload is of an earlier image
            }
            out.erase(std::remove_if(out.begin(), out.end(), here), out.end());
            continue;
        }
        const auto in_list = std::find_if(list.begin(), list.end(),
                                          [&event](const ipc::ModuleEntry& m) { return m.base == event.module.base && m.size == event.module.size; });
        if (in_list != list.end() && std::none_of(out.begin(), out.end(), here)) {
            out.push_back(*in_list);  // unloaded and loaded again before the list was taken: the list's entry is the new image
        }
    }
    return out;
}

std::vector<ipc::ModuleEntry> list_with_hooked(const std::vector<ipc::ModuleEntry>& known, const std::vector<ipc::ModuleEntry>& snapshot,
                                               const HookProbe& probe) {
    return merge_kept(snapshot, hooked_missing(known, snapshot, probe));
}

void adopt_full_list(ModuleTrackState& state, std::vector<ipc::ModuleEntry> list) {
    state.known = std::move(list);
    state.suspect.reset();
    // unloads_seen stays: it is only a hint for modules without hooks, and a list can lag behind an unload (review round 3).
}

ModulePollReport track_modules(ModuleTrackState& state, const std::vector<DrainedEvent>& drained, std::uint64_t lost,
                               const std::vector<ipc::ModuleEntry>* snapshot, bool quiet, bool known_trusted, const HookProbe& probe) {
    ModulePollReport report;
    report.lost = lost;
    std::uint32_t resync = lost > 0 ? module_events::ids::kRingOverflow : 0u;
    std::vector<ipc::ModuleEntry>& known = state.known;
    const auto find_base = [&known](std::uint64_t base) {
        return std::find_if(known.begin(), known.end(), [base](const ipc::ModuleEntry& m) { return m.base == base; });
    };
    const auto note_unload = [&state](std::uint64_t base) {
        if (std::find(state.unloads_seen.begin(), state.unloads_seen.end(), base) == state.unloads_seen.end()) {
            state.unloads_seen.push_back(base);
        }
    };
    const auto take_unload = [&state](std::uint64_t base) {
        const auto it = std::find(state.unloads_seen.begin(), state.unloads_seen.end(), base);
        if (it == state.unloads_seen.end()) {
            return false;
        }
        state.unloads_seen.erase(it);
        return true;
    };
    std::vector<std::uint64_t> held_bases;  // bases whose events wait this poll: a pending recheck, or a deferred cut-path load
    const auto held = [&held_bases](std::uint64_t base) {
        return std::find(held_bases.begin(), held_bases.end(), base) != held_bases.end();
    };

    // 1. Unloads whose image still had its hooks: reported once the hooks are gone, dropped as stale when they stay.
    std::vector<ModuleTrackState::Recheck> rechecks = std::move(state.rechecks);
    state.rechecks.clear();
    for (ModuleTrackState::Recheck& recheck : rechecks) {
        const auto at = find_base(recheck.event.module.base);
        if (at == known.end()) {
            continue;  // something else reported that base gone meanwhile
        }
        if (probe_of(probe, *at) == ImageHooks::Present) {
            if (--recheck.polls_left > 0) {
                held_bases.push_back(recheck.event.module.base);
                state.rechecks.push_back(std::move(recheck));
            } else {
                ++report.hooked_live;  // the hooked image stayed: the unload was of an earlier image
            }
            continue;
        }
        note_unload(at->base);
        report.events.push_back(make_event(ipc::ModuleEventKind::Unloaded, recheck.event.seq, *at));
        known.erase(at);
    }

    // 2. The events: what the previous poll held back, then what was drained now.
    std::vector<DrainedEvent> events = std::move(state.deferred);
    state.deferred.clear();
    events.insert(events.end(), drained.begin(), drained.end());
    for (const DrainedEvent& event : events) {
        if (event.patch_image) {
            ++report.patch_events;
            continue;
        }
        const std::uint64_t base = event.module.base;
        if (held(base)) {
            state.deferred.push_back(event);  // after a recheck or a deferred load at this base: keeps the order per base
            continue;
        }
        const auto at = find_base(base);
        if (event.kind == ipc::ModuleEventKind::Unloaded) {
            if (at == known.end()) {
                note_unload(base);
                ++report.redundant;  // never reported (e.g. loaded and unloaded before the session's full list was taken)
                continue;
            }
            if (probe_of(probe, *at) == ImageHooks::Present) {
                // The image at that base still carries the agent's hooks: either the loader has not unmapped it yet, or this unload
                // is of an earlier image (a stale event). Never forgotten on a guess: checked again at the next polls.
                state.rechecks.push_back(ModuleTrackState::Recheck{event, kHookedUnloadRechecks});
                held_bases.push_back(base);
                continue;
            }
            note_unload(base);
            report.events.push_back(make_event(ipc::ModuleEventKind::Unloaded, event.seq, *at));
            known.erase(at);
            continue;
        }
        ipc::ModuleEntry entry = event.module;
        if (event.path_truncated) {
            if (snapshot == nullptr) {
                // No list to read the full path from: this load, and the later events at its base, wait for the next poll. Every other
                // event goes on (an unload elsewhere must reach forget_range and the broker now, WP-45).
                state.deferred.push_back(event);
                held_bases.push_back(base);
                report.deferred = true;
                continue;
            }
            // While the module is still loaded the snapshot has its full path.
            const auto full = std::find_if(snapshot->begin(), snapshot->end(),
                                           [&entry](const ipc::ModuleEntry& m) { return m.base == entry.base && m.size == entry.size; });
            if (full == snapshot->end()) {
                if (resync == 0) {
                    resync = module_events::ids::kPathUnresolved;
                }
                continue;
            }
            entry.path = full->path;
        }
        const bool after_unload = take_unload(base);
        if (at != known.end()) {
            const ImageHooks hooks = probe_of(probe, *at);
            if (hooks == ImageHooks::Present) {
                ++report.hooked_live;  // the known image is the live, hooked one: this load is of an earlier image
                continue;
            }
            if (hooks == ImageHooks::None && same_module(*at, entry) && !after_unload) {
                ++report.redundant;  // the full list (or a resync) already had it
                continue;
            }
            // Another module was there; or the hooks of the known image are gone (Absent: replaced, even with the same path, size and
            // base); or, without hooks, an unload was seen at this base since: what the broker knows at this base is gone.
            report.events.push_back(make_event(ipc::ModuleEventKind::Unloaded, 0, *at));
            known.erase(at);
        }
        report.events.push_back(make_event(ipc::ModuleEventKind::Loaded, event.seq, entry));
        known.push_back(std::move(entry));
    }
    report.deferred = report.deferred || !state.deferred.empty();

    // 3. The module list as the fallback and the check.
    if (resync != 0) {
        state.suspect.reset();
        report.resync_reason = resync;
        if (snapshot == nullptr) {
            report.need_full_list = true;
        } else {
            append_snapshot_diff(report, state, *snapshot, probe);
        }
        return report;
    }
    if (!known_trusted) {
        // The broker has no full list yet (the session's could not be built, or was refused): the snapshot tells it at once, the way
        // the module-list diff did before WP-48. Not a disagreement, no diagnostic.
        state.suspect.reset();
        if (snapshot == nullptr) {
            report.need_full_list = true;
        } else {
            append_snapshot_diff(report, state, *snapshot, probe);
        }
        return report;
    }
    if (snapshot == nullptr || !events.empty() || !rechecks.empty() || !state.rechecks.empty() || !quiet) {
        state.suspect.reset();  // a change is under way: the list may lag behind the events, or run ahead of them
        return report;
    }
    ModuleDelta delta = diff_modules(known, *snapshot);
    if (delta.empty()) {
        state.suspect.reset();
        return report;
    }
    if (state.suspect.has_value() && same_delta(*state.suspect, delta)) {
        state.suspect.reset();
        report.resync_reason = module_events::ids::kListDisagrees;
        append_snapshot_diff(report, state, *snapshot, probe);
        return report;
    }
    state.suspect = std::move(delta);
    return report;
}

std::vector<ipc::ModuleEvents> to_messages(const ModulePollReport& report, std::size_t max_events, std::size_t max_bytes) {
    constexpr std::size_t kEventOverhead = 32;  // kind, seq, base, size, path length (and some slack)
    std::vector<ipc::ModuleEvents> messages;
    std::size_t next = 0;
    do {
        ipc::ModuleEvents message;
        if (messages.empty()) {
            if (report.resync_reason != 0) {
                message.flags = ipc::kModuleEventsResync;
                message.reason = report.resync_reason;
            }
            message.lost = report.lost;
        }
        std::size_t bytes = 0;
        while (next < report.events.size() && message.events.size() < max_events &&
               (message.events.empty() || bytes + report.events[next].module.path.size() + kEventOverhead <= max_bytes)) {
            bytes += report.events[next].module.path.size() + kEventOverhead;
            message.events.push_back(report.events[next++]);
        }
        messages.push_back(std::move(message));
    } while (next < report.events.size());
    return messages;
}

}  // namespace cml::agent
