#pragma once

namespace cml {

// Prints `message` to stderr and aborts the process. Used for programmer errors such as reading
// the value of a failed Result<T>; there are no exceptions in core/ and agent/.
[[noreturn]] void fail_fast(const char* message) noexcept;

}  // namespace cml
