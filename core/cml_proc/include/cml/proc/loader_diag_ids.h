#pragma once

#include <cstdint>

// Registry of the CML42xx diagnostic ids owned by WP-08 (address-space reservation and patch
// loading in the agent; ADR-0008 reserves CML4200..CML4299 for it). The ids travel on the wire as
// the `status` of PatchLoaded / Reply / ReserveRange answers, so the broker uses the same list.
// `docs/wp/WP-08.md` repeats each id with its "what to do" text.
namespace cml::ids {

inline constexpr std::uint32_t kLoaderInvalidRequest = 4200;   // parameters unusable (zero/misaligned size, ...)
inline constexpr std::uint32_t kLoaderNoAddressWindow = 4201;  // no free address space near the module
inline constexpr std::uint32_t kLoaderBaseMismatch = 4202;     // DLL loaded at a base other than the requested one
inline constexpr std::uint32_t kLoaderRangeTaken = 4203;       // range grabbed by someone else on every attempt
inline constexpr std::uint32_t kLoaderLoadFailed = 4204;       // LoadLibraryExW failed for a reason other than a collision
inline constexpr std::uint32_t kLoaderRangeNotReserved = 4205; // image range not inside a block the agent reserved
inline constexpr std::uint32_t kLoaderImageMismatch = 4206;    // loaded image is not what the request described
inline constexpr std::uint32_t kLoaderRel32OutOfRange = 4207;  // REL32 displacement does not fit 32 bits
inline constexpr std::uint32_t kLoaderFixupInvalid = 4208;     // fix-up site outside the image / misaligned / unwritable
inline constexpr std::uint32_t kLoaderInitializerFailed = 4209;  // initializer entry invalid or it faulted
inline constexpr std::uint32_t kLoaderUnknownPatch = 4210;     // no patch loaded at that base
inline constexpr std::uint32_t kLoaderReleaseFailed = 4211;    // ReleaseRange: range not owned / OS refused
inline constexpr std::uint32_t kLoaderPatchBusy = 4212;        // another operation is using that patch
inline constexpr std::uint32_t kLoaderUnloadFailed = 4213;     // FreeLibrary failed; the image stays mapped
inline constexpr std::uint32_t kLoaderAddressScanFailed = 4214;  // VirtualQuery failed while scanning

}  // namespace cml::ids
