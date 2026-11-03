// Windows half of the application hooks (WP-17): reading the hook records of the enabled modules and of the
// patch images of this process, and calling the hooks. See user_hooks.h for the rules.
//
// Agent rules: no exceptions (a hook that raises a processor exception, or a C++ exception thrown across our
// frames, is caught with SEH around the call and reported), no allocation while threads are suspended (none of
// this runs then: callers invoke it before the stop-the-world window opens and after it closed), every
// function callable from any thread.
#if defined(_WIN32)

#include <windows.h>
#include <malloc.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Chameleon_API.h"
#include "agent.h"
#include "cml/util/diag.h"
#include "cml/util/log.h"
#include "cml/util/path.h"
#include "hook/user_hook_ids.h"
#include "hook/user_hooks.h"
#include "loader/patch_loader.h"

namespace cml::agent::user_hooks {

// The layout the macros in Chameleon_API.h write is the layout parse_section reads.
static_assert(sizeof(cml_hook_entry) == kRecordSize, "hook record size");
static_assert(CML_HOOK_MAGIC == kMagic, "hook record magic");
static_assert(CML_HOOK_PREPATCH == static_cast<int>(Kind::PrePatch), "kind ids");
static_assert(CML_HOOK_POSTPATCH == static_cast<int>(Kind::PostPatch), "kind ids");
static_assert(CML_HOOK_COMPILE_START == static_cast<int>(Kind::CompileStart), "kind ids");
static_assert(CML_HOOK_COMPILE_SUCCESS == static_cast<int>(Kind::CompileSuccess), "kind ids");
static_assert(CML_HOOK_COMPILE_ERROR == static_cast<int>(Kind::CompileError), "kind ids");
static_assert(CML_HOOK_PRERESTART == static_cast<int>(Kind::PreRestart), "kind ids");

namespace {

struct KeyContext {
    std::uint64_t image_end = 0;
};

// Reads a NUL-terminated string from this process with ReadProcessMemory (an error instead of a fault).
bool read_key(void* context, std::uint64_t address, std::string* out) {
    const auto* key_context = static_cast<const KeyContext*>(context);
    out->clear();
    while (out->size() < kMaxKeyBytes) {
        if (address >= key_context->image_end) {
            return false;
        }
        char chunk[64];
        const std::uint64_t left = key_context->image_end - address;
        const SIZE_T want = static_cast<SIZE_T>(left < sizeof(chunk) ? left : sizeof(chunk));
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)),
                               chunk, want, &got) ||
            got != want) {
            return false;
        }
        for (SIZE_T i = 0; i < got; ++i) {
            if (chunk[i] == '\0') {
                return !out->empty();
            }
            out->push_back(chunk[i]);
            if (out->size() >= kMaxKeyBytes) {
                return false;  // an unterminated or absurdly long key is not one of ours
            }
        }
        address += got;
    }
    return false;
}

bool read_bytes(std::uint64_t address, std::size_t size, std::vector<std::uint8_t>* out) {
    out->assign(size, 0);
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)),
                             out->data(), size, &got) &&
           got == size;
}

// The hook records of one image (possibly none).
Source read_source(bool patch, std::uint64_t base, std::uint64_t size, const std::string& name) {
    Source source;
    source.patch = patch;
    source.name = name;
    source.base = base;
    std::vector<std::uint8_t> headers;
    const std::size_t header_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(size, 4096));
    if (!read_bytes(base, header_bytes, &headers)) {
        return source;  // not readable (just unloaded?): nothing to hook
    }
    std::vector<SectionRange> sections;
    if (!find_hook_sections(headers.data(), headers.size(), &sections) || sections.empty()) {
        return source;
    }
    KeyContext key_context;
    key_context.image_end = base + size;
    for (const SectionRange& section : sections) {
        if (section.rva >= size || section.size == 0) {
            continue;
        }
        const std::size_t bytes = std::min<std::size_t>(
            {static_cast<std::size_t>(section.size), kMaxSectionBytes, static_cast<std::size_t>(size - section.rva)});
        std::vector<std::uint8_t> data;
        if (!read_bytes(base + section.rva, bytes, &data)) {
            CML_LOG(Warn, "%s",
                    Diag::make(ids::kSectionUnreadable, "the hook section of " + name + " cannot be read",
                               "The module may have been unloaded; its hooks are skipped.")
                        .format()
                        .c_str());
            continue;
        }
        ParseStats stats;
        parse_section(data.data(), data.size(), base, size, &read_key, &key_context, &source.records, &stats);
        if (stats.rejected != 0) {
            CML_LOG(Warn, "%s",
                    Diag::make(ids::kEntryRejected,
                               std::to_string(stats.rejected) + " hook record(s) of " + name +
                                   " point outside the image or have an unknown kind",
                               "Build the module and the hooks with the same Chameleon_API.h; the records are skipped.")
                        .format()
                        .c_str());
        }
        if (stats.too_many) {
            CML_LOG(Warn, "%s",
                    Diag::make(ids::kTooManyHooks,
                               name + " has more than " + std::to_string(kMaxRecordsPerImage) + " hook records",
                               "Register fewer hooks per module; the rest are skipped.")
                        .format()
                        .c_str());
        }
    }
    return source;
}

std::string hex8(unsigned long value) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string text(8, '0');
    for (int i = 7; i >= 0; --i) {
        text[static_cast<std::size_t>(i)] = kDigits[value & 0xF];
        value >>= 4;
    }
    return text;
}

// Debugger-related exceptions are not ours to swallow.
int filter(unsigned long code) {
    switch (code) {
        case 0x80000003UL:  // EXCEPTION_BREAKPOINT
        case 0x80000004UL:  // EXCEPTION_SINGLE_STEP
        case 0x40010006UL:  // DBG_PRINTEXCEPTION_C
        case 0x4001000AUL:  // DBG_PRINTEXCEPTION_WIDE_C
        case 0x406D1388UL:  // MS_VC_EXCEPTION (thread name)
            return EXCEPTION_CONTINUE_SEARCH;
        default:
            return EXCEPTION_EXECUTE_HANDLER;
    }
}

// Set when a hook overflowed the stack and _resetstkoflw() could not restore the guard page: the next overflow
// on this thread would kill the process, so no more hooks run on it.
thread_local bool t_refuse_hooks = false;

// Calls one hook. Plain data only: a function with __try cannot hold objects that need unwinding.
unsigned long call_hook(Kind kind, std::uint64_t fn, const char* text) noexcept {
    unsigned long code = 0;
    __try {
        if (kind == Kind::CompileError) {
            reinterpret_cast<void (*)(const char*)>(static_cast<std::uintptr_t>(fn))(text != nullptr ? text : "");
        } else {
            reinterpret_cast<void (*)()>(static_cast<std::uintptr_t>(fn))();
        }
    } __except (code = GetExceptionCode(), filter(code)) {
        if (code == 0xC00000FDUL) {  // EXCEPTION_STACK_OVERFLOW: give the guard page back (as WP-08 does for initialisers)
            if (_resetstkoflw() == 0) {
                t_refuse_hooks = true;
            }
        }
        return code != 0 ? code : 0xFFFFFFFFUL;
    }
    return 0;
}

}  // namespace

Session::Session(const std::vector<std::string>& enabled_modules) {
    // Pin the patch images first, then look at the modules: nothing listed below can be unloaded while the
    // session lives.
    lease_ = loader::PatchLoader::shared().acquire_hook_lease();
    Result<std::vector<proc::ModuleInfo>> modules = proc::enumerate_modules(0);  // not under the loader's lock
    if (modules) {
        modules_ = std::move(*modules);
    }
    for (const proc::ModuleInfo& module : modules_) {
        const bool enabled = std::any_of(enabled_modules.begin(), enabled_modules.end(),
                                         [&module](const std::string& e) { return Agent::same_module(e, module.path); });
        if (enabled) {
            // A source exists for every enabled module, with or without records: patches are scoped to it.
            sources_.push_back(read_source(false, module.base, module.size, std::string(path::base_name(std::string_view(module.path)))));
        }
    }
    const std::vector<loader::PatchLoader::HookImage>& images = lease_.images();
    for (std::size_t i = 0; i < images.size(); ++i) {
        Source source = read_source(true, images[i].range.base, images[i].range.size, "patch image #" + std::to_string(i + 1));
        source.committed = images[i].committed;
        source.target_base = images[i].target_base;
        sources_.push_back(std::move(source));  // one per leased image (even without records): refresh relies on the order
    }
}

void Session::refresh_patch_flags() {
    lease_.refresh();
    const std::vector<loader::PatchLoader::HookImage>& images = lease_.images();
    std::size_t next = 0;
    for (Source& source : sources_) {
        if (source.patch && next < images.size()) {
            source.committed = images[next].committed;
            source.target_base = images[next].target_base;
            ++next;
        }
    }
}

void Session::commit(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& hooks) {
    loader::PatchLoader::shared().commit_hooked(hooks, modules_);
    refresh_patch_flags();
}

std::size_t Session::run(Kind kind, const char* text) {
    if (t_refuse_hooks) {
        CML_LOG(Error, "%s",
                Diag::make(ids::kStackNotRestored, "no application hook runs on this thread any more",
                           "An earlier hook overflowed the stack and the guard page could not be restored; restart the application.")
                    .format()
                    .c_str());
        return 0;
    }
    const std::vector<Record> selected = select_hooks(kind, sources_);
    std::size_t called = 0;
    for (const Record& record : selected) {
        if (t_refuse_hooks) {
            break;
        }
        CML_LOG(Debug, "hooks: calling %s hook %s", to_string(kind), record.key.c_str());
        const unsigned long fault = call_hook(kind, record.fn, text);
        ++called;
        if (fault != 0) {
            CML_LOG(Error, "%s",
                    Diag::make(ids::kHookFaulted,
                               std::string("the ") + to_string(kind) + " hook " + record.key + " raised exception 0x" + hex8(fault),
                               "Fix the hook; the next hook of this event runs and the application continues.")
                        .format()
                        .c_str());
        }
        if (t_refuse_hooks) {
            CML_LOG(Error, "%s",
                    Diag::make(ids::kStackNotRestored,
                               std::string("the ") + to_string(kind) + " hook " + record.key +
                                   " overflowed the stack and the guard page could not be restored",
                               "No further hook runs on this thread; fix the recursion in the hook and restart the application.")
                        .format()
                        .c_str());
        }
    }
    return called;
}

}  // namespace cml::agent::user_hooks

#endif  // _WIN32
