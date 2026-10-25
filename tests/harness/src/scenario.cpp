#include "scenario.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "cml/ipc/control.h"
#include "rx.h"
#include "toml.h"

namespace cmltest {

const char* to_string(StepKind kind) {
    switch (kind) {
        case StepKind::Launch: return "launch";
        case StepKind::ExpectStdout: return "expect_stdout";
        case StepKind::ExpectModuleEnabled: return "expect_module_enabled";
        case StepKind::Edit: return "edit";
        case StepKind::Reload: return "reload";
        case StepKind::ExpectDiag: return "expect_diag";
    }
    return "?";
}

std::string Step::describe() const {
    std::string out = to_string(kind);
    switch (kind) {
        case StepKind::Launch:
            break;
        case StepKind::ExpectStdout:
            out += " /" + regex + "/ within " + std::to_string(within_s) + " s";
            break;
        case StepKind::ExpectModuleEnabled:
            out += " " + module + " within " + std::to_string(within_s) + " s";
            break;
        case StepKind::Edit:
            out += " " + patch;
            break;
        case StepKind::Reload:
            out += " within " + std::to_string(within_s) + " s";
            break;
        case StepKind::ExpectDiag:
            out += " " + id + " within " + std::to_string(within_s) + " s";
            break;
    }
    return out;
}

const char* Step::required_capability() const {
    switch (kind) {
        case StepKind::Reload: return cml::ipc::capability::kReload;
        case StepKind::ExpectDiag: return cml::ipc::capability::kDiagEvents;
        default: return nullptr;
    }
}

namespace {

cml::Diag invalid(int line, const std::string& what) {
    return cml::Diag::make(cml::ids::kScenarioInvalid,
                           "scenario line " + std::to_string(line) + ": " + what,
                           "Fix the scenario file (tests/harness/README.md lists the keys of every step kind).");
}

bool name_chars_ok(const std::string& s, bool allow_dot) {
    return !s.empty() && s.size() <= 64 &&
           std::all_of(s.begin(), s.end(), [&](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                      (allow_dot && c == '.');
           });
}

cml::Result<std::string> require_string(const toml::Table& table, const char* key, const char* context) {
    const toml::Value* v = table.find(key);
    if (v == nullptr) {
        return invalid(table.line, std::string(context) + " needs the key '" + key + "'");
    }
    if (v->kind != toml::Value::Kind::String) {
        return invalid(v->line, std::string("'") + key + "' must be a string");
    }
    return v->text;
}

cml::Result<int> int_in_range(const toml::Table& table, const char* key, int fallback, int lo, int hi) {
    const toml::Value* v = table.find(key);
    if (v == nullptr) {
        return fallback;
    }
    if (v->kind != toml::Value::Kind::Integer || v->integer < lo || v->integer > hi) {
        return invalid(v->line, std::string("'") + key + "' must be an integer between " + std::to_string(lo) + " and " +
                                    std::to_string(hi));
    }
    return static_cast<int>(v->integer);
}

cml::Result<void> only_keys(const toml::Table& table, std::initializer_list<const char*> allowed, const char* context) {
    for (const auto& entry : table.entries) {
        const bool known = std::any_of(allowed.begin(), allowed.end(), [&](const char* k) { return entry.first == k; });
        if (!known) {
            return invalid(entry.second.line, "unknown key '" + entry.first + "' in " + context);
        }
    }
    return cml::Result<void>();
}

cml::Result<Step> parse_step(const toml::Table& table) {
    Step step;
    step.line = table.line;
    cml::Result<std::string> kind = require_string(table, "kind", "[[step]]");
    if (!kind) {
        return kind.error();
    }
    const char* context = "this step";
    if (*kind == "launch") {
        step.kind = StepKind::Launch;
        cml::Result<void> keys = only_keys(table, {"kind"}, "a launch step");
        if (!keys) return keys.error();
        return step;
    }
    if (*kind == "expect_stdout") {
        step.kind = StepKind::ExpectStdout;
        cml::Result<void> keys = only_keys(table, {"kind", "regex", "within_s"}, "an expect_stdout step");
        if (!keys) return keys.error();
        cml::Result<std::string> regex = require_string(table, "regex", "an expect_stdout step");
        if (!regex) return regex.error();
        cml::Result<Regex> compiled = Regex::compile(*regex);
        if (!compiled) {
            return compiled.error();
        }
        step.regex = *regex;
        cml::Result<int> within = int_in_range(table, "within_s", 10, 1, 3600);
        if (!within) return within.error();
        step.within_s = *within;
        return step;
    }
    if (*kind == "expect_module_enabled") {
        step.kind = StepKind::ExpectModuleEnabled;
        cml::Result<void> keys = only_keys(table, {"kind", "module", "within_s"}, "an expect_module_enabled step");
        if (!keys) return keys.error();
        cml::Result<std::string> module = require_string(table, "module", "an expect_module_enabled step");
        if (!module) return module.error();
        if (module->empty()) return invalid(table.line, "'module' must not be empty");
        step.module = *module;
        cml::Result<int> within = int_in_range(table, "within_s", 10, 1, 3600);
        if (!within) return within.error();
        step.within_s = *within;
        return step;
    }
    if (*kind == "edit") {
        step.kind = StepKind::Edit;
        cml::Result<void> keys = only_keys(table, {"kind", "patch"}, "an edit step");
        if (!keys) return keys.error();
        cml::Result<std::string> patch = require_string(table, "patch", "an edit step");
        if (!patch) return patch.error();
        const std::filesystem::path p(*patch);
        if (patch->empty() || p.is_absolute() || p.has_root_name() || patch->find("..") != std::string::npos ||
            patch->find(':') != std::string::npos || (*patch)[0] == '/' || (*patch)[0] == '\\') {
            return invalid(table.line, "'patch' must be a relative path below the fixture directory");
        }
        step.patch = *patch;
        return step;
    }
    if (*kind == "reload") {
        step.kind = StepKind::Reload;
        cml::Result<void> keys = only_keys(table, {"kind", "within_s"}, "a reload step");
        if (!keys) return keys.error();
        cml::Result<int> within = int_in_range(table, "within_s", 60, 1, 3600);
        if (!within) return within.error();
        step.within_s = *within;
        return step;
    }
    if (*kind == "expect_diag") {
        step.kind = StepKind::ExpectDiag;
        cml::Result<void> keys = only_keys(table, {"kind", "id", "within_s"}, "an expect_diag step");
        if (!keys) return keys.error();
        cml::Result<std::string> id = require_string(table, "id", "an expect_diag step");
        if (!id) return id.error();
        const bool shape = id->size() == 7 && id->compare(0, 3, "CML") == 0 &&
                           std::all_of(id->begin() + 3, id->end(), [](char c) { return c >= '0' && c <= '9'; });
        if (!shape) return invalid(table.line, "'id' must look like CML3101");
        step.id = *id;
        cml::Result<int> within = int_in_range(table, "within_s", 10, 1, 3600);
        if (!within) return within.error();
        step.within_s = *within;
        return step;
    }
    (void)context;
    return invalid(table.line, "unknown step kind '" + *kind +
                                   "' (launch, expect_stdout, expect_module_enabled, edit, reload, expect_diag)");
}

}  // namespace

cml::Result<Scenario> parse_scenario(std::string_view text) {
    cml::Result<toml::Document> doc = toml::parse(text);
    if (!doc) {
        return doc.error();
    }
    Scenario scenario;
    cml::Result<void> root_keys = only_keys(doc->root, {"name", "description", "fixture", "timeout_s"}, "the scenario header");
    if (!root_keys) return root_keys.error();
    if (!doc->tables.empty()) {
        return invalid(doc->tables[0].line, "plain tables are not used in scenarios (only [[step]])");
    }
    for (const toml::Table& t : doc->array_tables) {
        if (t.name != "step") {
            return invalid(t.line, "unknown array of tables [[" + t.name + "]] (only [[step]] exists)");
        }
    }
    cml::Result<std::string> name = require_string(doc->root, "name", "the scenario");
    if (!name) return name.error();
    if (!name_chars_ok(*name, true)) return invalid(doc->root.find("name")->line, "'name' must be 1-64 characters of a-z, A-Z, 0-9, '_', '-' or '.'");
    scenario.name = *name;
    cml::Result<std::string> fixture = require_string(doc->root, "fixture", "the scenario");
    if (!fixture) return fixture.error();
    if (!name_chars_ok(*fixture, false)) return invalid(doc->root.find("fixture")->line, "'fixture' must be a fixture directory name (tests/fixtures/<name>)");
    scenario.fixture = *fixture;
    if (const toml::Value* d = doc->root.find("description")) {
        if (d->kind != toml::Value::Kind::String) return invalid(d->line, "'description' must be a string");
        scenario.description = d->text;
    }
    cml::Result<int> timeout = int_in_range(doc->root, "timeout_s", 120, 10, 7200);
    if (!timeout) return timeout.error();
    scenario.timeout_s = *timeout;

    bool launched = false;
    for (const toml::Table& t : doc->array_tables) {
        cml::Result<Step> step = parse_step(t);
        if (!step) {
            return step.error();
        }
        if (step->kind == StepKind::Launch) {
            if (launched) return invalid(t.line, "a scenario can launch only once");
            launched = true;
        } else if (step->kind != StepKind::Edit && !launched) {
            return invalid(t.line, std::string(to_string(step->kind)) + " needs a launch step before it");
        }
        scenario.steps.push_back(std::move(*step));
    }
    if (scenario.steps.empty()) {
        return invalid(1, "the scenario has no [[step]]");
    }
    return scenario;
}

cml::Result<Scenario> load_scenario(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return cml::Diag::make(cml::ids::kHarnessUsage, "cannot read the scenario file '" + path.string() + "'",
                               "Pass the path of an existing scenario.toml with --scenario.");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    cml::Result<Scenario> scenario = parse_scenario(buffer.str());
    if (!scenario) {
        return cml::Diag::make(scenario.error().code, path.string() + ": " + scenario.error().message,
                               scenario.error().what_to_do);
    }
    scenario->path = path;
    return scenario;
}

}  // namespace cmltest
