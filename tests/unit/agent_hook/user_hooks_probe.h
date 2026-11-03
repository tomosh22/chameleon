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
// Makes the middle post-patch hook fail: 0 = no, 1 = raise a processor exception, 2 = overflow the stack.
void set_fault(int mode);
// Called by the pre-patch hook and by the compile-start hook, after they logged ("pre" / "compile-start").
using Action = void (*)(void* context, const char* where);
void set_action(Action action, void* context);
// The compile-start hook sleeps this long (a slow hook).
void set_compile_start_delay_ms(unsigned ms);
// Full path of this executable, UTF-8.
std::string module_path_utf8();

}  // namespace hooktest_user

#endif
