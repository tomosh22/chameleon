#include "cml/license/license.h"

#include <atomic>

#include "cml/license/license_diag_ids.h"

namespace cml::license {
namespace {

class NoopProvider final : public Provider {
public:
    Status check(const Context&) const override {
        Status status;
        status.verdict = Verdict::Allowed;
        status.detail = "no-op licence provider: all features enabled";
        return status;
    }
};

const NoopProvider g_noop;
std::atomic<const Provider*> g_provider{nullptr};

}  // namespace

const Provider& noop_provider() noexcept { return g_noop; }

void set_provider(const Provider* provider) noexcept { g_provider.store(provider); }

Status check(const Context& context) {
    const Provider* provider = g_provider.load();
    Status status = (provider != nullptr ? *provider : static_cast<const Provider&>(g_noop)).check(context);
    if (!status.allowed() && !status.diag.valid()) {
        if (status.verdict == Verdict::Refused) {
            status.diag = Diag::make(diag::kLicenseRefused,
                                     "the licence check refused this installation" + (status.detail.empty() ? "" : ": " + status.detail),
                                     "Install a valid licence for this version of Chameleon, or contact the vendor.");
        } else {
            status.diag = Diag::make(diag::kLicenseCheckFailed,
                                     "the licence check could not be completed" + (status.detail.empty() ? "" : ": " + status.detail),
                                     "Check that the licence file or service is reachable and run the broker again.");
        }
    }
    return status;
}

std::string describe(const Status& status) {
    if (status.allowed()) {
        return "licence check: allowed (" + status.detail + ")";
    }
    return status.diag.format();
}

}  // namespace cml::license
