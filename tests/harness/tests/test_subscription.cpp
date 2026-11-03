// WP-22: cmltest subscribes to `patch_reverted` only when the broker has the `revert` capability.
#include <algorithm>

#include "doctest.h"
#include "subscription.h"

using namespace cmltest;

TEST_CASE("subscription: patch_reverted only with the revert capability") {
    const auto has = [](const std::vector<std::string>& v, const char* name) { return std::find(v.begin(), v.end(), name) != v.end(); };
    const std::vector<std::string> older = subscription_events({"status", "reload", "wait", "set-option", "subscribe", "diag-events"});
    CHECK(has(older, "diag"));
    CHECK(has(older, "patch_applied"));
    CHECK(has(older, "log"));
    CHECK_FALSE(has(older, "patch_reverted"));  // a broker without it would refuse the whole subscribe
    const std::vector<std::string> newer = subscription_events({"status", "reload", "subscribe", "revert"});
    CHECK(has(newer, "patch_reverted"));
    CHECK(has(newer, "patch_applied"));
    CHECK_FALSE(has(subscription_events({}), "patch_reverted"));
}
