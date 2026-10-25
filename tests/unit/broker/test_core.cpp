#include "cml/core/version.h"
#include "cml/core/version_config.h"

#include <string>

#include "doctest.h"

TEST_CASE("version string is major.minor.patch from CMake") {
    const std::string expected = std::to_string(CML_VERSION_MAJOR) + "." +
                                 std::to_string(CML_VERSION_MINOR) + "." +
                                 std::to_string(CML_VERSION_PATCH);
    CHECK(std::string(cml::core::version_string()) == expected);
}
