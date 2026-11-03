// The reload pipeline end to end on every host, with a scripted agent and a fake compiler/linker (WP-12).
//
// Real here: the ReloadService, the module index (a real PDB from the WP-04 builder), the bind planner on a
// synthetic object, the section locators and binding object (WP-06 writer), fix-up generation, hook
// narrowing, the registry, the hook table and the control commands. Faked: cl/link (the "compiler" writes a
// prepared object, the "linker" places the locators at made-up RVAs), and the agent (answers every request).
// The same pipeline runs against the real compiler, linker and agent in the cmltest scenarios.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include "../bind/synth.h"
#include "../bind/worlds.h"
#include "../pdb/test_util.h"
#include "../pe/pe_builder.h"
#include "cml/core/control_server.h"
#include "cml/core/process_registry.h"
#include "cml/core/reload_service.h"
#include "cml/proc/diag_ids.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
using namespace cml_bind_test;
using ipc::json::Value;

namespace {

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

constexpr std::uint64_t kModuleBase = 0x140000000ull;
constexpr std::uint32_t kPid = 4711;
const std::array<std::uint8_t, 16> kGuid = {0x3C, 0x83, 0xC3, 0x8C, 0x00, 0x87, 0x0D, 0xEB, 0x4C, 0x4C, 0x44, 0x20, 0x50, 0x44, 0x42, 0x2E};

class Collect final : public EventListener {
public:
    void on_event(const Event& e) override {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(e);
    }
    std::vector<std::string> diag_ids() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> ids;
        for (const Event& e : events) {
            if (e.name == "diag" && e.body.get_string("severity") == "error") {
                ids.push_back(e.body.get_string("id"));
            }
        }
        return ids;
    }
    int count(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        int n = 0;
        for (const Event& e : events) {
            n += e.name == name ? 1 : 0;
        }
        return n;
    }
    std::mutex mutex;
    std::vector<Event> events;
};

// Object bytes of the patched TU: compute_value (hooked), reading g_tick and calling fixture_note (both bound to
// the old image), optionally calling `callee` (a symbol the registry may know).
std::vector<std::byte> make_object(const std::string& callee = {}, bool define_helper = false, const std::string& unknown_symbol = {}) {
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 32);
    const std::uint32_t tick = o.undefined("?g_tick@@3HA");
    const std::uint32_t note = o.undefined("fixture_note", true);
    o.reloc(f.section, 3, tick, kRel32_1, 0);
    o.reloc(f.section, 9, note, kRel32, 0);
    if (!callee.empty()) {
        const std::uint32_t c = o.undefined(callee, true);
        o.reloc(f.section, 15, c, kRel32, 0);
    }
    if (!unknown_symbol.empty()) {
        const std::uint32_t u = o.undefined(unknown_symbol, true);
        o.reloc(f.section, 21, u, kRel32, 0);
    }
    if (define_helper) {
        o.function("?new_helper@@YAHXZ", true, 16);
    }
    Built b = o.build();
    return std::vector<std::byte>(b.owned->bytes.begin(), b.owned->bytes.end());
}

struct Rig {
    testpdb::TempDir dir;
    std::string exe;
    std::vector<std::byte> object = make_object();

    // knobs of the fake compiler / linker / agent
    bool fail_compile = false;
    std::atomic<int> compile_calls{0};
    std::atomic<int> link_calls{0};
    std::atomic<int> resolve_calls{0};
    std::vector<std::byte> second_object;       // what the "compiler" produces for main.cpp (compiland 1), when set
    bool fail_send = false;                     // InstallHooks cannot be sent (pipe error)
    bool answer_load = true;                    // false: LoadPatch gets no answer (timeout)
    std::function<void()> compile_hook;         // runs inside the fake compiler
    std::uint32_t load_status = 0;              // status of PatchLoaded
    int load_collisions = 0;                    // the first n LoadPatch answer CML4203
    std::uint32_t fixups_status = 0;
    std::uint32_t init_status = 0;
    std::uint32_t hooks_status = 0;
    bool answer_hooks = true;
    bool close_on_load = false;

    std::mutex mutex;
    std::vector<ipc::Message> requests;         // what the agent received, in order
    std::vector<std::uint64_t> link_bases;

    EventBus bus;
    std::shared_ptr<Collect> events = std::make_shared<Collect>();
    std::shared_ptr<AgentLink> link;
    std::unique_ptr<ReloadService> service;
    Session session{1, "0.1.0"};
    std::shared_ptr<ModuleState> module;

    explicit Rig(bool default_session_dir = false) {
        // basic_world() with the compiland's build environment recorded at the head of its module stream (before
        // the first proc), as the compilers do.
        SynthWorld w;
        w.tu().symbols.envblock({{"cwd", dir.dir()}, {"cl", "C:\\VC\\bin\\cl.exe"}, {"cmd", "/c /Zi /Gy logic.cpp"}, {"src", "C:\\proj\\logic.cpp"}});
        w.proc(w.tu(), "compute_value", 0x10);
        w.main_obj().symbols.envblock({{"cwd", dir.dir()}, {"cl", "C:\\VC\\bin\\cl.exe"}, {"cmd", "/c /Zi /Gy main.cpp"}, {"src", "C:\\proj\\main.cpp"}});
        w.proc(w.main_obj(), "fixture_note", 0x100);
        w.data(w.main_obj(), "g_tick", 0x80, false);
        w.pub_func("?compute_value@@YAHXZ", 0x10);
        w.pub_func("fixture_note", 0x100);
        w.pub_data("?g_tick@@3HA", 0x80);
        w.spec.guid = kGuid;
        w.spec.age = 5;
        const std::string pdb = dir.file("app.pdb");
        testpdb::write_file(pdb, testpdb::build(w.spec));
        exe = dir.file("app.exe");
        testpdb::write_file(exe, testpdb::build_pe(kGuid, 5, "C:\\build\\app.pdb"));
        bus.subscribe(events);

        PipelineEnv env;
        env.compile_all = [this](const std::vector<toolchain::CompileRequest>& requests_in, std::vector<toolchain::CompileOutcome>& outcomes, unsigned,
                                 toolchain::CancelToken*) {
            std::vector<Result<void>> results;
            for (std::size_t i = 0; i < requests_in.size(); ++i) {
                ++compile_calls;
                if (compile_hook) {
                    compile_hook();
                }
                outcomes[i].object_path = requests_in[i].output_object;
                outcomes[i].compiler_used = "C:\\VC\\bin\\cl.exe";
                if (fail_compile) {
                    results.emplace_back(Diag::make(2200, "the compiler reported errors in logic.cpp", "Fix the errors."));
                    continue;
                }
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(requests_in[i].output_object).parent_path(), ec);
                std::ofstream out(requests_in[i].output_object, std::ios::binary | std::ios::trunc);
                const std::vector<std::byte>& bytes = (!second_object.empty() && requests_in[i].record.source == "C:\\proj\\main.cpp") ? second_object : object;
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                results.emplace_back();
            }
            return results;
        };
        env.resolve_linker = [this](const std::string&, const std::vector<std::string>&, const ReloadOptions&, std::vector<Diag>*) -> Result<toolchain::ToolInfo> {
            ++resolve_calls;
            toolchain::ToolInfo t;
            t.family = toolchain::ToolFamily::Link;
            t.path = "C:\\VC\\bin\\link.exe";
            t.version_text = "14.44";
            return t;
        };
        env.link_patch = [this](const toolchain::LinkPatchRequest& request, toolchain::LinkTrace*) -> Result<toolchain::LinkedPatch> {
            ++link_calls;
            {
                std::lock_guard<std::mutex> lock(mutex);
                link_bases.push_back(request.base);
            }
            toolchain::LinkedPatch lp;
            lp.dll_path = request.out_dir + "/" + request.name + ".dll";
            lp.pdb_path = request.out_dir + "/" + request.name + ".pdb";
            const cml_test::PeBytes pe = cml_test::build_pe_on_disk();
            std::ofstream out(lp.dll_path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(pe.data()), static_cast<std::streamsize>(pe.size()));
            lp.base = request.base;
            lp.image_size = 0x30000;
            std::uint32_t n = 0;
            for (const coff::SectionLocator& loc : request.objects[0].locators) {
                if (loc.kind == coff::LocatorKind::None) {
                    continue;
                }
                toolchain::SectionPlacement p;
                p.object = 0;
                p.section_index = loc.section_index;
                p.kind = loc.kind;
                p.symbol = loc.symbol_name;
                p.section_rva = 0x1000 + 0x20 * n++;
                p.symbol_rva = p.section_rva + loc.offset;
                lp.sections.push_back(p);
            }
            std::uint32_t k = 0;
            for (const coff::BindSymbol& b : request.bindings) {
                if (b.kind == coff::BindKind::Function) {
                    lp.slot_rvas[b.name] = 0x20000 + 8 * k;
                    lp.thunk_rvas[b.name] = 0x1800 + 16 * k;
                } else {
                    lp.data_rvas[b.name] = 0x21000 + 8 * k;
                }
                ++k;
            }
            return lp;
        };
        service = std::make_unique<ReloadService>(std::move(env), &bus, default_session_dir ? std::string() : dir.subdir("session"));
        link = std::make_shared<AgentLink>(1, [this](const ipc::Message& m) { return agent(m); });
        session.set_answer_sink([this](const ipc::Message& m) { return link->deliver(m); });
        service->attach(link);
    }

    // The scripted agent.
    Result<void> agent(const ipc::Message& m) {
        if (fail_send && std::holds_alternative<ipc::InstallHooks>(m)) {
            return Diag::make(ids::kPipeIoFailed, "pipe broke", "restart");
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            requests.push_back(m);
        }
        if (!answer_load && std::holds_alternative<ipc::LoadPatch>(m)) {
            return Result<void>();  // the request was sent, no answer comes
        }
        const std::uint64_t id = *request_id_of(m);
        const auto reply = [&](std::uint32_t status, const std::string& text = {}) {
            ipc::Reply r;
            r.request_id = id;
            r.status = status;
            r.message = text;
            link->deliver(r);
        };
        if (std::holds_alternative<ipc::ReserveRange>(m)) {
            ipc::ReserveRangeResult r;
            r.request_id = id;
            r.blocks.push_back({0x13F000000ull, 64ull << 20});
            link->deliver(r);
        } else if (const auto* load = std::get_if<ipc::LoadPatch>(&m)) {
            if (close_on_load) {
                std::thread([this] { link->close(); }).detach();
                return Result<void>();
            }
            ipc::PatchLoaded r;
            r.request_id = id;
            r.patch_id = load->patch_id;
            bool collide = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                std::size_t loads = 0;
                for (const ipc::Message& q : requests) {
                    loads += std::holds_alternative<ipc::LoadPatch>(q) ? 1 : 0;
                }
                collide = static_cast<int>(loads) <= load_collisions;
            }
            r.status = collide ? 4203 : load_status;
            r.message = collide ? "the range was taken" : (load_status != 0 ? "LoadLibraryExW failed" : "");
            if (r.status == 0) {
                r.image_base = load->candidates[0].image_base;
                r.image_size = load->candidates[0].image_size;
            }
            link->deliver(r);
        } else if (std::holds_alternative<ipc::ApplyFixups>(m)) {
            reply(fixups_status, fixups_status != 0 ? "fix-up refused" : "");
        } else if (std::holds_alternative<ipc::RunInitializers>(m)) {
            reply(init_status, init_status != 0 ? "initialiser faulted" : "");
        } else if (std::holds_alternative<ipc::InstallHooks>(m)) {
            if (answer_hooks) {
                ipc::HooksInstalled r;
                r.request_id = id;
                r.status = hooks_status;
                r.message = hooks_status != 0 ? "too short" : "";
                r.threads = 3;
                link->deliver(r);
            }
        } else if (std::holds_alternative<ipc::ReleaseRange>(m)) {
            reply(0);
        }
        return Result<void>();
    }

    // Enables app.exe in the session and waits for its index.
    void enable() {
        ipc::Hello hello;
        hello.request_id = 1;
        hello.pid = kPid;
        session.on_message(hello);
        ipc::ModuleList list;
        list.request_id = 2;
        list.modules.push_back({kModuleBase, 0x4000, exe});
        list.modules.push_back({0x7FF800000000ull, 0x90000, "C:\\Windows\\System32\\ntdll.dll"});
        session.on_message(list);
        ipc::EnableModule enable_module;
        enable_module.request_id = 3;
        enable_module.path = exe;
        session.on_message(enable_module);
        link->publish(session);
        service->on_session_changed(1);
        Value args = Value::object();
        args.set("for", Value::string("index"));
        args.set("timeout_ms", Value::integer(20000));
        Result<Value> waited = service->wait(args);
        REQUIRE_MESSAGE(waited.has_value(), waited.error().format());
        module = service->module_state(1, exe);
        REQUIRE(module != nullptr);
        REQUIRE(module->index->status().state == IndexState::Ready);
    }

    // Makes the next sweep report `file` as changed in compiland 0.
    void changed(const std::string& file = "C:\\proj\\logic.cpp") {
        module->index->set_sweep_override_for_testing([file]() -> Result<ModuleIndex::Sweep> {
            ModuleIndex::Sweep s;
            s.changed = {file};
            s.compilands = {0};
            s.file_compilands[file] = {0};
            s.stamps[file] = FileStamp{1, 2, 3};
            s.checked = 1;
            s.hashed = 1;
            return s;
        });
    }
    // Both compilands changed: logic.cpp (0) and main.cpp (1).
    void changed_both() {
        module->index->set_sweep_override_for_testing([]() -> Result<ModuleIndex::Sweep> {
            ModuleIndex::Sweep s;
            for (const char* f : {"C:\\proj\\logic.cpp", "C:\\proj\\main.cpp"}) {
                s.changed.push_back(f);
            }
            s.compilands = {0, 1};
            s.file_compilands["C:\\proj\\logic.cpp"] = {0};
            s.file_compilands["C:\\proj\\main.cpp"] = {1};
            s.stamps["C:\\proj\\logic.cpp"] = FileStamp{1, 2, 3};
            s.stamps["C:\\proj\\main.cpp"] = FileStamp{4, 5, 6};
            return s;
        });
    }
    void unchanged() {
        module->index->set_sweep_override_for_testing([]() -> Result<ModuleIndex::Sweep> { return ModuleIndex::Sweep{}; });
    }

    Result<Value> reload(std::uint32_t timeout_ms = 30000) {
        Value args = Value::object();
        args.set("timeout_ms", Value::integer(timeout_ms));
        return service->reload(args);
    }
    void option(const char* name, Value value) {
        Value args = Value::object();
        args.set("name", Value::string(name));
        args.set("value", std::move(value));
        Result<Value> r = service->set_option(args);
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
    }

    template <class T>
    std::vector<T> seen() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<T> out;
        for (const ipc::Message& m : requests) {
            if (const auto* t = std::get_if<T>(&m)) {
                out.push_back(*t);
            }
        }
        return out;
    }
};

}  // namespace

TEST_CASE("reload: the whole pipeline for the basic fixture shape") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    Result<Value> r = rig.reload();
    REQUIRE_MESSAGE(r.has_value(), r.error().format());

    // The agent saw the protocol in the order of plan section 3.
    const auto reserve = rig.seen<ipc::ReserveRange>();
    REQUIRE(reserve.size() == 1);
    CHECK(reserve[0].module_base == kModuleBase);
    CHECK(reserve[0].module_size == 0x4000);
    const auto loads = rig.seen<ipc::LoadPatch>();
    REQUIRE(loads.size() == 1);
    REQUIRE(loads[0].candidates.size() == 1);
    CHECK(loads[0].candidates[0].fixups.empty());  // applied by the separate ApplyFixups request (timed on its own)
    CHECK(loads[0].candidates[0].image_size == 0x30000);
    const std::uint64_t base = loads[0].candidates[0].image_base;
    CHECK(base % 0x10000 == 0);
    CHECK(base >= 0x13F000000ull);

    const auto fixups = rig.seen<ipc::ApplyFixups>();
    REQUIRE(fixups.size() == 1);
    CHECK(fixups[0].image_base == base);
    // g_tick (REL32_1 at +3), the call to fixture_note retargeted to the real function (REL32 at +9), and the
    // thunk slot.
    std::map<std::uint32_t, ipc::Fixup> by_rva;
    for (const ipc::Fixup& f : fixups[0].fixups) {
        by_rva[f.rva] = f;
    }
    REQUIRE(by_rva.count(0x1003) == 1);
    CHECK(by_rva[0x1003].kind == ipc::FixupKind::Rel32);
    CHECK(by_rva[0x1003].n == 1);
    CHECK(by_rva[0x1003].value == kModuleBase + 0x3080);   // g_tick
    REQUIRE(by_rva.count(0x1009) == 1);
    CHECK(by_rva[0x1009].n == 0);
    CHECK(by_rva[0x1009].value == kModuleBase + 0x1100);   // fixture_note, directly
    bool slot = false;
    for (const ipc::Fixup& f : fixups[0].fixups) {
        if (f.kind == ipc::FixupKind::SlotWrite) {
            slot = true;
            CHECK(f.value == kModuleBase + 0x1100);
        }
    }
    CHECK(slot);

    const auto hooks = rig.seen<ipc::InstallHooks>();
    REQUIRE(hooks.size() == 1);
    REQUIRE(hooks[0].hooks.size() == 1);
    CHECK(hooks[0].hooks[0].old_entry == kModuleBase + 0x1010);
    CHECK(hooks[0].hooks[0].new_entry == base + 0x1000);
    CHECK(hooks[0].hooks[0].name == "compute_value");
    CHECK(rig.seen<ipc::RunInitializers>().empty());  // nothing new to construct
    CHECK(rig.seen<ipc::ReleaseRange>().empty());

    // The answer: counts, per-stage timing, and the RELOAD-TIMING line.
    CHECK(r->get_integer("patched") == 1);
    CHECK(r->get_integer("hooks_installed") == 1);
    const Value* timing = r->find("timing_ms");
    REQUIRE(timing != nullptr);
    for (const char* stage : {"sweep", "compile", "plan", "anchor", "link", "load", "fixups", "init", "hooks", "total"}) {
        const Value* v = timing->find(stage);
        REQUIRE_MESSAGE(v != nullptr, stage);
        CHECK(v->as_double() >= 0.0);
    }
    const std::string line = r->get_string("timing");
    CHECK(line.rfind("RELOAD-TIMING fixture=app sweep_ms=", 0) == 0);
    CHECK(line.find(" hooks_ms=") != std::string::npos);
    CHECK(line.find(" total_ms=") != std::string::npos);

    // State: hook table, generation, accepted baseline, events.
    CHECK(rig.module->is_hooked(kModuleBase + 0x1010));
    CHECK(rig.module->generation == 1);
    CHECK(rig.service->generation() == 1);
    const auto accepted = rig.module->index->accepted_for_testing();
    REQUIRE(accepted.size() == 1);
    CHECK(accepted[0].count("C:\\proj\\logic.cpp") == 1);
    CHECK(rig.events->count("patch_applied") == 1);
    CHECK(rig.events->diag_ids().empty());
}

TEST_CASE("reload: no changed source is a successful no-op without any agent traffic") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.unchanged();
    Result<Value> r = rig.reload();
    REQUIRE(r.has_value());
    CHECK(r->get_integer("patched") == 0);
    CHECK(rig.compile_calls == 0);
    CHECK(rig.requests.empty());
    CHECK(rig.service->generation() == 0);
}

TEST_CASE("reload: a second patch re-points the hook; new functions become canonical and later patches bind to them") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    // Patch 1 defines a new function.
    rig.object = make_object({}, true);
    rig.changed();
    Result<Value> first = rig.reload();
    REQUIRE_MESSAGE(first.has_value(), first.error().format());
    const auto first_loads = rig.seen<ipc::LoadPatch>();
    REQUIRE(first_loads.size() == 1);
    const std::uint64_t base1 = first_loads[0].candidates[0].image_base;
    const auto symbol = rig.module->registry.find("?new_helper@@YAHXZ");
    REQUIRE(symbol.has_value());
    CHECK(symbol->is_function);
    // the helper's section is the second one with a locator: made-up RVA 0x1020
    CHECK(symbol->va == base1 + 0x1020);

    // Patch 2 calls it: the planner resolves it from the registry (resolution step 1).
    rig.object = make_object("?new_helper@@YAHXZ");
    rig.changed();
    Result<Value> second = rig.reload();
    REQUIRE_MESSAGE(second.has_value(), second.error().format());
    const auto loads = rig.seen<ipc::LoadPatch>();
    REQUIRE(loads.size() == 2);
    const std::uint64_t base2 = loads[1].candidates[0].image_base;
    CHECK(base2 > base1);  // placed after the first patch
    const auto fixups = rig.seen<ipc::ApplyFixups>();
    REQUIRE(fixups.size() == 2);
    bool bound_to_patch1 = false;
    for (const ipc::Fixup& f : fixups[1].fixups) {
        if (f.rva == 0x100F && f.kind == ipc::FixupKind::Rel32) {
            bound_to_patch1 = f.value == symbol->va;
        }
    }
    CHECK(bound_to_patch1);
    // compute_value was hooked by patch 1, so patch 2 hooks it again (never dropped by narrowing).
    const auto hooks = rig.seen<ipc::InstallHooks>();
    REQUIRE(hooks.size() == 2);
    REQUIRE(hooks[1].hooks.size() == 1);
    CHECK(hooks[1].hooks[0].old_entry == kModuleBase + 0x1010);
    CHECK(hooks[1].hooks[0].new_entry == base2 + 0x1000);
    CHECK(rig.module->generation == 2);
    CHECK(rig.service->generation() == 2);
}

TEST_CASE("reload: failures surface as diag events with the stage's own stable id") {
    QuietLogs quiet;
    SUBCASE("compile error: CML2200, nothing reaches the agent, the baseline is not advanced") {
        Rig rig;
        rig.enable();
        rig.changed();
        rig.fail_compile = true;
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML2200");
        CHECK(rig.events->diag_ids() == std::vector<std::string>{"CML2200"});
        CHECK(rig.requests.empty());
        CHECK(rig.link_calls == 0);
        CHECK(rig.module->index->accepted_for_testing().empty());  // compile failed: nothing was accepted at all
    }
    SUBCASE("an unresolvable symbol: CML3200 from the planner, before anything is linked") {
        Rig rig;
        rig.enable();
        rig.changed();
        rig.object = make_object({}, false, "no_such_function_anywhere");
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML3200");
        CHECK(r.error().message.find("no_such_function_anywhere") != std::string::npos);
        CHECK(rig.events->diag_ids() == std::vector<std::string>{"CML3200"});
        CHECK(rig.link_calls == 0);
        CHECK(rig.requests.empty());
        const auto accepted = rig.module->index->accepted_for_testing();
        REQUIRE(accepted.size() == 1);
        CHECK(accepted[0].empty());  // the file stays "changed": the next reload processes it again
    }
    SUBCASE("LoadPatch refused: the agent's CML4204 is the id, no hooks are installed, nothing to unload") {
        Rig rig;
        rig.enable();
        rig.changed();
        rig.load_status = 4204;
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML4204");
        CHECK(r.error().message.find("LoadLibraryExW failed") != std::string::npos);
        CHECK(rig.events->diag_ids() == std::vector<std::string>{"CML4204"});
        CHECK(rig.seen<ipc::InstallHooks>().empty());
        CHECK(rig.seen<ipc::ReleaseRange>().empty());
        CHECK_FALSE(rig.module->is_hooked(kModuleBase + 0x1010));
        CHECK(rig.module->generation == 0);
    }
    SUBCASE("ApplyFixups refused: the loaded patch is unloaded again") {
        Rig rig;
        rig.enable();
        rig.changed();
        rig.fixups_status = 4207;
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML4207");
        const auto unloads = rig.seen<ipc::ReleaseRange>();
        REQUIRE(unloads.size() == 1);
        CHECK(unloads[0].mode == ipc::ReleaseMode::UnloadPatch);
        CHECK(unloads[0].base == rig.seen<ipc::LoadPatch>()[0].candidates[0].image_base);
        CHECK(rig.seen<ipc::InstallHooks>().empty());
    }
    SUBCASE("InstallHooks refused: CML4301 from the agent, the patch is unloaded, the hook table is unchanged") {
        Rig rig;
        rig.enable();
        rig.changed();
        rig.hooks_status = 4301;
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML4301");
        CHECK(rig.seen<ipc::ReleaseRange>().size() == 1);
        CHECK_FALSE(rig.module->is_hooked(kModuleBase + 0x1010));
        CHECK(rig.module->generation == 0);
        CHECK(rig.events->count("patch_applied") == 0);
    }
    SUBCASE("a status that is no diagnostic id falls back to CML5307") {
        Rig rig;
        rig.enable();
        rig.changed();
        rig.load_status = 12;
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML5307");
    }
}

TEST_CASE("reload: synchronised mode - no HooksInstalled in time is CML5309 and the patch stays loaded") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    rig.answer_hooks = false;  // the application has not reached SyncPoint()
    rig.option("hooks_timeout_ms", Value::integer(150));
    Result<Value> r = rig.reload();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5309");
    CHECK(rig.seen<ipc::InstallHooks>().size() == 1);
    CHECK(rig.seen<ipc::ReleaseRange>().empty());  // the hooks may still be applied: unloading would crash the process
    CHECK(rig.module->is_hooked(kModuleBase + 0x1010));  // remembered, so the next reload re-points it
    CHECK(rig.events->diag_ids() == std::vector<std::string>{"CML5309"});
}

TEST_CASE("reload: a LoadPatch that finds its range taken is relinked at another base (CML4203)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    rig.load_collisions = 1;
    Result<Value> r = rig.reload();
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(rig.link_calls == 2);
    REQUIRE(rig.link_bases.size() == 2);
    CHECK(rig.link_bases[1] > rig.link_bases[0]);
    const auto loads = rig.seen<ipc::LoadPatch>();
    REQUIRE(loads.size() == 2);
    CHECK(loads[1].candidates[0].image_base == rig.link_bases[1]);
    CHECK(rig.seen<ipc::InstallHooks>().size() == 1);
    const Value* patches = r->find("patches");
    REQUIRE(patches != nullptr);
    CHECK(patches->items()[0].get_integer("load_attempts") == 2);
}

TEST_CASE("reload: once the attempts are used up the collision is reported") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    rig.load_collisions = 10;
    rig.option("max_load_attempts", Value::integer(2));
    Result<Value> r = rig.reload();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML4203");
    CHECK(rig.link_calls == 2);
}

TEST_CASE("reload: the agent disconnecting mid-reload ends it with CML5317") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    rig.close_on_load = true;
    Result<Value> r = rig.reload();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5317");
}

TEST_CASE("reload: no enabled module is CML5300; arguments are validated") {
    QuietLogs quiet;
    Rig rig;  // nothing enabled
    Result<Value> none = rig.reload();
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().id() == "CML5300");
    CHECK(rig.events->diag_ids() == std::vector<std::string>{"CML5300"});

    Value bad = Value::object();
    bad.set("pid", Value::string("x"));
    Result<Value> r = rig.service->reload(bad);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5311");

    rig.enable();
    Value other_pid = Value::object();
    other_pid.set("pid", Value::integer(kPid + 1));
    Result<Value> miss = rig.service->reload(other_pid);
    REQUIRE_FALSE(miss.has_value());
    CHECK(miss.error().id() == "CML5300");
    Value other_module = Value::object();
    other_module.set("module", Value::string("other.dll"));
    CHECK(rig.service->reload(other_module).error().id() == "CML5300");
    Value by_name = Value::object();
    by_name.set("module", Value::string("APP.EXE"));
    rig.unchanged();
    CHECK(rig.service->reload(by_name).has_value());
}

TEST_CASE("reload: a second reload while one runs is refused with CML5304") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    std::mutex m;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    rig.compile_hook = [&] {
        std::unique_lock<std::mutex> lock(m);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    };
    Result<Value> first = Diag::make(5300, "unset", "");
    std::thread worker([&] { first = rig.reload(); });
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return entered; });
    }
    CHECK(rig.service->reload_running());
    Result<Value> second = rig.reload();
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().id() == "CML5304");
    {
        std::lock_guard<std::mutex> lock(m);
        release = true;
    }
    cv.notify_all();
    worker.join();
    CHECK(first.has_value());
    CHECK_FALSE(rig.service->reload_running());
}

TEST_CASE("reload: a deadline that has passed stops the pipeline with CML5302 before the next stage") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    rig.compile_hook = [] { std::this_thread::sleep_for(std::chrono::milliseconds(80)); };
    Result<Value> r = rig.reload(40);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5302");
    CHECK(rig.requests.empty());
}

TEST_CASE("narrowing can be switched off at run time; options validate and report the previous value") {
    QuietLogs quiet;
    Rig rig;
    Value args = Value::object();
    args.set("name", Value::string("narrow_hooks"));
    args.set("value", Value::boolean(false));
    Result<Value> r = rig.service->set_option(args);
    REQUIRE(r.has_value());
    CHECK(r->find("previous")->as_bool() == true);
    CHECK_FALSE(rig.service->options().narrow_hooks);

    args.set("value", Value::integer(3));  // wrong type
    CHECK(rig.service->set_option(args).error().id() == "CML5311");
    args.set("name", Value::string("no_such_option"));
    Result<Value> unknown = rig.service->set_option(args);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().id() == "CML5311");
    CHECK(unknown.error().message.find("narrow_hooks") != std::string::npos);  // lists the options
    Value bad_range = Value::object();
    bad_range.set("name", Value::string("reserve_blocks"));
    bad_range.set("value", Value::integer(1000));
    CHECK(rig.service->set_option(bad_range).error().id() == "CML5311");
    CHECK(rig.service->set_option(Value::object()).error().id() == "CML5311");
}

TEST_CASE("wait: returns when the index is loaded and no reload runs; a bad argument is refused") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    Value args = Value::object();
    args.set("for", Value::string("idle"));
    args.set("timeout_ms", Value::integer(5000));
    Result<Value> r = rig.service->wait(args);
    REQUIRE(r.has_value());
    CHECK(r->get_integer("modules") == 1);
    CHECK(r->find("failed")->items().empty());
    args.set("for", Value::string("everything"));
    CHECK(rig.service->wait(args).error().id() == "CML5311");
}

TEST_CASE("the index of a module whose PDB is missing fails with the PDB's own diagnostic, and reload says so") {
    QuietLogs quiet;
    Rig rig;
    std::filesystem::remove(std::filesystem::path(rig.dir.file("app.pdb")));
    ipc::Hello hello;
    hello.request_id = 1;
    hello.pid = kPid;
    rig.session.on_message(hello);
    ipc::ModuleList list;
    list.request_id = 2;
    list.modules.push_back({kModuleBase, 0x4000, rig.exe});
    rig.session.on_message(list);
    ipc::EnableModule enable_module;
    enable_module.request_id = 3;
    enable_module.path = rig.exe;
    rig.session.on_message(enable_module);
    rig.link->publish(rig.session);
    rig.service->on_session_changed(1);
    auto module = rig.service->module_state(1, rig.exe);
    REQUIRE(module != nullptr);
    CHECK(module->index->wait(20000));
    const IndexStatus status = module->index->status();
    CHECK(status.state == IndexState::Failed);
    REQUIRE(status.error.has_value());
    CHECK(status.error->code >= 1400);
    CHECK(status.error->code < 1500);  // cml_pdb's range
    Result<Value> r = rig.reload();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == status.error->code);
}

TEST_CASE("disabling a module or losing the agent drops its state") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    CHECK(rig.service->index_status(1, rig.exe).has_value());
    ipc::DisableModule disable;
    disable.request_id = 9;
    disable.path = rig.exe;
    rig.session.on_message(disable);
    rig.link->publish(rig.session);
    rig.service->on_session_changed(1);
    CHECK_FALSE(rig.service->index_status(1, rig.exe).has_value());
    CHECK(rig.service->module_state(1, rig.exe) == nullptr);
    rig.service->detach(1);
    Result<Value> r = rig.reload();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5300");
}

TEST_CASE("the PDB index can be released and mapped again (ADR-0017)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    CHECK(rig.module->index->pdb() != nullptr);
    rig.module->index->release_pdb();
    CHECK(rig.module->index->pdb() == nullptr);
    // A reload maps it again by itself ...
    rig.unchanged();
    CHECK(rig.reload().has_value());
    CHECK(rig.module->index->pdb() != nullptr);
    // ... and with the option set it lets go when it is done.
    rig.option("release_pdb_after_reload", Value::boolean(true));
    CHECK(rig.reload().has_value());
    CHECK(rig.module->index->pdb() == nullptr);
}

// ---- the control pipe's view (handle_control_line with a service) -----------------------------------------

TEST_CASE("control: with a service the broker advertises reload, wait, set-option, subscribe and diag-events") {
    QuietLogs quiet;
    Rig rig;
    ControlContext context;
    context.version = "0.1.0";
    context.service = rig.service.get();
    const std::vector<std::string> caps = broker_capabilities(context);
    for (const char* c : {"status", "reload", "wait", "set-option", "subscribe", "diag-events"}) {
        CHECK_MESSAGE(std::find(caps.begin(), caps.end(), c) != caps.end(), c);
    }
    CHECK(broker_capabilities().size() == 1);  // without a service: status only (WP-13)
    ControlContext bare;
    CHECK(broker_capabilities(bare).size() == 1);
}

TEST_CASE("control: status reports the module's index state; reload / wait / set-option / subscribe answer over the line protocol") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    ProcessRegistry registry;
    registry.update(rig.session);
    std::set<std::string> subscribed;
    ControlContext context;
    context.version = "0.1.0";
    context.registry = &registry;
    context.service = rig.service.get();
    context.subscribe = [&](const std::set<std::string>& names) { subscribed = names; };

    const auto call = [&](const std::string& line) {
        Result<Value> parsed = ipc::json::parse(handle_control_line(line, context));
        REQUIRE(parsed.has_value());
        return *parsed;
    };
    const Value status = call(R"({"id":1,"cmd":"status"})");
    REQUIRE(status.get_bool("ok"));
    const Value* index = status.find("result")->find("processes")->items()[0].find("modules")->items()[0].find("index");
    REQUIRE(index != nullptr);
    CHECK(index->get_string("state") == "ready");

    rig.changed();
    const Value reloaded = call(R"({"id":2,"cmd":"reload","args":{"timeout_ms":30000}})");
    REQUIRE_MESSAGE(reloaded.get_bool("ok"), ipc::json::serialize(reloaded));
    CHECK(reloaded.find("result")->get_integer("patched") == 1);
    CHECK(reloaded.find("result")->get_string("timing").rfind("RELOAD-TIMING", 0) == 0);
    CHECK(call(R"({"id":3,"cmd":"status"})").find("result")->get_integer("generation") == 1);

    CHECK(call(R"({"id":4,"cmd":"wait","args":{"for":"idle","timeout_ms":2000}})").get_bool("ok"));
    CHECK(call(R"({"id":5,"cmd":"set-option","args":{"name":"narrow_hooks","value":false}})").get_bool("ok"));
    const Value bad_option = call(R"({"id":6,"cmd":"set-option","args":{"name":"nope","value":1}})");
    CHECK_FALSE(bad_option.get_bool("ok"));
    CHECK(bad_option.find("diag")->get_string("id") == "CML5311");

    CHECK(call(R"({"id":7,"cmd":"subscribe","args":{"events":["diag","log"]}})").get_bool("ok"));
    CHECK(subscribed == std::set<std::string>{"diag", "log"});
    CHECK(call(R"({"id":8,"cmd":"subscribe"})").get_bool("ok"));
    CHECK(subscribed.size() == 3);
    CHECK_FALSE(call(R"({"id":9,"cmd":"subscribe","args":{"events":["nothing"]}})").get_bool("ok"));
    CHECK(call(R"({"id":10,"cmd":"frobnicate"})").find("diag")->get_string("id") == "CML5402");
}

// ---- review fixes -------------------------------------------------------------------------------------------

TEST_CASE("a session directory the user supplied is never removed; the temporary default is") {
    QuietLogs quiet;
    std::string default_dir;
    {
        Rig user_dir;  // constructed with dir.subdir("session")
        user_dir.enable();
        user_dir.unchanged();
        REQUIRE(user_dir.reload().has_value());
        const std::string marker = user_dir.dir.file("session/marker.txt");
        std::ofstream(marker) << "mine";
        const std::string session = user_dir.service->session_dir();
        user_dir.service.reset();  // the destructor must leave the directory and its content
        CHECK(std::filesystem::exists(std::filesystem::path(session)));
        CHECK(std::filesystem::exists(std::filesystem::path(marker)));
    }
    {
        Rig temp(true);
        temp.enable();
        temp.unchanged();
        REQUIRE(temp.reload().has_value());
        default_dir = temp.service->session_dir();
        REQUIRE_FALSE(default_dir.empty());
        CHECK(std::filesystem::exists(std::filesystem::path(default_dir)));
        temp.service.reset();
    }
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(default_dir)));
}

TEST_CASE("status never shows an enabled module as ready before its index exists") {
    QuietLogs quiet;
    Rig rig;
    ipc::Hello hello;
    hello.request_id = 1;
    hello.pid = kPid;
    rig.session.on_message(hello);
    ipc::ModuleList list;
    list.request_id = 2;
    list.modules.push_back({kModuleBase, 0x4000, rig.exe});
    rig.session.on_message(list);
    ipc::EnableModule enable_module;
    enable_module.request_id = 3;
    enable_module.path = rig.exe;
    rig.session.on_message(enable_module);
    ProcessRegistry registry;
    registry.update(rig.session);  // published before the service created the index (the old order)
    ControlContext context;
    context.version = "0.1.0";
    context.registry = &registry;
    context.service = rig.service.get();
    Result<Value> parsed = ipc::json::parse(handle_control_line(R"({"id":1,"cmd":"status"})", context));
    REQUIRE(parsed.has_value());
    const Value* index = parsed->find("result")->find("processes")->items()[0].find("modules")->items()[0].find("index");
    REQUIRE(index != nullptr);
    CHECK(index->get_string("state") == "pending");
}

TEST_CASE("InstallHooks that cannot be sent is a plain failure: the patch is unloaded and the hook table untouched") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    rig.fail_send = true;
    Result<Value> r = rig.reload();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5306");
    CHECK(rig.seen<ipc::ReleaseRange>().size() == 1);
    CHECK_FALSE(rig.module->is_hooked(kModuleBase + 0x1010));
}

TEST_CASE("a LoadPatch without an answer is CML5320 and the patch is unloaded behind it") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    rig.answer_load = false;
    rig.option("agent_timeout_ms", Value::integer(150));
    Result<Value> r = rig.reload();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5320");
    const auto unloads = rig.seen<ipc::ReleaseRange>();
    REQUIRE(unloads.size() == 1);
    CHECK(unloads[0].base == rig.seen<ipc::LoadPatch>()[0].candidates[0].image_base);
}

TEST_CASE("the resolved linker follows set-option linker / linker_path") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    REQUIRE(rig.reload().has_value());
    CHECK(rig.resolve_calls == 1);
    rig.changed();
    REQUIRE(rig.reload().has_value());
    CHECK(rig.resolve_calls == 1);  // cached while the options are unchanged
    rig.option("linker", Value::string("lld-link"));
    rig.changed();
    REQUIRE(rig.reload().has_value());
    CHECK(rig.resolve_calls == 2);
    rig.option("linker_path", Value::string("C:\\tools\\other.exe"));
    rig.changed();
    REQUIRE(rig.reload().has_value());
    CHECK(rig.resolve_calls == 3);
}

TEST_CASE("set-option reserve_block_mb below the room a patch needs is refused") {
    QuietLogs quiet;
    Rig rig;
    Value args = Value::object();
    args.set("name", Value::string("reserve_block_mb"));
    args.set("value", Value::integer(2));
    Result<Value> r = rig.service->set_option(args);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5311");
    CHECK(r.error().message.find("at least 4") != std::string::npos);
    CHECK(rig.service->options().reserve_block_mb == 64);
    args.set("value", Value::integer(4));
    CHECK(rig.service->set_option(args).has_value());
    CHECK(rig.service->options().reserve_block_mb == 4);
}

TEST_CASE("patches applied before a later compiland failed still count in the generation") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed_both();
    rig.second_object = make_object({}, false, "no_such_function_anywhere");  // main.cpp cannot be planned
    Result<Value> r = rig.reload();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML3200");
    CHECK(rig.seen<ipc::LoadPatch>().size() == 1);  // logic.cpp was patched first
    CHECK(rig.module->generation == 1);
    CHECK(rig.service->generation() == 1);
    CHECK(rig.events->count("patch_applied") == 1);
    // Only logic.cpp's stamp is accepted: main.cpp is processed again by the next reload.
    const auto accepted = rig.module->index->accepted_for_testing();
    REQUIRE(accepted.size() == 1);
    CHECK(accepted[0].count("C:\\proj\\logic.cpp") == 1);
    CHECK(accepted[0].count("C:\\proj\\main.cpp") == 0);
}

TEST_CASE("set-option compile_on_save validates, reports the previous value and starts and stops the watcher (WP-14)") {
    QuietLogs quiet;
    Rig rig;
    std::atomic<int> factory_calls{0};
    rig.service->set_watcher_factory_for_testing(
        [&](const std::vector<watch::WatchRoot>&, watch::DirectoryWatcher::Sink, std::vector<Diag>*) -> Result<std::unique_ptr<watch::DirectoryWatcher>> {
            ++factory_calls;
            return Diag::make(5503, "fake", "none");
        });
    rig.enable();
    CHECK_FALSE(rig.service->options().compile_on_save);
    Value args = Value::object();
    args.set("name", Value::string("compile_on_save"));
    args.set("value", Value::integer(1));  // wrong type
    CHECK(rig.service->set_option(args).error().id() == "CML5311");
    CHECK_FALSE(rig.service->options().compile_on_save);
    args.set("value", Value::boolean(true));
    Result<Value> on = rig.service->set_option(args);
    REQUIRE(on.has_value());
    CHECK(on->find("previous")->as_bool() == false);
    CHECK(rig.service->options().compile_on_save);
    // The synthetic PDB lists files that do not exist: nothing is tracked, so no directory is watched.
    CHECK(factory_calls.load() == 0);
    rig.service->rearm_save_watcher();  // harmless
    args.set("value", Value::boolean(false));
    Result<Value> off = rig.service->set_option(args);
    REQUIRE(off.has_value());
    CHECK(off->find("previous")->as_bool() == true);
    CHECK_FALSE(rig.service->options().compile_on_save);
}
