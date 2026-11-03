#pragma once

// Application hooks (plan section 4.9, WP-17): CML_PREPATCH_HOOK, CML_POSTPATCH_HOOK, CML_COMPILE_START_HOOK,
// CML_COMPILE_SUCCESS_HOOK, CML_COMPILE_ERROR_HOOK, CML_PRERESTART_HOOK.
//
// The macros in Chameleon_API.h put one 24-byte record {magic, kind, function, key} per hook into the section
// ".cmlhk" of the module that contains them. This file finds those records and decides which ones to call.
//
//   * parse_section / find_hook_sections / select_hooks are portable (no Windows calls) and unit-tested on
//     every host.
//   * collect_hooks / run_hooks (user_hooks_win.cpp) read the enabled modules and the live patch images of
//     this process and call the functions.
//
// Which records run (select_hooks). A reload recompiles whole translation units, so the patch image holds a
// copy of every record of such a unit, whether or not its function changed. Calling both the original and
// the copy would run a hook twice. The rule is by key ("<file>|<function expression>", written by the macro):
//   1. every record of an enabled module runs. The original function is reached through the installed hook
//      when a patch replaced it, so it runs the newest code;
//   2. a record of a patch image runs only when no enabled module has a record of the same kind and key (the
//      hook was added by a patch); of several patch images with the same kind and key the newest wins.
// A hook that a patch removes from the source keeps running (its original record is still there).
//
// Nothing here may be called while host threads are suspended (it allocates and calls application code).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cml::agent::user_hooks {

enum class Kind : std::uint32_t {
    PrePatch = 1,
    PostPatch = 2,
    CompileStart = 3,
    CompileSuccess = 4,
    CompileError = 5,
    PreRestart = 6,
};

inline constexpr std::uint32_t kMagic = 0x4B484D43u;
inline constexpr std::size_t kRecordSize = 24;           // sizeof(cml_hook_entry) on x64
inline constexpr std::size_t kMaxRecordsPerImage = 4096;
inline constexpr std::size_t kMaxSectionBytes = 1u << 20;
inline constexpr std::size_t kMaxKeyBytes = 512;

struct Record {
    Kind kind = Kind::PrePatch;
    std::uint64_t fn = 0;
    std::string key;
};

// The records of one image. `patch` is true for a patch image. `name` is for log lines.
struct Source {
    bool patch = false;
    std::string name;
    std::vector<Record> records;
};

// Reads the key string at `address` (up to kMaxKeyBytes, without the terminator); false if unreadable.
using KeyReader = bool (*)(void* context, std::uint64_t address, std::string* out);

struct ParseStats {
    std::size_t rejected = 0;   // records whose function/key lie outside the image or whose kind is unknown
    bool too_many = false;      // kMaxRecordsPerImage reached
};

// Parses a copy of a ".cmlhk" section. Records are found by magic at every 8-byte step (a linker may pad
// between the contributions of two objects). `image_base`/`image_size` bound the function and key pointers.
void parse_section(const std::uint8_t* bytes, std::size_t size, std::uint64_t image_base, std::uint64_t image_size,
                   KeyReader read_key, void* read_context, std::vector<Record>* out, ParseStats* stats);

struct SectionRange {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;  // VirtualSize
};

// Finds the sections whose name is ".cmlhk" in the PE headers held in `headers` (at least SizeOfHeaders
// bytes, or what could be read). Returns false if the headers are not a PE32+ image.
bool find_hook_sections(const std::uint8_t* headers, std::size_t size, std::vector<SectionRange>* out);

// The records to call for `kind`, in call order (rules above). `sources`: enabled modules first, then patch
// images in load order (the order only matters for the newest-wins rule among patches).
std::vector<Record> select_hooks(Kind kind, const std::vector<Source>& sources);

const char* to_string(Kind kind) noexcept;

#if defined(_WIN32)

// Reads the hook records of the enabled modules (paths as the application spelled them) and of the live patch
// images of this process. Problems (unreadable section, rejected record) are logged and skipped.
std::vector<Source> collect_hooks(const std::vector<std::string>& enabled_modules);

// Calls the selected hooks of `kind` on the calling thread, in order. `text` is passed to CompileError hooks
// (UTF-8; may be null for the others). A hook that raises a processor exception is reported (CML5652) and the
// next one runs. Returns how many were called.
std::size_t run_hooks(Kind kind, const std::vector<Source>& sources, const char* text);

// The same, collecting first.
std::size_t run_hooks(Kind kind, const std::vector<std::string>& enabled_modules, const char* text);

#endif

}  // namespace cml::agent::user_hooks
