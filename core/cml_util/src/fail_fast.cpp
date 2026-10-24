#include "cml/util/fail_fast.h"

#include <cstdio>
#include <cstdlib>

namespace cml {

void fail_fast(const char* message) noexcept {
    std::fputs("chameleon: fatal: ", stderr);
    std::fputs(message != nullptr ? message : "(null)", stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::abort();
}

}  // namespace cml
