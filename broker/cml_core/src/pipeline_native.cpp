#include "cml/core/pipeline.h"

#include "cml/core/pipeline_diag_ids.h"
#include "cml/toolchain/link_patch.h"

namespace cml::core {

PipelineEnv native_pipeline_env() {
    PipelineEnv env;
#if defined(_WIN32)
    env.compile_all = [](const std::vector<toolchain::CompileRequest>& requests, std::vector<toolchain::CompileOutcome>& outcomes, unsigned max_parallel,
                         toolchain::CancelToken* cancel, const std::function<void(std::size_t)>& on_done) {
        return toolchain::compile_all(requests, outcomes, max_parallel, cancel, on_done);
    };
    env.resolve_linker = [](const std::string& module_path, const pdb::PdbIndex* module_pdb, const std::vector<std::string>& search_dirs,
                            const ReloadOptions& options, std::vector<Diag>* warnings) -> Result<toolchain::ToolInfo> {
        toolchain::DetectLinkerOptions detect;
        if (options.linker_override == "link") {
            detect.override_family = toolchain::ToolFamily::Link;
        } else if (options.linker_override == "lld-link") {
            detect.override_family = toolchain::ToolFamily::LldLink;
        }
        // WP-39b: the reload holds the module's PDB index; the detection reads it and only the headers of the module file.
        // Without one the module's own PDB is opened and released again inside (ADR-0017: no mapping outlives the call).
        Result<toolchain::LinkerDetection> detection = module_pdb != nullptr
                                                           ? toolchain::detect_module_linker_with_pdb(module_path, *module_pdb, detect)
                                                           : toolchain::detect_module_linker(module_path, pdb::OpenOptions{}, detect);
        if (!detection) {
            return detection.error();
        }
        if (warnings != nullptr) {
            for (const Diag& w : detection->warnings) {
                warnings->push_back(w);
            }
        }
        toolchain::ResolveLinkerOptions resolve;
        resolve.explicit_path = options.linker_path;
        resolve.search_dirs = search_dirs;
        return toolchain::resolve_linker(detection->family, resolve);
    };
    env.link_patch = [](const toolchain::LinkPatchRequest& request, toolchain::LinkTrace* trace) { return toolchain::link_patch(request, trace); };
#endif
    return env;
}

}  // namespace cml::core
