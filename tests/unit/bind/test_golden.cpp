// WP-07: golden JSON plans of synthetic scenarios, and the trace log of every decision.
//
// The goldens live in golden/*.json. They pin the complete plan (decisions with reasons, hooks,
// thunks, placeholders, new globals, initialisers, fix-ups). To regenerate after an intended change run
// the test with CML_UPDATE_GOLDEN=1 and review the diff.
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "cml/core/bind_plan.h"
#include "cml/util/log.h"
#include "doctest.h"
#include "synth.h"
#include "worlds.h"

using namespace cml_bind_test;
using namespace cml::core;
using namespace cml::core::bind;

namespace {

std::string golden_path(const std::string& name) { return std::string(CML_BIND_GOLDEN_DIR) + "/" + name + ".json"; }

std::string read_all(const std::string& path, bool* ok) {
    std::ifstream in(path, std::ios::binary);
    *ok = static_cast<bool>(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    // The repository may be checked out with CRLF on Windows.
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c != '\r') {
            out.push_back(c);
        }
    }
    return out;
}

void check_golden(const std::string& name, const std::string& actual) {
    const std::string path = golden_path(name);
#ifdef _WIN32
    char* update_buf = nullptr;
    std::size_t update_len = 0;
    _dupenv_s(&update_buf, &update_len, "CML_UPDATE_GOLDEN");
    const bool update_requested = update_buf != nullptr && update_buf[0] == '1';
    std::free(update_buf);
#else
    const char* update = std::getenv("CML_UPDATE_GOLDEN");
    const bool update_requested = update != nullptr && update[0] == '1';
#endif
    if (update_requested) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << actual;
        return;
    }
    bool ok = false;
    const std::string expected = read_all(path, &ok);
    REQUIRE_MESSAGE(ok, "missing golden file " << path << " (run with CML_UPDATE_GOLDEN=1)");
    CHECK_MESSAGE(expected == actual, "golden mismatch for " << name << "; actual plan:\n" << actual);
}

// The shape of `basic` with its `value_2` edit: compute_value hooked, one data and one function
// external of main.cpp, the stack-cookie helpers of /GS, the EH handler through ADDR32NB.
std::string basic_like() {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x40);
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    const std::uint32_t note = o.undefined("fixture_note", true);
    const std::uint32_t cookie = o.undefined("__security_cookie", false);
    const std::uint32_t chk = o.undefined("__security_check_cookie", true);
    const std::uint32_t eh = o.undefined("__CxxFrameHandler4", true);
    const std::uint32_t rdata = o.section(".rdata", kRdata, 0x20);
    o.symbol("$SG1234", 0, static_cast<std::int32_t>(rdata), false, false);
    const std::uint32_t xdata = o.section(".xdata", kRdata | coff::scn::LnkComdat, 0x10, coff::ComdatSelection::Associative, f.section);
    o.symbol("$unwind$?compute_value@@YAHXZ", 0, static_cast<std::int32_t>(xdata), false, false);
    o.reloc(f.section, 0x04, cookie, kRel32);
    o.reloc(f.section, 0x0B, tick, kRel32_1, 0);
    o.reloc(f.section, 0x12, note, kRel32);
    o.reloc(f.section, 0x1C, chk, kRel32);
    o.reloc(xdata, 0, eh, kAddr32Nb);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    return to_json(*r);
}

// The shape of `statics` with its edit: file-static, function-local static + guard, static member,
// a const table, a static helper; everything bound to the running process's storage.
std::string statics_like() {
    SynthWorld w = basic_world(false);
    w.data(w.tu(), "`anonymous namespace'::s_calls", 0x10);
    w.data(w.tu(), "k_weights", 0x40);
    w.proc_with_statics(w.tu(), "statics_value", 0x50, {{"g", 0x20}});
    w.proc(w.tu(), "make_base", 0x90, 0x10, true);
    w.pub_func("?statics_value@@YAHXZ", 0x50);
    w.open();
    SynthObj o;
    auto fv = o.function("?statics_value@@YAHXZ", true, 0x60);
    auto mb = o.function("?make_base@@YAHXZ", false, 0x10);
    const std::uint32_t bss = o.section(".data", kData, 0x60);
    const std::int32_t sec = static_cast<std::int32_t>(bss);
    const std::uint32_t calls = o.symbol("?s_calls@?A0x5a5a5a5a@@3HA", 0x10, sec, false, false);
    const std::uint32_t g = o.symbol("?g@?1??statics_value@@YAHXZ@4UGuarded@@A", 0x20, sec, false, false);
    const std::uint32_t guard = o.symbol("?$TSS0@?1??statics_value@@YAHXZ@4HA", 0x24, sec, false, false);
    const std::uint32_t weights = o.symbol("?k_weights@@3QBHB", 0x40, sec, false, false);
    const std::uint32_t total = o.undefined("?total@Accum@@2HA", false);
    o.reloc(fv.section, 0x08, mb.symbol, kRel32);
    o.reloc(fv.section, 0x10, guard, kRel32);
    o.reloc(fv.section, 0x18, g, kRel32);
    o.reloc(fv.section, 0x20, calls, kRel32_1);
    o.reloc(fv.section, 0x28, weights, kRel32);
    o.reloc(fv.section, 0x30, total, kRel32_1);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    return to_json(*r);
}

// The shape of `new_global` and `new_function`: a new global with a dynamic initialiser and an atexit
// destructor, a new function called from the existing (hooked) one.
std::string new_things_like() {
    SynthWorld w = basic_world();
    SynthObj o;
    auto cv = o.function("?compute_value@@YAHXZ", true, 0x40);
    auto nh = o.function("?new_helper@@YAHXZ", true, 0x10);
    const std::uint32_t data = o.section(".data", kData, 0x20);
    const std::uint32_t tracker = o.symbol("?g_tracker@?A0x9@@3UTracker@@A", 0, static_cast<std::int32_t>(data), false, false);
    auto init = o.function("??__Eg_tracker@?A0x9@@YAXXZ", false, 0x20);
    auto fini = o.function("??__Fg_tracker@?A0x9@@YAXXZ", false, 0x10);
    const std::uint32_t xcu = o.section(".CRT$XCU", kRdata, 8);
    const std::uint32_t atexit = o.undefined("atexit", true);
    o.reloc(cv.section, 0x05, nh.symbol, kRel32);
    o.reloc(cv.section, 0x0C, tracker, kRel32);
    o.reloc(init.section, 0x04, atexit, kRel32);
    o.reloc(init.section, 0x0A, fini.symbol, kRel32);
    o.reloc(xcu, 0, init.symbol, kAddr64);
    Built b = o.build();
    InMemoryProcessModules mods;
    mods.add_export("ucrtbase.dll", "atexit", 0x7FFB10001000ull, true);
    w.modules = &mods;
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    return to_json(*r);
}

struct Capture {
    std::mutex mu;
    std::vector<std::string> lines;
};

void capture_sink(void* user, cml::LogLevel level, const char*, int, const char* message) {
    auto* c = static_cast<Capture*>(user);
    if (level != cml::LogLevel::Trace) {
        return;
    }
    std::lock_guard<std::mutex> lock(c->mu);
    c->lines.emplace_back(message);
}

}  // namespace

TEST_CASE("golden: basic-like plan") { check_golden("basic_like", basic_like()); }
TEST_CASE("golden: statics-like plan") { check_golden("statics_like", statics_like()); }
TEST_CASE("golden: new_global / new_function-like plan") { check_golden("new_things_like", new_things_like()); }

TEST_CASE("the JSON of a plan without addresses is the same for any module base") {
    // Same scenario twice, different module base: addresses differ, the address-free form does not.
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x40);
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    o.reloc(f.section, 3, tick, kRel32_1);
    Built b = o.build();
    PlanInput a = w.input(b);
    PlanInput c = w.input(b);
    c.module_base = 0x7FF600000000ull;
    auto ra = plan_bind(a);
    auto rc = plan_bind(c);
    REQUIRE_MESSAGE(ra.has_value(), err(ra));
    REQUIRE_MESSAGE(rc.has_value(), err(rc));
    JsonOptions bare;
    bare.addresses = false;
    CHECK(to_json(*ra) != to_json(*rc));
    CHECK(to_json(*ra, bare) == to_json(*rc, bare));
    bare.reasons = false;
    bare.pretty = false;
    const std::string compact = to_json(*ra, bare);
    CHECK(compact.find('\n') == std::string::npos);
    CHECK(compact.find("\"decision\":\"patch-hooked\"") != std::string::npos);
}

TEST_CASE("every decision and every fix-up is logged at trace level") {
    Capture cap;
    const cml::LogLevel old_level = cml::log_level();
    cml::set_log_sink(&capture_sink, &cap);
    cml::set_log_level(cml::LogLevel::Trace);

    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x40);
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    const std::uint32_t note = o.undefined("fixture_note", true);
    o.function("?new_helper@@YAHXZ", true, 0x10);
    o.reloc(f.section, 3, tick, kRel32_1);
    o.reloc(f.section, 9, note, kRel32);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    cml::set_log_level(old_level);
    cml::set_log_sink(nullptr, nullptr);
    REQUIRE_MESSAGE(r.has_value(), err(r));

    REQUIRE(r->decisions.size() >= 4);
    for (const SymbolDecision& d : r->decisions) {
        bool found = false;
        for (const std::string& line : cap.lines) {
            if (line.find("sym=" + d.name + " ") != std::string::npos &&
                line.find(std::string("decision=") + to_string(d.decision)) != std::string::npos) {
                found = true;
                break;
            }
        }
        CHECK_MESSAGE(found, "no trace line for " << d.name);
    }
    bool fixup_logged = false;
    bool compiland_logged = false;
    for (const std::string& line : cap.lines) {
        if (line.find("bind: fixup") != std::string::npos && line.find("?g_tick@@3HA") != std::string::npos) {
            fixup_logged = true;
        }
        if (line.find("bind: object=logic.cpp.obj") != std::string::npos) {
            compiland_logged = true;
        }
    }
    CHECK(fixup_logged);
    CHECK(compiland_logged);
}
