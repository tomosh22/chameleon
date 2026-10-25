#include "loader_test_support.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

#include "cml/pe/pe.h"
#include "doctest.h"

#ifndef CML_TEST_PATCH_PROTO
#error "CML_TEST_PATCH_PROTO must name the prototype patch DLL"
#endif

// The function patches call through a REL32. noinline + extern "C": a distinct, stable address.
extern "C" __declspec(noinline) int cml_test_host_target() {
    return loadertest::kHostTargetResult;
}

namespace loadertest {

namespace {

std::vector<std::byte>& proto_bytes() {
    static std::vector<std::byte> bytes;
    return bytes;
}

std::uint32_t export_rva(const cml::pe::ExportTable& exports, const char* name) {
    const cml::pe::Export* e = exports.find_by_name(name);
    REQUIRE_MESSAGE(e != nullptr, (std::string("prototype DLL does not export ") + name));
    return e->rva;
}

ProtoLayout build_layout() {
    cml::Result<cml::pe::OwnedImage> loaded = cml::pe::load_image_file(proto_path());
    REQUIRE_MESSAGE(loaded.has_value(), (loaded.has_value() ? "" : loaded.error().format()));
    const cml::pe::Image& image = loaded->image;
    proto_bytes() = loaded->bytes;
    cml::Result<cml::pe::ExportTable> exports = image.exports();
    REQUIRE(exports.has_value());

    ProtoLayout l;
    l.proto_base = image.optional_header().image_base;
    l.image_size = image.optional_header().size_of_image;
    l.rva_code_call = export_rva(*exports, "cml_code_call");
    l.rva_data = export_rva(*exports, "cml_data");
    l.rva_rodata = export_rva(*exports, "cml_rodata");
    l.rva_slot_a = export_rva(*exports, "cml_slot_a");
    l.rva_slot_b = export_rva(*exports, "cml_slot_b");
    l.rva_inits = export_rva(*exports, "cml_inits");
    l.rva_init_fault = export_rva(*exports, "cml_init_fault");
    l.rva_init_overflow = export_rva(*exports, "cml_init_overflow");
    l.rva_log = export_rva(*exports, "cml_log");
    l.rva_log_len = export_rva(*exports, "cml_log_len");
    l.rva_answer = export_rva(*exports, "cml_patch_answer");

    // The init tables hold absolute pointers baked for the prototype's own base.
    const auto read_pointer = [&](std::uint32_t rva) {
        cml::Result<cml::pe::ByteSpan> bytes = image.read_rva(rva, 8);
        REQUIRE(bytes.has_value());
        std::uint64_t value = 0;
        std::memcpy(&value, bytes->data(), 8);
        REQUIRE(value > l.proto_base);
        return static_cast<std::uint32_t>(value - l.proto_base);
    };
    for (std::uint32_t i = 0; i < 3; ++i) {
        l.init_fn_rva[i] = read_pointer(l.rva_inits + 8 * i);
    }
    l.fault_fn_rva = read_pointer(l.rva_init_fault);
    l.overflow_fn_rva = read_pointer(l.rva_init_overflow);
    return l;
}

}  // namespace

std::string proto_path() {
    return CML_TEST_PATCH_PROTO;
}

const ProtoLayout& proto() {
    static const ProtoLayout layout = build_layout();
    return layout;
}

std::string work_dir() {
    static const std::string dir = [] {
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string path = exe;
        path.resize(path.find_last_of("\\/") + 1);  // the exe's directory, with the separator
        // <build>/bin/ -> <build>/test_work/agent_loader/<pid>/
        path += "..\\test_work";
        CreateDirectoryA(path.c_str(), nullptr);
        path += "\\agent_loader";
        CreateDirectoryA(path.c_str(), nullptr);
        path += "\\" + std::to_string(GetCurrentProcessId());
        CreateDirectoryA(path.c_str(), nullptr);
        return path;
    }();
    return dir;
}

void remove_work_dir() {
    const std::string dir = work_dir();
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA((dir + "\\*.dll").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileA((dir + "\\" + data.cFileName).c_str());  // fails for DLLs still mapped; fine
        } while (FindNextFileA(find, &data));
        FindClose(find);
    }
    RemoveDirectoryA(dir.c_str());
}

std::string make_variant(std::uint64_t base, const std::string& tag) {
    const ProtoLayout& l = proto();
    REQUIRE(base % 0x10000 == 0);
    std::vector<std::byte> bytes = proto_bytes();
    IMAGE_DOS_HEADER dos;
    std::memcpy(&dos, bytes.data(), sizeof(dos));
    REQUIRE(dos.e_magic == IMAGE_DOS_SIGNATURE);
    IMAGE_NT_HEADERS64 nt;
    std::memcpy(&nt, bytes.data() + dos.e_lfanew, sizeof(nt));
    REQUIRE(nt.OptionalHeader.ImageBase == l.proto_base);
    nt.OptionalHeader.ImageBase = base;
    nt.OptionalHeader.DllCharacteristics &= static_cast<WORD>(~IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
    nt.OptionalHeader.CheckSum = 0;
    std::memcpy(bytes.data() + dos.e_lfanew, &nt, sizeof(nt));

    const std::string path = work_dir() + "\\" + tag + ".dll";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE_MESSAGE(out.good(), ("cannot create " + path));
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    REQUIRE_MESSAGE(out.good(), ("cannot write " + path));
    return path;
}

std::uint64_t host_base() {
    return reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
}

std::uint64_t host_size() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(host_base());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(host_base() + static_cast<std::uint64_t>(dos->e_lfanew));
    return nt->OptionalHeader.SizeOfImage;
}

std::uint64_t host_target_address() {
    return reinterpret_cast<std::uint64_t>(&cml_test_host_target);
}

std::uint64_t stride() {
    return (static_cast<std::uint64_t>(proto().image_size) + 0xFFFF) / 0x10000 * 0x10000;
}

namespace {

cml::ipc::Fixup fixup(std::uint32_t rva, cml::ipc::FixupKind kind, std::uint8_t n, std::uint64_t value) {
    cml::ipc::Fixup f;
    f.rva = rva;
    f.kind = kind;
    f.n = n;
    f.value = value;
    return f;
}

}  // namespace

std::vector<cml::ipc::Fixup> standard_fixups(std::uint64_t base, std::uint64_t target) {
    using cml::ipc::FixupKind;
    const ProtoLayout& l = proto();
    std::vector<cml::ipc::Fixup> list;
    list.push_back(fixup(l.rva_code_call + 5, FixupKind::Rel32, 0, target));
    for (std::uint8_t n = 1; n <= 5; ++n) {
        list.push_back(fixup(l.rva_data + 8u * (n - 1u), FixupKind::Rel32, n, target + 0x100u * n));
    }
    list.push_back(fixup(l.rva_data + 64, FixupKind::Addr64, 0, 0x1122334455667788ull));
    list.push_back(fixup(l.rva_data + 73, FixupKind::Addr64, 0, 0xAABBCCDD00112233ull));  // unaligned
    list.push_back(fixup(l.rva_rodata + 8, FixupKind::Addr64, 0, 0xDEADBEEF0BADF00Dull));  // read-only page
    // The initializer tables hold pointers baked for the prototype's base: re-point them.
    for (std::uint32_t i = 0; i < 3; ++i) {
        list.push_back(fixup(l.rva_inits + 8 * i, FixupKind::Addr64, 0, base + l.init_fn_rva[i]));
    }
    list.push_back(fixup(l.rva_init_fault, FixupKind::Addr64, 0, base + l.fault_fn_rva));
    list.push_back(fixup(l.rva_init_overflow, FixupKind::Addr64, 0, base + l.overflow_fn_rva));
    list.push_back(fixup(l.rva_slot_a, FixupKind::SlotWrite, 0, target));
    list.push_back(fixup(l.rva_slot_b, FixupKind::SlotWrite, 0, target + 0x10));
    return list;
}

std::vector<std::uint32_t> init_entry_rvas() {
    const ProtoLayout& l = proto();
    return {l.rva_inits, l.rva_inits + 8, l.rva_inits + 16};
}

std::vector<std::uint32_t> fault_entry_rvas() {
    return {proto().rva_init_fault};
}

std::vector<std::uint32_t> overflow_entry_rvas() {
    return {proto().rva_init_overflow};
}

cml::ipc::LoadPatch make_request(std::uint64_t base, const std::string& tag, bool with_fixups, bool with_inits) {
    cml::ipc::LoadPatch request;
    request.request_id = base >> 16;
    request.patch_id = base;
    cml::ipc::PatchCandidate candidate;
    candidate.path = make_variant(base, tag);
    candidate.image_base = base;
    candidate.image_size = proto().image_size;
    if (with_fixups) {
        candidate.fixups = standard_fixups(base, host_target_address());
    }
    request.candidates.push_back(std::move(candidate));
    if (with_inits) {
        request.initializer_rvas = init_entry_rvas();
    }
    return request;
}

MEMORY_BASIC_INFORMATION query(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION info;
    std::memset(&info, 0, sizeof(info));
    VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info));
    return info;
}

bool is_module_loaded_at(std::uint64_t base) {
    const MEMORY_BASIC_INFORMATION info = query(base);
    return info.State == MEM_COMMIT && info.Type == MEM_IMAGE;
}

std::string initializer_log(std::uint64_t base) {
    const ProtoLayout& l = proto();
    const auto length = *reinterpret_cast<const volatile unsigned long long*>(base + l.rva_log_len);
    std::string text;
    for (unsigned long long i = 0; i < length && i < 16; ++i) {
        text.push_back(*reinterpret_cast<const volatile char*>(base + l.rva_log + i));
    }
    return text;
}

std::string verify_standard(std::uint64_t base, std::uint64_t target) {
    const ProtoLayout& l = proto();
    std::string problems;
    const auto fail = [&problems](const std::string& what) { problems += what + "; "; };
    const auto read32 = [&](std::uint32_t rva) {
        std::int32_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(base + rva), 4);
        return v;
    };
    const auto read64 = [&](std::uint32_t rva) {
        std::uint64_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(base + rva), 8);
        return v;
    };
    // REL32 n=0 at the code stub: target - (site + 4).
    {
        const std::uint64_t site = base + l.rva_code_call + 5;
        const std::int64_t expected = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site + 4);
        if (read32(l.rva_code_call + 5) != static_cast<std::int32_t>(expected)) {
            fail("code stub rel32");
        }
    }
    for (std::uint32_t n = 1; n <= 5; ++n) {
        const std::uint32_t rva = l.rva_data + 8 * (n - 1);
        const std::uint64_t site = base + rva;
        const std::int64_t expected =
            static_cast<std::int64_t>(target + 0x100u * n) - static_cast<std::int64_t>(site + 4 + n);
        if (read32(rva) != static_cast<std::int32_t>(expected)) {
            fail("rel32 n=" + std::to_string(n));
        }
    }
    if (read64(l.rva_data + 64) != 0x1122334455667788ull) fail("addr64");
    if (read64(l.rva_data + 73) != 0xAABBCCDD00112233ull) fail("unaligned addr64");
    if (read64(l.rva_rodata + 8) != 0xDEADBEEF0BADF00Dull) fail("addr64 in read-only data");
    if (read64(l.rva_slot_a) != target) fail("slot a");
    if (read64(l.rva_slot_b) != target + 0x10) fail("slot b");
    for (std::uint32_t i = 0; i < 3; ++i) {
        if (read64(l.rva_inits + 8 * i) != base + l.init_fn_rva[i]) fail("init table entry " + std::to_string(i));
    }
    // Protection was restored exactly.
    if (query(base + l.rva_code_call).Protect != PAGE_EXECUTE_READ) fail("code page protection not restored");
    if (query(base + l.rva_rodata).Protect != PAGE_READONLY) fail("read-only page protection not restored");
    if (query(base + l.rva_inits).Protect != PAGE_READONLY) fail("init table protection not restored");
    // The stub really reaches the host function through the fixed-up call.
    const auto stub = reinterpret_cast<int (*)()>(base + l.rva_code_call);
    if (stub() != kHostTargetResult) fail("calling the code stub did not reach the host function");
    const auto answer = reinterpret_cast<int (*)()>(base + l.rva_answer);
    if (answer() != 42) fail("cml_patch_answer");
    return problems;
}

}  // namespace loadertest

LoaderSession::LoaderSession(unsigned count, std::uint64_t block_size) {
    cml::agent::loader::ReserveParams params;
    params.module_base = loadertest::host_base();
    params.module_size = loadertest::host_size();
    params.block_size = block_size;
    params.block_count = count;
    cml::Result<std::vector<cml::proc::AddressRange>> reserved = loader.reserve_ranges(params);
    REQUIRE_MESSAGE(reserved.has_value(), (reserved.has_value() ? "" : reserved.error().format()));
    blocks = *reserved;
    REQUIRE(blocks.size() == count);
}

LoaderSession::~LoaderSession() {
    for (const std::uint64_t base : loaded) {
        if (loader.is_loaded(base)) {
            static_cast<void>(loader.unload_patch(base));
        }
    }
}

cml::ipc::PatchLoaded LoaderSession::load(const cml::ipc::LoadPatch& request) {
    cml::ipc::PatchLoaded answer = loader.load_patch(request);
    if (answer.status == 0) {
        loaded.push_back(answer.image_base);
    }
    return answer;
}
