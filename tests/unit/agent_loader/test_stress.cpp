// unit.agent_loader: stress, failure injection (the release/LoadLibrary race), threads and the
// private-heap rules (WP-08).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "cml/proc/address_space.h"
#include "cml/proc/loader_diag_ids.h"
#include "doctest.h"
#include "cml/proc/proc.h"
#include "heap.h"
#include "module_diff.h"
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
        CHECK(answer.message.find("5 collided") != std::string::npos);
        CHECK(answer.message.find("0 not reserved") != std::string::npos);
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

TEST_CASE("staged requests for unknown patches; a failing staged request never unloads a live patch") {
    LoaderSession session(1);
    const std::uint64_t base = session.blocks[0].base;
    REQUIRE(session.load(make_request(base, "staged", true, true)).status == 0);  // live from here on
    // Unknown base.
    CHECK(session.loader.run_initializers(base + 0x1000000, std::vector<std::uint32_t>{}).error().code ==
          ids::kLoaderUnknownPatch);
    CHECK(session.loader.unload_patch(base + 0x1000000).error().code == ids::kLoaderUnknownPatch);

    // Everything the patch contains right now.
    const ProtoLayout& l = proto();
    const auto snapshot = [&] {
        std::vector<unsigned char> bytes(l.image_size - l.rva_rodata);
        std::memcpy(bytes.data(), reinterpret_cast<const void*>(base + l.rva_rodata), bytes.size());
        return bytes;
    };
    const std::vector<unsigned char> before = snapshot();
    const std::string log_before = initializer_log(base);

    // A bad initializer RVA.
    std::vector<std::uint32_t> bad = {proto().image_size + 8};
    const cml::Result<std::uint32_t> failed = session.loader.run_initializers(base, bad);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == ids::kLoaderInitializerFailed);
    // A REL32 that cannot reach, after a good entry in the same list.
    ipc::Fixup good;
    good.rva = l.rva_data + 64;
    good.kind = ipc::FixupKind::Addr64;
    good.value = 0x0102030405060708ull;
    ipc::Fixup distant;
    distant.rva = l.rva_data + 8;
    distant.kind = ipc::FixupKind::Rel32;
    distant.value = base + (5ull << 30);
    const cml::Result<std::uint32_t> unreachable =
        session.loader.apply_fixups(base, std::vector<ipc::Fixup>{good, distant});
    REQUIRE_FALSE(unreachable.has_value());
    CHECK(unreachable.error().code == ids::kLoaderRel32OutOfRange);
    // A site outside the image.
    ipc::Fixup outside = good;
    outside.rva = l.image_size + 0x2000;
    CHECK(session.loader.apply_fixups(base, std::vector<ipc::Fixup>{outside}).error().code == ids::kLoaderFixupInvalid);
    // The same refusals through the broker-facing dispatcher.
    ipc::ApplyFixups message;
    message.request_id = 3;
    message.image_base = base;
    message.fixups = {good, distant};
    const ipc::Message answer = session.loader.handle_request(message);
    REQUIRE(std::holds_alternative<ipc::Reply>(answer));
    CHECK(std::get<ipc::Reply>(answer).status == ids::kLoaderRel32OutOfRange);
    // A faulting initializer in a staged request is reported, the patch stays.
    const cml::Result<std::uint32_t> faulted = session.loader.run_initializers(base, fault_entry_rvas());
    REQUIRE_FALSE(faulted.has_value());
    CHECK(faulted.error().code == ids::kLoaderInitializerFailed);

    // Loaded, callable and unchanged.
    CHECK(session.loader.is_loaded(base));
    CHECK(is_module_loaded_at(base));
    CHECK(session.loader.patch_count() == 1);
    CHECK(reinterpret_cast<int (*)()>(base + l.rva_answer)() == 42);
    CHECK(initializer_log(base) == log_before);
    CHECK(snapshot() == before);
    const std::string problems = verify_standard(base, host_target_address());
    CHECK_MESSAGE(problems.empty(), problems);
    // Still addressable afterwards, and only an explicit unload removes it.
    CHECK(session.loader.apply_fixups(base, std::vector<ipc::Fixup>{good}).has_value());
    ipc::ReleaseRange release;
    release.request_id = 4;
    release.base = base;
    release.mode = ipc::ReleaseMode::UnloadPatch;
    const ipc::Message unloaded = session.loader.handle_request(release);
    CHECK(std::get<ipc::Reply>(unloaded).status == 0);
    CHECK_FALSE(session.loader.is_loaded(base));
    CHECK(query(base).State == MEM_RESERVE);
}

TEST_CASE("unload verifies that the image is really gone") {
    LoaderSession session(1);
    const std::uint64_t base = session.blocks[0].base;
    const ipc::LoadPatch request = make_request(base, "refcount", false, false);
    REQUIRE(session.load(request).status == 0);
    const std::wstring wide(request.candidates[0].path.begin(), request.candidates[0].path.end());
    HMODULE extra = LoadLibraryW(wide.c_str());  // a second reference, held by "someone else"
    REQUIRE(reinterpret_cast<std::uint64_t>(extra) == base);

    const cml::Result<void> refused = session.loader.unload_patch(base);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ids::kLoaderUnloadFailed);
    CHECK(refused.error().message.find("still referenced elsewhere") != std::string::npos);
    CHECK(session.loader.is_loaded(base));       // the record is kept
    CHECK(is_module_loaded_at(base));            // and so is the image
    CHECK_FALSE(owned_covers(session.loader, base, 1));  // the range is not given back as reserved
    CHECK(query(base).State == MEM_COMMIT);

    // The other reference goes away: the retry completes the unload without another FreeLibrary.
    REQUIRE(FreeLibrary(extra));
    const cml::Result<void> done = session.loader.unload_patch(base);
    REQUIRE_MESSAGE(done.has_value(), (done.has_value() ? "" : done.error().format()));
    CHECK_FALSE(session.loader.is_loaded(base));
    CHECK(query(base).State == MEM_RESERVE);
    CHECK(owned_covers(session.loader, base, stride()));
}

TEST_CASE("module snapshots leave patch images out, so no ModuleLoaded/ModuleUnloaded is ever sent for them") {
    LoaderSession session(1);
    const std::uint64_t base = session.blocks[0].base;
    const auto entries = [](const std::vector<proc::ModuleInfo>& modules) {
        std::vector<ipc::ModuleEntry> list;
        for (const proc::ModuleInfo& m : modules) {
            list.push_back({m.base, m.size, m.path});
        }
        return list;
    };
    const auto contains = [](const std::vector<proc::ModuleInfo>& modules, std::uint64_t module_base) {
        for (const proc::ModuleInfo& m : modules) {
            if (m.base == module_base) {
                return true;
            }
        }
        return false;
    };
    // The agent's poll compares consecutive snapshots with diff_modules.
    const auto filtered_before = session.loader.enumerate_modules_without_patches();
    const auto raw_before = proc::enumerate_modules(0);
    REQUIRE(filtered_before.has_value());
    REQUIRE(raw_before.has_value());

    REQUIRE(session.load(make_request(base, "poll", true, true)).status == 0);
    CHECK(session.loader.is_patch_image(base));
    CHECK_FALSE(session.loader.is_patch_image(base + 0x10000));

    const auto filtered_after = session.loader.enumerate_modules_without_patches();
    const auto raw_after = proc::enumerate_modules(0);
    REQUIRE(filtered_after.has_value());
    REQUIRE(raw_after.has_value());
    CHECK(contains(*raw_after, base));  // control: the toolhelp snapshot does see the patch
    CHECK_FALSE(contains(*filtered_after, base));
    const cml::agent::ModuleDelta without = cml::agent::diff_modules(entries(*filtered_before), entries(*filtered_after));
    const cml::agent::ModuleDelta with = cml::agent::diff_modules(entries(*raw_before), entries(*raw_after));
    const auto mentions = [&](const std::vector<ipc::ModuleEntry>& list) {
        for (const ipc::ModuleEntry& m : list) {
            if (m.base == base) {
                return true;
            }
        }
        return false;
    };
    CHECK_FALSE(mentions(without.loaded));
    CHECK_FALSE(mentions(without.unloaded));
    CHECK(mentions(with.loaded));  // what the agent would have reported without the filter

    // Unloading is silent too.
    REQUIRE(session.loader.unload_patch(base).has_value());
    const auto filtered_gone = session.loader.enumerate_modules_without_patches();
    REQUIRE(filtered_gone.has_value());
    const cml::agent::ModuleDelta gone = cml::agent::diff_modules(entries(*filtered_after), entries(*filtered_gone));
    CHECK_FALSE(mentions(gone.loaded));
    CHECK_FALSE(mentions(gone.unloaded));
    // Other modules are untouched by the filter.
    CHECK(contains(*filtered_gone, host_base()));
}

TEST_CASE("fault classification: stack overflow and C++ exceptions are named") {
    using cml::agent::loader::classify_fault;
    using cml::agent::loader::describe_fault;
    using cml::agent::loader::FaultKind;
    CHECK(classify_fault(0xC00000FD) == FaultKind::StackOverflow);
    CHECK(classify_fault(0xE06D7363) == FaultKind::CppException);
    CHECK(classify_fault(0xC0000005) == FaultKind::Other);
    CHECK(classify_fault(0x80000003) == FaultKind::Other);
    CHECK(describe_fault(0xC00000FD, true).find("overflowed the stack") != std::string::npos);
    CHECK(describe_fault(0xC00000FD, true).find("restored") != std::string::npos);
    CHECK(describe_fault(0xC00000FD, true).find("could not be restored") == std::string::npos);
    CHECK(describe_fault(0xC00000FD, false).find("could not be restored") != std::string::npos);
    CHECK(describe_fault(0xC00000FD, false).find("refuses") != std::string::npos);
    CHECK(describe_fault(0xE06D7363, true).find("C++ exception") != std::string::npos);
    CHECK(describe_fault(0xE06D7363, true).find("0xe06d7363") != std::string::npos);
    CHECK(describe_fault(0xC0000005, true).find("raised exception 0xc0000005") != std::string::npos);
    CHECK_FALSE(cml::agent::loader::initializers_refused_on_this_thread());
}

TEST_CASE("an initializer that overflows the stack is caught, the guard page restored, the patch unloaded") {
    LoaderSession session(1);
    const std::uint64_t base = session.blocks[0].base;
    ipc::LoadPatch overflow = make_request(base, "overflow", true, false);
    overflow.initializer_rvas = overflow_entry_rvas();
    ipc::LoadPatch normal = make_request(base, "after_overflow", true, true);

    // On a thread of its own: its stack is the one that overflows, and the test host's main thread is
    // never at risk. The same thread then runs a normal load, which proves the guard page is back.
    ipc::PatchLoaded first;
    ipc::PatchLoaded second;
    bool refused_afterwards = true;
    std::thread worker([&] {
        first = session.loader.load_patch(overflow);
        refused_afterwards = cml::agent::loader::initializers_refused_on_this_thread();
        second = session.loader.load_patch(normal);
    });
    worker.join();
    CHECK(first.status == ids::kLoaderInitializerFailed);
    CHECK(first.message.find("overflowed the stack") != std::string::npos);
    CHECK(first.message.find("guard page was restored") != std::string::npos);
    CHECK_FALSE(refused_afterwards);
    CHECK_MESSAGE(second.status == 0, second.message);
    session.loaded.push_back(second.image_base);
    CHECK(initializer_log(base) == "ABC");
}

TEST_CASE("the final CML4203 reports the breakdown; without a collision the real reason is returned") {
    LoaderSession session(3);
    Grabber grabber;
    grabber.install(session.loader);

    SUBCASE("a collision and a skipped candidate") {
        grabber.grab_through_attempt = 1;
        ipc::LoadPatch request = candidates_in_blocks(session, 1, "bd_a");
        request.candidates.push_back(request.candidates[0]);  // same range: lost to the thief, then not reserved
        const ipc::PatchLoaded answer = session.load(request);
        CHECK(answer.status == ids::kLoaderRangeTaken);
        CHECK(answer.attempts == 2);
        CHECK(answer.message.find("1 collided") != std::string::npos);
        CHECK(answer.message.find("1 not reserved") != std::string::npos);
        CHECK(answer.message.find("0 other") != std::string::npos);
    }
    SUBCASE("no attempt collided: the last non-collision diagnostic, not CML4203") {
        grabber.grab_through_attempt = 0;
        ipc::LoadPatch request = make_request(0x5500000000ull, "bd_b0", false, false);
        const ipc::LoadPatch other = make_request(0x5600000000ull, "bd_b1", false, false);
        request.candidates.push_back(other.candidates[0]);
        const ipc::PatchLoaded answer = session.load(request);
        CHECK(answer.status == ids::kLoaderRangeNotReserved);
        CHECK(answer.attempts == 2);
        CHECK(answer.message.find("0x5600000000") != std::string::npos);  // the last candidate's reason
        CHECK(grabber.attempts_seen.empty());
    }
    SUBCASE("a failure after a candidate was placed keeps its own code") {
        grabber.grab_through_attempt = 1;
        ipc::LoadPatch request = candidates_in_blocks(session, 2, "bd_c");
        request.candidates[1].fixups[0].rva = proto().image_size + 8;  // fails after its load
        const ipc::PatchLoaded answer = session.load(request);
        CHECK(answer.status == ids::kLoaderFixupInvalid);
    }
    session.loader.set_test_hooks(TestHooks{});
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
