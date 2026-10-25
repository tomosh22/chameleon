#include "cml/util/fail_fast.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace cml {

namespace {
std::atomic<FailFastHandler> g_handler{nullptr};
}  // namespace

void set_fail_fast_handler(FailFastHandler handler) noexcept {
    g_handler.store(handler, std::memory_order_release);
}

void fail_fast(const char* message) {
    const FailFastHandler handler = g_handler.load(std::memory_order_acquire);
    if (handler != nullptr) {
        handler(message);
    }
    std::fputs("chameleon: fatal: ", stderr);
    std::fputs(message != nullptr ? message : "(null)", stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::abort();
}

}  // namespace cml
