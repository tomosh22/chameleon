// Eager part of open(): DBI header, module table, S_COMPILE3 / S_ENVBLOCK heads, source-info lists
// and the inverted file map, section headers and section contributions.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>

#include "cml/pdb/diag_ids.h"
#include "cml/util/log.h"
#include "detail/codeview.h"
#include "detail/le.h"
#include "detail/parallel.h"
#include "index_impl.h"

namespace cml::pdb {

using namespace detail;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

constexpr std::uint32_t kDbiStream = 3;
constexpr std::uint32_t kDbiHeaderSize = 64;
constexpr std::uint32_t kModuleInfoFixed = 64;
constexpr std::uint32_t kHeadScanMax = 65536;

std::string file_label(const PdbIndex::Impl& impl) {
    return "'" + impl.path + "'";
}

Diag bad_dbi(const PdbIndex::Impl& impl, const std::string& why) {
    return Diag::make(diag::kBadDbiStream, "The DBI stream of " + file_label(impl) + " is invalid (" + why + ").",
                      "Rebuild the module so the linker regenerates the PDB. /DEBUG:FASTLINK PDBs are not supported.");
}

// What the head of a module symbol stream told us.
struct ModuleHead {
    bool has_compile3 = false;
    CompileInfo compile;
    std::string version;
    std::string objname;
    bool has_env = false;
    std::vector<std::pair<std::string, std::string>> env;
};

void decode_compile3(const std::uint8_t* b, std::uint32_t len, ModuleHead& h) {
    // flags u32, machine u16, fe[4] u16, be[4] u16, version string
    if (len < 22) {
        return;
    }
    CompileInfo& c = h.compile;
    c.raw_flags = rd32(b);
    c.language = static_cast<std::uint8_t>(c.raw_flags & 0xFF);
    c.edit_and_continue = (c.raw_flags & (1u << 8)) != 0;
    c.no_debug_info = (c.raw_flags & (1u << 9)) != 0;
    c.ltcg = (c.raw_flags & (1u << 10)) != 0;
    c.no_data_align = (c.raw_flags & (1u << 11)) != 0;
    c.managed = (c.raw_flags & (1u << 12)) != 0;
    c.security_checks = (c.raw_flags & (1u << 13)) != 0;
    c.hotpatch = (c.raw_flags & (1u << 14)) != 0;
    c.cvtcil = (c.raw_flags & (1u << 15)) != 0;
    c.msil_module = (c.raw_flags & (1u << 16)) != 0;
    c.sdl = (c.raw_flags & (1u << 17)) != 0;
    c.pgo = (c.raw_flags & (1u << 18)) != 0;
    c.exp_module = (c.raw_flags & (1u << 19)) != 0;
    c.machine = rd16(b + 4);
    for (std::size_t i = 0; i < 4; ++i) {
        c.frontend[i] = rd16(b + 6 + i * 2);
        c.backend[i] = rd16(b + 14 + i * 2);
    }
    h.version.assign(reinterpret_cast<const char*>(b + 22), cv::cstr_len(b + 22, len - 22));
    h.has_compile3 = true;
}

void decode_envblock(const std::uint8_t* b, std::uint32_t len, ModuleHead& h) {
    // flags u8 (bit 0 = edit and continue), then key\0 value\0 ... terminated by an empty key.
    if (len < 1) {
        return;
    }
    std::uint32_t pos = 1;
    h.has_env = true;
    while (pos < len) {
        const std::size_t klen = cv::cstr_len(b + pos, len - pos);
        if (klen == 0 || pos + klen >= len) {
            break;
        }
        const std::uint8_t* key = b + pos;
        pos += static_cast<std::uint32_t>(klen) + 1;
        if (pos >= len) {
            break;
        }
        const std::size_t vlen = cv::cstr_len(b + pos, len - pos);
        h.env.emplace_back(std::string(reinterpret_cast<const char*>(key), klen),
                           std::string(reinterpret_cast<const char*>(b + pos), vlen));
        pos += static_cast<std::uint32_t>(vlen) + 1;
    }
}

void parse_head(const Msf& msf, const Compiland& c, ModuleHead& h) {
    if (c.symbol_stream == 0xFFFF || c.symbol_bytes <= 4) {
        return;
    }
    const std::size_t limit = std::min<std::size_t>(c.symbol_bytes, kHeadScanMax);
    std::size_t want = std::min<std::size_t>(c.symbol_bytes, 16384);
    CountedBuffer scratch;
    for (;;) {
        std::size_t got = 0;
        const std::uint8_t* p = msf.read_range(c.symbol_stream, 0, want, scratch, got);
        if (p == nullptr || got < 8) {
            return;
        }
        h = ModuleHead();
        bool need_more = false;
        bool stop = false;
        std::uint64_t off = 4;  // skip the 4-byte signature
        while (!stop && off + 4 <= got) {
            const std::uint32_t reclen = rd16(p + off);
            if (reclen < 2) {
                return;  // corrupt; the full symbol load reports it
            }
            if (off + 2 + reclen > got) {
                need_more = true;
                break;
            }
            const std::uint16_t kind = rd16(p + off + 2);
            const std::uint8_t* body = p + off + 4;
            const std::uint32_t blen = reclen - 2;
            switch (kind) {
                case cv::S_OBJNAME:
                    if (blen > 4) {
                        h.objname.assign(reinterpret_cast<const char*>(body + 4), cv::cstr_len(body + 4, blen - 4));
                    }
                    break;
                case cv::S_COMPILE3:
                    decode_compile3(body, blen, h);
                    break;
                case cv::S_ENVBLOCK:
                    decode_envblock(body, blen, h);
                    break;
                default:
                    // Anything that starts real content ends the header area.
                    if (cv::is_scope_opener(kind) || kind == cv::S_LDATA32 || kind == cv::S_GDATA32 ||
                        kind == cv::S_UDT || kind == cv::S_LTHREAD32 || kind == cv::S_GTHREAD32) {
                        stop = true;
                    }
                    break;
            }
            if (h.has_compile3 && h.has_env) {
                stop = true;
            }
            off += 2 + reclen;
        }
        if (!need_more || stop || want >= limit) {
            return;
        }
        want = std::min(limit, want * 4);
    }
}

}  // namespace

std::optional<std::uint32_t> PdbIndex::Impl::section_rva(std::uint16_t section, std::uint32_t offset) const noexcept {
    if (section == 0 || section > sections.size()) {
        return std::nullopt;
    }
    const std::uint64_t rva = static_cast<std::uint64_t>(sections[static_cast<std::size_t>(section) - 1].rva) + offset;
    if (rva >= kNoRva) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(rva);
}

Result<void> build_index(PdbIndex::Impl& impl) {
    const auto t_total = Clock::now();
    Msf& msf = *impl.msf;

    // GUID and age come from probe_pdb(), which already read the info stream: impl.id is set.

    // ---- DBI header -----------------------------------------------------------------------
    auto t = Clock::now();
    impl.dbi = msf.read_stream(kDbiStream);
    const std::uint8_t* dbi = impl.dbi.data();
    const std::size_t dbi_size = impl.dbi.size();
    if (dbi_size < kDbiHeaderSize) {
        return bad_dbi(impl, "stream missing or shorter than its header");
    }
    if (rd32(dbi) != 0xFFFFFFFFu) {
        return bad_dbi(impl, "bad version signature");
    }
    const std::uint32_t version = rd32(dbi + 4);
    if (version != 19990903u && version != 20091201u) {
        return bad_dbi(impl, "unsupported DBI version " + std::to_string(version));
    }
    impl.dbi_age = rd32(dbi + 8);
    impl.symrec_stream = rd16(dbi + 20);
    const std::uint32_t mod_size = rd32(dbi + 24);
    const std::uint32_t sc_size = rd32(dbi + 28);
    const std::uint32_t secmap_size = rd32(dbi + 32);
    const std::uint32_t fileinfo_size = rd32(dbi + 36);
    const std::uint32_t typeserver_size = rd32(dbi + 40);
    const std::uint32_t ec_size = rd32(dbi + 52);
    const std::uint32_t dbghdr_size = rd32(dbi + 48);
    impl.machine = rd16(dbi + 58);

    const std::uint64_t mod_off = kDbiHeaderSize;
    const std::uint64_t sc_off = mod_off + mod_size;
    const std::uint64_t secmap_off = sc_off + sc_size;
    const std::uint64_t fileinfo_off = secmap_off + secmap_size;
    const std::uint64_t ts_off = fileinfo_off + fileinfo_size;
    const std::uint64_t ec_off = ts_off + typeserver_size;
    const std::uint64_t dbghdr_off = ec_off + ec_size;
    const std::uint64_t dbi_end = dbghdr_off + dbghdr_size;
    if ((mod_size | sc_size | secmap_size | fileinfo_size | typeserver_size | ec_size | dbghdr_size) > 0x7FFFFFFFu ||
        dbi_end > dbi_size) {
        return bad_dbi(impl, "substream sizes exceed the stream");
    }
    impl.open_timings.dbi_ms = ms_since(t);

    // ---- Module table ---------------------------------------------------------------------
    t = Clock::now();
    struct ModRaw {
        const char* name;
        std::uint32_t name_len;
        const char* obj;
        std::uint32_t obj_len;
    };
    std::vector<ModRaw> raws;
    {
        raws.reserve(mod_size / 160 + 1);
        std::uint64_t off = mod_off;
        while (off < sc_off) {
            const std::uint64_t avail = sc_off - off;
            if (avail < kModuleInfoFixed + 2) {
                return Diag::make(diag::kBadModuleInfo,
                                  "The module table of " + file_label(impl) + " ends inside a module record.",
                                  "Rebuild the module so the linker regenerates the PDB.");
            }
            const std::uint8_t* e = dbi + off;
            const std::uint8_t* name = e + kModuleInfoFixed;
            const std::size_t name_len = cv::cstr_len(name, avail - kModuleInfoFixed);
            const std::uint64_t obj_pos = kModuleInfoFixed + name_len + 1;
            if (obj_pos >= avail) {
                return Diag::make(diag::kBadModuleInfo,
                                  "Module name " + std::to_string(raws.size()) + " in " + file_label(impl) + " is not terminated.",
                                  "Rebuild the module so the linker regenerates the PDB.");
            }
            const std::uint8_t* obj = e + obj_pos;
            const std::size_t obj_len = cv::cstr_len(obj, avail - obj_pos);
            if (obj_pos + obj_len >= avail) {
                return Diag::make(diag::kBadModuleInfo,
                                  "Object name of module " + std::to_string(raws.size()) + " in " + file_label(impl) +
                                      " is not terminated.",
                                  "Rebuild the module so the linker regenerates the PDB.");
            }
            Compiland c;
            c.index = static_cast<std::uint32_t>(impl.compilands.size());
            c.symbol_stream = rd16(e + 34);
            c.symbol_bytes = rd32(e + 36);
            c.file_count = rd16(e + 48);
            impl.compilands.push_back(std::move(c));
            raws.push_back(ModRaw{reinterpret_cast<const char*>(name), static_cast<std::uint32_t>(name_len),
                                  reinterpret_cast<const char*>(obj), static_cast<std::uint32_t>(obj_len)});
            off += (kModuleInfoFixed + name_len + 1 + obj_len + 1 + 3) & ~static_cast<std::uint64_t>(3);
        }
    }
    const std::size_t module_count = impl.compilands.size();

    // Heads (S_OBJNAME, S_COMPILE3, S_ENVBLOCK) of every module stream, in parallel.
    std::vector<ModuleHead> heads(module_count);
    {
        const unsigned threads = module_count < 64 ? 1u : impl.options.threads;
        parallel_for(module_count, threads, [&](std::size_t i) { parse_head(msf, impl.compilands[i], heads[i]); });
    }

    // Assemble the text pool (module names, versions, env strings) and the compiland views.
    {
        std::size_t total = 0;
        for (std::size_t i = 0; i < module_count; ++i) {
            const ModRaw& r = raws[i];
            const ModuleHead& h = heads[i];
            total += r.name_len + 1;
            if (r.obj_len != r.name_len || std::memcmp(r.obj, r.name, r.name_len) != 0) {
                total += r.obj_len + 1;
            }
            total += h.objname.size() + 1 + h.version.size() + 1;
            for (const auto& kv : h.env) {
                total += kv.first.size() + kv.second.size() + 2;
            }
        }
        if (total > 0xFFFFFFF0u) {
            return Diag::make(diag::kCapacityExceeded, "The module names of " + file_label(impl) + " exceed 4 GiB.",
                              "This PDB is too large for the index; split the module.");
        }
        impl.text.reserve(total);
        auto put = [&impl](const char* s, std::size_t n) -> std::string_view {
            const std::size_t at = impl.text.size();
            impl.text.insert(impl.text.end(), s, s + n);
            impl.text.push_back('\0');
            return std::string_view(impl.text.data() + at, n);
        };
        for (std::size_t i = 0; i < module_count; ++i) {
            const ModRaw& r = raws[i];
            ModuleHead& h = heads[i];
            Compiland& c = impl.compilands[i];
            c.object_name = put(r.name, r.name_len);
            if (r.obj_len != r.name_len || std::memcmp(r.obj, r.name, r.name_len) != 0) {
                c.library_name = put(r.obj, r.obj_len);
            }
            c.objname_record = put(h.objname.data(), h.objname.size());
            c.has_compile3 = h.has_compile3;
            c.compile = h.compile;
            c.compile.version = put(h.version.data(), h.version.size());
            if (h.has_env) {
                c.env_source = EnvSource::EnvBlock;
                c.env.reserve(h.env.size());
                for (const auto& kv : h.env) {
                    const std::string_view k = put(kv.first.data(), kv.first.size());
                    const std::string_view v = put(kv.second.data(), kv.second.size());
                    c.env.emplace_back(k, v);
                }
            }
        }
    }
    impl.open_timings.modules_ms = ms_since(t);

    // ---- Source info: per-compiland file lists and the inverted file map -----------------
    t = Clock::now();
    impl.compiland_file_start.assign(module_count + 1, 0);
    // file_count describes what compiland_files() returns: the module record's own count is only a
    // hint, and modules the source-info table does not cover have no list.
    for (Compiland& c : impl.compilands) {
        c.file_count = 0;
    }
    if (fileinfo_size >= 4) {
        const std::uint8_t* fi = dbi + fileinfo_off;
        const std::uint32_t fi_modules = rd16(fi);
        if (fi_modules > module_count) {
            return Diag::make(diag::kBadSourceInfo,
                              "The source-info table of " + file_label(impl) + " lists more modules than the module table.",
                              "Rebuild the module so the linker regenerates the PDB.");
        }
        const std::uint64_t counts_off = 4 + static_cast<std::uint64_t>(fi_modules) * 2;  // after mod_start[]
        const std::uint64_t names_ptr_off = counts_off + static_cast<std::uint64_t>(fi_modules) * 2;
        if (names_ptr_off > fileinfo_size) {
            return Diag::make(diag::kBadSourceInfo, "The source-info table of " + file_label(impl) + " is truncated.",
                              "Rebuild the module so the linker regenerates the PDB.");
        }
        std::uint64_t total_files = 0;
        for (std::uint32_t m = 0; m < fi_modules; ++m) {
            total_files += rd16(fi + counts_off + static_cast<std::uint64_t>(m) * 2);
        }
        const std::uint64_t names_off = names_ptr_off + total_files * 4;
        if (names_off > fileinfo_size) {
            return Diag::make(diag::kBadSourceInfo, "The source-info file list of " + file_label(impl) + " is truncated.",
                              "Rebuild the module so the linker regenerates the PDB.");
        }
        impl.source_names = fi + names_off;
        impl.source_names_len = static_cast<std::size_t>(fileinfo_size - names_off);

        using OffsetMap = std::unordered_map<std::uint32_t, std::uint32_t, std::hash<std::uint32_t>,
                                             std::equal_to<std::uint32_t>,
                                             CountingAllocator<std::pair<const std::uint32_t, std::uint32_t>>>;
        OffsetMap by_offset;
        by_offset.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(total_files, 1u << 20)));
        impl.compiland_files.reserve(static_cast<std::size_t>(total_files));

        std::uint64_t cursor = 0;
        for (std::uint32_t m = 0; m < module_count; ++m) {
            impl.compiland_file_start[m] = static_cast<std::uint32_t>(impl.compiland_files.size());
            if (m >= fi_modules) {
                continue;
            }
            const std::uint32_t count = rd16(fi + counts_off + static_cast<std::uint64_t>(m) * 2);
            // Relative names resolve against the compiland's cwd: S_ENVBLOCK's, else (clang-cl and cl
            // record only S_BUILDINFO) the one in S_BUILDINFO, which is looked up on first need.
            const std::optional<std::string_view> env_cwd = impl.compilands[m].cwd();
            std::optional<std::string> info_cwd;
            bool info_cwd_tried = false;
            for (std::uint32_t k = 0; k < count; ++k, ++cursor) {
                const std::uint32_t name_off = rd32(fi + names_ptr_off + cursor * 4);
                if (name_off >= impl.source_names_len) {
                    return Diag::make(diag::kBadSourceInfo,
                                      "A file name offset in the source-info table of " + file_label(impl) + " is out of range.",
                                      "Rebuild the module so the linker regenerates the PDB.");
                }
                const std::size_t avail = impl.source_names_len - name_off;
                const std::size_t len = cv::cstr_len(impl.source_names + name_off, avail);
                if (len == avail) {
                    return Diag::make(diag::kBadSourceInfo,
                                      "A file name in the source-info table of " + file_label(impl) + " is not terminated.",
                                      "Rebuild the module so the linker regenerates the PDB.");
                }
                const std::string_view raw(reinterpret_cast<const char*>(impl.source_names + name_off), len);
                const bool absolute = is_absolute_path(raw);
                std::uint32_t entry_id;
                const auto found = absolute ? by_offset.find(name_off) : by_offset.end();
                if (found != by_offset.end()) {
                    entry_id = found->second;
                } else {
                    std::string_view use_cwd;
                    if (!absolute) {
                        if (env_cwd) {
                            use_cwd = *env_cwd;
                        } else {
                            if (!info_cwd_tried) {
                                info_cwd_tried = true;
                                info_cwd = buildinfo_cwd(impl, m);
                            }
                            if (info_cwd) {
                                use_cwd = *info_cwd;
                            }
                        }
                    }
                    const std::string norm = normalize_path(raw, use_cwd);
                    if (impl.file_pool.size() + norm.size() >= 0xFFFFFFF0u || impl.file_entries.size() >= 0xFFFFFFF0u) {
                        return Diag::make(diag::kCapacityExceeded, "The file table of " + file_label(impl) + " exceeds 4 GiB.",
                                          "This PDB is too large for the index.");
                    }
                    FileEntry fe;
                    fe.raw_off = name_off;
                    fe.raw_len = static_cast<std::uint32_t>(len);
                    fe.norm_off = static_cast<std::uint32_t>(impl.file_pool.size());
                    fe.norm_len = static_cast<std::uint32_t>(norm.size());
                    impl.file_pool.insert(impl.file_pool.end(), norm.begin(), norm.end());
                    entry_id = static_cast<std::uint32_t>(impl.file_entries.size());
                    impl.file_entries.push_back(fe);
                    if (absolute) {
                        by_offset.emplace(name_off, entry_id);
                    }
                }
                impl.compiland_files.push_back(entry_id);
            }
            impl.compilands[m].file_count = count;
        }
        impl.compiland_file_start[module_count] = static_cast<std::uint32_t>(impl.compiland_files.size());
    }

    // Distinct files, sorted by normalised path, with compiland lists in CSR form.
    {
        const std::size_t entries = impl.file_entries.size();
        const char* pool = impl.file_pool.data();
        auto norm_of = [&](std::uint32_t e) {
            return std::string_view(pool + impl.file_entries[e].norm_off, impl.file_entries[e].norm_len);
        };
        Vec<std::uint32_t> order(entries);
        for (std::size_t i = 0; i < entries; ++i) {
            order[i] = static_cast<std::uint32_t>(i);
        }
        std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return norm_of(a) < norm_of(b); });
        Vec<std::uint32_t> uid_of(entries);
        for (std::size_t i = 0; i < entries; ++i) {
            if (i == 0 || norm_of(order[i]) != norm_of(order[i - 1])) {
                impl.unique_files.push_back(order[i]);
            }
            uid_of[order[i]] = static_cast<std::uint32_t>(impl.unique_files.size() - 1);
        }
        Vec<std::uint64_t> pairs;
        pairs.reserve(impl.compiland_files.size());
        for (std::size_t m = 0; m < module_count; ++m) {
            for (std::uint32_t k = impl.compiland_file_start[m]; k < impl.compiland_file_start[m + 1]; ++k) {
                pairs.push_back((static_cast<std::uint64_t>(uid_of[impl.compiland_files[k]]) << 32) | m);
            }
        }
        std::sort(pairs.begin(), pairs.end());
        pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
        impl.unique_file_start.assign(impl.unique_files.size() + 1, 0);
        impl.unique_file_compilands.reserve(pairs.size());
        for (const std::uint64_t p : pairs) {
            ++impl.unique_file_start[static_cast<std::size_t>(p >> 32) + 1];
            impl.unique_file_compilands.push_back(static_cast<std::uint32_t>(p & 0xFFFFFFFFu));
        }
        for (std::size_t i = 1; i < impl.unique_file_start.size(); ++i) {
            impl.unique_file_start[i] += impl.unique_file_start[i - 1];
        }
    }
    impl.open_timings.files_ms = ms_since(t);

    // ---- Section headers and contributions -----------------------------------------------
    t = Clock::now();
    if (dbghdr_size >= 12) {
        // Optional debug header: u16 stream indexes; [5] = section headers.
        const std::uint16_t sec_stream = rd16(dbi + dbghdr_off + 5 * 2);
        if (sec_stream != 0xFFFF && msf.stream_exists(sec_stream)) {
            const StreamData sd = msf.read_stream(sec_stream);
            const std::size_t n = sd.size() / 40;
            impl.sections.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint8_t* s = sd.data() + i * 40;
                SectionHeader h;
                std::memcpy(h.name.data(), s, 8);
                h.name[8] = '\0';
                h.virtual_size = rd32(s + 8);
                h.rva = rd32(s + 12);
                h.raw_size = rd32(s + 16);
                h.characteristics = rd32(s + 36);
                impl.sections.push_back(h);
            }
        }
    }
    if (impl.sections.empty()) {
        CML_LOG(Debug, "cml_pdb: %s has no section header stream; RVAs are unavailable", impl.path.c_str());
    }
    if (sc_size >= 4) {
        const std::uint8_t* sc = dbi + sc_off;
        const std::uint32_t ver = rd32(sc);
        std::uint32_t entry_size;
        if (ver == 0xEFFE0000u + 19970605u) {
            entry_size = 28;
        } else if (ver == 0xEFFE0000u + 20140516u) {
            entry_size = 32;
        } else {
            return Diag::make(diag::kBadSectionContributions,
                              "The section contribution table of " + file_label(impl) + " has unknown version " +
                                  std::to_string(ver) + ".",
                              "Rebuild the module with a supported linker (MSVC link.exe or lld-link).");
        }
        if ((sc_size - 4) % entry_size != 0) {
            return Diag::make(diag::kBadSectionContributions,
                              "The section contribution table of " + file_label(impl) + " has a partial entry.",
                              "Rebuild the module so the linker regenerates the PDB.");
        }
        const std::size_t n = (sc_size - 4) / entry_size;
        impl.contributions.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t* e = sc + 4 + i * entry_size;
            const std::uint16_t section = rd16(e);
            const std::uint32_t offset = rd32(e + 4);
            const std::uint32_t size = rd32(e + 8);
            const std::uint16_t module = rd16(e + 16);
            if (section == 0 || section > impl.sections.size() || module >= module_count || size == 0 || size > 0x7FFFFFFFu ||
                offset > 0x7FFFFFFFu) {
                continue;  // linker-internal or unmappable contribution
            }
            const auto rva = impl.section_rva(section, offset);
            if (!rva) {
                continue;
            }
            CompactContribution c;
            c.section = section;
            c.size = size;
            c.compiland = module;
            c.rva = *rva;
            impl.contributions.push_back(c);
        }
        std::stable_sort(impl.contributions.begin(), impl.contributions.end(),
                         [](const CompactContribution& a, const CompactContribution& b) { return a.rva < b.rva; });
    }
    impl.open_timings.sections_ms = ms_since(t);

    // Release the DBI stream: everything in it has been parsed into compact tables except the
    // source-info names block, which the file lists still point into.
    if (impl.dbi.owned()) {
        if (impl.source_names_len > 0) {
            const char* names = reinterpret_cast<const char*>(impl.source_names);
            impl.names_copy.assign(names, names + impl.source_names_len);
            impl.source_names = reinterpret_cast<const std::uint8_t*>(impl.names_copy.data());
        } else {
            impl.source_names = nullptr;
        }
        impl.dbi = StreamData();
    }
    impl.open_timings.open_total_ms = ms_since(t_total) + impl.open_timings.map_ms + impl.open_timings.msf_ms;
    return Result<void>();
}

}  // namespace cml::pdb
