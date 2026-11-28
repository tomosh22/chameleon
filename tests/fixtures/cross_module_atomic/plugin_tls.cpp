// Never edited. The DLL's own thread_local, so the old DLL image has thread-local storage and the refusal of the edit is that of
// the NEW thread_local in plugin.cpp (CML3400), not of the TLS machinery.
#define FIXTURE_PLUGIN_EXPORTS
#include "plugin.h"

thread_local int t_frames = 0;

int plugin_frames()
{
    return ++t_frames;
}
