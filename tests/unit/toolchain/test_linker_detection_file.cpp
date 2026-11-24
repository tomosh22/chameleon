// WP-39b: detect_module_linker reads the headers of the module file and nothing else. The broker used to read the whole
// module (several hundred MiB for a large program) into private memory to learn which linker built it.
//
//   * a module file that ends right after its section table (the section contents are absent) is detected like the
//     complete file, and like the same headers followed by megabytes of other bytes;
//   * (Windows) with everything after the first 64 KiB locked by another handle, reading the whole file fails and the
//     detection still succeeds: it does not depend on the file's tail;
//   * what cml_pe refuses in the headers is still refused with its diagnostic, prefixed with the path.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "cml/pe/pe.h"
#include "cml/toolchain/link_patch.h"
#include "doctest.h"

using namespace cml::toolchain;

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kLfanew = 0x100;
constexpr std::size_t kHeadersEnd = kLfanew + 24 + 0xF0 + 40;  // NT headers, a 240-byte optional header, one section header

// The headers of an x64 image as link.exe 14.44 writes them (Rich header present): DOS header, NT headers and a section
// table of one .text section whose raw data (256 MiB at file offset 0x400) is NOT part of these bytes.
std::vector<std::byte> module_headers(std::uint16_t magic = 0x20B) {
    std::vector<std::byte> b(0x400, std::byte{0});
    const auto w16 = [&](std::size_t o, unsigned v) {
        b[o] = static_cast<std::byte>(v & 0xFF);
        b[o + 1] = static_cast<std::byte>((v >> 8) & 0xFF);
    };
    const auto w32 = [&](std::size_t o, std::uint32_t v) {
        w16(o, v & 0xFFFF);
        w16(o + 2, v >> 16);
    };
    b[0] = std::byte{'M'};
    b[1] = std::byte{'Z'};
    w32(0x3C, static_cast<std::uint32_t>(kLfanew));
    std::memcpy(b.data() + 0x80, "DanS", 4);
    std::memcpy(b.data() + 0xA0, "Rich", 4);
    w32(kLfanew, 0x00004550);
    w16(kLfanew + 4, 0x8664);      // machine
    w16(kLfanew + 6, 1);           // one section
    w16(kLfanew + 20, 0xF0);       // SizeOfOptionalHeader
    w16(kLfanew + 22, 0x0022);     // executable, large address aware
    const std::size_t opt = kLfanew + 24;
    w16(opt, magic);
    b[opt + 2] = std::byte{14};    // linker 14.44
    b[opt + 3] = std::byte{44};
    w32(opt + 32, 0x1000);         // section alignment
    w32(opt + 36, 0x200);          // file alignment
    w32(opt + 56, 0x10002000);     // SizeOfImage
    w32(opt + 60, 0x400);          // SizeOfHeaders
    w32(opt + 108, 16);            // data directories (all empty)
    const std::size_t sec = opt + 0xF0;
    std::memcpy(b.data() + sec, ".text", 5);
    w32(sec + 8, 0x10000000);      // VirtualSize
    w32(sec + 12, 0x1000);         // VirtualAddress
    w32(sec + 16, 0x10000000);     // SizeOfRawData: 256 MiB ...
    w32(sec + 20, 0x400);          // ... at file offset 0x400
    w32(sec + 36, 0x60000020);
    return b;
}

struct TempFile {
    fs::path path;
    explicit TempFile(const char* name) {
        std::error_code ec;
        path = fs::temp_directory_path(ec) / (std::string("cml_wp39b_link_") + name);
        fs::remove(path, ec);
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
    void write(const std::vector<std::byte>& head, std::size_t head_bytes, std::size_t tail = 0) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head_bytes));
        const std::vector<char> block(64 * 1024, static_cast<char>(0xEE));
        for (std::size_t left = tail; left != 0;) {
            const std::size_t n = left < block.size() ? left : block.size();
            out.write(block.data(), static_cast<std::streamsize>(n));
            left -= n;
        }
        out.close();
        REQUIRE(static_cast<bool>(out));
    }
    std::string name() const { return path.string(); }
};

// link.exe by the Rich header; there is no PDB for these synthetic modules, which is a warning (ADR-0027), not an error.
void check_is_link_from_pe(const cml::Result<LinkerDetection>& d) {
    REQUIRE_MESSAGE(d.has_value(), d.error().format());
    CHECK(d->family == ToolFamily::Link);
    CHECK(d->from_pe == ToolFamily::Link);
    CHECK(d->from_pdb == ToolFamily::Unknown);
    CHECK(d->version[0] == 14u);
    CHECK(d->version[1] == 44u);
    REQUIRE(d->warnings.size() == 1);
    CHECK(d->warnings[0].code == diag::kCannotReadModule);
    CHECK(d->warnings[0].message.find("PDB") != std::string::npos);
}

}  // namespace

TEST_CASE("linker detection reads the module's headers only: a file cut after the section table is detected like the complete one") {
    const std::vector<std::byte> head = module_headers();

    // 1. Nothing after the section table: the section's 256 MiB of raw data are not in the file at all.
    TempFile cut("cut.exe");
    cut.write(head, kHeadersEnd);
    REQUIRE(fs::file_size(cut.path) == kHeadersEnd);
    const auto from_cut = detect_module_linker(cut.name());
    check_is_link_from_pe(from_cut);

    // 2. The same headers followed by 24 MiB that are not an image at all.
    TempFile big("big.exe");
    big.write(head, head.size(), 24u * 1024u * 1024u);
    const auto from_big = detect_module_linker(big.name());
    check_is_link_from_pe(from_big);
    CHECK(from_big->evidence == from_cut->evidence);  // the same clues, line by line

    // The reader the detection uses took one 4 KiB block of that file.
    const auto prefix = cml::pe::read_file_headers(big.name());
    REQUIRE(prefix.has_value());
    CHECK(prefix->size() == 4096u);

    // 3. The override still replaces the decision and keeps the evidence.
    DetectLinkerOptions forced;
    forced.override_family = ToolFamily::LldLink;
    const auto overridden = detect_module_linker(cut.name(), {}, forced);
    REQUIRE_MESSAGE(overridden.has_value(), overridden.error().format());
    CHECK(overridden->family == ToolFamily::LldLink);
    CHECK(overridden->overridden);
    CHECK(overridden->from_pe == ToolFamily::Link);
}

TEST_CASE("linker detection: what cml_pe refuses in the headers is still refused, with the path in the message") {
    TempFile file("bad.exe");

    file.write(module_headers(0x10B), 0x400);  // PE32
    auto r = detect_module_linker(file.name());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 1303u);
    CHECK(r.error().message.find(file.name()) == 0);

    file.write(module_headers(), kLfanew + 24 + 0x40);  // cut inside the optional header
    r = detect_module_linker(file.name());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 1305u);
    CHECK(r.error().message.find(file.name()) == 0);

    file.write(module_headers(), kHeadersEnd - 8);  // cut inside the section table
    r = detect_module_linker(file.name());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 1306u);

    file.write(module_headers(), 0x20);  // not even a DOS header
    r = detect_module_linker(file.name());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 1301u);

    r = detect_module_linker((fs::temp_directory_path() / "cml_wp39b_link_no_such_module.exe").string());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 1300u);
}

#if defined(_WIN32)
TEST_CASE("linker detection does not touch the file's tail: it succeeds while another handle holds the tail locked") {
    const std::vector<std::byte> head = module_headers();
    TempFile file("locked.exe");
    constexpr std::size_t kTail = 1024 * 1024;
    file.write(head, head.size(), kTail);

    // Lock everything from 64 KiB to the end, exclusively, through our own handle. A second handle (std::ifstream inside
    // cml_pe) cannot read the locked range, also in the same process.
    const HANDLE h = CreateFileW(file.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    constexpr DWORD kLockFrom = 64 * 1024;
    OVERLAPPED range{};
    range.Offset = kLockFrom;
    const DWORD lock_bytes = static_cast<DWORD>(head.size() + kTail - kLockFrom);
    const BOOL locked = LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, lock_bytes, 0, &range);
    REQUIRE(locked != FALSE);

    // Control: the old way (the whole file) now fails, so this test can tell the two apart.
    const auto whole = cml::pe::read_file(file.name());
    const bool whole_failed = !whole.has_value();
    // The detection reads 4 KiB and decides.
    const auto detected = detect_module_linker(file.name());

    OVERLAPPED unlock{};
    unlock.Offset = kLockFrom;
    UnlockFileEx(h, 0, lock_bytes, 0, &unlock);
    CloseHandle(h);

    REQUIRE(whole_failed);
    CHECK(whole.error().code == 1300u);
    check_is_link_from_pe(detected);
}
#endif
