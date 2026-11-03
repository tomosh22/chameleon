#pragma once

// Application hooks registered with the real CML_*_HOOK macros inside the test executable (WP-17), with a
// log the tests read. Kept in its own translation unit because the macros need Chameleon_API.h as an
// application sees it (no CML_AGENT_BUILD), while the dispatcher tests include the agent's headers.
#if defined(_WIN32)

#include <string>
#include <vector>

namespace hooktest_user {

// Clears the log. `probe` (may be null) is called by the pre/post patch hooks with 1; its result is logged.
void reset(int (*probe)(int));
// "pre:<probe result>", "post-a:<...>", "post-b:<...>", "compile-start", "compile-success", "compile-error:<text>".
std::vector<std::string> log();
// Thread id of every logged entry, same order.
std::vector<unsigned long> threads();
// Makes the middle post-patch hook raise a processor exception.
void set_fault(bool on);
// Full path of this executable, UTF-8.
std::string module_path_utf8();

}  // namespace hooktest_user

#endif
