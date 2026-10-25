#include "cml/coff/coff.h"

#include <string>

#include "doctest.h"

TEST_CASE("cml_coff links and reports its name") {
    CHECK(std::string(cml::coff::library_name()) == "cml_coff");
}
