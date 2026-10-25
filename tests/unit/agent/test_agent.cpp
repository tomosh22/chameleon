#include "Chameleon_API.h"

#include "doctest.h"

TEST_CASE("cml_agent exports its ABI version") {
    CHECK(cml_agent_abi_version() == CML_AGENT_ABI_VERSION);
}
