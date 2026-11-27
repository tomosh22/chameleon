#pragma once

// Logging for code running inside cml_agent.dll.
//
// The agent has its own copy of cml_util (static link), so this sink affects only the agent. Every
// message goes to
//   * OutputDebugString, when a debugger is attached;
//   * the file named by CML_AGENT_LOG, if set (appended, flushed per line);
//   * the broker, for warn and error messages: they are queued here and sent as `Log` messages by
//     the agent thread once connected (bounded queue, oldest dropped). The agent thread sends them
//     before the answers it has queued (WP-50), so a line logged before an answer reaches the broker first.
//     forward_to_broker() queues one line of any level (WP-50's timing evidence).
// CML_LOG_LEVEL (trace|debug|info|warn|error) sets the level; the default is info.

#if defined(_WIN32)

#include <cstddef>
#include <string>
#include <vector>

#include "cml/ipc/messages.h"
#include "cml/util/log.h"

namespace cml::agent {

// Idempotent. Call before the first CML_LOG in an agent.
void install_logging();

// Reference-counted: forwarded messages are only queued while at least one agent wants them.
void set_log_forwarding(bool enabled);

// WP-50: queues `text` for the broker whatever its level (the sink forwards warn and error only). Nothing happens while no
// agent forwards (set_log_forwarding). Does not write the agent's own log: log the line with CML_LOG as well.
void forward_to_broker(LogLevel level, const std::string& text);

// Moves up to `max` queued messages out of the forward queue (oldest first).
std::vector<ipc::Log> take_forwarded_logs(std::size_t max);

}  // namespace cml::agent

#endif  // _WIN32
