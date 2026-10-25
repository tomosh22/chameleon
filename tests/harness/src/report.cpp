#include "report.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "cml/ipc/control_ids.h"

namespace cmltest {

const char* to_string(StepStatus status) {
    switch (status) {
        case StepStatus::Passed: return "PASS";
        case StepStatus::Failed: return "FAIL";
        case StepStatus::SkippedCapability: return "SKIP";
        case StepStatus::NotRun: return "NOT RUN";
    }
    return "?";
}

const char* to_string(Outcome outcome) {
    switch (outcome) {
        case Outcome::Passed: return "passed";
        case Outcome::Partial: return "partial";
        case Outcome::Failed: return "failed";
    }
    return "?";
}

std::size_t ScenarioReport::count(StepStatus status) const {
    std::size_t n = 0;
    for (const StepResult& s : steps) {
        if (s.status == status) {
            ++n;
        }
    }
    return n;
}

Outcome ScenarioReport::outcome(bool require_all_capabilities) const {
    if (count(StepStatus::Failed) > 0 || count(StepStatus::NotRun) > 0) {
        return Outcome::Failed;
    }
    if (count(StepStatus::SkippedCapability) > 0) {
        return require_all_capabilities ? Outcome::Failed : Outcome::Partial;
    }
    return Outcome::Passed;
}

namespace {

std::string seconds_text(double seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", seconds);
    return buf;
}

std::string step_lines(const ScenarioReport& report) {
    std::string out;
    int index = 0;
    for (const StepResult& s : report.steps) {
        ++index;
        out += "[" + std::string(to_string(s.status)) + "] " + std::to_string(index) + ". " + s.name;
        if (s.status == StepStatus::Passed || s.status == StepStatus::Failed) {
            out += " (" + seconds_text(s.seconds) + " s)";
        }
        if (!s.detail.empty()) {
            out += "\n       " + s.detail;
        }
        out += "\n";
    }
    return out;
}

}  // namespace

std::string result_line(const ScenarioReport& report, bool require_all_capabilities) {
    return "CMLTEST_RESULT scenario=" + report.scenario + " generator=" + report.generator + " config=" + report.config +
           " compiler=" + report.compiler + " outcome=" + to_string(report.outcome(require_all_capabilities)) +
           " passed=" + std::to_string(report.count(StepStatus::Passed)) +
           " failed=" + std::to_string(report.count(StepStatus::Failed)) +
           " skipped=" + std::to_string(report.count(StepStatus::SkippedCapability)) +
           " not_run=" + std::to_string(report.count(StepStatus::NotRun));
}

std::string console_report(const ScenarioReport& report, bool require_all_capabilities) {
    std::string out = "== scenario " + report.scenario + " (" + report.generator + ", " + report.config + ", " + report.compiler +
                      ") ==\n";
    out += step_lines(report);
    const std::size_t skipped = report.count(StepStatus::SkippedCapability);
    const Outcome outcome = report.outcome(require_all_capabilities);
    out += "scenario " + report.scenario + ": " + to_string(outcome) + " (" + std::to_string(report.count(StepStatus::Passed)) +
           " passed, " + std::to_string(report.count(StepStatus::Failed)) + " failed, " + std::to_string(skipped) +
           " skipped, " + std::to_string(report.count(StepStatus::NotRun)) + " not run) in " + seconds_text(report.seconds) +
           " s\n";
    if (skipped > 0) {
        out += std::string(require_all_capabilities ? "FAILED (--require-all-capabilities): " : "PARTIAL: ") +
               std::to_string(skipped) + " steps skipped for missing broker capabilities\n";
    }
    out += result_line(report, require_all_capabilities) + "\n";
    return out;
}

std::string xml_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            case '\t':
            case '\n':
            case '\r':
                out.push_back(static_cast<char>(c));
                break;
            default:
                if (c >= 0x20) {
                    out.push_back(static_cast<char>(c));  // bytes >= 0x80 pass through (UTF-8)
                }
                // other control characters are not allowed in XML 1.0: dropped
        }
    }
    return out;
}

std::string junit_xml(const ScenarioReport& report, bool require_all_capabilities) {
    const Outcome outcome = report.outcome(require_all_capabilities);
    const std::size_t skipped_steps = report.count(StepStatus::SkippedCapability);
    const int failures = outcome == Outcome::Failed ? 1 : 0;
    const int skipped = outcome == Outcome::Partial ? 1 : 0;
    const std::string classname = "cmltest." + report.generator + "." + report.config + "." + report.compiler;
    const std::string elapsed = seconds_text(report.seconds);

    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += "<testsuites name=\"cmltest\" tests=\"1\" failures=\"" + std::to_string(failures) + "\" errors=\"0\" skipped=\"" +
           std::to_string(skipped) + "\" time=\"" + elapsed + "\">\n";
    xml += "  <testsuite name=\"cmltest\" tests=\"1\" failures=\"" + std::to_string(failures) + "\" errors=\"0\" skipped=\"" +
           std::to_string(skipped) + "\" time=\"" + elapsed + "\">\n";
    xml += "    <properties>\n";
    xml += "      <property name=\"generator\" value=\"" + xml_escape(report.generator) + "\"/>\n";
    xml += "      <property name=\"config\" value=\"" + xml_escape(report.config) + "\"/>\n";
    xml += "      <property name=\"compiler\" value=\"" + xml_escape(report.compiler) + "\"/>\n";
    xml += "      <property name=\"steps_passed\" value=\"" + std::to_string(report.count(StepStatus::Passed)) + "\"/>\n";
    xml += "      <property name=\"steps_failed\" value=\"" + std::to_string(report.count(StepStatus::Failed)) + "\"/>\n";
    xml += "      <property name=\"steps_skipped\" value=\"" + std::to_string(skipped_steps) + "\"/>\n";
    xml += "      <property name=\"steps_not_run\" value=\"" + std::to_string(report.count(StepStatus::NotRun)) + "\"/>\n";
    xml += "    </properties>\n";
    xml += "    <testcase classname=\"" + xml_escape(classname) + "\" name=\"" + xml_escape(report.scenario) + "\" time=\"" + elapsed +
           "\">\n";

    // The reasons, from the first step that has one.
    std::string first_failure;
    std::string first_failure_id;
    std::string skip_reasons;
    for (const StepResult& s : report.steps) {
        if (s.status == StepStatus::Failed && first_failure.empty()) {
            first_failure = s.name + ": " + s.detail;
            first_failure_id = s.diag_id;
        }
        if (s.status == StepStatus::SkippedCapability) {
            skip_reasons += (skip_reasons.empty() ? "" : "; ") + s.name + " (" + s.detail + ")";
        }
    }
    if (outcome == Outcome::Failed) {
        std::string message = first_failure;
        if (message.empty() && skipped_steps > 0) {
            message = "--require-all-capabilities: " + std::to_string(skipped_steps) + " steps skipped for missing broker capabilities: " +
                      skip_reasons;
        }
        if (message.empty()) {
            message = "scenario failed";
        }
        xml += "      <failure message=\"" + xml_escape(message) + "\" type=\"" + xml_escape(first_failure_id.empty() ? "CMLTEST" : first_failure_id) +
               "\">" + xml_escape(message) + "</failure>\n";
    } else if (outcome == Outcome::Partial) {
        xml += "      <skipped message=\"" +
               xml_escape("PARTIAL: " + std::to_string(skipped_steps) + " steps skipped for missing broker capabilities: " + skip_reasons) +
               "\"/>\n";
    }
    xml += "      <system-out>" + xml_escape(step_lines(report)) + "</system-out>\n";
    xml += "    </testcase>\n";
    xml += "  </testsuite>\n";
    xml += "</testsuites>\n";
    return xml;
}

cml::Result<void> write_text_file(const std::string& path, const std::string& content) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
    }
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    if (!out) {
        return cml::Diag::make(cml::ids::kJunitWriteFailed, "cannot write the report file '" + path + "'",
                               "Check that the directory exists and is writable.");
    }
    return cml::Result<void>();
}

}  // namespace cmltest
