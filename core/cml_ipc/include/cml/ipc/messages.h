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

    // Address space (WP-08).
    ReserveRange = 0x0020,  // implemented  broker -> agent (request); the agent answers with ReserveRange (answer form)
    ReleaseRange = 0x0021,  // implemented  broker -> agent (request, answered by Reply)

    // Patch pipeline (WP-08).
    LoadPatch = 0x0030,         // implemented  broker -> agent (request, answered by PatchLoaded)
    PatchLoaded = 0x0031,       // implemented  agent -> broker (answer to LoadPatch)
    ApplyFixups = 0x0032,       // implemented  broker -> agent (request, answered by Reply)
    RunInitializers = 0x0033,   // implemented  broker -> agent (request, answered by Reply)

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

// --- address space and patch loading (WP-08) ---------------------------------------------------
//
// All of these are broker -> agent requests; ids are chosen by the broker. The agent answers
// LoadPatch with PatchLoaded and ReserveRange with a ReserveRange in its answer form (both carry
// status 0 or a CML42xx id, so a refusal still says why); everything else is answered with Reply.
// A request whose payload cannot be decoded is answered with Reply (CML5201..5203).

inline constexpr std::uint32_t kMaxFixupsPerMessage = 60000;   // 16 bytes each: fits the 1 MiB frame
inline constexpr std::uint32_t kMaxInitializersPerMessage = 16384;
inline constexpr std::uint32_t kMaxLoadCandidates = 5;         // plan section 4.7: retry up to 5 times
inline constexpr std::uint32_t kMaxReservedBlocks = 64;

// One fix-up: a place in the patch image (RVA) and what to write there. ADR-0025: the broker reads
// the in-place addend and sends the ABSOLUTE target (symbol + addend) in `value`; the agent
// computes the REL32 displacement itself, so it can range-check it against the real site address:
//   Rel32     writes int32(value - (site_va + 4 + n)); n (0..5) is the number of immediate bytes
//             that follow the displacement (REL32 .. REL32_5). Fails with CML4207 when it does not fit.
//   Addr64    writes uint64(value) (8 bytes, no alignment required).
//   SlotWrite writes uint64(value) atomically into an 8-byte-aligned thunk slot (__cml_slot_<name>).
// ADDR32NB never reaches the agent: the broker rejects it (CML33xx).
enum class FixupKind : std::uint8_t { Rel32 = 1, Addr64 = 2, SlotWrite = 3 };

struct Fixup {
    std::uint32_t rva = 0;  // site, relative to the patch image base
    FixupKind kind = FixupKind::Addr64;
    std::uint8_t n = 0;     // Rel32 only: 0..5; must be 0 for the other kinds
    std::uint64_t value = 0;
};

// Wire layout of a Fixup: u32 rva, u8 kind, u8 n, u16 reserved(0), u64 value.
inline constexpr std::uint32_t kFixupWireBytes = 16;

// A way to place the patch: the DLL file, linked `/FIXED /BASE:image_base`, and its fix-ups (which
// depend on the base, so they travel with the candidate). The agent tries candidates in order and
// moves to the next one only when the range was taken by someone else between release and load.
struct PatchCandidate {
    std::string path;                // absolute path of the patch DLL, UTF-8
    std::uint64_t image_base = 0;    // the /BASE the DLL was linked at; 64 KiB aligned
    std::uint32_t image_size = 0;    // SizeOfImage of that DLL
    std::vector<Fixup> fixups;       // applied in order, after the load
};

struct LoadPatch {
    std::uint64_t request_id = 0;
    std::uint64_t patch_id = 0;      // broker's handle, echoed in PatchLoaded
    std::uint32_t flags = 0;         // reserved, must be 0
    std::vector<PatchCandidate> candidates;       // 1..kMaxLoadCandidates
    std::vector<std::uint32_t> initializer_rvas;  // RVAs of .CRT$XCU entries, run in this order
};

struct PatchLoaded {
    std::uint64_t request_id = 0;
    std::uint64_t patch_id = 0;
    std::uint32_t status = 0;        // 0 or the CML42xx id
    std::string message;
    std::uint64_t image_base = 0;    // where it was loaded (0 on failure)
    std::uint32_t image_size = 0;
    std::uint32_t candidate_index = 0;  // which candidate (0-based) was loaded
    std::uint32_t attempts = 0;         // load attempts made, including the successful one
    std::uint32_t fixups_applied = 0;
    std::uint32_t initializers_run = 0;
};

struct ApplyFixups {
    std::uint64_t request_id = 0;
    std::uint64_t image_base = 0;    // base of a patch loaded by LoadPatch
    std::vector<Fixup> fixups;
};

struct RunInitializers {
    std::uint64_t request_id = 0;
    std::uint64_t image_base = 0;
    std::vector<std::uint32_t> rvas;
};

struct AddressBlock {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
};

// Request form: reserve up to `block_count` blocks of `block_size` bytes inside the window around
// [module_base, module_base + module_size) that keeps every REL32 between module and block in
// range (and within `reach` of the module). Answer form: the blocks actually reserved.
struct ReserveRange {
    std::uint64_t request_id = 0;
    bool answer = false;
    // request
    std::uint64_t module_base = 0;
    std::uint64_t module_size = 0;
    std::uint64_t block_size = 64ull << 20;
    std::uint64_t reach = 1536ull << 20;
    std::uint32_t block_count = 4;
    // answer
    std::uint32_t status = 0;
    std::string message;
    std::vector<AddressBlock> blocks;
};

enum class ReleaseMode : std::uint32_t {
    Free = 0,         // give [base, base+size) of reserved address space back to the OS
    UnloadPatch = 1,  // FreeLibrary the patch loaded at `base` and return its range to the reserved state
};

struct ReleaseRange {
    std::uint64_t request_id = 0;
    std::uint64_t base = 0;
    std::uint64_t size = 0;  // Free: the extent; UnloadPatch: ignored (0)
    ReleaseMode mode = ReleaseMode::Free;
};

using Message = std::variant<Hello, Welcome, ModuleList, EnableModule, DisableModule, ModuleLoaded,
                             ModuleUnloaded, Log, Reply, ReserveRange, ReleaseRange, LoadPatch,
                             PatchLoaded, ApplyFixups, RunInitializers>;

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
// DisableModule, and the WP-08 requests ReserveRange, ReleaseRange, LoadPatch, ApplyFixups,
// RunInitializers). Takes the raw wire value, so it also works for frames that failed to decode.
bool is_request_type(std::uint16_t type) noexcept;

// The request id of a frame whose payload may be malformed: every type whose payload starts with a
// request id (Hello, Welcome, Reply, ModuleList, EnableModule, DisableModule and the WP-08 types)
// carries it in the
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
