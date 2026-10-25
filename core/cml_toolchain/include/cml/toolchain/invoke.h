#pragma once

// Running compilers (plan section 4.4): CreateProcess with captured output, cancellation through a
// job object, a parallel job pool, and re-emission of compiler output as clickable
// `file(line,col): severity CODE: message` lines.
//
// Portable here: CancelToken, run_parallel, parse_tool_output. Windows only: run_process,
// compile_tu, compile_all.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cml/toolchain/record.h"
#include "cml/toolchain/rewrite.h"
#include "cml/util/result.h"

namespace cml::toolchain {

// ---- cancellation -------------------------------------------------------------------------
// One token can cancel any number of running processes. cancel() is sticky and callable from any
// thread; a process registered after cancel() is killed at registration.
class CancelToken {
public:
    using Killer = std::function<void()>;
    using Handle = std::uint64_t;

    void cancel();
    bool cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }

    // For process runners: `killer` is called (once, from the cancelling thread) on cancel().
    // If the token is already cancelled the killer runs before add_killer returns.
    Handle add_killer(Killer killer);
    void remove_killer(Handle handle);

private:
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::map<Handle, Killer> killers_;
    Handle next_ = 1;
};

// ---- parallel job pool --------------------------------------------------------------------
// min(jobs, hardware_concurrency) workers (or `max_workers` when non-zero and smaller).
unsigned pool_size(std::size_t jobs, unsigned max_workers = 0) noexcept;
// Calls work(index) for every index in [0, count) on pool_size(count, max_workers) threads and
// returns when all are done. When `cancel` is set, indices not yet started are skipped (their
// work() is never called). The calling thread participates, so a pool of one runs inline.
void run_parallel(std::size_t count, unsigned max_workers, const std::function<void(std::size_t index)>& work,
                  const CancelToken* cancel = nullptr);

// ---- compiler output ----------------------------------------------------------------------
enum class Severity { Note, Warning, Error, FatalError };
const char* to_string(Severity severity) noexcept;

struct ToolDiagnostic {
    Severity severity = Severity::Error;
    std::string file;      // empty for "cl : Command line error D8003 : ..."
    int line = 0;          // 0 = none
    int column = 0;        // 0 = none
    std::string code;      // "C2065", "LNK1104", "" (clang)
    std::string message;
    std::string tool;      // set instead of `file` for "cl : ..." / "LINK : ..." forms
    // The canonical clickable form: `file(line,col): error C2065: message`.
    std::string format() const;
};

// Recognises MSVC-style (`file(12,5): error C2065: ...`, `cl : Command line error D8016 : ...`,
// `LINK : fatal error LNK1104: ...`) and clang GCC-style (`file:12:5: error: ...`) lines.
// Other lines are ignored. Windows drive letters are handled.
std::vector<ToolDiagnostic> parse_tool_output(std::string_view output);
// Lines of `output` that are `Note: including file:` (cl, clang-cl): the included paths.
std::vector<std::string> parse_show_includes(std::string_view output);
// Output with every recognised diagnostic line rewritten to its clickable form, others verbatim.
std::string reformat_tool_output(std::string_view output);

// ---- processes (Windows) ------------------------------------------------------------------
struct ProcessSpec {
    std::string exe;                      // full path of the program
    std::vector<std::string> args;        // WITHOUT the program name; quoted with quote_argument()
    std::string cwd;                      // working directory ("" = inherit)
    std::vector<std::pair<std::string, std::string>> env_overrides;  // added to / replacing the inherited environment
    unsigned timeout_ms = 0;              // 0 = none
    bool force_response_file = false;     // always pass the arguments in an @file
    std::string response_file_dir;        // where @files are written ("" = the temp directory)
    std::size_t max_command_line = 30000; // longer command lines go into an @file (CreateProcess limit is 32767)
};

struct ProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    bool cancelled = false;
    std::string output;                   // stdout and stderr, interleaved in arrival order, as UTF-8 bytes
    std::string command_line;             // what was actually passed to CreateProcess
    std::string response_file;            // set when an @file was used (deleted after the run)
};

#if defined(_WIN32)
// CML2202 when the process cannot be created, CML2206 when the response file cannot be written.
// A process that ran and failed is NOT an error here (see exit_code).
Result<ProcessResult> run_process(const ProcessSpec& spec, CancelToken* cancel = nullptr);
#endif

// ---- compiling one translation unit (Windows) ---------------------------------------------
struct CompileRequest {
    CompileRecord record;
    std::string output_object;            // absolute; see patch_object_path()
    PchDebugMode pch_debug = PchDebugMode::Auto;
    bool capture_includes = false;
    bool allow_compiler_fallback = true;  // use the toolchain found in the environment when the recorded one is gone
    std::vector<std::string> fallback_search_dirs;  // tried before PATH when looking for the fallback
    bool force_response_file = false;
    unsigned timeout_ms = 0;
    CancelToken* cancel = nullptr;
};

struct CompileOutcome {
    std::string source;
    std::string object_path;
    std::string compiler_used;
    std::string command_line;             // program + arguments as run
    int exit_code = -1;
    double seconds = 0.0;
    std::string output;                   // the compiler's output, diagnostics rewritten to clickable form
    std::vector<ToolDiagnostic> diagnostics;
    std::vector<std::string> includes;    // when capture_includes
    std::vector<Diag> warnings;           // rewrite warnings, fallback notice, ...
    bool compiler_fallback_used = false;
};

#if defined(_WIN32)
// CML2200 (the compiler reported errors; `outcome.diagnostics` has them), CML2201 is a warning in
// outcome.warnings, CML2202 (no compiler at all), CML2203 (cancelled), CML2204 (timeout), CML2205
// (exit code 0 but no object), CML2206 (cannot create the output directory), plus every rewrite
// failure (CML15xx, CML2102). `outcome` is filled as far as it got, also on failure.
Result<void> compile_tu(const CompileRequest& request, CompileOutcome& outcome);

// Compiles all requests with min(requests, hardware_concurrency) workers. outcomes[i] and the
// returned results[i] belong to requests[i]. On cancel, unstarted requests fail with CML2203.
std::vector<Result<void>> compile_all(const std::vector<CompileRequest>& requests, std::vector<CompileOutcome>& outcomes,
                                      unsigned max_parallel = 0, CancelToken* cancel = nullptr);
#endif

}  // namespace cml::toolchain
