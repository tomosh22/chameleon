#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "pdb_builder.h"

namespace cml::testpdb {

// Unique temp directory removed on destruction.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        const auto base = std::filesystem::temp_directory_path();
        for (;;) {
            const auto candidate = base / ("cml_pdb_test_" + std::to_string(counter.fetch_add(1)) + "_" +
                                           std::to_string(static_cast<unsigned long long>(
                                               std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFF)));
            std::error_code ec;
            if (std::filesystem::create_directories(candidate, ec) && !ec) {
                path_ = candidate;
                break;
            }
        }
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string file(const std::string& name) const { return (path_ / name).string(); }
    std::string dir() const { return path_.string(); }
    std::string subdir(const std::string& name) const {
        const auto p = path_ / name;
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p.string();
    }

private:
    std::filesystem::path path_;
};

inline void write_file(const std::string& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// The standard content used by most synthetic tests (see test_synthetic.cpp for the expectations).
inline Spec standard_spec() {
    Spec s;
    s.sections = {{".text", 0x1000, 0x1000, 0x1000, 0x60000020},
                  {".rdata", 0x800, 0x2000, 0x800, 0x40000040},
                  {".data", 0x400, 0x3000, 0x400, 0xC0000040}};

    // IPI: build info for module 1 (what clang-cl records instead of S_ENVBLOCK).
    const std::uint32_t cwd = s.ipi.string_id("C:\\proj");
    const std::uint32_t tool = s.ipi.string_id("C:\\llvm\\clang-cl.exe");
    const std::uint32_t src = s.ipi.string_id("main.cpp");
    const std::uint32_t pdb = s.ipi.string_id("");
    // Long strings are stored as pieces: the substring list holds the leading pieces, the record's own
    // text is the last one (verified against a cl 19.51 PDB with llvm-pdbutil; WP-05).
    const std::uint32_t part1 = s.ipi.string_id("\"-cc1\"");
    const std::uint32_t part2 = s.ipi.string_id(" -c");
    const std::uint32_t list = s.ipi.substr_list({part1, part2});
    const std::uint32_t cmd = s.ipi.string_id(" -O2", list);
    const std::uint32_t build_info = s.ipi.buildinfo({cwd, tool, src, pdb, cmd});

    {
        Module m;
        m.name = "C:\\proj\\obj\\logic.cpp.obj";
        m.symbols.objname(m.name);
        m.symbols.compile3(1, (1u << 10) | (1u << 14) | (1u << 13), 0xD0, "Test Compiler 19.44");
        m.symbols.envblock({{"cwd", "C:\\proj"},
                            {"cl", "C:\\tools\\cl.exe"},
                            {"cmd", "/c /Zi /Od /O2 /Gy logic.cpp"},
                            {"src", "C:\\proj\\src\\logic.cpp"},
                            {"pdb", "C:\\proj\\obj\\vc143.pdb"}});
        m.symbols.proc(0x1110, "compute_value", 1, 0x100, 0x20);
        m.symbols.data(0x110C, "counter", 3, 0x10);  // function-local static
        m.symbols.block();
        m.symbols.data(0x110C, "in_block", 3, 0x14);
        m.symbols.end();
        m.symbols.end();
        m.symbols.proc(0x110F, "helper", 1, 0x120, 0x10);
        m.symbols.end();
        m.symbols.data(0x110C, "`anonymous namespace'::s_counter", 3, 0x20);
        m.symbols.data(0x110D, "g_tick", 3, 0x0);
        m.symbols.data(0x1112, "tls_local", 3, 0x28);
        m.symbols.udt("Point");
        m.files = {"C:\\proj\\src\\logic.cpp", "C:\\proj\\src\\logic.h", "C:\\Program Files\\SDK\\include\\stdio.h"};
        s.modules.push_back(std::move(m));
    }
    {
        Module m;
        m.name = "C:\\proj\\obj\\main.cpp.obj";
        m.lib = "C:\\libs\\mylib.lib";
        m.symbols.objname(m.name);
        m.symbols.compile3(1, 0, 0xD0, "Test Compiler 19.44");
        m.symbols.proc(0x1110, "main", 1, 0x200, 0x40);
        m.symbols.end();
        m.symbols.buildinfo(build_info);
        m.files = {"c:/proj/src/MAIN.cpp", "C:\\proj\\src\\..\\src\\logic.h", "..\\include\\rel.h"};
        s.modules.push_back(std::move(m));
    }
    {
        Module m;
        m.name = "C:\\proj\\obj\\rel.cpp.obj";
        m.symbols.compile3(0, 0, 0xD0, "Test C Compiler");
        m.symbols.envblock({{"cwd", "D:\\work\\sub"}, {"src", "rel.cpp"}});
        m.symbols.proc(0x1147, "rel_func", 1, 0x300, 0x10);  // S_GPROC32_ID closes with S_PROC_ID_END
        m.symbols.data(0x110C, "rel_static", 3, 0x30);
        m.symbols.add(0x114F, Bytes());
        m.symbols.data(0x110C, "after_proc", 3, 0x34);
        m.files = {"..\\include\\rel.h", "rel.cpp"};
        s.modules.push_back(std::move(m));
    }
    {
        Module m;
        m.name = "Import:kernel32.dll";
        m.no_symbols = true;
        s.modules.push_back(std::move(m));
    }
    {
        Module m;
        m.name = "* Linker *";
        m.symbols.compile3(7, 0, 0xD0, "Test Linker");
        m.symbols.envblock({{"cwd", "C:\\proj"}, {"exe", "C:\\tools\\link.exe"}, {"cmd", "/DEBUG:FULL"}});
        s.modules.push_back(std::move(m));
    }
    s.contributions = {{1, 0x100, 0x30, 0}, {1, 0x200, 0x40, 1}, {1, 0x300, 0x10, 2}, {3, 0x0, 0x40, 0}, {9, 0, 4, 0}, {1, 0x400, 4, 77}};

    // Symbol record stream: publics and global data.
    s.symrec.pub32("?compute_value@@YAHXZ", 0x3, 1, 0x100);
    s.symrec.pub32("compute_alias", 0x3, 1, 0x100);
    s.symrec.pub32("main", 0x3, 1, 0x200);
    s.symrec.pub32("?g_tick@@3HA", 0, 3, 0x0);
    s.symrec.pub32("_tls_index", 0, 3, 0x38);
    s.symrec.data(0x110D, "g_tick", 3, 0x0);
    s.symrec.add(0x1108, Bytes{1, 0, 0, 0, 'X', 0});  // S_UDT: ignored here
    return s;
}

}  // namespace cml::testpdb
