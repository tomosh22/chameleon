#pragma once

#include <string>

#include "cml/util/diag.h"

// Licensing hook (WP-32). Commercial enforcement is outside the engineering plan: this library defines the seam and a
// no-op implementation, so that a later commercial build can replace the provider without touching the broker.
//
//   cml::license::Context context;
//   context.product_version = cml::core::version_string();
//   const cml::license::Status status = cml::license::check(context);   // once, at broker start-up
//
// The default provider (noop_provider) always allows. A different provider is installed with set_provider() before the
// check; the pointer must stay valid for the rest of the process. No exceptions, no RTTI (core/ conventions).
namespace cml::license {

enum class Verdict {
    Allowed,  // run
    Refused,  // the check completed and the answer is no (diagnostic CML5998)
    Failed,   // the check could not be completed (diagnostic CML5999)
};

// What the provider may base its decision on. Only fields that every caller can fill are here.
struct Context {
    std::string product_version;  // "0.1.0"
    std::string product_name = "Chameleon";
};

struct Status {
    Verdict verdict = Verdict::Allowed;
    std::string holder;   // who the licence is for ("" when not applicable)
    std::string detail;   // one line for the log ("no-op licence provider: all features enabled")
    cml::Diag diag;       // set (valid()) for Refused and Failed

    bool allowed() const noexcept { return verdict == Verdict::Allowed; }
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual Status check(const Context& context) const = 0;
};

// Always allows.
const Provider& noop_provider() noexcept;

// Installs `provider` (nullptr restores the no-op provider). Not thread-safe against a concurrent check(): call it
// before the broker starts.
void set_provider(const Provider* provider) noexcept;

// Asks the installed provider. A provider that reports Refused or Failed without a diagnostic gets the matching one
// (CML5998 / CML5999) filled in, so callers can always log `status.diag`.
Status check(const Context& context);

// "licence check: allowed (<detail>)" or the diagnostic's formatted text, for the broker log.
std::string describe(const Status& status);

}  // namespace cml::license
