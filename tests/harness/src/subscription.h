#pragma once

// The events cmltest subscribes to at connect (portable, unit-tested). An event name the broker does not know is a refused
// `subscribe`, so a name is only listed when the broker advertises the capability that produces it (WP-22: `patch_reverted`
// needs capability "revert"; without it every scenario would fail at connect against a broker that lacks it).

#include <string>
#include <vector>

namespace cmltest {

std::vector<std::string> subscription_events(const std::vector<std::string>& capabilities);

}  // namespace cmltest
