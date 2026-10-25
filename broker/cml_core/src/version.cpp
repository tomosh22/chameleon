#include "cml/core/version.h"

#include "cml/core/version_config.h"

namespace cml::core {

const char* version_string() noexcept {
    return CML_VERSION_STRING;
}

}  // namespace cml::core
