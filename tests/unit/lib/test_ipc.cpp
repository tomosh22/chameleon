#include "cml/ipc/ipc.h"

#include <string>

#include "doctest.h"

TEST_CASE("cml_ipc links and reports its name") {
    CHECK(std::string(cml::ipc::library_name()) == "cml_ipc");
}
