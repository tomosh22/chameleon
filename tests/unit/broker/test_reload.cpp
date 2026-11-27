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
#include "cml/core/restart.h"
#include "cml/core/revert.h"
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
// `const_fill` >= 0 also defines `g_const` (8 bytes of that value), data the old image owns (Rig(..., const_symbol = true)).
std::vector<std::byte> make_object(const std::string& callee = {}, bool define_helper = false, const std::string& unknown_symbol = {}, int const_fill = -1) {
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
    if (const_fill >= 0) {
        const std::uint32_t sec = o.section(".data$g", kDataComdat, 8, coff::ComdatSelection::Any);
        o.symbol("?g_const@@3HB", 0, static_cast<std::int32_t>(sec), false, true);
        for (std::byte& b : o.model().sections[sec - 1].data) {
            b = static_cast<std::byte>(const_fill);
        }
    }
    Built b = o.build();
    return std::vector<std::byte>(b.owned->bytes.begin(), b.owned->bytes.end());
}

struct Rig {
    testpdb::TempDir dir;
    std::string exe;
    std::string logic_src = "C:\\proj\\logic.cpp";  // WP-45: a real file in `dir` with Rig(..., real_sources = true)
    std::vector<std::string> link_dirs;             // WP-45: out_dir of every link request, in order
    std::vector<std::byte> object = make_object();

    // knobs of the fake compiler / linker / agent
    bool fail_compile = false;
    std::string compile_cwd = "C:\\proj\\build";               // WP-35: the directory the fake compiler "ran in"
    std::vector<toolchain::ToolDiagnostic> compile_errors;  // WP-35: what the failing fake compiler printed (parsed)
    bool short_results = false;                 // compile_all returns one result too few
    bool compile_warning = false;               // the fake compiler reports a warning
    std::vector<std::string> compile_outputs;   // output_object of every compile request
    std::atomic<int> compile_calls{0};
    std::atomic<bool> block_until_cancel{false};  // WP-45: the fake compiler runs until the broker cancels it
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
    std::uint32_t hooks_status2 = 0;            // WP-22: the second process' answer to InstallHooks
    std::uint32_t uncommit_status = 0;          // WP-22: status of the answer to ReleaseRange mode Uncommit (an older agent refuses it)
    bool answer_hooks = true;
    bool close_on_load = false;
    std::uint32_t arm_status = 0;               // WP-28: status of the Reply to ArmFault (failure injection)
    std::uint32_t arm_status2 = 0;              // WP-28: the same for the second process
    std::function<void(const ipc::ArmFault&)> on_arm;  // WP-44: runs when an agent receives ArmFault, before it answers

    std::mutex mutex;
    std::vector<ipc::Message> requests;         // what the agent received, in order (requests only)
    std::vector<ipc::CompileEvent> compile_events;  // WP-17: the compile notifications, in order
    std::vector<std::uint64_t> link_bases;

    PipelineEnv pipeline_env;                   // a copy of what the service uses (tests that drive PatchPipeline directly)
    EventBus bus;
    std::shared_ptr<Collect> events = std::make_shared<Collect>();
    std::shared_ptr<AgentLink> link;
    std::unique_ptr<ReloadService> service;
    Session session{1, "0.1.0"};
    std::shared_ptr<ModuleState> module;
    std::shared_ptr<AgentLink> link2;           // WP-22: a second process running the same exe (enable2)
    Session session2{2, "0.1.0"};
    std::shared_ptr<ModuleState> module2;

    // `real_sources` (WP-45): logic.cpp exists (in `dir`) and is the compiland's source, so the module index tracks it and the real
    // sweep (no override) sees its edits.
    explicit Rig(bool default_session_dir = false, const std::string& logic_flags = std::string(), bool const_symbol = false, bool real_sources = false) {
        if (real_sources) {
            logic_src = dir.file("logic.cpp");
            write_logic(1);
        }
        // basic_world() with the compiland's build environment recorded at the head of its module stream (before
        // the first proc), as the compilers do.
        SynthWorld w;
        w.tu().symbols.envblock({{"cwd", dir.dir()}, {"cl", "C:\\VC\\bin\\cl.exe"}, {"cmd", "/c /Zi /Gy " + logic_flags + " logic.cpp"}, {"src", logic_src}});
        if (real_sources) {
            w.tu().files = {logic_src};  // the compiland's file list (what the index tracks)
        }
        w.proc(w.tu(), "compute_value", 0x10);
        w.main_obj().symbols.envblock({{"cwd", dir.dir()}, {"cl", "C:\\VC\\bin\\cl.exe"}, {"cmd", "/c /Zi /Gy main.cpp"}, {"src", "C:\\proj\\main.cpp"}});
        w.proc(w.main_obj(), "fixture_note", 0x100);
        w.data(w.main_obj(), "g_tick", 0x80, false);
        w.pub_func("?compute_value@@YAHXZ", 0x10);
        w.pub_func("fixture_note", 0x100);
        w.pub_data("?g_tick@@3HA", 0x80);
        if (const_symbol) {
            w.pub_data("?g_const@@3HB", 0x30, SynthWorld::kRdataSec);  // WP-25: data of the old image (RVA 0x2030, in the synthetic PE's .rdata)
        }
        w.spec.guid = kGuid;
        w.spec.age = 5;
        const std::string pdb = dir.file("app.pdb");
        testpdb::write_file(pdb, testpdb::build(w.spec));
        exe = dir.file("app.exe");
        testpdb::write_file(exe, testpdb::build_pe(kGuid, 5, "C:\\build\\app.pdb"));
        bus.subscribe(events);

        PipelineEnv env;
        env.compile_all = [this](const std::vector<toolchain::CompileRequest>& requests_in, std::vector<toolchain::CompileOutcome>& outcomes, unsigned,
                                 toolchain::CancelToken* cancel, const std::function<void(std::size_t)>& on_done) {
            std::vector<Result<void>> results;
            for (std::size_t i = 0; i < requests_in.size(); ++i) {
                ++compile_calls;
                if (compile_hook) {
                    compile_hook();
                }
                // WP-45: a compiler that runs until the broker cancels it (like the real driver, which kills its processes).
                while (block_until_cancel.load() && (cancel == nullptr || !cancel->cancelled())) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (cancel != nullptr && cancel->cancelled()) {
                    results.emplace_back(Diag::make(2200, "the compile was cancelled", "x"));
                    continue;
                }
                outcomes[i].object_path = requests_in[i].output_object;
                outcomes[i].compiler_used = "C:\\VC\\bin\\cl.exe";
                if (on_done) {
                    on_done(i);
                }
                if (short_results && i + 1 == requests_in.size() && requests_in.size() > 1) {
                    break;  // the driver "loses" the last result
                }
                compile_outputs.push_back(requests_in[i].output_object);
                if (compile_warning) {
                    outcomes[i].warnings.push_back(Diag::make(2201, "the compiler warned about logic.cpp", "Fix it."));
                }
                if (fail_compile) {
                    outcomes[i].diagnostics = compile_errors;
                    outcomes[i].cwd = compile_cwd;
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
        env.resolve_linker = [this](const std::string&, const pdb::PdbIndex*, const std::vector<std::string>&, const ReloadOptions&,
                                    std::vector<Diag>*) -> Result<toolchain::ToolInfo> {
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
                link_dirs.push_back(request.out_dir);
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
        pipeline_env = env;
        service = std::make_unique<ReloadService>(std::move(env), &bus, default_session_dir ? std::string() : dir.subdir("session"));
        {
            // WP-50: the tests decide when the module file is read (several rewrite it after the enable); the background read has
            // tests of its own.
            Value off = Value::object();
            off.set("name", Value::string("prefetch_module_image"));
            off.set("value", Value::boolean(false));
            REQUIRE(service->set_option(off).has_value());
        }
        link = std::make_shared<AgentLink>(1, [this](const ipc::Message& m) { return agent(m); });
        session.set_answer_sink([this](const ipc::Message& m) { return link->deliver(m); });
        service->attach(link);
    }

    // The scripted agent of the first process; agent_on() serves any link (WP-22: a second process).
    Result<void> agent(const ipc::Message& m) { return agent_on(*link, m); }
    Result<void> agent_on(AgentLink& l, const ipc::Message& m) {
        if (fail_send && std::holds_alternative<ipc::InstallHooks>(m)) {
            return Diag::make(ids::kPipeIoFailed, "pipe broke", "restart");
        }
        if (const auto* event = std::get_if<ipc::CompileEvent>(&m)) {
            std::lock_guard<std::mutex> lock(mutex);
            compile_events.push_back(*event);  // an event: no request id, no answer
            return Result<void>();
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
            l.deliver(r);
        };
        if (std::holds_alternative<ipc::ReserveRange>(m)) {
            ipc::ReserveRangeResult r;
            r.request_id = id;
            r.blocks.push_back({0x13F000000ull, 64ull << 20});
            l.deliver(r);
        } else if (const auto* load = std::get_if<ipc::LoadPatch>(&m)) {
            if (close_on_load) {
                std::thread([&l] { l.close(); }).detach();
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
            l.deliver(r);
        } else if (std::holds_alternative<ipc::ApplyFixups>(m)) {
            reply(fixups_status, fixups_status != 0 ? "fix-up refused" : "");
        } else if (std::holds_alternative<ipc::RunInitializers>(m)) {
            reply(init_status, init_status != 0 ? "initialiser faulted" : "");
        } else if (std::holds_alternative<ipc::InstallHooks>(m)) {
            if (answer_hooks) {
                ipc::HooksInstalled r;
                r.request_id = id;
                const std::uint32_t hooks_result = (link2 && &l == link2.get()) ? hooks_status2 : hooks_status;
                r.status = hooks_result;
                r.message = hooks_result != 0 ? "too short" : "";
                r.threads = 3;
                l.deliver(r);
            }
        } else if (const auto* arm = std::get_if<ipc::ArmFault>(&m)) {
            if (on_arm) {
                on_arm(*arm);
            }
            const std::uint32_t arm_result = (link2 && &l == link2.get()) ? arm_status2 : arm_status;
            reply(arm_result, arm_result != 0 ? "not an injection stage of the agent" : "armed");
        } else if (const auto* release = std::get_if<ipc::ReleaseRange>(&m)) {
            if (release->mode == ipc::ReleaseMode::Uncommit && uncommit_status != 0) {
                reply(uncommit_status, "unknown release mode");  // an older agent
            } else {
                reply(0);
            }
        }
        return Result<void>();
    }

    // WP-45: (re)writes logic.cpp with compute_value() returning `value` (Rig(..., real_sources = true)).
    void write_logic(int value) {
        std::ofstream out(logic_src, std::ios::binary | std::ios::trunc);
        // A different size for every value: the sweep's cheap test (size and time) must not see two edits as one.
        out << "int compute_value() { return " << value << "; }\n" << std::string(static_cast<std::size_t>(value), '/') << "\n";
    }

    // Enables app.exe in the session and waits for its index.
    // WP-45: the agent registers (Hello) without enabling anything yet; enable() then skips its Hello.
    bool said_hello = false;
    void hello() {
        ipc::Hello h;
        h.request_id = 1;
        h.pid = kPid;
        session.on_message(h);
        link->publish(session);
        service->on_session_changed(1);
        said_hello = true;
    }

    void enable() {
        if (!said_hello) {
            ipc::Hello hello;
            hello.request_id = 1;
            hello.pid = kPid;
            session.on_message(hello);
            said_hello = true;
        }
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

    // WP-22: a second process (pid kPid + 1, agent 2) that runs the same exe; its answers come from agent_on().
    void enable2() {
        link2 = std::make_shared<AgentLink>(2, [this](const ipc::Message& m) { return agent_on(*link2, m); });
        session2.set_answer_sink([this](const ipc::Message& m) { return link2->deliver(m); });
        service->attach(link2);
        ipc::Hello hello;
        hello.request_id = 1;
        hello.pid = kPid + 1;
        session2.on_message(hello);
        ipc::ModuleList list;
        list.request_id = 2;
        list.modules.push_back({kModuleBase, 0x4000, exe});
        list.modules.push_back({0x7FF800000000ull, 0x90000, "C:\\Windows\\System32\\ntdll.dll"});
        session2.on_message(list);
        ipc::EnableModule enable_module;
        enable_module.request_id = 3;
        enable_module.path = exe;
        session2.on_message(enable_module);
        link2->publish(session2);
        service->on_session_changed(2);
        Value args = Value::object();
        args.set("for", Value::string("index"));
        args.set("timeout_ms", Value::integer(20000));
        Result<Value> waited = service->wait(args);
        REQUIRE_MESSAGE(waited.has_value(), waited.error().format());
        module2 = service->module_state(2, exe);
        REQUIRE(module2 != nullptr);
        REQUIRE(module2->index->status().state == IndexState::Ready);
    }
    void changed_on(ModuleState& m, const std::string& file = "C:\\proj\\logic.cpp") {
        m.index->set_sweep_override_for_testing([file]() -> Result<ModuleIndex::Sweep> {
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

    std::uint32_t faults_left(const std::string& stage) { return service->faults().armed(ipc::fault::find_stage(stage)->stage); }

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

    // WP-17: the agent was told that the compile started and then that it succeeded.
    REQUIRE(rig.compile_events.size() == 2);
    CHECK(rig.compile_events[0].phase == ipc::CompilePhase::Start);
    CHECK(rig.compile_events[1].phase == ipc::CompilePhase::Success);
    CHECK(rig.compile_events[0].module.find("app") == 0);
    CHECK(rig.compile_events[1].diag == 0);
}

// ---- WP-36 (ADR-0042): image-relative references to old data -----------------------------------------------------

namespace {

// compute_value: lea rdi,[__ImageBase] ; <an instruction with g_tick@IMGREL + 0x10> ; ret.
// `displacement`: mov eax,[rdi+rax*4+disp32] (what cl emits); otherwise mov eax, imm32 (never retargetable).
std::vector<std::byte> make_imgrel_object(bool displacement) {
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 32);
    const std::uint32_t image_base = o.undefined("__ImageBase");
    const std::uint32_t tick = o.undefined("?g_tick@@3HA");
    const std::vector<int> code = displacement ? std::vector<int>{0x48, 0x8D, 0x3D, 0, 0, 0, 0, 0x8B, 0x84, 0x87, 0, 0, 0, 0, 0xC3}
                                               : std::vector<int>{0x48, 0x8D, 0x3D, 0, 0, 0, 0, 0xB8, 0, 0, 0, 0, 0xC3};
    coff::ModelSection& s = o.model().sections[f.section - 1];
    for (std::size_t i = 0; i < s.data.size(); ++i) {
        s.data[i] = static_cast<std::byte>(i < code.size() ? code[i] : 0xCC);
    }
    o.reloc(f.section, 3, image_base, kRel32, 0);
    o.reloc(f.section, displacement ? 10 : 8, tick, kAddr32Nb, 0x10);
    Built b = o.build();
    return std::vector<std::byte>(b.owned->bytes.begin(), b.owned->bytes.end());
}

}  // namespace

TEST_CASE("reload (WP-36): an image-relative displacement to old data reaches the agent as an Addr32NbDisp fix-up") {
    QuietLogs quiet;
    Rig rig;
    rig.object = make_imgrel_object(true);
    rig.enable();
    rig.changed();
    Result<Value> r = rig.reload();
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    const auto fixups = rig.seen<ipc::ApplyFixups>();
    REQUIRE(fixups.size() == 1);
    std::map<std::uint32_t, ipc::Fixup> by_rva;
    for (const ipc::Fixup& f : fixups[0].fixups) {
        by_rva[f.rva] = f;
    }
    // The displacement at +10 of the function (section at RVA 0x1000): the absolute target S + A travels, the agent
    // subtracts the base the patch really has.
    REQUIRE(by_rva.count(0x100A) == 1);
    CHECK(by_rva[0x100A].kind == ipc::FixupKind::Addr32NbDisp);
    CHECK(by_rva[0x100A].n == 0);
    CHECK(by_rva[0x100A].value == kModuleBase + 0x3080 + 0x10);
    // __ImageBase stays the patch's own: the lea at +3 gets no fix-up.
    CHECK(by_rva.count(0x1003) == 0);
    // The evidence counters, in the answer and per patch.
    CHECK(r->get_integer("imgrel_sites") == 1);
    CHECK(r->get_integer("imgrel_verified") == 1);
    const Value* patches = r->find("patches");
    REQUIRE(patches != nullptr);
    REQUIRE(patches->is_array());
    REQUIRE(patches->size() == 1);
    CHECK(patches->items()[0].get_integer("imgrel_sites") == 1);
    CHECK(patches->items()[0].get_integer("imgrel_verified") == 1);
    CHECK(r->get_integer("patched") == 1);
    CHECK(rig.events->diag_ids().empty());
}

TEST_CASE("reload (WP-36): an image-relative reference that is not a displacement is refused with CML3101 before anything is linked") {
    QuietLogs quiet;
    Rig rig;
    rig.object = make_imgrel_object(false);
    rig.enable();
    rig.changed();
    Result<Value> r = rig.reload();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML3101");
    CHECK(r.error().message.find("b8 10 00 00 00") != std::string::npos);  // the instruction bytes (the addend 0x10 in place)
    CHECK(r.error().message.find("?g_tick@@3HA") != std::string::npos);
    CHECK(rig.events->diag_ids() == std::vector<std::string>{"CML3101"});
    CHECK(rig.link_calls == 0);
    CHECK(rig.requests.empty());
}

TEST_CASE("reload: the answer always carries the WP-36 counters (0 when the object has no image-relative reference)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    Result<Value> r = rig.reload();
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    const Value* sites = r->find("imgrel_sites");
    REQUIRE(sites != nullptr);
    CHECK(sites->is_integer());
    CHECK(r->get_integer("imgrel_sites") == 0);
    CHECK(r->get_integer("imgrel_verified") == 0);
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

// ---- WP-22: patch revert ------------------------------------------------------------------------------------------

namespace {

Result<Value> revert_with(Rig& rig, const char* scope) {
    Value args = Value::object();
    if (scope != nullptr) {
        args.set("scope", Value::string(scope));
    }
    return rig.service->revert(args);
}

// Patch 1 defines new_helper and hooks compute_value; patch 2 calls the helper and hooks compute_value again.
void apply_two_patches(Rig& rig) {
    rig.object = make_object({}, true);
    rig.changed();
    Result<Value> first = rig.reload();
    REQUIRE_MESSAGE(first.has_value(), first.error().format());
    rig.object = make_object("?new_helper@@YAHXZ");
    rig.changed();
    Result<Value> second = rig.reload();
    REQUIRE_MESSAGE(second.has_value(), second.error().format());
}

}  // namespace

TEST_CASE("revert last: the function jumps back to the previous patch; its symbols, generation and baseline stay") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    apply_two_patches(rig);
    const auto loads = rig.seen<ipc::LoadPatch>();
    REQUIRE(loads.size() == 2);
    const std::uint64_t base1 = loads[0].candidates[0].image_base;
    const std::uint64_t base2 = loads[1].candidates[0].image_base;
    CHECK(rig.module->hooked.at(kModuleBase + 0x1010) == base2 + 0x1000);
    REQUIRE(rig.module->batches.size() == 2);

    Result<Value> r = revert_with(rig, "last");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    const Value* reverted = r->find("reverted");
    REQUIRE(reverted != nullptr);
    REQUIRE(reverted->items().size() == 1);
    CHECK(reverted->items()[0].get_integer("pid") == kPid);
    CHECK(reverted->items()[0].get_integer("generation") == 1);
    CHECK(reverted->items()[0].get_integer("hooks_repointed") == 1);
    CHECK(reverted->items()[0].get_integer("hooks_restored") == 0);
    CHECK(r->get_integer("generation") == 1);

    // The agent was asked to re-point (not to restore the original bytes), quietly, to patch 1's function.
    const auto hooks = rig.seen<ipc::InstallHooks>();
    REQUIRE(hooks.size() == 3);
    CHECK(hooks[2].flags == ipc::kInstallHooksQuiet);
    REQUIRE(hooks[2].hooks.size() == 1);
    CHECK(hooks[2].hooks[0].old_entry == kModuleBase + 0x1010);
    CHECK(hooks[2].hooks[0].new_entry == base1 + 0x1000);
    CHECK(rig.module->hooked.at(kModuleBase + 0x1010) == base1 + 0x1000);
    CHECK(rig.module->generation == 1);
    CHECK(rig.service->generation() == 1);
    CHECK(rig.module->batches.size() == 1);
    CHECK(rig.events->count("patch_reverted") == 1);
    // Patch 1's helper is still canonical: a later reload may bind to it.
    CHECK(rig.module->registry.find("?new_helper@@YAHXZ").has_value());
    // The loaded patches stay where they are: nothing is unloaded; the reverted patch is only un-committed (WP-22).
    const auto releases = rig.seen<ipc::ReleaseRange>();
    REQUIRE(releases.size() == 1);
    CHECK(releases[0].mode == ipc::ReleaseMode::Uncommit);
    CHECK(releases[0].base == base2);
}

TEST_CASE("revert all: the original bytes come back, the registry forgets the patches' symbols and the next reload cannot bind to them") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    apply_two_patches(rig);

    Result<Value> r = revert_with(rig, "all");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->find("reverted")->items().size() == 1);
    CHECK(r->find("reverted")->items()[0].get_integer("batches") == 2);
    CHECK(r->find("reverted")->items()[0].get_integer("patches") == 2);
    CHECK(r->find("reverted")->items()[0].get_integer("hooks_restored") == 1);
    CHECK(r->find("reverted")->items()[0].get_integer("hooks_repointed") == 0);
    CHECK(r->find("reverted")->items()[0].get_integer("generation") == 0);
    CHECK(r->get_integer("generation") == 0);

    const auto hooks = rig.seen<ipc::InstallHooks>();
    REQUIRE(hooks.size() == 3);
    CHECK(hooks[2].flags == ipc::kInstallHooksRevert);
    REQUIRE(hooks[2].hooks.size() == 1);
    CHECK(hooks[2].hooks[0].old_entry == kModuleBase + 0x1010);
    CHECK(rig.module->hooked.empty());
    CHECK(rig.module->generation == 0);
    CHECK(rig.service->generation() == 0);
    CHECK(rig.module->batches.empty());
    CHECK_FALSE(rig.module->registry.find("?new_helper@@YAHXZ").has_value());
    CHECK(rig.events->count("patch_reverted") == 1);

    // A reload after the revert: the helper of the reverted patch must not be found (FU-13's second-reload case).
    rig.object = make_object("?new_helper@@YAHXZ");
    rig.changed();
    Result<Value> again = rig.reload();
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().id() == "CML3200");

    // With a definition of its own the reload works, and the numbering of the patch files carries on (the earlier
    // DLLs are still loaded, their names are taken).
    rig.object = make_object({}, true);
    rig.changed();
    Result<Value> third = rig.reload();
    REQUIRE_MESSAGE(third.has_value(), third.error().format());
    CHECK(rig.module->generation == 1);
    const auto loads = rig.seen<ipc::LoadPatch>();
    REQUIRE(loads.size() == 3);
    CHECK(loads[2].candidates[0].image_base > loads[1].candidates[0].image_base);
    CHECK(loads[2].candidates[0].path != loads[0].candidates[0].path);
    CHECK(loads[2].candidates[0].path != loads[1].candidates[0].path);
}

TEST_CASE("revert last, then last again, then nothing: LIFO, and CML5850 when no patch is left") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    apply_two_patches(rig);
    REQUIRE(revert_with(rig, nullptr).has_value());  // scope defaults to "last"
    CHECK(rig.module->generation == 1);
    Result<Value> second = revert_with(rig, "last");
    REQUIRE_MESSAGE(second.has_value(), second.error().format());
    CHECK(second->find("reverted")->items()[0].get_integer("hooks_restored") == 1);
    CHECK(rig.module->generation == 0);
    CHECK(rig.module->hooked.empty());
    Result<Value> third = revert_with(rig, "last");
    REQUIRE_FALSE(third.has_value());
    CHECK(third.error().id() == "CML5850");
    CHECK(rig.seen<ipc::InstallHooks>().size() == 4);  // two applied + two reverted; the refused third sent nothing
}

TEST_CASE("revert: the reload that follows recompiles the files again (the source baseline went back)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    REQUIRE(rig.reload().has_value());
    REQUIRE(rig.module->index->accepted_for_testing().size() == 1);
    REQUIRE(revert_with(rig, "all").has_value());
    // The batch replaced the baseline stamp of logic.cpp; the revert puts the previous one back. The synthetic index has
    // no baseline of real files, so there is nothing to put back here: what must hold is that a revert restores and does
    // not count as an accept, and that a following reload still patches.
    CHECK(rig.module->index->accepted_for_testing().size() == 1);
    rig.changed();
    Result<Value> again = rig.reload();
    REQUIRE_MESSAGE(again.has_value(), again.error().format());
    CHECK(again->get_integer("patched") == 1);
    CHECK(rig.module->generation == 1);
}

TEST_CASE("revert: a reload that applied two patches is one batch; a refusal by the agent changes nothing (CML5851)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.option("narrow_hooks", Value::boolean(false));  // WP-25: identical code is not loaded again; this test is about batches
    rig.changed_both();
    rig.second_object = make_object();  // main.cpp patches too
    Result<Value> r = rig.reload();
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(rig.module->batches.size() == 1);
    CHECK(rig.module->batches[0].patches.size() == 2);
    CHECK(rig.module->generation == 2);

    rig.hooks_status = 4305;  // a thread would not leave the function
    Result<Value> refused = revert_with(rig, "last");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().id() == "CML5851");
    CHECK(refused.error().message.find("CML4305") != std::string::npos);
    CHECK(rig.module->generation == 2);
    CHECK(rig.module->batches.size() == 1);
    CHECK_FALSE(rig.module->hooked.empty());
    CHECK(rig.service->generation() == 2);
    CHECK(rig.events->count("patch_reverted") == 0);
    CHECK(rig.events->diag_ids().back() == "CML5851");

    rig.hooks_status = 0;
    Result<Value> ok = revert_with(rig, "last");
    REQUIRE_MESSAGE(ok.has_value(), ok.error().format());
    CHECK(ok->find("reverted")->items()[0].get_integer("patches") == 2);
    CHECK(rig.module->generation == 0);
    CHECK(rig.service->generation() == 0);
}

TEST_CASE("revert: arguments are validated; a module or process that does not match has nothing to revert") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    apply_two_patches(rig);
    Result<Value> bad = revert_with(rig, "everything");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().id() == "CML5853");
    Value args = Value::object();
    args.set("pid", Value::integer(kPid + 1));
    Result<Value> other = rig.service->revert(args);
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error().id() == "CML5850");
    Value named = Value::object();
    named.set("module", Value::string("app.exe"));
    named.set("scope", Value::string("last"));
    CHECK(rig.service->revert(named).has_value());
    CHECK(rig.module->generation == 1);
}

TEST_CASE("revert last means the newest reload: a module that reload did not touch keeps its older one") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.enable2();
    rig.option("narrow_hooks", Value::boolean(false));  // WP-25: identical code is not loaded again; this test is about batches
    // Reload 1 patches both processes ...
    rig.changed_on(*rig.module);
    rig.changed_on(*rig.module2);
    REQUIRE(rig.reload().has_value());
    REQUIRE(rig.module->batches.size() == 1);
    REQUIRE(rig.module2->batches.size() == 1);
    // ... reload 2 only the first.
    rig.changed();
    Value only_first = Value::object();
    only_first.set("pid", Value::integer(kPid));
    REQUIRE(rig.service->reload(only_first).has_value());
    REQUIRE(rig.module->batches.size() == 2);
    CHECK(rig.module->batches[1].reload_id > rig.module->batches[0].reload_id);
    CHECK(rig.module->batches[0].reload_id == rig.module2->batches[0].reload_id);

    Result<Value> r = revert_with(rig, "last");  // no pid, no module
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->find("reverted")->items().size() == 1);  // the second process was not part of reload 2
    CHECK(r->find("reverted")->items()[0].get_integer("pid") == kPid);
    CHECK(rig.module->batches.size() == 1);
    CHECK(rig.module2->batches.size() == 1);  // intact
    CHECK(rig.module2->generation == 1);
    CHECK_FALSE(rig.module2->hooked.empty());

    // The next `last` is reload 1, in both processes.
    Result<Value> again = revert_with(rig, "last");
    REQUIRE_MESSAGE(again.has_value(), again.error().format());
    CHECK(again->find("reverted")->items().size() == 2);
    CHECK(rig.module->batches.empty());
    CHECK(rig.module2->batches.empty());
    CHECK(rig.service->generation() == 0);
}

TEST_CASE("revert across processes: a refusal in one process still reports the other as reverted, and a retry does not undo it again (CML5854)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.enable2();
    rig.changed_on(*rig.module);
    rig.changed_on(*rig.module2);
    REQUIRE(rig.reload().has_value());
    const std::size_t hook_requests = rig.seen<ipc::InstallHooks>().size();

    rig.hooks_status2 = 4305;  // a thread of the second process would not leave the function
    Result<Value> r = revert_with(rig, "all");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(r->find("reverted")->items().size() == 1);
    CHECK(r->find("reverted")->items()[0].get_integer("pid") == kPid);
    CHECK(r->get_bool("partial"));
    REQUIRE(r->find("failed") != nullptr);
    REQUIRE(r->find("failed")->items().size() == 1);
    CHECK(r->find("failed")->items()[0].get_integer("pid") == kPid + 1);
    CHECK(r->find("failed")->items()[0].get_string("id") == "CML5851");
    CHECK(r->get_string("diag") == "CML5854");
    CHECK(rig.events->diag_ids().back() == "CML5854");
    CHECK(rig.module->batches.empty());
    CHECK(rig.module2->batches.size() == 1);
    CHECK(rig.module2->generation == 1);
    CHECK(rig.service->generation() == 1);

    // The retry only touches what is still applied: the first process is not asked again.
    rig.hooks_status2 = 0;
    const std::size_t before_retry = rig.seen<ipc::InstallHooks>().size();
    Result<Value> retry = revert_with(rig, "all");
    REQUIRE_MESSAGE(retry.has_value(), retry.error().format());
    REQUIRE(retry->find("reverted")->items().size() == 1);
    CHECK(retry->find("reverted")->items()[0].get_integer("pid") == kPid + 1);
    CHECK(retry->find("failed") == nullptr);
    CHECK(rig.seen<ipc::InstallHooks>().size() == before_retry + 1);
    CHECK(before_retry == hook_requests + 2);
    CHECK(rig.service->generation() == 0);
    CHECK(rig.module2->batches.empty());
}

TEST_CASE("revert across processes: when every module fails the error is the first one, with the count") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.enable2();
    rig.changed_on(*rig.module);
    rig.changed_on(*rig.module2);
    REQUIRE(rig.reload().has_value());
    rig.hooks_status = 4305;
    rig.hooks_status2 = 4305;
    Result<Value> r = revert_with(rig, "all");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5851");
    CHECK(r.error().message.find("2 of 2 modules failed") != std::string::npos);
    CHECK(rig.module->batches.size() == 1);
    CHECK(rig.module2->batches.size() == 1);
}

TEST_CASE("revert: the reverted patches are un-committed in the agent, newest first; an agent that refuses that costs a warning, not the revert (CML5855)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    apply_two_patches(rig);
    const auto loads = rig.seen<ipc::LoadPatch>();
    REQUIRE(loads.size() == 2);
    const std::uint64_t base1 = loads[0].candidates[0].image_base;
    const std::uint64_t base2 = loads[1].candidates[0].image_base;

    // last: only patch 2's image
    REQUIRE(revert_with(rig, "last").has_value());
    auto releases = rig.seen<ipc::ReleaseRange>();
    REQUIRE(releases.size() == 1);
    CHECK(releases[0].mode == ipc::ReleaseMode::Uncommit);
    CHECK(releases[0].base == base2);

    // all: the rest (patch 1)
    rig.uncommit_status = 5201;  // an agent that does not know the mode
    Result<Value> r = revert_with(rig, "all");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    releases = rig.seen<ipc::ReleaseRange>();
    REQUIRE(releases.size() == 2);
    CHECK(releases[1].mode == ipc::ReleaseMode::Uncommit);
    CHECK(releases[1].base == base1);
    CHECK(rig.module->batches.empty());  // the revert itself went through
    bool warned = false;
    for (const Event& e : rig.events->events) {
        warned = warned || (e.name == "diag" && e.body.get_string("id") == "CML5855" && e.body.get_string("severity") == "warning");
    }
    CHECK(warned);
}

TEST_CASE("revert injection (WP-43): the answers of the agent's stages revert-repoint, revert-restore and revert-uncommit take the revert's error paths") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    apply_two_patches(rig);
    const std::uint64_t live = rig.module->hooked.at(kModuleBase + 0x1010);

    // revert-repoint: `last` re-points compute_value to patch 1; the agent refuses inside its window, nothing changes.
    rig.hooks_status = ipc::fault::ids::kAgentRevertRepoint;
    Result<Value> refused = revert_with(rig, "last");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().id() == "CML5851");
    CHECK(refused.error().message.find("CML4658") != std::string::npos);
    CHECK(rig.module->batches.size() == 2);
    CHECK(rig.module->hooked.at(kModuleBase + 0x1010) == live);
    CHECK(rig.module->generation == 2);
    rig.hooks_status = 0;
    REQUIRE(revert_with(rig, "last").has_value());  // the next revert works
    CHECK(rig.module->batches.size() == 1);

    // revert-restore: `all` restores the original bytes; refused, nothing changes.
    rig.hooks_status = ipc::fault::ids::kAgentRevertRestore;
    refused = revert_with(rig, "all");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().id() == "CML5851");
    CHECK(refused.error().message.find("CML4659") != std::string::npos);
    CHECK(rig.module->batches.size() == 1);
    CHECK(rig.events->count("patch_reverted") == 1);

    // revert-uncommit: the revert goes through, the un-commit failure is the warning CML5855.
    rig.hooks_status = 0;
    rig.uncommit_status = ipc::fault::ids::kAgentRevertUncommit;
    Result<Value> ok = revert_with(rig, "all");
    REQUIRE_MESSAGE(ok.has_value(), ok.error().format());
    CHECK(rig.module->batches.empty());
    CHECK(rig.module->hooked.empty());
    bool warned = false;
    for (const Event& e : rig.events->events) {
        warned = warned || (e.name == "diag" && e.body.get_string("id") == "CML5855" && e.body.get_string("message").find("CML4660") != std::string::npos);
    }
    CHECK(warned);
}

TEST_CASE("revert: a module that is no longer loaded is refused (CML5856) and its stale patch list is dropped") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    apply_two_patches(rig);
    const std::size_t hook_requests = rig.seen<ipc::InstallHooks>().size();
    ipc::ModuleUnloaded unloaded;
    unloaded.module = {kModuleBase, 0x4000, rig.exe};
    rig.session.on_message(unloaded);
    rig.link->publish(rig.session);

    Result<Value> r = revert_with(rig, "all");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5856");
    CHECK(rig.seen<ipc::InstallHooks>().size() == hook_requests);  // nothing was written into whatever is there now
    CHECK(rig.module->batches.empty());
    CHECK(rig.module->hooked.empty());
    // Nothing is left to revert afterwards.
    Result<Value> again = revert_with(rig, "all");
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().id() == "CML5850");
}

TEST_CASE("revert: one deadline for the whole command; a spent deadline stops before a step and changes nothing (CML5858)") {
    QuietLogs quiet;
    {
        Rig rig;
        rig.enable();
        apply_two_patches(rig);
        // revert_module directly with a deadline that has passed: the re-point of the last revert is not even sent.
        RevertOptions options;
        options.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        const std::size_t hook_requests = rig.seen<ipc::InstallHooks>().size();
        Result<RevertReport> r = revert_module(*rig.link, *rig.module, RevertScope::Last, options);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML5858");
        CHECK(rig.seen<ipc::InstallHooks>().size() == hook_requests);
        CHECK(rig.module->batches.size() == 2);
        CHECK(rig.module->generation == 2);
    }
    {
        // The command's timeout_ms caps the wait for an agent that never answers (hooks_timeout_ms alone would be 30 s).
        Rig rig;
        rig.enable();
        rig.changed();
        REQUIRE(rig.reload().has_value());
        rig.answer_hooks = false;
        Value args = Value::object();
        args.set("scope", Value::string("all"));
        args.set("timeout_ms", Value::integer(300));
        const auto start = std::chrono::steady_clock::now();
        Result<Value> r = rig.service->revert(args);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
        CHECK(elapsed < 5000);
        bool unconfirmed = false;
        bool uncommit_out_of_time = false;
        for (const Event& e : rig.events->events) {
            unconfirmed = unconfirmed || (e.name == "diag" && e.body.get_string("id") == "CML5852");
            uncommit_out_of_time = uncommit_out_of_time || (e.name == "diag" && e.body.get_string("id") == "CML5855");
        }
        CHECK(unconfirmed);
        CHECK(uncommit_out_of_time);  // the spent deadline reaches the later steps too
    }
}

TEST_CASE("events: patch_reverted carries pid, module and the generation after the revert") {
    const Event e = make_patch_reverted_event(4711, "app.exe", 3);
    CHECK(e.name == "patch_reverted");
    CHECK(e.body.get_string("event") == "patch_reverted");
    CHECK(e.body.get_integer("pid") == 4711);
    CHECK(e.body.get_string("module") == "app.exe");
    CHECK(e.body.get_integer("generation") == 3);
}

TEST_CASE("revert: in synchronised mode an unconfirmed request is a warning, and the broker's state still goes back") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    REQUIRE(rig.reload().has_value());
    rig.answer_hooks = false;  // the application does not reach SyncPoint()
    rig.option("hooks_timeout_ms", Value::integer(100));
    Result<Value> r = revert_with(rig, "all");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(rig.module->generation == 0);
    CHECK(rig.module->hooked.empty());
    bool warned = false;
    for (const Event& e : rig.events->events) {
        warned = warned || (e.name == "diag" && e.body.get_string("id") == "CML5852");
    }
    CHECK(warned);
}

TEST_CASE("control: revert is advertised and answers over the line protocol; patch_reverted is a known event") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    apply_two_patches(rig);
    ProcessRegistry registry;
    registry.update(rig.session);
    std::set<std::string> subscribed;
    ControlContext context;
    context.version = "0.1.0";
    context.registry = &registry;
    context.service = rig.service.get();
    context.subscribe = [&](const std::set<std::string>& names) { subscribed = names; };
    const std::vector<std::string> caps = broker_capabilities(context);
    CHECK(std::find(caps.begin(), caps.end(), "revert") != caps.end());
    const auto call = [&](const std::string& line) {
        Result<Value> parsed = ipc::json::parse(handle_control_line(line, context));
        REQUIRE(parsed.has_value());
        return *parsed;
    };
    const Value done = call(R"({"id":1,"cmd":"revert","args":{"scope":"last"}})");
    REQUIRE_MESSAGE(done.get_bool("ok"), ipc::json::serialize(done));
    CHECK(done.find("result")->find("reverted")->items()[0].get_integer("generation") == 1);
    CHECK(call(R"({"id":2,"cmd":"status"})").find("result")->get_integer("generation") == 1);
    CHECK(call(R"({"id":3,"cmd":"subscribe","args":{"events":["patch_reverted"]}})").get_bool("ok"));
    CHECK(subscribed == std::set<std::string>{"patch_reverted"});
    const Value bad = call(R"({"id":4,"cmd":"revert","args":{"scope":"x"}})");
    CHECK_FALSE(bad.get_bool("ok"));
    CHECK(bad.find("diag")->get_string("id") == "CML5853");
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
        // WP-17: start, then error with the diagnostic's id and a one-line text for the application's hook.
        REQUIRE(rig.compile_events.size() == 2);
        CHECK(rig.compile_events[0].phase == ipc::CompilePhase::Start);
        CHECK(rig.compile_events[1].phase == ipc::CompilePhase::Error);
        CHECK(rig.compile_events[1].diag == 2200);
        CHECK_FALSE(rig.compile_events[1].text.empty());
        CHECK(rig.compile_events[1].text.find('\n') == std::string::npos);
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

// WP-35: the first compiler error's position comes from the parsed ToolDiagnostic.
TEST_CASE("reload: a compile error carries file, line and column on the diag event and in the control answer (WP-35)") {
    QuietLogs quiet;
    const auto error_at = [](const char* file, int line, int column, toolchain::Severity severity = toolchain::Severity::Error) {
        toolchain::ToolDiagnostic d;
        d.severity = severity;
        d.file = file;
        d.line = line;
        d.column = column;
        d.code = "C2059";
        d.message = "syntax error: ';'";
        return d;
    };
    const auto find_diag = [](Rig& rig) -> Value {
        for (const Event& e : rig.events->events) {
            if (e.name == "diag" && e.body.get_string("id") == "CML2200") {
                return e.body;
            }
        }
        return Value();
    };
    SUBCASE("file, line and column of the FIRST error (a warning before it is skipped)") {
        Rig rig;
        rig.enable();
        rig.changed();
        rig.fail_compile = true;
        rig.compile_errors = {error_at("C:\\proj\\logic.cpp", 3, 1, toolchain::Severity::Warning), error_at("C:\\proj\\logic.cpp", 12, 15),
                              error_at("C:\\proj\\other.cpp", 99, 2)};
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().file == "C:\\proj\\logic.cpp");
        CHECK(r.error().line == 12);
        CHECK(r.error().column == 15);
        const Value event = find_diag(rig);
        REQUIRE(event.is_object());
        CHECK(event.get_string("file") == "C:\\proj\\logic.cpp");
        CHECK(event.get_integer("line") == 12);
        CHECK(event.get_integer("column") == 15);
        // Nothing that existed changed: the text is the same one-line message with the compiler's line appended.
        CHECK(event.get_string("message").find("First error: C:\\proj\\logic.cpp(12,15): error C2059") != std::string::npos);
        // The control answer carries the same fields in `diag`, and the broker advertises the capability.
        ProcessRegistry registry;
        registry.update(rig.session);
        ControlContext context;
        context.version = "0.1.0";
        context.registry = &registry;
        context.service = rig.service.get();
        const std::vector<std::string> caps = broker_capabilities(context);
        CHECK(std::find(caps.begin(), caps.end(), "diag-locations") != caps.end());
        rig.changed();
        Result<Value> answer = ipc::json::parse(handle_control_line(R"({"id":1,"cmd":"reload","args":{"timeout_ms":30000}})", context));
        REQUIRE(answer.has_value());
        REQUIRE_FALSE(answer->get_bool("ok"));
        const Value* diag = answer->find("diag");
        REQUIRE(diag != nullptr);
        CHECK(diag->get_string("id") == "CML2200");
        CHECK(diag->get_string("file") == "C:\\proj\\logic.cpp");
        CHECK(diag->get_integer("line") == 12);
        CHECK(diag->get_integer("column") == 15);
    }
    SUBCASE("cl prints no column: `file` and `line` only") {
        Rig rig;
        rig.enable();
        rig.changed();
        rig.fail_compile = true;
        rig.compile_errors = {error_at("C:\\proj\\logic.cpp", 12, 0)};
        REQUIRE_FALSE(rig.reload().has_value());
        const Value event = find_diag(rig);
        CHECK(event.get_integer("line") == 12);
        CHECK(event.find("column") == nullptr);
    }
    SUBCASE("a relative file name is resolved against the directory the compiler ran in, and normalised") {
        Rig rig;
        rig.enable();
        rig.changed();
        rig.fail_compile = true;
        rig.compile_errors = {error_at("..\\src\\logic.cpp", 7, 0)};
        REQUIRE_FALSE(rig.reload().has_value());
        const Value event = find_diag(rig);
        CHECK(event.get_string("file") == "C:\\proj\\src\\logic.cpp");
        CHECK(event.get_integer("line") == 7);
    }
    SUBCASE("no absolute path can be formed: no location, so the clients parse the text") {
        Rig rig;
        rig.enable();
        rig.changed();
        rig.fail_compile = true;
        rig.compile_cwd.clear();  // the compiler's directory is unknown
        rig.compile_errors = {error_at("src\\logic.cpp", 7, 0)};
        REQUIRE_FALSE(rig.reload().has_value());
        const Value event = find_diag(rig);
        REQUIRE(event.is_object());
        CHECK(event.find("file") == nullptr);
        CHECK(event.find("line") == nullptr);
    }
    SUBCASE("no location when the compiler named none (a command-line error), or no error was parsed") {
        for (int variant = 0; variant < 2; ++variant) {
            Rig rig;
            rig.enable();
            rig.changed();
            rig.fail_compile = true;
            if (variant == 0) {
                toolchain::ToolDiagnostic d;
                d.tool = "cl";
                d.code = "D8003";
                d.message = "missing source filename";
                rig.compile_errors = {d};
            }
            Result<Value> r = rig.reload();
            REQUIRE_FALSE(r.has_value());
            CHECK(r.error().file.empty());
            CHECK(r.error().line == 0);
            const Value event = find_diag(rig);
            REQUIRE(event.is_object());
            CHECK(event.find("file") == nullptr);
            CHECK(event.find("line") == nullptr);
            CHECK(event.find("column") == nullptr);
        }
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

TEST_CASE("WP-50: the module file is read in the background once the index is ready, the first reload uses that copy, and a drop releases it") {
    QuietLogs quiet;
    Rig rig;
    Value on = Value::object();
    on.set("name", Value::string("prefetch_module_image"));
    on.set("value", Value::boolean(true));
    REQUIRE(rig.service->set_option(on).has_value());
    rig.enable();
    std::shared_ptr<const pe::OwnedImage> prefetched;
    for (int i = 0; i < 500 && prefetched == nullptr; ++i) {
        {
            std::lock_guard<std::mutex> lock(rig.module->mutex);
            prefetched = rig.module->image;
        }
        if (prefetched == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    REQUIRE_MESSAGE(prefetched != nullptr, "no background read within 10 s of the index being ready");

    // The reload compares with the copy read in the background: no second read.
    rig.changed();
    REQUIRE(rig.reload().has_value());
    {
        std::lock_guard<std::mutex> lock(rig.module->mutex);
        CHECK(rig.module->image == prefetched);
    }
    const ModuleState::ImageRead again = rig.module->ensure_image(false);
    CHECK_FALSE(again.read_now);
    CHECK(again.image == prefetched);

    // Disabled: the state drops its copy, and a background read that comes late stores nothing.
    const std::shared_ptr<ModuleState> old = rig.module;
    ipc::DisableModule disable;
    disable.request_id = 9;
    disable.path = rig.exe;
    rig.session.on_message(disable);
    rig.link->publish(rig.session);
    rig.service->on_session_changed(1);
    CHECK(rig.service->module_state(1, rig.exe) == nullptr);
    {
        std::lock_guard<std::mutex> lock(old->mutex);
        CHECK(old->image == nullptr);
        CHECK(old->image_released);
    }
    const ModuleState::ImageRead late = old->ensure_image(true);
    CHECK_FALSE(late.read_now);
    CHECK(late.image == nullptr);
    // A reload's own read still works (it compares, but keeps nothing on a released state).
    const ModuleState::ImageRead own = old->ensure_image(false);
    CHECK(own.read_now);
    CHECK(own.image != nullptr);
    {
        std::lock_guard<std::mutex> lock(old->mutex);
        CHECK(old->image == nullptr);
    }
}

TEST_CASE("WP-50: a module file that cannot be read is reported by the read, not stored") {
    ModuleState state;
    state.path = "C:\\no\\such\\dir\\missing_module.exe";
    const ModuleState::ImageRead background = state.ensure_image(true);
    CHECK(background.read_now);
    CHECK(background.image == nullptr);
    CHECK(background.error.has_value());
    CHECK(state.image == nullptr);
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

// ---- failure injection (WP-28, ADR-0035) -----------------------------------------------------------------------

#if defined(CML_FAULT_INJECTION)

namespace {

Value inject_args(const std::string& stage, int count = 0, int pid = 0) {
    Value args = Value::object();
    args.set("stage", Value::string(stage));
    if (count != 0) {
        args.set("count", Value::integer(count));
    }
    if (pid != 0) {
        args.set("pid", Value::integer(pid));
    }
    return args;
}

}  // namespace

TEST_CASE("fault injection: each stage of the broker fails with its own id, undoes nothing it did not do, and the next reload succeeds") {
    QuietLogs quiet;
    struct Case {
        const char* stage;
        const char* id;
        int link_calls;     // linker runs of the failed reload
        bool sent_load;     // LoadPatch reached the agent
    };
    const Case cases[] = {
        {"pdb-load", "CML5963", 0, false},          {"change-detection", "CML5964", 0, false}, {"compile", "CML5965", 0, false},
        {"bind-plan", "CML5966", 0, false},         {"link", "CML5967", 0, false},             {"pre-apply", "CML5968", 1, false},
        {"send-patch", "CML5969", 1, false},
    };
    for (const Case& c : cases) {
        CAPTURE(c.stage);
        Rig rig;
        rig.enable();
        rig.changed();
        Result<Value> armed = inject_failure(*rig.service, inject_args(c.stage));
        REQUIRE_MESSAGE(armed.has_value(), armed.error().format());
        CHECK(armed->get_bool("armed"));
        CHECK(armed->get_string("stage") == c.stage);
        CHECK(armed->get_string("side") == "broker");

        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == c.id);
        CHECK(r.error().message.find(c.stage) != std::string::npos);
        CHECK_FALSE(r.error().what_to_do.empty());
        CHECK(rig.events->diag_ids() == std::vector<std::string>{c.id});
        CHECK(rig.link_calls == c.link_calls);
        CHECK(rig.seen<ipc::LoadPatch>().empty());  // nothing was loaded, so nothing is left in the application
        CHECK(rig.seen<ipc::InstallHooks>().empty());
        CHECK_FALSE(rig.module->is_hooked(kModuleBase + 0x1010));
        CHECK(rig.module->generation == 0);
        CHECK(rig.events->count("patch_applied") == 0);
        CHECK(rig.faults_left(c.stage) == 0);  // used up

        // Without injection the very same reload succeeds.
        Result<Value> again = rig.reload();
        REQUIRE_MESSAGE(again.has_value(), again.error().format());
        CHECK(again->get_integer("patched") == 1);
        CHECK(rig.module->is_hooked(kModuleBase + 0x1010));
        CHECK(rig.module->generation == 1);
        CHECK(rig.events->count("patch_applied") == 1);
    }
}

TEST_CASE("fault injection: the compile stage tells the application's compile-error hook; send-patch leaves the linked DLL unused") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    REQUIRE(inject_failure(*rig.service, inject_args("compile")).has_value());
    REQUIRE_FALSE(rig.reload().has_value());
    REQUIRE(rig.compile_events.size() == 2);
    CHECK(rig.compile_events[0].phase == ipc::CompilePhase::Start);
    CHECK(rig.compile_events[1].phase == ipc::CompilePhase::Error);
    CHECK(rig.compile_events[1].diag == 5965);
    CHECK(rig.compile_calls == 0);  // the failure comes before the compiler runs
}

TEST_CASE("fault injection: count passes fail, then the stage disarms itself; none disarms at once; re-arming replaces") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    Result<Value> armed = inject_failure(*rig.service, inject_args("link", 2));
    REQUIRE(armed.has_value());
    CHECK(armed->get_integer("count") == 2);
    CHECK(rig.service->faults().armed(ipc::fault::Stage::Link) == 2);
    CHECK(rig.reload().error().id() == "CML5967");
    CHECK(rig.service->faults().armed(ipc::fault::Stage::Link) == 1);
    CHECK(rig.reload().error().id() == "CML5967");
    CHECK(rig.reload().has_value());  // used up

    rig.changed();
    REQUIRE(inject_failure(*rig.service, inject_args("compile")).has_value());
    Result<Value> none = inject_failure(*rig.service, inject_args("none"));
    REQUIRE(none.has_value());
    CHECK_FALSE(none->get_bool("armed"));
    CHECK(rig.service->faults().armed(ipc::fault::Stage::Compile) == 0);
    CHECK(rig.reload().has_value());

    REQUIRE(inject_failure(*rig.service, inject_args("link", 5)).has_value());
    REQUIRE(inject_failure(*rig.service, inject_args("link", 1)).has_value());
    CHECK(rig.service->faults().armed(ipc::fault::Stage::Link) == 1);
}

TEST_CASE("fault injection: pid selects the process whose reloads fail") {
    QuietLogs quiet;
    {
        Rig rig;
        rig.enable();
        rig.changed();
        REQUIRE(inject_failure(*rig.service, inject_args("link", 1, static_cast<int>(kPid) + 99)).error().id() == "CML5961");  // no such process
        // Armed for another pid than the one that reloads: no effect on this process.
        REQUIRE(rig.service->faults().arm(ipc::fault::Stage::Link, 1, kPid + 99).has_value());
        CHECK(rig.reload().has_value());
        CHECK(rig.service->faults().armed(ipc::fault::Stage::Link) == 1);  // still armed for the other process
    }
    {
        Rig rig;
        rig.enable();
        rig.changed();
        REQUIRE(inject_failure(*rig.service, inject_args("link", 1, static_cast<int>(kPid))).has_value());
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML5967");
    }
}

TEST_CASE("fault injection: a stage of the agent is armed through ArmFault in every connected agent; a refusal is CML5962") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    for (const char* stage : {"reservation", "patch-load", "hook-install", "post-patch-hooks"}) {
        CAPTURE(stage);
        rig.requests.clear();
        Result<Value> r = inject_failure(*rig.service, inject_args(stage, 3));
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
        CHECK(r->get_string("side") == "agent");
        CHECK(r->find("processes")->items().size() == 1);
        const auto arms = rig.seen<ipc::ArmFault>();
        REQUIRE(arms.size() == 1);
        CHECK(arms[0].stage == stage);
        CHECK(arms[0].count == 3);
    }
    rig.requests.clear();
    REQUIRE(inject_failure(*rig.service, inject_args("none")).has_value());
    const auto disarm = rig.seen<ipc::ArmFault>();
    REQUIRE(disarm.size() == 1);
    CHECK(disarm[0].stage == "none");

    rig.arm_status = 4654;
    Result<Value> refused = inject_failure(*rig.service, inject_args("patch-load"));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().id() == "CML5962");
    CHECK(refused.error().message.find("CML4654") != std::string::npos);
    rig.arm_status = 0;
    CHECK(inject_failure(*rig.service, inject_args("patch-load", 1, static_cast<int>(kPid) + 1)).error().id() == "CML5961");
}

TEST_CASE("fault injection: a bad request is CML5960 and says what the stages are") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    const auto bad = [&](Value args) {
        Result<Value> r = inject_failure(*rig.service, args);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML5960");
        return r.error();
    };
    CHECK(bad(Value::object()).message.find("stage") != std::string::npos);
    CHECK(bad(inject_args("no-such-stage")).what_to_do.find("link") != std::string::npos);
    Value count0 = inject_args("link");
    count0.set("count", Value::integer(0));
    bad(count0);
    Value huge = inject_args("link");
    huge.set("count", Value::integer(100000));
    bad(huge);
    Value pid_text = inject_args("link");
    pid_text.set("pid", Value::string("1"));
    bad(pid_text);
    Value stage_number = Value::object();
    stage_number.set("stage", Value::integer(3));
    bad(stage_number);
    CHECK(rig.service->faults().armed(ipc::fault::Stage::Link) == 0);
    CHECK(rig.requests.empty());  // nothing was sent to the agent
}

TEST_CASE("fault injection: the control capability, fault-stages and inject-failure over the line protocol") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    ControlContext context;
    context.version = "0.1.0";
    context.service = rig.service.get();
    const std::vector<std::string> caps = broker_capabilities(context);
    CHECK(std::find(caps.begin(), caps.end(), "fault-injection") != caps.end());
    ControlContext bare;
    const std::vector<std::string> bare_caps = broker_capabilities(bare);
    CHECK(std::find(bare_caps.begin(), bare_caps.end(), "fault-injection") == bare_caps.end());

    const auto call = [&](const std::string& line) {
        Result<Value> parsed = ipc::json::parse(handle_control_line(line, context));
        REQUIRE(parsed.has_value());
        return *parsed;
    };
    const Value stages = call(R"({"id":1,"cmd":"fault-stages"})");
    REQUIRE_MESSAGE(stages.get_bool("ok"), ipc::json::serialize(stages));
    const Value* list = stages.find("result")->find("stages");
    REQUIRE(list != nullptr);
    std::vector<std::string> names;
    for (const Value& v : list->items()) {
        names.push_back(v.as_string());
    }
    CHECK(names == std::vector<std::string>{"pdb-load", "change-detection", "compile", "bind-plan", "reservation", "link", "pre-apply", "send-patch",
                                            "patch-load", "hook-install", "post-patch-hooks",
                                            // WP-43: the restart's and the revert's stages, after the reload's
                                            "pre-restart-hooks", "restart-terminate", "restart-relaunch", "restart-hold", "restart-registration",
                                            "restart-reenable", "restart-reapply", "revert-repoint", "revert-restore", "revert-uncommit",
                                            // WP-44: the crash handler's and the watcher's
                                            "exception-hold", "exception-report", "exception-continue", "exception-leave", "exception-reexecute",
                                            "watch-debounce", "watch-change-detection", "watch-reload"});
    CHECK(stages.find("result")->find("details")->items().size() == names.size());

    const Value armed = call(R"({"id":2,"cmd":"inject-failure","args":{"stage":"bind-plan"}})");
    REQUIRE_MESSAGE(armed.get_bool("ok"), ipc::json::serialize(armed));
    CHECK(armed.find("result")->get_string("stage") == "bind-plan");
    CHECK(armed.find("result")->get_bool("armed"));
    const Value disarmed = call(R"({"id":3,"cmd":"inject-failure","args":{"stage":"none"}})");
    REQUIRE(disarmed.get_bool("ok"));
    CHECK_FALSE(disarmed.find("result")->get_bool("armed"));
    const Value bad = call(R"({"id":4,"cmd":"inject-failure","args":{"stage":"nope"}})");
    CHECK_FALSE(bad.get_bool("ok"));
    CHECK(bad.find("diag")->get_string("id") == "CML5960");
    // Without a service neither command exists.
    ControlContext without;
    Result<Value> none = ipc::json::parse(handle_control_line(R"({"id":5,"cmd":"inject-failure","args":{"stage":"link"}})", without));
    REQUIRE(none.has_value());
    CHECK(none->find("diag")->get_string("id") == "CML5402");
}

TEST_CASE("fault injection: arming several agents is all or nothing and a refusal names the pid") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.enable2();
    rig.requests.clear();
    rig.arm_status2 = 4654;
    Result<Value> r = inject_failure(*rig.service, inject_args("patch-load", 2));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5962");
    CHECK(r.error().message.find(std::to_string(kPid + 1)) != std::string::npos);
    // The first agent was armed and then disarmed again (count 0 = this stage only).
    const auto arms = rig.seen<ipc::ArmFault>();
    REQUIRE(arms.size() == 3);
    CHECK(arms[0].count == 2);
    CHECK(arms[1].count == 2);  // the refusing agent
    CHECK(arms[2].stage == "patch-load");
    CHECK(arms[2].count == 0);
    rig.arm_status2 = 0;
}

TEST_CASE("fault injection: 'none' with an unknown pid changes nothing; it disarms what it can and reports the agents it could not reach") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    REQUIRE(inject_failure(*rig.service, inject_args("link", 3)).has_value());
    Result<Value> unknown = inject_failure(*rig.service, inject_args("none", 0, static_cast<int>(kPid) + 99));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().id() == "CML5961");
    CHECK(rig.service->faults().armed(ipc::fault::Stage::Link) == 3);  // untouched
    CHECK(rig.seen<ipc::ArmFault>().empty());

    rig.enable2();
    rig.arm_status2 = 4654;  // the second agent cannot be disarmed
    Result<Value> partial = inject_failure(*rig.service, inject_args("none"));
    REQUIRE_FALSE(partial.has_value());
    CHECK(partial.error().id() == "CML5971");
    CHECK(partial.error().message.find(std::to_string(kPid + 1)) != std::string::npos);
    CHECK(rig.service->faults().armed(ipc::fault::Stage::Link) == 0);  // the broker's stages and the first agent were disarmed
    std::size_t nones = 0;
    for (const ipc::ArmFault& a : rig.seen<ipc::ArmFault>()) {
        nones += a.stage == "none" ? 1 : 0;
    }
    CHECK(nones == 2);  // both agents were tried
}

TEST_CASE("fault injection: the reservation stage is refused for a module that reserved already (CML5970)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    REQUIRE(inject_failure(*rig.service, inject_args("reservation")).has_value());  // first patch still ahead
    REQUIRE(inject_failure(*rig.service, inject_args("none")).has_value());
    REQUIRE(rig.reload().has_value());  // reserves
    rig.requests.clear();
    Result<Value> late = inject_failure(*rig.service, inject_args("reservation"));
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error().id() == "CML5970");
    CHECK(late.error().message.find("first patch") != std::string::npos);
    CHECK(rig.seen<ipc::ArmFault>().empty());  // nothing was armed anywhere
}

TEST_CASE("fault injection: a failure after the link gives the address space back (a count that would exhaust the reservation)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    constexpr int kFailures = 250;  // each linked image takes 256 KiB of the 64 MiB block; 4 MiB must stay free: 250 would not fit
    REQUIRE(inject_failure(*rig.service, inject_args("send-patch", kFailures)).has_value());
    for (int i = 0; i < kFailures; ++i) {
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().id() == "CML5969");
    }
    REQUIRE(rig.link_bases.size() == static_cast<std::size_t>(kFailures));
    for (const std::uint64_t base : rig.link_bases) {
        CHECK(base == rig.link_bases.front());  // the same slice every time
    }
    Result<Value> ok = rig.reload();
    REQUIRE_MESSAGE(ok.has_value(), ok.error().format());
    CHECK(rig.seen<ipc::LoadPatch>().front().candidates[0].image_base == rig.link_bases.front());
}

TEST_CASE("fault injection: pre-apply and a refused LoadPatch give the address space back as well; an image that is loaded keeps its slice") {
    QuietLogs quiet;
    {
        Rig rig;
        rig.enable();
        rig.changed();
        REQUIRE(inject_failure(*rig.service, inject_args("pre-apply", 2)).has_value());
        CHECK_FALSE(rig.reload().has_value());
        CHECK_FALSE(rig.reload().has_value());
        REQUIRE(rig.link_bases.size() == 2);
        CHECK(rig.link_bases[0] == rig.link_bases[1]);
    }
    {
        Rig rig;
        rig.enable();
        rig.changed();
        rig.load_status = 4204;  // the agent refuses to load
        CHECK_FALSE(rig.reload().has_value());
        CHECK_FALSE(rig.reload().has_value());
        REQUIRE(rig.link_bases.size() == 2);
        CHECK(rig.link_bases[0] == rig.link_bases[1]);
    }
    {
        Rig rig;
        rig.enable();
        rig.changed();
        REQUIRE(rig.reload().has_value());  // loaded: its slice stays used
        rig.changed();
        rig.object = make_object("", true);  // new code, so the second reload really links
        REQUIRE(rig.reload().has_value());
        REQUIRE(rig.link_bases.size() == 2);
        CHECK(rig.link_bases[1] > rig.link_bases[0]);
    }
}

TEST_CASE("fault injection: when the agent could not take its hooks out again (CML4656) the patch image stays mapped") {
    QuietLogs quiet;
    {
        // The contrast: an ordinary agent failure of InstallHooks unloads the patch.
        Rig rig;
        rig.enable();
        rig.changed();
        rig.hooks_status = ipc::fault::ids::kAgentPostPatchHooks;
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML4653");
        const auto releases = rig.seen<ipc::ReleaseRange>();
        REQUIRE(releases.size() == 1);
        CHECK(releases[0].mode == ipc::ReleaseMode::UnloadPatch);
        CHECK_FALSE(rig.module->is_hooked(kModuleBase + 0x1010));
    }
    {
        Rig rig;
        rig.enable();
        rig.changed();
        rig.hooks_status = ipc::fault::ids::kAgentRollbackIncomplete;
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML4656");
        CHECK(rig.events->diag_ids() == std::vector<std::string>{"CML4656"});
        CHECK(rig.seen<ipc::ReleaseRange>().empty());               // never freed: a live hook may still target it
        CHECK(rig.module->is_hooked(kModuleBase + 0x1010));          // remembered as hooked, like after a timeout
        CHECK(rig.module->batches.size() == 1);                      // recorded as live, like after a timeout, so that a revert knows it
    }
}

TEST_CASE("revert injection (WP-43): an armed revert stage the revert does not reach is disarmed in the agent at its end, never carried over") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    apply_two_patches(rig);
    // revert-restore is armed, but `last` only re-points (both patches hook compute_value): the stage is not reached.
    REQUIRE(inject_failure(*rig.service, inject_args("revert-restore")).has_value());
    Result<Value> last = revert_with(rig, "last");
    REQUIRE_MESSAGE(last.has_value(), last.error().format());
    const Value* disarmed = last->find("injection_disarmed");
    REQUIRE(disarmed != nullptr);
    REQUIRE(disarmed->items().size() == 1);
    CHECK(disarmed->items()[0].as_string() == "revert-restore");
    const auto arms = rig.seen<ipc::ArmFault>();
    REQUIRE(arms.size() == 2);
    CHECK(arms[0].stage == "revert-restore");
    CHECK(arms[0].count == 1);
    CHECK(arms[1].stage == "revert-restore");
    CHECK(arms[1].count == 0);  // disarmed in the agent at the end of the revert
    // A stage that fired is disarmed too, and is not listed as unused.
    REQUIRE(inject_failure(*rig.service, inject_args("revert-restore")).has_value());
    rig.hooks_status = ipc::fault::ids::kAgentRevertRestore;  // the scripted agent's answer when the stage fires
    Result<Value> all = revert_with(rig, "all");
    REQUIRE_FALSE(all.has_value());
    CHECK(all.error().message.find("CML4659") != std::string::npos);
    CHECK(rig.seen<ipc::ArmFault>().size() == 4);
    rig.hooks_status = 0;
    Result<Value> again = revert_with(rig, "all");  // nothing armed any more: no ArmFault, nothing listed
    REQUIRE_MESSAGE(again.has_value(), again.error().format());
    CHECK(again->find("injection_disarmed") == nullptr);
    CHECK(rig.seen<ipc::ArmFault>().size() == 4);
    // A revert stage takes count 1 only.
    Result<Value> two = inject_failure(*rig.service, inject_args("revert-uncommit", 2));
    REQUIRE_FALSE(two.has_value());
    CHECK(two.error().id() == "CML5960");
}

// ---- WP-44 (R-51): races and stale records of WP-43's failure injection, revert side ----------------------------------------------

TEST_CASE("revert injection (R-51 1): while a revert disarms what is left for it, inject-failure is refused busy") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    apply_two_patches(rig);
    REQUIRE(inject_failure(*rig.service, inject_args("revert-restore")).has_value());  // `last` only re-points: not reached
    std::optional<Result<Value>> during_cleanup;
    rig.on_arm = [&](const ipc::ArmFault& arm) {
        if (arm.count == 0 && !during_cleanup) {
            during_cleanup.emplace(inject_failure(*rig.service, inject_args("revert-restore")));
        }
    };
    Result<Value> last = revert_with(rig, "last");
    REQUIRE_MESSAGE(last.has_value(), last.error().format());
    REQUIRE(during_cleanup.has_value());
    REQUIRE_FALSE(during_cleanup->has_value());  // before R-51: armed:true, and then disarmed by the ending revert
    CHECK(during_cleanup->error().id() == "CML5978");
    CHECK_FALSE(rig.service->faults().take_agent_armed(ipc::fault::Stage::RevertRestore, kPid));
}

namespace {

// WP-44: a process host that only answers has_exited (the revert tests do not restart anything).
class ExitHost final : public ProcessHost {
public:
    Result<std::unique_ptr<RestartableProcess>> open(std::uint32_t, const std::uint64_t*, std::uint32_t, std::vector<Diag>*) override {
        return Diag::make(5805, "not in this test", "none");
    }
    bool has_exited(std::uint32_t pid) const override {
        std::lock_guard<std::mutex> lock(mutex);
        return exited.count(pid) != 0;
    }
    mutable std::mutex mutex;
    std::set<std::uint32_t> exited;
};

std::shared_ptr<ExitHost> install_exit_host(Rig& rig) {
    auto host = std::make_shared<ExitHost>();
    rig.service->set_process_host(host);
    rig.service->restart_coordinator_for_testing()->set_exit_recheck_for_testing(std::chrono::milliseconds(30), std::chrono::milliseconds(5));
    return host;
}

}  // namespace

TEST_CASE("revert injection (R-51 3): an agent whose process exited takes its armings along; a later process with its pid reports none") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    const std::shared_ptr<ExitHost> host = install_exit_host(rig);
    REQUIRE(inject_failure(*rig.service, inject_args("revert-repoint")).has_value());
    {
        std::lock_guard<std::mutex> lock(host->mutex);
        host->exited.insert(kPid);
    }
    rig.service->detach(1);
    CHECK(finish_injection(*rig.service, "revert", kPid, std::string()).empty());  // before R-51: {"revert-repoint"}, a phantom
}

TEST_CASE("fault injection (WP-44 fix round): a persisted agent arming survives a graceful broker stop, and a pipe that breaks") {
    QuietLogs quiet;
    testpdb::TempDir logs;
    const std::string state = logs.file("b.log.fault-injection.json");
    {
        Rig rig;
        rig.service->faults().set_state_file(state);
        rig.enable();
        const std::shared_ptr<ExitHost> host = install_exit_host(rig);
        Value keep = inject_args("revert-repoint");
        keep.set("persist", Value::boolean(true));
        REQUIRE(inject_failure(*rig.service, keep).has_value());
        rig.service->detach(1);  // the pipe broke; the process runs on
        CHECK(std::filesystem::exists(state));
        rig.service->attach(rig.link);  // it reconnects
        {
            std::lock_guard<std::mutex> lock(host->mutex);
            host->exited.insert(kPid);  // whatever the host says while the broker goes
        }
        rig.service->cancel();   // the broker stops: cancel, then every connection is detached
        rig.service->detach(1);  // before the fix this dropped the record and rewrote the state file without it
        CHECK(std::filesystem::exists(state));
    }
    Rig restarted;
    restarted.service->faults().set_state_file(state);
    CHECK(restarted.service->faults().load_state().empty());
    CHECK(restarted.service->faults().take_agent_armed(ipc::fault::Stage::RevertRepoint, kPid));  // the next revert still disarms it
}

TEST_CASE("fault injection (WP-44 fix round): none with a pid clears that process only; the others' armings and persisted entries stay") {
    QuietLogs quiet;
    testpdb::TempDir logs;
    const std::string state = logs.file("b.log.fault-injection.json");
    Rig rig;
    rig.service->faults().set_state_file(state);
    rig.enable();
    rig.enable2();
    Value keep = inject_args("revert-repoint");
    keep.set("persist", Value::boolean(true));
    REQUIRE(inject_failure(*rig.service, keep).has_value());  // both processes
    Value other = inject_args("link", 0, static_cast<int>(kPid + 1));
    other.set("persist", Value::boolean(true));
    REQUIRE(inject_failure(*rig.service, other).has_value());  // a broker stage for the second process only
    Value every = inject_args("compile");
    every.set("persist", Value::boolean(true));
    REQUIRE(inject_failure(*rig.service, every).has_value());                      // a broker stage for every process
    REQUIRE(inject_failure(*rig.service, inject_args("watch-reload")).has_value());  // and one of the watcher (never per process)
    Value own = inject_args("bind-plan", 0, static_cast<int>(kPid));
    own.set("persist", Value::boolean(true));
    REQUIRE(inject_failure(*rig.service, own).has_value());  // one armed for kPid itself
    REQUIRE(inject_failure(*rig.service, inject_args("none", 0, static_cast<int>(kPid))).has_value());
    CHECK(rig.faults_left("bind-plan") == 0);     // armed for kPid: goes
    CHECK(rig.faults_left("compile") == 1);       // without a pid: stays (re-review: it was cleared, the answer named only kPid)
    CHECK(rig.faults_left("watch-reload") == 1);  // the watcher's: stays
    CHECK(rig.faults_left("link") == 1);          // armed for the other process: stays
    CHECK_FALSE(rig.service->faults().take_agent_armed(ipc::fault::Stage::RevertRepoint, kPid));
    // The other process's record and persisted entries are still there (before the fix every record and the whole file went).
    Rig restarted;
    restarted.service->faults().set_state_file(state);
    CHECK(restarted.service->faults().load_state().empty());
    CHECK(restarted.faults_left("link") == 1);
    CHECK(restarted.faults_left("compile") == 1);
    CHECK(restarted.faults_left("bind-plan") == 0);
    CHECK(restarted.service->faults().take_agent_armed(ipc::fault::Stage::RevertRepoint, kPid + 1));
    CHECK_FALSE(restarted.service->faults().take_agent_armed(ipc::fault::Stage::RevertRepoint, kPid));
    CHECK(rig.service->faults().take_agent_armed(ipc::fault::Stage::RevertRepoint, kPid + 1));
}

TEST_CASE("fault injection (WP-44 fix round): persist is refused for an agent stage the broker keeps nothing for") {
    QuietLogs quiet;
    testpdb::TempDir logs;
    Rig rig;
    rig.service->faults().set_state_file(logs.file("b.log.fault-injection.json"));
    rig.enable();
    for (const char* stage : {"patch-load", "exception-hold"}) {
        CAPTURE(stage);
        Value keep = inject_args(stage);
        keep.set("persist", Value::boolean(true));
        Result<Value> refused = inject_failure(*rig.service, keep);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().id() == "CML5960");
    }
    CHECK(rig.seen<ipc::ArmFault>().empty());  // nothing was armed
}


TEST_CASE("revert injection (R-51 4): a revert across processes decides per process which agent stage fired") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.enable2();
    rig.changed_on(*rig.module);
    rig.changed_on(*rig.module2);
    REQUIRE(rig.reload().has_value());
    REQUIRE(inject_failure(*rig.service, inject_args("revert-restore")).has_value());  // armed in both agents
    rig.hooks_status = ipc::fault::ids::kAgentRevertRestore;  // it fires in the first process ...
    rig.hooks_status2 = 0;                                     // ... and is not reached in the second (its revert succeeds)
    Result<Value> r = revert_with(rig, "all");
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->get_bool("partial"));
    const Value* disarmed = r->find("injection_disarmed");
    REQUIRE(disarmed != nullptr);  // before R-51 the first process's CML4659 counted as fired for both: nothing was listed
    REQUIRE(disarmed->items().size() == 1);
    CHECK(disarmed->items()[0].as_string() == "revert-restore");
    CHECK_FALSE(rig.service->faults().take_agent_armed(ipc::fault::Stage::RevertRestore, kPid));
    CHECK_FALSE(rig.service->faults().take_agent_armed(ipc::fault::Stage::RevertRestore, kPid + 1));
}

// ---- WP-44: persistent arming ----------------------------------------------------------------------------------------------------

TEST_CASE("fault injection (WP-44): an arming with persist survives a broker restart; one without does not; it fires once") {
    QuietLogs quiet;
    testpdb::TempDir logs;
    const std::string state = logs.file("broker.log.fault-injection.json");
    {
        Rig first;
        first.service->faults().set_state_file(state);
        first.enable();
        Value keep = inject_args("link");
        keep.set("persist", Value::boolean(true));
        Result<Value> armed = inject_failure(*first.service, keep);
        REQUIRE_MESSAGE(armed.has_value(), armed.error().format());
        CHECK(armed->get_bool("persist"));
        REQUIRE(inject_failure(*first.service, inject_args("compile")).has_value());  // not kept
        CHECK(std::filesystem::exists(state));
    }  // the broker exits
    {
        Rig second;  // a new broker with the same log
        second.service->faults().set_state_file(state);
        CHECK(second.service->faults().load_state().empty());
        CHECK(second.faults_left("link") == 1);
        CHECK(second.faults_left("compile") == 0);
        second.enable();
        second.changed();
        Result<Value> failed = second.reload();
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().id() == "CML5967");  // link, not compile
        CHECK_FALSE(std::filesystem::exists(state));  // it fired: nothing is kept any more
        second.changed();
        Result<Value> again = second.reload();
        REQUIRE_MESSAGE(again.has_value(), again.error().format());
    }
    {
        Rig third;  // and a third broker finds nothing
        third.service->faults().set_state_file(state);
        CHECK(third.service->faults().load_state().empty());
        CHECK(third.faults_left("link") == 0);
    }
}

TEST_CASE("fault injection (WP-44): none clears the kept armings; an agent stage's record is kept; without a log file persist is CML5984") {
    QuietLogs quiet;
    testpdb::TempDir logs;
    const std::string state = logs.file("b.log.fault-injection.json");
    {
        Rig rig;
        rig.enable();
        Value keep = inject_args("link");
        keep.set("persist", Value::boolean(true));
        Result<Value> refused = inject_failure(*rig.service, keep);  // no state file: the broker writes no log
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().id() == "CML5984");
        CHECK(rig.faults_left("link") == 0);
        Value bad = inject_args("link");
        bad.set("persist", Value::string("yes"));
        CHECK(inject_failure(*rig.service, bad).error().id() == "CML5960");

        rig.service->faults().set_state_file(state);
        REQUIRE(inject_failure(*rig.service, keep).has_value());
        Value agent_stage = inject_args("revert-repoint");
        agent_stage.set("persist", Value::boolean(true));
        REQUIRE(inject_failure(*rig.service, agent_stage).has_value());
        CHECK(std::filesystem::exists(state));
    }
    {
        Rig rig;
        rig.service->faults().set_state_file(state);
        CHECK(rig.service->faults().load_state().empty());
        CHECK(rig.faults_left("link") == 1);
        // The agent arming lives in the agent process; the broker remembers that the end of the next revert must disarm it there.
        CHECK(rig.service->faults().take_agent_armed(ipc::fault::Stage::RevertRepoint, kPid));
        rig.service->faults().note_agent_armed(ipc::fault::Stage::RevertRepoint, kPid, true);
        REQUIRE(inject_failure(*rig.service, inject_args("none")).has_value());
        CHECK(rig.faults_left("link") == 0);
        CHECK_FALSE(std::filesystem::exists(state));
    }
    {
        Rig rig;
        rig.service->faults().set_state_file(state);
        CHECK(rig.service->faults().load_state().empty());
        CHECK(rig.faults_left("link") == 0);
        CHECK_FALSE(rig.service->faults().take_agent_armed(ipc::fault::Stage::RevertRepoint, kPid));
    }
}

TEST_CASE("fault injection (WP-44): a state file that cannot be used arms nothing, is put aside and is reported (CML5984)") {
    QuietLogs quiet;
    testpdb::TempDir logs;
    const std::string state = logs.file("b.log.fault-injection.json");
    for (const char* text : {"not json", R"({"version":2,"broker":[]})", R"({"version":1,"broker":[{"stage":"patch-load","count":1}]})",
                             R"({"version":1,"broker":[{"stage":"restart-hold","count":2}]})", R"({"version":1,"agent":[{"stage":"link","pid":5}]})",
                             R"({"version":1,"broker":[{"stage":"watch-reload","count":1,"pid":5}]})",
                             R"({"version":1,"agent":[{"stage":"exception-hold","pid":5}]})",    // no record is kept for this group
                             R"({"version":1,"agent":[{"stage":"patch-load","pid":5}]})"}) {
        CAPTURE(text);
        {
            std::ofstream out(state, std::ios::binary | std::ios::trunc);
            out << text;
        }
        FaultInjector faults;
        faults.set_state_file(state);
        const std::vector<Diag> problems = faults.load_state();
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].id() == "CML5984");
        CHECK(problems[0].message.find(state) != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(state));
        CHECK(std::filesystem::exists(state + ".bad"));
        for (const ipc::fault::StageInfo& s : ipc::fault::kStages) {
            CHECK(faults.armed(s.stage) == 0);
        }
    }
    // A count of a reload stage is kept as what is left of it.
    FaultInjector faults;
    faults.set_state_file(state);
    REQUIRE(faults.arm(ipc::fault::Stage::Compile, 3, 77u, true).has_value());
    CHECK(faults.hit(ipc::fault::Stage::Compile, 77).has_value());
    FaultInjector later;
    later.set_state_file(state);
    CHECK(later.load_state().empty());
    CHECK(later.armed(ipc::fault::Stage::Compile) == 2);
    CHECK_FALSE(later.hit(ipc::fault::Stage::Compile, 78).has_value());  // still for process 77 only
    CHECK(later.hit(ipc::fault::Stage::Compile, 77).has_value());
}

#endif  // CML_FAULT_INJECTION

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
    CHECK(subscribed.size() == 4);  // diag, patch_applied, patch_reverted (WP-22), log
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
    // WP-25: reloading identical code loads nothing; this test is about the linker of every reload, so hook everything.
    rig.option("narrow_hooks", Value::boolean(false));
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

TEST_CASE("reload_for_save: busy and not-ready are quiet; a ready module is compiled (WP-14)") {
    QuietLogs quiet;
    {
        // No module at all: nothing to do, and no CML5300 event.
        Rig rig;
        CHECK(rig.service->reload_for_save() == SaveWatcherHooks::ReloadResult::Done);
        CHECK(rig.events->diag_ids().empty());
    }
    {
        // A module whose index failed is skipped without a failed reload.
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
        CHECK(rig.service->reload_for_save() == SaveWatcherHooks::ReloadResult::Done);
        CHECK(rig.events->diag_ids().empty());
        CHECK(rig.compile_calls == 0);
    }
    {
        // A reload is running: the automatic one says Busy and publishes nothing.
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
        CHECK(rig.service->reload_for_save() == SaveWatcherHooks::ReloadResult::Busy);
        CHECK(rig.events->diag_ids().empty());
        {
            std::lock_guard<std::mutex> lock(m);
            release = true;
        }
        cv.notify_all();
        worker.join();
        CHECK(first.has_value());
    }
    {
        // A ready module with a change is compiled.
        Rig rig;
        rig.enable();
        rig.changed();
        CHECK(rig.service->reload_for_save() == SaveWatcherHooks::ReloadResult::Done);
        CHECK(rig.compile_calls >= 1);
    }
}

// ---- WP-15: header dependency handling ----------------------------------------------------------------------------

TEST_CASE("reload: a changed file of a precompiled header is refused with CML5550 before anything is compiled") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.module->index->set_sweep_override_for_testing([]() -> Result<ModuleIndex::Sweep> {
        ModuleIndex::Sweep s;
        const std::string pch = "C:\\proj\\pch.h";
        s.changed = {pch};
        s.compilands = {0, 1};
        s.file_compilands[pch] = {0, 1};
        s.pch_headers = {pch};
        s.stamps[pch] = FileStamp{1, 2, 3};
        return s;
    });
    Result<Value> r = rig.reload();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5550");
    CHECK(r.error().message.find("pch.h") != std::string::npos);
    CHECK(rig.events->diag_ids() == std::vector<std::string>{"CML5550"});
    CHECK(rig.compile_calls == 0);
    CHECK(rig.requests.empty());
    CHECK(rig.module->index->accepted_for_testing().empty());  // still "changed": reverting it makes the next reload a no-op
}

TEST_CASE("reload: more affected compilands than max_affected_compilands needs a confirmation (CML5551)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed_both();
    rig.second_object = make_object({}, false, "no_such_function_anywhere");  // stops after the compile stage; the cap is what is tested
    SUBCASE("above the limit and not confirmed: nothing is compiled and the count is named") {
        rig.option("max_affected_compilands", Value::integer(1));
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML5551");
        CHECK(r.error().message.find("2 compilands") != std::string::npos);
        CHECK(r.error().what_to_do.find("confirm") != std::string::npos);
        CHECK(rig.compile_calls == 0);
        CHECK(rig.requests.empty());
    }
    SUBCASE("confirmed: the reload proceeds") {
        rig.option("max_affected_compilands", Value::integer(1));
        Value args = Value::object();
        args.set("confirm", Value::boolean(true));
        Result<Value> r = rig.service->reload(args);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML3200");  // got as far as planning
        CHECK(rig.compile_calls >= 1);
    }
    SUBCASE("at the limit no confirmation is needed") {
        rig.option("max_affected_compilands", Value::integer(2));
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML3200");
    }
    SUBCASE("with the limit off no confirmation is needed") {
        rig.option("max_affected_compilands", Value::integer(0));
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML3200");
    }
    SUBCASE("'confirm' must be a boolean") {
        Value args = Value::object();
        args.set("confirm", Value::string("yes"));
        Result<Value> r = rig.service->reload(args);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML5311");
    }
}

TEST_CASE("reload: one compile call for all units; progress is reported per finished unit") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed_both();
    rig.second_object = make_object({}, false, "no_such_function_anywhere");
    Result<Value> r = rig.reload();
    REQUIRE_FALSE(r.has_value());
    CHECK(rig.events->count("progress") == 3);  // 0/2, 1/2, 2/2
}

TEST_CASE("reload: a compile driver that returns too few results fails the missing units (CML5318)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed_both();
    rig.short_results = true;
    Result<Value> r = rig.reload();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().id() == "CML5318");
    CHECK(r.error().message.find("main.cpp") != std::string::npos);
    CHECK(rig.requests.empty());
}

TEST_CASE("reload: a /Yu compiland whose precompiled header cannot be determined is refused (CML5552), whatever the scan finds") {
    QuietLogs quiet;
    SUBCASE("/Yu without a header name: no file to track, no .pch known to be current") {
        Rig rig(false, "/Yu");
        rig.enable();
        rig.changed();
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML5552");
        CHECK(r.error().message.find("precompiled header") != std::string::npos);
        CHECK(rig.compile_calls == 0);
    }
    SUBCASE("/Yu naming a header that is not on disk is refused as well") {
        Rig rig(false, "/Yupch.h");
        rig.enable();
        rig.changed();
        Result<Value> r = rig.reload();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML5552");
        CHECK(rig.compile_calls == 0);
    }
    SUBCASE("a TU without a precompiled header is not affected") {
        Rig rig;
        rig.enable();
        rig.changed();
        Result<Value> r = rig.reload();
        CHECK(r.has_value());
    }
}

// ---- WP-16: one compile per distinct command line, link and load per module ------------------------------------------------

TEST_CASE("reload result: compiler runs, reused results, relinks and the modules / processes that were patched") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    rig.load_collisions = 1;
    Result<Value> r = rig.reload();
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->get_integer("patched") == 1);
    CHECK(r->get_integer("compiled") == 1);
    CHECK(r->get_integer("compile_reused") == 0);
    CHECK(r->get_integer("load_attempts") == 2);  // one patch, one relink at another base
    CHECK(r->get_integer("modules_patched") == 1);
    CHECK(r->get_integer("processes_patched") == 1);
    CHECK(rig.compile_calls == 1);

    // A reload that changes nothing compiles and loads nothing.
    rig.unchanged();
    Result<Value> idle = rig.reload();
    REQUIRE_MESSAGE(idle.has_value(), idle.error().format());
    CHECK(idle->get_integer("compiled") == 0);
    CHECK(idle->get_integer("load_attempts") == 0);
    CHECK(idle->get_integer("modules_patched") == 0);
    CHECK(idle->get_integer("processes_patched") == 0);
}

TEST_CASE("compile cache: the same command line is compiled once for the modules of one reload; the next reload compiles again") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    // WP-25: reloading identical code loads nothing; this test is about the compile cache, so hook everything.
    rig.option("narrow_hooks", Value::boolean(false));
    const ReloadOptions options = rig.service->options();
    PipelineContext ctx;
    ctx.session_dir = rig.service->session_dir();
    ctx.options = &options;
    ctx.events = &rig.bus;
    ctx.exports = std::make_shared<ExportCache>();
    ctx.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    ctx.compile_cache = std::make_shared<CompileCache>();
    PatchPipeline pipeline(rig.pipeline_env);

    // The first run compiles; a second run of the same command line (another process would do this) takes the result.
    rig.changed();
    Result<PatchReport> first = pipeline.patch_module(*rig.link, *rig.module, ctx);
    REQUIRE_MESSAGE(first.has_value(), first.error().format());
    CHECK(first->compiled == 1);
    CHECK(first->compile_reused == 0);
    rig.changed();
    Result<PatchReport> second = pipeline.patch_module(*rig.link, *rig.module, ctx);
    REQUIRE_MESSAGE(second.has_value(), second.error().format());
    CHECK(second->compiled == 0);
    CHECK(second->compile_reused == 1);
    CHECK(rig.compile_calls == 1);
    CHECK(rig.seen<ipc::LoadPatch>().size() == 2);  // loading is per module: both runs linked and loaded their own patch
    CHECK(rig.link_calls == 2);

    // A new reload has a new cache.
    ctx.compile_cache = std::make_shared<CompileCache>();
    rig.changed();
    Result<PatchReport> third = pipeline.patch_module(*rig.link, *rig.module, ctx);
    REQUIRE_MESSAGE(third.has_value(), third.error().format());
    CHECK(third->compiled == 1);
    CHECK(rig.compile_calls == 2);
}

TEST_CASE("compile cache: a failed compile is shared too and every module reports it") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.fail_compile = true;
    const ReloadOptions options = rig.service->options();
    PipelineContext ctx;
    ctx.session_dir = rig.service->session_dir();
    ctx.options = &options;
    ctx.events = &rig.bus;
    ctx.exports = std::make_shared<ExportCache>();
    ctx.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    ctx.compile_cache = std::make_shared<CompileCache>();
    PatchPipeline pipeline(rig.pipeline_env);
    rig.changed();
    Result<PatchReport> first = pipeline.patch_module(*rig.link, *rig.module, ctx);
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().code == 2200);
    rig.changed();
    Result<PatchReport> second = pipeline.patch_module(*rig.link, *rig.module, ctx);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == 2200);
    CHECK(rig.compile_calls == 1);  // the compiler ran once
}

TEST_CASE("compile cache: the key follows everything that decides what the compiler is asked to do") {
    toolchain::CompileRecord a;
    a.cwd = "C:\\proj";
    a.compiler = "C:\\VC\\cl.exe";
    a.args = {"/c", "/Zi", "x.cpp"};
    a.source = "C:\\proj\\x.cpp";
    toolchain::CompileRecord same = a;
    CHECK(CompileCache::key_of(a) == CompileCache::key_of(same));
    toolchain::CompileRecord other_args = a;
    other_args.args.push_back("/DFOO");
    CHECK(CompileCache::key_of(a) != CompileCache::key_of(other_args));
    toolchain::CompileRecord other_cwd = a;
    other_cwd.cwd = "C:\\other";
    CHECK(CompileCache::key_of(a) != CompileCache::key_of(other_cwd));
    toolchain::CompileRecord other_source = a;
    other_source.source = "C:\\proj\\y.cpp";
    CHECK(CompileCache::key_of(a) != CompileCache::key_of(other_source));
    toolchain::CompileRecord other_compiler = a;
    other_compiler.compiler = "C:\\llvm\\clang-cl.exe";
    CHECK(CompileCache::key_of(a) != CompileCache::key_of(other_compiler));
    // The argument boundaries count: {"/D", "FOO"} is not {"/DFOO"}.
    toolchain::CompileRecord split = a;
    split.args = {"/c", "/Zi", "/D", "FOO"};
    toolchain::CompileRecord joined = a;
    joined.args = {"/c", "/Zi", "/DFOO"};
    CHECK(CompileCache::key_of(split) != CompileCache::key_of(joined));

    CompileCache cache;
    CHECK_FALSE(cache.find("k1").has_value());
    CompileCache::Entry entry;
    entry.outcome.object_path = "C:\\o.obj";
    cache.put("k1", entry);
    REQUIRE(cache.find("k1").has_value());
    CHECK(cache.find("k1")->outcome.object_path == "C:\\o.obj");
    CHECK_FALSE(cache.find("k2").has_value());
}

TEST_CASE("compile cache: a reused result replays the compiler's warnings for the second module") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.compile_warning = true;
    const ReloadOptions options = rig.service->options();
    PipelineContext ctx;
    ctx.session_dir = rig.service->session_dir();
    ctx.options = &options;
    ctx.events = &rig.bus;
    ctx.exports = std::make_shared<ExportCache>();
    ctx.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    ctx.compile_cache = std::make_shared<CompileCache>();
    PatchPipeline pipeline(rig.pipeline_env);
    rig.changed();
    Result<PatchReport> first = pipeline.patch_module(*rig.link, *rig.module, ctx);
    REQUIRE_MESSAGE(first.has_value(), first.error().format());
    rig.changed();
    Result<PatchReport> second = pipeline.patch_module(*rig.link, *rig.module, ctx);
    REQUIRE_MESSAGE(second.has_value(), second.error().format());
    CHECK(second->compile_reused == 1);
    const auto count_2201 = [](const PatchReport& r) {
        int n = 0;
        for (const Diag& w : r.warnings) {
            n += w.code == 2201 ? 1 : 0;
        }
        return n;
    };
    CHECK(count_2201(*first) == 1);
    CHECK(count_2201(*second) == 1);
    CHECK(rig.compile_calls == 1);
}

TEST_CASE("compile objects go below a per-process directory, so two processes never share an object path") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    Result<Value> r = rig.reload();
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    REQUIRE(rig.compile_outputs.size() == 1);
    const std::string marker = "/app.exe/p" + std::to_string(kPid) + "/obj";
    std::string normalised = rig.compile_outputs[0];
    for (char& c : normalised) {
        c = c == '\\' ? '/' : c;
    }
    CHECK_MESSAGE(normalised.find(marker) != std::string::npos, normalised);
}

TEST_CASE("compile cache: a cached failure stops a module before it compiles anything else") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.fail_compile = true;
    const ReloadOptions options = rig.service->options();
    PipelineContext ctx;
    ctx.session_dir = rig.service->session_dir();
    ctx.options = &options;
    ctx.events = &rig.bus;
    ctx.exports = std::make_shared<ExportCache>();
    ctx.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    ctx.compile_cache = std::make_shared<CompileCache>();
    PatchPipeline pipeline(rig.pipeline_env);
    rig.changed_both();  // compilands 0 and 1: both fail on the first run
    REQUIRE_FALSE(pipeline.patch_module(*rig.link, *rig.module, ctx).has_value());
    const int calls = rig.compile_calls;
    rig.changed_both();
    Result<PatchReport> second = pipeline.patch_module(*rig.link, *rig.module, ctx);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == 2200);
    CHECK(rig.compile_calls == calls);
}

TEST_CASE("reload_for_save: a change above max_affected_compilands (CML5551) is reported once and nothing is compiled or confirmed (WP-14 + WP-15)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.option("max_affected_compilands", Value::integer(1));
    rig.changed_both();  // two compilands
    const int calls = rig.compile_calls;
    CHECK(rig.service->reload_for_save() == SaveWatcherHooks::ReloadResult::Done);
    CHECK(rig.compile_calls == calls);
    CHECK(rig.events->diag_ids() == std::vector<std::string>{"CML5551"});
    // The explicit reload still refuses, and "confirm" still lets it through.
    Result<Value> refused = rig.reload();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().id() == "CML5551");
    Value args = Value::object();
    args.set("confirm", Value::boolean(true));
    CHECK(rig.service->reload(args).has_value());
}

#if defined(_WIN32)
TEST_CASE("compile on save watches the headers that only the include scan finds (WP-14 + WP-15)") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const auto write_text = [](const std::string& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    };
    std::filesystem::create_directories(std::filesystem::path(dir.dir()) / "inc");
    write_text(dir.file("logic.cpp"), "#include \"inc/dep.h\"\nint compute_value() { return VALUE; }\n");
    write_text(dir.file("inc\\dep.h"), "#pragma once\n#define VALUE 1\n");
    SynthWorld w;
    w.tu().symbols.envblock({{"cwd", dir.dir()}, {"cl", "C:\\VC\\bin\\cl.exe"}, {"cmd", "/c /Zi /Gy logic.cpp"}, {"src", dir.file("logic.cpp")}});
    w.proc(w.tu(), "compute_value", 0x10);
    w.pub_func("?compute_value@@YAHXZ", 0x10);
    w.spec.guid = kGuid;
    w.spec.age = 5;
    const std::string pdb = dir.file("app.pdb");
    testpdb::write_file(pdb, testpdb::build(w.spec));
    const std::string exe = dir.file("app.exe");
    testpdb::write_file(exe, testpdb::build_pe(kGuid, 5, "C:\\build\\app.pdb"));

    ModuleIndexSpec spec;
    spec.module_path = exe;
    spec.pdb_path = pdb;
    std::shared_ptr<ModuleIndex> index = ModuleIndex::start(spec);
    REQUIRE(index->wait(30000));
    REQUIRE(index->status().state == IndexState::Ready);
    CHECK(index->scanned_headers() >= 1);

    const std::string header = pdb::normalize_path(dir.file("inc\\dep.h"));
    const std::vector<std::string> tracked = index->tracked_files();
    CHECK(std::find(tracked.begin(), tracked.end(), header) != tracked.end());
    // ... so its directory is one of the watched roots.
    const std::vector<watch::WatchRoot> roots = watch::plan_watch_roots(tracked);
    bool watched = false;
    for (const watch::WatchRoot& r : roots) {
        watched = watched || r.directory == pdb::normalize_path(dir.file("inc"));
    }
    CHECK(watched);
}
#endif

// ---- WP-25: unchanged-function elision ---------------------------------------------------------------------------------

namespace {

// The diag events of one severity and id the rig's listener has seen.
int count_diag(Rig& rig, const std::string& id, const std::string& severity) {
    std::lock_guard<std::mutex> lock(rig.events->mutex);
    int n = 0;
    for (const Event& e : rig.events->events) {
        if (e.name == "diag" && e.body.get_string("id") == id && e.body.get_string("severity") == severity) {
            ++n;
        }
    }
    return n;
}

}  // namespace

TEST_CASE("elision: a reload that recompiles to identical code loads nothing, advances nothing and warns CML3450") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    Result<Value> first = rig.reload();
    REQUIRE_MESSAGE(first.has_value(), first.error().format());
    CHECK(first->get_integer("hooks_planned") == 1);
    CHECK(first->get_integer("hooks_dropped") == 0);
    CHECK(first->get_integer("hooks_installed") == 1);
    CHECK_FALSE(first->get_bool("no_functional_change"));
    CHECK(count_diag(rig, "CML3450", "warning") == 0);
    REQUIRE(rig.module->generation == 1);

    // The same object again: compute_value is hooked by patch 1 and equals what patch 1 put there.
    rig.changed();
    Result<Value> second = rig.reload();
    REQUIRE_MESSAGE(second.has_value(), second.error().format());
    CHECK(second->get_integer("patched") == 0);
    CHECK(second->get_integer("hooks_planned") == 1);
    CHECK(second->get_integer("hooks_dropped") == 1);
    CHECK(second->get_integer("hooks_installed") == 0);
    CHECK(second->get_integer("modules_unchanged") == 1);
    CHECK(second->get_bool("no_functional_change"));
    CHECK(second->get_integer("generation") == 1);
    CHECK(rig.module->generation == 1);
    CHECK(rig.service->generation() == 1);
    // Nothing reached the agent for the second reload: one load, one hook request, no unload.
    CHECK(rig.seen<ipc::LoadPatch>().size() == 1);
    CHECK(rig.seen<ipc::InstallHooks>().size() == 1);
    CHECK(rig.seen<ipc::ReleaseRange>().empty());
    CHECK(rig.link_calls == 1);
    // The warning is in the response and a diag event; no patch_applied event for the empty reload.
    const Value* module_report = second->find("patches");  // the warnings are per module report (no second copy at the top)
    REQUIRE(module_report != nullptr);
    REQUIRE(module_report->items().size() == 1);
    CHECK(second->find("warnings") == nullptr);
    const Value* warnings = module_report->items()[0].find("warnings");
    REQUIRE(warnings != nullptr);
    bool in_response = false;
    for (const Value& w : warnings->items()) {
        in_response = in_response || (w.as_string().find("CML3450") != std::string::npos && w.as_string().find("identical code") != std::string::npos);
    }
    CHECK(in_response);
    CHECK(count_diag(rig, "CML3450", "warning") == 1);
    CHECK(rig.events->count("patch_applied") == 1);
    CHECK(rig.events->diag_ids().empty());  // a warning, not an error
    // The source baseline moved on, so the next sweep does not report the file again.
    CHECK(rig.module->index->accepted_for_testing().size() == 2);
}

TEST_CASE("elision: a partly unchanged reload reports how many hooks were dropped") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    REQUIRE(rig.reload().has_value());
    // Patch 2: compute_value is unchanged, but the object now also defines a new function, so it is loaded without a hook.
    rig.object = make_object({}, true);
    rig.changed();
    Result<Value> second = rig.reload();
    REQUIRE_MESSAGE(second.has_value(), second.error().format());
    CHECK(second->get_integer("patched") == 1);
    CHECK(second->get_integer("hooks_planned") == 1);
    CHECK(second->get_integer("hooks_dropped") == 1);
    CHECK(second->get_integer("hooks_installed") == 0);
    CHECK_FALSE(second->get_bool("no_functional_change"));
    CHECK(count_diag(rig, "CML3450", "warning") == 0);
    const auto patch_events = [&] {
        std::lock_guard<std::mutex> lock(rig.events->mutex);
        std::vector<Value> v;
        for (const Event& e : rig.events->events) {
            if (e.name == "patch_applied") {
                v.push_back(e.body);
            }
        }
        return v;
    }();
    REQUIRE(patch_events.size() == 2);
    CHECK(patch_events[1].get_integer("hooks_planned") == 1);
    CHECK(patch_events[1].get_integer("hooks_dropped") == 1);
    CHECK(patch_events[1].get_integer("hooks") == 0);
}

TEST_CASE("elision: a function that changed since the earlier patch is hooked again, one that did not is not") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    REQUIRE(rig.reload().has_value());
    // Patch 2 calls a function patch 1 did not know: other relocations, so compute_value differs from patch 1's code.
    rig.object = make_object({}, true);
    rig.changed();
    REQUIRE(rig.reload().has_value());  // defines new_helper (loaded, compute_value dropped)
    rig.object = make_object("?new_helper@@YAHXZ");
    rig.changed();
    Result<Value> third = rig.reload();
    REQUIRE_MESSAGE(third.has_value(), third.error().format());
    CHECK(third->get_integer("hooks_planned") == 1);
    CHECK(third->get_integer("hooks_dropped") == 0);
    CHECK(third->get_integer("hooks_installed") == 1);
    CHECK_FALSE(third->get_bool("no_functional_change"));
}

TEST_CASE("elision: narrow_hooks false hooks everything again, and a no-op reload then still loads") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    REQUIRE(rig.reload().has_value());
    rig.option("narrow_hooks", Value::boolean(false));
    rig.changed();
    Result<Value> second = rig.reload();
    REQUIRE_MESSAGE(second.has_value(), second.error().format());
    CHECK(second->get_integer("patched") == 1);
    CHECK(second->get_integer("hooks_dropped") == 0);
    CHECK(second->get_integer("hooks_installed") == 1);
    CHECK_FALSE(second->get_bool("no_functional_change"));
}

// ---- WP-25 review items ------------------------------------------------------------------------------------------------

namespace {

// The module file the rig's pipeline compares with: the synthetic PE instead of the minimal one, with 8 zero bytes where the
// PDB puts g_const (RVA 0x2030, inside .rdata's raw data).
void use_rich_module_file(Rig& rig) {
    cml_test::PeBytes pe = cml_test::build_pe_on_disk();
    for (std::size_t i = 0; i < 8; ++i) {
        pe[cml_test::rdata_off(0x2030) + i] = 0;
    }
    std::ofstream out(rig.exe, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(pe.data()), static_cast<std::streamsize>(pe.size()));
    out.close();
    std::lock_guard<std::mutex> lock(rig.module->mutex);
    rig.module->image.reset();  // the pipeline reads the file again
}

Value module_report(const Value& result) { return result.find("patches")->items().at(0); }

}  // namespace

TEST_CASE("elision: identical code but a changed initial value of data in the running image warns CML3451, not CML3450, and loads nothing") {
    QuietLogs quiet;
    Rig rig(false, std::string(), true);
    rig.enable();
    use_rich_module_file(rig);
    rig.changed();
    REQUIRE(rig.reload().has_value());  // patch 1: compute_value is hooked
    REQUIRE(rig.module->generation == 1);

    SUBCASE("the data differs from the old image's") {
        rig.object = make_object({}, false, {}, 0x2A);
        rig.changed();
        Result<Value> second = rig.reload();
        REQUIRE_MESSAGE(second.has_value(), second.error().format());
        const Value report = module_report(*second);
        CHECK(report.get_bool("data_only_change"));
        CHECK_FALSE(report.get_bool("no_functional_change"));
        CHECK_FALSE(second->get_bool("no_functional_change"));
        CHECK(second->get_integer("patched") == 0);
        CHECK(rig.module->generation == 1);
        CHECK(rig.seen<ipc::LoadPatch>().size() == 1);
        CHECK(count_diag(rig, "CML3451", "warning") == 1);
        CHECK(count_diag(rig, "CML3450", "warning") == 0);
        const std::string text = report.find("warnings")->items().at(0).as_string();
        CHECK(text.find("g_const") != std::string::npos);
        CHECK(text.find("do not run again") != std::string::npos);
    }
    SUBCASE("the data equals the old image's: plain CML3450") {
        rig.object = make_object({}, false, {}, 0);
        rig.changed();
        Result<Value> second = rig.reload();
        REQUIRE_MESSAGE(second.has_value(), second.error().format());
        CHECK(second->get_bool("no_functional_change"));
        CHECK_FALSE(module_report(*second).get_bool("data_only_change"));
        CHECK(count_diag(rig, "CML3450", "warning") == 1);
        CHECK(count_diag(rig, "CML3451", "warning") == 0);
    }
}

TEST_CASE("elision: hooks_kept leaves out 'narrowing off' and the counts add up") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.option("narrow_hooks", Value::boolean(false));
    rig.changed();
    Result<Value> r = rig.reload();
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    const Value report = module_report(*r);
    CHECK(report.find("hooks_kept")->items().empty());
    CHECK(report.get_integer("hooks_planned") - report.get_integer("hooks_dropped") == report.get_integer("hooks_installed"));
}

TEST_CASE("elision: code records follow the hooks: reverted patches are pruned") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    rig.changed();
    REQUIRE(rig.reload().has_value());  // patch 1 hooks compute_value: one record
    {
        std::lock_guard<std::mutex> lock(rig.module->mutex);
        CHECK(rig.module->code.size() == 1);
    }
    // Patch 2: compute_value is unchanged (dropped), it only introduces new_helper, which gets a record of its own.
    rig.object = make_object({}, true);
    rig.changed();
    REQUIRE(rig.reload().has_value());
    {
        std::lock_guard<std::mutex> lock(rig.module->mutex);
        CHECK(rig.module->code.size() == 2);
    }
    Result<Value> reverted = revert_with(rig, "last");
    REQUIRE_MESSAGE(reverted.has_value(), reverted.error().format());
    {
        std::lock_guard<std::mutex> lock(rig.module->mutex);
        CHECK(rig.module->code.size() == 1);  // new_helper is no longer canonical: its record is gone
    }
    REQUIRE(revert_with(rig, "all").has_value());
    {
        std::lock_guard<std::mutex> lock(rig.module->mutex);
        CHECK(rig.module->code.empty());
    }
}

TEST_CASE("elision: prune_code keeps what the hook table reaches and the canonical new functions, and nothing else") {
    ModuleState m;
    const auto record = [](const char* symbol) {
        auto image = std::make_shared<CodeImage>();
        image->symbol = symbol;
        return std::shared_ptr<const CodeImage>(image);
    };
    m.record_code(0x1000, record("?hooked@@YAXXZ"));      // the target of a hook
    m.record_code(0x2000, record("?superseded@@YAXXZ"));  // an older hook target
    m.record_code(0x3000, record("?fresh@@YAXXZ"));       // a function a patch introduced, canonical in the registry
    m.record_code(0x4000, record("?gone@@YAXXZ"));        // introduced by a patch that was reverted (not in the registry)
    m.record_code(0x5000, record("?moved@@YAXXZ"));       // introduced and canonical, but hooked since: the hook target's record applies
    m.record_code(0x6000, record("?moved@@YAXXZ"));
    m.hooked[0xA000] = 0x1000;
    m.hooked[0x5000] = 0x6000;
    SymbolAddress a;
    a.va = 0x3000;
    a.is_function = true;
    m.registry.replace("?fresh@@YAXXZ", a);
    SymbolAddress b;
    b.va = 0x5000;
    b.is_function = true;
    m.registry.replace("?moved@@YAXXZ", b);
    m.prune_code();
    CHECK(m.code.count(0x1000) == 1);
    CHECK(m.code.count(0x2000) == 0);
    CHECK(m.code.count(0x3000) == 1);
    CHECK(m.code.count(0x4000) == 0);
    CHECK(m.code.count(0x5000) == 0);
    CHECK(m.code.count(0x6000) == 1);
    CHECK(m.code_running_at(0xA000) != nullptr);
}

// ---- WP-26b: a changed file that only skipped compilands own (CML5921) -------------------------------------------------------

namespace {

// A sweep that reports `file` as owned only by skipped compilands until the baseline accepted its stamp (what the real baseline does),
// plus, with `with_compiland`, a change that compiland 0 owns.
void not_owned_sweep(Rig& rig, const std::string& file, bool with_compiland) {
    ModuleIndex* index = rig.module->index.get();
    index->set_sweep_override_for_testing([index, file, with_compiland]() -> Result<ModuleIndex::Sweep> {
        ModuleIndex::Sweep s;
        for (const auto& accepted : index->accepted_for_testing()) {
            if (accepted.count(file) != 0) {
                return s;  // reported before: not changed any more
            }
        }
        s.changed = {file};
        s.not_owned.push_back({file, "member.obj in game.lib (no build environment recorded)"});
        s.stamps[file] = FileStamp{1, 2, 3};
        if (with_compiland) {
            const std::string logic = "C:\\proj\\logic.cpp";
            s.changed.push_back(logic);
            s.compilands = {0};
            s.file_compilands[logic] = {0};
            s.stamps[logic] = FileStamp{4, 5, 6};
        }
        s.checked = 1;
        s.hashed = 1;
        return s;
    });
}

}  // namespace

TEST_CASE("static libraries (WP-26b): CML5921 is raised once for an edit, on every path that ends the reload") {
    QuietLogs quiet;
    const std::string file = "C:\\proj\\lib_member.cpp";
    SUBCASE("a sweep whose only change is the not-owned file: nothing to compile, still reported once") {
        Rig rig;
        rig.enable();
        not_owned_sweep(rig, file, false);
        Result<Value> first = rig.reload();
        REQUIRE_MESSAGE(first.has_value(), first.error().format());
        CHECK(count_diag(rig, "CML5921", "warning") == 1);
        Result<Value> second = rig.reload();
        REQUIRE_MESSAGE(second.has_value(), second.error().format());
        CHECK(count_diag(rig, "CML5921", "warning") == 1);
    }
    SUBCASE("a reload that fails after the sweep still accepted the not-owned file: the retry does not repeat the warning") {
        Rig rig;
        rig.enable();
        not_owned_sweep(rig, file, true);
        rig.fail_compile = true;
        Result<Value> failed = rig.reload();
        REQUIRE_FALSE(failed.has_value());
        CHECK(count_diag(rig, "CML5921", "warning") == 1);
        // Only the not-owned file was accepted; the compile failure left logic.cpp changed.
        const auto accepted = rig.module->index->accepted_for_testing();
        REQUIRE(accepted.size() == 1);
        CHECK(accepted[0].count(file) == 1);
        CHECK(accepted[0].count("C:\\proj\\logic.cpp") == 0);
        rig.fail_compile = false;
        Result<Value> retry = rig.reload();
        (void)retry;
        CHECK(count_diag(rig, "CML5921", "warning") == 1);
    }
}

// ---- WP-45: the session's edits are re-applied to a module instance that is enabled later ------------------------------------
// These need a source file the module index tracks: pdb::normalize_path makes every path a Windows one, so only on Windows.

#if defined(_WIN32)
namespace {

// A process host that cannot restart anything and reports every process as exited (WP-45: detach drops the carries then).
class ExitedHost final : public ProcessHost {
public:
    Result<std::unique_ptr<RestartableProcess>> open(std::uint32_t, const std::uint64_t[3], std::uint32_t, std::vector<Diag>*) override {
        return Diag::make(5805, "not in this test", "x");
    }
    bool has_exited(std::uint32_t) const override { return true; }
};

// A message of the agent of the rig's first process, then what the connection thread does after it.
void agent_says(Rig& rig, const ipc::Message& m) {
    rig.session.on_message(m);
    rig.link->publish(rig.session);
    rig.service->on_session_changed(1);
}

void unload_exe(Rig& rig, std::uint64_t base = kModuleBase) {
    ipc::ModuleUnloaded unloaded;
    unloaded.module = {base, 0x4000, rig.exe};
    agent_says(rig, unloaded);
}

void load_exe(Rig& rig, std::uint64_t base = kModuleBase) {
    ipc::ModuleLoaded loaded;
    loaded.module = {base, 0x4000, rig.exe};
    agent_says(rig, loaded);
}

// `wait idle`: indexes loaded, no reload and no re-apply queued or running.
void wait_idle(Rig& rig) {
    Value args = Value::object();
    args.set("for", Value::string("idle"));
    args.set("timeout_ms", Value::integer(30000));
    Result<Value> waited = rig.service->wait(args);
    REQUIRE_MESSAGE(waited.has_value(), waited.error().format());
    CHECK(rig.service->reapply_pending() == 0);
}

std::shared_ptr<ModuleState> current_state(Rig& rig) {
    std::shared_ptr<ModuleState> state = rig.service->module_state(1, rig.exe);
    REQUIRE(state != nullptr);
    return state;
}

std::uint64_t generation_of(ModuleState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.generation;
}

// The messages of the diag events with this id.
std::vector<std::string> diag_messages(Rig& rig, const std::string& id) {
    std::lock_guard<std::mutex> lock(rig.events->mutex);
    std::vector<std::string> out;
    for (const Event& e : rig.events->events) {
        if (e.name == "diag" && e.body.get_string("id") == id) {
            out.push_back(e.body.get_string("message"));
        }
    }
    return out;
}

std::size_t links_made(Rig& rig) {
    std::lock_guard<std::mutex> lock(rig.mutex);
    return rig.link_dirs.size();
}

std::string link_dir(Rig& rig, std::size_t i) {
    std::lock_guard<std::mutex> lock(rig.mutex);
    return rig.link_dirs.at(i);
}

}  // namespace

TEST_CASE("WP-45: a module freed and loaded again gets the session's edits re-applied; its patches go to a work directory of its own") {
    QuietLogs quiet;
    Rig rig(false, std::string(), false, true);
    rig.enable();
    rig.write_logic(22);
    Result<Value> first = rig.reload();
    REQUIRE_MESSAGE(first.has_value(), first.error().format());
    REQUIRE(first->get_integer("patched") == 1);
    REQUIRE(links_made(rig) == 1);
    CHECK(rig.compile_calls == 1);
    const std::string pid_dir = "p" + std::to_string(kPid);

    // FreeLibrary: the state goes, the baseline with the accepted edit stays for the process (F-42.1).
    unload_exe(rig);
    CHECK(rig.service->module_state(1, rig.exe) == nullptr);
    bool live = true;
    CHECK(rig.service->has_carry_for_testing(kPid, rig.exe, &live));
    CHECK_FALSE(live);

    // LoadLibrary at the same base: a new instance, which runs the original code. Its enable re-applies the edit by itself.
    load_exe(rig);
    wait_idle(rig);
    const std::shared_ptr<ModuleState> again = current_state(rig);
    CHECK(again->instance == 2);
    CHECK(again->index->carried_files() == 1);
    CHECK(generation_of(*again) == 1);
    CHECK(rig.compile_calls == 2);
    CHECK(rig.seen<ipc::InstallHooks>().size() == 2);
    CHECK_FALSE(rig.service->has_carry_for_testing(kPid, rig.exe));
    REQUIRE(links_made(rig) == 2);
    // F-42.2: never the directory of the freed instance's patch DLLs (they stay loaded in the process).
    CHECK(link_dir(rig, 1) != link_dir(rig, 0));
    CHECK(link_dir(rig, 0).find(pid_dir + "-i") == std::string::npos);
    CHECK(link_dir(rig, 1).find(pid_dir + "-i2") != std::string::npos);
    CHECK(diag_messages(rig, "CML5600").empty());

    // The next edit links in the new instance's directory too. (The fake compiler makes the same object for every edit: without
    // narrowing it is loaded anyway instead of being elided as identical code, CML3450.)
    rig.option("narrow_hooks", Value::boolean(false));
    rig.write_logic(333);
    Result<Value> second = rig.reload();
    REQUIRE_MESSAGE(second.has_value(), second.error().format());
    CHECK(second->get_integer("patched") == 1);
    REQUIRE(links_made(rig) == 3);
    CHECK(link_dir(rig, 2).find(pid_dir + "-i2") != std::string::npos);

    // A third instance, at another base this time: the baseline (both edits accepted) goes along again, to a third directory.
    unload_exe(rig);
    load_exe(rig, kModuleBase + 0x100000);
    wait_idle(rig);
    const std::shared_ptr<ModuleState> third = current_state(rig);
    CHECK(third->instance == 3);
    CHECK(third->base == kModuleBase + 0x100000);
    CHECK(generation_of(*third) == 1);
    REQUIRE(links_made(rig) == 4);
    CHECK(link_dir(rig, 3).find(pid_dir + "-i3") != std::string::npos);
}

TEST_CASE("WP-45: disabled and enabled again while its image stays loaded, a module keeps its baseline and nothing is recompiled") {
    QuietLogs quiet;
    Rig rig(false, std::string(), false, true);
    rig.enable();
    rig.write_logic(22);
    REQUIRE(rig.reload().has_value());
    REQUIRE(rig.compile_calls == 1);

    ipc::DisableModule disable;
    disable.request_id = 20;
    disable.path = rig.exe;
    agent_says(rig, disable);
    bool live = false;
    CHECK(rig.service->has_carry_for_testing(kPid, rig.exe, &live));
    CHECK(live);

    ipc::EnableModule enable;
    enable.request_id = 21;
    enable.path = rig.exe;
    agent_says(rig, enable);
    wait_idle(rig);
    const std::shared_ptr<ModuleState> again = current_state(rig);
    CHECK(again->instance == 2);
    CHECK(again->index->carried_files() == 1);
    CHECK(rig.compile_calls == 1);  // the image still runs the patch: nothing to re-apply
    Result<Value> nothing = rig.reload();
    REQUIRE_MESSAGE(nothing.has_value(), nothing.error().format());
    CHECK(nothing->get_integer("patched") == 0);
    CHECK(rig.compile_calls == 1);

    // Disabled, then freed and loaded again: the image the baseline was kept for is gone, so the next instance re-applies.
    disable.request_id = 22;
    agent_says(rig, disable);
    unload_exe(rig);
    CHECK(rig.service->has_carry_for_testing(kPid, rig.exe, &live));
    CHECK_FALSE(live);
    load_exe(rig);
    CHECK(rig.service->module_state(1, rig.exe) == nullptr);  // not enabled: nothing happens yet
    enable.request_id = 23;
    agent_says(rig, enable);
    wait_idle(rig);
    CHECK(rig.compile_calls == 2);
    CHECK(generation_of(*current_state(rig)) == 1);
}

TEST_CASE("WP-45: a re-apply that fails is CML5600 naming the module and the file; the edit stays pending for the next reload") {
    QuietLogs quiet;
    Rig rig(false, std::string(), false, true);
    rig.enable();
    rig.write_logic(22);
    REQUIRE(rig.reload().has_value());
    unload_exe(rig);
    rig.fail_compile = true;
    load_exe(rig);
    wait_idle(rig);
    const std::vector<std::string> failed = diag_messages(rig, "CML5600");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].find("app.exe") != std::string::npos);
    CHECK(failed[0].find("logic.cpp") != std::string::npos);
    CHECK(failed[0].find("runs its original code") != std::string::npos);
    CHECK(generation_of(*current_state(rig)) == 0);

    rig.fail_compile = false;
    Result<Value> retry = rig.reload();
    REQUIRE_MESSAGE(retry.has_value(), retry.error().format());
    CHECK(retry->get_integer("patched") == 1);
    CHECK(generation_of(*current_state(rig)) == 1);
}

TEST_CASE("WP-45: a module whose index fails cannot get its edits re-applied: CML5601 names the module and the file") {
    QuietLogs quiet;
    Rig rig(false, std::string(), false, true);
    rig.enable();
    rig.write_logic(22);
    REQUIRE(rig.reload().has_value());
    unload_exe(rig);
    std::error_code removed;
    std::filesystem::remove(rig.dir.file("app.pdb"), removed);  // the new instance's index cannot open its PDB
    REQUIRE_MESSAGE(!removed, removed.message());
    load_exe(rig);
    wait_idle(rig);
    const std::vector<std::string> failed = diag_messages(rig, "CML5601");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].find("app.exe") != std::string::npos);
    CHECK(failed[0].find("logic.cpp") != std::string::npos);
}

TEST_CASE("WP-45: a baseline a restart hands over is re-applied when the module is enabled later, or at once when it just was") {
    QuietLogs quiet;
    Rig rig(false, std::string(), false, true);
    FileStamp original;
    REQUIRE(stamp_file(rig.logic_src, &original));
    rig.write_logic(22);  // the edit the previous process had applied, still on disk

    // The carried baseline, keyed like the index's (taken from an index of the same module, before the edit).
    BaselineSnapshot carried;
    {
        ModuleIndexSpec spec;
        spec.module_path = rig.exe;
        std::shared_ptr<ModuleIndex> probe = ModuleIndex::start(spec);
        REQUIRE(probe->wait(30000));
        REQUIRE(probe->status().state == IndexState::Ready);
        const BaselineSnapshot now = probe->baseline_snapshot();
        REQUIRE(now.stamps.size() == 1);
        carried.originals[now.stamps.begin()->first] = original;
        carried.stamps[now.stamps.begin()->first] = original;
    }

    SUBCASE("the module is not enabled yet (CML5809): its enable re-applies the edit") {
        rig.hello();  // the new process has registered (the restart hands over only then)
        rig.service->carry_to_late_module(1, kPid, rig.exe, carried, "restart");
        CHECK(rig.service->has_carry_for_testing(kPid, rig.exe));
        rig.enable();
        wait_idle(rig);
        CHECK(rig.compile_calls == 1);
        CHECK(generation_of(*current_state(rig)) == 1);
        CHECK(current_state(rig)->index->carried_files() == 1);
    }
    SUBCASE("the module was enabled just before the baseline arrived: the worker restores it and reloads") {
        rig.enable();
        Result<Value> nothing = rig.reload();  // its index took the edited file as the baseline: nothing to do (F-42.3)
        REQUIRE(nothing.has_value());
        CHECK(nothing->get_integer("patched") == 0);
        rig.service->carry_to_late_module(1, kPid, rig.exe, carried, "restart");
        CHECK_FALSE(rig.service->has_carry_for_testing(kPid, rig.exe));
        wait_idle(rig);
        CHECK(rig.compile_calls == 1);
        CHECK(generation_of(*current_state(rig)) == 1);
    }
    SUBCASE("a baseline without accepted edits is no carry") {
        rig.service->carry_to_late_module(1, kPid, rig.exe, BaselineSnapshot{}, "restart");
        CHECK_FALSE(rig.service->has_carry_for_testing(kPid, rig.exe));
    }
    SUBCASE("a kept baseline goes with its process") {
        rig.hello();
        rig.service->carry_to_late_module(1, kPid, rig.exe, carried, "restart");
        rig.enable();
        wait_idle(rig);
        unload_exe(rig);
        CHECK(rig.service->has_carry_for_testing(kPid, rig.exe));
        CHECK(rig.service->kept_baselines_for_testing(kPid).size() == 1);
        rig.service->set_process_host(std::make_shared<ExitedHost>());  // the process has exited when its agent disconnects
        rig.service->detach(1);
        CHECK_FALSE(rig.service->has_carry_for_testing(kPid, rig.exe));
    }
}

TEST_CASE("WP-45: a reload asked for while a re-apply runs waits for it instead of being refused busy") {
    QuietLogs quiet;
    Rig rig(false, std::string(), false, true);
    rig.enable();
    rig.write_logic(22);
    REQUIRE(rig.reload().has_value());
    unload_exe(rig);
    std::atomic<bool> release{false};
    rig.compile_hook = [&] {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    load_exe(rig);
    while (rig.compile_calls.load() < 2) {  // the re-apply is compiling
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        release = true;
    });
    Result<Value> user = rig.reload();
    releaser.join();
    REQUIRE_MESSAGE(user.has_value(), user.error().format());
    CHECK(user->get_integer("patched") == 0);  // the re-apply did it
    CHECK(diag_messages(rig, "CML5304").empty());
    wait_idle(rig);
}
#endif  // _WIN32

TEST_CASE("WP-45: SourceBaseline::adopt takes stamps and originals over as they are; restore marks the originals changed") {
    testpdb::TempDir dir;
    const std::string file = dir.file("a.cpp");
    {
        std::ofstream out(file, std::ios::binary);
        out << "int a() { return 2; }\n";
    }
    FileStamp now;
    REQUIRE(stamp_file(file, &now));
    BaselineSnapshot snap;
    snap.stamps[file] = now;                                // the patched content's stamp
    snap.originals[file] = FileStamp{0x11, now.size, 5};    // what the binary was built from
    snap.originals["untracked.cpp"] = FileStamp{1, 2, 3};
    {
        SourceBaseline b;
        b.take({file}, BaselineOptions{}, nullptr);
        CHECK(b.adopt(snap) == 1);
        const BaselineSnapshot after = b.session_snapshot();
        CHECK(after.stamps.at(file).hash == now.hash);
        CHECK(after.originals.at(file).hash == 0x11);
        CHECK(after.originals.count("untracked.cpp") == 0);
        CHECK(b.sweep().changed.empty());  // the image runs this content: nothing to do
    }
    {
        SourceBaseline b;
        b.take({file}, BaselineOptions{}, nullptr);
        CHECK(b.restore(snap) == 1);
        CHECK(b.sweep().changed == std::vector<std::string>{file});  // differs from the original: it is re-applied
    }
}

// ---- WP-45, review round 1 ----------------------------------------------------------------------------------------------------

namespace {

BaselineSnapshot edited(const std::string& file, std::uint64_t original_hash) {
    BaselineSnapshot b;
    b.originals[file] = FileStamp{original_hash, 10, 1};
    b.stamps[file] = FileStamp{original_hash + 1, 12, 2};
    return b;
}

void say(Rig& rig, const ipc::Message& m) {
    rig.session.on_message(m);
    rig.link->publish(rig.session);
    rig.service->on_session_changed(1);
}

std::size_t count_diag_id(Rig& rig, const std::string& id) {
    std::lock_guard<std::mutex> lock(rig.events->mutex);
    std::size_t n = 0;
    for (const Event& e : rig.events->events) {
        n += (e.name == "diag" && e.body.get_string("id") == id) ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("WP-45 review 6: a carry stored while a dropped instance's baseline was being taken is merged with it, not replaced") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    // The exe's instance was given a baseline (a.cpp) to re-apply.
    {
        std::lock_guard<std::mutex> lock(rig.module->mutex);
        rig.module->carried_in = edited("c:\\proj\\a.cpp", 0x10);
    }
    // In the moment the service's lock is free during the drop (the state has left the map, its baseline is not kept yet), a restart
    // hands over b.cpp for the same process and module: carry_to_late_module finds no state and stores it.
    bool handed = false;
    rig.service->set_drop_hook_for_testing([&] {
        handed = rig.service->carry_to_late_module(1, kPid, rig.exe, edited("c:\\proj\\b.cpp", 0x20), "restart");
    });
    ipc::ModuleUnloaded unloaded;
    unloaded.module = {kModuleBase, 0x4000, rig.exe};
    say(rig, unloaded);
    rig.service->set_drop_hook_for_testing({});
    CHECK(handed);
    const auto kept = rig.service->kept_baselines_for_testing(kPid);
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].second.originals.count("c:\\proj\\a.cpp") == 1);  // the dropped instance's
    CHECK(kept[0].second.originals.count("c:\\proj\\b.cpp") == 1);  // and the restart's
    bool live = true;
    CHECK(rig.service->has_carry_for_testing(kPid, rig.exe, &live));
    CHECK_FALSE(live);
}
TEST_CASE("WP-45 review 2: an instance that goes away before its re-apply ran is abandoned, not reported (no CML5601)") {
    QuietLogs quiet;
    Rig rig;
    rig.enable();
    const std::string gone = "C:\\app\\gone.dll";  // no PDB behind it: its index fails
    // The worker is busy with an earlier item that waits for the run lock (held here).
    std::unique_ptr<ExclusiveRun> held = rig.service->lock_reloads();
    REQUIRE(held != nullptr);
    rig.service->carry_to_late_module(1, kPid, rig.exe, edited("c:\\proj\\x.cpp", 0x30), "restart");
    rig.service->carry_to_late_module(1, kPid, gone, edited("c:\\proj\\gone.cpp", 0x40), "restart");
    ipc::ModuleLoaded loaded;
    loaded.module = {0x180000000ull, 0x10000, gone};
    say(rig, loaded);
    ipc::EnableModule enable;
    enable.request_id = 30;
    enable.path = gone;
    say(rig, enable);
    REQUIRE(rig.service->module_state(1, gone) != nullptr);
    CHECK(rig.service->reapply_pending() == 2);
    ipc::ModuleUnloaded unloaded;
    unloaded.module = loaded.module;
    say(rig, unloaded);  // gone.dll is freed again before the worker got to it
    CHECK(rig.service->module_state(1, gone) == nullptr);
    held.reset();
    for (int i = 0; i < 1000 && rig.service->reapply_pending() != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(rig.service->reapply_pending() == 0);
    CHECK(count_diag_id(rig, "CML5601") == 0);
    CHECK(count_diag_id(rig, "CML5600") == 0);
    // Its carry was handed on (to its next instance), not lost.
    CHECK(rig.service->has_carry_for_testing(kPid, gone));
}

namespace {

// A process host that can tell nothing: no restart, no exit, no creation time (ProcessHost's defaults).
class BlindHost final : public ProcessHost {
public:
    Result<std::unique_ptr<RestartableProcess>> open(std::uint32_t, const std::uint64_t[3], std::uint32_t, std::vector<Diag>*) override {
        return Diag::make(5805, "not in this test", "x");
    }
};

}  // namespace

TEST_CASE("WP-45 final review 2: no carry is kept for an agent that is gone or has not registered") {
    QuietLogs quiet;
    Rig rig;
    rig.service->set_process_host(std::make_shared<BlindHost>());  // creation times cannot be read (also on Windows)
    // The agent is gone (never attached, or detached since).
    rig.service->carry_to_late_module(99, kPid, rig.exe, edited("c:\\proj\\a.cpp", 0x10), "restart");
    CHECK_FALSE(rig.service->has_carry_for_testing(kPid, rig.exe));
    // Attached, but no Hello yet: no process to keep it for.
    rig.service->carry_to_late_module(1, kPid, rig.exe, edited("c:\\proj\\a.cpp", 0x10), "restart");
    CHECK_FALSE(rig.service->has_carry_for_testing(kPid, rig.exe));
    // Registered: kept (here without a process host, so its creation time is unknown) ...
    rig.hello();
    rig.service->carry_to_late_module(1, kPid, rig.exe, edited("c:\\proj\\a.cpp", 0x10), "restart");
    CHECK(rig.service->has_carry_for_testing(kPid, rig.exe));
    // ... and a carry whose process identity is not proven is dropped when the next agent registers with that pid.
    auto next = std::make_shared<AgentLink>(4, [](const ipc::Message&) { return Result<void>(); });
    rig.service->attach(next);
    Session next_session{4, "0.1.0"};
    ipc::Hello hello;
    hello.request_id = 1;
    hello.pid = kPid;
    next_session.on_message(hello);
    next->publish(next_session);
    rig.service->on_session_changed(4);
    CHECK_FALSE(rig.service->has_carry_for_testing(kPid, rig.exe));
}

TEST_CASE("WP-45 final ruling 1: a carry a stale agent leaves after another agent registered with its pid is never taken") {
    QuietLogs quiet;
    Rig rig;
    rig.service->set_process_host(std::make_shared<BlindHost>());  // creation times cannot be read: no identity is proven
    rig.enable();
    {
        std::lock_guard<std::mutex> lock(rig.module->mutex);
        rig.module->carried_in = edited("c:\\proj\\a.cpp", 0x10);
    }
    // A new process got the pid (the old one is still in teardown; its agent's connection has not closed yet).
    auto next = std::make_shared<AgentLink>(3, [](const ipc::Message&) { return Result<void>(); });
    rig.service->attach(next);
    Session next_session{3, "0.1.0"};
    ipc::Hello hello;
    hello.request_id = 1;
    hello.pid = kPid;
    next_session.on_message(hello);
    next->publish(next_session);
    rig.service->on_session_changed(3);
    // The old agent's queued ModuleUnloaded arrives now: what its instance leaves is not kept.
    ipc::ModuleUnloaded unloaded;
    unloaded.module = {kModuleBase, 0x4000, rig.exe};
    say(rig, unloaded);
    CHECK_FALSE(rig.service->has_carry_for_testing(kPid, rig.exe));
    // A late hand-over through the old agent is refused (and says so: false), as is one to an agent of another process.
    CHECK_FALSE(rig.service->carry_to_late_module(1, kPid, rig.exe, edited("c:\\proj\\b.cpp", 0x20), "restart"));
    CHECK_FALSE(rig.service->has_carry_for_testing(kPid, rig.exe));
    // The new process's own enable of the exe takes nothing.
    ipc::ModuleList list;
    list.request_id = 2;
    list.modules.push_back({kModuleBase, 0x4000, rig.exe});
    next_session.on_message(list);
    ipc::EnableModule enable;
    enable.request_id = 3;
    enable.path = rig.exe;
    next_session.on_message(enable);
    next->publish(next_session);
    rig.service->on_session_changed(3);
    const std::shared_ptr<ModuleState> fresh = rig.service->module_state(3, rig.exe);
    REQUIRE(fresh != nullptr);
    {
        std::lock_guard<std::mutex> lock(fresh->mutex);
        CHECK_FALSE(fresh->carried_in.has_value());
    }
    CHECK(rig.service->reapply_pending() == 0);
    // Its own hand-overs are taken.
    CHECK(rig.service->carry_to_late_module(3, kPid, "C:\\app\\later.dll", edited("c:\\proj\\later.cpp", 0x30), "restart"));
    CHECK(rig.service->has_carry_for_testing(kPid, "C:\\app\\later.dll"));
}


#if defined(_WIN32)
TEST_CASE("WP-45 review 7: a reload that applied the restored edits before the worker got the run lock is not repeated") {
    QuietLogs quiet;
    Rig rig(false, std::string(), false, true);
    rig.enable();
    rig.write_logic(22);
    REQUIRE(rig.reload().has_value());
    REQUIRE(rig.compile_calls == 1);
    unload_exe(rig);
    std::unique_ptr<ExclusiveRun> held = rig.service->lock_reloads();  // the worker will wait for it
    REQUIRE(held != nullptr);
    load_exe(rig);
    const std::shared_ptr<ModuleState> again = current_state(rig);
    REQUIRE(again->index->wait(30000));
    // A reload (here under the held lock, as a save or another command would get it first) applies the restored edit ...
    Value args = Value::object();
    args.set("timeout_ms", Value::integer(30000));
    Result<Value> first = rig.service->reload_locked(args);
    REQUIRE_MESSAGE(first.has_value(), first.error().format());
    CHECK(first->get_integer("patched") == 1);
    held.reset();
    wait_idle(rig);
    // ... and the worker sees the generation moved and does not reload again.
    CHECK(rig.compile_calls == 2);
    CHECK(generation_of(*again) == 1);
    CHECK(count_diag_id(rig, "CML5600") == 0);
}

TEST_CASE("WP-45 review 4: a broker that shuts down while the re-apply worker reloads returns promptly, without a CML5600") {
    QuietLogs quiet;
    Rig rig(false, std::string(), false, true);
    rig.enable();
    rig.write_logic(22);
    REQUIRE(rig.reload().has_value());
    unload_exe(rig);
    rig.block_until_cancel = true;  // the re-apply's compile runs until the broker cancels it
    load_exe(rig);
    for (int i = 0; i < 3000 && rig.compile_calls.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(rig.compile_calls.load() == 2);
    const auto start = std::chrono::steady_clock::now();
    rig.service.reset();  // the destructor: stop flag, cancel, join
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(took < 10000);
    CHECK(count_diag_id(rig, "CML5600") == 0);
}
#endif  // _WIN32
