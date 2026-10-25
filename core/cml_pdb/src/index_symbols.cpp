// Lazy symbol loading: per-compiland module symbol streams, the global symbol record stream
// (publics and global data), the IPI-based S_BUILDINFO environment fallback, and the symbol queries.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

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

std::uint64_t ns_since(Clock::time_point t0) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}

std::uint32_t rva_for(const std::vector<SectionHeader>& sections, std::uint16_t section, std::uint32_t offset) noexcept {
    if (section == 0 || section > sections.size()) {
        return kNoRva;
    }
    const std::uint64_t rva = static_cast<std::uint64_t>(sections[static_cast<std::size_t>(section) - 1].rva) + offset;
    return rva >= kNoRva ? kNoRva : static_cast<std::uint32_t>(rva);
}

void sort_by_rva(Vec<RvaIndex>& v) {
    std::sort(v.begin(), v.end(), [](const RvaIndex& a, const RvaIndex& b) {
        return a.rva != b.rva ? a.rva < b.rva : a.index < b.index;
    });
}

std::string_view sym_name(const SymRec& s, const char* base) noexcept {
    return std::string_view(base + s.name_off, s.name_len);
}

std::string_view pub_name(const PubRec& s, const char* base) noexcept {
    return std::string_view(base + s.name_off, s.name_len);
}

constexpr std::uint8_t kProcKind = static_cast<std::uint8_t>(SymbolKind::GlobalProc);
constexpr std::uint8_t kLocalProcKind = static_cast<std::uint8_t>(SymbolKind::LocalProc);

bool is_proc_rec(const SymRec& s) noexcept {
    return s.kind == kProcKind || s.kind == kLocalProcKind;
}

// What the module record walker reports for each symbol it keeps.
struct Found {
    SymbolKind kind;
    const std::uint8_t* name;
    std::size_t name_len;
    std::uint16_t section;
    std::uint32_t offset;
    std::uint32_t size;
    std::uint32_t type_index;
    std::uint8_t flags;
    std::uint32_t parent;
};

// Walks the symbol area of a module stream. `emit(const Found&)` returns the index given to the
// symbol (used as the parent of function-local symbols). Returns false if a record is malformed.
template <class Emit>
bool walk_module_records(const std::uint8_t* data, std::uint64_t len, std::uint32_t& buildinfo_id, Emit&& emit) {
    std::vector<std::uint32_t> scope_proc;  // parallel to the open scopes: enclosing proc index or kNoSymbol
    return cv::for_each_record(data, 4, len, [&](const cv::Record& r) {
        const std::uint16_t k = r.kind;
        const std::uint8_t* b = r.body;
        const std::uint32_t n = r.body_len;
        if (cv::is_proc(k)) {
            scope_proc.push_back(kNoSymbol);
            if (n < 36) {
                return;
            }
            const std::uint32_t parent = scope_proc.size() >= 2 ? scope_proc[scope_proc.size() - 2] : kNoSymbol;
            Found f;
            f.kind = cv::is_global_proc(k) ? SymbolKind::GlobalProc : SymbolKind::LocalProc;
            f.name = b + 35;
            f.name_len = cv::cstr_len(b + 35, n - 35);
            f.section = rd16(b + 32);
            f.offset = rd32(b + 28);
            f.size = rd32(b + 12);
            f.type_index = rd32(b + 24);
            f.flags = b[34];
            f.parent = parent;
            scope_proc.back() = emit(f);
        } else if (cv::is_scope_opener(k)) {
            scope_proc.push_back(scope_proc.empty() ? kNoSymbol : scope_proc.back());
        } else if (cv::is_scope_closer(k)) {
            if (!scope_proc.empty()) {
                scope_proc.pop_back();
            }
        } else if (k == cv::S_LDATA32 || k == cv::S_GDATA32 || k == cv::S_LTHREAD32 || k == cv::S_GTHREAD32) {
            if (n < 11) {
                return;
            }
            Found f;
            f.kind = SymbolKind::LocalData;
            if (k == cv::S_GDATA32) {
                f.kind = SymbolKind::GlobalData;
            } else if (k == cv::S_LTHREAD32) {
                f.kind = SymbolKind::LocalThread;
            } else if (k == cv::S_GTHREAD32) {
                f.kind = SymbolKind::GlobalThread;
            }
            f.name = b + 10;
            f.name_len = cv::cstr_len(b + 10, n - 10);
            f.section = rd16(b + 8);
            f.offset = rd32(b + 4);
            f.size = 0;
            f.type_index = rd32(b);
            f.flags = 0;
            f.parent = scope_proc.empty() ? kNoSymbol : scope_proc.back();
            emit(f);
        } else if (k == cv::S_UDT) {
            if (n < 5) {
                return;
            }
            Found f;
            f.kind = SymbolKind::Udt;
            f.name = b + 4;
            f.name_len = cv::cstr_len(b + 4, n - 4);
            f.section = 0;
            f.offset = 0;
            f.size = 0;
            f.type_index = rd32(b);
            f.flags = 0;
            f.parent = scope_proc.empty() ? kNoSymbol : scope_proc.back();
            emit(f);
        } else if (k == cv::S_BUILDINFO) {
            if (n >= 4) {
                buildinfo_id = rd32(b);
            }
        }
    });
}

}  // namespace

namespace detail {

void ModuleSymbols::ensure_names() const {
    if (names_ready.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(lazy_mutex);
    if (names_ready.load(std::memory_order_relaxed)) {
        return;
    }
    const char* base = pool.data();
    names.build(syms.size(), [&](std::size_t i) { return sym_name(syms[i], base); });
    names_ready.store(true, std::memory_order_release);
}

void ModuleSymbols::ensure_procs(const std::vector<SectionHeader>& sections) const {
    if (procs_ready.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(lazy_mutex);
    if (procs_ready.load(std::memory_order_relaxed)) {
        return;
    }
    std::size_t count = 0;
    for (const SymRec& s : syms) {
        count += (is_proc_rec(s) && rva_for(sections, s.section, s.offset) != kNoRva) ? 1 : 0;
    }
    procs_by_rva.reserve(count);
    for (std::size_t i = 0; i < syms.size(); ++i) {
        const SymRec& s = syms[i];
        if (is_proc_rec(s)) {
            const std::uint32_t rva = rva_for(sections, s.section, s.offset);
            if (rva != kNoRva) {
                procs_by_rva.push_back(RvaIndex{rva, static_cast<std::uint32_t>(i)});
            }
        }
    }
    sort_by_rva(procs_by_rva);
    procs_ready.store(true, std::memory_order_release);
}

void GlobalSymbols::ensure_pub_names() const {
    if (pub_names_ready.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(lazy_mutex);
    if (pub_names_ready.load(std::memory_order_relaxed)) {
        return;
    }
    const char* base = reinterpret_cast<const char*>(stream.data());
    pub_names.build(pubs.size(), [&](std::size_t i) { return pub_name(pubs[i], base); });
    pub_names_ready.store(true, std::memory_order_release);
}

void GlobalSymbols::ensure_glob_names() const {
    if (glob_names_ready.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(lazy_mutex);
    if (glob_names_ready.load(std::memory_order_relaxed)) {
        return;
    }
    const char* base = reinterpret_cast<const char*>(stream.data());
    glob_names.build(globs.size(), [&](std::size_t i) { return pub_name(globs[i], base); });
    glob_names_ready.store(true, std::memory_order_release);
}

void GlobalSymbols::ensure_pub_rva(const std::vector<SectionHeader>& sections) const {
    if (pub_rva_ready.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(lazy_mutex);
    if (pub_rva_ready.load(std::memory_order_relaxed)) {
        return;
    }
    std::size_t count = 0;
    for (const PubRec& p : pubs) {
        count += rva_for(sections, p.section, p.offset) != kNoRva ? 1 : 0;
    }
    pubs_by_rva.reserve(count);
    for (std::size_t i = 0; i < pubs.size(); ++i) {
        const std::uint32_t rva = rva_for(sections, pubs[i].section, pubs[i].offset);
        if (rva != kNoRva) {
            pubs_by_rva.push_back(RvaIndex{rva, static_cast<std::uint32_t>(i)});
        }
    }
    sort_by_rva(pubs_by_rva);
    pub_rva_ready.store(true, std::memory_order_release);
}

Result<std::unique_ptr<ModuleSymbols>> parse_module_symbols(const PdbIndex::Impl& impl, std::uint32_t compiland) {
    const Compiland& c = impl.compilands[compiland];
    auto out = std::make_unique<ModuleSymbols>();
    if (c.symbol_stream == 0xFFFF || c.symbol_bytes <= 4) {
        return out;
    }
    auto corrupt = [&](const char* why) {
        return Diag::make(diag::kBadModuleSymbols,
                          "The symbols of module '" + std::string(c.object_name) + "' in '" + impl.path + "' are corrupt (" + why + ").",
                          "Rebuild the module so the linker regenerates the PDB.");
    };
    const std::uint32_t stream_size = impl.msf->stream_size(c.symbol_stream);
    if (c.symbol_bytes > stream_size) {
        return corrupt("symbol area larger than its stream");
    }
    CountedBuffer scratch;
    std::size_t got = 0;
    const std::uint8_t* data = impl.msf->read_range(c.symbol_stream, 0, c.symbol_bytes, scratch, got);
    if (data == nullptr || got < c.symbol_bytes) {
        return corrupt("short read");
    }

    ModuleSymbols& ms = *out;

    // Pass 1: exact sizes, so the vectors are allocated once with no slack.
    std::size_t count = 0;
    std::uint64_t pool_bytes = 0;
    std::uint32_t buildinfo = 0;
    if (!walk_module_records(data, c.symbol_bytes, buildinfo, [&](const Found& f) -> std::uint32_t {
            pool_bytes += f.name_len;
            return static_cast<std::uint32_t>(count++);
        })) {
        return corrupt("record leaves the symbol area");
    }
    if (pool_bytes >= 0xFFFFFFF0u || count >= kNoSymbol) {
        return Diag::make(diag::kCapacityExceeded, "The symbol names of module '" + std::string(c.object_name) + "' exceed 4 GiB.",
                          "This PDB is too large for the index.");
    }
    ms.syms.reserve(count);
    ms.pool.reserve(static_cast<std::size_t>(pool_bytes));

    // Pass 2: fill.
    walk_module_records(data, c.symbol_bytes, ms.buildinfo_id, [&](const Found& f) -> std::uint32_t {
        SymRec s;
        s.name_off = static_cast<std::uint32_t>(ms.pool.size());
        s.name_len = static_cast<std::uint32_t>(f.name_len);
        ms.pool.insert(ms.pool.end(), reinterpret_cast<const char*>(f.name), reinterpret_cast<const char*>(f.name) + f.name_len);
        s.offset = f.offset;
        s.size = f.size;
        s.type_index = f.type_index;
        s.parent = f.parent;
        s.section = f.section;
        s.kind = static_cast<std::uint8_t>(f.kind);
        s.flags = f.flags;
        ms.syms.push_back(s);
        return static_cast<std::uint32_t>(ms.syms.size() - 1);
    });
    return out;
}

Result<std::unique_ptr<GlobalSymbols>> parse_global_symbols(const PdbIndex::Impl& impl) {
    auto out = std::make_unique<GlobalSymbols>();
    GlobalSymbols& g = *out;
    if (impl.symrec_stream == 0xFFFF || !impl.msf->stream_exists(impl.symrec_stream)) {
        return out;
    }
    g.stream = impl.msf->read_stream(impl.symrec_stream);
    const std::uint8_t* data = g.stream.data();
    const std::size_t size = g.stream.size();

    auto handle = [&](const cv::Record& r, bool fill) {
        const std::uint8_t* b = r.body;
        const std::uint32_t n = r.body_len;
        if (r.kind == cv::S_PUB32) {
            if (n < 11) {
                return 0;
            }
            if (fill) {
                PubRec s;
                s.name_off = static_cast<std::uint32_t>(b + 10 - data);
                s.name_len = static_cast<std::uint32_t>(cv::cstr_len(b + 10, n - 10));
                s.flags = static_cast<std::uint8_t>(rd32(b) & 0xFF);
                s.offset = rd32(b + 4);
                s.section = rd16(b + 8);
                s.kind = static_cast<std::uint8_t>(SymbolKind::Public);
                g.pubs.push_back(s);
            }
            return 1;
        }
        if (r.kind == cv::S_GDATA32 || r.kind == cv::S_GTHREAD32) {
            if (n < 11) {
                return 0;
            }
            if (fill) {
                PubRec s;
                s.name_off = static_cast<std::uint32_t>(b + 10 - data);
                s.name_len = static_cast<std::uint32_t>(cv::cstr_len(b + 10, n - 10));
                s.type_index = rd32(b);
                s.offset = rd32(b + 4);
                s.section = rd16(b + 8);
                s.kind = static_cast<std::uint8_t>(r.kind == cv::S_GDATA32 ? SymbolKind::GlobalData : SymbolKind::GlobalThread);
                g.globs.push_back(s);
            }
            return 2;
        }
        return 0;
    };

    auto corrupt = [&]() {
        return Diag::make(diag::kBadSymbolRecords,
                          "The global symbol record stream of '" + impl.path + "' is corrupt (a record leaves the stream).",
                          "Rebuild the module so the linker regenerates the PDB.");
    };
    if (size >= 0xFFFFFFF0u) {
        return Diag::make(diag::kCapacityExceeded, "The global symbol record stream of '" + impl.path + "' exceeds 4 GiB.",
                          "This PDB is too large for the index.");
    }
    std::size_t npub = 0;
    std::size_t nglob = 0;
    if (!cv::for_each_record(data, 0, size, [&](const cv::Record& r) {
            const int kind = handle(r, false);
            npub += kind == 1 ? 1 : 0;
            nglob += kind == 2 ? 1 : 0;
        })) {
        return corrupt();
    }
    g.pubs.reserve(npub);
    g.globs.reserve(nglob);
    cv::for_each_record(data, 0, size, [&](const cv::Record& r) { handle(r, true); });
    return out;
}

}  // namespace detail

Result<void> PdbIndex::Impl::ensure_publics() const {
    if (publics_state.load(std::memory_order_acquire) == static_cast<std::uint8_t>(LazyState::Ready)) {
        return Result<void>();
    }
    std::lock_guard<std::mutex> lock(mutex);
    const std::uint8_t state = publics_state.load(std::memory_order_acquire);
    if (state == static_cast<std::uint8_t>(LazyState::Ready)) {
        return Result<void>();
    }
    if (state == static_cast<std::uint8_t>(LazyState::Failed)) {
        return *publics_error;
    }
    const auto t0 = Clock::now();
    auto parsed = parse_global_symbols(*this);
    publics_ns.fetch_add(ns_since(t0), std::memory_order_relaxed);
    if (!parsed) {
        publics_error = parsed.error();
        publics_state.store(static_cast<std::uint8_t>(LazyState::Failed), std::memory_order_release);
        return *publics_error;
    }
    globals = std::move(parsed).value();
    publics_state.store(static_cast<std::uint8_t>(LazyState::Ready), std::memory_order_release);
    return Result<void>();
}

Result<void> PdbIndex::Impl::ensure_compiland(std::uint32_t index) const {
    if (index >= compilands.size()) {
        return Diag::make(diag::kCompilandOutOfRange,
                          "Compiland index " + std::to_string(index) + " is out of range (" + std::to_string(compilands.size()) +
                              " compilands in '" + path + "').",
                          "Use an index below PdbIndex::compiland_count().");
    }
    const std::uint8_t state = module_state[index].load(std::memory_order_acquire);
    if (state == static_cast<std::uint8_t>(LazyState::Ready)) {
        return Result<void>();
    }
    if (state == static_cast<std::uint8_t>(LazyState::Failed)) {
        return *module_errors[index];
    }
    const auto t0 = Clock::now();
    auto parsed = parse_module_symbols(*this, index);
    module_ns.fetch_add(ns_since(t0), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex);
    if (module_state[index].load(std::memory_order_acquire) == static_cast<std::uint8_t>(LazyState::NotLoaded)) {
        if (parsed) {
            module_symbols[index] = std::move(parsed).value();
            module_state[index].store(static_cast<std::uint8_t>(LazyState::Ready), std::memory_order_release);
        } else {
            module_errors[index] = parsed.error();
            module_state[index].store(static_cast<std::uint8_t>(LazyState::Failed), std::memory_order_release);
        }
    }
    if (module_state[index].load(std::memory_order_acquire) == static_cast<std::uint8_t>(LazyState::Failed)) {
        return *module_errors[index];
    }
    return Result<void>();
}

// ---- IPI / S_BUILDINFO ------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kIpiStream = 4;

Result<void> ensure_ipi(const PdbIndex::Impl& impl) {
    if (impl.ipi_state.load(std::memory_order_acquire) == static_cast<std::uint8_t>(LazyState::Ready)) {
        return Result<void>();
    }
    std::lock_guard<std::mutex> lock(impl.mutex);
    const std::uint8_t state = impl.ipi_state.load(std::memory_order_acquire);
    if (state == static_cast<std::uint8_t>(LazyState::Ready)) {
        return Result<void>();
    }
    if (state == static_cast<std::uint8_t>(LazyState::Failed)) {
        return *impl.ipi_error;
    }
    const auto t0 = Clock::now();
    auto fail = [&](const char* why) -> Result<void> {
        impl.ipi_error = Diag::make(diag::kBadBuildInfo,
                                    "The IPI stream of '" + impl.path + "' is invalid (" + why + "); build information is unavailable.",
                                    "Rebuild the module so the linker regenerates the PDB.");
        impl.ipi_state.store(static_cast<std::uint8_t>(LazyState::Failed), std::memory_order_release);
        return *impl.ipi_error;
    };
    auto table = std::make_unique<IpiTable>();
    table->data = impl.msf->read_stream(kIpiStream);
    const std::uint8_t* d = table->data.data();
    const std::size_t size = table->data.size();
    if (size < 56) {
        return fail("stream missing or shorter than its header");
    }
    const std::uint32_t header_size = rd32(d + 4);
    table->type_begin = rd32(d + 8);
    const std::uint32_t record_bytes = rd32(d + 16);
    if (header_size < 56 || header_size > size || record_bytes > size - header_size) {
        return fail("header sizes exceed the stream");
    }
    table->records_begin = header_size;
    table->records_end = static_cast<std::uint64_t>(header_size) + record_bytes;
    table->offsets.reserve(record_bytes / 24 + 8);
    const std::uint8_t* first = d + header_size;
    const bool ok = cv::for_each_record(first, 0, record_bytes, [&](const cv::Record& r) {
        table->offsets.push_back(static_cast<std::uint32_t>(r.offset));
    });
    if (!ok) {
        return fail("a record leaves the stream");
    }
    impl.ipi = std::move(table);
    impl.ipi_ns.fetch_add(ns_since(t0), std::memory_order_relaxed);
    impl.ipi_state.store(static_cast<std::uint8_t>(LazyState::Ready), std::memory_order_release);
    return Result<void>();
}

struct IpiRecord {
    std::uint16_t kind = 0;
    const std::uint8_t* body = nullptr;
    std::uint32_t body_len = 0;
};

bool ipi_find(const IpiTable& t, std::uint32_t id, IpiRecord& out) {
    if (id < t.type_begin || id - t.type_begin >= t.offsets.size()) {
        return false;
    }
    const std::uint8_t* base = t.data.data() + t.records_begin;
    const std::uint64_t region = t.records_end - t.records_begin;
    const std::uint64_t off = t.offsets[id - t.type_begin];
    if (off + 4 > region) {
        return false;
    }
    const std::uint32_t reclen = rd16(base + off);
    if (reclen < 2 || off + 2 + reclen > region) {
        return false;
    }
    out.kind = rd16(base + off + 2);
    out.body = base + off + 4;
    out.body_len = reclen - 2;
    return true;
}

constexpr std::size_t kMaxBuildString = 4u << 20;

// LF_STRING_ID, followed by the strings of its LF_SUBSTR_LIST when it has one.
bool ipi_string(const IpiTable& t, std::uint32_t id, std::string& out, int depth) {
    IpiRecord r;
    if (depth > 4 || !ipi_find(t, id, r) || r.kind != cv::LF_STRING_ID || r.body_len < 5) {
        return false;
    }
    const std::uint32_t substr = rd32(r.body);
    out.append(reinterpret_cast<const char*>(r.body + 4), cv::cstr_len(r.body + 4, r.body_len - 4));
    if (out.size() > kMaxBuildString) {
        return false;
    }
    if (substr != 0) {
        IpiRecord list;
        if (!ipi_find(t, substr, list) || list.kind != cv::LF_SUBSTR_LIST || list.body_len < 4) {
            return false;
        }
        const std::uint32_t count = rd32(list.body);
        if (static_cast<std::uint64_t>(count) * 4 + 4 > list.body_len) {
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!ipi_string(t, rd32(list.body + 4 + static_cast<std::size_t>(i) * 4), out, depth + 1)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

Result<BuildEnv> PdbIndex::build_env(std::uint32_t ci) const {
    const Impl& im = *impl_;
    if (ci >= im.compilands.size()) {
        return im.ensure_compiland(ci).error();
    }
    BuildEnv env;
    const Compiland& c = im.compilands[ci];
    if (c.env_source == EnvSource::EnvBlock) {
        env.source = EnvSource::EnvBlock;
        for (const EnvPair& p : c.env) {
            env.pairs.emplace_back(std::string(p.first), std::string(p.second));
        }
        return env;
    }
    if (auto r = im.ensure_compiland(ci); !r) {
        return r.error();
    }
    const ModuleSymbols* ms = im.module(ci);
    if (ms == nullptr || ms->buildinfo_id == 0) {
        return env;  // source == None
    }
    if (auto r = ensure_ipi(im); !r) {
        return r.error();
    }
    const IpiTable& t = *im.ipi;
    IpiRecord bi;
    if (!ipi_find(t, ms->buildinfo_id, bi) || bi.kind != cv::LF_BUILDINFO || bi.body_len < 2) {
        return Diag::make(diag::kBadBuildInfo,
                          "S_BUILDINFO of module '" + std::string(c.object_name) + "' refers to a record that is not LF_BUILDINFO.",
                          "Rebuild the module so the compiler and linker regenerate the debug info.");
    }
    const std::uint32_t count = rd16(bi.body);
    if (2 + static_cast<std::uint64_t>(count) * 4 > bi.body_len) {
        return Diag::make(diag::kBadBuildInfo,
                          "LF_BUILDINFO of module '" + std::string(c.object_name) + "' is truncated.",
                          "Rebuild the module so the compiler and linker regenerate the debug info.");
    }
    static const char* const kKeys[5] = {"cwd", "cl", "src", "pdb", "cmd"};
    env.source = EnvSource::BuildInfo;
    for (std::uint32_t i = 0; i < count && i < 5; ++i) {
        const std::uint32_t id = rd32(bi.body + 2 + static_cast<std::size_t>(i) * 4);
        std::string value;
        if (id != 0 && !ipi_string(t, id, value, 0)) {
            value.clear();
        }
        env.pairs.emplace_back(kKeys[i], std::move(value));
    }
    return env;
}

// ---- Public queries on the lazily loaded tables ---------------------------------------------------

namespace {

SymbolView view_of(const PdbIndex::Impl& im, const SymRec& s, const char* base, std::uint32_t compiland, std::uint32_t index,
                   const ModuleSymbols* ms) {
    SymbolView v;
    v.name = std::string_view(base + s.name_off, s.name_len);
    v.kind = static_cast<SymbolKind>(s.kind);
    v.compiland = compiland;
    v.index = index;
    v.section = s.section;
    v.offset = s.offset;
    v.rva = im.rva_or_none(s.section, s.offset);
    v.size = s.size;
    v.type_index = s.type_index;
    v.flags = s.flags;
    v.parent_proc = s.parent;
    if (ms != nullptr && s.parent != kNoSymbol && s.parent < ms->syms.size()) {
        v.parent_name = sym_name(ms->syms[s.parent], ms->pool.data());
    }
    return v;
}

SymbolView view_of_pub(const PdbIndex::Impl& im, const PubRec& s, const char* base, std::uint32_t index) {
    SymbolView v;
    v.name = std::string_view(base + s.name_off, s.name_len);
    v.kind = static_cast<SymbolKind>(s.kind);
    v.index = index;
    v.section = s.section;
    v.offset = s.offset;
    v.rva = im.rva_or_none(s.section, s.offset);
    v.type_index = s.type_index;
    v.flags = s.flags;
    return v;
}

}  // namespace

Result<std::vector<SymbolView>> PdbIndex::find_public(std::string_view name) const {
    if (auto r = impl_->ensure_publics(); !r) {
        return r.error();
    }
    const GlobalSymbols& g = *impl_->globals;
    g.ensure_pub_names();
    const char* base = reinterpret_cast<const char*>(g.stream.data());
    std::vector<SymbolView> out;
    g.pub_names.find(
        name, [&](std::size_t i) { return pub_name(g.pubs[i], base); },
        [&](std::size_t i) { out.push_back(view_of_pub(*impl_, g.pubs[i], base, static_cast<std::uint32_t>(i))); });
    return out;
}

Result<std::vector<SymbolView>> PdbIndex::find_global_data(std::string_view name) const {
    if (auto r = impl_->ensure_publics(); !r) {
        return r.error();
    }
    const GlobalSymbols& g = *impl_->globals;
    g.ensure_glob_names();
    const char* base = reinterpret_cast<const char*>(g.stream.data());
    std::vector<SymbolView> out;
    g.glob_names.find(
        name, [&](std::size_t i) { return pub_name(g.globs[i], base); },
        [&](std::size_t i) { out.push_back(view_of_pub(*impl_, g.globs[i], base, static_cast<std::uint32_t>(i))); });
    return out;
}

Result<std::vector<SymbolView>> PdbIndex::find_global(std::string_view name) const {
    auto pubs = find_public(name);
    if (!pubs) {
        return pubs;
    }
    auto globs = find_global_data(name);
    if (!globs) {
        return globs;
    }
    std::vector<SymbolView> out = std::move(pubs).value();
    for (const SymbolView& v : *globs) {
        out.push_back(v);
    }
    return out;
}

Result<std::vector<SymbolView>> PdbIndex::publics_at_rva(std::uint32_t rva) const {
    if (auto r = impl_->ensure_publics(); !r) {
        return r.error();
    }
    const GlobalSymbols& g = *impl_->globals;
    g.ensure_pub_rva(impl_->sections);
    const char* base = reinterpret_cast<const char*>(g.stream.data());
    std::vector<SymbolView> out;
    auto it = std::lower_bound(g.pubs_by_rva.begin(), g.pubs_by_rva.end(), rva,
                               [](const RvaIndex& e, std::uint32_t v) { return e.rva < v; });
    for (; it != g.pubs_by_rva.end() && it->rva == rva; ++it) {
        out.push_back(view_of_pub(*impl_, g.pubs[it->index], base, it->index));
    }
    return out;
}

Result<std::optional<SymbolView>> PdbIndex::public_containing_rva(std::uint32_t rva) const {
    if (auto r = impl_->ensure_publics(); !r) {
        return r.error();
    }
    const GlobalSymbols& g = *impl_->globals;
    g.ensure_pub_rva(impl_->sections);
    const char* base = reinterpret_cast<const char*>(g.stream.data());
    auto it = std::upper_bound(g.pubs_by_rva.begin(), g.pubs_by_rva.end(), rva,
                               [](std::uint32_t v, const RvaIndex& e) { return v < e.rva; });
    if (it == g.pubs_by_rva.begin()) {
        return std::optional<SymbolView>();
    }
    --it;
    const PubRec& s = g.pubs[it->index];
    const auto want = section_offset_from_rva(rva);
    if (!want || want->section != s.section) {
        return std::optional<SymbolView>();
    }
    return std::optional<SymbolView>(view_of_pub(*impl_, s, base, it->index));
}

std::size_t PdbIndex::public_count() const {
    if (!impl_->ensure_publics()) {
        return 0;
    }
    return impl_->globals->pubs.size();
}

Result<std::vector<SymbolView>> PdbIndex::all_publics() const {
    if (auto r = impl_->ensure_publics(); !r) {
        return r.error();
    }
    const GlobalSymbols& g = *impl_->globals;
    const char* base = reinterpret_cast<const char*>(g.stream.data());
    std::vector<SymbolView> out;
    out.reserve(g.pubs.size());
    for (std::size_t i = 0; i < g.pubs.size(); ++i) {
        out.push_back(view_of_pub(*impl_, g.pubs[i], base, static_cast<std::uint32_t>(i)));
    }
    return out;
}

Result<std::vector<SymbolView>> PdbIndex::find_in_compiland(std::uint32_t ci, std::string_view name) const {
    if (auto r = impl_->ensure_compiland(ci); !r) {
        return r.error();
    }
    const ModuleSymbols& ms = *impl_->module(ci);
    ms.ensure_names();
    const char* base = ms.pool.data();
    std::vector<SymbolView> out;
    ms.names.find(
        name, [&](std::size_t i) { return sym_name(ms.syms[i], base); },
        [&](std::size_t i) { out.push_back(view_of(*impl_, ms.syms[i], base, ci, static_cast<std::uint32_t>(i), &ms)); });
    return out;
}

Result<std::vector<SymbolView>> PdbIndex::compiland_symbols(std::uint32_t ci) const {
    if (auto r = impl_->ensure_compiland(ci); !r) {
        return r.error();
    }
    const ModuleSymbols& ms = *impl_->module(ci);
    const char* base = ms.pool.data();
    std::vector<SymbolView> out;
    out.reserve(ms.syms.size());
    for (std::size_t i = 0; i < ms.syms.size(); ++i) {
        out.push_back(view_of(*impl_, ms.syms[i], base, ci, static_cast<std::uint32_t>(i), &ms));
    }
    return out;
}

namespace {

// Proc of `ms` containing rva, or nullptr.
const SymRec* proc_containing(const ModuleSymbols& ms, const std::vector<SectionHeader>& sections, std::uint32_t rva,
                              std::uint32_t& index) {
    ms.ensure_procs(sections);
    auto it = std::upper_bound(ms.procs_by_rva.begin(), ms.procs_by_rva.end(), rva,
                               [](std::uint32_t v, const RvaIndex& e) { return v < e.rva; });
    if (it != ms.procs_by_rva.begin()) {
        --it;  // procs do not nest, so only the nearest preceding proc can contain rva
        const SymRec& s = ms.syms[it->index];
        if (rva - it->rva < s.size || (s.size == 0 && rva == it->rva)) {
            index = it->index;
            return &s;
        }
    }
    return nullptr;
}

}  // namespace

Result<std::optional<SymbolView>> PdbIndex::find_proc_by_rva(std::uint32_t rva) const {
    const auto comp = compiland_for_rva(rva);
    if (comp) {
        if (auto r = impl_->ensure_compiland(*comp); !r) {
            return r.error();
        }
        const ModuleSymbols& ms = *impl_->module(*comp);
        std::uint32_t index = 0;
        if (const SymRec* s = proc_containing(ms, impl_->sections, rva, index)) {
            return std::optional<SymbolView>(view_of(*impl_, *s, ms.pool.data(), *comp, index, &ms));
        }
    }
    // Fallback (no contribution covers rva, or the contributing compiland has no such proc, e.g.
    // after identical COMDAT folding): search every compiland.
    if (auto r = load_all(); !r) {
        return r.error();
    }
    for (std::uint32_t i = 0; i < impl_->compilands.size(); ++i) {
        const ModuleSymbols* ms = impl_->module(i);
        std::uint32_t index = 0;
        if (ms != nullptr) {
            if (const SymRec* s = proc_containing(*ms, impl_->sections, rva, index)) {
                return std::optional<SymbolView>(view_of(*impl_, *s, ms->pool.data(), i, index, ms));
            }
        }
    }
    return std::optional<SymbolView>();
}

Result<void> PdbIndex::load_publics() const {
    return impl_->ensure_publics();
}

Result<void> PdbIndex::load_compiland(std::uint32_t ci) const {
    return impl_->ensure_compiland(ci);
}

Result<void> PdbIndex::load_all(unsigned threads) const {
    const auto t0 = Clock::now();
    const std::size_t n = impl_->compilands.size();
    if (threads == 0) {
        threads = impl_->options.threads;
    }
    std::atomic<bool> failed{false};
    std::mutex err_mutex;
    std::optional<Diag> first_error;
    parallel_for(n, threads, [&](std::size_t i) {
        if (failed.load(std::memory_order_relaxed)) {
            return;
        }
        auto r = impl_->ensure_compiland(static_cast<std::uint32_t>(i));
        if (!r) {
            std::lock_guard<std::mutex> lock(err_mutex);
            if (!first_error) {
                first_error = r.error();
            }
            failed.store(true, std::memory_order_relaxed);
        }
    });
    impl_->load_all_ns.store(ns_since(t0), std::memory_order_relaxed);
    if (first_error) {
        return *first_error;
    }
    return Result<void>();
}

}  // namespace cml::pdb
