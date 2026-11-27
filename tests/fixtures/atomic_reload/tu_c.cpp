#include "shared.h"
#include "tus.h"

#if CML_EXTRA_TLS
thread_local int t_extra = 0;  // NEW thread_local: the planner refuses this translation unit (CML3400)
#endif

int c_value()
{
#if CML_EXTRA_TLS
    return shared_scale() * 1000 + kScale + t_extra;
#else
    return shared_scale() * 1000 + kScale;
#endif
}
