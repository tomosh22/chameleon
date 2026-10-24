#include "cml/proc/proc.h"

#include <string>

#include "doctest.h"

TEST_CASE("cml_proc links and reports its name") {
    CHECK(std::string(cml::proc::library_name()) == "cml_proc");
}
