// The linker driver, Windows part (WP-09): finding the linker and running it.
#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "cml/toolchain/command_line.h"
#include "cml/toolchain/diag_ids.h"
#include "cml/toolchain/link_patch.h"
#include "cml/util/log.h"
#include "fs_util.h"

namespace cml::toolchain {

namespace {

namespace fs = std::filesystem;

std::string env_value(const char* name) {
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
}

void add_candidate(std::vector<std::string>& out, const fs::path& p) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec) && !ec) {
        out.push_back(detail::to_utf8(p));
    }
}

}  // namespace

Result<ToolInfo> resolve_linker(ToolFamily family, const ResolveLinkerOptions& options) {
    if (family != ToolFamily::Link && family != ToolFamily::LldLink) {
        return Diag::make(diag::kLinkerNotFound, std::string("'") + to_string(family) + "' is not a linker", "Use link or lld-link.");
    }
    const std::string exe_name = default_file_name(family);
    std::vector<std::string> candidates;
    if (!options.explicit_path.empty()) {
        if (!detail::file_exists(options.explicit_path)) {
            return Diag::make(diag::kLinkerNotFound, "the linker '" + options.explicit_path + "' does not exist",
                              "Check the linker path in the configuration.");
        }
        candidates.push_back(options.explicit_path);
    } else {
        for (const std::string& dir : options.search_dirs) {
            add_candidate(candidates, detail::to_path(dir) / exe_name);
        }
        if (family == ToolFamily::Link) {
            const std::string vc = env_value("VCToolsInstallDir");
            if (!vc.empty()) {
                add_candidate(candidates, detail::to_path(vc) / "bin" / "Hostx64" / "x64" / exe_name);
            }
        } else {
            add_candidate(candidates, fs::path("C:\\Program Files\\LLVM\\bin") / exe_name);
            const std::string pf = env_value("ProgramFiles");
            if (!pf.empty()) {
                add_candidate(candidates, detail::to_path(pf) / "LLVM" / "bin" / exe_name);
            }
        }
        Result<std::string> on_path = locate_executable(exe_name);
        if (on_path) {
            candidates.push_back(*on_path);
        }
    }
    std::string tried;
    for (const std::string& c : candidates) {
        Result<ToolInfo> info = detect_tool(c);
        if (info && info->family == family) {
            return info;
        }
        tried += "\n  " + c + (info ? std::string(" is ") + to_string(info->family) : " (" + info.error().message + ")");
    }
    return Diag::make(diag::kLinkerNotFound,
                      std::string("no ") + to_string(family) + " found" + (tried.empty() ? "" : "; tried:" + tried),
                      family == ToolFamily::Link ? "Run from a Visual Studio developer environment or set the linker path."
                                                 : "Install LLVM (lld-link.exe) or set the linker path.");
}

Result<LinkedPatch> link_patch(const LinkPatchRequest& request, LinkTrace* trace) {
    using Clock = std::chrono::steady_clock;
    LinkTrace local;
    LinkTrace& tr = trace != nullptr ? *trace : local;
    tr = LinkTrace();

    if (request.linker.path.empty() || (request.linker.family != ToolFamily::Link && request.linker.family != ToolFamily::LldLink)) {
        return Diag::make(diag::kBadLinkRequest, "the link request has no resolved linker", "Call resolve_linker() first.");
    }
    if (request.out_dir.empty() || request.name.empty()) {
        return Diag::make(diag::kBadLinkRequest, "the link request has no output directory or name", "Name the output.");
    }
    if (request.cancel != nullptr && request.cancel->cancelled()) {
        return Diag::make(diag::kLinkCancelled, "linking '" + request.name + "' was cancelled", "Save the file again to restart the patch.");
    }

    const fs::path dir = detail::to_path(request.out_dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return Diag::make(diag::kCannotPrepareLink, "cannot create the output directory '" + request.out_dir + "': " + ec.message(),
                          "Check that the session temp directory is writable.");
    }
    const std::string out_dll = detail::to_utf8(dir / (request.name + ".dll"));
    const std::string out_pdb = detail::to_utf8(dir / (request.name + ".pdb"));
    // No stale output: a failed link must not look like a successful one. Errors are ignored here (a PDB that is still
    // mapped makes the link itself fail, which is reported with the linker's message).
    static_cast<void>(fs::remove(detail::to_path(out_dll), ec));
    static_cast<void>(fs::remove(detail::to_path(out_pdb), ec));

    LinkCommandSpec spec;
    spec.family = request.linker.family;
    for (const PatchObjectInput& o : request.objects) {
        spec.objects.push_back(o.path);
    }
    if (!request.binding_object.empty()) {
        spec.objects.push_back(request.binding_object);
    } else {
        // Nothing to bind: link a padding binding object anyway (see make_padding_binding_object()).
        Result<std::vector<std::byte>> pad = make_padding_binding_object();
        if (!pad) {
            return pad.error();
        }
        const fs::path pad_path = dir / (request.name + ".pad.obj");
        std::ofstream pf(pad_path, std::ios::binary | std::ios::trunc);
        pf.write(reinterpret_cast<const char*>(pad->data()), static_cast<std::streamsize>(pad->size()));
        pf.close();
        if (!pf) {
            return Diag::make(diag::kCannotPrepareLink, "cannot write the padding binding object '" + detail::to_utf8(pad_path) + "'",
                              "Check that the session temp directory is writable and not full.");
        }
        tr.padding_binding_object = detail::to_utf8(pad_path);
        spec.objects.push_back(tr.padding_binding_object);
    }
    spec.out_dll = out_dll;
    spec.out_pdb = out_pdb;
    spec.base = request.base;
    spec.extra_args = request.extra_args;
    spec.ignore_warnings = request.ignore_warnings;
    Result<std::vector<std::string>> args = build_link_command(spec);
    if (!args) {
        return args.error();
    }
    tr.args = *args;

    ProcessSpec ps;
    ps.exe = request.linker.path;
    ps.cwd = request.out_dir;
    ps.timeout_ms = request.timeout_ms;
    ps.max_command_line = 32000;  // run_process must not wrap a second time: the response file is written here
    ps.args = *args;
    if (request.force_response_file || needs_response_file(request.linker.path, *args, request.max_command_line)) {
        const fs::path rsp = dir / (request.name + ".rsp");
        const std::vector<std::byte> bytes = render_response_file(*args);
        std::ofstream f(rsp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        f.close();
        if (!f) {
            return Diag::make(diag::kCannotPrepareLink, "cannot write the response file '" + detail::to_utf8(rsp) + "'",
                              "Check that the session temp directory is writable and not full.");
        }
        tr.response_file = detail::to_utf8(rsp);
        ps.args = {"@" + tr.response_file};
    }

    CML_LOG(Debug, "link: %s %s", request.linker.path.c_str(), join_command_line(*args).c_str());
    const Clock::time_point t0 = Clock::now();
    Result<ProcessResult> run = run_process(ps, request.cancel);
    tr.link_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (!run) {
        return Diag::make(diag::kCannotStartLinker, "cannot start the linker '" + request.linker.path + "': " + run.error().message,
                          "Check that the linker exists and can be run.");
    }
    tr.output = run->output;
    tr.exit_code = run->exit_code;
    tr.command_line = run->command_line;
    if (run->cancelled) {
        return Diag::make(diag::kLinkCancelled, "linking '" + request.name + "' was cancelled", "Save the file again to restart the patch.");
    }
    if (run->timed_out) {
        return Diag::make(diag::kLinkTimedOut,
                          "linking '" + request.name + "' did not finish within " + std::to_string(request.timeout_ms) + " ms",
                          "Check the linker output and for a hung mspdbsrv.exe.");
    }
    const LinkOutputSummary summary = summarize_link_output(run->output);
    if (run->exit_code != 0) {
        if (!summary.unresolved.empty()) {
            return Diag::make(diag::kUnresolvedSymbol,
                              "unresolved symbol '" + summary.unresolved.front() + "' while linking '" + request.name + "'" +
                                  (summary.unresolved.size() > 1 ? " (+" + std::to_string(summary.unresolved.size() - 1) + " more)" : ""),
                              "The bind list is missing this symbol, or the patch references something that is neither defined nor bound.");
        }
        return Diag::make(diag::kLinkFailed,
                          "linking '" + request.name + "' failed (exit code " + std::to_string(run->exit_code) + ")" +
                              (summary.errors.empty() ? std::string() : ": " + summary.errors.front()),
                          "See the linker output in the log.");
    }
    std::error_code e1;
    std::error_code e2;
    if (!fs::is_regular_file(detail::to_path(out_dll), e1) || !fs::is_regular_file(detail::to_path(out_pdb), e2)) {
        return Diag::make(diag::kNoLinkOutput, "the linker succeeded but '" + out_dll + "' or '" + out_pdb + "' was not written",
                          "Check the linker output in the log.");
    }

    ReadPatchRequest rp;
    rp.dll_path = out_dll;
    rp.pdb_path = out_pdb;
    rp.base = request.base;
    rp.objects = request.objects;
    rp.bindings = request.bindings;
    rp.new_functions = request.new_functions;
    Result<LinkedPatch> patch = read_linked_patch(rp);
    if (!patch) {
        return patch;
    }
    patch->link_ms = tr.link_ms;
    patch->linker_family = request.linker.family;
    patch->linker_path = request.linker.path;
    patch->linker_version = request.linker.version_text;
    for (const std::string& w : summary.warnings) {
        patch->warnings.push_back(Diag::make(diag::kLinkerWarning, "the linker warned: " + w, "Usually harmless; see the linker output in the log."));
    }
    return patch;
}

}  // namespace cml::toolchain
