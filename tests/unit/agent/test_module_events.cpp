// WP-48 (ADR-0044): the loader notification ring (module_ring.h) and the reconciliation of its events with the module list
// (track_modules, module_diff.h). Portable: runs on every host.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
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
    SUBCASE("a redundant unload is remembered too (a resync of the same poll can still show the image)") {
        ModuleTrackState state;
        state.known = {kGame};
        const std::vector<ModuleEntry> lagging = {kGame, kPlugin};
        ModulePollReport report = track_modules(state, {unloaded(20, kPlugin)}, 1, &lagging, true);
        CHECK(report.redundant == 1);
        CHECK(state.known.size() == 2);  // the lagging list brought it in
        report = track_modules(state, {loaded(21, kPlugin)}, 0, &lagging, true);
        REQUIRE(report.events.size() == 2);
        CHECK(report.events[0].kind == ModuleEventKind::Unloaded);
        CHECK(report.events[1].seq == 21);
    }
}

// --- review round 3: the hook bytes are the arbiter (ADR-0044 ruling) -------------------------------------------------------------

namespace {

// A stand-in for Installer::hooks_in_range: what the probe finds at each base (None where nothing is set).
struct FakeHooks {
    std::vector<std::pair<std::uint64_t, ImageHooks>> at;
    void set(std::uint64_t base, ImageHooks hooks) {
        for (auto& entry : at) {
            if (entry.first == base) {
                entry.second = hooks;
                return;
            }
        }
        at.emplace_back(base, hooks);
    }
    HookProbe probe() const {
        return [this](const ModuleEntry& m) {
            for (const auto& entry : at) {
                if (entry.first == m.base) {
                    return entry.second;
                }
            }
            return ImageHooks::None;
        };
    }
};

std::size_t unloads_of(const ModulePollReport& report, std::uint64_t base) {
    std::size_t n = 0;
    for (const ModuleEvent& e : report.events) {
        n += e.kind == ModuleEventKind::Unloaded && e.module.base == base ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("WP-48 review round 3 (1): a list that lags over two polls cannot hide a reload") {
    // The plugin's unload is drained in poll 1 and reported (its hook records are forgotten with it). The loader list still shows the
    // old image in polls 2 and 3 (preemption before ntdll unlinks it), so a resync puts it back; the new image's load comes in poll 4.
    const std::vector<ModuleEntry> lagging = {kGame, kPlugin};
    FakeHooks hooks;
    ModuleTrackState state;
    state.known = {kGame, kPlugin};
    hooks.set(kPlugin.base, ImageHooks::Absent);  // poll 1: unmapped
    ModulePollReport report = track_modules(state, {unloaded(1, kPlugin)}, 0, &lagging, true, true, hooks.probe());
    REQUIRE(unloads_of(report, kPlugin.base) == 1);
    hooks.set(kPlugin.base, ImageHooks::None);  // forget_range ran with that report
    report = track_modules(state, {}, 3, &lagging, true, true, hooks.probe());  // poll 2: a resync from the lagging list
    CHECK(report.resync_reason == module_events::ids::kRingOverflow);
    CHECK(state.known.size() == 2);
    report = track_modules(state, {}, 0, &lagging, true, /*known_trusted=*/false, hooks.probe());  // poll 3: still lagging
    CHECK(report.events.empty());

    SUBCASE("no hooks were re-applied in between: the reload is reported (unloads_seen is kept as the hint)") {
        report = track_modules(state, {loaded(2, kPlugin)}, 0, &lagging, true, true, hooks.probe());
        REQUIRE(report.events.size() == 2);
        CHECK(is(report.events[0], ModuleEventKind::Unloaded, 0, kPlugin.base, "C:\\game\\plugin.dll"));
        CHECK(is(report.events[1], ModuleEventKind::Loaded, 2, kPlugin.base, "C:\\game\\plugin.dll"));
    }
    SUBCASE("the broker re-applied into the new image already (its hooks are in place): the load is of that image, redundant") {
        hooks.set(kPlugin.base, ImageHooks::Present);
        report = track_modules(state, {loaded(2, kPlugin)}, 0, &lagging, true, true, hooks.probe());
        CHECK(report.events.empty());
        CHECK(report.hooked_live == 1);
    }
}

TEST_CASE("WP-48 review round 3 (2): unloads are judged against what the broker believes, so a same-base reload is two changes") {
    // The agent keeps `known` as the last list the broker accepted when it refuses one (agent.cpp); against that list an unload and a
    // load of the same base are an Unloaded and a Loaded. (Against an emptied list the unload would be redundant: a bare Loaded.)
    FakeHooks hooks;
    hooks.set(kPlugin.base, ImageHooks::Absent);  // the old image's hooks went with it
    ModuleTrackState believed;
    believed.known = {kGame, kPlugin};
    const std::vector<ModuleEntry> snapshot = {kGame, kPlugin};
    ModulePollReport report =
        track_modules(believed, {unloaded(5, kPlugin), loaded(6, kPlugin)}, 0, &snapshot, true, /*known_trusted=*/false, hooks.probe());
    REQUIRE(report.events.size() == 2);
    CHECK(is(report.events[0], ModuleEventKind::Unloaded, 5, kPlugin.base, "C:\\game\\plugin.dll"));
    CHECK(is(report.events[1], ModuleEventKind::Loaded, 6, kPlugin.base, "C:\\game\\plugin.dll"));

    ModuleTrackState emptied;  // what the round-2 change did
    report = track_modules(emptied, {unloaded(5, kPlugin), loaded(6, kPlugin)}, 0, &snapshot, true, false, hooks.probe());
    CHECK(unloads_of(report, kPlugin.base) == 0);  // the defect: no unload, so no re-enable in the broker
}

TEST_CASE("WP-48 review round 3 (3): a live, hooked image survives stale older events and stale lists; it is never unloaded") {
    // A list sent again showed the new image, the broker re-applied the edits into it (its hooks are in place), and only then are
    // older notifications applied: an unload of the old image and the load of the new one.
    FakeHooks hooks;
    hooks.set(kPlugin.base, ImageHooks::Present);
    ModuleTrackState state;
    state.known = {kGame, kPlugin};
    const std::vector<ModuleEntry> snapshot = {kGame, kPlugin};
    ModulePollReport report = track_modules(state, {unloaded(7, kPlugin), loaded(8, kPlugin), loaded(9, kOther)}, 0, &snapshot,
                                            true, true, hooks.probe());
    CHECK(unloads_of(report, kPlugin.base) == 0);  // never reported: no forget_range for the live hooks
    REQUIRE(report.events.size() == 1);            // the unrelated load goes on
    CHECK(report.events[0].module.base == kOther.base);
    REQUIRE(state.rechecks.size() == 1);            // checked again (the loader may not have unmapped it yet) ...
    REQUIRE(state.deferred.size() == 1);            // ... and the load at that base waits behind it, in order
    const std::vector<ModuleEntry> all = {kGame, kPlugin, kOther};
    for (unsigned poll = 0; poll < kHookedUnloadRechecks + 2; ++poll) {
        report = track_modules(state, {}, 0, &all, true, true, hooks.probe());
        CHECK(report.events.empty());
    }
    CHECK(state.rechecks.empty());  // dropped as stale: the hooks never went
    CHECK(state.deferred.empty());  // the load was redundant once it could be judged (the hooked image is live)
    CHECK(state.known.size() == 3);

    SUBCASE("a stale list that does not show it (a resync) does not unload it either") {
        const std::vector<ModuleEntry> without = {kGame, kOther};
        report = track_modules(state, {}, 2, &without, true, true, hooks.probe());
        CHECK(report.resync_reason == module_events::ids::kRingOverflow);
        CHECK(unloads_of(report, kPlugin.base) == 0);
        CHECK(report.hooked_live == 1);
        CHECK(state.known.size() == 3);
        CHECK(list_with_hooked(state.known, without, hooks.probe()).size() == 3);  // and a list sent again keeps it
    }
    SUBCASE("nor a session's first list that drained its old unload") {
        CHECK(apply_before_list({unloaded(7, kPlugin)}, snapshot, hooks.probe()).size() == 2);
        CHECK(apply_before_list({unloaded(7, kPlugin)}, snapshot).size() == 1);  // without hooks the unload applies
    }
}

TEST_CASE("WP-48 review round 3: an unload is reported as soon as its image's hooks are gone (the loader unmapped it after notifying)") {
    FakeHooks hooks;
    hooks.set(kPlugin.base, ImageHooks::Present);  // the notification came a moment before the unmap
    ModuleTrackState state;
    state.known = {kGame, kPlugin};
    const std::vector<ModuleEntry> snapshot = {kGame, kPlugin};
    ModulePollReport report = track_modules(state, {unloaded(11, kPlugin)}, 0, &snapshot, true, true, hooks.probe());
    CHECK(report.events.empty());
    hooks.set(kPlugin.base, ImageHooks::Absent);  // unmapped now; the new image is loaded and its notification drained
    report = track_modules(state, {loaded(12, kPlugin)}, 0, &snapshot, true, true, hooks.probe());
    REQUIRE(report.events.size() == 2);
    CHECK(is(report.events[0], ModuleEventKind::Unloaded, 11, kPlugin.base, "C:\\game\\plugin.dll"));  // its own sequence number
    CHECK(is(report.events[1], ModuleEventKind::Loaded, 12, kPlugin.base, "C:\\game\\plugin.dll"));
    CHECK(state.rechecks.empty());
}

TEST_CASE("WP-48 review round 3: a replaced image with the same path, size and base is told by its missing jump bytes") {
    FakeHooks hooks;
    ModuleTrackState state;
    state.known = {kGame, kPlugin};
    const std::vector<ModuleEntry> snapshot = {kGame, kPlugin};
    SUBCASE("hooks gone: a load of the very same entry is a reload") {
        hooks.set(kPlugin.base, ImageHooks::Absent);
        const ModulePollReport report = track_modules(state, {loaded(3, kPlugin)}, 0, &snapshot, true, true, hooks.probe());
        REQUIRE(report.events.size() == 2);
        CHECK(is(report.events[0], ModuleEventKind::Unloaded, 0, kPlugin.base, "C:\\game\\plugin.dll"));
        CHECK(is(report.events[1], ModuleEventKind::Loaded, 3, kPlugin.base, "C:\\game\\plugin.dll"));
    }
    SUBCASE("hooks in place: the same load is redundant") {
        hooks.set(kPlugin.base, ImageHooks::Present);
        const ModulePollReport report = track_modules(state, {loaded(3, kPlugin)}, 0, &snapshot, true, true, hooks.probe());
        CHECK(report.events.empty());
        CHECK(report.hooked_live == 1);
    }
    SUBCASE("no hooks: as before (redundant unless an unload was seen at that base)") {
        const ModulePollReport report = track_modules(state, {loaded(3, kPlugin)}, 0, &snapshot, true, true, hooks.probe());
        CHECK(report.events.empty());
        CHECK(report.redundant == 1);
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

TEST_CASE("WP-48 review 5: without a module list a cut path waits for the next poll, with the later events at its base only") {
    const ModuleEntry long_module = module(0x1C0000000ull, 0x5000, "C:\\a\\very\\long\\path\\plugin.dll");
    const ModuleEntry cut = module(long_module.base, long_module.size, "C:\\a\\very");
    ModuleTrackState state;
    state.known = {kGame, kPlugin, kOther};
    // Review round 2 (2): an unrelated unload and reload (other.dll) after the cut load is processed now, so its forget_range and the
    // broker's re-enable are not held back while the module list cannot be taken; the long module's own later events wait with it.
    ModulePollReport report = track_modules(
        state, {unloaded(1, kPlugin), loaded(2, cut, true), unloaded(3, kOther), loaded(4, kOther), unloaded(5, long_module), loaded(6, cut, true)},
        0, nullptr, true);
    CHECK(report.resync_reason == 0);
    CHECK(report.deferred);
    CHECK_FALSE(report.need_full_list);
    REQUIRE(report.events.size() == 3);
    CHECK(is(report.events[0], ModuleEventKind::Unloaded, 1, kPlugin.base, "C:\\game\\plugin.dll"));
    CHECK(is(report.events[1], ModuleEventKind::Unloaded, 3, kOther.base, "C:\\game\\other.dll"));
    CHECK(is(report.events[2], ModuleEventKind::Loaded, 4, kOther.base, "C:\\game\\other.dll"));
    REQUIRE(state.deferred.size() == 3);
    CHECK(state.deferred[0].seq == 2);
    CHECK(state.deferred[1].seq == 5);
    CHECK(state.deferred[2].seq == 6);
    // With a list the long module's events go out in their order.
    const std::vector<ModuleEntry> snapshot = {kGame, long_module, kOther};
    report = track_modules(state, {}, 0, &snapshot, true);
    CHECK(report.resync_reason == 0);
    CHECK_FALSE(report.deferred);
    REQUIRE(report.events.size() == 3);
    CHECK(is(report.events[0], ModuleEventKind::Loaded, 2, long_module.base, "C:\\a\\very\\long\\path\\plugin.dll"));
    CHECK(is(report.events[1], ModuleEventKind::Unloaded, 5, long_module.base, "C:\\a\\very\\long\\path\\plugin.dll"));
    CHECK(is(report.events[2], ModuleEventKind::Loaded, 6, long_module.base, "C:\\a\\very\\long\\path\\plugin.dll"));
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
