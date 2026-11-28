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
//     where on every path back from the `or` the register is loaded from the guard (`mov r32, [guard]`, `mov r8, [guard+n]`)
//     with only test / cmp / bt / jumps between; or `or [guard], imm` / `bts [guard], imm8` (32-bit, bit < 32) directly.
//     Other uses: loads, `test`, `cmp`, `bt` (reads), and the clearing of a bit after an initialiser threw (`and` / `btr`,
//     an EH funclet's), which claims nothing.
//   * a thread-safe guard (`?$TSS<n>@<block>@4HA`, both compilers' default): a claim is `lea rcx, [guard]` immediately
//     followed by a call of `_Init_thread_header`, reached only from that lea. Other uses: loads and `cmp` (reads),
//     `lea rcx, [guard]` + `call _Init_thread_footer` (the end of a region) or `_Init_thread_abort` (an EH funclet's).
// A claim's region is every instruction reachable from it (direct branches followed, calls return) that the code cannot
// reach from any function start without passing the claim, up to the next use of the guard (a bitmask) or the guard's
// footer (thread-safe: every path must end there or rejoin the rest of the code; none may end at a ret, a tail call or the
// end of the section, only at int3 or ud2 right behind a call, an inlined throw; no branch out of the section and no fall
// into another function in either kind). Nothing may enter the region from outside it: a function start, every symbol of
// the section and every relocation target in it (GuardEntryRef) count as entered from outside, and no fall-through
// crosses a function start. Its static is the ONE candidate (the statics the guard's name ties:
// its function's or its block's) that the region WRITES: any instruction that references a candidate writes it unless it
// is on an explicit list of pure reads (mov/movzx/movsx loads, cmp, test, bt, the ALU forms that read memory into a
// register, the SSE loads; so a store, a read-modify-write, cmpxchg8b/16b, an x87 store or `lea` all write); a candidate
// the region only reads does not count, and a region that writes none or several is not read. A register stored to the
// guard after an `or` (a claim) or an `and` (a release) must be loaded from the guard on every path back. Every
// function of a section that uses the guard must decode completely (the agent's length decoder, from the start of the
// section and from every function symbol, as WP-36's SiteProver: every relocation exactly a displacement or an immediate
// of one decoded instruction), with no indirect jump, and every use of the guard must be one of these forms. A guard
// referenced from data other than debug information (by its name or its section), through ANY other symbol of its section
// (an alias: a section symbol, a label, another name; in any code section), or at an offset that is not its own is not
// read; nor is one without any claim; nor a section that uses the guard and names another symbol of a candidate's section.
// Each bit (or the thread-safe guard) must belong to one static, and each static to one bit.
//
// What it proves: the statics initialised under the guard, and so the statics of its scope that are not (no claim's region
// writes them: constant-initialised, or guarded by another guard of their block). It also lists the candidates whose
// address the code puts in a register or an immediate (`address_taken`): a region could write such a static through that
// address while naming another, so the planner keeps a bound static only when its address is not taken. The residual
// (docs/wp/WP-63.md): a region that writes a static through an address read from memory or returned by a call (the
// initialiser of a new static writing a kept one: it runs once, the program's own semantics).

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
    bool target_is_section = false;  // the target is a section symbol (`.bss`, ...); informational: any alias refuses
    std::int32_t target_section = 0;  // the target's section number (1-based; <= 0: undefined, absolute, ...)
};

// A place in a code section that something outside the code's own branches may reach (review round 3): a symbol defined
// there (any type: a label, a static, a section symbol) or the target of a relocation from any linked section of the
// object. `offsets`: where it may be (one offset, or for a REL32 whose instruction is not known the offsets that its tail
// allows: relocation_target_offsets). Every one of them that is an instruction start is an entry: entered from outside.
// None at an instruction start: refused (control lands inside an instruction), unless `state_key` (a relocation from
// `.xdata`: clang's ip2state keys sit one byte behind a call's return address, they are lookup keys, not entries).
struct GuardEntryRef {
    std::vector<std::uint32_t> offsets;
    bool state_key = false;
    std::string from;  // for messages: what refers there
};

// One code section of the patch.
struct GuardCodeSection {
    std::string name;  // for messages (".text$mn", "section 7")
    std::span<const std::byte> data;
    std::vector<GuardCodeReloc> relocs;  // any order
    std::vector<std::uint32_t> function_starts;  // offsets of the section's function symbols (any order); 0 is added
    std::vector<GuardEntryRef> entry_refs;       // further places entered from outside (see GuardEntryRef)
    std::string outside_model;  // non-empty: why the section cannot be read at all (a function of it has catch handlers)
};

// The offsets in its target's section that a relocation may point at: `target_value` (the symbol's offset in its
// section) + the addend in `field` (4 bytes; 8 for ADDR64), and for the REL32 family the bytes behind the field, which the
// relocation does not say: exactly 0 when the field follows a call / jmp / jcc rel32 opcode (`before`: the two bytes in
// front of the field, 0 where there are none), else every tail from k to 4 (REL32_k). Empty for a type that names no offset
// (SECTION, SECREL, ...).
std::vector<std::uint32_t> relocation_target_offsets(std::uint16_t raw_type, std::span<const std::byte> field, std::uint8_t before2, std::uint8_t before1,
                                                     std::uint32_t target_value);

struct GuardClaim {
    std::string section;
    std::uint32_t at = 0;    // the claim (the store, or the header call)
    std::uint32_t end = 0;   // the end of its region
    std::uint32_t bits = 0;  // a bitmask's bit (one bit set); 0 for a thread-safe guard
    std::string static_name; // the one candidate its region writes
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

// `code`: the code sections of the patch that refer to the guard or to its section (others are skipped). `guard_section`:
// the guard's own section (any relocation into it that does not name the guard refuses). `candidates`: name -> section of
// the statics the guard's name ties. `data_reference`: a non-code, non-debug section of the patch refers to the guard or
// to its section (then nothing is proved).
GuardCodeReading read_guard_code(std::span<const GuardCodeSection* const> code, std::string_view guard, std::int32_t guard_section, GuardCodeKind kind,
                                 const std::map<std::string, std::int32_t, std::less<>>& candidates, std::string_view data_reference = {});

}  // namespace cml::core::bind
