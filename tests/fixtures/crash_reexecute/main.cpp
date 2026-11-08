// Fixture `crash_reexecute` (WP-23): see CMakeLists.txt. Prints
//   TICK n=<k> stage=<s> result=<r> seen_k=<k> seen_scale=<x.xxx> opt=<o>
// Stages (the harness' signal file starts stage 1):
//   0  waiting for the signal; step(&g_slot, n, 0.5) every frame
//   1  ONE frame calls step(nullptr, 13, 2.75): a null dereference in step(), which is built without optimisation
//   2  four ordinary frames (step(&g_slot, n, 0.5))
//   3  ONE frame calls opt_step(nullptr, 21): the same fault in a function built with /O2
//   4  ordinary frames that call opt_step(&g_slot, n)
// `seen_k` and `seen_scale` are what the last step() call saw: after "Re-execute" the stage-1 line shows 13 and 2.750 although the
// call came back through the fixed code (result = -7 + 13 + 2750 + 100000 = 102756 with edits/fix_step.edit).
#include <windows.h>

#include <cstdio>

#include "fixture_runtime.h"
#include "logic.h"

int g_slot = 7;
int g_seen_k = 0;
double g_seen_scale = 0.0;

namespace {

wchar_t g_signal_file[4096] = L"";
int g_stage = 0;
int g_stage2_frames = 0;

}  // namespace

int main(int argc, char** argv)
{
    GetEnvironmentVariableW(L"CML_FIXTURE_SIGNAL_FILE", g_signal_file, 4096);
    return cml_fixture::run(argc, argv, [](int n) {
        if (g_stage == 0 && g_signal_file[0] != L'\0' && GetFileAttributesW(g_signal_file) != INVALID_FILE_ATTRIBUTES) {
            g_stage = 1;
            std::printf("ARMED\n");
        }
        int result = 0;
        int opt = -1;
        switch (g_stage) {
            case 1:
                result = step(nullptr, 13, 2.75);
                break;
            case 3:
                result = step(&g_slot, n, 0.5);
                opt = opt_step(nullptr, 21);
                break;
            case 4:
                result = step(&g_slot, n, 0.5);
                opt = opt_step(&g_slot, n);
                break;
            default:
                result = step(&g_slot, n, 0.5);
                break;
        }
        std::printf("TICK n=%d stage=%d result=%d seen_k=%d seen_scale=%.3f opt=%d\n", n, g_stage, result, g_seen_k, g_seen_scale, opt);
        if (g_stage == 1) {
            g_stage = 2;
        } else if (g_stage == 2 && ++g_stage2_frames >= 4) {
            g_stage = 3;
        } else if (g_stage == 3) {
            g_stage = 4;
        }
    });
}
