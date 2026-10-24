#include "cml/util/result.h"

#include <memory>
#include <string>

#include "doctest.h"

namespace {

cml::Result<int> parse_positive(int v) {
    if (v <= 0) {
        return cml::Diag::make(1201, "value is not positive", "pass a positive number.");
    }
    return v;
}

cml::Result<void> check(bool ok) {
    if (!ok) {
        return cml::Diag::make(5101, "check failed", "retry.");
    }
    return {};
}

}  // namespace

TEST_CASE("Result holds a value") {
    const auto r = parse_positive(7);
    REQUIRE(r.has_value());
    CHECK(static_cast<bool>(r));
    CHECK(r.value() == 7);
    CHECK(*r == 7);
    CHECK(r.value_or(-1) == 7);
}

TEST_CASE("Result holds a Diag") {
    const auto r = parse_positive(0);
    REQUIRE_FALSE(r.has_value());
    CHECK_FALSE(static_cast<bool>(r));
    CHECK(r.error().id() == "CML1201");
    CHECK(r.value_or(-1) == -1);
}

TEST_CASE("Result supports move-only payloads") {
    cml::Result<std::unique_ptr<int>> r = std::make_unique<int>(42);
    REQUIRE(r.has_value());
    std::unique_ptr<int> p = std::move(r).value();
    REQUIRE(p != nullptr);
    CHECK(*p == 42);
}

TEST_CASE("Result arrow operator and strings") {
    cml::Result<std::string> r = std::string("hello");
    REQUIRE(r.has_value());
    CHECK(r->size() == 5u);
}

TEST_CASE("Result<void>") {
    CHECK(check(true).has_value());
    const auto bad = check(false);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == 5101u);
}

TEST_CASE("Result propagates errors between layers") {
    auto outer = [](int v) -> cml::Result<int> {
        auto inner = parse_positive(v);
        if (!inner) {
            return inner.error();
        }
        return *inner * 2;
    };
    CHECK(outer(4).value() == 8);
    CHECK(outer(-4).error().code == 1201u);
}
