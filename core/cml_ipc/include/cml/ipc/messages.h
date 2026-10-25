#pragma once

// Wire format of the agent <-> broker protocol (plan section 4.13).
//
// Frame (all integers little-endian):
//
//     uint32 payload_size   bytes that follow the 8-byte header (not counting the header)
//     uint16 type           MessageType
//     uint16 version        version of that message's payload layout (kMessageVersion)
//     uint8  payload[payload_size]
//
// Payloads are hand-rolled: fixed-width little-endian integers, and strings as
// `uint32 byte_count` followed by that many UTF-8 bytes (no terminator). Every decoder is
// bounds-checked, rejects trailing bytes and invalid UTF-8, and reports problems as a cml::Diag
// (CML5200..CML5204); no input can make it read out of range, allocate more than the frame limit
// allows, or throw. This code is portable (no Win32) so it can be unit-tested and fuzzed anywhere.
//
// Requests carry a `request_id` chosen by the sender; the answer (Welcome or Reply) repeats it.
// Events (ModuleLoaded, ModuleUnloaded, Log) have no id and no answer.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cml/util/result.h"

namespace cml::ipc {

inline constexpr std::uint32_t kFrameHeaderSize = 8;
inline constexpr std::uint32_t kMaxPayloadSize = 1u << 20;  // 1 MiB hard limit per frame
inline constexpr std::uint32_t kMaxStringBytes = 1u << 18;  // 256 KiB per string field
inline constexpr std::uint32_t kMaxModuleCount = 16384;     // entries in one ModuleList
inline constexpr std::uint32_t kMaxHooksPerRequest = 16384; // entries in one InstallHooks / HooksInstalled
inline constexpr std::uint16_t kMessageVersion = 1;         // payload layout version of every type
inline constexpr std::uint32_t kProtocolVersion = 1;        // exchanged in Hello / Welcome

// Stable message ids. Every family of plan section 4.13 has its ids reserved now; only the ones
// marked "implemented" have payloads in this work package. Never renumber.
enum class MessageType : std::uint16_t {
    // Handshake and generic answer.
    Hello = 0x0001,    // implemented  agent -> broker
    Welcome = 0x0002,  // implemented  broker -> agent
    Reply = 0x0003,    // implemented  either way: request id + status + UTF-8 message

    // Modules.
    ModuleList = 0x0010,      // implemented  agent -> broker (request, answered by Reply)
    EnableModule = 0x0011,    // implemented  agent -> broker (request, answered by Reply)
    DisableModule = 0x0012,   // implemented  agent -> broker (request, answered by Reply)
    ModuleLoaded = 0x0013,    // implemented  agent -> broker (event)
    ModuleUnloaded = 0x0014,  // implemented  agent -> broker (event)

    // Address space (later work packages).
    ReserveRange = 0x0020,
    ReleaseRange = 0x0021,

    // Patch pipeline (later work packages).
    LoadPatch = 0x0030,
    PatchLoaded = 0x0031,
    ApplyFixups = 0x0032,
    RunInitializers = 0x0033,

    // Hooks (WP-10) and synchronisation (later work package).
    InstallHooks = 0x0040,    // implemented  broker -> agent (request, answered by HooksInstalled)
    HooksInstalled = 0x0041,  // implemented  agent -> broker (the answer; repeats the request id)
    SyncPointReady = 0x0042,

    // Exceptions (later work packages).
    Exception = 0x0050,
    ExceptionAction = 0x0051,

    // Diagnostics and lifecycle.
    Log = 0x0060,      // implemented  agent -> broker (event)
    Restart = 0x0061,  // later work package
};

// Name for logs ("Hello"); "Unknown" for ids that are not in the enum.
const char* to_string(MessageType type) noexcept;
// True for the types that have payloads in this work package.
bool is_implemented(MessageType type) noexcept;

// --- messages -----------------------------------------------------------------------------------

enum class AgentMode : std::uint32_t { Immediate = 0, Synchronized = 1 };

struct Hello {
    std::uint64_t request_id = 0;
    std::uint32_t protocol_version = kProtocolVersion;
    std::uint32_t agent_abi_version = 0;
    std::uint32_t pid = 0;
    AgentMode mode = AgentMode::Immediate;
    std::string agent_path;    // full path of cml_agent.dll, UTF-8
    std::string command_line;  // command line of the host process, UTF-8
};

struct Welcome {
    std::uint64_t request_id = 0;  // the Hello's id
    std::uint32_t protocol_version = kProtocolVersion;
    std::uint64_t agent_id = 0;    // broker-assigned, unique per broker run
    std::string broker_version;    // "0.1.0"
};

struct ModuleEntry {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::string path;  // full path, UTF-8
};

struct ModuleList {
    std::uint64_t request_id = 0;
    std::vector<ModuleEntry> modules;
};

// Bit flags of EnableModule::options (mirrors cml::ModuleOptions in Chameleon_API.h).
inline constexpr std::uint32_t kModuleOptionAllImports = 1u;

struct EnableModule {
    std::uint64_t request_id = 0;
    std::uint32_t options = 0;
    std::string path;  // full path, or a bare file name that the broker matches by name
};

struct DisableModule {
    std::uint64_t request_id = 0;
    std::string path;
};

struct ModuleLoaded {
    ModuleEntry module;
};

struct ModuleUnloaded {
    ModuleEntry module;
};

struct Log {
    std::uint32_t level = 2;  // cml::LogLevel numeric value (0 trace .. 4 error)
    std::string text;
};

// Generic answer. status 0 is success; anything else is the numeric id of a CMLnnnn diagnostic.
struct Reply {
    std::uint64_t request_id = 0;
    std::uint32_t status = 0;
    std::string message;  // UTF-8, human readable
};

// --- hooks (WP-10, plan section 4.8) -------------------------------------------------------------

// Bit flags of InstallHooks::flags. Unknown bits are rejected by the decoder.
// Revert is folded into InstallHooks (rather than a message of its own) because it is the same
// operation on the same list, applied under the same stop-the-world window, and answered by the
// same HooksInstalled; with the flag set `new_entry` is ignored and the original bytes come back.
inline constexpr std::uint32_t kInstallHooksRevert = 1u;
inline constexpr std::uint32_t kInstallHooksKnownFlags = kInstallHooksRevert;

struct HookSpec {
    std::uint64_t old_entry = 0;  // entry of the function being replaced (its first hook's target)
    std::uint64_t new_entry = 0;  // entry of the new implementation
    std::string name;             // for diagnostics only
};

// One request is applied all or nothing, in one stop-the-world window.
struct InstallHooks {
    std::uint64_t request_id = 0;
    std::uint32_t flags = 0;
    std::vector<HookSpec> hooks;
};

enum class HookStrategy : std::uint32_t { None = 0, PaddingTwoByte = 1, FiveByte = 2 };
enum class HookResultStatus : std::uint32_t {
    Installed = 0,
    Repointed = 1,
    Reverted = 2,
    Failed = 3,        // this hook is why the request failed
    NotAttempted = 4,  // not changed: the request failed
    Unchanged = 5,     // already hooked to this very new_entry
};

struct HookResult {
    HookStrategy strategy = HookStrategy::None;
    HookResultStatus status = HookResultStatus::NotAttempted;
    std::uint32_t diag = 0;  // CML43xx for Failed / NotAttempted, else 0
    std::string message;     // diagnostic text; only filled for Failed entries
};

struct HooksInstalled {
    std::uint64_t request_id = 0;  // the InstallHooks' id
    std::uint32_t status = 0;      // 0: every hook applied; else the CML number of the first failure
    std::string message;           // text of that failure
    std::uint32_t attempts = 0;    // times the application's threads were stopped
    std::uint32_t retries = 0;     // RIP-in-window conflicts that forced another look
    std::uint32_t threads = 0;     // threads stopped in the largest window
    std::uint32_t window_ms = 0;   // longest time they were stopped
    std::vector<HookResult> results;  // one per hook, in request order
};

using Message = std::variant<Hello, Welcome, ModuleList, EnableModule, DisableModule, ModuleLoaded,
                             ModuleUnloaded, Log, Reply, InstallHooks, HooksInstalled>;

MessageType type_of(const Message& message) noexcept;

// --- framing ------------------------------------------------------------------------------------

struct FrameHeader {
    std::uint32_t payload_size = 0;
    std::uint16_t type = 0;  // raw wire value; may not be a known MessageType
    std::uint16_t version = 0;
};

// Parses the 8 header bytes (`size` must be >= kFrameHeaderSize, else CML5200). A payload_size
// above kMaxPayloadSize is CML5200: the stream cannot be trusted afterwards and the connection
// should be closed.
Result<FrameHeader> parse_frame_header(const std::uint8_t* bytes, std::size_t size);

// Serialises a complete frame (header + payload). CML5204 when a string or the payload exceeds
// its limit.
Result<std::vector<std::uint8_t>> encode_frame(const Message& message);

// Decodes one payload. CML5202 for a version other than kMessageVersion, CML5203 for a type that
// is unknown or not implemented yet, CML5201 for any malformed content. `payload` may be null
// only when `size` is 0.
Result<Message> decode_message(std::uint16_t type, std::uint16_t version, const std::uint8_t* payload,
                               std::size_t size);

// Convenience for tests and tools: decodes a whole frame (header + payload) held in memory.
Result<Message> decode_frame(const std::uint8_t* frame, std::size_t size);

// True for the message types that expect an answer (Hello, ModuleList, EnableModule,
// DisableModule, InstallHooks). Takes the raw wire value, so it also works for frames that failed
// to decode.
bool is_request_type(std::uint16_t type) noexcept;

// The request id of a frame whose payload may be malformed: every type whose payload starts with a
// request id (Hello, Welcome, Reply, ModuleList, EnableModule, DisableModule, InstallHooks,
// HooksInstalled) carries it in the
// first 8 bytes. nullopt for other types or when fewer than 8 bytes are present. Used to answer
// a request that arrived with a bad payload instead of leaving its sender waiting.
std::optional<std::uint64_t> peek_request_id(std::uint16_t type, const std::uint8_t* payload,
                                             std::size_t size) noexcept;

// One-line description for logs, e.g. `EnableModule request=3 path="C:\x.dll" options=1`.
std::string describe(const Message& message);

// True when `text` is well-formed UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
bool is_valid_utf8(std::string_view text) noexcept;

// Largest n <= max_bytes such that cutting `text` after n bytes does not split a UTF-8 sequence
// (assuming `text` is valid UTF-8 up to that point). Use it wherever a string is shortened.
std::size_t utf8_cut_point(std::string_view text, std::size_t max_bytes) noexcept;

// Returns a well-formed UTF-8 copy of `text` no longer than `max_bytes`: invalid bytes become
// U+FFFD and the cut falls on a character boundary. Strings that go on the wire (Log text, ...)
// pass through this when their origin is not known to be clean (e.g. a log line the C runtime cut
// at a fixed byte count).
std::string sanitize_utf8(std::string_view text, std::size_t max_bytes);

}  // namespace cml::ipc
