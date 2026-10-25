#pragma once

// Binding object generator (plan section 4.5), WP-06.
//
// `binding.obj` satisfies the linker for symbols that a patch object references but that must keep
// resolving to the OLD module's definition ("bind-to-old"), and makes image-relative references to
// them legal. No /FORCE:UNRESOLVED is needed.
//
// Layout (one regular x64 COFF object, written by write()):
//
//   .text  (CODE | EXECUTE | READ, 16-byte aligned)
//     one 16-byte thunk per FUNCTION symbol, in input order:
//         FF 25 <rel32>        jmp qword ptr [rip + rel32]      6 bytes, rel32 = IMAGE_REL_AMD64_REL32
//                                                                 against __cml_slot_<name>
//         CC CC CC CC CC CC CC CC CC CC                         10 bytes int3 padding
//     The thunk symbol is named exactly <name> (EXTERNAL, type DTYPE_FUNCTION), so every reference
//     in the patch objects (call/jmp rel32, ADDR32NB in .xdata/.pdata, ADDR64 function pointers in
//     .rdata) resolves to the thunk.
//
//   .data  (INITIALIZED_DATA | READ | WRITE, 8-byte aligned)
//     one 8-byte zero slot per FUNCTION symbol:  EXTERNAL symbol __cml_slot_<name>, and
//     one 8-byte zero placeholder per DATA symbol: EXTERNAL symbol named exactly <name>.
//
// Why 16 bytes, not the plan's "14": FF 25 rel32 is 6 bytes. 14 bytes is the size of the other
// common form (FF 25 00 00 00 00 + an inline 8-byte target, which needs no slot). The plan's
// retargeter writes SLOTS (a separate, 8-byte aligned, atomically writable qword that is also what
// `jmp [rip+slot]` reads), so the 6-byte form is used and padded to 16 so that every thunk starts
// on a 16-byte boundary (the section alignment) and a slot overwrite never shares a cache line
// with code that is patched.
//
// Unwind info: thunks have none. A function without a RUNTIME_FUNCTION entry is a leaf function
// for the x64 unwinder (return address at [rsp]); the thunk neither pushes nor allocates, so the
// unwinder gets the right answer, and the target it jumps to supplies its own unwind data. No
// .pdata/.xdata is emitted.
//
// Locating slots: every slot is an EXTERNAL symbol `__cml_slot_<name>`, hence a public (S_PUB32) in
// the patch PDB with its RVA, and also reachable through the DLL's symbol-less address via the
// thunk's rel32. Data placeholders are public under their own names.
//
// Defined-data symbols that are bind-to-old need nothing here (the patch object's own definition
// satisfies the linker).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "cml/coff/writer.h"

namespace cml::coff {

enum class BindKind : std::uint8_t { Function, Data };

struct BindSymbol {
    std::string name;  // decorated linker name, exactly as the patch object references it
    BindKind kind = BindKind::Function;
};

inline constexpr std::string_view kSlotPrefix = "__cml_slot_";
inline constexpr std::uint32_t kThunkSize = 16;
inline constexpr std::uint32_t kSlotSize = 8;

// "__cml_slot_<name>".
std::string slot_name(std::string_view function_name);

// Builds the binding object model (so tests and the broker can inspect or extend it).
// CML3310: empty name, NUL in a name, or a linker-provided name (__ImageBase).
// CML3311: duplicate name, or a name that collides with another symbol's slot name.
// CML3312: the object would exceed a COFF limit.
Result<ObjectModel> make_binding_model(std::span<const BindSymbol> symbols);

// make_binding_model() + write().
Result<ByteVector> make_binding_object(std::span<const BindSymbol> symbols);

}  // namespace cml::coff
