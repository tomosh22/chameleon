#pragma once

// cml_toolchain umbrella header (WP-05): command-line parsing and rewriting for cl and clang-cl,
// toolchain detection, build-flag detection, compiler invocation. See docs/wp/WP-05.md.

#include "cml/toolchain/build_flags.h"
#include "cml/toolchain/command_line.h"
#include "cml/toolchain/compile_commands.h"
#include "cml/toolchain/diag_ids.h"
#include "cml/toolchain/invoke.h"
#include "cml/toolchain/record.h"
#include "cml/toolchain/rewrite.h"
#include "cml/toolchain/symbol_set.h"
#include "cml/toolchain/tool_info.h"

namespace cml::toolchain {

const char* library_name() noexcept;

}  // namespace cml::toolchain
