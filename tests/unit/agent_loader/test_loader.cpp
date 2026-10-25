// unit.agent_loader: the patch loader inside this test host process (WP-08).
//
// The "host module" patches are placed near is this very executable. Patches are copies of the
// prototype DLL (proto/patch_proto.cpp) rebased in the PE header; see proto/CMakeLists.txt.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "cml/proc/address_space.h"
#include "cml/proc/diag_ids.h"
#include "cml/proc/loader_diag_ids.h"
#include "doctest.h"
#include "loader/image_writer.h"
#include "loader/patch_loader.h"
#include "loader_test_support.h"

using namespace loadertest;
using cml::agent::loader::ImageRef;
using cml::agent::loader::PatchLoader;
using cml::agent::loader::ReserveParams;
using cml::agent::loader::TestHooks;
namespace ipc = cml::ipc;
namespace ids = cml::ids;
namespace proc = cml::proc;

#ifndef CML_TEST_DUMMY_MODULE
#error "CML_TEST_DUMMY_MODULE must name a harmless ordinary DLL"
#endif

namespace {

constexpr std::uint64_t kGranule = 0x10000;

bool owned_covers(const PatchLoader& loader, std::uint64_t base, std::uint64_t size) {
    for (const proc::AddressRange& r : loader.owned_ranges()) {
        if (r.base <= base && base + size <= r.end()) {
            return true;
        }
    }
    return false;
}

std::string hex_text(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::uint64_t read64(std::uint64_t address) {
    std::uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(address), 8);
    return v;
}

}  // namespace

TEST_CASE("scan and reserve blocks near the host module") {
    PatchLoader loader;
    ReserveParams params;
    params.module_base = host_base();
    params.module_size = host_size();
    params.block_size = 4ull << 20;
    params.block_count = 4;
    cml::Result<std::vector<proc::AddressRange>> reserved = loader.reserve_ranges(params);
    REQUIRE_MESSAGE(reserved.has_value(), (reserved.has_value() ? "" : reserved.error().format()));
    REQUIRE(reserved->size() == 4);

    const std::uint64_t module_end = host_base() + host_size();
    for (const proc::AddressRange& block : *reserved) {
        CHECK(block.size == (4ull << 20));
        CHECK(block.base % kGranule == 0);
        // Inside +-1.5 GiB of the module and within REL32 reach of every byte of it, both ways.
        CHECK((block.end() <= host_base() || block.base >= module_end));
        CHECK(block.end() <= module_end + (1536ull << 20));
        CHECK(block.base + (1536ull << 20) >= host_base());
        CHECK(block.end() <= host_base() + proc::kRel32Reach);
        CHECK(module_end <= block.base + proc::kRel32Reach);
        // It really is reserved, and nobody else can take it.
        const MEMORY_BASIC_INFORMATION info = query(block.base);
        CHECK(info.State == MEM_RESERVE);
        CHECK(info.RegionSize >= block.size);
        CHECK(VirtualAlloc(reinterpret_cast<LPVOID>(block.base), 4096, MEM_RESERVE, PAGE_NOACCESS) == nullptr);
        CHECK(owned_covers(loader, block.base, block.size));
    }
    // Distinct, non-overlapping.
    for (std::size_t i = 0; i < reserved->size(); ++i) {
        for (std::size_t j = i + 1; j < reserved->size(); ++j) {
            CHECK(((*reserved)[i].end() <= (*reserved)[j].base || (*reserved)[j].end() <= (*reserved)[i].base));
        }
    }
    // The spike (ADR-0020) found the first block directly adjacent to the image on an empty address
    // space; here it only has to be nearest-first.
    const auto distance = [&](const proc::AddressRange& b) {
        return b.end() <= host_base() ? host_base() - b.end() : b.base - module_end;
    };
    for (std::size_t i = 1; i < reserved->size(); ++i) {
        CHECK(distance((*reserved)[i]) + (16ull << 20) >= distance((*reserved)[i - 1]));
    }

    SUBCASE("release gives exactly that block back") {
        const proc::AddressRange first = (*reserved)[0];
        cml::Result<void> released = loader.release_range(first.base, first.size);
        REQUIRE_MESSAGE(released.has_value(), (released.has_value() ? "" : released.error().format()));
        CHECK_FALSE(owned_covers(loader, first.base, first.size));
        const MEMORY_BASIC_INFORMATION info = query(first.base);
        CHECK(info.State == MEM_FREE);
        // A second release, or a release of something never owned, is refused.
        cml::Result<void> again = loader.release_range(first.base, first.size);
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().code == ids::kLoaderReleaseFailed);
        // The other blocks are untouched.
        CHECK(query((*reserved)[1].base).State == MEM_RESERVE);
    }
    SUBCASE("releasing part of a block re-reserves the rest") {
        const proc::AddressRange first = (*reserved)[0];
        REQUIRE(loader.release_range(first.base + kGranule, 2 * kGranule).has_value());
        CHECK(query(first.base).State == MEM_RESERVE);                      // head
        CHECK(query(first.base + 3 * kGranule).State == MEM_RESERVE);       // tail
        CHECK(owned_covers(loader, first.base, kGranule));
        CHECK(owned_covers(loader, first.base + 3 * kGranule, first.size - 3 * kGranule));
        CHECK_FALSE(owned_covers(loader, first.base + kGranule, kGranule));
    }
    SUBCASE("invalid parameters") {
        ReserveParams bad = params;
        bad.module_size = 0;
        CHECK(loader.reserve_ranges(bad).error().code == ids::kLoaderInvalidRequest);
        bad = params;
        bad.block_count = 0;
        CHECK(loader.reserve_ranges(bad).error().code == ids::kLoaderInvalidRequest);
        bad = params;
        bad.block_size = 2ull << 30;
        CHECK(loader.reserve_ranges(bad).error().code == ids::kLoaderInvalidRequest);
        CHECK(loader.release_range(0x1234, 0x1000).error().code == ids::kLoaderInvalidRequest);
    }
}

TEST_CASE("destroying the loader returns its reservations to the OS") {
    std::vector<proc::AddressRange> blocks;
    {
        PatchLoader loader;
        ReserveParams params;
        params.module_base = host_base();
        params.module_size = host_size();
        params.block_size = 1ull << 20;
        params.block_count = 2;
        cml::Result<std::vector<proc::AddressRange>> reserved = loader.reserve_ranges(params);
        REQUIRE(reserved.has_value());
        blocks = *reserved;
        CHECK(query(blocks[0].base).State == MEM_RESERVE);
    }
    for (const proc::AddressRange& b : blocks) {
        CHECK(query(b.base).State != MEM_RESERVE);  // free (or reused by someone else, never ours)
    }
}

TEST_CASE("release and load: the patch lands exactly at its base inside the reserved block") {
    LoaderSession session(2);
    const proc::AddressRange block = session.blocks[0];
    const std::uint64_t first = block.base;
    const std::uint64_t second = block.base + stride();

    const ipc::PatchLoaded a = session.load(make_request(first, "load_a", false, false));
    REQUIRE_MESSAGE(a.status == 0, a.message);
    CHECK(a.image_base == first);
    CHECK(a.image_size == proto().image_size);
    CHECK(a.attempts == 1);
    CHECK(a.candidate_index == 0);
    CHECK(is_module_loaded_at(first));
    CHECK(session.loader.is_loaded(first));
    CHECK(session.loader.patch_count() == 1);
    // The rest of the block stayed reserved for the next patch.
    CHECK(query(second).State == MEM_RESERVE);
    CHECK(owned_covers(session.loader, second, block.end() - second));
    CHECK_FALSE(owned_covers(session.loader, first, 1));

    // Bump-allocate the next patch right behind it.
    const ipc::PatchLoaded b = session.load(make_request(second, "load_b", false, false));
    REQUIRE_MESSAGE(b.status == 0, b.message);
    CHECK(b.image_base == second);
    CHECK(is_module_loaded_at(second));
    CHECK(session.loader.patch_count() == 2);
    HMODULE module = nullptr;
    CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(second + 8), &module) != 0);
    CHECK(reinterpret_cast<std::uint64_t>(module) == second);
    // The loaded code is callable (a plain function, no fix-ups needed).
    CHECK(reinterpret_cast<int (*)()>(second + proto().rva_answer)() == 42);

    // Unloading puts the range back into the reserved state, available again.
    REQUIRE(session.loader.unload_patch(first).has_value());
    CHECK_FALSE(is_module_loaded_at(first));
    CHECK(query(first).State == MEM_RESERVE);
    CHECK(owned_covers(session.loader, first, stride()));
    CHECK(session.loader.patch_count() == 1);
    const ipc::PatchLoaded again = session.load(make_request(first, "load_a2", false, false));
    CHECK_MESSAGE(again.status == 0, again.message);
    // A range that is occupied cannot be loaded into twice.
    const ipc::PatchLoaded twice = session.load(make_request(first, "load_a3", false, false));
    CHECK(twice.status == ids::kLoaderRangeNotReserved);
}

TEST_CASE("fix-ups of every kind and thunk slots are written; protection is restored") {
    LoaderSession session(1);
    const std::uint64_t base = session.blocks[0].base;
    const ipc::PatchLoaded answer = session.load(make_request(base, "fixups", true, false));
    REQUIRE_MESSAGE(answer.status == 0, answer.message);
    CHECK(answer.fixups_applied == standard_fixups(base, host_target_address()).size());
    const std::string problems = verify_standard(base, host_target_address());
    CHECK_MESSAGE(problems.empty(), problems);
}

TEST_CASE("a REL32 out of range is rejected, the DLL is unloaded and the block is reusable") {
    LoaderSession session(1);
    const std::uint64_t base = session.blocks[0].base;
    ipc::LoadPatch request = make_request(base, "rel32_far", true, false);
    // 3 GiB away from the site: cannot be a 32-bit displacement.
    ipc::Fixup distant;
    distant.rva = proto().rva_data + 128;
    distant.kind = ipc::FixupKind::Rel32;
    distant.n = 0;
    distant.value = base + (3ull << 30);
    request.candidates[0].fixups.push_back(distant);

    const ipc::PatchLoaded answer = session.load(request);
    CHECK(answer.status == ids::kLoaderRel32OutOfRange);
    CHECK(answer.image_base == 0);
    CHECK(answer.message.find("REL32") != std::string::npos);
    CHECK(answer.message.find("What to do") != std::string::npos);
    CHECK_FALSE(is_module_loaded_at(base));
    CHECK(GetModuleHandleW(std::wstring(request.candidates[0].path.begin(), request.candidates[0].path.end()).c_str()) == nullptr);
    CHECK(session.loader.patch_count() == 0);
    CHECK(query(base).State == MEM_RESERVE);
    CHECK(owned_covers(session.loader, base, session.blocks[0].size));

    // Same base, valid fix-ups: loads fine.
    const ipc::PatchLoaded retry = session.load(make_request(base, "rel32_ok", true, false));
    CHECK_MESSAGE(retry.status == 0, retry.message);
}

TEST_CASE("fix-ups are validated before anything is written") {
    LoaderSession session(1);
    const std::uint64_t base = session.blocks[0].base;
    REQUIRE(session.load(make_request(base, "validate", false, false)).status == 0);
    const ImageRef image{base, proto().image_size};
    const std::uint64_t before = read64(base + proto().rva_data);

    ipc::Fixup good;
    good.rva = proto().rva_data;
    good.kind = ipc::FixupKind::Addr64;
    good.value = 0x0123456789ABCDEFull;

    SUBCASE("a distant REL32 later in the list blocks the earlier good write") {
        ipc::Fixup distant;
        distant.rva = proto().rva_data + 8;
        distant.kind = ipc::FixupKind::Rel32;
        distant.value = base + (4ull << 30);
        const std::vector<ipc::Fixup> list = {good, distant};
        const cml::Result<std::uint32_t> r = cml::agent::loader::apply_fixups(image, list);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ids::kLoaderRel32OutOfRange);
        CHECK(read64(base + proto().rva_data) == before);
    }
    SUBCASE("a site outside the image") {
        ipc::Fixup outside = good;
        outside.rva = proto().image_size - 4;  // an 8-byte write would run over the end
        const std::vector<ipc::Fixup> list = {good, outside};
        const cml::Result<std::uint32_t> r = cml::agent::loader::apply_fixups(image, list);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ids::kLoaderFixupInvalid);
        CHECK(read64(base + proto().rva_data) == before);
        outside.rva = proto().image_size + 0x1000;
        const std::vector<ipc::Fixup> list2 = {outside};
        CHECK(cml::agent::loader::apply_fixups(image, list2).error().code == ids::kLoaderFixupInvalid);
    }
    SUBCASE("a misaligned thunk slot") {
        ipc::Fixup slot;
        slot.rva = proto().rva_slot_a + 4;
        slot.kind = ipc::FixupKind::SlotWrite;
        slot.value = 1;
        const std::vector<ipc::Fixup> list = {good, slot};
        const cml::Result<std::uint32_t> r = cml::agent::loader::apply_fixups(image, list);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ids::kLoaderFixupInvalid);
        CHECK(read64(base + proto().rva_data) == before);
    }
    SUBCASE("a good list is applied, fix-ups before slots") {
        ipc::Fixup slot;
        slot.rva = proto().rva_slot_a;
        slot.kind = ipc::FixupKind::SlotWrite;
        slot.value = 0x77;
        const std::vector<ipc::Fixup> list = {slot, good};
        const cml::Result<std::uint32_t> r = cml::agent::loader::apply_fixups(image, list);
        REQUIRE(r.has_value());
        CHECK(*r == 2);
        CHECK(read64(base + proto().rva_data) == good.value);
        CHECK(read64(base + proto().rva_slot_a) == 0x77);
    }
    SUBCASE("empty image or empty list") {
        CHECK(cml::agent::loader::apply_fixups(ImageRef{}, std::vector<ipc::Fixup>{good}).error().code ==
              ids::kLoaderFixupInvalid);
        const std::vector<ipc::Fixup> none;
        const cml::Result<std::uint32_t> r = cml::agent::loader::apply_fixups(image, none);
        REQUIRE(r.has_value());
        CHECK(*r == 0);
    }
}

TEST_CASE("initializers run in the order the broker names, on the loading thread") {
    LoaderSession session(1);
    const std::uint64_t base = session.blocks[0].base;

    SUBCASE("inline with LoadPatch: A, B, C") {
        const ipc::PatchLoaded answer = session.load(make_request(base, "init_inline", true, true));
        REQUIRE_MESSAGE(answer.status == 0, answer.message);
        CHECK(answer.initializers_run == 3);
        CHECK(initializer_log(base) == "ABC");
    }
    SUBCASE("staged, in another order, with a null entry skipped") {
        REQUIRE(session.load(make_request(base, "init_staged", true, false)).status == 0);
        CHECK(initializer_log(base).empty());
        const std::vector<std::uint32_t> entries = init_entry_rvas();
        const std::vector<std::uint32_t> order = {entries[2], entries[0], entries[1]};
        cml::Result<std::uint32_t> ran = session.loader.run_initializers(base, order);
        REQUIRE_MESSAGE(ran.has_value(), (ran.has_value() ? "" : ran.error().format()));
        CHECK(*ran == 3);
        CHECK(initializer_log(base) == "CAB");

        // Zero the middle table entry: it is skipped, as the CRT skips null entries.
        ipc::Fixup zero;
        zero.rva = entries[1];
        zero.kind = ipc::FixupKind::Addr64;
        zero.value = 0;
        REQUIRE(session.loader.apply_fixups(base, std::vector<ipc::Fixup>{zero}).has_value());
        ran = session.loader.run_initializers(base, std::vector<std::uint32_t>{entries[0], entries[1], entries[2]});
        REQUIRE(ran.has_value());
        CHECK(*ran == 2);
        CHECK(initializer_log(base) == "CABAC");
    }
    SUBCASE("a faulting initializer fails the load and the patch is unloaded") {
        ipc::LoadPatch request = make_request(base, "init_fault", true, false);
        request.initializer_rvas = {init_entry_rvas()[0], fault_entry_rvas()[0]};
        const ipc::PatchLoaded answer = session.load(request);
        CHECK(answer.status == ids::kLoaderInitializerFailed);
        CHECK(answer.message.find("exception 0xc0000005") != std::string::npos);
        CHECK_FALSE(is_module_loaded_at(base));
        CHECK(session.loader.patch_count() == 0);
        CHECK(query(base).State == MEM_RESERVE);
    }
    SUBCASE("a table entry that points outside the patch is refused before anything runs") {
        REQUIRE(session.load(make_request(base, "init_outside", true, false)).status == 0);
        const std::vector<std::uint32_t> entries = init_entry_rvas();
        ipc::Fixup elsewhere;
        elsewhere.rva = entries[1];
        elsewhere.kind = ipc::FixupKind::Addr64;
        elsewhere.value = host_target_address();
        const std::vector<ipc::Fixup> list = {elsewhere};
        REQUIRE(cml::agent::loader::apply_fixups(ImageRef{base, proto().image_size}, list).has_value());
        const cml::Result<std::uint32_t> ran =
            cml::agent::loader::run_initializers(ImageRef{base, proto().image_size}, entries);
        REQUIRE_FALSE(ran.has_value());
        CHECK(ran.error().code == ids::kLoaderInitializerFailed);
        CHECK(initializer_log(base).empty());  // not even the valid first entry ran
    }
    SUBCASE("an RVA outside the image or misaligned") {
        REQUIRE(session.load(make_request(base, "init_rva", true, false)).status == 0);
        const ImageRef image{base, proto().image_size};
        CHECK(cml::agent::loader::run_initializers(image, std::vector<std::uint32_t>{proto().image_size}).error().code ==
              ids::kLoaderInitializerFailed);
        CHECK(cml::agent::loader::run_initializers(image, std::vector<std::uint32_t>{proto().rva_inits + 4}).error().code ==
              ids::kLoaderInitializerFailed);
        CHECK(cml::agent::loader::run_initializers(image, std::vector<std::uint32_t>{}).has_value());
    }
}

TEST_CASE("every failure unloads the DLL and returns the block to the reserved state") {
    LoaderSession session(1);
    const std::uint64_t base = session.blocks[0].base;
    const auto expect_clean = [&](const ipc::PatchLoaded& answer, std::uint32_t code) {
        CHECK_MESSAGE(answer.status == code, answer.message);
        CHECK(answer.image_base == 0);
        CHECK_FALSE(answer.message.empty());
        CHECK(session.loader.patch_count() == 0);
        CHECK(query(base).State == MEM_RESERVE);
        CHECK(owned_covers(session.loader, base, session.blocks[0].size));
    };

    SUBCASE("a DLL that is not linked at the announced base (an ordinary ASLR DLL) is refused before anything is released") {
        ipc::LoadPatch request;
        request.request_id = 1;
        ipc::PatchCandidate candidate;
        candidate.path = CML_TEST_DUMMY_MODULE;
        candidate.image_base = base;
        candidate.image_size = 0x4000;
        request.candidates.push_back(candidate);
        const ipc::PatchLoaded answer = session.loader.load_patch(request);
        expect_clean(answer, ids::kLoaderImageMismatch);
        CHECK(answer.attempts == 0);
        CHECK(answer.message.find("ImageBase") != std::string::npos);
        std::wstring wide(candidate.path.begin(), candidate.path.end());
        CHECK(GetModuleHandleW(wide.c_str()) == nullptr);  // never loaded
    }
    SUBCASE("the announced SizeOfImage is wrong: CML4206 from the file header, nothing released, no retry") {
        struct Count {
            int releases = 0;
            static void hook(void* context, std::uint64_t, std::uint64_t, std::uint32_t) {
                ++static_cast<Count*>(context)->releases;
            }
        } count;
        TestHooks hooks;
        hooks.after_release = &Count::hook;
        hooks.context = &count;
        session.loader.set_test_hooks(hooks);
        for (const std::int64_t delta : {-0x1000ll, +0x1000ll, +0x10000ll}) {
            ipc::LoadPatch request = make_request(base, "wrong_size" + std::to_string(delta), false, false);
            request.candidates[0].image_size = static_cast<std::uint32_t>(proto().image_size + delta);
            const ipc::PatchLoaded answer = session.loader.load_patch(request);
            expect_clean(answer, ids::kLoaderImageMismatch);
            CHECK(answer.attempts == 0);
            CHECK(answer.message.find(hex_text(proto().image_size)) != std::string::npos);  // what the file says
            CHECK(answer.message.find(hex_text(request.candidates[0].image_size)) != std::string::npos);  // what was announced
        }
        CHECK(count.releases == 0);
        // With several candidates a bad one stops the request: a wrong announcement is not retried.
        ipc::LoadPatch two = make_request(base, "wrong_two_a", false, false);
        ipc::LoadPatch second = make_request(base + stride(), "wrong_two_b", false, false);
        second.candidates[0].image_size += 0x1000;
        two.candidates.push_back(second.candidates[0]);
        CHECK(session.loader.load_patch(two).status == ids::kLoaderImageMismatch);
        CHECK(count.releases == 0);
        session.loader.set_test_hooks(TestHooks{});
        // The correct size loads.
        CHECK(session.load(make_request(base, "right_size", false, false)).status == 0);
    }
    SUBCASE("the file does not exist") {
        ipc::LoadPatch request = make_request(base, "gone", false, false);
        request.candidates[0].path = work_dir() + "\\does_not_exist.dll";
        expect_clean(session.loader.load_patch(request), ids::kLoaderLoadFailed);
    }
    SUBCASE("the file is not a PE image") {
        const std::string path = work_dir() + "\\garbage.dll";
        {
            std::ofstream out(path, std::ios::binary);
            out << "this is not a portable executable";
        }
        ipc::LoadPatch request = make_request(base, "garbage_src", false, false);
        request.candidates[0].path = path;
        expect_clean(session.loader.load_patch(request), ids::kLoaderLoadFailed);
    }
    SUBCASE("the image range is not inside address space the agent reserved") {
        ipc::LoadPatch request = make_request(0x5500000000ull, "not_reserved", false, false);
        const ipc::PatchLoaded answer = session.loader.load_patch(request);
        CHECK(answer.status == ids::kLoaderRangeNotReserved);
        CHECK(answer.attempts == 1);
        // A range sticking out of the end of the block.
        ipc::LoadPatch edge = make_request(session.blocks[0].end() - kGranule, "edge", false, false);
        edge.candidates[0].image_size = 0x20000;
        CHECK(session.loader.load_patch(edge).status == ids::kLoaderRangeNotReserved);
    }
    SUBCASE("fix-ups that fail after a successful load") {
        ipc::LoadPatch request = make_request(base, "late_fixup", true, false);
        request.candidates[0].fixups[0].rva = proto().image_size + 8;  // outside the image
        expect_clean(session.loader.load_patch(request), ids::kLoaderFixupInvalid);
    }
    SUBCASE("invalid requests are refused without touching anything") {
        ipc::LoadPatch request = make_request(base, "invalid", false, false);
        ipc::LoadPatch no_candidates = request;
        no_candidates.candidates.clear();
        CHECK(session.loader.load_patch(no_candidates).status == ids::kLoaderInvalidRequest);
        ipc::LoadPatch six = request;
        while (six.candidates.size() < 6) {
            six.candidates.push_back(request.candidates[0]);
        }
        CHECK(session.loader.load_patch(six).status == ids::kLoaderInvalidRequest);
        ipc::LoadPatch misaligned = request;
        misaligned.candidates[0].image_base += 0x1000;
        CHECK(session.loader.load_patch(misaligned).status == ids::kLoaderInvalidRequest);
        ipc::LoadPatch zero_size = request;
        zero_size.candidates[0].image_size = 0;
        CHECK(session.loader.load_patch(zero_size).status == ids::kLoaderInvalidRequest);
        ipc::LoadPatch no_path = request;
        no_path.candidates[0].path.clear();
        CHECK(session.loader.load_patch(no_path).status == ids::kLoaderInvalidRequest);
        CHECK(query(base).State == MEM_RESERVE);
    }
}

TEST_CASE("a patch linked at its real /FIXED /BASE loads there, and its own initializer table needs no fix-ups") {
    PatchLoader loader;
    const ProtoLayout& l = proto();
    cml::Result<void> reserved = loader.reserve_exact(l.proto_base, 1ull << 20);
    if (!reserved) {
        WARN_MESSAGE(false, "the linker's /BASE " << l.proto_base << " is occupied in this process; skipped");
        return;
    }
    ipc::LoadPatch request;
    request.request_id = 1;
    ipc::PatchCandidate candidate;
    candidate.path = proto_path();
    candidate.image_base = l.proto_base;
    candidate.image_size = l.image_size;
    request.candidates.push_back(candidate);
    request.initializer_rvas = init_entry_rvas();
    const ipc::PatchLoaded answer = loader.load_patch(request);
    REQUIRE_MESSAGE(answer.status == 0, answer.message);
    CHECK(answer.image_base == l.proto_base);
    CHECK(initializer_log(l.proto_base) == "ABC");
    CHECK(reinterpret_cast<int (*)()>(l.proto_base + l.rva_answer)() == 42);
    REQUIRE(loader.unload_patch(l.proto_base).has_value());
}

TEST_CASE("the broker-facing dispatcher answers every request type") {
    PatchLoader loader;
    ipc::ReserveRange reserve;
    reserve.request_id = 1;
    reserve.module_base = host_base();
    reserve.module_size = host_size();
    reserve.block_size = 1ull << 20;
    reserve.block_count = 2;
    CHECK(PatchLoader::is_loader_request(reserve));
    ipc::Message answer = loader.handle_request(reserve);
    REQUIRE(std::holds_alternative<ipc::ReserveRangeResult>(answer));
    const auto reserved = std::get<ipc::ReserveRangeResult>(answer);
    CHECK(reserved.request_id == 1);
    REQUIRE_MESSAGE(reserved.status == 0, reserved.message);
    REQUIRE(reserved.blocks.size() == 2);
    CHECK_FALSE(PatchLoader::is_loader_request(reserved));  // an answer is not a request
    CHECK_FALSE(PatchLoader::is_loader_request(ipc::Reply{}));
    CHECK_FALSE(PatchLoader::is_loader_request(ipc::PatchLoaded{}));

    const std::uint64_t base = reserved.blocks[0].base;
    ipc::LoadPatch load = make_request(base, "dispatch", false, false);
    CHECK(PatchLoader::is_loader_request(load));
    answer = loader.handle_request(load);
    REQUIRE(std::holds_alternative<ipc::PatchLoaded>(answer));
    REQUIRE_MESSAGE(std::get<ipc::PatchLoaded>(answer).status == 0, std::get<ipc::PatchLoaded>(answer).message);
    CHECK(std::get<ipc::PatchLoaded>(answer).request_id == load.request_id);
    CHECK(std::get<ipc::PatchLoaded>(answer).patch_id == load.patch_id);

    ipc::ApplyFixups fixups;
    fixups.request_id = 7;
    fixups.image_base = base;
    fixups.fixups = standard_fixups(base, host_target_address());
    answer = loader.handle_request(fixups);
    REQUIRE(std::holds_alternative<ipc::Reply>(answer));
    CHECK_MESSAGE(std::get<ipc::Reply>(answer).status == 0, std::get<ipc::Reply>(answer).message);
    CHECK(std::get<ipc::Reply>(answer).request_id == 7);
    const std::string problems = verify_standard(base, host_target_address());
    CHECK_MESSAGE(problems.empty(), problems);

    ipc::RunInitializers inits;
    inits.request_id = 8;
    inits.image_base = base;
    inits.rvas = init_entry_rvas();
    answer = loader.handle_request(inits);
    REQUIRE(std::holds_alternative<ipc::Reply>(answer));
    CHECK(std::get<ipc::Reply>(answer).status == 0);
    CHECK(initializer_log(base) == "ABC");

    // Errors travel as Reply with the CML42xx id.
    fixups.image_base = base + 0x10000000;
    answer = loader.handle_request(fixups);
    REQUIRE(std::holds_alternative<ipc::Reply>(answer));
    CHECK(std::get<ipc::Reply>(answer).status == ids::kLoaderUnknownPatch);
    CHECK(std::get<ipc::Reply>(answer).request_id == 7);
    CHECK(std::get<ipc::Reply>(answer).message.find("What to do") != std::string::npos);

    ipc::ReleaseRange release;
    release.request_id = 9;
    release.base = reserved.blocks[1].base;
    release.size = reserved.blocks[1].size;
    release.mode = ipc::ReleaseMode::Free;
    answer = loader.handle_request(release);
    CHECK(std::get<ipc::Reply>(answer).status == 0);
    answer = loader.handle_request(release);  // already gone
    CHECK(std::get<ipc::Reply>(answer).status == ids::kLoaderReleaseFailed);

    release.mode = ipc::ReleaseMode::UnloadPatch;
    release.base = base + 0x10000000;
    answer = loader.handle_request(release);
    CHECK(std::get<ipc::Reply>(answer).status == ids::kLoaderUnknownPatch);
    release.base = base;
    answer = loader.handle_request(release);
    CHECK_MESSAGE(std::get<ipc::Reply>(answer).status == 0, std::get<ipc::Reply>(answer).message);
    CHECK_FALSE(is_module_loaded_at(base));
    CHECK(query(base).State == MEM_RESERVE);

    // Bad ReserveRange parameters are answered with a ReserveRangeResult carrying the status.
    reserve.module_size = 0;
    answer = loader.handle_request(reserve);
    REQUIRE(std::holds_alternative<ipc::ReserveRangeResult>(answer));
    CHECK(std::get<ipc::ReserveRangeResult>(answer).status == ids::kLoaderInvalidRequest);
    CHECK(std::get<ipc::ReserveRangeResult>(answer).blocks.empty());

    // Anything else is refused with CML5203.
    answer = loader.handle_request(ipc::Hello{});
    REQUIRE(std::holds_alternative<ipc::Reply>(answer));
    CHECK(std::get<ipc::Reply>(answer).status == ids::kMessageTypeUnsupported);

    // The answers survive the wire.
    for (const ipc::Message& m : {ipc::Message(reserved), answer}) {
        cml::Result<std::vector<std::uint8_t>> frame = ipc::encode_frame(m);
        REQUIRE(frame.has_value());
        CHECK(ipc::decode_frame(frame->data(), frame->size()).has_value());
    }
}
