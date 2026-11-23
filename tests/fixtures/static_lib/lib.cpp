// Member of the static library of fixture `static_lib`: the patch target of edits/value_70.edit.
#include "lib.h"

#define CML_NOINLINE __declspec(noinline)

namespace {
int s_calls = 0;  // file-static data of the member; the patched code must keep using the running image's copy
}

CML_NOINLINE int static_lib_value()
{
    ++s_calls;
    return 7;
}

CML_NOINLINE int static_lib_calls()
{
    return s_calls;
}
