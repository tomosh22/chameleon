#include "cml/license/license.h"
#include "cml/license/license_diag_ids.h"

#include <string>
#include <utility>

#include "doctest.h"

namespace {

class FixedProvider final : public cml::license::Provider {
public:
    FixedProvider(cml::license::Verdict verdict, std::string detail) : verdict_(verdict), detail_(std::move(detail)) {}
    cml::license::Status check(const cml::license::Context& context) const override {
        seen_version = context.product_version;
        cml::license::Status status;
        status.verdict = verdict_;
        status.detail = detail_;
        return status;
    }
    mutable std::string seen_version;

private:
    cml::license::Verdict verdict_;
    std::string detail_;
};

// Restores the default provider when a test case ends, however it ends.
struct ProviderGuard {
    ~ProviderGuard() { cml::license::set_provider(nullptr); }
};

}  // namespace

TEST_CASE("the default provider allows") {
    cml::license::set_provider(nullptr);
    cml::license::Context context;
    context.product_version = "0.1.0";
    const cml::license::Status status = cml::license::check(context);
    CHECK(status.allowed());
    CHECK(status.verdict == cml::license::Verdict::Allowed);
    CHECK_FALSE(status.diag.valid());
    CHECK(cml::license::describe(status) == "licence check: allowed (no-op licence provider: all features enabled)");
}

TEST_CASE("the no-op provider allows whatever the context says") {
    const cml::license::Context empty;
    CHECK(cml::license::noop_provider().check(empty).allowed());
}

TEST_CASE("a refusing provider gets CML5998 filled in") {
    ProviderGuard guard;
    FixedProvider provider(cml::license::Verdict::Refused, "licence expired");
    cml::license::set_provider(&provider);
    cml::license::Context context;
    context.product_version = "9.9.9";
    const cml::license::Status status = cml::license::check(context);
    CHECK_FALSE(status.allowed());
    CHECK(provider.seen_version == "9.9.9");
    CHECK(status.diag.id() == "CML5998");
    CHECK(status.diag.message.find("licence expired") != std::string::npos);
    CHECK_FALSE(status.diag.what_to_do.empty());
    CHECK(cml::license::describe(status) == status.diag.format());
}

TEST_CASE("a failing provider gets CML5999 filled in") {
    ProviderGuard guard;
    FixedProvider provider(cml::license::Verdict::Failed, "");
    cml::license::set_provider(&provider);
    const cml::license::Status status = cml::license::check(cml::license::Context{});
    CHECK_FALSE(status.allowed());
    CHECK(status.verdict == cml::license::Verdict::Failed);
    CHECK(status.diag.id() == "CML5999");
    CHECK(status.diag.valid());
}

TEST_CASE("set_provider(nullptr) restores the no-op provider") {
    FixedProvider provider(cml::license::Verdict::Refused, "no");
    cml::license::set_provider(&provider);
    CHECK_FALSE(cml::license::check(cml::license::Context{}).allowed());
    cml::license::set_provider(nullptr);
    CHECK(cml::license::check(cml::license::Context{}).allowed());
}

TEST_CASE("the licence ids stay inside the range ADR-0038 reserves") {
    CHECK(cml::license::diag::kLicenseRefused == 5998);
    CHECK(cml::license::diag::kLicenseCheckFailed == 5999);
}
