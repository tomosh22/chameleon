// WP-63 (R-69, ADR-0047): which statics an inline function's external guard guards, read from the patch's code.
//
// An inline function's statics and their guard (clang-cl's and cl's bitmask `??_B<scope>@5<n>` without thread-safe statics,
// the external `?$TSS<n>@<block>@4HA` of one static with them) are external. A constant-initialised static defines the same
// symbols as a dynamic one, so WP-59 (ADR-0046) refused (CML3600) an inline function that keeps `static int calls = 0;` and
// gains a dynamic static: its guard's name ties it to calls (bound) and to the new static. guard_code.cpp reads which
// statics are initialised under the guard: the bit a store sets (or the _Init_thread_header / _Init_thread_footer bracket)
// and the one static the code behind it refers to. Where every static it initialises is new, the guard is new and the bound
// constant-initialised static keeps its live value; anything else, and any use that is not read, refuses as before.
//
// Two halves:
//   * real clang-cl 18 images: tests/unit/bind/wp63/<case>.{exe,pdb,patch.obj} (built by tests/unit/bind/wp63/build.sh; the
//     n* cases without thread-safe statics, the s* cases with them) and WP-59's b4_late / b6_const / b7_both;
//   * the reader on hand-assembled code in the shapes cl emits (its x64 code generation for a function-local static, with
//     and without /Zc:threadSafeInit-), and the shapes that must not be read.
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "cml/core/guard_code.h"
#include "cml/core/hook_narrowing.h"
#include "cml/pdb/index.h"
#include "cml/pe/pe.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
using namespace cml::core::bind;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), path);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::transform(raw.begin(), raw.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

const char* const kCalls = "?calls@?1??counter@@YAHXZ@4HA";
const char* const kA = "?a@?1??counter@@YAHXZ@4UObj@@A";
const char* const kB = "?b@?1??counter@@YAHXZ@4UObj@@A";
const char* const kD = "?d@?1??counter@@YAHXZ@4UD@@A";
const char* const kBits = "??_B?1??counter@@YAHXZ@51";
const char* const kTss1 = "?$TSS1@?1??counter@@YAHXZ@4HA";
const char* const kTss2 = "?$TSS2@?1??counter@@YAHXZ@4HA";

// One real case: the old image, the patch, the plan (or what a failed planning decided) and the bitmask proof.
struct Case {
    std::string name;
    std::shared_ptr<const pdb::PdbIndex> pdb;
    std::vector<std::byte> object_bytes;
    std::unique_ptr<coff::Object> object;
    std::unique_ptr<pe::OwnedImage> image;
    bool planned = false;
    BindPlan plan;
    std::vector<Diag> plan_errors;
    std::vector<Diag> proof;

    Case(const std::string& dir, const std::string& n) : name(n) {
        const std::string base = dir + "/" + n;
        auto p = pdb::PdbIndex::open_pdb(base + ".pdb");
        REQUIRE_MESSAGE(p.has_value(), p.error().message);
        pdb = *p;
        object_bytes = read_file(base + ".patch.obj");
        auto o = coff::Object::parse(coff::ByteSpan(object_bytes.data(), object_bytes.size()));
        REQUIRE_MESSAGE(o.has_value(), o.error().message);
        object = std::make_unique<coff::Object>(std::move(*o));
        auto img = pe::load_image_file(base + ".exe");
        REQUIRE_MESSAGE(img.has_value(), img.error().message);
        image = std::make_unique<pe::OwnedImage>(std::move(*img));

        PlanInput in;
        in.object = object.get();
        in.object_name = n + ".patch.obj";
        in.module_pdb = pdb.get();
        in.module_name = n + ".exe";
        in.module_base = 0x140000000ull;
        in.compiland = n + ".old.obj";
        BindPlan partial;
        auto r = plan_bind(in, &plan_errors, &partial);
        planned = r.has_value();
        plan = planned ? std::move(*r) : std::move(partial);

        NarrowInput ni;
        ni.object = object.get();
        ni.plan = &plan;
        ni.old_image = &image->image;
        ni.load_base = in.module_base;
        ni.is_hooked = [](std::uint64_t) { return false; };
        proof = prove_bitmask_guards(ni);
    }

    const SymbolDecision& decision(const std::string& symbol) const {
        const SymbolDecision* d = plan.find_decision(symbol);
        REQUIRE_MESSAGE(d != nullptr, symbol);
        return *d;
    }
    const Diag* plan_error(const char* code_id) const {
        for (const Diag& d : plan_errors) {
            if (d.id() == code_id) {
                return &d;
            }
        }
        return nullptr;
    }
    void no_guard_warning() const {
        for (const Diag& w : plan.warnings) {
            CHECK_MESSAGE(w.code != diag::kGuardUnmatched, w.message);
        }
    }
};

Case wp63(const std::string& n) { return Case(CML_BIND_WP63_DIR, n); }
Case wp59(const std::string& n) { return Case(CML_BIND_WP59_DIR, n); }

}  // namespace

TEST_CASE("R-69: an inline function keeps its constant-initialised static and gains a dynamic one, bitmask (b6): calls kept, a and the bitmask new") {
    // clang-cl 18 /Zc:threadSafeInit-: calls is static number 0 and tests no bit; a takes bit 1 (0x2). The code claims 0x2
    // only, and its region refers to a only: the bitmask guards a, which is new. Until WP-63: CML3600 (WP-59 review item 2).
    const Case c = wp59("b6_const");
    for (const Diag& e : c.plan_errors) {
        CHECK_MESSAGE(false, e.message);
    }
    REQUIRE(c.planned);
    CHECK(c.decision(kCalls).decision == DecisionKind::BindOldData);
    CHECK(c.decision(kA).decision == DecisionKind::PatchNewData);
    CHECK(c.decision(kBits).decision == DecisionKind::PatchNewData);
    CHECK(contains(c.decision(kBits).reason, "read from the patch's code"));
    CHECK(c.plan.bitmask_guards.empty());
    CHECK(c.proof.empty());
    c.no_guard_warning();
}

TEST_CASE("R-69: the same with thread-safe statics (s1): a's guard $TSS1 initialises a only, calls kept") {
    // a's guard is `$TSS1` (calls is number 0 and has none); its name ties it to calls and a (one block). The bracket
    // _Init_thread_header .. _Init_thread_footer of `$TSS1` refers to a only.
    const Case c = wp63("s1_const");
    for (const Diag& e : c.plan_errors) {
        CHECK_MESSAGE(false, e.message);
    }
    REQUIRE(c.planned);
    CHECK(c.decision(kCalls).decision == DecisionKind::BindOldData);
    CHECK(c.decision(kA).decision == DecisionKind::PatchNewData);
    CHECK(c.decision(kTss1).decision == DecisionKind::PatchNewData);
    CHECK(c.plan.bitmask_guards.empty());
    c.no_guard_warning();
}

TEST_CASE("R-69: a constant-initialised static made dynamic next to the new one stays refused, with and without thread-safe statics (b7, s2)") {
    // b7: the code claims calls' bit (0x1) and a's (0x2); s2: `$TSS0` initialises calls, `$TSS1` a. A bound static is
    // initialised under a guard next to a new one (b7), or under a guard the old image does not have (s2): CML3600.
    const Case b7 = wp59("b7_both");
    CHECK_FALSE(b7.planned);
    const Diag* d = b7.plan_error("CML3600");
    REQUIRE(d != nullptr);
    CAPTURE(d->message);
    CHECK(contains(d->message, kBits));
    CHECK(contains(d->message, "the patch's code initialises"));
    CHECK(contains(d->message, "counter::calls"));
    CHECK(contains(d->message, "counter::a"));

    const Case s2 = wp63("s2_both");
    CHECK_FALSE(s2.planned);
    const Diag* e = s2.plan_error("CML3600");
    REQUIRE(e != nullptr);
    CAPTURE(e->message);
    CHECK(contains(e->message, "?$TSS0@?1??counter@@YAHXZ@4HA"));
    CHECK(contains(e->message, "counter::calls"));
    CHECK(contains(e->message, "the patch's code initialises"));
    CHECK(s2.decision(kCalls).decision == DecisionKind::BindOldData);
}

TEST_CASE("R-69: a constant-initialised static made dynamic, alone, stays refused (b4): the code initialises the bound a") {
    const Case c = wp59("b4_late");
    CHECK_FALSE(c.planned);
    const Diag* d = c.plan_error("CML3600");
    REQUIRE(d != nullptr);
    CAPTURE(d->message);
    CHECK(contains(d->message, "the old image has no bitmask of that name"));
    CHECK(contains(d->message, "the patch's code initialises"));
    CHECK(contains(d->message, "counter::a"));
}

TEST_CASE("R-69: two new dynamic statics, one with a destructor (/EHsc), next to the kept calls (n3 bitmask, s3 thread-safe)") {
    // n3: bits 0x2 (a) and 0x4 (d, registered with atexit); the EH funclets clear the bits (`and byte ptr [bitmask], ~bit`),
    // which claims nothing. s3: `$TSS1` (a) and `$TSS2` (d); the funclets call _Init_thread_abort.
    const Case n3 = wp63("n3_dtor");
    for (const Diag& e : n3.plan_errors) {
        CHECK_MESSAGE(false, e.message);
    }
    REQUIRE(n3.planned);
    CHECK(n3.decision(kCalls).decision == DecisionKind::BindOldData);
    CHECK(n3.decision(kA).decision == DecisionKind::PatchNewData);
    CHECK(n3.decision(kD).decision == DecisionKind::PatchNewData);
    CHECK(n3.decision(kBits).decision == DecisionKind::PatchNewData);
    n3.no_guard_warning();

    const Case s3 = wp63("s3_dtor");
    for (const Diag& e : s3.plan_errors) {
        CHECK_MESSAGE(false, e.message);
    }
    REQUIRE(s3.planned);
    CHECK(s3.decision(kCalls).decision == DecisionKind::BindOldData);
    CHECK(s3.decision(kTss1).decision == DecisionKind::PatchNewData);
    CHECK(s3.decision(kTss2).decision == DecisionKind::PatchNewData);
    s3.no_guard_warning();
}

TEST_CASE("R-69: the inline function inlined into its caller twice (n4): both copies claim a's bit, calls kept") {
    const Case c = wp63("n4_inl");
    for (const Diag& e : c.plan_errors) {
        CHECK_MESSAGE(false, e.message);
    }
    REQUIRE(c.planned);
    CHECK(c.decision(kCalls).decision == DecisionKind::BindOldData);
    CHECK(c.decision(kA).decision == DecisionKind::PatchNewData);
    CHECK(c.decision(kBits).decision == DecisionKind::PatchNewData);
}

TEST_CASE("R-69: a new dynamic static next to a bound dynamic one stays refused (n5): the code initialises both under the bitmask") {
    const Case c = wp63("n5_kept");
    CHECK_FALSE(c.planned);
    const Diag* d = c.plan_error("CML3600");
    REQUIRE(d != nullptr);
    CAPTURE(d->message);
    CHECK(contains(d->message, kBits));
    CHECK(contains(d->message, "counter::b"));
    CHECK(contains(d->message, "the patch's code initialises"));
    CHECK(c.decision(kB).decision == DecisionKind::PatchNewData);
}

TEST_CASE("R-69: the bound constant-initialised static's address in a register keeps the refusal (n6): a region could write it") {
    // As b6, but counter() passes &calls to keep(): `lea rcx, [calls]`. The code claims only a's bit, yet calls' address is
    // in a register in the code that uses the bitmask, so a region that names a only could still write calls through it.
    const Case c = wp63("n6_addr");
    CHECK_FALSE(c.planned);
    const Diag* d = c.plan_error("CML3600");
    REQUIRE(d != nullptr);
    CAPTURE(d->message);
    CHECK(contains(d->message, kBits));
    CHECK(contains(d->message, "it takes the address of 'counter::calls'"));
    CHECK(c.decision(kCalls).decision == DecisionKind::BindOldData);
}

// ---- the reader on hand-assembled code ---------------------------------------------------------

namespace {

// A code section assembled byte by byte; `field` appends a 4-byte relocated field.
struct Asm {
    std::vector<std::uint8_t> bytes;
    std::vector<GuardCodeReloc> relocs;
    std::vector<std::uint32_t> starts;

    Asm& raw(std::initializer_list<int> b) {
        for (const int v : b) {
            bytes.push_back(static_cast<std::uint8_t>(v));
        }
        return *this;
    }
    // A REL32_k field (cl's spelling: the addend is the offset into the target) for `target`.
    Asm& rel(const std::string& target, int k = 0, std::int32_t addend = 0, std::int32_t section = 0) {
        GuardCodeReloc r;
        r.offset = static_cast<std::uint32_t>(bytes.size());
        r.width = 4;
        r.rel_tail = static_cast<std::int8_t>(k);
        r.target = target;
        r.target_section = section;
        relocs.push_back(r);
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<std::uint8_t>((static_cast<std::uint32_t>(addend) >> (8 * i)) & 0xFFu));
        }
        return *this;
    }
    Asm& secrel(const std::string& target) {
        GuardCodeReloc r;
        r.offset = static_cast<std::uint32_t>(bytes.size());
        r.width = 4;
        r.target = target;
        relocs.push_back(r);
        raw({0, 0, 0, 0});
        return *this;
    }
    std::uint32_t here() const { return static_cast<std::uint32_t>(bytes.size()); }
    // Patches the rel8 of the branch whose opcode is at `at` to jump to `to`.
    void jump8(std::uint32_t at, std::uint32_t to) { bytes[at + 1] = static_cast<std::uint8_t>(static_cast<std::int32_t>(to) - static_cast<std::int32_t>(at + 2)); }

    GuardCodeSection section(const std::string& name) const {
        GuardCodeSection s;
        s.name = name;
        s.data = std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
        s.relocs = relocs;
        s.function_starts = starts;
        return s;
    }
};

const char* const kG = "?$TSS0@?1??counter@@YAHXZ@4HA";  // cl: a, the first guarded static of the function, takes $TSS0
const char* const kM = "??_B?1??counter@@YAHXZ@51";

std::map<std::string, std::int32_t, std::less<>> candidates() { return {{kCalls, 3}, {kA, 4}}; }

// cl /O2, thread-safe statics (its code generation for `static Obj a = make(100);` next to `static int calls = 0;`):
//   mov eax,[G]; mov ecx,[_tls_index]; mov rdx,gs:[58h]; mov rcx,[rdx+rcx*8]; cmp eax,[rcx+_Init_thread_epoch]; jle done
//   lea rcx,[G]; call _Init_thread_header; cmp dword ptr [G],-1; jne done
//   mov ecx,100; call make; mov [a],rax; lea rcx,[G]; call _Init_thread_footer
//   done: inc dword ptr [calls]; mov eax,[calls]; ret
// `options`: 'h' hoists the guard's address into rbx (lea rbx,[G]; mov rcx,rbx), 'c' writes calls in the bracket too,
// 'n' writes another static instead of a, 'j' adds a jump from behind the function into the bracket, 'z' a jmp to the next
// instruction after the call of make (clang -O0).
Asm cl_thread_safe(const std::string& options = "") {
    Asm s;
    s.raw({0x48, 0x83, 0xEC, 0x28});                   // sub rsp, 28h
    s.raw({0x8B, 0x05}).rel(kG);                        // mov eax, [rip+G]
    s.raw({0x8B, 0x0D}).rel("_tls_index");              // mov ecx, [rip+_tls_index]
    s.raw({0x65, 0x48, 0x8B, 0x14, 0x25, 0x58, 0, 0, 0});  // mov rdx, gs:[58h]
    s.raw({0x48, 0x8B, 0x0C, 0xCA});                   // mov rcx, [rdx+rcx*8]
    s.raw({0x3B, 0x81}).secrel("_Init_thread_epoch");   // cmp eax, [rcx+_Init_thread_epoch]
    const std::uint32_t jle = s.here();
    s.raw({0x7E, 0x00});                                // jle done
    if (options.find('h') != std::string::npos) {
        s.raw({0x48, 0x8D, 0x1D}).rel(kG);              // lea rbx, [rip+G]
        s.raw({0x48, 0x8B, 0xCB});                      // mov rcx, rbx
    } else {
        s.raw({0x48, 0x8D, 0x0D}).rel(kG);              // lea rcx, [rip+G]
    }
    s.raw({0xE8}).rel("_Init_thread_header");           // call _Init_thread_header
    s.raw({0x83, 0x3D}).rel(kG, 1).raw({0xFF});         // cmp dword ptr [rip+G], -1
    const std::uint32_t jne = s.here();
    s.raw({0x75, 0x00});                                // jne done
    const std::uint32_t inside = s.here();
    s.raw({0xB9, 0x64, 0, 0, 0});                       // mov ecx, 100
    s.raw({0xE8}).rel("?make@@YA?AUObj@@H@Z");          // call make
    if (options.find('z') != std::string::npos) {
        s.raw({0xEB, 0x00});                            // jmp next (clang -O0: an invoke's normal destination)
    }
    s.raw({0x48, 0x89, 0x05}).rel(options.find('n') != std::string::npos ? "?other@@3HA" : kA);  // mov [rip+a], rax
    if (options.find('c') != std::string::npos) {
        s.raw({0x89, 0x05}).rel(kCalls);                // mov [rip+calls], eax
    }
    s.raw({0x48, 0x8D, 0x0D}).rel(kG);                  // lea rcx, [rip+G]
    s.raw({0xE8}).rel("_Init_thread_footer");           // call _Init_thread_footer
    const std::uint32_t done = s.here();
    s.raw({0xFF, 0x05}).rel(kCalls);                    // inc dword ptr [rip+calls]
    s.raw({0x8B, 0x05}).rel(kCalls);                    // mov eax, [rip+calls]
    s.raw({0x48, 0x83, 0xC4, 0x28});                   // add rsp, 28h
    s.raw({0xC3});                                      // ret
    if (options.find('j') != std::string::npos) {
        const std::uint32_t back = s.here();
        s.raw({0xEB, 0x00});                            // jmp inside
        s.jump8(back, inside);
    }
    s.jump8(jle, done);
    s.jump8(jne, done);
    return s;
}

// cl /O2 /Zc:threadSafeInit- (bitmask; `options` as above, 'm' sets the bit with `or dword ptr [M], 1` directly, '3' or's 3,
// 'v' stores a plain value instead of an or, 'e' adds an EH funclet that clears the bit):
//   mov eax,[M]; test al,1; jne done; or eax,1; mov [M],eax; mov ecx,100; call make; mov [a],rax
//   done: inc dword ptr [calls]; mov eax,[calls]; ret
Asm cl_bitmask(const std::string& options = "") {
    Asm s;
    s.raw({0x8B, 0x05}).rel(kM);                        // mov eax, [rip+M]
    s.raw({0xA8, 0x01});                                // test al, 1
    const std::uint32_t jne = s.here();
    s.raw({0x75, 0x00});                                // jne done
    if (options.find('m') != std::string::npos) {
        s.raw({0x83, 0x0D}).rel(kM, 1).raw({0x01});     // or dword ptr [rip+M], 1
    } else {
        if (options.find('v') != std::string::npos) {
            s.raw({0xB8, 0x01, 0, 0, 0});               // mov eax, 1
        } else {
            s.raw({0x83, 0xC8, options.find('3') != std::string::npos ? 0x03 : 0x01});  // or eax, 1 (or 3)
        }
        s.raw({0x89, 0x05}).rel(kM);                    // mov [rip+M], eax
    }
    s.raw({0xB9, 0x64, 0, 0, 0});                       // mov ecx, 100
    s.raw({0xE8}).rel("?make@@YA?AUObj@@H@Z");          // call make
    if (options.find('z') != std::string::npos) {
        s.raw({0xEB, 0x00});                            // jmp next (clang -O0: an invoke's normal destination)
    }
    s.raw({0x48, 0x89, 0x05}).rel(options.find('n') != std::string::npos ? "?other@@3HA" : kA);  // mov [rip+a], rax
    if (options.find('c') != std::string::npos) {
        s.raw({0x89, 0x05}).rel(kCalls);                // mov [rip+calls], eax
    }
    const std::uint32_t done = s.here();
    s.raw({0xFF, 0x05}).rel(kCalls);                    // inc dword ptr [rip+calls]
    s.raw({0x8B, 0x05}).rel(kCalls);                    // mov eax, [rip+calls]
    s.raw({0xC3});                                      // ret
    if (options.find('e') != std::string::npos) {
        s.raw({0xCC, 0xCC, 0xCC});
        s.starts.push_back(s.here());                   // ?dtor$0@?0??counter@@YAHXZ@4HA
        s.raw({0x83, 0x25}).rel(kM, 1).raw({0xFE});     // and dword ptr [rip+M], -2
        s.raw({0xC3});                                  // ret
    }
    s.jump8(jne, done);
    return s;
}

GuardCodeReading read(const Asm& a, const char* guard, GuardCodeKind kind, std::string_view data_reference = {},
                      const std::map<std::string, std::int32_t, std::less<>>& cands = candidates()) {
    const std::vector<GuardCodeSection> code = {a.section(".text$mn")};
    const GuardCodeReading r = read_guard_code(code, guard, 9, kind, cands, data_reference);
    if (!r.proved) {
        MESSAGE(r.why);
    }
    return r;
}

}  // namespace

TEST_CASE("R-69 (reader): cl's thread-safe shape: the bracket of $TSS0 initialises a, and nothing else") {
    const GuardCodeReading r = read(cl_thread_safe(), kG, GuardCodeKind::ThreadSafe);
    REQUIRE(r.proved);
    CHECK(r.statics == std::set<std::string>{kA});
    REQUIRE(r.claims.size() == 1);
    CHECK(r.claims[0].static_name == kA);
    CHECK(r.references == 4);
}

TEST_CASE("R-69 (reader): cl's bitmask shape: bit 0x1 initialises a; the EH funclet's and claims nothing; `or [M], 1` is a claim too") {
    for (const char* options : {"", "e", "m", "me"}) {
        CAPTURE(options);
        const GuardCodeReading r = read(cl_bitmask(options), kM, GuardCodeKind::Bitmask);
        REQUIRE(r.proved);
        CHECK(r.statics == std::set<std::string>{kA});
        REQUIRE(r.claims.size() == 1);
        CHECK(r.claims[0].bits == 0x1u);
    }
}

TEST_CASE("R-69 (reader): a jmp to the next instruction inside a region falls through (clang -O0 shape)") {
    const GuardCodeReading t = read(cl_thread_safe("z"), kG, GuardCodeKind::ThreadSafe);
    REQUIRE(t.proved);
    CHECK(t.statics == std::set<std::string>{kA});
    const GuardCodeReading m = read(cl_bitmask("z"), kM, GuardCodeKind::Bitmask);
    REQUIRE(m.proved);
    CHECK(m.statics == std::set<std::string>{kA});
}

TEST_CASE("R-69 (reader): shapes that are not read refuse") {
    struct Bad {
        const char* what;
        Asm code;
        const char* guard;
        GuardCodeKind kind;
        const char* why;
    };
    const std::vector<Bad> bad = {
        {"the guard's address hoisted into rbx", cl_thread_safe("h"), kG, GuardCodeKind::ThreadSafe, "by an instruction that is not read"},
        {"calls written in the bracket too", cl_thread_safe("c"), kG, GuardCodeKind::ThreadSafe, "writes 2 of them"},
        {"no candidate written in the bracket", cl_thread_safe("n"), kG, GuardCodeKind::ThreadSafe, "writes none"},
        {"a jump into the bracket", cl_thread_safe("j"), kG, GuardCodeKind::ThreadSafe, "is entered from"},
        {"a stored value that is no or of a constant", cl_bitmask("v"), kM, GuardCodeKind::Bitmask, "not an or"},
        {"two bits set at once", cl_bitmask("3"), kM, GuardCodeKind::Bitmask, "not one bit"},
        {"calls written in the region too", cl_bitmask("c"), kM, GuardCodeKind::Bitmask, "writes 2 of them"},
        {"a bitmask read as a thread-safe guard", cl_bitmask(), kM, GuardCodeKind::ThreadSafe, "not read"},
    };
    for (const Bad& b : bad) {
        CAPTURE(b.what);
        const GuardCodeReading r = read(b.code, b.guard, b.kind);
        CHECK_FALSE(r.proved);
        CHECK(r.statics.empty());
        CHECK_MESSAGE(contains(r.why, b.why), r.why);
    }
}

TEST_CASE("R-69 (reader): the guard in data, through a section symbol, behind undecoded code, without a claim, or a stray footer: not read") {
    {
        const GuardCodeReading r = read(cl_bitmask(), kM, GuardCodeKind::Bitmask, "section 5 (.data)");
        CHECK_FALSE(r.proved);
        CHECK(contains(r.why, "stored in section 5"));
    }
    {
        Asm a = cl_bitmask();
        GuardCodeReloc sec;
        sec.offset = 0;  // anywhere: the reference hides which symbol of a's section it is
        sec.width = 4;
        sec.rel_tail = 0;
        sec.target = ".bss";
        sec.target_is_section = true;
        sec.target_section = 4;  // a's
        a.relocs.push_back(sec);
        const GuardCodeReading r = read(a, kM, GuardCodeKind::Bitmask);
        CHECK_FALSE(r.proved);
        CHECK(contains(r.why, "through another symbol"));
    }
    {
        // A relocation inside a run of nops: the sweep stops there, and the guard's uses behind it are not decoded.
        Asm a;
        a.raw({0x90, 0x90, 0x90, 0x90, 0x90, 0x90});
        GuardCodeReloc jt;
        jt.offset = 1;
        jt.width = 4;
        jt.target = "$LN5";
        a.relocs.push_back(jt);
        const Asm rest = cl_bitmask();
        for (GuardCodeReloc r : rest.relocs) {
            r.offset += a.here();
            a.relocs.push_back(r);
        }
        a.bytes.insert(a.bytes.end(), rest.bytes.begin(), rest.bytes.end());
        const GuardCodeReading r = read(a, kM, GuardCodeKind::Bitmask);
        CHECK_FALSE(r.proved);
        CHECK(contains(r.why, "does not decode completely"));
    }
    {
        Asm a;
        a.raw({0x8B, 0x05}).rel(kM);  // mov eax, [rip+M]
        a.raw({0xC3});
        const GuardCodeReading r = read(a, kM, GuardCodeKind::Bitmask);
        CHECK_FALSE(r.proved);
        CHECK(contains(r.why, "no code of the patch initialises a static under the guard"));
    }
    {
        Asm a;
        a.raw({0x48, 0x8D, 0x0D}).rel(kG);         // lea rcx, [rip+G]
        a.raw({0xE8}).rel("_Init_thread_footer");  // call _Init_thread_footer
        a.raw({0xC3});
        const GuardCodeReading r = read(a, kG, GuardCodeKind::ThreadSafe);
        CHECK_FALSE(r.proved);
        CHECK(contains(r.why, "ends no region"));
    }
}

TEST_CASE("R-69 (reader): a candidate whose address is put in a register is reported, wherever it is") {
    const GuardCodeReading plain = read(cl_bitmask(), kM, GuardCodeKind::Bitmask);
    REQUIRE(plain.proved);
    CHECK(plain.address_taken.empty());
    Asm a = cl_bitmask();
    a.raw({0x48, 0x8D, 0x0D}).rel(kCalls);  // lea rcx, [rip+calls]
    a.raw({0xC3});
    const GuardCodeReading r = read(a, kM, GuardCodeKind::Bitmask);
    REQUIRE(r.proved);
    CHECK(r.statics == std::set<std::string>{kA});
    CHECK(r.address_taken == std::set<std::string>{kCalls});
}

TEST_CASE("R-69 (review 1): a ternary initialiser: the claim is the static the region writes, not one it only reads") {
    // `static int calls = pick(x) ? 1 : make_int(2);` next to new statics x and a: the store to calls sits behind the
    // jmp/join of the ternary, and the region reads x first. Until review 1 the region stopped at the jmp and pinned the bit
    // on x (calls would then be bound next to a fresh bitmask and initialised again); now it follows the branches to the join
    // and counts only what it writes.
    const char* const kX = "?x@?1??counter@@YAHXZ@4HA";
    Asm s;
    s.raw({0x8B, 0x05}).rel(kM);                   // mov eax, [rip+M]
    s.raw({0xA8, 0x02});                           // test al, 2
    const std::uint32_t jne_done = s.here();
    s.raw({0x75, 0x00});                           // jne done
    s.raw({0x83, 0xC8, 0x02});                     // or eax, 2
    s.raw({0x89, 0x05}).rel(kM);                   // mov [rip+M], eax
    s.raw({0x8B, 0x0D}).rel(kX);                   // mov ecx, [rip+x]
    s.raw({0xE8}).rel("?pick@@YAHH@Z");            // call pick
    s.raw({0x85, 0xC0});                           // test eax, eax
    const std::uint32_t jne_one = s.here();
    s.raw({0x75, 0x00});                           // jne L1
    s.raw({0xB9, 0x02, 0, 0, 0});                  // mov ecx, 2
    s.raw({0xE8}).rel("?make_int@@YAHH@Z");        // call make_int
    const std::uint32_t jmp_join = s.here();
    s.raw({0xEB, 0x00});                           // jmp L2
    const std::uint32_t one = s.here();
    s.raw({0xB8, 0x01, 0, 0, 0});                  // L1: mov eax, 1
    const std::uint32_t join = s.here();
    s.raw({0x89, 0x05}).rel(kCalls);               // L2: mov [rip+calls], eax
    const std::uint32_t done = s.here();
    s.raw({0x8B, 0x05}).rel(kCalls);               // done: mov eax, [rip+calls]
    s.raw({0xC3});                                 // ret
    s.jump8(jne_done, done);
    s.jump8(jne_one, one);
    s.jump8(jmp_join, join);
    const GuardCodeReading r = read(s, kM, GuardCodeKind::Bitmask, {}, {{kCalls, 3}, {kA, 4}, {kX, 5}});
    REQUIRE(r.proved);
    CHECK(r.statics == std::set<std::string>{kCalls});
    REQUIRE(r.claims.size() == 1);
    CHECK(r.claims[0].bits == 0x2u);
}

TEST_CASE("R-69 (review residual b): the register a claim stores must be loaded from the guard") {
    // `mov eax, [other]; ... or eax, 1; mov [M], eax`: the store could set any bits; refused. The cl shape (loaded from the
    // guard before the test) is proved (above).
    Asm s;
    s.raw({0x8B, 0x05}).rel("?other@@3HA");        // mov eax, [rip+other]
    s.raw({0xA8, 0x01});                           // test al, 1
    const std::uint32_t jne = s.here();
    s.raw({0x75, 0x00});                           // jne done
    s.raw({0x83, 0xC8, 0x01});                     // or eax, 1
    s.raw({0x89, 0x05}).rel(kM);                   // mov [rip+M], eax
    s.raw({0x48, 0x89, 0x05}).rel(kA);             // mov [rip+a], rax
    const std::uint32_t done = s.here();
    s.raw({0xC3});                                 // done: ret
    s.jump8(jne, done);
    const GuardCodeReading r = read(s, kM, GuardCodeKind::Bitmask);
    CHECK_FALSE(r.proved);
    CHECK(contains(r.why, "is not shown to hold the guard"));
}

TEST_CASE("R-69 (review 3, 4, 2): an alias of the guard, bts of a 64-bit form or a bit beyond 31, a candidate behind a decoder stop") {
    {
        Asm a = cl_bitmask();
        a.raw({0x8B, 0x05}).rel("$guard_alias", 0, 0, 9);  // mov eax, [rip+label in the guard's section]
        a.raw({0xC3});
        const GuardCodeReading r = read(a, kM, GuardCodeKind::Bitmask);
        CHECK_FALSE(r.proved);
        CHECK(contains(r.why, "refers to the guard's section through another symbol"));
    }
    {
        Asm a = cl_bitmask();
        a.raw({0x0F, 0xBA, 0x2D}).rel(kM, 1).raw({0x21});  // bts dword ptr [rip+M], 33
        a.raw({0xC3});
        const GuardCodeReading r = read(a, kM, GuardCodeKind::Bitmask);
        CHECK_FALSE(r.proved);
        CHECK(contains(r.why, "in a form that is not read"));
    }
    {
        Asm a = cl_bitmask();
        a.raw({0x48, 0x0F, 0xBA, 0x2D}).rel(kM, 1).raw({0x02});  // bts qword ptr [rip+M], 2
        a.raw({0xC3});
        const GuardCodeReading r = read(a, kM, GuardCodeKind::Bitmask);
        CHECK_FALSE(r.proved);
        CHECK(contains(r.why, "in a form that is not read"));
    }
    {
        // The guard's uses all decode; behind them, data the sweep cannot pass, then `lea rcx, [calls]`.
        Asm a = cl_bitmask();
        a.raw({0x90, 0x90, 0x90, 0x90, 0x90});
        GuardCodeReloc jt;
        jt.offset = a.here() - 4;
        jt.width = 4;
        jt.target = "$LN7";
        a.relocs.push_back(jt);
        a.raw({0x48, 0x8D, 0x0D}).rel(kCalls);  // lea rcx, [rip+calls]
        a.raw({0xC3});
        const GuardCodeReading r = read(a, kM, GuardCodeKind::Bitmask);
        CHECK_FALSE(r.proved);
        CHECK(contains(r.why, "does not decode completely"));
    }
}

TEST_CASE("R-69 (reader): one bit per static, one static per bit") {
    // Two claims of bit 0x1 whose regions initialise a and calls: which one the bit is cannot be told.
    Asm a = cl_bitmask();
    a.raw({0x8B, 0x05}).rel(kM);   // mov eax, [rip+M]
    a.raw({0x83, 0xC8, 0x01});     // or eax, 1
    a.raw({0x89, 0x05}).rel(kM);   // mov [rip+M], eax
    a.raw({0x89, 0x05}).rel(kCalls);  // mov [rip+calls], eax
    a.raw({0xC3});
    const GuardCodeReading r = read(a, kM, GuardCodeKind::Bitmask);
    CHECK_FALSE(r.proved);
    CHECK(contains(r.why, "do not pair one to one"));
}
