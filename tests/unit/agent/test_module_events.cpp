// WP-48 (ADR-0044): the loader notification ring (module_ring.h) and the reconciliation of its events with the module list
// (track_modules, module_diff.h). Portable: runs on every host.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "module_diff.h"
#include "module_event_ids.h"
#include "module_ring.h"

using namespace cml::agent;
using cml::ipc::ModuleEntry;
using cml::ipc::ModuleEvent;
using cml::ipc::ModuleEventKind;

namespace {

struct Item {
    std::uint32_t producer = 0;
    std::uint32_t index = 0;
    std::uint64_t seq = 0;
};

ModuleEntry module(std::uint64_t base, std::uint64_t size, const char* path) {
    return ModuleEntry{base, size, path};
}

DrainedEvent loaded(std::uint64_t seq, const ModuleEntry& m, bool truncated = false) {
    DrainedEvent e;
    e.kind = ModuleEventKind::Loaded;
    e.seq = seq;
    e.module = m;
    e.path_truncated = truncated;
    return e;
}

DrainedEvent unloaded(std::uint64_t seq, const ModuleEntry& m) {
    DrainedEvent e;
    e.kind = ModuleEventKind::Unloaded;
    e.seq = seq;
    e.module = m;
    return e;
}

bool is(const ModuleEvent& e, ModuleEventKind kind, std::uint64_t seq, std::uint64_t base, const char* path) {
    return e.kind == kind && e.seq == seq && e.module.base == base && e.module.path == path;
}

const ModuleEntry kGame = module(0x140000000ull, 0x30000, "C:\\game\\game.exe");
const ModuleEntry kPlugin = module(0x180000000ull, 0x10000, "C:\\game\\plugin.dll");
const ModuleEntry kOther = module(0x190000000ull, 0x20000, "C:\\game\\other.dll");

}  // namespace

// --- the ring ------------------------------------------------------------------------------------------------------------------

TEST_CASE("WP-48 ring: events come out in the order they went in, numbered 1, 2, ...; an empty ring pops nothing") {
    EventRing<Item, 8> ring;
    Item out;
    CHECK_FALSE(ring.pop(out));
    CHECK_FALSE(ring.has_pending());
    for (std::uint32_t i = 0; i < 5; ++i) {
        CHECK(ring.push([i](Item& slot, std::uint64_t seq) { slot = Item{0, i, seq}; }));
    }
    CHECK(ring.has_pending());
    CHECK(ring.size_approx() == 5);
    for (std::uint32_t i = 0; i < 5; ++i) {
        REQUIRE(ring.pop(out));
        CHECK(out.index == i);
        CHECK(out.seq == i + 1);
    }
    CHECK_FALSE(ring.pop(out));
    CHECK(ring.lost() == 0);
    // Many laps: positions wrap around the slots, the numbering goes on.
    for (std::uint32_t i = 0; i < 1000; ++i) {
        REQUIRE(ring.push([i](Item& slot, std::uint64_t seq) { slot = Item{0, i, seq}; }));
        REQUIRE(ring.pop(out));
        CHECK(out.index == i);
        CHECK(out.seq == 6 + i);
    }
}

TEST_CASE("WP-48 ring: a full ring drops the new event and counts it; what is in it stays intact; a freed slot is used again") {
    EventRing<Item, 4> ring;
    for (std::uint32_t i = 0; i < 4; ++i) {
        REQUIRE(ring.push([i](Item& slot, std::uint64_t seq) { slot = Item{0, i, seq}; }));
    }
    bool written = false;
    CHECK_FALSE(ring.push([&written](Item&, std::uint64_t) { written = true; }));
    CHECK_FALSE(ring.push([&written](Item&, std::uint64_t) { written = true; }));
    CHECK_FALSE(written);  // a dropped event never touches a slot
    CHECK(ring.lost() == 2);
    CHECK(ring.size_approx() == 4);
    Item out;
    REQUIRE(ring.pop(out));
    CHECK(out.index == 0);
    CHECK(ring.push([](Item& slot, std::uint64_t seq) { slot = Item{0, 99, seq}; }));
    for (std::uint32_t expected : {1u, 2u, 3u, 99u}) {
        REQUIRE(ring.pop(out));
        CHECK(out.index == expected);
    }
    CHECK(out.seq == 5);  // dropped events get no number: the numbers of what was kept have no gaps
    CHECK(ring.lost() == 2);
}

TEST_CASE("WP-48 ring: drained while several threads push, nothing is lost twice, duplicated or reordered per producer") {
    constexpr std::uint32_t kProducers = 4;
    constexpr std::uint32_t kPerProducer = 50000;
    auto ring = std::make_unique<EventRing<Item, 64>>();  // small, so it is full again and again
    std::atomic<bool> go{false};
    std::atomic<std::uint32_t> done{0};
    std::vector<std::uint64_t> pushed(kProducers, 0);
    std::vector<std::thread> producers;
    for (std::uint32_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            for (std::uint32_t i = 0; i < kPerProducer; ++i) {
                if (ring->push([p, i](Item& slot, std::uint64_t seq) { slot = Item{p, i, seq}; })) {
                    ++pushed[p];
                }
            }
            done.fetch_add(1);
        });
    }
    std::vector<std::int64_t> last_index(kProducers, -1);
    std::vector<std::uint64_t> popped(kProducers, 0);
    std::uint64_t last_seq = 0;
    bool ordered = true;
    bool numbered = true;
    go.store(true);
    Item out;
    for (;;) {
        const bool finished = done.load() == kProducers;
        bool any = false;
        while (ring->pop(out)) {
            any = true;
            ordered = ordered && out.producer < kProducers && static_cast<std::int64_t>(out.index) > last_index[out.producer];
            numbered = numbered && out.seq == last_seq + 1;  // the consumer sees every number once, in order
            last_index[out.producer] = out.index;
            last_seq = out.seq;
            ++popped[out.producer];
        }
        if (finished && !any) {
            break;
        }
    }
    for (std::thread& t : producers) {
        t.join();
    }
    CHECK(ordered);
    CHECK(numbered);
    std::uint64_t total_pushed = 0;
    for (std::uint32_t p = 0; p < kProducers; ++p) {
        CHECK(popped[p] == pushed[p]);  // what was accepted was delivered, once
        total_pushed += pushed[p];
    }
    CHECK(total_pushed + ring->lost() == std::uint64_t{kProducers} * kPerProducer);  // and the rest was counted as lost
    CHECK(last_seq == total_pushed);
}

TEST_CASE("WP-48 ring: a slot holds a path of up to 520 units; a longer one is cut and flagged; patch images are marked") {
    auto slot = std::make_unique<RawModuleEvent>();
    const std::u16string path = u"C:\\game\\plugin.dll";
    fill_raw_event(*slot, 7, kRawEventLoaded, 0x180000000ull, 0x10000, path.data(), path.size(), false);
    DrainedEvent e = to_drained(*slot);
    CHECK(e.kind == ModuleEventKind::Loaded);
    CHECK(e.seq == 7);
    CHECK(e.module.base == 0x180000000ull);
    CHECK(e.module.size == 0x10000);
    CHECK(e.module.path == "C:\\game\\plugin.dll");
    CHECK_FALSE(e.path_truncated);
    CHECK_FALSE(e.patch_image);

    const std::u16string long_path(kRawPathUnits + 40, u'a');
    fill_raw_event(*slot, 8, kRawEventUnloaded, 0x10000, 0x1000, long_path.data(), long_path.size(), true);
    e = to_drained(*slot);
    CHECK(e.kind == ModuleEventKind::Unloaded);
    CHECK(e.path_truncated);
    CHECK(e.patch_image);
    CHECK(e.module.path.size() == kRawPathUnits);

    fill_raw_event(*slot, 9, kRawEventLoaded, 0x10000, 0x1000, nullptr, 12, false);
    CHECK(to_drained(*slot).module.path.empty());

    // UTF-16 to UTF-8: BMP, a surrogate pair, an unpaired surrogate.
    const char16_t text[] = {u'\x00e9', 0xD83D, 0xDE00, 0xD800, u'x'};
    CHECK(utf8_from_utf16(text, 5) == "\xC3\xA9\xF0\x9F\x98\x80\xEF\xBF\xBDx");
}

// --- track_modules -------------------------------------------------------------------------------------------------------------

TEST_CASE("WP-48: an unload and a load at the same base inside one poll are both reported, in order (R-53)") {
    ModuleTrackState state;
    state.known = {kGame, kPlugin};
    const std::vector<ModuleEntry> snapshot = {kGame, kPlugin};  // the list cannot tell: same base, size and path
    const ModulePollReport report = track_modules(state, {unloaded(11, kPlugin), loaded(12, kPlugin)}, 0, &snapshot, true);
    REQUIRE(report.events.size() == 2);
    CHECK(is(report.events[0], ModuleEventKind::Unloaded, 11, kPlugin.base, "C:\\game\\plugin.dll"));
    CHECK(is(report.events[1], ModuleEventKind::Loaded, 12, kPlugin.base, "C:\\game\\plugin.dll"));
    CHECK(report.resync_reason == 0);
    CHECK_FALSE(report.need_full_list);
    CHECK(state.known.size() == 2);
    // The plain list diff of the same poll sees nothing: the defect this package fixes.
    CHECK(diff_modules({kGame, kPlugin}, snapshot).empty());
}

TEST_CASE("WP-48: a load the broker knows already, an unload of something it never knew, and patch images are not reported") {
    ModuleTrackState state;
    state.known = {kGame, kPlugin};
    DrainedEvent patch = loaded(3, module(0x200000000ull, 0x8000, "C:\\session\\g1\\patch.dll"));
    patch.patch_image = true;
    DrainedEvent patch_gone = unloaded(5, patch.module);
    patch_gone.patch_image = true;
    const std::vector<ModuleEntry> snapshot = {kGame, kPlugin, kOther};
    const ModulePollReport report = track_modules(
        state, {loaded(1, kPlugin), unloaded(2, module(0x300000000ull, 0x1000, "C:\\gone.dll")), patch, loaded(4, kOther), patch_gone}, 0,
        &snapshot, true);
    REQUIRE(report.events.size() == 1);
    CHECK(is(report.events[0], ModuleEventKind::Loaded, 4, kOther.base, "C:\\game\\other.dll"));
    CHECK(report.redundant == 2);
    CHECK(report.patch_events == 2);
    CHECK(state.known.size() == 3);
}

TEST_CASE("WP-48: a load at a base that holds another module reports that module's unload first") {
    ModuleTrackState state;
    state.known = {kGame, kPlugin};
    const ModuleEntry replacement = module(kPlugin.base, 0x12000, "C:\\game\\replacement.dll");
    const std::vector<ModuleEntry> snapshot = {kGame, replacement};
    const ModulePollReport report = track_modules(state, {loaded(9, replacement)}, 0, &snapshot, true);
    REQUIRE(report.events.size() == 2);
    CHECK(is(report.events[0], ModuleEventKind::Unloaded, 0, kPlugin.base, "C:\\game\\plugin.dll"));
    CHECK(is(report.events[1], ModuleEventKind::Loaded, 9, kPlugin.base, "C:\\game\\replacement.dll"));
}

TEST_CASE("WP-48: a ring that overflowed reports the events it kept and completes the rest from the module list (CML4721)") {
    ModuleTrackState state;
    state.known = {kGame, kPlugin};
    // Kept: the plugin's unload and load. Lost: other.dll's load.
    const std::vector<ModuleEntry> snapshot = {kGame, kPlugin, kOther};
    const ModulePollReport report = track_modules(state, {unloaded(20, kPlugin), loaded(21, kPlugin)}, 3, &snapshot, true);
    CHECK(report.resync_reason == module_events::ids::kRingOverflow);
    CHECK(report.lost == 3);
    REQUIRE(report.events.size() == 3);
    CHECK(is(report.events[0], ModuleEventKind::Unloaded, 20, kPlugin.base, "C:\\game\\plugin.dll"));
    CHECK(is(report.events[1], ModuleEventKind::Loaded, 21, kPlugin.base, "C:\\game\\plugin.dll"));
    CHECK(is(report.events[2], ModuleEventKind::Loaded, 0, kOther.base, "C:\\game\\other.dll"));
    CHECK(state.known.size() == 3);

    SUBCASE("without a module list the agent asks for a full one") {
        ModuleTrackState fresh;
        fresh.known = {kGame};
        const ModulePollReport blind = track_modules(fresh, {loaded(30, kPlugin)}, 1, nullptr, true);
        CHECK(blind.need_full_list);
        CHECK(blind.resync_reason == module_events::ids::kRingOverflow);
        REQUIRE(blind.events.size() == 1);  // what it has is still reported
        CHECK(blind.events[0].seq == 30);
    }
}

TEST_CASE("WP-48: a cut path is completed from the module list; one that cannot be is a resync (CML4723)") {
    const std::string full = "C:\\a\\very\\long\\path\\plugin.dll";
    const ModuleEntry long_module = module(0x1C0000000ull, 0x5000, full.c_str());
    SUBCASE("still loaded: the list has the full path") {
        ModuleTrackState state;
        state.known = {kGame};
        const std::vector<ModuleEntry> snapshot = {kGame, long_module};
        const ModulePollReport report =
            track_modules(state, {loaded(2, module(long_module.base, long_module.size, "C:\\a\\very"), true)}, 0, &snapshot, true);
        CHECK(report.resync_reason == 0);
        REQUIRE(report.events.size() == 1);
        CHECK(report.events[0].module.path == full);
    }
    SUBCASE("gone again: completed from the list diff") {
        ModuleTrackState state;
        state.known = {kGame, kPlugin};
        const std::vector<ModuleEntry> snapshot = {kGame};
        const ModulePollReport report =
            track_modules(state, {loaded(2, module(long_module.base, long_module.size, "C:\\a\\very"), true), unloaded(3, kPlugin)}, 0,
                          &snapshot, true);
        CHECK(report.resync_reason == module_events::ids::kPathUnresolved);
        REQUIRE(report.events.size() == 1);  // the plugin's unload; the long module came and went
        CHECK(is(report.events[0], ModuleEventKind::Unloaded, 3, kPlugin.base, "C:\\game\\plugin.dll"));
        CHECK(state.known.size() == 1);
    }
}

TEST_CASE("WP-48: a disagreement with the module list is corrected only when two quiet polls in a row show it (CML4722)") {
    ModuleTrackState state;
    state.known = {kGame, kPlugin};
    const std::vector<ModuleEntry> without_plugin = {kGame};

    // A notification that runs ahead of the list (or a list that runs ahead of it) is a transient: nothing is reported.
    ModulePollReport report = track_modules(state, {}, 0, &without_plugin, true);
    CHECK(report.events.empty());
    CHECK(report.resync_reason == 0);
    CHECK(state.suspect.has_value());

    SUBCASE("the same difference again: corrected from the list") {
        report = track_modules(state, {}, 0, &without_plugin, true);
        CHECK(report.resync_reason == module_events::ids::kListDisagrees);
        REQUIRE(report.events.size() == 1);
        CHECK(is(report.events[0], ModuleEventKind::Unloaded, 0, kPlugin.base, "C:\\game\\plugin.dll"));
        CHECK(state.known.size() == 1);
        CHECK_FALSE(state.suspect.has_value());
    }
    SUBCASE("the event arrives in the next poll: no correction") {
        report = track_modules(state, {unloaded(40, kPlugin)}, 0, &without_plugin, true);
        CHECK(report.resync_reason == 0);
        REQUIRE(report.events.size() == 1);
        CHECK(report.events[0].seq == 40);
        CHECK_FALSE(state.suspect.has_value());
        CHECK(track_modules(state, {}, 0, &without_plugin, true).events.empty());
    }
    SUBCASE("a notification pending while the list was taken: not quiet, nothing remembered") {
        report = track_modules(state, {}, 0, &without_plugin, false);
        CHECK(report.events.empty());
        CHECK_FALSE(state.suspect.has_value());
        report = track_modules(state, {}, 0, &without_plugin, true);
        CHECK(report.events.empty());  // only the first sighting again
        CHECK(state.suspect.has_value());
    }
    SUBCASE("another difference the second time: remembered, not corrected") {
        const std::vector<ModuleEntry> with_other = {kGame, kOther};
        report = track_modules(state, {}, 0, &with_other, true);
        CHECK(report.events.empty());
        CHECK(state.suspect.has_value());
    }
}

// --- review round 1 ------------------------------------------------------------------------------------------------------------

TEST_CASE("WP-48 review 2: the patch image marker names the base AND the thread; another thread's load at that base is a module") {
    CHECK_FALSE(is_patch_image_event(0x200000000ull, 7));
    {
        const ScopedPatchImage marker(0x200000000ull, 7);
        CHECK(is_patch_image_event(0x200000000ull, 7));        // the patch loader's own load
        CHECK_FALSE(is_patch_image_event(0x200000000ull, 8));  // an application DLL another thread loaded there (CML4202 window)
        CHECK_FALSE(is_patch_image_event(0x210000000ull, 7));  // a dependency the patch pulled in, at its own base
        CHECK_FALSE(is_patch_image_event(0, 7));
    }
    CHECK_FALSE(is_patch_image_event(0x200000000ull, 7));  // cleared with the scope
    CHECK(g_patch_image_thread.load() == 0);
}

TEST_CASE("WP-48 review 1: while the broker has no full list, a difference from the snapshot is reported at once, without CML4722") {
    ModuleTrackState state;  // the session's first list could not be built: the broker got an empty one
    const std::vector<ModuleEntry> snapshot = {kGame, kPlugin};
    const ModulePollReport report = track_modules(state, {}, 0, &snapshot, true, /*known_trusted=*/false);
    CHECK(report.resync_reason == 0);
    REQUIRE(report.events.size() == 2);
    CHECK(is(report.events[0], ModuleEventKind::Loaded, 0, kGame.base, "C:\\game\\game.exe"));
    CHECK(is(report.events[1], ModuleEventKind::Loaded, 0, kPlugin.base, "C:\\game\\plugin.dll"));
    CHECK_FALSE(state.suspect.has_value());
    CHECK(state.known.size() == 2);
    // The next polls agree: nothing more, and never a disagreement.
    CHECK(track_modules(state, {}, 0, &snapshot, true, false).events.empty());
    const ModulePollReport trusted = track_modules(state, {}, 0, &snapshot, true, true);
    CHECK(trusted.events.empty());
    CHECK(trusted.resync_reason == 0);

    SUBCASE("events of the moment come first, in order") {
        ModuleTrackState fresh;
        const ModulePollReport mixed = track_modules(fresh, {loaded(5, kOther)}, 0, &snapshot, true, false);
        REQUIRE(mixed.events.size() == 4);
        CHECK(is(mixed.events[0], ModuleEventKind::Loaded, 5, kOther.base, "C:\\game\\other.dll"));
        CHECK(is(mixed.events[1], ModuleEventKind::Unloaded, 0, kOther.base, "C:\\game\\other.dll"));  // not in the snapshot: the list wins
        CHECK(mixed.events[2].kind == ModuleEventKind::Loaded);
        CHECK(mixed.events[3].kind == ModuleEventKind::Loaded);
        CHECK(mixed.resync_reason == 0);
    }
    SUBCASE("without a snapshot the full list is asked for") {
        ModuleTrackState fresh;
        CHECK(track_modules(fresh, {}, 0, nullptr, true, false).need_full_list);
    }
}

TEST_CASE("WP-48 review 3: a load at a base whose unload was seen is never redundant, even when a lagging list brought the entry back") {
    SUBCASE("a resync whose list still showed the unloaded image") {
        ModuleTrackState state;
        state.known = {kGame, kPlugin};
        const std::vector<ModuleEntry> lagging = {kGame, kPlugin, kOther};  // the plugin's unload is delivered, the list still has it
        ModulePollReport report = track_modules(state, {unloaded(10, kPlugin)}, 2, &lagging, true);
        CHECK(report.resync_reason == module_events::ids::kRingOverflow);
        REQUIRE(report.events.size() == 3);
        CHECK(is(report.events[0], ModuleEventKind::Unloaded, 10, kPlugin.base, "C:\\game\\plugin.dll"));
        CHECK(report.events[1].kind == ModuleEventKind::Loaded);  // from the lagging list
        CHECK(state.known.size() == 3);
        // The real reload at that base: unload (inferred, seq 0) and load, so the broker takes the re-enable path.
        report = track_modules(state, {loaded(11, kPlugin)}, 0, &lagging, true);
        REQUIRE(report.events.size() == 2);
        CHECK(is(report.events[0], ModuleEventKind::Unloaded, 0, kPlugin.base, "C:\\game\\plugin.dll"));
        CHECK(is(report.events[1], ModuleEventKind::Loaded, 11, kPlugin.base, "C:\\game\\plugin.dll"));
        CHECK(report.redundant == 0);
        CHECK(state.unloads_seen.empty());
        // Once that load is reported, the next identical load is redundant again.
        report = track_modules(state, {loaded(12, kPlugin)}, 0, &lagging, true);
        CHECK(report.events.empty());
        CHECK(report.redundant == 1);
    }
    SUBCASE("a redundant unload is remembered too (a list sent again later can still show the image)") {
        ModuleTrackState state;
        state.known = {kGame};
        ModulePollReport report = track_modules(state, {unloaded(20, kPlugin)}, 0, nullptr, true);
        CHECK(report.redundant == 1);
        state.known = {kGame, kPlugin};  // a full list sent again (not a new session): unloads_seen is kept
        report = track_modules(state, {loaded(21, kPlugin)}, 0, nullptr, true);
        REQUIRE(report.events.size() == 2);
        CHECK(report.events[0].kind == ModuleEventKind::Unloaded);
        CHECK(report.events[1].seq == 21);
    }
}

TEST_CASE("WP-48 review 3: the session's first list takes the notifications drained before it into account") {
    const std::vector<ModuleEntry> list = {kGame, kPlugin};
    SUBCASE("an unload the list does not show yet: the image is left out") {
        const std::vector<ModuleEntry> out = apply_before_list({unloaded(1, kPlugin)}, list);
        REQUIRE(out.size() == 1);
        CHECK(out[0].base == kGame.base);
    }
    SUBCASE("unloaded and loaded again: the list's entry stays") {
        const std::vector<ModuleEntry> out = apply_before_list({unloaded(1, kPlugin), loaded(2, kPlugin)}, list);
        REQUIRE(out.size() == 2);
        CHECK(out[1].base == kPlugin.base);
        CHECK(out[1].path == "C:\\game\\plugin.dll");
    }
    SUBCASE("a load the list does not have (gone again) is not added; patch images are ignored") {
        DrainedEvent patch = unloaded(4, kGame);
        patch.patch_image = true;
        const std::vector<ModuleEntry> out = apply_before_list({loaded(3, kOther), patch}, list);
        CHECK(out.size() == 2);
    }
    SUBCASE("nothing drained: the list as it is") {
        CHECK(apply_before_list({}, list).size() == 2);
    }
}

TEST_CASE("WP-48 review 5: without a module list a cut path waits for the next poll, with everything after it, instead of CML4723") {
    const ModuleEntry long_module = module(0x1C0000000ull, 0x5000, "C:\\a\\very\\long\\path\\plugin.dll");
    ModuleTrackState state;
    state.known = {kGame, kPlugin};
    ModulePollReport report = track_modules(
        state, {unloaded(1, kPlugin), loaded(2, module(long_module.base, long_module.size, "C:\\a\\very"), true), loaded(3, kOther)}, 0,
        nullptr, true);
    CHECK(report.resync_reason == 0);
    CHECK(report.deferred);
    REQUIRE(report.events.size() == 1);  // the unload before it
    CHECK(report.events[0].seq == 1);
    REQUIRE(state.deferred.size() == 2);
    const std::vector<ModuleEntry> snapshot = {kGame, long_module, kOther};
    report = track_modules(state, {}, 0, &snapshot, true);
    CHECK(report.resync_reason == 0);
    CHECK_FALSE(report.deferred);
    REQUIRE(report.events.size() == 2);
    CHECK(is(report.events[0], ModuleEventKind::Loaded, 2, long_module.base, "C:\\a\\very\\long\\path\\plugin.dll"));
    CHECK(is(report.events[1], ModuleEventKind::Loaded, 3, kOther.base, "C:\\game\\other.dll"));
    CHECK(state.deferred.empty());
}

TEST_CASE("WP-48 review 4: a report split over several ModuleEvents carries the resync on the first message only, in order") {
    ModulePollReport report;
    report.resync_reason = module_events::ids::kRingOverflow;
    report.lost = 9;
    for (std::uint64_t i = 0; i < 5; ++i) {
        ModuleEvent e;
        e.kind = ModuleEventKind::Loaded;
        e.seq = i + 1;
        e.module = module(0x100000000ull + i * 0x10000ull, 0x1000, "C:\\m.dll");
        report.events.push_back(e);
    }
    const std::vector<cml::ipc::ModuleEvents> messages = to_messages(report, 2, 1u << 20);
    REQUIRE(messages.size() == 3);
    CHECK(messages[0].flags == cml::ipc::kModuleEventsResync);
    CHECK(messages[0].reason == module_events::ids::kRingOverflow);
    CHECK(messages[0].lost == 9);
    for (std::size_t m = 1; m < messages.size(); ++m) {
        CHECK(messages[m].flags == 0);
        CHECK(messages[m].reason == 0);
        CHECK(messages[m].lost == 0);
    }
    std::uint64_t seq = 0;
    for (const cml::ipc::ModuleEvents& m : messages) {
        CHECK(m.events.size() <= 2);
        for (const ModuleEvent& e : m.events) {
            CHECK(e.seq == ++seq);
        }
    }
    CHECK(seq == 5);

    // By size: every message holds at least one event, however long its path.
    const std::vector<cml::ipc::ModuleEvents> by_bytes = to_messages(report, 1024, 40);
    CHECK(by_bytes.size() == 5);

    // A resync without events is one (empty) message, so the broker still hears of it.
    ModulePollReport empty;
    empty.resync_reason = module_events::ids::kPathUnresolved;
    const std::vector<cml::ipc::ModuleEvents> one = to_messages(empty, 1024, 1u << 20);
    REQUIRE(one.size() == 1);
    CHECK(one[0].events.empty());
    CHECK(one[0].reason == module_events::ids::kPathUnresolved);
}
