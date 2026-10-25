#pragma once

#include <cstdint>

// Registry of the CML43xx hook-installer diagnostic ids (ADR-0008: CML4300-4399 belong to WP-10).
// docs/wp/WP-10.md repeats every id with its message and "what to do" text.
namespace cml::agent::hook::ids {

inline constexpr std::uint32_t kDecodeUnknown = 4300;         // an instruction in the overwritten region is not understood
inline constexpr std::uint32_t kFunctionTooShort = 4301;      // < 5 bytes and no trailing padding
inline constexpr std::uint32_t kOutOfRange = 4302;            // the jump target is not within +-2 GB
inline constexpr std::uint32_t kCodeUnreadable = 4303;        // the function's bytes cannot be read
inline constexpr std::uint32_t kCallInWindow = 4304;          // a call ends inside the overwritten region
inline constexpr std::uint32_t kThreadInWindow = 4305;        // a thread stayed inside the region for all retries
inline constexpr std::uint32_t kThreadEnumFailed = 4306;      // cannot enumerate threads / too many threads
inline constexpr std::uint32_t kThreadsNotConverging = 4307;  // new threads kept appearing
inline constexpr std::uint32_t kWriteFailed = 4308;           // VirtualProtect failed
inline constexpr std::uint32_t kWindowTimeout = 4309;         // threads were stopped longer than the budget
inline constexpr std::uint32_t kBadArgument = 4310;           // null / equal / duplicate addresses in the request
inline constexpr std::uint32_t kNotInstalled = 4311;          // revert of a function that has no hook
inline constexpr std::uint32_t kCodeChanged = 4312;           // the bytes are not the ones recorded or planned
inline constexpr std::uint32_t kBatchAborted = 4313;          // not attempted: another hook of the request failed
inline constexpr std::uint32_t kSuspendFailed = 4314;         // a thread could not be suspended or inspected
inline constexpr std::uint32_t kTooManyHooks = 4315;          // request above the per-request limit
inline constexpr std::uint32_t kOverlap = 4316;               // the bytes of two hooks overlap

}  // namespace cml::agent::hook::ids
