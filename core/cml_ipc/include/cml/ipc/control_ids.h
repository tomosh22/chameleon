#pragma once

#include <cstdint>

// Diagnostic ids CML5400..5499, owned by WP-13 (control protocol and the cmltest harness).
// docs/wp/WP-13.md repeats every id with its message and "what to do" text.
namespace cml::ids {

// --- control protocol (cml_ipc/json, cml_ipc/control, broker control server) --------------------
inline constexpr std::uint32_t kControlJsonMalformed = 5400;    // line is not valid JSON (offset given)
inline constexpr std::uint32_t kControlRequestInvalid = 5401;   // JSON ok, but not a valid request/response
inline constexpr std::uint32_t kControlUnknownCommand = 5402;   // command not known / not provided by this broker
inline constexpr std::uint32_t kControlLineTooLong = 5403;      // message above 1 MiB without newline / too big to send
inline constexpr std::uint32_t kControlUnreachable = 5404;      // cannot connect to the control pipe
inline constexpr std::uint32_t kControlIoFailed = 5405;         // control pipe closed or failed mid-conversation
inline constexpr std::uint32_t kControlCapabilityMissing = 5406;  // broker lacks a required capability
inline constexpr std::uint32_t kControlResponseInvalid = 5407;  // response does not match the request / bad shape
inline constexpr std::uint32_t kControlTimeout = 5408;          // no answer within the timeout

// --- cmltest harness ----------------------------------------------------------------------------
inline constexpr std::uint32_t kScenarioSyntax = 5410;          // scenario.toml is not in the supported TOML subset
inline constexpr std::uint32_t kScenarioInvalid = 5411;         // unknown key/kind, missing field, bad value
inline constexpr std::uint32_t kPatchMalformed = 5412;          // unified diff cannot be parsed
inline constexpr std::uint32_t kPatchMismatch = 5413;           // a hunk does not match the target file
inline constexpr std::uint32_t kFixtureBuildFailed = 5414;      // cmake configure/build of the fixture failed
inline constexpr std::uint32_t kLaunchFailed = 5415;            // a child process could not be started
inline constexpr std::uint32_t kExpectStdoutTimeout = 5416;     // expect_stdout: no match in time
inline constexpr std::uint32_t kExpectDiagTimeout = 5417;       // expect_diag: diagnostic not seen in time
inline constexpr std::uint32_t kExpectModuleTimeout = 5418;     // expect_module_enabled: not reported in time
inline constexpr std::uint32_t kScenarioFailed = 5419;          // process died early / scenario timeout
inline constexpr std::uint32_t kRegexInvalid = 5420;            // expect_stdout pattern cannot be compiled
inline constexpr std::uint32_t kHarnessUsage = 5421;            // bad command line / missing tool or file
inline constexpr std::uint32_t kEditFailed = 5422;              // edit step: file could not be read/written
inline constexpr std::uint32_t kDumpFailed = 5423;              // crash dump could not be written
inline constexpr std::uint32_t kReloadFailed = 5424;            // reload command answered with an error
inline constexpr std::uint32_t kCapabilitySkipped = 5425;       // steps skipped for a missing capability (failure under --require-all-capabilities)
inline constexpr std::uint32_t kJunitWriteFailed = 5426;        // report file could not be written

}  // namespace cml::ids
