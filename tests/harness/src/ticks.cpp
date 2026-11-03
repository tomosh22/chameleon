#include "ticks.h"

#include <cctype>

#include "cml/ipc/control_ids.h"

namespace cmltest {

std::vector<long long> tick_values(std::string_view output, std::string_view key) {
    std::vector<long long> values;
    std::size_t pos = 0;
    while (pos < output.size()) {
        std::size_t end = output.find('\n', pos);
        if (end == std::string_view::npos) {
            end = output.size();
        }
        std::string_view line = output.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.substr(0, 5) != "TICK ") {
            continue;
        }
        std::size_t at = 4;
        while (at < line.size()) {
            // A token starts after a space.
            if (line[at] != ' ') {
                ++at;
                continue;
            }
            ++at;
            if (line.compare(at, key.size(), key) == 0 && at + key.size() < line.size() && line[at + key.size()] == '=') {
                std::size_t v = at + key.size() + 1;
                bool negative = false;
                if (v < line.size() && line[v] == '-') {
                    negative = true;
                    ++v;
                }
                long long value = 0;
                std::size_t digits = 0;
                while (v < line.size() && std::isdigit(static_cast<unsigned char>(line[v])) != 0 && digits < 18) {
                    value = value * 10 + (line[v] - '0');
                    ++v;
                    ++digits;
                }
                if (digits > 0 && (v >= line.size() || line[v] == ' ')) {
                    values.push_back(negative ? -value : value);
                }
                break;
            }
        }
    }
    return values;
}

cml::Result<std::string> check_monotonic(std::string_view output, std::string_view key, std::size_t min_samples) {
    const std::vector<long long> values = tick_values(output, key);
    const std::string name(key);
    if (values.size() < min_samples) {
        return cml::Diag::make(cml::ids::kCounterNotMonotonic,
                               "only " + std::to_string(values.size()) + " TICK samples of '" + name + "' (at least " + std::to_string(min_samples) + " needed)",
                               "Run the fixture longer before this step, or check that it prints the key.");
    }
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (values[i] < values[i - 1]) {
            return cml::Diag::make(cml::ids::kCounterNotMonotonic,
                                   "'" + name + "' went from " + std::to_string(values[i - 1]) + " to " + std::to_string(values[i]) + " at sample " + std::to_string(i) +
                                       ": the patched code is using its own copy of the state, not the old storage",
                                   "The bind planner must bind this variable to the old image (docs/wp/WP-07.md).");
        }
    }
    if (values.front() == values.back()) {
        return cml::Diag::make(cml::ids::kCounterNotMonotonic, "'" + name + "' never changed over " + std::to_string(values.size()) + " samples",
                               "The fixture should count; check that it keeps ticking.");
    }
    return name + ": " + std::to_string(values.size()) + " samples, " + std::to_string(values.front()) + " -> " + std::to_string(values.back());
}

}  // namespace cmltest
