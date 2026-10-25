#pragma once

// Test-only builder of small, valid MSF 7.00 PDB files with exactly the content a test needs:
// modules with S_COMPILE3 / S_ENVBLOCK / symbols, a symbol record stream with publics, section
// headers and contributions, source-info file lists and an IPI stream with LF_BUILDINFO. The same
// content can be laid out contiguously or fragmented (blocks of different streams interleaved) so
// both MSF read paths are covered without depending on any compiler.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace cml::testpdb {

using Bytes = std::vector<std::uint8_t>;

inline void put16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
inline void put32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}
inline void putstr(Bytes& b, const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
}
inline void pad4(Bytes& b) {
    while (b.size() % 4 != 0) {
        b.push_back(0);
    }
}

// A sequence of CodeView records (length, kind, body, padding to 4 bytes).
class Records {
public:
    void add(std::uint16_t kind, const Bytes& body) {
        Bytes padded = body;
        // total = 2 (length field) + 2 (kind) + padded.size() must be a multiple of 4
        while ((4 + padded.size()) % 4 != 0) {
            padded.push_back(0);
        }
        put16(bytes_, static_cast<std::uint16_t>(2 + padded.size()));
        put16(bytes_, kind);
        bytes_.insert(bytes_.end(), padded.begin(), padded.end());
    }
    const Bytes& bytes() const { return bytes_; }

    // Symbol helpers -------------------------------------------------------------------------
    void proc(std::uint16_t kind, const std::string& name, std::uint16_t seg, std::uint32_t off, std::uint32_t len,
              std::uint8_t flags = 0) {
        Bytes b;
        put32(b, 0);       // parent
        put32(b, 0);       // end
        put32(b, 0);       // next
        put32(b, len);     // code length
        put32(b, 0);       // debug start
        put32(b, len);     // debug end
        put32(b, 0x1000);  // type
        put32(b, off);
        put16(b, seg);
        b.push_back(flags);
        putstr(b, name);
        add(kind, b);
    }
    void data(std::uint16_t kind, const std::string& name, std::uint16_t seg, std::uint32_t off, std::uint32_t type = 0x74) {
        Bytes b;
        put32(b, type);
        put32(b, off);
        put16(b, seg);
        putstr(b, name);
        add(kind, b);
    }
    void udt(const std::string& name, std::uint32_t type = 0x1001) {
        Bytes b;
        put32(b, type);
        putstr(b, name);
        add(0x1108, b);
    }
    void pub32(const std::string& name, std::uint32_t flags, std::uint16_t seg, std::uint32_t off) {
        Bytes b;
        put32(b, flags);
        put32(b, off);
        put16(b, seg);
        putstr(b, name);
        add(0x110E, b);
    }
    void end() { add(0x0006, Bytes()); }
    // Appends raw bytes (for building deliberately broken streams).
    void raw(const Bytes& b) { bytes_.insert(bytes_.end(), b.begin(), b.end()); }
    void block() {
        Bytes b(20, 0);
        putstr(b, "");
        add(0x1103, b);
    }
    void compile3(std::uint8_t language, std::uint32_t extra_flags, std::uint16_t machine, const std::string& version) {
        Bytes b;
        put32(b, language | extra_flags);
        put16(b, machine);
        const std::uint16_t versions[8] = {19, 29, 30133, 0, 14, 29, 30133, 0};
        for (std::uint16_t v : versions) {
            put16(b, v);
        }
        putstr(b, version);
        add(0x113C, b);
    }
    void objname(const std::string& name) {
        Bytes b;
        put32(b, 0);
        putstr(b, name);
        add(0x1101, b);
    }
    void envblock(const std::vector<std::pair<std::string, std::string>>& pairs) {
        Bytes b;
        b.push_back(0);  // flags
        for (const auto& kv : pairs) {
            putstr(b, kv.first);
            putstr(b, kv.second);
        }
        b.push_back(0);  // empty key terminates
        add(0x113D, b);
    }
    void buildinfo(std::uint32_t id) {
        Bytes b;
        put32(b, id);
        add(0x114C, b);
    }

private:
    Bytes bytes_;
};

struct Module {
    std::string name;  // object path
    std::string lib;   // archive path; empty = same as name
    Records symbols;
    std::vector<std::string> files;
    bool no_symbols = false;  // symbol stream index 0xFFFF
};

struct Section {
    std::string name;
    std::uint32_t virtual_size;
    std::uint32_t rva;
    std::uint32_t raw_size;
    std::uint32_t characteristics;
};

struct Contribution {
    std::uint16_t section;
    std::uint32_t offset;
    std::uint32_t size;
    std::uint16_t module;
};

// IPI string/record builder: ids start at 0x1000.
class Ipi {
public:
    std::uint32_t string_id(const std::string& s, std::uint32_t substr = 0) {
        Bytes b;
        put32(b, substr);
        putstr(b, s);
        recs_.add(0x1605, b);
        return next_++;
    }
    std::uint32_t substr_list(const std::vector<std::uint32_t>& ids) {
        Bytes b;
        put32(b, static_cast<std::uint32_t>(ids.size()));
        for (auto id : ids) {
            put32(b, id);
        }
        recs_.add(0x1604, b);
        return next_++;
    }
    std::uint32_t buildinfo(const std::vector<std::uint32_t>& args) {
        Bytes b;
        put16(b, static_cast<std::uint16_t>(args.size()));
        for (auto id : args) {
            put32(b, id);
        }
        recs_.add(0x1603, b);
        return next_++;
    }
    Bytes stream() const {
        Bytes s;
        put32(s, 20040203);  // version
        put32(s, 56);        // header size
        put32(s, 0x1000);    // type index begin
        put32(s, next_);     // type index end
        put32(s, static_cast<std::uint32_t>(recs_.bytes().size()));
        while (s.size() < 56) {
            s.push_back(0);
        }
        s.insert(s.end(), recs_.bytes().begin(), recs_.bytes().end());
        return s;
    }
    bool empty() const { return next_ == 0x1000; }
    std::uint32_t next_id() const { return next_; }  // the id the next record added will get

private:
    Records recs_;
    std::uint32_t next_ = 0x1000;
};

struct Spec {
    std::array<std::uint8_t, 16> guid{{0x3C, 0x83, 0xC3, 0x8C, 0x00, 0x87, 0x0D, 0xEB, 0x4C, 0x4C, 0x44, 0x20, 0x50, 0x44, 0x42, 0x2E}};
    std::uint32_t age = 1;
    std::uint16_t machine = 0x8664;
    std::uint32_t block_size = 4096;
    bool fragmented = false;
    std::vector<Module> modules;
    Records symrec;  // publics and global data
    std::vector<Section> sections;
    std::vector<Contribution> contributions;
    Ipi ipi;
    bool omit_section_stream = false;
    // Deliberate defects.
    bool bad_dbi_signature = false;
    bool empty_info_stream = false;
    bool bad_contrib_version = false;
    bool bad_fileinfo_offset = false;
    // Age written to the PDB info stream when it should differ from the DBI age (spec.age).
    std::uint32_t info_age_override = 0xFFFFFFFFu;
    // Number of modules the source-info table covers (the module records still claim their files).
    std::size_t fileinfo_modules = static_cast<std::size_t>(-1);
};

// Builds the PDB image.
inline Bytes build(const Spec& spec) {
    std::vector<Bytes> streams;
    streams.resize(7);  // 0 old dir, 1 info, 2 TPI, 3 DBI, 4 IPI, 5 symrec, 6 sections
    {
        Bytes& info = streams[1];
        put32(info, 20000404);
        put32(info, 0x12345678);
        put32(info, spec.info_age_override != 0xFFFFFFFFu ? spec.info_age_override : spec.age);
        info.insert(info.end(), spec.guid.begin(), spec.guid.end());
    }
    if (spec.empty_info_stream) {
        streams[1].clear();
    }
    streams[4] = spec.ipi.stream();
    streams[5] = spec.symrec.bytes();
    {
        Bytes& sec = streams[6];
        for (const Section& s : spec.sections) {
            std::array<char, 8> name{};
            std::memcpy(name.data(), s.name.data(), std::min<std::size_t>(8, s.name.size()));
            sec.insert(sec.end(), name.begin(), name.end());
            put32(sec, s.virtual_size);
            put32(sec, s.rva);
            put32(sec, s.raw_size);
            put32(sec, 0);
            put32(sec, 0);
            put32(sec, 0);
            put16(sec, 0);
            put16(sec, 0);
            put32(sec, s.characteristics);
        }
    }
    // Module streams.
    std::vector<std::uint16_t> module_stream;
    for (const Module& m : spec.modules) {
        if (m.no_symbols) {
            module_stream.push_back(0xFFFF);
            continue;
        }
        Bytes s;
        put32(s, 4);  // C13 signature
        s.insert(s.end(), m.symbols.bytes().begin(), m.symbols.bytes().end());
        module_stream.push_back(static_cast<std::uint16_t>(streams.size()));
        streams.push_back(std::move(s));
    }

    // DBI stream.
    Bytes modinfo;
    for (std::size_t i = 0; i < spec.modules.size(); ++i) {
        const Module& m = spec.modules[i];
        Bytes e;
        put32(e, 0);
        for (int k = 0; k < 7; ++k) {
            put32(e, 0);  // embedded SectionContribEntry (28 bytes)
        }
        put16(e, 0);                 // flags            @32
        put16(e, module_stream[i]);  // stream           @34
        put32(e, module_stream[i] == 0xFFFF ? 0 : static_cast<std::uint32_t>(m.symbols.bytes().size() + 4));  // @36
        put32(e, 0);                 // c11              @40
        put32(e, 0);                 // c13              @44
        put16(e, static_cast<std::uint16_t>(m.files.size()));  // @48
        put16(e, 0);
        put32(e, 0);
        put32(e, 0);
        put32(e, 0);
        putstr(e, m.name);
        putstr(e, m.lib.empty() ? m.name : m.lib);
        pad4(e);
        modinfo.insert(modinfo.end(), e.begin(), e.end());
    }
    Bytes contrib;
    put32(contrib, spec.bad_contrib_version ? 0x1234u : 0xEFFE0000u + 19970605u);
    for (const Contribution& c : spec.contributions) {
        put16(contrib, c.section);
        put16(contrib, 0);
        put32(contrib, c.offset);
        put32(contrib, c.size);
        put32(contrib, 0x60000020);
        put16(contrib, c.module);
        put16(contrib, 0);
        put32(contrib, 0);
        put32(contrib, 0);
    }
    Bytes fileinfo;
    {
        std::vector<std::uint32_t> name_offsets;
        Bytes names;
        std::vector<std::string> seen;
        std::vector<std::uint32_t> seen_off;
        const std::size_t covered = std::min(spec.modules.size(), spec.fileinfo_modules);
        for (std::size_t mi = 0; mi < covered; ++mi) {
            const Module& m = spec.modules[mi];
            for (const std::string& f : m.files) {
                auto it = std::find(seen.begin(), seen.end(), f);
                if (it == seen.end()) {
                    seen.push_back(f);
                    seen_off.push_back(static_cast<std::uint32_t>(names.size()));
                    putstr(names, f);
                    it = seen.end() - 1;
                }
                name_offsets.push_back(seen_off[static_cast<std::size_t>(it - seen.begin())]);
            }
        }
        put16(fileinfo, static_cast<std::uint16_t>(covered));
        put16(fileinfo, static_cast<std::uint16_t>(seen.size()));
        std::uint16_t start = 0;
        for (std::size_t mi = 0; mi < covered; ++mi) {
            put16(fileinfo, start);
            start = static_cast<std::uint16_t>(start + spec.modules[mi].files.size());
        }
        for (std::size_t mi = 0; mi < covered; ++mi) {
            put16(fileinfo, static_cast<std::uint16_t>(spec.modules[mi].files.size()));
        }
        for (std::uint32_t o : name_offsets) {
            put32(fileinfo, spec.bad_fileinfo_offset ? 0x7FFFFFFFu : o);
        }
        fileinfo.insert(fileinfo.end(), names.begin(), names.end());
        pad4(fileinfo);
    }
    Bytes dbghdr;
    for (int i = 0; i < 11; ++i) {
        put16(dbghdr, i == 5 && !spec.omit_section_stream ? 6 : 0xFFFF);
    }
    Bytes& dbi = streams[3];
    put32(dbi, spec.bad_dbi_signature ? 0x12345678u : 0xFFFFFFFFu);
    put32(dbi, 19990903u);
    put32(dbi, spec.age);
    put16(dbi, 0xFFFF);  // global stream
    put16(dbi, 0);
    put16(dbi, 0xFFFF);  // public stream
    put16(dbi, 0);
    put16(dbi, 5);  // symbol record stream
    put16(dbi, 0);
    put32(dbi, static_cast<std::uint32_t>(modinfo.size()));
    put32(dbi, static_cast<std::uint32_t>(contrib.size()));
    put32(dbi, 0);  // section map
    put32(dbi, static_cast<std::uint32_t>(fileinfo.size()));
    put32(dbi, 0);  // type server map
    put32(dbi, 0);  // mfc
    put32(dbi, static_cast<std::uint32_t>(dbghdr.size()));
    put32(dbi, 0);  // ec
    put16(dbi, 0);
    put16(dbi, spec.machine);
    put32(dbi, 0);
    dbi.insert(dbi.end(), modinfo.begin(), modinfo.end());
    dbi.insert(dbi.end(), contrib.begin(), contrib.end());
    dbi.insert(dbi.end(), fileinfo.begin(), fileinfo.end());
    dbi.insert(dbi.end(), dbghdr.begin(), dbghdr.end());

    // ---- MSF layout ----------------------------------------------------------------------
    const std::uint32_t bs = spec.block_size;
    auto blocks_for = [bs](std::size_t n) { return static_cast<std::uint32_t>((n + bs - 1) / bs); };
    std::vector<std::uint32_t> need;
    for (const Bytes& s : streams) {
        need.push_back(blocks_for(s.size()));
    }
    // Directory.
    Bytes dir;
    // (block numbers are known only after allocation; allocate data blocks first)
    std::uint32_t next_block = 3;  // 0 super block, 1-2 free block maps
    std::vector<std::vector<std::uint32_t>> assigned(streams.size());
    if (!spec.fragmented) {
        for (std::size_t i = 0; i < streams.size(); ++i) {
            for (std::uint32_t k = 0; k < need[i]; ++k) {
                assigned[i].push_back(next_block++);
            }
        }
    } else {
        std::uint32_t most = 0;
        for (std::uint32_t n : need) {
            most = std::max(most, n);
        }
        for (std::uint32_t k = 0; k < most; ++k) {
            for (std::size_t i = 0; i < streams.size(); ++i) {
                if (k < need[i]) {
                    assigned[i].push_back(next_block++);
                }
            }
        }
    }
    put32(dir, static_cast<std::uint32_t>(streams.size()));
    for (const Bytes& s : streams) {
        put32(dir, static_cast<std::uint32_t>(s.size()));
    }
    for (const auto& a : assigned) {
        for (std::uint32_t b : a) {
            put32(dir, b);
        }
    }
    const std::uint32_t dir_blocks = blocks_for(dir.size());
    std::vector<std::uint32_t> dir_assigned;
    for (std::uint32_t k = 0; k < dir_blocks; ++k) {
        dir_assigned.push_back(next_block++);
    }
    const std::uint32_t map_block = next_block++;
    const std::uint32_t total_blocks = next_block;

    Bytes file(static_cast<std::size_t>(total_blocks) * bs, 0);
    static const char magic[32] = {'M', 'i', 'c', 'r', 'o', 's', 'o', 'f', 't', ' ', 'C', '/', 'C', '+', '+', ' ',
                                   'M', 'S', 'F', ' ', '7', '.', '0', '0', '\r', '\n', 0x1A, 'D', 'S', 0, 0, 0};
    std::memcpy(file.data(), magic, 32);
    auto poke32 = [&file](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            file[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    };
    poke32(32, bs);
    poke32(36, 1);  // free block map
    poke32(40, total_blocks);
    poke32(44, static_cast<std::uint32_t>(dir.size()));
    poke32(48, 0);
    poke32(52, map_block);
    for (std::size_t i = 0; i < streams.size(); ++i) {
        for (std::uint32_t k = 0; k < need[i]; ++k) {
            const std::size_t from = static_cast<std::size_t>(k) * bs;
            const std::size_t n = std::min<std::size_t>(bs, streams[i].size() - from);
            std::memcpy(file.data() + static_cast<std::size_t>(assigned[i][k]) * bs, streams[i].data() + from, n);
        }
    }
    for (std::uint32_t k = 0; k < dir_blocks; ++k) {
        const std::size_t from = static_cast<std::size_t>(k) * bs;
        const std::size_t n = std::min<std::size_t>(bs, dir.size() - from);
        std::memcpy(file.data() + static_cast<std::size_t>(dir_assigned[k]) * bs, dir.data() + from, n);
        poke32(static_cast<std::size_t>(map_block) * bs + static_cast<std::size_t>(k) * 4, dir_assigned[k]);
    }
    return file;
}

// A minimal PE32+ image whose debug directory holds one RSDS record.
// `section_raw_size` is the section's SizeOfRawData; the debug directory sits at offset 0x10 of the
// section, so a value of 0x10 or less puts it in the section's virtual-only tail.
inline Bytes build_pe(const std::array<std::uint8_t, 16>& guid, std::uint32_t age, const std::string& pdb_path,
                      bool with_debug_dir = true, std::uint32_t section_raw_size = 0x200) {
    Bytes f(0x400, 0);
    auto poke16 = [&f](std::size_t at, std::uint16_t v) {
        f[at] = static_cast<std::uint8_t>(v);
        f[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    auto poke32 = [&f](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            f[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    };
    f[0] = 'M';
    f[1] = 'Z';
    poke32(0x3C, 0x80);
    std::memcpy(&f[0x80], "PE\0\0", 4);
    poke16(0x84, 0x8664);  // machine
    poke16(0x86, 1);       // sections
    poke16(0x94, 240);     // size of optional header
    const std::size_t opt = 0x98;
    poke16(opt, 0x20B);
    poke32(opt + 108, 16);  // NumberOfRvaAndSizes
    if (with_debug_dir) {
        poke32(opt + 112 + 6 * 8, 0x1000 + 0x10);  // debug directory RVA
        poke32(opt + 112 + 6 * 8 + 4, 28);
    }
    const std::size_t sec = opt + 240;
    std::memcpy(&f[sec], ".rdata\0\0", 8);
    poke32(sec + 8, 0x200);   // virtual size
    poke32(sec + 12, 0x1000); // rva
    poke32(sec + 16, section_raw_size);  // raw size
    poke32(sec + 20, 0x200);  // raw pointer
    // Debug directory at file offset 0x200 + 0x10.
    const std::size_t dd = 0x210;
    poke32(dd + 12, 2);  // IMAGE_DEBUG_TYPE_CODEVIEW
    Bytes cv;
    cv.insert(cv.end(), {'R', 'S', 'D', 'S'});
    cv.insert(cv.end(), guid.begin(), guid.end());
    put32(cv, age);
    putstr(cv, pdb_path);
    poke32(dd + 16, static_cast<std::uint32_t>(cv.size()));
    poke32(dd + 20, 0x1000 + 0x40);
    poke32(dd + 24, 0x240);
    if (f.size() < 0x240 + cv.size()) {
        f.resize(0x240 + cv.size());
    }
    std::memcpy(&f[0x240], cv.data(), cv.size());
    return f;
}

}  // namespace cml::testpdb
