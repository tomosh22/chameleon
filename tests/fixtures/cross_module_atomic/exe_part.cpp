// The exe's translation unit that includes scale.h (scenario cross_module_atomic).
#include "exe_part.h"
#include "shared.h"

int exe_value()
{
    return shared_scale() * 10 + kScale;
}
