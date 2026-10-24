#include "cml/watch/watch.h"

#include <string>

#include "doctest.h"

TEST_CASE("cml_watch links and reports its name") {
    CHECK(std::string(cml::watch::library_name()) == "cml_watch");
}
