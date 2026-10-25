#pragma once

#include <cstdint>

// Registry of the CML52xx runtime diagnostic ids owned by the agent/IPC work package (WP-11).
// CML5200..5299 is reserved for it; later packages must pick ids outside this block.
//
// The header lives in cml_proc (the lowest library that needs ids) so that cml_proc, cml_ipc,
// the agent and the broker can all share one list. `docs/wp/WP-11.md` repeats every id with its
// message and "what to do" text.
namespace cml::ids {

// --- wire format (cml_ipc messages) -------------------------------------------------------------
inline constexpr std::uint32_t kFrameHeaderInvalid = 5200;   // bad length / oversized frame
inline constexpr std::uint32_t kPayloadMalformed = 5201;     // truncated, trailing bytes, bad count, bad UTF-8
inline constexpr std::uint32_t kMessageVersionUnsupported = 5202;
inline constexpr std::uint32_t kMessageTypeUnsupported = 5203;  // unknown id or not implemented yet
inline constexpr std::uint32_t kMessageTooLarge = 5204;      // encoder: payload or string over the limit

// --- transport ----------------------------------------------------------------------------------
inline constexpr std::uint32_t kBrokerUnreachable = 5205;    // connect failed (no pipe, access denied)
inline constexpr std::uint32_t kPipeIoFailed = 5206;         // read/write failed with an OS error
inline constexpr std::uint32_t kPipeCreateFailed = 5207;     // server side: cannot create the pipe
inline constexpr std::uint32_t kPipeNameInvalid = 5208;      // pipe name lacks the pipe-namespace prefix
inline constexpr std::uint32_t kPipeServerUntrusted = 5209;  // pipe is owned by a different user

// --- agent / broker behaviour -------------------------------------------------------------------
inline constexpr std::uint32_t kHandshakeFailed = 5210;      // Hello/Welcome mismatch or timeout
inline constexpr std::uint32_t kBrokerStartFailed = 5211;    // auto-start of cml_broker.exe failed
inline constexpr std::uint32_t kModuleNotLoaded = 5212;      // EnableModule: module not in the process
inline constexpr std::uint32_t kModuleNotEnabled = 5213;     // DisableModule: module was not enabled
inline constexpr std::uint32_t kProtocolViolation = 5214;    // message not allowed in this state
inline constexpr std::uint32_t kRequestCancelled = 5215;     // agent destroyed with the request open
inline constexpr std::uint32_t kConnectionLost = 5216;       // connection dropped before the reply
inline constexpr std::uint32_t kRequestQueueFull = 5217;     // too many outstanding requests
inline constexpr std::uint32_t kAgentCreateFailed = 5218;    // agent thread/event could not be created

// --- process and module information (cml_proc) --------------------------------------------------
inline constexpr std::uint32_t kModuleEnumFailed = 5220;
inline constexpr std::uint32_t kSidQueryFailed = 5221;

// --- public header status codes (Chameleon_API.h, see CML_STATUS_*) -----------------------------
inline constexpr std::uint32_t kNotImplemented = 5290;
inline constexpr std::uint32_t kAbiMismatch = 5291;
inline constexpr std::uint32_t kAgentDllNotFound = 5292;
inline constexpr std::uint32_t kInvalidArgument = 5293;
inline constexpr std::uint32_t kAgentLoadFailed = 5294;
inline constexpr std::uint32_t kNotConnected = 5295;  // reserved
inline constexpr std::uint32_t kTimeout = 5296;
inline constexpr std::uint32_t kUnknownRequest = 5297;

}  // namespace cml::ids
