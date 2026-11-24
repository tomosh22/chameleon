// WP-39b: pe::read_file_headers reads the headers of an image file and nothing after the section table, whatever the size
// of the file, and Image::parse sees in them what it sees in the whole file (same headers, same diagnostics).
#include "cml/pe/pe.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "doctest.h"
#include "pe_builder.h"

using namespace cml::pe;
using namespace cml_test;
using namespace cml_test::pe_layout;

namespace {

namespace fs = std::filesystem;

struct TempFile {
    fs::path path;
    explicit TempFile(const char* name) {
        std::error_code ec;
        path = fs::temp_directory_path(ec) / (std::string("cml_wp39b_pe_") + name);
        fs::remove(path, ec);
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
    // The image bytes followed by `tail` bytes of 0xEE (section contents, as far as the reader is concerned).
    void write(const PeBytes& head, std::size_t tail = 0) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
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

// The end of the section table of the builder's image: everything Image::parse reads lies before it.
constexpr std::size_t kHeaderEnd = kSections + 3 * 40;

}  // namespace

TEST_CASE("pe file headers: a 32 MiB file costs one 4 KiB read and parses like the whole file") {
    const PeBytes disk = build_pe_on_disk();
    TempFile file("large.exe");
    constexpr std::size_t kTail = 32u * 1024u * 1024u;
    file.write(disk, kTail);
    REQUIRE(fs::file_size(file.path) == disk.size() + kTail);

    const auto head = read_file_headers(file.name());
    REQUIRE_MESSAGE(head.has_value(), head.error().format());
    CHECK(head->size() == 4096u);  // the first block holds all the headers (they end at kHeaderEnd): nothing else is read
    REQUIRE(kHeaderEnd <= head->size());
    CHECK(std::memcmp(head->data(), disk.data(), kHeaderEnd) == 0);

    const auto from_head = Image::parse(ByteSpan(head->data(), head->size()), ImageKind::OnDisk);
    const auto whole = load_image_file(file.name());
    REQUIRE_MESSAGE(from_head.has_value(), from_head.error().format());
    REQUIRE_MESSAGE(whole.has_value(), whole.error().format());
    CHECK(from_head->file_header().machine == whole->image.file_header().machine);
    CHECK(from_head->file_header().number_of_sections == 3);
    CHECK(from_head->optional_header().size_of_image == whole->image.optional_header().size_of_image);
    CHECK(from_head->optional_header().major_linker_version == whole->image.optional_header().major_linker_version);
    REQUIRE(from_head->sections().size() == whole->image.sections().size());
    for (std::size_t i = 0; i < from_head->sections().size(); ++i) {
        CHECK(from_head->sections()[i].name() == whole->image.sections()[i].name());
        CHECK(from_head->sections()[i].pointer_to_raw_data == whole->image.sections()[i].pointer_to_raw_data);
    }
    CHECK(from_head->data_directory(dir::Debug).rva == whole->image.data_directory(dir::Debug).rva);
}

TEST_CASE("pe file headers: a file cut anywhere gives the diagnostic the whole-file reader gives") {
    const PeBytes disk = build_pe_on_disk();
    TempFile file("cut.exe");
    // Every length through the headers, then a few beyond them.
    for (std::size_t len = 0; len <= disk.size(); len += (len < kHeaderEnd + 8 ? 1 : 257)) {
        file.write(PeBytes(disk.begin(), disk.begin() + static_cast<std::ptrdiff_t>(len)));
        const auto head = read_file_headers(file.name());
        const auto all = read_file(file.name());
        REQUIRE(head.has_value());
        REQUIRE(all.has_value());
        CHECK(head->size() <= all->size());
        const auto a = Image::parse(ByteSpan(head->data(), head->size()), ImageKind::OnDisk);
        const auto b = Image::parse(ByteSpan(all->data(), all->size()), ImageKind::OnDisk);
        INFO("file length " << len);
        REQUIRE(a.has_value() == b.has_value());
        if (!a.has_value()) {
            CHECK(a.error().code == b.error().code);
            CHECK(a.error().message == b.error().message);  // also the byte counts the messages name
        } else {
            CHECK(a->sections().size() == b->sections().size());
        }
    }
}

TEST_CASE("pe file headers: NT headers beyond the first block are followed, and still nothing after the section table is read") {
    // The same headers, moved to offset 0x2000 (a long DOS stub), with 1 MiB after the section table.
    const PeBytes disk = build_pe_on_disk();
    constexpr std::size_t kFar = 0x2000;
    PeBytes moved(kFar + (kHeaderEnd - kLfanew), 0);
    w16(moved, 0, 0x5A4D);
    w32(moved, 0x3C, static_cast<std::uint32_t>(kFar));
    std::memcpy(moved.data() + kFar, disk.data() + kLfanew, kHeaderEnd - kLfanew);
    TempFile file("far.exe");
    file.write(moved, 1024 * 1024);

    const auto head = read_file_headers(file.name());
    REQUIRE_MESSAGE(head.has_value(), head.error().format());
    CHECK(head->size() == moved.size());  // exactly up to the end of the section table
    const auto image = Image::parse(ByteSpan(head->data(), head->size()), ImageKind::OnDisk);
    REQUIRE_MESSAGE(image.has_value(), image.error().format());
    CHECK(image->nt_headers_offset() == kFar);
    CHECK(image->sections().size() == 3);

    // An e_lfanew that points past the end of the file: the whole (small) file is read and the usual diagnostic follows.
    PeBytes bad = disk;
    w32(bad, 0x3C, 0x7FFFFFF0u);
    file.write(bad);
    const auto bad_head = read_file_headers(file.name());
    REQUIRE(bad_head.has_value());
    CHECK(bad_head->size() == bad.size());
    const auto refused = Image::parse(ByteSpan(bad_head->data(), bad_head->size()), ImageKind::OnDisk);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == 1302u);
}

TEST_CASE("pe file headers: a missing file is CML1300, an empty one is read as empty") {
    const auto missing = read_file_headers("/definitely/not/here.exe");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == 1300u);
    CHECK(missing.error().message.find("here.exe") != std::string::npos);

    TempFile file("empty.exe");
    file.write(PeBytes{});
    const auto empty = read_file_headers(file.name());
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}
