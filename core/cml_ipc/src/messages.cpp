#include "cml/ipc/messages.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>

#include "cml/proc/diag_ids.h"

namespace cml::ipc {

namespace {

constexpr const char* kSameBuildAdvice =
    "Use an agent and a broker from the same Chameleon build; if they are, report this as a bug "
    "with the broker log.";

Diag malformed(const char* type_name, const char* what) {
    return Diag::make(ids::kPayloadMalformed,
                      std::string("malformed ") + type_name + " payload: " + what,
                      kSameBuildAdvice);
}

// Smallest encoded ModuleEntry: base + size + empty string.
constexpr std::size_t kMinModuleEntryBytes = 8 + 8 + 4;
// Smallest encoded HookSpec (two addresses + empty name) and HookResult (three u32 + empty text).
constexpr std::size_t kMinHookSpecBytes = 8 + 8 + 4;
constexpr std::size_t kMinHookResultBytes = 4 + 4 + 4 + 4;

// --- little-endian writer -----------------------------------------------------------------------

class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}

    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    void str(std::string_view s) {
        if (s.size() > kMaxStringBytes) {
            too_large_ = true;
            u32(0);
            return;
        }
        if (!is_valid_utf8(s)) {
            invalid_utf8_ = true;  // never put a frame on the wire that the peer would reject
            u32(0);
            return;
        }
        u32(static_cast<std::uint32_t>(s.size()));
        out_.insert(out_.end(), s.begin(), s.end());
    }
    void entry(const ModuleEntry& e) {
        u64(e.base);
        u64(e.size);
        str(e.path);
    }
    void u8(std::uint8_t v) { out_.push_back(v); }
    void fixup(const Fixup& f) {
        u32(f.rva);
        u8(static_cast<std::uint8_t>(f.kind));
        u8(f.n);
        u8(0);
        u8(0);
        u64(f.value);
    }
    bool too_large() const noexcept { return too_large_; }
    bool invalid_utf8() const noexcept { return invalid_utf8_; }

private:
    std::vector<std::uint8_t>& out_;
    bool too_large_ = false;
    bool invalid_utf8_ = false;
};

// --- bounds-checked little-endian reader --------------------------------------------------------

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool ok() const noexcept { return error_ == nullptr; }
    const char* error() const noexcept { return error_; }
    std::size_t remaining() const noexcept { return size_ - pos_; }
    bool at_end() const noexcept { return pos_ == size_; }

    std::uint32_t u32() { return static_cast<std::uint32_t>(take(4)); }
    std::uint64_t u64() { return take(8); }

    std::string str() {
        const std::uint32_t length = u32();
        if (!ok()) {
            return {};
        }
        if (length > kMaxStringBytes) {
            fail("string longer than the 256 KiB field limit");
            return {};
        }
        if (length > remaining()) {
            fail("string runs past the end of the payload");
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data_ + pos_), length);
        pos_ += length;
        if (!is_valid_utf8(s)) {
            fail("string is not valid UTF-8");
            return {};
        }
        return s;
    }

    ModuleEntry entry() {
        ModuleEntry e;
        e.base = u64();
        e.size = u64();
        e.path = str();
        return e;
    }

    void fail(const char* what) noexcept {
        if (error_ == nullptr) {
            error_ = what;
        }
    }

    std::uint8_t u8() { return static_cast<std::uint8_t>(take(1)); }

    // A count that is backed by payload: `count * min_bytes` must fit in what is left, so a lying
    // count cannot force a large allocation. Returns 0 (and fails) otherwise.
    std::uint32_t count(std::uint32_t limit, std::size_t min_bytes, const char* what) {
        const std::uint32_t n = u32();
        if (!ok()) {
            return 0;
        }
        if (n > limit) {
            fail(what);
            return 0;
        }
        if (static_cast<std::uint64_t>(n) * min_bytes > remaining()) {
            fail("a count is larger than the payload can hold");
            return 0;
        }
        return n;
    }

    Fixup fixup() {
        Fixup f;
        f.rva = u32();
        const std::uint8_t kind = u8();
        f.n = u8();
        const std::uint8_t reserved_lo = u8();
        const std::uint8_t reserved_hi = u8();
        f.value = u64();
        if (!ok()) {
            return f;
        }
        if (kind < static_cast<std::uint8_t>(FixupKind::Rel32) ||
            kind > static_cast<std::uint8_t>(FixupKind::SlotWrite)) {
            fail("unknown fix-up kind");
            return f;
        }
        if (reserved_lo != 0 || reserved_hi != 0) {
            fail("reserved fix-up bytes are not zero");
            return f;
        }
        const bool n_ok = kind == static_cast<std::uint8_t>(FixupKind::Rel32) ? f.n <= 5 : f.n == 0;
        if (!n_ok) {
            fail("fix-up n must be 0..5 for Rel32 and 0 for other kinds");
            return f;
        }
        f.kind = static_cast<FixupKind>(kind);
        return f;
    }
    std::vector<Fixup> fixups() {
        std::vector<Fixup> list;
        const std::uint32_t n = count(kMaxFixupsPerMessage, kFixupWireBytes, "too many fix-ups in one message");
        if (ok()) {
            list.reserve(n);
            for (std::uint32_t i = 0; i < n && ok(); ++i) {
                list.push_back(fixup());
            }
        }
        return list;
    }
    std::vector<std::uint32_t> rvas() {
        std::vector<std::uint32_t> list;
        const std::uint32_t n = count(kMaxInitializersPerMessage, 4, "too many initializers in one message");
        if (ok()) {
            list.reserve(n);
            for (std::uint32_t i = 0; i < n && ok(); ++i) {
                list.push_back(u32());
            }
        }
        return list;
    }

private:
    std::uint64_t take(std::size_t bytes) {
        if (error_ != nullptr) {
            return 0;
        }
        if (bytes > remaining()) {
            fail("payload is truncated");
            return 0;
        }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            v |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8 * i);
        }
        pos_ += bytes;
        return v;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    const char* error_ = nullptr;
};

// --- per-message encoders -----------------------------------------------------------------------

void encode(Writer& w, const Hello& m) {
    w.u64(m.request_id);
    w.u32(m.protocol_version);
    w.u32(m.agent_abi_version);
    w.u32(m.pid);
    w.u32(static_cast<std::uint32_t>(m.mode));
    w.str(m.agent_path);
    w.str(m.command_line);
}
void encode(Writer& w, const Welcome& m) {
    w.u64(m.request_id);
    w.u32(m.protocol_version);
    w.u64(m.agent_id);
    w.str(m.broker_version);
}
void encode(Writer& w, const ModuleList& m) {
    w.u64(m.request_id);
    w.u32(static_cast<std::uint32_t>(m.modules.size()));
    for (const ModuleEntry& e : m.modules) {
        w.entry(e);
    }
}
void encode(Writer& w, const EnableModule& m) {
    w.u64(m.request_id);
    w.u32(m.options);
    w.str(m.path);
}
void encode(Writer& w, const DisableModule& m) {
    w.u64(m.request_id);
    w.str(m.path);
}
void encode(Writer& w, const ModuleLoaded& m) {
    w.entry(m.module);
}
void encode(Writer& w, const ModuleUnloaded& m) {
    w.entry(m.module);
}
void encode(Writer& w, const Log& m) {
    w.u32(m.level);
    w.str(m.text);
}
void encode(Writer& w, const Reply& m) {
    w.u64(m.request_id);
    w.u32(m.status);
    w.str(m.message);
}
void encode(Writer& w, const InstallHooks& m) {
    w.u64(m.request_id);
    w.u32(m.flags);
    w.u32(static_cast<std::uint32_t>(m.hooks.size()));
    for (const HookSpec& h : m.hooks) {
        w.u64(h.old_entry);
        w.u64(h.new_entry);
        w.str(h.name);
    }
}
void encode(Writer& w, const HooksInstalled& m) {
    w.u64(m.request_id);
    w.u32(m.status);
    w.str(m.message);
    w.u32(m.attempts);
    w.u32(m.retries);
    w.u32(m.threads);
    w.u32(m.window_ms);
    w.u32(static_cast<std::uint32_t>(m.results.size()));
    for (const HookResult& r : m.results) {
        w.u32(static_cast<std::uint32_t>(r.strategy));
        w.u32(static_cast<std::uint32_t>(r.status));
        w.u32(r.diag);
        w.str(r.message);
    }
}

// --- WP-08 encoders (append-only block) ---
void encode(Writer& w, const ReserveRange& m) {
    w.u64(m.request_id);
    w.u64(m.module_base);
    w.u64(m.module_size);
    w.u64(m.block_size);
    w.u64(m.reach);
    w.u32(m.block_count);
}
void encode(Writer& w, const ReserveRangeResult& m) {
    w.u64(m.request_id);
    w.u32(m.status);
    w.str(m.message);
    w.u32(static_cast<std::uint32_t>(m.blocks.size()));
    for (const AddressBlock& b : m.blocks) {
        w.u64(b.base);
        w.u64(b.size);
    }
}
void encode(Writer& w, const ReleaseRange& m) {
    w.u64(m.request_id);
    w.u64(m.base);
    w.u64(m.size);
    w.u32(static_cast<std::uint32_t>(m.mode));
}
void encode_fixups(Writer& w, const std::vector<Fixup>& list) {
    w.u32(static_cast<std::uint32_t>(list.size()));
    for (const Fixup& f : list) {
        w.fixup(f);
    }
}
void encode(Writer& w, const LoadPatch& m) {
    w.u64(m.request_id);
    w.u64(m.patch_id);
    w.u32(m.flags);
    w.u32(static_cast<std::uint32_t>(m.candidates.size()));
    for (const PatchCandidate& c : m.candidates) {
        w.str(c.path);
        w.u64(c.image_base);
        w.u32(c.image_size);
        encode_fixups(w, c.fixups);
    }
    w.u32(static_cast<std::uint32_t>(m.initializer_rvas.size()));
    for (const std::uint32_t rva : m.initializer_rvas) {
        w.u32(rva);
    }
}
void encode(Writer& w, const PatchLoaded& m) {
    w.u64(m.request_id);
    w.u64(m.patch_id);
    w.u32(m.status);
    w.str(m.message);
    w.u64(m.image_base);
    w.u32(m.image_size);
    w.u32(m.candidate_index);
    w.u32(m.attempts);
    w.u32(m.fixups_applied);
    w.u32(m.initializers_run);
}
void encode(Writer& w, const ApplyFixups& m) {
    w.u64(m.request_id);
    w.u64(m.image_base);
    encode_fixups(w, m.fixups);
}
void encode(Writer& w, const RunInitializers& m) {
    w.u64(m.request_id);
    w.u64(m.image_base);
    w.u32(static_cast<std::uint32_t>(m.rvas.size()));
    for (const std::uint32_t rva : m.rvas) {
        w.u32(rva);
    }
}

// --- per-message decoders -----------------------------------------------------------------------

Message decode_hello(Reader& r) {
    Hello m;
    m.request_id = r.u64();
    m.protocol_version = r.u32();
    m.agent_abi_version = r.u32();
    m.pid = r.u32();
    const std::uint32_t mode = r.u32();
    if (r.ok() && mode > static_cast<std::uint32_t>(AgentMode::Synchronized)) {
        r.fail("unknown agent mode");
    }
    m.mode = static_cast<AgentMode>(mode);
    m.agent_path = r.str();
    m.command_line = r.str();
    return m;
}
Message decode_welcome(Reader& r) {
    Welcome m;
    m.request_id = r.u64();
    m.protocol_version = r.u32();
    m.agent_id = r.u64();
    m.broker_version = r.str();
    return m;
}
Message decode_module_list(Reader& r) {
    ModuleList m;
    m.request_id = r.u64();
    const std::uint32_t count = r.u32();
    if (r.ok() && count > kMaxModuleCount) {
        r.fail("module count above the limit of 16384");
    }
    // Each entry needs at least kMinModuleEntryBytes, so a lying count cannot force a huge
    // allocation: it must be backed by real payload bytes.
    if (r.ok() && static_cast<std::uint64_t>(count) * kMinModuleEntryBytes > r.remaining()) {
        r.fail("module count is larger than the payload can hold");
    }
    if (r.ok()) {
        m.modules.reserve(count);
        for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
            m.modules.push_back(r.entry());
        }
    }
    return m;
}
Message decode_enable(Reader& r) {
    EnableModule m;
    m.request_id = r.u64();
    m.options = r.u32();
    m.path = r.str();
    return m;
}
Message decode_disable(Reader& r) {
    DisableModule m;
    m.request_id = r.u64();
    m.path = r.str();
    return m;
}
Message decode_loaded(Reader& r) {
    ModuleLoaded m;
    m.module = r.entry();
    return m;
}
Message decode_unloaded(Reader& r) {
    ModuleUnloaded m;
    m.module = r.entry();
    return m;
}
Message decode_log(Reader& r) {
    Log m;
    m.level = r.u32();
    if (r.ok() && m.level > 4) {
        r.fail("unknown log level");
    }
    m.text = r.str();
    return m;
}
Message decode_reply(Reader& r) {
    Reply m;
    m.request_id = r.u64();
    m.status = r.u32();
    m.message = r.str();
    return m;
}
Message decode_install_hooks(Reader& r) {
    InstallHooks m;
    m.request_id = r.u64();
    m.flags = r.u32();
    if (r.ok() && (m.flags & ~kInstallHooksKnownFlags) != 0) {
        r.fail("unknown InstallHooks flags");
    }
    const std::uint32_t count = r.u32();
    if (r.ok() && count > kMaxHooksPerRequest) {
        r.fail("hook count above the limit of 16384");
    }
    // As for ModuleList: a lying count must be backed by real payload bytes before anything is reserved.
    if (r.ok() && static_cast<std::uint64_t>(count) * kMinHookSpecBytes > r.remaining()) {
        r.fail("hook count is larger than the payload can hold");
    }
    if (r.ok()) {
        m.hooks.reserve(count);
        for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
            HookSpec h;
            h.old_entry = r.u64();
            h.new_entry = r.u64();
            h.name = r.str();
            m.hooks.push_back(std::move(h));
        }
    }
    return m;
}
Message decode_hooks_installed(Reader& r) {
    HooksInstalled m;
    m.request_id = r.u64();
    m.status = r.u32();
    m.message = r.str();
    m.attempts = r.u32();
    m.retries = r.u32();
    m.threads = r.u32();
    m.window_ms = r.u32();
    const std::uint32_t count = r.u32();
    if (r.ok() && count > kMaxHooksPerRequest) {
        r.fail("hook result count above the limit of 16384");
    }
    if (r.ok() && static_cast<std::uint64_t>(count) * kMinHookResultBytes > r.remaining()) {
        r.fail("hook result count is larger than the payload can hold");
    }
    if (r.ok()) {
        m.results.reserve(count);
        for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
            HookResult h;
            const std::uint32_t strategy = r.u32();
            const std::uint32_t status = r.u32();
            if (r.ok() && strategy > static_cast<std::uint32_t>(HookStrategy::FiveByte)) {
                r.fail("unknown hook strategy");
            }
            if (r.ok() && status > static_cast<std::uint32_t>(HookResultStatus::Unchanged)) {
                r.fail("unknown hook status");
            }
            h.strategy = static_cast<HookStrategy>(strategy);
            h.status = static_cast<HookResultStatus>(status);
            h.diag = r.u32();
            h.message = r.str();
            m.results.push_back(std::move(h));
        }
    }
    return m;
}

// --- WP-08 decoders (append-only block) ---
Message decode_reserve_range(Reader& r) {
    ReserveRange m;
    m.request_id = r.u64();
    m.module_base = r.u64();
    m.module_size = r.u64();
    m.block_size = r.u64();
    m.reach = r.u64();
    m.block_count = r.u32();
    if (r.ok() && m.block_count > kMaxReservedBlocks) {
        r.fail("block count above the limit of 64");
    }
    return m;
}
Message decode_reserve_result(Reader& r) {
    ReserveRangeResult m;
    m.request_id = r.u64();
    m.status = r.u32();
    m.message = r.str();
    const std::uint32_t n = r.count(kMaxReservedBlocks, 16, "block count above the limit of 64");
    if (r.ok()) {
        m.blocks.reserve(n);
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
            AddressBlock b;
            b.base = r.u64();
            b.size = r.u64();
            m.blocks.push_back(b);
        }
    }
    return m;
}
Message decode_release_range(Reader& r) {
    ReleaseRange m;
    m.request_id = r.u64();
    m.base = r.u64();
    m.size = r.u64();
    const std::uint32_t mode = r.u32();
    if (r.ok() && mode > static_cast<std::uint32_t>(ReleaseMode::UnloadPatch)) {
        r.fail("unknown release mode");
    }
    m.mode = static_cast<ReleaseMode>(mode);
    return m;
}
Message decode_load_patch(Reader& r) {
    LoadPatch m;
    m.request_id = r.u64();
    m.patch_id = r.u64();
    m.flags = r.u32();
    if (r.ok() && m.flags != 0) {
        r.fail("LoadPatch flags are reserved and must be 0");
    }
    // A candidate is at least: empty path (4) + base (8) + size (4) + fix-up count (4).
    const std::uint32_t n = r.count(kMaxLoadCandidates, 20, "more than 5 load candidates");
    if (r.ok() && n == 0) {
        r.fail("LoadPatch needs at least one candidate");
    }
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        PatchCandidate c;
        c.path = r.str();
        c.image_base = r.u64();
        c.image_size = r.u32();
        c.fixups = r.fixups();
        m.candidates.push_back(std::move(c));
    }
    if (r.ok()) {
        m.initializer_rvas = r.rvas();
    }
    return m;
}
Message decode_patch_loaded(Reader& r) {
    PatchLoaded m;
    m.request_id = r.u64();
    m.patch_id = r.u64();
    m.status = r.u32();
    m.message = r.str();
    m.image_base = r.u64();
    m.image_size = r.u32();
    m.candidate_index = r.u32();
    m.attempts = r.u32();
    m.fixups_applied = r.u32();
    m.initializers_run = r.u32();
    return m;
}
Message decode_apply_fixups(Reader& r) {
    ApplyFixups m;
    m.request_id = r.u64();
    m.image_base = r.u64();
    m.fixups = r.fixups();
    return m;
}
Message decode_run_initializers(Reader& r) {
    RunInitializers m;
    m.request_id = r.u64();
    m.image_base = r.u64();
    m.rvas = r.rvas();
    return m;
}

// Lower-case hex of `value`, zero-padded to at least `min_digits` digits.
std::string hex(std::uint64_t value, std::size_t min_digits = 1) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    do {
        s.insert(s.begin(), kHex[value & 0xF]);
        value >>= 4;
    } while (value != 0);
    while (s.size() < min_digits) {
        s.insert(s.begin(), '0');
    }
    return s;
}

std::string clip(const std::string& s, std::size_t limit = 260) {
    if (s.size() <= limit) {
        return s;
    }
    return s.substr(0, utf8_cut_point(s, limit)) + "...";
}

std::string quoted(const std::string& s) {
    return "\"" + clip(s) + "\"";
}

}  // namespace

const char* to_string(MessageType type) noexcept {
    switch (type) {
        case MessageType::Hello: return "Hello";
        case MessageType::Welcome: return "Welcome";
        case MessageType::Reply: return "Reply";
        case MessageType::ModuleList: return "ModuleList";
        case MessageType::EnableModule: return "EnableModule";
        case MessageType::DisableModule: return "DisableModule";
        case MessageType::ModuleLoaded: return "ModuleLoaded";
        case MessageType::ModuleUnloaded: return "ModuleUnloaded";
        case MessageType::ReserveRange: return "ReserveRange";
        case MessageType::ReserveRangeResult: return "ReserveRangeResult";
        case MessageType::ReleaseRange: return "ReleaseRange";
        case MessageType::LoadPatch: return "LoadPatch";
        case MessageType::PatchLoaded: return "PatchLoaded";
        case MessageType::ApplyFixups: return "ApplyFixups";
        case MessageType::RunInitializers: return "RunInitializers";
        case MessageType::InstallHooks: return "InstallHooks";
        case MessageType::HooksInstalled: return "HooksInstalled";
        case MessageType::SyncPointReady: return "SyncPointReady";
        case MessageType::Exception: return "Exception";
        case MessageType::ExceptionAction: return "ExceptionAction";
        case MessageType::Log: return "Log";
        case MessageType::Restart: return "Restart";
    }
    return "Unknown";
}

bool is_implemented(MessageType type) noexcept {
    switch (type) {
        case MessageType::Hello:
        case MessageType::Welcome:
        case MessageType::Reply:
        case MessageType::ModuleList:
        case MessageType::EnableModule:
        case MessageType::DisableModule:
        case MessageType::ModuleLoaded:
        case MessageType::ModuleUnloaded:
        case MessageType::InstallHooks:
        case MessageType::HooksInstalled:
        case MessageType::Log:
        case MessageType::ReserveRange:
        case MessageType::ReserveRangeResult:
        case MessageType::ReleaseRange:
        case MessageType::LoadPatch:
        case MessageType::PatchLoaded:
        case MessageType::ApplyFixups:
        case MessageType::RunInitializers:
            return true;
        default:
            return false;
    }
}

MessageType type_of(const Message& message) noexcept {
    struct Visitor {
        MessageType operator()(const Hello&) const noexcept { return MessageType::Hello; }
        MessageType operator()(const Welcome&) const noexcept { return MessageType::Welcome; }
        MessageType operator()(const ModuleList&) const noexcept { return MessageType::ModuleList; }
        MessageType operator()(const EnableModule&) const noexcept { return MessageType::EnableModule; }
        MessageType operator()(const DisableModule&) const noexcept { return MessageType::DisableModule; }
        MessageType operator()(const ModuleLoaded&) const noexcept { return MessageType::ModuleLoaded; }
        MessageType operator()(const ModuleUnloaded&) const noexcept { return MessageType::ModuleUnloaded; }
        MessageType operator()(const Log&) const noexcept { return MessageType::Log; }
        MessageType operator()(const Reply&) const noexcept { return MessageType::Reply; }
        MessageType operator()(const InstallHooks&) const noexcept { return MessageType::InstallHooks; }
        MessageType operator()(const HooksInstalled&) const noexcept { return MessageType::HooksInstalled; }
        MessageType operator()(const ReserveRange&) const noexcept { return MessageType::ReserveRange; }
        MessageType operator()(const ReserveRangeResult&) const noexcept { return MessageType::ReserveRangeResult; }
        MessageType operator()(const ReleaseRange&) const noexcept { return MessageType::ReleaseRange; }
        MessageType operator()(const LoadPatch&) const noexcept { return MessageType::LoadPatch; }
        MessageType operator()(const PatchLoaded&) const noexcept { return MessageType::PatchLoaded; }
        MessageType operator()(const ApplyFixups&) const noexcept { return MessageType::ApplyFixups; }
        MessageType operator()(const RunInitializers&) const noexcept { return MessageType::RunInitializers; }
    };
    return std::visit(Visitor{}, message);
}

bool is_valid_utf8(std::string_view text) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t code = 0;
        std::uint32_t min_code = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
            code = c & 0x1Fu;
            min_code = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            code = c & 0x0Fu;
            min_code = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            code = c & 0x07u;
            min_code = 0x10000;
        } else {
            return false;  // continuation byte, overlong lead (C0/C1) or invalid lead (F5..FF)
        }
        if (i + extra >= n) {
            return false;  // sequence runs past the end
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = p[i + k];
            if ((cc & 0xC0u) != 0x80u) {
                return false;
            }
            code = (code << 6) | (cc & 0x3Fu);
        }
        if (code < min_code || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

std::size_t utf8_cut_point(std::string_view text, std::size_t max_bytes) noexcept {
    if (max_bytes >= text.size()) {
        return text.size();
    }
    std::size_t n = max_bytes;
    // Back up over continuation bytes, then over the lead byte they belong to.
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0u) == 0x80u) {
        --n;
    }
    return n;
}

std::string sanitize_utf8(std::string_view text, std::size_t max_bytes) {
    std::string out;
    out.reserve(std::min(text.size(), max_bytes));
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        // Length of the well-formed sequence at i, or 0.
        std::size_t len = 1;
        if (p[i] >= 0x80) {
            len = 0;
            for (std::size_t candidate = 2; candidate <= 4 && i + candidate <= n; ++candidate) {
                if (is_valid_utf8(text.substr(i, candidate))) {
                    len = candidate;
                    break;
                }
            }
        }
        const std::string_view piece = len == 0 ? std::string_view("\xEF\xBF\xBD") : text.substr(i, len);
        if (out.size() + piece.size() > max_bytes) {
            break;
        }
        out.append(piece);
        i += len == 0 ? 1 : len;
    }
    return out;
}

Result<FrameHeader> parse_frame_header(const std::uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr || size < kFrameHeaderSize) {
        return Diag::make(ids::kFrameHeaderInvalid,
                          "frame header is shorter than 8 bytes (" + std::to_string(size) + ")",
                          kSameBuildAdvice);
    }
    FrameHeader h;
    h.payload_size = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
                     (static_cast<std::uint32_t>(bytes[2]) << 16) |
                     (static_cast<std::uint32_t>(bytes[3]) << 24);
    h.type = static_cast<std::uint16_t>(bytes[4] | (bytes[5] << 8));
    h.version = static_cast<std::uint16_t>(bytes[6] | (bytes[7] << 8));
    if (h.payload_size > kMaxPayloadSize) {
        return Diag::make(ids::kFrameHeaderInvalid,
                          "frame announces " + std::to_string(h.payload_size) +
                              " payload bytes, above the limit of " +
                              std::to_string(kMaxPayloadSize),
                          kSameBuildAdvice);
    }
    return h;
}

Result<std::vector<std::uint8_t>> encode_frame(const Message& message) {
    std::vector<std::uint8_t> frame(kFrameHeaderSize, 0);
    Writer writer(frame);
    std::visit([&writer](const auto& m) { encode(writer, m); }, message);
    const MessageType type = type_of(message);
    const std::size_t payload_size = frame.size() - kFrameHeaderSize;
    if (writer.invalid_utf8()) {
        return malformed(to_string(type), "a string field is not valid UTF-8 (refusing to send it)");
    }
    if (writer.too_large() || payload_size > kMaxPayloadSize) {
        return Diag::make(ids::kMessageTooLarge,
                          std::string("cannot send ") + to_string(type) +
                              ": a string or the whole payload exceeds the protocol limit",
                          "Shorten the path or text, or report it if this is a real-world size.");
    }
    const auto size32 = static_cast<std::uint32_t>(payload_size);
    const auto type16 = static_cast<std::uint16_t>(type);
    for (int i = 0; i < 4; ++i) {
        frame[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(size32 >> (8 * i));
    }
    frame[4] = static_cast<std::uint8_t>(type16);
    frame[5] = static_cast<std::uint8_t>(type16 >> 8);
    frame[6] = static_cast<std::uint8_t>(kMessageVersion);
    frame[7] = static_cast<std::uint8_t>(kMessageVersion >> 8);
    return frame;
}

Result<Message> decode_message(std::uint16_t type, std::uint16_t version, const std::uint8_t* payload,
                               std::size_t size) {
    const auto message_type = static_cast<MessageType>(type);
    if (!is_implemented(message_type)) {
        return Diag::make(ids::kMessageTypeUnsupported,
                          "message type 0x" + hex(type, 4) + " (" + to_string(message_type) +
                              ") is unknown or not implemented in this build",
                          kSameBuildAdvice);
    }
    if (version != kMessageVersion) {
        return Diag::make(ids::kMessageVersionUnsupported,
                          std::string(to_string(message_type)) + " uses payload version " +
                              std::to_string(version) + " but this build understands version " +
                              std::to_string(kMessageVersion),
                          kSameBuildAdvice);
    }
    if (payload == nullptr && size != 0) {
        return malformed(to_string(message_type), "null payload with a non-zero size");
    }
    Reader reader(payload, size);
    Message message;
    switch (message_type) {
        case MessageType::Hello: message = decode_hello(reader); break;
        case MessageType::Welcome: message = decode_welcome(reader); break;
        case MessageType::Reply: message = decode_reply(reader); break;
        case MessageType::ModuleList: message = decode_module_list(reader); break;
        case MessageType::EnableModule: message = decode_enable(reader); break;
        case MessageType::DisableModule: message = decode_disable(reader); break;
        case MessageType::ModuleLoaded: message = decode_loaded(reader); break;
        case MessageType::ModuleUnloaded: message = decode_unloaded(reader); break;
        case MessageType::Log: message = decode_log(reader); break;
        case MessageType::InstallHooks: message = decode_install_hooks(reader); break;
        case MessageType::HooksInstalled: message = decode_hooks_installed(reader); break;
        case MessageType::ReserveRange: message = decode_reserve_range(reader); break;
        case MessageType::ReserveRangeResult: message = decode_reserve_result(reader); break;
        case MessageType::ReleaseRange: message = decode_release_range(reader); break;
        case MessageType::LoadPatch: message = decode_load_patch(reader); break;
        case MessageType::PatchLoaded: message = decode_patch_loaded(reader); break;
        case MessageType::ApplyFixups: message = decode_apply_fixups(reader); break;
        case MessageType::RunInitializers: message = decode_run_initializers(reader); break;
        default: break;  // unreachable: is_implemented() was checked above
    }
    if (!reader.ok()) {
        return malformed(to_string(message_type), reader.error());
    }
    if (!reader.at_end()) {
        return malformed(to_string(message_type), "unexpected trailing bytes");
    }
    return message;
}

Result<Message> decode_frame(const std::uint8_t* frame, std::size_t size) {
    Result<FrameHeader> header = parse_frame_header(frame, size);
    if (!header) {
        return header.error();
    }
    if (size - kFrameHeaderSize != header->payload_size) {
        return Diag::make(ids::kFrameHeaderInvalid,
                          "frame header announces " + std::to_string(header->payload_size) +
                              " payload bytes but " + std::to_string(size - kFrameHeaderSize) +
                              " are present",
                          kSameBuildAdvice);
    }
    return decode_message(header->type, header->version, frame + kFrameHeaderSize,
                          header->payload_size);
}

bool is_request_type(std::uint16_t type) noexcept {
    switch (static_cast<MessageType>(type)) {
        case MessageType::Hello:
        case MessageType::ModuleList:
        case MessageType::EnableModule:
        case MessageType::DisableModule:
        case MessageType::InstallHooks:
        case MessageType::ReserveRange:
        case MessageType::ReleaseRange:
        case MessageType::LoadPatch:
        case MessageType::ApplyFixups:
        case MessageType::RunInitializers:
            return true;
        default:
            return false;
    }
}

std::optional<std::uint64_t> peek_request_id(std::uint16_t type, const std::uint8_t* payload,
                                             std::size_t size) noexcept {
    switch (static_cast<MessageType>(type)) {
        case MessageType::Hello:
        case MessageType::Welcome:
        case MessageType::Reply:
        case MessageType::ModuleList:
        case MessageType::EnableModule:
        case MessageType::DisableModule:
        case MessageType::InstallHooks:
        case MessageType::HooksInstalled:
        case MessageType::ReserveRange:
        case MessageType::ReserveRangeResult:
        case MessageType::ReleaseRange:
        case MessageType::LoadPatch:
        case MessageType::PatchLoaded:
        case MessageType::ApplyFixups:
        case MessageType::RunInitializers:
            break;
        default:
            return std::nullopt;
    }
    if (payload == nullptr || size < 8) {
        return std::nullopt;
    }
    std::uint64_t id = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        id |= static_cast<std::uint64_t>(payload[i]) << (8 * i);
    }
    return id;
}

std::string describe(const Message& message) {
    struct Visitor {
        std::string operator()(const Hello& m) const {
            return "Hello request=" + std::to_string(m.request_id) +
                   " protocol=" + std::to_string(m.protocol_version) +
                   " abi=" + std::to_string(m.agent_abi_version) + " pid=" + std::to_string(m.pid) +
                   " mode=" + (m.mode == AgentMode::Synchronized ? "synchronized" : "immediate") +
                   " agent=" + quoted(m.agent_path) + " cmdline=" + quoted(m.command_line);
        }
        std::string operator()(const Welcome& m) const {
            return "Welcome request=" + std::to_string(m.request_id) +
                   " protocol=" + std::to_string(m.protocol_version) +
                   " agent_id=" + std::to_string(m.agent_id) + " broker=" + quoted(m.broker_version);
        }
        std::string operator()(const ModuleList& m) const {
            return "ModuleList request=" + std::to_string(m.request_id) +
                   " modules=" + std::to_string(m.modules.size());
        }
        std::string operator()(const EnableModule& m) const {
            return "EnableModule request=" + std::to_string(m.request_id) +
                   " options=" + std::to_string(m.options) + " path=" + quoted(m.path);
        }
        std::string operator()(const DisableModule& m) const {
            return "DisableModule request=" + std::to_string(m.request_id) + " path=" + quoted(m.path);
        }
        std::string operator()(const ModuleLoaded& m) const {
            return "ModuleLoaded base=0x" + hex(m.module.base) + " size=" +
                   std::to_string(m.module.size) + " path=" + quoted(m.module.path);
        }
        std::string operator()(const ModuleUnloaded& m) const {
            return "ModuleUnloaded base=0x" + hex(m.module.base) + " size=" +
                   std::to_string(m.module.size) + " path=" + quoted(m.module.path);
        }
        std::string operator()(const Log& m) const {
            return "Log level=" + std::to_string(m.level) + " text=" + quoted(m.text);
        }
        std::string operator()(const Reply& m) const {
            return "Reply request=" + std::to_string(m.request_id) +
                   " status=" + std::to_string(m.status) + " message=" + quoted(m.message);
        }
        std::string operator()(const InstallHooks& m) const {
            return std::string("InstallHooks request=") + std::to_string(m.request_id) +
                   " flags=" + std::to_string(m.flags) + ((m.flags & kInstallHooksRevert) != 0 ? " (revert)" : "") +
                   " hooks=" + std::to_string(m.hooks.size());
        }
        std::string operator()(const HooksInstalled& m) const {
            return "HooksInstalled request=" + std::to_string(m.request_id) +
                   " status=" + std::to_string(m.status) + " results=" + std::to_string(m.results.size()) +
                   " attempts=" + std::to_string(m.attempts) + " retries=" + std::to_string(m.retries) +
                   " threads=" + std::to_string(m.threads) + " window_ms=" + std::to_string(m.window_ms) +
                   (m.message.empty() ? std::string() : " message=" + quoted(m.message));
        }
        std::string operator()(const ReserveRangeResult& m) const {
            return "ReserveRangeResult request=" + std::to_string(m.request_id) +
                   " status=" + std::to_string(m.status) + " blocks=" + std::to_string(m.blocks.size()) +
                   " message=" + quoted(m.message);
        }
        std::string operator()(const ReserveRange& m) const {
            return "ReserveRange request=" + std::to_string(m.request_id) + " module=0x" + hex(m.module_base) +
                   "+0x" + hex(m.module_size) + " block_size=0x" + hex(m.block_size) +
                   " count=" + std::to_string(m.block_count) + " reach=0x" + hex(m.reach);
        }
        std::string operator()(const ReleaseRange& m) const {
            return "ReleaseRange request=" + std::to_string(m.request_id) + " base=0x" + hex(m.base) +
                   " size=0x" + hex(m.size) + (m.mode == ReleaseMode::Free ? " mode=free" : " mode=unload-patch");
        }
        std::string operator()(const LoadPatch& m) const {
            std::string s = "LoadPatch request=" + std::to_string(m.request_id) +
                            " patch=" + std::to_string(m.patch_id) +
                            " candidates=" + std::to_string(m.candidates.size()) +
                            " initializers=" + std::to_string(m.initializer_rvas.size());
            if (!m.candidates.empty()) {
                const PatchCandidate& c = m.candidates.front();
                s += " first={base=0x" + hex(c.image_base) + " size=0x" + hex(c.image_size) +
                     " fixups=" + std::to_string(c.fixups.size()) + " path=" + quoted(c.path) + "}";
            }
            return s;
        }
        std::string operator()(const PatchLoaded& m) const {
            return "PatchLoaded request=" + std::to_string(m.request_id) + " patch=" + std::to_string(m.patch_id) +
                   " status=" + std::to_string(m.status) + " base=0x" + hex(m.image_base) +
                   " candidate=" + std::to_string(m.candidate_index) + " attempts=" + std::to_string(m.attempts) +
                   " message=" + quoted(m.message);
        }
        std::string operator()(const ApplyFixups& m) const {
            return "ApplyFixups request=" + std::to_string(m.request_id) + " base=0x" + hex(m.image_base) +
                   " fixups=" + std::to_string(m.fixups.size());
        }
        std::string operator()(const RunInitializers& m) const {
            return "RunInitializers request=" + std::to_string(m.request_id) + " base=0x" + hex(m.image_base) +
                   " initializers=" + std::to_string(m.rvas.size());
        }
    };
    return std::visit(Visitor{}, message);
}

}  // namespace cml::ipc
