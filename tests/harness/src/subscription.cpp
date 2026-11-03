#include "subscription.h"

#include <algorithm>

namespace cmltest {

std::vector<std::string> subscription_events(const std::vector<std::string>& capabilities) {
    std::vector<std::string> events = {"diag", "patch_applied", "log"};
    if (std::find(capabilities.begin(), capabilities.end(), "revert") != capabilities.end()) {
        events.push_back("patch_reverted");
    }
    return events;
}

}  // namespace cmltest
