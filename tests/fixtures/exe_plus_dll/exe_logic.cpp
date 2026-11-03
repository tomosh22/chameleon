// Exe side of fixture exe_plus_dll: the patch target of exe_changes.edit.
#include "exe_logic.h"
#include "lib.h"

#define CML_NOINLINE __declspec(noinline)

CML_NOINLINE int exe_callback(int x)
{
    return x + 1;
}

CML_NOINLINE int exe_import_user(int x)
{
    return lib_offset(x) * 2;
}
