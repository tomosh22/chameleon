#pragma once

// Narrowing the hook list to the functions that really changed (WP-12, plan 4.5 + review item 3).
//
// The bind planner hooks every function the patched object defines and the old image also has: for an
// STL-heavy translation unit that is up to 83 functions, nearly all of them untouched instantiations. Each
// extra hook is a chance for a refusal (a 3-byte function, CML4301) that would fail the whole batch, and each
// needs the application's threads stopped. The old copy of an unchanged function keeps running, which is
// correct: it is the same code.
//
// The rule (documented in docs/wp/WP-12.md). A hook is DROPPED only when everything below holds, and KEPT
// ("when in doubt, hook") in every other case:
//
//   1. the function is in the module file (old RVA known), its entry is not currently redirected by an
//      earlier patch (`is_hooked`), and the PDB gave its size;
//   2. the new code is as long as the old code, and the bytes are equal with the 4 (or 8) bytes of every
//      relocation site masked out ("compared normalised for relocations"): the old bytes come from the module
//      file on disk, the new ones from the compiler's object;
//   3. every relocation resolves to the SAME target as in the old code:
//        * a symbol that lives in the old image (bound data / function, a hooked function, __ImageBase): the
//          target address recovered from the old displacement (S = disp + site + 4 + N - A) equals the address
//          the planner resolved for it; SECREL sites hold the planned offset; ADDR64 sites the planned address;
//        * read-only data the patch carries as a fresh copy (string literals, constant tables): the bytes of
//          the new copy equal the bytes at the old target (relocations inside it masked);
//        * anything else (a new function or global, a fresh function, a section symbol, a writable fresh
//          symbol, a relocation type that cannot be checked): the function is kept.
//
// Rule 3 is what the plain "mask relocation sites" comparison lacks: `call helper_a` -> `call helper_b` or a
// string literal edit leaves the masked bytes identical.
//
// WP-25 (unchanged-function elision). A function that an earlier patch hooked (or defined) has no code in the module
// file to compare with: the entry jumps to the earlier patch. The broker therefore records, for every function a patch
// brings (`capture_code`), a `CodeImage`: its bytes with the relocation sites zeroed, and per relocation WHAT it
// resolved to (the old address of a bound symbol, the planned SECREL value, the content of a fresh read-only constant,
// the address of a symbol the patch introduced). `NarrowInput::prior_code` hands that record back for an entry, and the
// same three rules apply to it: same length, equal masked bytes, every relocation reaching the same target (with the same
// in-place addend). Whatever cannot be recorded or compared keeps the hook ("when in doubt, hook").
//
// WP-49 (R-46, ADR-0044). A jump table in the function's OWN code (cl: a `switch` compiles to `lea rcx,[__ImageBase]` /
// `mov eax,[rcx+rax*4+$LNt]` / `add rax,rcx` / `jmp rax` and a table of ADDR32NB entries to the case labels, placed behind
// the code inside the function) reaches places relative to wherever the function lies: the old code reaches its own case
// labels, the patch its own. Such references are compared entry by entry relative to the function start: every ADDR32NB
// whose target lies inside the function must be either an entry of a table (a run of consecutive 4-byte ADDR32NB entries,
// every one of them to a place inside the function, starting where a reference points) or a reference to the start of one;
// the target of each, relative to the function start, must be the same in the old code (or in an earlier patch's record).
// A table whose entries leave the function, that runs to the function end with an ADDR32NB right behind it (its end cannot
// be told), a reference to a place inside the function that is not a table (a byte index table), or a table in another
// section (clang-cl puts its tables in .rdata) keeps the hook as before.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/core/bind_plan.h"
#include "cml/pe/pe.h"

namespace cml::core {

// What one relocation of a recorded function reaches (WP-25).
struct CodeTarget {
    enum class Kind : std::uint8_t {
        Address,    // a symbol of the running process: `value` is its address (REL32 / ADDR64 sites)
        SecRel,     // a SECREL site: `value` is the planned section-relative value
        Constant,   // a fresh read-only constant the patch carries (string literal, table): `bytes` is its content
        NewSymbol,  // a symbol this patch introduced; `name` until the patch is laid out, then turned into an Address
        Local,      // WP-49: a place inside the function itself (a jump-table entry or the table it loads from): `value` is
                    // its offset from the function start
    };
    Kind kind = Kind::Address;
    std::uint64_t value = 0;
    std::vector<std::uint8_t> bytes;
    std::string name;
    // Set when the reference is to a function the patch defines AND hooks (PatchHooked): the linked patch calls its own copy of
    // it, not the old entry. Not part of the comparison (the address says what it is); used by narrow_hooks to keep a caller
    // whose earlier-patch copy would go on calling a stale copy of a callee that is hooked again.
    std::string local_callee;
    bool operator==(const CodeTarget& o) const { return kind == o.kind && value == o.value && bytes == o.bytes && name == o.name; }
};

struct CodeReloc {
    std::uint32_t offset = 0;      // in the function
    std::uint16_t type = 0;        // coff::RelocType
    std::uint64_t addend = 0;      // the in-place value of the object (REL32 / ADDR64 / SECREL addend)
    CodeTarget target;
    bool operator==(const CodeReloc& o) const { return offset == o.offset && type == o.type && addend == o.addend && target == o.target; }
};

// The code a patch gave one function entry, as far as it can be compared with a later compile of the same function.
struct CodeImage {
    std::vector<std::uint8_t> bytes;  // relocation sites zeroed
    std::vector<CodeReloc> relocs;    // in section order
    std::string display;
    std::string symbol;  // decorated name of the function (lets the broker tell whether a record is still reachable)
};

// Turns the NewSymbol targets into addresses once the patch is laid out; false when one cannot be resolved (the image
// must then not be recorded).
bool finalize_code(CodeImage& image, const std::function<std::optional<std::uint64_t>(const std::string& symbol)>& address_of);

enum class NarrowReason : std::uint8_t {
    Unchanged,            // dropped: identical code, identical targets
    BytesDiffer,          // kept
    SizeDiffers,          // kept
    UnknownOldSize,       // kept: the PDB did not give a size
    OldBytesUnavailable,  // kept: not in the module file, or not readable
    AlreadyHooked,        // kept: the old entry currently jumps to an earlier patch
    TargetChanged,        // kept: a relocation resolves differently than in the old code
    TargetNew,            // kept: a relocation refers to something the old image does not have
    Unverifiable,         // kept: a relocation or symbol this check cannot judge
    Disabled,             // kept: narrowing switched off
};

const char* to_string(NarrowReason reason) noexcept;
inline bool is_dropped(NarrowReason reason) noexcept { return reason == NarrowReason::Unchanged; }

struct NarrowDecision {
    std::string symbol;   // decorated name of the patch definition
    std::string display;
    NarrowReason reason = NarrowReason::Unchanged;
    std::string detail;   // e.g. "byte 12 differs", "relocation to 'g' resolves to 0x..., expected 0x..."
    bool from_prior = false;  // WP-25: judged against the record of an earlier patch (not against the module file)
    // WP-57: kept because the callee step could not prove a called new static function to be the old code there (the inlined-
    // static proofs say "cannot be proven unchanged", not "changed").
    bool unproven = false;
};

struct NarrowInput {
    const coff::Object* object = nullptr;
    const bind::BindPlan* plan = nullptr;
    // The module as stored on disk (ImageKind::OnDisk); may be null: nothing can then be compared.
    const pe::Image* old_image = nullptr;
    std::uint64_t load_base = 0;  // where the module is loaded in the process
    // True when the old entry currently redirects to an earlier patch (always kept: the new code may equal the
    // original but the entry must still be re-pointed).
    std::function<bool(std::uint64_t old_va)> is_hooked;
    bool enabled = true;
    // WP-25: the recorded code that currently runs at `old_va` (what an earlier patch put there: the target of the hook
    // when the entry is hooked, else the entry itself when an earlier patch defined the function). Null: nothing is
    // known, and an entry that is hooked or defined by a patch is kept. May be empty.
    std::function<std::shared_ptr<const CodeImage>(std::uint64_t old_va)> prior_code;
    // WP-57 (ADR-0046), the callee step of the inlined-static proofs (prove_inlined_lambda_statics,
    // prove_inlined_owner_statics; hook narrowing never uses it). A relocation of the judged code to a static function the
    // patch introduces (PatchNewFunction: a template instantiation over a closure, which the planner never pairs) counts as
    // the old code's target when that target starts an old procedure of the same code size (`old_proc_size`: the code size
    // of the old procedure that starts exactly at an RVA, nullopt when none does) and the new function compares equal with it
    // by the same rules, recursively. A pair being compared counts as equal (recursion); more than `max_callee_pairs` pairs
    // in one judgement cannot be judged. Logs `WP57-CALLEE caller=... callee=... old va=... verdict=...` (trace).
    bool follow_new_callees = false;
    std::function<std::optional<std::uint32_t>(std::uint32_t rva)> old_proc_size;
    std::size_t max_callee_pairs = 64;
};

struct NarrowResult {
    std::vector<bind::HookEntry> hooks;  // the hooks to install, in plan order, one per old entry (the first kept entry of an old entry wins)
    std::vector<NarrowDecision> decisions;  // one per plan hook (kept and dropped), in plan order
    // WP-25 counts, after merging plan hooks that share an old entry: planned - dropped == hooks.size().
    std::size_t planned = 0;  // distinct old entries the planner wanted to hook
    std::size_t dropped = 0;  // of those, the ones not hooked
};

// WP-25: records the code of functions defined in the new object, for `prior_code` of a later patch. One recorder per object:
// the maps over the plan and the resolved relocations of each section are built once.
class CodeRecorder {
public:
    explicit CodeRecorder(const NarrowInput& input);  // `input` must outlive the recorder
    ~CodeRecorder();
    CodeRecorder(const CodeRecorder&) = delete;
    CodeRecorder& operator=(const CodeRecorder&) = delete;
    // nullopt (and `*why`, when given) when it cannot be judged: such a function is simply kept next time.
    std::optional<CodeImage> capture(const std::string& symbol, std::string* why = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// One-off convenience over CodeRecorder.
std::optional<CodeImage> capture_code(const NarrowInput& input, const std::string& symbol, std::string* why = nullptr);

// WP-25: names of the data symbols the object defines that are bound to the old image's storage (BindOldData) and whose
// initial contents differ from the old image's: an edit that only changes these compiles to "identical code" but is not applied
// (the patch's copy is dead, initialisers do not run again). At most `limit` names. Symbols that cannot be judged are left out.
std::vector<std::string> bound_data_changes(const NarrowInput& input, std::size_t limit = 5);

// Never fails: whatever cannot be judged is kept. Every decision is logged at trace level.
NarrowResult narrow_hooks(const NarrowInput& input);

// WP-40 (R-48 item 5, ADR-0043 rule (d)). For every BindPlan::lambda_sets entry (lambdas of one function that the type
// records cannot tell apart; the planner paired their statics by the compiler's numbers), each new call operator is
// compared with each old one by the rules above, under the hypothesis that its statics are the old lambda's:
//   1. pairs that are equivalent one-to-one are paired by that, also against the numbers (the lambdas changed places);
//   2. if exactly one lambda is left on each side (its body changed), those two are paired;
//   3. otherwise (several changed, or a comparison that cannot be made) the lambdas left pair by their number, with the
//      warning CML3602; one whose number's old lambda a code-equivalent new lambda took is refused (CML3603).
// String literals and other fresh constants are compared by their bytes (SNAME("a") is not SNAME("b")).
struct LambdaPairing {
    std::map<std::string, std::string> closure_pairing;  // patch closure -> old closure, only where it is not the number's
    std::vector<Diag> warnings;                          // CML3602
    std::vector<Diag> errors;                            // CML3603: a lambda left over whose number's old lambda is taken
};
LambdaPairing pair_lambdas_by_code(const NarrowInput& input);

// WP-40 (second high review, ADR-0043 ruling). For every BindPlan::bitmask_guards entry (a `$S<n>` bitmask bound to the old
// bitmask): the bits are handed out to the function's dynamically initialised statics in the order they are reached, so
// the old bitmask fits only while those statics are the same set in the same order. That is proven when the function's
// code is unchanged (the comparison above, with the plan's decisions: the same bits tested and set, on the same statics);
// otherwise, or when it cannot be compared, the result holds a CML3600 for the bitmask. Logs
// `WP40-BITMASK guard=... function=... proven=unchanged|refused`.
std::vector<Diag> prove_bitmask_guards(const NarrowInput& input);

// WP-46 (F-41.2, ADR-0043 ruling D1). For every BindPlan::hashed_lambdas entry (a lambda cl names by a hash, whose functions
// and statics the planner paired by that name): each of its hooked functions is compared with the old function it is hooked
// to, by the rules above and with the plan's decisions (its statics at the old lambda's addresses). The pairing is proven when
// every lambda's code is unchanged except at most one lambda's (rule (d): the one changed lambda is the edited one). Otherwise,
// or when a function cannot be compared, the result holds one CML3630 naming the lambdas. Logs
// `WP46-HASHED-LAMBDAS object=... closures=N unchanged=N changed=N unknown=N proven=unchanged|one-changed|refused`.
std::vector<Diag> prove_hashed_lambdas(const NarrowInput& input);

// WP-53 (R-61, ADR-0045). For every BindPlan::inlined_lambda_statics entry (a lambda's static bound to a record directly in
// the procedure it was inlined into, which may as well be that procedure's own static): the patch's version of the
// procedure (the hook whose old entry is the procedure's) is compared with the old code, by the rules above and with the
// plan's decisions. Unchanged proves the pairing: the old procedure used that record exactly as the new one uses the
// lambda's static. Anything else (changed, not hooked, not comparable, an earlier patch's code that differs) gives one
// CML3207 per listed static of that procedure. One verdict per procedure. Logs `WP53-INLINED-PROOF static=... proc=...
// proven=unchanged|refused` (trace) per static and `WP53-INLINED object=... listed=N proven=N refused=N skipped=N` (Info, when the
// plan lists any).
// WP-57: with `verdicts`, the verdicts per procedure ((its old entry RVA, its name) -> "" proven, else why not; a procedure
// without an RVA is never kept) are taken from and
// added to it, so prove_inlined_owner_statics on the same plan judges each procedure once. Both proofs use the callee step
// (NarrowInput::old_proc_size).
using InlinedProcVerdicts = std::map<std::pair<std::uint32_t, std::string>, std::string>;  // (entry RVA, procedure name)
std::vector<Diag> prove_inlined_lambda_statics(const NarrowInput& input, InlinedProcVerdicts* verdicts = nullptr);

// WP-57 (R-59, ADR-0046). For every BindPlan::inlined_owner_statics entry (an own static of a function whose old procedure
// holds its record and inlined closure code, which may as well be the static of a lambda the edit deleted, R-54): the same
// proof as WP-53's, on the static's own procedure. Unchanged under the plan proves the pairing; anything else gives one
// CML3207 per listed static, with WP-51's message and why the proof failed. Logs `WP57-OWNER-PROOF static=... proc=...
// proven=unchanged|refused` (trace) and `WP57-OWNER object=... listed=N proven=N refused=N skipped=N` (Info, when the plan
// lists any).
std::vector<Diag> prove_inlined_owner_statics(const NarrowInput& input, InlinedProcVerdicts* verdicts = nullptr);

}  // namespace cml::core
