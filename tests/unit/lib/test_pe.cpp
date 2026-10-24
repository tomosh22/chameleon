#include "cml/pe/pe.h"

#include <string>

#include "doctest.h"

TEST_CASE("cml_pe links and reports its name") {
    CHECK(std::string(cml::pe::library_name()) == "cml_pe");
}
