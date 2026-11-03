// The application's hooks (WP-17). Each one says which thread it runs on and whether that thread is inside
// SyncPoint(), and the pre/post-patch hooks print what compute_value() returns at that moment: the original
// code before the hooks are written, the patched code after.
#include <cstdio>

#include "Chameleon_API.h"
#include "logic.h"
#include "state.h"

namespace {

const char* thread_name()
{
    return GetCurrentThreadId() == g_main_thread.load() ? "main" : "other";
}

void OnBeforePatch()
{
    std::printf("HOOK pre thread=%s in_sync=%d value=%d\n", thread_name(), g_in_sync.load(), compute_value());
}

void OnAfterPatch()
{
    std::printf("HOOK post thread=%s in_sync=%d value=%d\n", thread_name(), g_in_sync.load(), compute_value());
}

void OnCompileStart()
{
    std::printf("HOOK compile-start thread=%s in_sync=%d\n", thread_name(), g_in_sync.load());
}

void OnCompileSuccess()
{
    std::printf("HOOK compile-success thread=%s in_sync=%d\n", thread_name(), g_in_sync.load());
}

void OnCompileError(const char* message)
{
    std::printf("HOOK compile-error thread=%s in_sync=%d message=\"%.120s\"\n", thread_name(), g_in_sync.load(), message);
}

} // namespace

CML_PREPATCH_HOOK(OnBeforePatch);
CML_POSTPATCH_HOOK(OnAfterPatch);
CML_COMPILE_START_HOOK(OnCompileStart);
CML_COMPILE_SUCCESS_HOOK(OnCompileSuccess);
CML_COMPILE_ERROR_HOOK(OnCompileError);
