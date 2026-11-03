#include "cml/core/check_command.h"

#include "cml/core/module_audit.h"

namespace cml::core {

void print_check_usage(std::FILE* out) {
    std::fputs(
        "usage: cml_broker check [--json] [--verbose] [--pdb-dir DIR]... <module.exe|module.dll>\n"
        "\n"
        "  Audits one module against the build requirements (plan 4.2) without a running process and\n"
        "  prints every check as PASS, WARN or FAIL with its diagnostic id. Exit code 0: no FAIL;\n"
        "  1: at least one FAIL; 2: bad command line.\n"
        "  --json         one JSON object instead of text\n"
        "  --verbose      also print the evidence behind each verdict\n"
        "  --pdb-dir DIR  also look for the module's PDB in DIR (repeatable)\n",
        out);
}

int run_check_command(const std::vector<std::string>& args, std::FILE* out, std::FILE* err) {
    bool json = false;
    bool verbose = false;
    std::string module;
    pdb::OpenOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json") {
            json = true;
        } else if (a == "--verbose") {
            verbose = true;
        } else if (a == "--pdb-dir") {
            if (i + 1 >= args.size()) {
                std::fprintf(err, "cml_broker check: --pdb-dir needs a directory\n");
                print_check_usage(err);
                return 2;
            }
            options.override_dirs.push_back(args[++i]);
        } else if (a == "--help") {
            print_check_usage(out);
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(err, "cml_broker check: unknown option '%s'\n", a.c_str());
            print_check_usage(err);
            return 2;
        } else if (module.empty()) {
            module = a;
        } else {
            std::fprintf(err, "cml_broker check: only one module can be checked at a time ('%s' is extra)\n", a.c_str());
            print_check_usage(err);
            return 2;
        }
    }
    if (module.empty()) {
        std::fprintf(err, "cml_broker check: no module given\n");
        print_check_usage(err);
        return 2;
    }
    const ModuleAudit audit = audit_module_file(module, options);
    if (json) {
        std::fprintf(out, "%s\n", ipc::json::serialize(audit_to_json(audit)).c_str());
    } else {
        std::fputs(format_audit_text(audit, verbose).c_str(), out);
    }
    std::fflush(out);
    return audit.has_failures() ? 1 : 0;
}

}  // namespace cml::core
