#include "cml/util/diag.h"

#include "doctest.h"

TEST_CASE("Diag formats a stable id") {
    const cml::Diag d = cml::Diag::make(1101, "cannot open 'a.pdb'", "check the path.");
    CHECK(d.id() == "CML1101");
    CHECK(d.valid());
    CHECK(d.category() == cml::DiagCategory::Parse);
    CHECK(d.format() == "CML1101: cannot open 'a.pdb' What to do: check the path.");
}

TEST_CASE("Diag categories follow the thousands digit") {
    CHECK(cml::Diag::make(2001, "m", "w").category() == cml::DiagCategory::Compile);
    CHECK(cml::Diag::make(3999, "m", "w").category() == cml::DiagCategory::Link);
    CHECK(cml::Diag::make(4500, "m", "w").category() == cml::DiagCategory::Load);
    CHECK(cml::Diag::make(5000, "m", "w").category() == cml::DiagCategory::Runtime);
}

TEST_CASE("Diag with out-of-range code is invalid") {
    const cml::Diag none;
    CHECK_FALSE(none.valid());
    CHECK(none.id() == "CML????");
    CHECK(none.category() == cml::DiagCategory::Unknown);
    CHECK_FALSE(cml::Diag::make(999, "m", "w").valid());
    CHECK_FALSE(cml::Diag::make(6000, "m", "w").valid());
}

TEST_CASE("Diag::id never aliases an out-of-range code onto a valid-looking id") {
    CHECK(cml::Diag::make(11101, "m", "w").id() == "CML????");
    CHECK(cml::Diag::make(6101, "m", "w").id() == "CML????");
    CHECK(cml::Diag::make(101, "m", "w").id() == "CML????");
    CHECK(cml::Diag::make(1000, "m", "w").id() == "CML1000");
    CHECK(cml::Diag::make(5999, "m", "w").id() == "CML5999");
    CHECK(cml::Diag::make(11101, "m", "w").format().rfind("CML????:", 0) == 0);
}

TEST_CASE("Diag without a hint omits the what-to-do clause") {
    CHECK(cml::Diag::make(5001, "boom", "").format() == "CML5001: boom");
}

TEST_CASE("DiagCategory names") {
    CHECK(std::string(cml::to_string(cml::DiagCategory::Parse)) == "parse");
    CHECK(std::string(cml::to_string(cml::DiagCategory::Unknown)) == "unknown");
}
