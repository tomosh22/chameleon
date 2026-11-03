// DLL of fixture two_processes: the patch target of value_70.edit.
#define FIXTURE_LIB_EXPORTS
#include "lib.h"

namespace {
int s_calls = 0;  // file-static data of the DLL: every process has its own, and a patch must keep using it
}

__declspec(noinline) int tp_value()
{
    ++s_calls;
    return 7;
}

__declspec(noinline) int tp_calls()
{
    return s_calls;
}
