// DLL of fixture exe_plus_dll: the patch target of dll_scale_30.edit.
#define FIXTURE_LIB_EXPORTS
#include "lib.h"

#define CML_NOINLINE __declspec(noinline)

CML_NOINLINE int lib_scale(int x)
{
    return x * 3;
}

CML_NOINLINE int lib_offset(int x)
{
    return x + 1;
}

CML_NOINLINE int lib_apply(lib_callback cb, int x)
{
    return cb(x);
}
