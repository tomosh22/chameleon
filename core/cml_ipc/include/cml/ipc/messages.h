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

    // Hooks and synchronisation (later work packages).
    InstallHooks = 0x0040,
    HooksInstalled = 0x0041,
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

using Message = std::variant<Hello, Welcome, ModuleList, EnableModule, DisableModule, ModuleLoaded,
                             ModuleUnloaded, Log, Reply>;

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

// One-line description for logs, e.g. `EnableModule request=3 path="C:\x.dll" options=1`.
std::string describe(const Message& message);

// True when `text` is well-formed UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
bool is_valid_utf8(std::string_view text) noexcept;

}  // namespace cml::ipc
