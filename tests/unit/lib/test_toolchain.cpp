#include "cml/toolchain/toolchain.h"

#include <string>

#include "doctest.h"

TEST_CASE("cml_toolchain links and reports its name") {
    CHECK(std::string(cml::toolchain::library_name()) == "cml_toolchain");
}
