// Compiling one translation unit from its recorded command, and the parallel driver.
#include <windows.h>

#include <chrono>
#include <filesystem>

#include "cml/toolchain/diag_ids.h"
#include "cml/toolchain/invoke.h"
#include "cml/toolchain/tool_info.h"
#include "cml/util/log.h"
#include "cml/util/path.h"
#include "fs_util.h"

namespace cml::toolchain {

namespace {

ToolFamily family_for(Dialect dialect) {
    return dialect == Dialect::Cl ? ToolFamily::Cl : ToolFamily::ClangCl;
}

Diag cancelled_diag(const std::string& source) {
    return Diag::make(diag::kCompileCancelled, "compiling '" + source + "' was cancelled",
                      "Save the file again to restart the compile.");
}

}  // namespace

Result<void> compile_tu(const CompileRequest& request, CompileOutcome& outcome) {
    namespace fs = std::filesystem;
    outcome = CompileOutcome();
    outcome.source = request.record.source;
    outcome.object_path = request.output_object;

    if (request.cancel != nullptr && request.cancel->cancelled()) {
        return cancelled_diag(outcome.source);
    }

    RewriteOptions ro;
    ro.output_object = request.output_object;
    ro.pch_debug = request.pch_debug;
    ro.capture_includes = request.capture_includes;
    Result<RewriteResult> rw = rewrite_for_patch(request.record, ro);
    if (!rw) {
        return rw.error();
    }
    outcome.source = rw->source;
    outcome.warnings = rw->warnings;

    // ---- output directory, no stale object --------------------------------------------------------
    {
        std::error_code ec;
        const fs::path object = detail::to_path(request.output_object);
        if (object.has_parent_path()) {
            fs::create_directories(object.parent_path(), ec);
            if (ec) {
                return Diag::make(diag::kCannotPrepareOutput,
                                  "cannot create the output directory for '" + request.output_object + "': " + ec.message(),
                                  "Check the session temp directory is writable.");
            }
        }
        fs::remove(object, ec);
    }

    // ---- which compiler ---------------------------------------------------------------------------
    std::string compiler = rw->compiler;
    if (compiler.empty() || !detail::file_exists(compiler)) {
        const std::string wanted = compiler;
        if (!request.allow_compiler_fallback) {
            return Diag::make(diag::kCannotStartCompiler, "the recorded compiler '" + wanted + "' does not exist",
                              "Reinstall the toolchain the project was built with.");
        }
        Result<std::string> found = Diag::make(diag::kToolNotFound, "unset", "unset");
        if (!wanted.empty() && !path::has_separator(std::string_view(wanted))) {
            found = locate_executable(wanted, request.fallback_search_dirs);  // bare name from compile_commands.json
        } else {
            found = locate_tool(family_for(rw->dialect), request.fallback_search_dirs);
        }
        if (!found) {
            return Diag::make(diag::kCannotStartCompiler,
                              "the recorded compiler '" + wanted + "' does not exist and no " + default_file_name(family_for(rw->dialect)) +
                                  " was found on PATH",
                              "Run the broker from a developer command prompt or reinstall the toolchain.");
        }
        outcome.warnings.push_back(Diag::make(diag::kCompilerFallback,
                                              "the recorded compiler '" + wanted + "' does not exist; using '" + *found + "'",
                                              "Rebuild the project with the installed toolchain to remove this warning."));
        CML_LOG(Warn, "toolchain: recorded compiler '%s' is missing; using '%s'", wanted.c_str(), found->c_str());
        compiler = *found;
        outcome.compiler_fallback_used = true;
    }
    outcome.compiler_used = compiler;

    // ---- private compiler PDB (PchDebugMode::KeepZiPrivatePdb) -----------------------------------
    if (!rw->pdb_copy_from.empty()) {
        std::error_code ec;
        fs::copy_file(detail::to_path(rw->pdb_copy_from), detail::to_path(rw->pdb_copy_to), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            outcome.warnings.push_back(Diag::make(diag::kPdbCopyFailed,
                                                  "cannot copy the compiler PDB '" + rw->pdb_copy_from + "' for the PCH-using TU: " + ec.message(),
                                                  "Build with /Z7 so no compiler PDB is shared with the patch compile."));
        }
    }

    // ---- run ---------------------------------------------------------------------------------------
    ProcessSpec spec;
    spec.exe = compiler;
    spec.args = rw->args;
    if (!rw->cwd.empty() && fs::is_directory(detail::to_path(rw->cwd))) {
        spec.cwd = rw->cwd;
    }
    spec.timeout_ms = request.timeout_ms;
    spec.force_response_file = request.force_response_file;
    spec.response_file_dir = std::string(path::parent_directory(std::string_view(request.output_object)));
    outcome.command_line = quote_argument(compiler) + " " + rw->command_line;

    const auto started = std::chrono::steady_clock::now();
    Result<ProcessResult> run = run_process(spec, request.cancel);
    outcome.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!run) {
        return run.error();
    }
    outcome.exit_code = run->exit_code;
    outcome.diagnostics = parse_tool_output(run->output);
    outcome.output = reformat_tool_output(run->output);
    if (request.capture_includes) {
        outcome.includes = parse_show_includes(run->output);
    }

    if (run->exit_code != 0 && run->cancelled) {
        return cancelled_diag(outcome.source);
    }
    if (run->timed_out) {
        return Diag::make(diag::kCompileTimedOut,
                          "compiling '" + outcome.source + "' did not finish within " + std::to_string(request.timeout_ms) + " ms",
                          "Raise the compile timeout or check for a stuck compiler process.");
    }
    if (run->exit_code != 0) {
        std::string first;
        for (const ToolDiagnostic& d : outcome.diagnostics) {
            if (d.severity == Severity::Error || d.severity == Severity::FatalError) {
                first = d.format();
                break;
            }
        }
        return Diag::make(diag::kCompileFailed,
                          "compiling '" + outcome.source + "' failed with exit code " + std::to_string(run->exit_code) +
                              (first.empty() ? std::string() : ": " + first),
                          "Fix the errors shown in the compile output and save again.");
    }
    std::error_code ec;
    if (!fs::is_regular_file(detail::to_path(request.output_object), ec)) {
        return Diag::make(diag::kNoObjectProduced,
                          "the compiler succeeded for '" + outcome.source + "' but '" + request.output_object + "' does not exist",
                          "Check the compile output; the command line may redirect the object elsewhere.");
    }
    CML_LOG(Debug, "toolchain: compiled %s in %.2f s", outcome.source.c_str(), outcome.seconds);
    return {};
}

std::vector<Result<void>> compile_all(const std::vector<CompileRequest>& requests, std::vector<CompileOutcome>& outcomes,
                                      unsigned max_parallel, CancelToken* cancel) {
    outcomes.assign(requests.size(), CompileOutcome());
    std::vector<Result<void>> results;
    results.reserve(requests.size());
    for (const CompileRequest& r : requests) {
        results.emplace_back(cancelled_diag(r.record.source));  // replaced below for every request that runs
    }
    run_parallel(
        requests.size(), max_parallel,
        [&](std::size_t i) {
            CompileRequest request = requests[i];
            if (request.cancel == nullptr) {
                request.cancel = cancel;
            }
            results[i] = compile_tu(request, outcomes[i]);
        },
        cancel);
    return results;
}

}  // namespace cml::toolchain
