#pragma once

// `cml_broker check [--json] [--verbose] [--pdb-dir DIR]... <exe|dll>` (WP-18): the offline audit of one module
// (module_audit.h). `args` are the arguments AFTER the word "check". Exit code: 0 no FAIL (WARN is allowed), 1 at least
// one FAIL, 2 usage error. Output goes to `out` (the report) and `err` (usage errors). Portable.

#include <cstdio>
#include <string>
#include <vector>

namespace cml::core {

int run_check_command(const std::vector<std::string>& args, std::FILE* out, std::FILE* err);

void print_check_usage(std::FILE* out);

}  // namespace cml::core
