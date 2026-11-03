// Fixture `exe_plus_dll`: prints `TICK n=<k> dll=<lib_scale(2)> cb=<lib_apply(exe_callback, 5)> imp=<exe_import_user(4)>`.
//   dll  exe -> DLL: the exe calls lib_scale through its import slot (DLL function patched)
//   cb   DLL -> exe: the DLL calls exe_callback through a function pointer (exe function patched)
//   imp  patched exe code reading the DLL's import slot (__imp_lib_offset)
#include <cstdio>

#include "exe_logic.h"
#include "fixture_runtime.h"
#include "lib.h"

int main(int argc, char** argv)
{
    cml_fixture::extra_modules().push_back(L"fixture_exe_plus_dll_lib.dll");
    return cml_fixture::run(argc, argv, [](int n) {
        std::printf("TICK n=%d dll=%d cb=%d imp=%d\n", n, lib_scale(2), lib_apply(exe_callback, 5), exe_import_user(4));
    });
}
