#pragma once

// Builds a small but complete PE32+ image for tests (independent of cml_pe):
//   .text   RVA 0x1000, file 0x200   code filler (0xCC, no NUL bytes)
//   .rdata  RVA 0x2000, file 0x400   imports, exports (with a forwarder), debug, load config, TLS
//   .pdata  RVA 0x3000, file 0xE00   three sorted RUNTIME_FUNCTIONs
// The file layout and the mapped layout differ (file offset != RVA), so on-disk and in-memory
// parsing exercise different address mapping. SizeOfImage is 0x4000.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace cml_test {

using PeBytes = std::vector<std::uint8_t>;

namespace pe_layout {
inline constexpr std::size_t kLfanew = 0x80;
inline constexpr std::size_t kFileHeader = kLfanew + 4;
inline constexpr std::size_t kOptional = kFileHeader + 20;
inline constexpr std::size_t kDirectories = kOptional + 112;
inline constexpr std::size_t kSections = kOptional + 0xF0;
inline constexpr std::size_t kTextRaw = 0x200;
inline constexpr std::size_t kRdataRaw = 0x400;
inline constexpr std::size_t kPdataRaw = 0xE00;
inline constexpr std::uint32_t kTextRva = 0x1000;
inline constexpr std::uint32_t kRdataRva = 0x2000;
inline constexpr std::uint32_t kPdataRva = 0x3000;
inline constexpr std::uint32_t kImageSize = 0x4000;
inline constexpr std::size_t kFileSize = 0xE40;
// RVAs of the interesting structures inside .rdata.
inline constexpr std::uint32_t kImportDir = 0x2000;
inline constexpr std::uint32_t kIlt = 0x2100;
inline constexpr std::uint32_t kIat = 0x2140;
inline constexpr std::uint32_t kDllName = 0x2180;
inline constexpr std::uint32_t kHintName = 0x2200;
inline constexpr std::uint32_t kExportDir = 0x2300;
inline constexpr std::uint32_t kExportDirSize = 0x200;
inline constexpr std::uint32_t kExportFuncs = 0x2330;
inline constexpr std::uint32_t kExportNames = 0x2340;
inline constexpr std::uint32_t kExportOrdinals = 0x2350;
inline constexpr std::uint32_t kDebugDir = 0x2600;
inline constexpr std::uint32_t kRsds = 0x2700;
inline constexpr std::uint32_t kLoadConfig = 0x2800;
inline constexpr std::uint32_t kLoadConfigSize = 0x98;
inline constexpr std::uint32_t kTlsDir = 0x2900;
}  // namespace pe_layout

inline void w16(PeBytes& b, std::size_t off, std::uint32_t v) {
    b[off] = static_cast<std::uint8_t>(v);
    b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}
inline void w32(PeBytes& b, std::size_t off, std::uint32_t v) {
    w16(b, off, v & 0xFFFFu);
    w16(b, off + 2, v >> 16);
}
inline void w64(PeBytes& b, std::size_t off, std::uint64_t v) {
    w32(b, off, static_cast<std::uint32_t>(v));
    w32(b, off + 4, static_cast<std::uint32_t>(v >> 32));
}
inline void wstr(PeBytes& b, std::size_t off, const std::string& s) {
    std::memcpy(b.data() + off, s.c_str(), s.size() + 1);
}

// Offset in the on-disk file of a .rdata RVA.
inline std::size_t rdata_off(std::uint32_t rva) {
    return pe_layout::kRdataRaw + (rva - pe_layout::kRdataRva);
}

inline PeBytes build_pe_on_disk() {
    using namespace pe_layout;
    PeBytes b(kFileSize, 0);
    w16(b, 0, 0x5A4D);
    w32(b, 0x3C, static_cast<std::uint32_t>(kLfanew));
    w32(b, kLfanew, 0x00004550);
    // File header.
    w16(b, kFileHeader + 0, 0x8664);
    w16(b, kFileHeader + 2, 3);
    w32(b, kFileHeader + 4, 0x5F000000);
    w16(b, kFileHeader + 16, 0xF0);
    w16(b, kFileHeader + 18, 0x0022);
    // Optional header.
    w16(b, kOptional + 0, 0x20B);
    w32(b, kOptional + 16, kTextRva);  // entry point
    w32(b, kOptional + 20, kTextRva);
    w64(b, kOptional + 24, 0x140000000ull);
    w32(b, kOptional + 32, 0x1000);
    w32(b, kOptional + 36, 0x200);
    w32(b, kOptional + 56, kImageSize);
    w32(b, kOptional + 60, 0x200);
    w16(b, kOptional + 68, 3);       // console
    w16(b, kOptional + 70, 0x4140);  // GuardCf | DynamicBase | NxCompat
    w32(b, kOptional + 108, 16);
    const auto dir = [&](std::size_t index, std::uint32_t rva, std::uint32_t size) {
        w32(b, kDirectories + index * 8, rva);
        w32(b, kDirectories + index * 8 + 4, size);
    };
    dir(0, kExportDir, kExportDirSize);
    dir(1, kImportDir, 0x28);
    dir(3, kPdataRva, 36);
    dir(6, kDebugDir, 28);
    dir(9, kTlsDir, 40);
    dir(10, kLoadConfig, kLoadConfigSize);
    dir(12, kIat, 24);
    // Section table.
    const auto section = [&](std::size_t index, const char* name, std::uint32_t vsize, std::uint32_t rva,
                             std::uint32_t rawsize, std::uint32_t rawptr, std::uint32_t chars) {
        const std::size_t o = kSections + index * 40;
        std::memcpy(b.data() + o, name, std::strlen(name));
        w32(b, o + 8, vsize);
        w32(b, o + 12, rva);
        w32(b, o + 16, rawsize);
        w32(b, o + 20, rawptr);
        w32(b, o + 36, chars);
    };
    section(0, ".text", 0x200, kTextRva, 0x200, static_cast<std::uint32_t>(kTextRaw), 0x60000020);
    section(1, ".rdata", 0xA00, kRdataRva, 0xA00, static_cast<std::uint32_t>(kRdataRaw), 0x40000040);
    section(2, ".pdata", 0x24, kPdataRva, 0x40, static_cast<std::uint32_t>(kPdataRaw), 0x40000040);

    for (std::size_t i = 0; i < 0x200; ++i) {
        b[kTextRaw + i] = 0xCC;
    }
    // Imports: KERNEL32.dll { GetTickCount64 (hint 0x1F0), ordinal 7 }.
    const std::size_t imp = rdata_off(kImportDir);
    w32(b, imp + 0, kIlt);
    w32(b, imp + 12, kDllName);
    w32(b, imp + 16, kIat);
    for (std::uint32_t base : {kIlt, kIat}) {
        w64(b, rdata_off(base) + 0, kHintName);
        w64(b, rdata_off(base) + 8, 0x8000000000000007ull);
    }
    wstr(b, rdata_off(kDllName), "KERNEL32.dll");
    w16(b, rdata_off(kHintName), 0x1F0);
    wstr(b, rdata_off(kHintName) + 2, "GetTickCount64");
    // Exports: base 10; alpha (ordinal 10, RVA 0x1000), unused slot 11, beta (ordinal 12, forwarder).
    const std::size_t exp = rdata_off(kExportDir);
    w32(b, exp + 4, 0x5E000000);
    w32(b, exp + 12, 0x2380);
    w32(b, exp + 16, 10);
    w32(b, exp + 20, 3);
    w32(b, exp + 24, 2);
    w32(b, exp + 28, kExportFuncs);
    w32(b, exp + 32, kExportNames);
    w32(b, exp + 36, kExportOrdinals);
    w32(b, rdata_off(kExportFuncs) + 0, kTextRva);
    w32(b, rdata_off(kExportFuncs) + 4, 0);
    w32(b, rdata_off(kExportFuncs) + 8, 0x2420);
    w32(b, rdata_off(kExportNames) + 0, 0x2400);
    w32(b, rdata_off(kExportNames) + 4, 0x2410);
    w16(b, rdata_off(kExportOrdinals) + 0, 0);
    w16(b, rdata_off(kExportOrdinals) + 2, 2);
    wstr(b, rdata_off(0x2380), "test.dll");
    wstr(b, rdata_off(0x2400), "alpha");
    wstr(b, rdata_off(0x2410), "beta");
    wstr(b, rdata_off(0x2420), "NTDLL.RtlFoo");
    // Debug directory: one CodeView entry.
    const std::string pdb_path = "C:\\build\\test.pdb";
    const std::size_t dbg = rdata_off(kDebugDir);
    w32(b, dbg + 4, 0x5F001234);
    w32(b, dbg + 12, 2);
    w32(b, dbg + 16, static_cast<std::uint32_t>(24 + pdb_path.size() + 1));
    w32(b, dbg + 20, kRsds);
    w32(b, dbg + 24, static_cast<std::uint32_t>(rdata_off(kRsds)));
    const std::size_t rsds = rdata_off(kRsds);
    w32(b, rsds, 0x53445352);
    for (std::size_t i = 0; i < 16; ++i) {
        b[rsds + 4 + i] = static_cast<std::uint8_t>(i + 1);
    }
    w32(b, rsds + 20, 3);
    wstr(b, rsds + 24, pdb_path);
    // Load config.
    const std::size_t lc = rdata_off(kLoadConfig);
    w32(b, lc + 0, kLoadConfigSize);
    w64(b, lc + 88, 0x140003000ull);
    w64(b, lc + 112, 0x140001234ull);
    w64(b, lc + 120, 0x140001240ull);
    w64(b, lc + 128, 0x140002F00ull);
    w64(b, lc + 136, 5);
    w32(b, lc + 144, 0x100 | 0x400);
    // TLS.
    const std::size_t tls = rdata_off(kTlsDir);
    w64(b, tls + 0, 0x140004000ull);
    w64(b, tls + 8, 0x140004100ull);
    w64(b, tls + 16, 0x140003010ull);
    w64(b, tls + 24, 0x140002100ull);
    w32(b, tls + 32, 0x20);
    // .pdata.
    const std::uint32_t entries[3][3] = {{0x1000, 0x1010, 0x2800}, {0x1010, 0x1040, 0x2810}, {0x1100, 0x1180, 0x2821}};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t k = 0; k < 3; ++k) {
            w32(b, kPdataRaw + i * 12 + k * 4, entries[i][k]);
        }
    }
    return b;
}

// The same image laid out as the loader maps it: headers at 0, every section at its RVA.
inline PeBytes build_pe_in_memory(const PeBytes& disk) {
    using namespace pe_layout;
    PeBytes m(kImageSize, 0);
    std::memcpy(m.data(), disk.data(), 0x200);
    std::memcpy(m.data() + kTextRva, disk.data() + kTextRaw, 0x200);
    std::memcpy(m.data() + kRdataRva, disk.data() + kRdataRaw, 0xA00);
    std::memcpy(m.data() + kPdataRva, disk.data() + kPdataRaw, 0x40);
    return m;
}

inline std::span<const std::byte> pe_span(const PeBytes& b) {
    return std::as_bytes(std::span<const std::uint8_t>(b.data(), b.size()));
}

}  // namespace cml_test
