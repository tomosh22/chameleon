#pragma once

// WP-63 (R-69, ADR-0047): which statics an inline function's external guard guards, read from the patch object's code.
//
// An inline function's statics and their guard are external symbols, each in a COMDAT of its own, and a static that is
// constant-initialised (`static int n = 0;`) defines the same symbols as one that is initialised dynamically. Which of a
// function's statics test the guard only its code shows. WP-59 (ADR-0046) therefore refused (CML3600) an inline function
// that keeps a constant-initialised static and gains a dynamic one. This file reads the guard's uses from the code and says
// which statics are initialised under it; anything it does not recognise is "not proved" and the planner refuses as before.
//
// Two kinds of guard:
//   * a bitmask (`??_B<scope>@5<n>`: clang-cl and cl without thread-safe statics): a claim is the store that sets ONE bit,
//     `or r32, imm` / `bts r32, imm8` immediately followed by `mov [guard], r32` (or `mov [guard], r8` after `or r8, imm8`),
//     or `or [guard], imm` / `bts [guard], imm8` directly. Its region runs from the claim to the first instruction that is
//     a branch target, refers to the guard again, or ends the flow (jmp, ret, int3/ud2): the code that runs once the bit
//     is set, in the layout both compilers use (clang: an out-of-line block that jumps back; cl: a block skipped by a jne).
//     Other uses: loads, `test`, `cmp`, `bt` (reads), and the clearing of a bit after an initialiser threw (`and` / `btr`,
//     an EH funclet's), which claims nothing.
//   * a thread-safe guard (`?$TSS<n>@<block>@4HA`, both compilers' default): a claim is `lea rcx, [guard]` immediately
//     followed by a call of `_Init_thread_header`; its region runs to `lea rcx, [guard]` + `call _Init_thread_footer`, in a
//     straight line (no jmp, ret or trap before it, no branch into it from outside, no second header). Other uses: loads and
//     `cmp` (reads), `lea rcx, [guard]` + `call _Init_thread_abort` (an EH funclet's).
// A claim's static is the ONE candidate static (the statics the guard's name ties: its function's or its block's) that the
// region refers to; a region that refers to none or to more than one is not read. Every reference to the guard in every
// code section of the patch must be one of these forms, in an instruction the sweep decoded (the agent's length decoder,
// from the start of the section and from every function symbol, as WP-36's SiteProver: every relocation must be exactly a
// displacement or an immediate of one decoded instruction). A guard referenced from data other than debug information,
// through a section symbol, or at an offset that is not its own is not read; nor is one without any claim. Each bit (or the
// thread-safe guard) must belong to one static, and each static to one bit.
//
// What it proves: the statics initialised under the guard, and so the statics of its scope that are not (no claim's region
// refers to them: constant-initialised, or guarded by another guard of their block). It also lists the candidates whose
// address the code puts in a register or an immediate (`address_taken`): a region could write such a static through that
// address while naming another, so the planner keeps a bound static only when its address is not taken. The residual
// (docs/wp/WP-63.md): a region that writes a static through an address read from memory or returned by a call.

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cml::core::bind {

enum class GuardCodeKind : std::uint8_t {
    Bitmask,     // `??_B<scope>@5<n>`: one bit per static of the function
    ThreadSafe,  // `?$TSS<n>@<block>@4HA`: one guard per static
};

// One relocation of a code section, with what the planner knows of its target.
struct GuardCodeReloc {
    std::uint32_t offset = 0;      // of the field, in the section
    std::uint32_t width = 0;       // bytes the relocation patches (4 or 8); 0 = not known (fits no operand)
    std::int8_t rel_tail = -1;     // REL32_k: k (bytes between the field and the end of the instruction); -1: not REL32*
    std::string target;            // the decorated name of the target symbol
    bool target_is_section = false;  // the target is a section symbol (`.bss`, ...)
    std::int32_t target_section = 0;  // the target's section number (1-based; <= 0: undefined, absolute, ...)
};

// One code section of the patch.
struct GuardCodeSection {
    std::string name;  // for messages (".text$mn", "section 7")
    std::span<const std::byte> data;
    std::vector<GuardCodeReloc> relocs;  // any order
    std::vector<std::uint32_t> function_starts;  // offsets of the section's function symbols (any order); 0 is added
};

struct GuardClaim {
    std::string section;
    std::uint32_t at = 0;    // the claim (the store, or the header call)
    std::uint32_t end = 0;   // the end of its region
    std::uint32_t bits = 0;  // a bitmask's bit (one bit set); 0 for a thread-safe guard
    std::string static_name; // the one candidate its region refers to
};

struct GuardCodeReading {
    bool proved = false;
    std::string why;  // when not proved: the first use that is not read, naming its section, offset and bytes
    std::vector<GuardClaim> claims;
    std::set<std::string> statics;  // the candidates initialised under the guard
    // The candidates whose address the code that uses the guard puts in a register or an immediate (`lea`, a relocated
    // immediate, anything but a RIP-relative memory operand), inside a region or not: such a static could be written
    // through that address in a region that names another static.
    std::set<std::string> address_taken;
    std::size_t references = 0;     // the guard's references that were read
};

// `code`: every code section of the patch that refers to the guard (others are skipped). `guard_section`: the guard's own
// section (a relocation to that section's symbol is not read). `candidates`: name -> section of the statics the guard's
// name ties. `data_reference`: a non-code, non-debug section of the patch refers to the guard (then nothing is proved).
GuardCodeReading read_guard_code(std::span<const GuardCodeSection> code, std::string_view guard, std::int32_t guard_section, GuardCodeKind kind,
                                 const std::map<std::string, std::int32_t, std::less<>>& candidates, std::string_view data_reference = {});

}  // namespace cml::core::bind
