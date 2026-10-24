#include "cml/util/hash.h"

#include <cstdint>

#include "doctest.h"

TEST_CASE("hash64 matches the XXH3-64 reference value for empty input") {
    CHECK(cml::hash64(nullptr, 0) == UINT64_C(0x2D06800538D394C2));
    CHECK(cml::hash64(std::string_view{}) == UINT64_C(0x2D06800538D394C2));
}

TEST_CASE("hash64 is deterministic and input sensitive") {
    CHECK(cml::hash64("chameleon") == cml::hash64("chameleon"));
    CHECK(cml::hash64("chameleon") != cml::hash64("chameleoN"));
    CHECK(cml::hash64("a") != cml::hash64("b"));
}
