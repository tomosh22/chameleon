#include "cml/util/format.h"

#include "doctest.h"

TEST_CASE("hex: lower-case, 0x prefix, no padding (the one copy of what six files used to define)") {
    CHECK(cml::hex(0) == "0x0");
    CHECK(cml::hex(0xF) == "0xf");
    CHECK(cml::hex(0x10) == "0x10");
    CHECK(cml::hex(0x1f40) == "0x1f40");
    CHECK(cml::hex(0x140000000ull) == "0x140000000");
    CHECK(cml::hex(0xFFFFFFFFFFFFFFFFull) == "0xffffffffffffffff");
    CHECK(cml::hex(0x8000000000000000ull) == "0x8000000000000000");
}
