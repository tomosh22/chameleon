// Fuzz target: the control-pipe JSON parser and message codec (WP-28). cml_ipc/json is hand-written, as are the request /
// response / event decoders on top of it. Input = one line as a peer would send it (or any bytes).
//
// A parsed value is serialised and parsed again: the second parse must succeed and serialise to the same text.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <variant>

#include "cml/ipc/control.h"
#include "cml/ipc/json.h"
#include "fuzz_common.h"

namespace {

[[noreturn]] void bug(const char* what) {
    std::fprintf(stderr, "fuzz_control: %s\n", what);
    std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace cml;
    const std::string_view line(reinterpret_cast<const char*>(data), size);

    if (Result<ipc::json::Value> v = ipc::json::parse(line)) {
        const std::string text = ipc::json::serialize(*v);
        Result<ipc::json::Value> again = ipc::json::parse(text);
        if (!again) {
            bug("serialised JSON does not parse");
        }
        if (ipc::json::serialize(*again) != text) {
            bug("JSON serialise/parse/serialise is not stable");
        }
        fuzz::use(text.size());
    } else {
        fuzz::use(v.error().code);
    }

    if (Result<ipc::ControlRequest> r = ipc::decode_request(line)) {
        // A decoded request must encode again (unless it exceeds the line limit).
        if (Result<std::string> e = ipc::encode_request(*r)) {
            if (!ipc::decode_request(*e)) {
                bug("an encoded request does not decode");
            }
        }
    } else {
        fuzz::use(ipc::peek_control_request_id(line).value_or(0));
    }

    if (Result<std::variant<ipc::ControlResponse, ipc::ControlEvent>> m = ipc::decode_server_message(line)) {
        if (const auto* resp = std::get_if<ipc::ControlResponse>(&*m)) {
            if (Result<std::string> e = ipc::encode_response(*resp)) {
                fuzz::use(e->size());
            }
            if (resp->diag) {
                fuzz::use(ipc::to_diag(*resp->diag).code);
            }
        } else if (const auto* ev = std::get_if<ipc::ControlEvent>(&*m)) {
            if (Result<std::string> e = ipc::encode_event(*ev)) {
                fuzz::use(e->size());
            }
        }
    }

    // `status` result shapes (what cmltest and the IDE extensions read).
    if (Result<ipc::json::Value> v = ipc::json::parse(line)) {
        if (Result<ipc::StatusReport> s = ipc::status_from_json(*v)) {
            fuzz::use(ipc::status_to_json(*s).size());
        }
    }
    return 0;
}
