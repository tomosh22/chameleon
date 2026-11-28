// The DLL's translation unit that includes scale.h (scenario cross_module_atomic).
#define FIXTURE_PLUGIN_EXPORTS
#include "plugin.h"
#include "shared.h"

#if CML_PLUGIN_TLS
thread_local int t_extra = 0;  // NEW thread_local: the planner refuses this translation unit (CML3400)
#endif

int plugin_value()
{
#if CML_PLUGIN_TLS
    return shared_scale() * 100 + kScale + t_extra;
#else
    return shared_scale() * 100 + kScale;
#endif
}
