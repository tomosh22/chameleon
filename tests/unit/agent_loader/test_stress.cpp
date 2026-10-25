// unit.agent_loader: stress, failure injection (the release/LoadLibrary race), threads and the
// private-heap rules (WP-08).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "cml/proc/address_space.h"
#include "cml/proc/loader_diag_ids.h"
#include "doctest.h"
#include "heap.h"
#include "loader/image_writer.h"
#include "loader/patch_loader.h"
#include "loader_test_support.h"

using namespace loadertest;
using cml::agent::loader::PatchLoader;
using cml::agent::loader::ReserveParams;
using cml::agent::loader::TestHooks;
namespace ipc = cml::ipc;
namespace ids = cml::ids;
namespace proc = cml::proc;
namespace heap = cml::agent::heap;

namespace {

bool owned_covers(const PatchLoader& loader, std::uint64_t base, std::uint64_t size) {
    for (const proc::AddressRange& r : loader.owned_ranges()) {
        if (r.base <= base && base + size <= r.end()) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("stress: 500 small patches are loaded into the reserved blocks") {
    PatchLoader loader;
    ReserveParams params;  // the plan's defaults: 4 blocks of 64 MiB within 1.5 GiB
    params.module_base = host_base();
    params.module_size = host_size();
    cml::Result<std::vector<proc::AddressRange>> reserved = loader.reserve_ranges(params);
    REQUIRE_MESSAGE(reserved.has_value(), (reserved.has_value() ? "" : reserved.error().format()));
    REQUIRE(reserved->size() == 4);
    const proc::AddressRange block = (*reserved)[0];

    constexpr unsigned kCount = 500;
    REQUIRE(kCount * stride() <= block.size);
    std::vector<ipc::LoadPatch> requests;
    requests.reserve(kCount);
    for (unsigned i = 0; i < kCount; ++i) {
        requests.push_back(make_request(block.base + i * stride(), "stress_" + std::to_string(i)));
    }

    std::vector<ipc::PatchLoaded> answers;
    answers.reserve(kCount);
    const auto started = std::chrono::steady_clock::now();
    for (const ipc::LoadPatch& request : requests) {
        answers.push_back(loader.load_patch(request));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    unsigned loaded = 0;
    unsigned failures = 0;
    std::string first_problem;
    for (unsigned i = 0; i < kCount; ++i) {
        const std::uint64_t base = block.base + i * stride();
        const ipc::PatchLoaded& a = answers[i];
        std::string problem;
        if (a.status != 0) {
            problem = "patch " + std::to_string(i) + ": " + a.message;
        } else if (a.image_base != base || !is_module_loaded_at(base)) {
            problem = "patch " + std::to_string(i) + " is not at its base";
        } else if (a.attempts != 1 || a.initializers_run != 3) {
            problem = "patch " + std::to_string(i) + " attempts/initializers wrong";
        } else {
            problem = verify_standard(base, host_target_address());  // fix-ups, slots, protection, stub call
            if (problem.empty() && initializer_log(base) != "ABC") {
                problem = "initializers did not run in order";
            }
            if (!problem.empty()) {
                problem = "patch " + std::to_string(i) + ": " + problem;
            }
        }
        if (problem.empty()) {
            ++loaded;
        } else {
            ++failures;
            if (first_problem.empty()) {
                first_problem = problem;
            }
        }
    }
    std::printf("WP08-STRESS loaded=%u failures=%u ms=%lld\n", loaded, failures, ms);
    std::fflush(stdout);
    CHECK_MESSAGE(failures == 0, first_problem);
    CHECK(loaded == kCount);
    CHECK(loader.patch_count() == kCount);

    // Unload everything: the whole block is reserved again and merged back into one range.
    unsigned unload_failures = 0;
    for (unsigned i = kCount; i-- > 0;) {
        if (!loader.unload_patch(block.base + i * stride())) {
            ++unload_failures;
        }
    }
    CHECK(unload_failures == 0);
    CHECK(loader.patch_count() == 0);
    CHECK(owned_covers(loader, block.base, block.size));
    CHECK(query(block.base).State == MEM_RESERVE);
}

namespace {

// Test hook: another thread grabs the released range before LoadLibraryExW gets to it.
struct Grabber {
    std::uint32_t grab_through_attempt = 0;  // grab on attempts 1..N
    std::vector<void*> grabbed;
    std::vector<std::uint32_t> attempts_seen;
    std::vector<std::uint64_t> bases_seen;

    static void hook(void* context, std::uint64_t base, std::uint64_t size, std::uint32_t attempt) {
        auto* self = static_cast<Grabber*>(context);
        self->attempts_seen.push_back(attempt);
        self->bases_seen.push_back(base);
        if (attempt > self->grab_through_attempt) {
            return;
        }
        void* got = nullptr;
        std::thread thief([&] {
            got = VirtualAlloc(reinterpret_cast<LPVOID>(base), static_cast<SIZE_T>(size), MEM_RESERVE | MEM_COMMIT,
                               PAGE_READWRITE);
        });
        thief.join();
        if (got != nullptr) {
            self->grabbed.push_back(got);
        }
    }
    void install(PatchLoader& loader) {
        TestHooks hooks;
        hooks.after_release = &Grabber::hook;
        hooks.context = this;
        loader.set_test_hooks(hooks);
    }
    ~Grabber() {
        for (void* p : grabbed) {
            VirtualFree(p, 0, MEM_RELEASE);
        }
    }
};

ipc::LoadPatch candidates_in_blocks(const LoaderSession& session, unsigned count, const std::string& tag) {
    ipc::LoadPatch request;
    request.request_id = 77;
    request.patch_id = 7;
    for (unsigned i = 0; i < count; ++i) {
        const std::uint64_t base = session.blocks[i].base;
        const ipc::LoadPatch one = make_request(base, tag + std::to_string(i), true, false);
        request.candidates.push_back(one.candidates[0]);
    }
    request.initializer_rvas = init_entry_rvas();
    return request;
}

}  // namespace

TEST_CASE("failure injection: the range is grabbed after release; retry with the next block") {
    LoaderSession session(5);
    Grabber grabber;
    grabber.install(session.loader);

    SUBCASE("two collisions, the third candidate loads") {
        grabber.grab_through_attempt = 2;
        const ipc::LoadPatch request = candidates_in_blocks(session, 3, "inj_a");
        const ipc::PatchLoaded answer = session.load(request);
        REQUIRE_MESSAGE(answer.status == 0, answer.message);
        CHECK(answer.attempts == 3);
        CHECK(answer.candidate_index == 2);
        CHECK(answer.image_base == session.blocks[2].base);
        CHECK(grabber.grabbed.size() == 2);
        CHECK(grabber.attempts_seen == std::vector<std::uint32_t>{1, 2, 3});
        const std::string problems = verify_standard(answer.image_base, host_target_address());
        CHECK_MESSAGE(problems.empty(), problems);
        CHECK(initializer_log(answer.image_base) == "ABC");
        // The grabbed ranges are lost to us; the rest of those blocks is still ours.
        for (unsigned i = 0; i < 2; ++i) {
            CHECK_FALSE(owned_covers(session.loader, session.blocks[i].base, 1));
            CHECK(owned_covers(session.loader, session.blocks[i].base + stride(), session.blocks[i].size - stride()));
            CHECK(query(session.blocks[i].base).State == MEM_COMMIT);  // the thief's allocation
        }
    }
    SUBCASE("every one of 5 attempts collides: a clean CML4203 and nothing loaded") {
        grabber.grab_through_attempt = 5;
        const ipc::LoadPatch request = candidates_in_blocks(session, 5, "inj_b");
        const ipc::PatchLoaded answer = session.load(request);
        CHECK(answer.status == ids::kLoaderRangeTaken);
        CHECK(answer.attempts == 5);
        CHECK(answer.image_base == 0);
        CHECK(answer.message.find("5 load attempt") != std::string::npos);
        CHECK(answer.message.find("What to do") != std::string::npos);
        CHECK(grabber.grabbed.size() == 5);
        CHECK(grabber.attempts_seen == std::vector<std::uint32_t>{1, 2, 3, 4, 5});
        CHECK(session.loader.patch_count() == 0);
        for (unsigned i = 0; i < 5; ++i) {
            CHECK_FALSE(session.loader.is_loaded(session.blocks[i].base));
        }
        // The agent is still usable: the tails of the blocks take the next try.
        grabber.grab_through_attempt = 0;
        const ipc::PatchLoaded next = session.load(make_request(session.blocks[0].base + stride(), "inj_b_next", true, true));
        CHECK_MESSAGE(next.status == 0, next.message);
        CHECK(next.attempts == 1);
    }
    SUBCASE("a single candidate that collides reports CML4203 after one attempt; the caller retries elsewhere") {
        grabber.grab_through_attempt = 1;
        const ipc::LoadPatch first = candidates_in_blocks(session, 1, "inj_c");
        const ipc::PatchLoaded failed = session.load(first);
        CHECK(failed.status == ids::kLoaderRangeTaken);
        CHECK(failed.attempts == 1);
        grabber.grab_through_attempt = 0;  // the retry is not disturbed
        const ipc::LoadPatch second = candidates_in_blocks(session, 2, "inj_c2");
        ipc::LoadPatch only_second;
        only_second.candidates.push_back(second.candidates[1]);
        const ipc::PatchLoaded retried = session.load(only_second);
        CHECK_MESSAGE(retried.status == 0, retried.message);
        CHECK(retried.image_base == session.blocks[1].base);
    }
    SUBCASE("a candidate whose range was lost earlier is skipped (it counts as an attempt, never reaches the release)") {
        grabber.grab_through_attempt = 1;
        ipc::LoadPatch request = candidates_in_blocks(session, 3, "inj_d");
        request.candidates[1] = request.candidates[0];  // same range as the (now stolen) first one
        const ipc::PatchLoaded answer = session.load(request);
        REQUIRE_MESSAGE(answer.status == 0, answer.message);
        CHECK(answer.candidate_index == 2);
        CHECK(answer.attempts == 3);
        CHECK(grabber.attempts_seen == std::vector<std::uint32_t>{1, 3});  // candidate 1 never released anything
    }
    session.loader.set_test_hooks(TestHooks{});
}

TEST_CASE("failure injection: a thief thread that works concurrently with the release") {
    // Not deterministic by design: a background thread keeps trying to reserve inside the block
    // while patches are loaded. Whatever the interleaving, every answer is either a success at the
    // requested base or a clean CML4203, and nothing is left half-loaded.
    LoaderSession session(1, 8ull << 20);
    const proc::AddressRange block = session.blocks[0];
    std::atomic<bool> stop{false};
    std::vector<void*> stolen;
    std::thread thief([&] {
        std::uint64_t cursor = block.base;
        while (!stop.load()) {
            void* got = VirtualAlloc(reinterpret_cast<LPVOID>(cursor), 0x10000, MEM_RESERVE, PAGE_NOACCESS);
            if (got != nullptr) {
                stolen.push_back(got);
            }
            cursor += 0x10000;
            if (cursor >= block.end()) {
                cursor = block.base;
            }
            Sleep(0);  // yield: a thief that never lets go would make every load collide
        }
    });
    unsigned loaded = 0;
    unsigned taken = 0;
    unsigned other = 0;
    constexpr unsigned kTries = 40;
    for (unsigned i = 0; i < kTries; ++i) {
        const std::uint64_t base = block.base + i * stride();
        const ipc::PatchLoaded answer = session.load(make_request(base, "thief_" + std::to_string(i), false, false));
        if (answer.status == 0) {
            ++loaded;
            if (answer.image_base != base || !is_module_loaded_at(base)) {
                ++other;
            }
        } else if (answer.status == ids::kLoaderRangeTaken || answer.status == ids::kLoaderRangeNotReserved) {
            ++taken;
        } else {
            ++other;
        }
    }
    stop.store(true);
    thief.join();
    for (void* p : stolen) {
        VirtualFree(p, 0, MEM_RELEASE);
    }
    CHECK(other == 0);
    CHECK(loaded + taken == kTries);
    CHECK(session.loader.patch_count() == loaded);
    std::printf("WP08-RACE loaded=%u collided=%u\n", loaded, taken);
}

TEST_CASE("threads: concurrent loads from four threads into one block") {
    LoaderSession session(1, 16ull << 20);
    const std::uint64_t base0 = session.blocks[0].base;
    constexpr unsigned kThreads = 4;
    constexpr unsigned kPerThread = 25;
    std::vector<std::vector<ipc::LoadPatch>> requests(kThreads);
    for (unsigned t = 0; t < kThreads; ++t) {
        for (unsigned i = 0; i < kPerThread; ++i) {
            const unsigned index = t * kPerThread + i;
            requests[t].push_back(make_request(base0 + index * stride(), "thr_" + std::to_string(index)));
        }
    }
    std::vector<std::vector<ipc::PatchLoaded>> answers(kThreads);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (const ipc::LoadPatch& request : requests[t]) {
                answers[t].push_back(session.loader.load_patch(request));
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    unsigned ok = 0;
    for (unsigned t = 0; t < kThreads; ++t) {
        REQUIRE(answers[t].size() == kPerThread);
        for (unsigned i = 0; i < kPerThread; ++i) {
            const std::uint64_t base = base0 + (t * kPerThread + i) * stride();
            const ipc::PatchLoaded& a = answers[t][i];
            session.loaded.push_back(a.image_base);
            if (a.status == 0 && a.image_base == base && verify_standard(base, host_target_address()).empty() &&
                initializer_log(base) == "ABC") {
                ++ok;
            }
        }
    }
    CHECK(ok == kThreads * kPerThread);
    CHECK(session.loader.patch_count() == kThreads * kPerThread);
}

namespace {

// While LoadPatch is still working on a patch (it is busy), other requests for it are refused.
struct BusyProbe {
    PatchLoader* loader = nullptr;
    std::uint32_t unload_code = 0;
    std::uint32_t fixups_code = 0;
    std::uint32_t initializers_code = 0;
    int calls = 0;

    static void hook(void* context, std::uint64_t image_base) {
        auto* self = static_cast<BusyProbe*>(context);
        ++self->calls;
        const cml::Result<void> unload = self->loader->unload_patch(image_base);
        self->unload_code = unload ? 0 : unload.error().code;
        const cml::Result<std::uint32_t> fixups = self->loader->apply_fixups(image_base, std::vector<ipc::Fixup>{});
        self->fixups_code = fixups ? 0 : fixups.error().code;
        const cml::Result<std::uint32_t> inits =
            self->loader->run_initializers(image_base, std::vector<std::uint32_t>{});
        self->initializers_code = inits ? 0 : inits.error().code;
    }
};

}  // namespace

TEST_CASE("operations on a patch that is being loaded are refused with CML4212") {
    LoaderSession session(1);
    const std::uint64_t base = session.blocks[0].base;
    BusyProbe probe;
    probe.loader = &session.loader;
    TestHooks hooks;
    hooks.while_busy = &BusyProbe::hook;
    hooks.context = &probe;
    session.loader.set_test_hooks(hooks);
    const ipc::PatchLoaded answer = session.load(make_request(base, "busy", true, true));
    session.loader.set_test_hooks(TestHooks{});
    REQUIRE_MESSAGE(answer.status == 0, answer.message);
    CHECK(probe.calls == 1);
    CHECK(probe.unload_code == ids::kLoaderPatchBusy);
    CHECK(probe.fixups_code == ids::kLoaderPatchBusy);
    CHECK(probe.initializers_code == ids::kLoaderPatchBusy);
    // The refused requests changed nothing, and the patch is usable afterwards.
    CHECK(session.loader.is_loaded(base));
    CHECK(initializer_log(base) == "ABC");
    CHECK(session.loader.run_initializers(base, std::vector<std::uint32_t>{}).has_value());
}

TEST_CASE("staged requests for unknown or failed patches") {
    LoaderSession session(1);
    const std::uint64_t base = session.blocks[0].base;
    REQUIRE(session.load(make_request(base, "staged", true, false)).status == 0);
    // Unknown base.
    CHECK(session.loader.run_initializers(base + 0x1000000, std::vector<std::uint32_t>{}).error().code ==
          ids::kLoaderUnknownPatch);
    CHECK(session.loader.unload_patch(base + 0x1000000).error().code == ids::kLoaderUnknownPatch);
    // After a failed staged call the patch is gone (unloaded), so a second call sees it unknown.
    std::vector<std::uint32_t> bad = {proto().image_size + 8};
    const cml::Result<std::uint32_t> failed = session.loader.run_initializers(base, bad);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == ids::kLoaderInitializerFailed);
    CHECK_FALSE(session.loader.is_loaded(base));
    CHECK(session.loader.run_initializers(base, std::vector<std::uint32_t>{}).error().code == ids::kLoaderUnknownPatch);
    CHECK(query(base).State == MEM_RESERVE);
}

namespace {

void one_balance_cycle(unsigned cycle) {
    LoaderSession session(2);
    for (unsigned i = 0; i < 6; ++i) {
        const ipc::PatchLoaded answer = session.load(
            make_request(session.blocks[0].base + i * stride(), "bal_" + std::to_string(cycle) + "_" + std::to_string(i)));
        if (answer.status != 0) {
            FAIL("balance cycle: " << answer.message);
        }
    }
    // One failing load as well (fix-up out of range): exercises the failure cleanup path.
    ipc::LoadPatch bad = make_request(session.blocks[1].base, "bal_bad_" + std::to_string(cycle), true, false);
    bad.candidates[0].fixups[0].value = bad.candidates[0].image_base + (3ull << 30);
    if (session.load(bad).status != ids::kLoaderRel32OutOfRange) {
        FAIL("balance cycle: expected CML4207");
    }
}

}  // namespace

TEST_CASE("the agent's private heap balances across load, failure and unload cycles") {
    proto();  // first-use allocations
    one_balance_cycle(0);
    const std::size_t before = heap::live_blocks();
    one_balance_cycle(1);
    one_balance_cycle(2);
    const std::size_t after = heap::live_blocks();
    CHECK(after == before);
}

TEST_CASE("apply_fixups allocates nothing on its success path (usable while allocation is forbidden)") {
    LoaderSession session(1);
    const std::uint64_t base = session.blocks[0].base;
    REQUIRE(session.load(make_request(base, "noalloc", false, false)).status == 0);
    const std::vector<ipc::Fixup> fixups = standard_fixups(base, host_target_address());
    heap::set_alloc_forbidden(true);  // any allocation through the agent heap aborts the process
    const cml::Result<std::uint32_t> result =
        cml::agent::loader::apply_fixups(cml::agent::loader::ImageRef{base, proto().image_size}, fixups);
    const bool ok = result.has_value();
    heap::set_alloc_forbidden(false);
    CHECK(ok);
    const std::string problems = verify_standard(base, host_target_address());
    CHECK_MESSAGE(problems.empty(), problems);
}
