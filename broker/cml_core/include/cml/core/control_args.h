#pragma once

// Argument readers shared by the control commands of the broker (reload, wait, restart ...).

#include <cstdint>
#include <optional>
#include <string>

#include "cml/ipc/json.h"
#include "cml/util/diag.h"

namespace cml::core {

// Reads an optional unsigned integer argument; false (and *error set from make_error(message)) when it is present with a wrong
// type or outside 0..max.
template <class MakeError>
bool read_uint_arg(const ipc::json::Value& args, const char* key, std::uint32_t max, std::uint32_t* out, std::optional<Diag>* error,
                   MakeError make_error) {
    const ipc::json::Value* v = args.find(key);
    if (v == nullptr || v->is_null()) {
        return true;
    }
    if (!v->is_integer() || v->as_integer() < 0 || v->as_integer() > static_cast<std::int64_t>(max)) {
        *error = make_error(std::string("argument '") + key + "' must be an integer from 0 to " + std::to_string(max));
        return false;
    }
    *out = static_cast<std::uint32_t>(v->as_integer());
    return true;
}

}  // namespace cml::core
