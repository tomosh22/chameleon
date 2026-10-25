#pragma once

namespace cml {

// Prints `message` to stderr and aborts the process. Used for programmer errors such as reading
// the value of a failed Result<T>; there are no exceptions in core/ and agent/.
//
// Not declared noexcept on purpose: unit tests install a handler (below) that throws so that
// the abort paths can be tested. Production code never installs one.
[[noreturn]] void fail_fast(const char* message);

// Test hook. When set, fail_fast calls the handler first; if the handler returns, fail_fast
// still aborts. Pass nullptr to restore the default. Not thread-safe against concurrent
// fail_fast calls; install it at the start of a test and remove it at the end.
using FailFastHandler = void (*)(const char* message);
void set_fail_fast_handler(FailFastHandler handler) noexcept;

}  // namespace cml
