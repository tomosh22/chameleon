/*
 * Chameleon_API.h - public API of the Chameleon agent (cml_agent.dll).
 *
 * Header-only. The application does NOT link against the agent: cml_create_*_agent() (or the C++
 * cml::CreateDefaultAgent()) LoadLibrary()s "cml_agent.dll" from a directory you pass and resolves
 * its exports with GetProcAddress, so the agent keeps its own C runtime and the application needs
 * no import library. The C API below is the primary interface (usable from C99 and through FFI);
 * the C++ wrapper at the end of the file is a thin layer on top of it.
 *
 * Quick start (C++):
 *
 *     cml::Agent agent = cml::CreateDefaultAgent(L"C:/tools/chameleon");
 *     agent.EnableModule(cml::GetCurrentModulePath(), cml::ModuleOptions::AllImports);
 *     for (;;) { frame(); agent.SyncPoint(); }
 *     cml::DestroyAgent(agent);
 *
 * Threading: every function may be called from any thread. DestroyAgent must not be called
 * concurrently with other calls on the same agent, except threads already blocked in
 * WaitForRequest, which are released with CML5215. Calling any function on a destroyed agent is
 * undefined. DestroyAgent called from inside a call into the same agent (for example from work run
 * by SyncPoint) is refused with CML_STATUS_INVALID_ARGUMENT and destroys nothing.
 * None of them blocks on the broker:
 * requests are queued for the agent's own thread and answered later (see cml_wait_request). The
 * only function that may do real work on the caller's thread is cml_sync_point(), by design.
 * If the broker is not running the agent starts it, keeps retrying in the background and the
 * application runs normally meanwhile.
 *
 * Environment variables read by the agent:
 *   CML_PIPE_NAME      full pipe name to use instead of \\.\pipe\cml_broker_<logon session SID>
 *   CML_BROKER_PATH    full path of cml_broker.exe to auto-start (default: next to cml_agent.dll)
 *   CML_BROKER_ARGS    extra command-line arguments appended when auto-starting the broker
 *   CML_AGENT_LOG      path of a file the agent appends its log to
 *
 * Hooks (file scope, any module the agent watches, also in code you add in a patch):
 *   CML_PREPATCH_HOOK(fn)         void fn(void)  runs before the hooks of a patch are written
 *   CML_POSTPATCH_HOOK(fn)        void fn(void)  runs after they were written (rebuild state by hand)
 *   CML_COMPILE_START_HOOK(fn)    void fn(void)  the broker started compiling
 *   CML_COMPILE_SUCCESS_HOOK(fn)  void fn(void)  the compile finished without errors
 *   CML_COMPILE_ERROR_HOOK(fn)    void fn(const char* message)  the compile failed (UTF-8, one line)
 *   CML_PRERESTART_HOOK(fn)       void fn(void)  reserved: restart is not implemented yet, never called
 * Patch hooks run on the thread that applies the patch: inside SyncPoint() in synchronised mode,
 * on the agent thread in immediate mode. Once the pre-patch hooks ran, the post-patch hooks run too, also when
 * the installation failed (ask cml_last_patch_status()). Compile hooks run inside SyncPoint() in synchronised
 * mode and on a separate worker thread in immediate mode (a slow one never stalls the agent). Several hooks of
 * one kind run in the order the linker placed them. A hook must not call DestroyAgent (refused, CML5655), and a
 * hook on the agent thread (immediate mode, patch hooks) must not block in WaitForRequest (refused, CML5656;
 * polling with timeout 0 is fine).
 *
 * Compiles clean as C99, C++14 and C++20 at /W4 /WX (cl) and -Wall -Wextra -Werror (clang-cl),
 * with WIN32_LEAN_AND_MEAN / NOMINMAX defined or not. The header uses no exceptions and no RTTI.
 */
#ifndef CHAMELEON_API_H
#define CHAMELEON_API_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#if !defined(_WIN32)
#error "Chameleon_API.h: Chameleon supports Windows only"
#endif

#include <windows.h>

/* Bumped on any incompatible change to the agent's exported functions or the structs below. */
#define CML_AGENT_ABI_VERSION 2

/* C++: ordinary inline functions (one definition per module after linking). C: static inline. */
#if defined(__cplusplus)
#define CML_INLINE inline
#elif defined(_MSC_VER) && !defined(__clang__)
#define CML_INLINE static __inline
#else
#define CML_INLINE static inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------ */
/* Types                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/* 0 is success. Anything else is a CMLnnnn diagnostic number (for example 5212 is CML5212) or one
 * of the CML_STATUS_* codes below. Replies from the broker use the same numbers. */
typedef int32_t cml_status;

enum {
    CML_STATUS_OK = 0,
    CML_STATUS_NOT_IMPLEMENTED = 5290, /* the call exists but its feature arrives in a later version */
    CML_STATUS_ABI_MISMATCH = 5291,    /* cml_agent.dll was built for another CML_AGENT_ABI_VERSION */
    CML_STATUS_AGENT_NOT_FOUND = 5292, /* cml_agent.dll is not in the directory you passed */
    CML_STATUS_INVALID_ARGUMENT = 5293,
    CML_STATUS_AGENT_LOAD_FAILED = 5294,
    CML_STATUS_NOT_CONNECTED = 5295,   /* reserved */
    CML_STATUS_TIMEOUT = 5296,         /* the wait elapsed; the request is still open */
    CML_STATUS_UNKNOWN_REQUEST = 5297, /* no such request id: never issued, already consumed, or an answer nobody collected for
                                        * 10 minutes (the agent drops those; FU-02) */
    /* The agent diagnostics an application can get back from a call, named so that "the agent is not
     * running any more" is told apart from "the wait timed out" (FU-01). */
    CML_STATUS_NOT_RUNNING = 5215,     /* the agent was stopped (cml_destroy_agent): nothing is queued, every open
                                        * request was answered with this status; it is final, do not retry */
    CML_STATUS_QUEUE_FULL = 5217       /* 1024 requests are open; wait for answers and try again */
};

/* Immediate: patches are applied by the agent thread as soon as they are ready.
 * Synchronized: they are applied inside cml_sync_point() on the thread that calls it. */
enum { CML_MODE_IMMEDIATE = 0, CML_MODE_SYNCHRONIZED = 1 };

/* Bit flags for cml_enable_module(). */
enum { CML_MODULE_NONE = 0, CML_MODULE_ALL_IMPORTS = 1 };

/* Modifier bits of a hotkey (same values as RegisterHotKey's MOD_* flags). */
enum { CML_HOTKEY_ALT = 1, CML_HOTKEY_CONTROL = 2, CML_HOTKEY_SHIFT = 4, CML_HOTKEY_WIN = 8 };

typedef struct cml_hotkey {
    uint32_t modifiers;   /* CML_HOTKEY_* flags; 0 together with virtual_key 0 means "none" */
    uint32_t virtual_key; /* VK_* code */
} cml_hotkey;

typedef struct cml_hotkeys {
    uint32_t struct_size; /* sizeof(cml_hotkeys) */
    cml_hotkey reload;
    cml_hotkey restart;
} cml_hotkeys;

#define CML_REPLY_MESSAGE_CAPACITY 512

/* The answer to a request, filled by cml_wait_request(). */
typedef struct cml_reply {
    cml_status status;       /* 0 = the broker accepted the request, else a CMLnnnn number */
    uint32_t message_length; /* bytes in `message`, not counting the terminator */
    char message[CML_REPLY_MESSAGE_CAPACITY]; /* UTF-8, truncated if necessary, always terminated */
} cml_reply;

/* ------------------------------------------------------------------------------------------ */
/* Hook registration records (CML_*_HOOK macros). The agent finds them by scanning the section */
/* ".cmlhk" of every enabled module and patch image, so this layout is part of the ABI.       */
/* ------------------------------------------------------------------------------------------ */

/* Generic function pointer; GetProcAddress results and hook functions are converted through it so
 * that the casts stay warning-free under -Wcast-function-type. */
typedef void (*cml_generic_fn)(void);

#define CML_HOOK_MAGIC 0x4B484D43u /* 'C' 'M' 'H' 'K' */

enum {
    CML_HOOK_PREPATCH = 1,
    CML_HOOK_POSTPATCH = 2,
    CML_HOOK_COMPILE_START = 3,
    CML_HOOK_COMPILE_SUCCESS = 4,
    CML_HOOK_COMPILE_ERROR = 5,
    CML_HOOK_PRERESTART = 6
};

typedef struct cml_hook_entry {
    uint32_t magic; /* CML_HOOK_MAGIC */
    uint32_t kind;  /* CML_HOOK_* */
    cml_generic_fn fn;
    const char* key; /* "<file>|<function expression>": names the hook across a patch of its file */
} cml_hook_entry;

/* ------------------------------------------------------------------------------------------ */
/* Agent ABI: the functions cml_agent.dll exports. Applications use the cml_* wrappers further */
/* down instead; this section is what the loader resolves and what the agent implements.      */
/* ------------------------------------------------------------------------------------------ */

typedef struct cml_agent_create_params {
    uint32_t struct_size;      /* sizeof(cml_agent_create_params) */
    uint32_t mode;             /* CML_MODE_* */
    uint32_t host_abi_version; /* CML_AGENT_ABI_VERSION of the header the host was built with */
    uint32_t reserved;         /* must be 0 */
} cml_agent_create_params;

typedef int (*cml_pfn_abi_version)(void);
typedef cml_status (*cml_pfn_create)(const cml_agent_create_params* params, void** out_agent);
typedef cml_status (*cml_pfn_destroy)(void* agent);
typedef cml_status (*cml_pfn_request)(void* agent, const wchar_t* path, uint32_t options,
                                      uint64_t* out_request_id);
typedef cml_status (*cml_pfn_wait_request)(void* agent, uint64_t request_id, uint32_t timeout_ms,
                                           cml_reply* out_reply);
typedef void (*cml_pfn_sync_point)(void* agent);
typedef cml_status (*cml_pfn_call)(void* agent);
typedef cml_status (*cml_pfn_wait_for_reload)(void* agent, uint32_t timeout_ms);
typedef cml_status (*cml_pfn_set_hotkeys)(void* agent, const cml_hotkeys* hotkeys);
typedef int (*cml_pfn_is_connected)(void* agent);
typedef int32_t (*cml_pfn_last_patch_status)(void);

/* Export names, resolved with GetProcAddress. */
#define CML_EXPORT_ABI_VERSION "cml_agent_abi_version"
#define CML_EXPORT_CREATE "cml_agent_create"
#define CML_EXPORT_DESTROY "cml_agent_destroy"
#define CML_EXPORT_ENABLE_MODULE "cml_agent_enable_module"
#define CML_EXPORT_DISABLE_MODULE "cml_agent_disable_module"
#define CML_EXPORT_WAIT_REQUEST "cml_agent_wait_request"
#define CML_EXPORT_SYNC_POINT "cml_agent_sync_point"
#define CML_EXPORT_SCHEDULE_RELOAD "cml_agent_schedule_reload"
#define CML_EXPORT_WAIT_FOR_RELOAD "cml_agent_wait_for_reload"
#define CML_EXPORT_SCHEDULE_RESTART "cml_agent_schedule_restart"
#define CML_EXPORT_SET_GLOBAL_HOTKEYS "cml_agent_set_global_hotkeys"
#define CML_EXPORT_IS_CONNECTED "cml_agent_is_connected"
#define CML_EXPORT_LAST_PATCH_STATUS "cml_agent_last_patch_status"

#if !defined(CML_AGENT_BUILD) /* everything below is for applications, not for the agent itself */

/* ------------------------------------------------------------------------------------------ */
/* C API                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/* A loaded agent. Opaque; create with cml_create_*_agent(), release with cml_destroy_agent(). */
typedef struct cml_agent_s {
    uint32_t mode;
    void* impl; /* the agent's own object */
    cml_pfn_destroy destroy;
    cml_pfn_request enable_module;
    cml_pfn_request disable_module;
    cml_pfn_wait_request wait_request;
    cml_pfn_sync_point sync_point;
    cml_pfn_call schedule_reload;
    cml_pfn_wait_for_reload wait_for_reload;
    cml_pfn_call schedule_restart;
    cml_pfn_set_hotkeys set_global_hotkeys;
    cml_pfn_is_connected is_connected;
} * cml_agent;

/* Path of the module (exe or dll) that contains the code calling this function, which is what you
 * pass to cml_enable_module() to enable "myself". Writes up to `capacity` wide characters
 * including the terminator and the length without the terminator to *length_out (may be NULL).
 * Returns CML_STATUS_INVALID_ARGUMENT if the buffer is too small; 32768 characters always fit. */
CML_INLINE cml_status cml_get_current_module_path(wchar_t* buffer, uint32_t capacity,
                                                  uint32_t* length_out) {
    static int cml_module_anchor = 0; /* any object inside the calling module's image */
    HMODULE module = NULL;
    DWORD length;
    if (length_out != NULL) {
        *length_out = 0;
    }
    if (buffer == NULL || capacity == 0) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&cml_module_anchor, &module)) {
        return CML_STATUS_AGENT_LOAD_FAILED;
    }
    length = GetModuleFileNameW(module, buffer, (DWORD)capacity);
    if (length == 0) {
        buffer[0] = L'\0';
        return CML_STATUS_AGENT_LOAD_FAILED;
    }
    if (length >= (DWORD)capacity) {
        buffer[capacity - 1] = L'\0';
        return CML_STATUS_INVALID_ARGUMENT;
    }
    if (length_out != NULL) {
        *length_out = (uint32_t)length;
    }
    return CML_STATUS_OK;
}

CML_INLINE cml_generic_fn cml_resolve_export(HMODULE module, const char* name) {
    return (cml_generic_fn)GetProcAddress(module, name);
}

/* Returns "<dir>\cml_agent.dll" in a HeapAlloc'd buffer (NULL if out of memory). */
CML_INLINE wchar_t* cml_join_agent_path(const wchar_t* dir, size_t dir_length) {
    static const wchar_t dll_name[] = L"cml_agent.dll";
    const size_t name_length = sizeof(dll_name) / sizeof(dll_name[0]) - 1;
    const size_t separator =
        (dir_length > 0 && dir[dir_length - 1] != L'\\' && dir[dir_length - 1] != L'/') ? 1 : 0;
    wchar_t* path = (wchar_t*)HeapAlloc(GetProcessHeap(), 0,
                                        (dir_length + separator + name_length + 1) * sizeof(wchar_t));
    if (path == NULL) {
        return NULL;
    }
    memcpy(path, dir, dir_length * sizeof(wchar_t));
    if (separator != 0) {
        path[dir_length] = L'\\';
    }
    memcpy(path + dir_length + separator, dll_name, (name_length + 1) * sizeof(wchar_t));
    return path;
}

/* Loads cml_agent.dll from `agent_dir` (a directory; NULL or empty means the directory of the
 * calling module) and starts an agent in the given mode (CML_MODE_*). Returns NULL on failure
 * with the reason in *status_out (may be NULL): CML_STATUS_AGENT_NOT_FOUND,
 * CML_STATUS_ABI_MISMATCH, CML_STATUS_AGENT_LOAD_FAILED or an agent diagnostic. Does not block on
 * the broker.
 *
 * Once the DLL's create function has been called the DLL is never unloaded again, whether or not
 * creation succeeded: it has by then set up process-lifetime state (its private heap, its log
 * sink and log file) that cannot be torn down safely, and later versions install hooks that
 * point into it. A failed creation therefore leaves the DLL loaded; retrying reuses it and
 * initialises nothing twice, so repeated failing calls do not grow memory or handles. A DLL
 * that fails the ABI check never ran any of that and is unloaded again. */
CML_INLINE cml_agent cml_create_agent(const wchar_t* agent_dir, uint32_t mode,
                                      cml_status* status_out) {
    cml_status status = CML_STATUS_OK;
    cml_agent agent = NULL;
    HMODULE module = NULL;
    wchar_t* path = NULL;
    cml_pfn_abi_version abi_version;
    cml_agent_create_params params;
    cml_pfn_create create;
    void* impl = NULL;
    int create_called = 0;
    int missing;

    if (mode != CML_MODE_IMMEDIATE && mode != CML_MODE_SYNCHRONIZED) {
        status = CML_STATUS_INVALID_ARGUMENT;
        goto done;
    }

    if (agent_dir != NULL && agent_dir[0] != L'\0') {
        path = cml_join_agent_path(agent_dir, wcslen(agent_dir));
    } else {
        /* Default: the directory of the calling module. */
        wchar_t* module_path = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, 32768 * sizeof(wchar_t));
        uint32_t length = 0;
        if (module_path != NULL &&
            cml_get_current_module_path(module_path, 32768, &length) == CML_STATUS_OK) {
            while (length > 0 && module_path[length - 1] != L'\\' && module_path[length - 1] != L'/') {
                --length;
            }
            if (length > 0) {
                --length; /* drop the separator; cml_join_agent_path adds it back */
            }
            path = cml_join_agent_path(module_path, length);
        }
        if (module_path != NULL) {
            HeapFree(GetProcessHeap(), 0, module_path);
        }
    }
    if (path == NULL) {
        status = CML_STATUS_AGENT_NOT_FOUND;
        goto done;
    }

    module = LoadLibraryW(path);
    if (module == NULL) {
        const DWORD error = GetLastError();
        status = (error == ERROR_MOD_NOT_FOUND || error == ERROR_FILE_NOT_FOUND ||
                  error == ERROR_PATH_NOT_FOUND)
                     ? CML_STATUS_AGENT_NOT_FOUND
                     : CML_STATUS_AGENT_LOAD_FAILED;
        goto done;
    }

    abi_version = (cml_pfn_abi_version)cml_resolve_export(module, CML_EXPORT_ABI_VERSION);
    if (abi_version == NULL || abi_version() != CML_AGENT_ABI_VERSION) {
        status = CML_STATUS_ABI_MISMATCH;
        goto done;
    }

    agent = (cml_agent)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*agent));
    if (agent == NULL) {
        status = CML_STATUS_AGENT_LOAD_FAILED;
        goto done;
    }
    agent->mode = mode;
    create = (cml_pfn_create)cml_resolve_export(module, CML_EXPORT_CREATE);
    agent->destroy = (cml_pfn_destroy)cml_resolve_export(module, CML_EXPORT_DESTROY);
    agent->enable_module = (cml_pfn_request)cml_resolve_export(module, CML_EXPORT_ENABLE_MODULE);
    agent->disable_module = (cml_pfn_request)cml_resolve_export(module, CML_EXPORT_DISABLE_MODULE);
    agent->wait_request = (cml_pfn_wait_request)cml_resolve_export(module, CML_EXPORT_WAIT_REQUEST);
    agent->sync_point = (cml_pfn_sync_point)cml_resolve_export(module, CML_EXPORT_SYNC_POINT);
    agent->schedule_reload = (cml_pfn_call)cml_resolve_export(module, CML_EXPORT_SCHEDULE_RELOAD);
    agent->wait_for_reload =
        (cml_pfn_wait_for_reload)cml_resolve_export(module, CML_EXPORT_WAIT_FOR_RELOAD);
    agent->schedule_restart = (cml_pfn_call)cml_resolve_export(module, CML_EXPORT_SCHEDULE_RESTART);
    agent->set_global_hotkeys =
        (cml_pfn_set_hotkeys)cml_resolve_export(module, CML_EXPORT_SET_GLOBAL_HOTKEYS);
    agent->is_connected = (cml_pfn_is_connected)cml_resolve_export(module, CML_EXPORT_IS_CONNECTED);
    missing = create == NULL || agent->destroy == NULL || agent->enable_module == NULL ||
              agent->disable_module == NULL || agent->wait_request == NULL ||
              agent->sync_point == NULL || agent->schedule_reload == NULL ||
              agent->wait_for_reload == NULL || agent->schedule_restart == NULL ||
              agent->set_global_hotkeys == NULL || agent->is_connected == NULL;
    if (missing) {
        status = CML_STATUS_ABI_MISMATCH;
        goto done;
    }

    params.struct_size = (uint32_t)sizeof(params);
    params.mode = mode;
    params.host_abi_version = CML_AGENT_ABI_VERSION;
    params.reserved = 0;
    create_called = 1;
    status = create(&params, &impl);
    if (status != CML_STATUS_OK) {
        goto done;
    }
    agent->impl = impl;

done:
    if (path != NULL) {
        HeapFree(GetProcessHeap(), 0, path);
    }
    if (status != CML_STATUS_OK) {
        if (agent != NULL) {
            HeapFree(GetProcessHeap(), 0, agent);
            agent = NULL;
        }
        if (module != NULL && !create_called) {
            /* The agent code never ran (wrong ABI, missing export): safe to unload. After a
             * create attempt the DLL stays loaded, see cml_create_agent(). */
            FreeLibrary(module);
        }
    }
    if (status_out != NULL) {
        *status_out = status;
    }
    return agent;
}

CML_INLINE cml_agent cml_create_default_agent(const wchar_t* agent_dir) {
    return cml_create_agent(agent_dir, CML_MODE_IMMEDIATE, NULL);
}

CML_INLINE cml_agent cml_create_synchronized_agent(const wchar_t* agent_dir) {
    return cml_create_agent(agent_dir, CML_MODE_SYNCHRONIZED, NULL);
}

/* Stops the agent thread and disconnects from the broker; the agent handle is invalid afterwards.
 * Safe to call from any thread except from inside DllMain (it joins the agent thread). NULL is
 * ignored. Contract: must not be called concurrently with other calls on the same agent, except
 * threads already blocked in cml_wait_request(), which are released with CML5215; calling any
 * function on a destroyed agent is undefined. Called from inside a call into the same agent (for
 * example from work run by cml_sync_point()) it destroys nothing and returns
 * CML_STATUS_INVALID_ARGUMENT; the handle then stays valid. Otherwise returns CML_STATUS_OK. */
CML_INLINE cml_status cml_destroy_agent(cml_agent agent) {
    cml_status status;
    if (agent == NULL) {
        return CML_STATUS_OK;
    }
    status = agent->destroy(agent->impl);
    if (status == CML_STATUS_OK) {
        HeapFree(GetProcessHeap(), 0, agent);
    }
    return status;
}

/* Asks the broker to enable `path` (a full path, or a bare file name such as L"Game.dll" that
 * matches a loaded module by name) for hot reload. Returns at once: CML_STATUS_OK means "queued"
 * and *request_id (may be NULL) identifies the request for cml_wait_request(). */
CML_INLINE cml_status cml_enable_module(cml_agent agent, const wchar_t* path, uint32_t options,
                                        uint64_t* request_id) {
    if (request_id != NULL) {
        *request_id = 0;
    }
    if (agent == NULL || path == NULL) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    return agent->enable_module(agent->impl, path, options, request_id);
}

CML_INLINE cml_status cml_disable_module(cml_agent agent, const wchar_t* path,
                                         uint64_t* request_id) {
    if (request_id != NULL) {
        *request_id = 0;
    }
    if (agent == NULL || path == NULL) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    return agent->disable_module(agent->impl, path, 0, request_id);
}

/* Waits up to timeout_ms (0 = just look) for the answer to a request. CML_STATUS_OK: `reply` is
 * filled and the request is consumed (a second wait gives CML_STATUS_UNKNOWN_REQUEST).
 * CML_STATUS_TIMEOUT: no answer yet, ask again later. This is the only blocking call; applications
 * normally poll with timeout 0, tests and tools wait. */
CML_INLINE cml_status cml_wait_request(cml_agent agent, uint64_t request_id, uint32_t timeout_ms,
                                       cml_reply* reply) {
    if (agent == NULL || reply == NULL) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    return agent->wait_request(agent->impl, request_id, timeout_ms, reply);
}

/* Call once per frame on the thread that should apply patches. In synchronized mode, runs the
 * work that the agent queued for the caller's thread; in immediate mode it does nothing and costs
 * a single branch. */
CML_INLINE void cml_sync_point(cml_agent agent) {
    if (agent != NULL && agent->mode == CML_MODE_SYNCHRONIZED) {
        agent->sync_point(agent->impl);
    }
}

/* Same as pressing the reload hotkey. Not implemented yet: returns CML_STATUS_NOT_IMPLEMENTED. */
CML_INLINE cml_status cml_schedule_reload(cml_agent agent) {
    if (agent == NULL) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    return agent->schedule_reload(agent->impl);
}

/* Waits until a scheduled reload finished. Not implemented yet: CML_STATUS_NOT_IMPLEMENTED. */
CML_INLINE cml_status cml_wait_for_reload(cml_agent agent, uint32_t timeout_ms) {
    if (agent == NULL) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    return agent->wait_for_reload(agent->impl, timeout_ms);
}

/* Not implemented yet: returns CML_STATUS_NOT_IMPLEMENTED. */
CML_INLINE cml_status cml_schedule_restart(cml_agent agent) {
    if (agent == NULL) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    return agent->schedule_restart(agent->impl);
}

/* Not implemented yet: returns CML_STATUS_NOT_IMPLEMENTED (after validating the arguments). */
CML_INLINE cml_status cml_set_global_hotkeys(cml_agent agent, const cml_hotkeys* hotkeys) {
    if (agent == NULL || hotkeys == NULL || hotkeys->struct_size < sizeof(cml_hotkeys)) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    return agent->set_global_hotkeys(agent->impl, hotkeys);
}

/* How the most recent patch installation went: 0 = every hook was written, else a CMLnnnn number (for example
 * 4305). Meant for CML_POSTPATCH_HOOK functions: they also run after an installation that FAILED (whenever the
 * pre-patch hooks ran), so that state torn down in a pre-patch hook can always be rebuilt. Callable from any
 * thread, needs no agent handle. CML_STATUS_AGENT_NOT_FOUND when no agent is loaded in this process. */
CML_INLINE cml_status cml_last_patch_status(void) {
    HMODULE module = GetModuleHandleW(L"cml_agent.dll");
    cml_pfn_last_patch_status fn;
    if (module == NULL) {
        return CML_STATUS_AGENT_NOT_FOUND;
    }
    fn = (cml_pfn_last_patch_status)cml_resolve_export(module, CML_EXPORT_LAST_PATCH_STATUS);
    return fn == NULL ? CML_STATUS_AGENT_NOT_FOUND : (cml_status)fn();
}

/* Nonzero while the agent has a live, handshaken connection to the broker. For diagnostics. */
CML_INLINE int cml_is_connected(cml_agent agent) {
    return agent != NULL && agent->is_connected(agent->impl) != 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Hook macros. Use at file scope, with a semicolon:  CML_POSTPATCH_HOOK(OnAfterPatch);       */
/* The function must be defined (or declared) before the macro. The record is NOT const on   */
/* purpose: cl drops an unreferenced internal const, a plain static survives. Sections:       */
/* ".cmlhk$a" (pre-patch), ".cmlhk$b" (post-patch), ".cmlhk$c" (the others).                  */
/* ------------------------------------------------------------------------------------------ */

#define CML_HOOK_CONCAT2_(a, b) a##b
#define CML_HOOK_CONCAT_(a, b) CML_HOOK_CONCAT2_(a, b)
#if defined(__clang__)
#define CML_HOOK_USED_ __attribute__((used))
#else
#define CML_HOOK_USED_
#endif
#define CML_HOOK_REGISTER_(sec, kind_value, fn)                                                    \
    __pragma(section(sec, read))                                                                   \
    __declspec(allocate(sec)) CML_HOOK_USED_ static cml_hook_entry                                 \
        CML_HOOK_CONCAT_(cml_hook_entry_, __COUNTER__) = {CML_HOOK_MAGIC, (kind_value),            \
                                                          (cml_generic_fn)(fn), __FILE__ "|" #fn}

#define CML_PREPATCH_HOOK(fn) CML_HOOK_REGISTER_(".cmlhk$a", CML_HOOK_PREPATCH, fn)
#define CML_POSTPATCH_HOOK(fn) CML_HOOK_REGISTER_(".cmlhk$b", CML_HOOK_POSTPATCH, fn)
#define CML_COMPILE_START_HOOK(fn) CML_HOOK_REGISTER_(".cmlhk$c", CML_HOOK_COMPILE_START, fn)
#define CML_COMPILE_SUCCESS_HOOK(fn) CML_HOOK_REGISTER_(".cmlhk$c", CML_HOOK_COMPILE_SUCCESS, fn)
#define CML_COMPILE_ERROR_HOOK(fn) CML_HOOK_REGISTER_(".cmlhk$c", CML_HOOK_COMPILE_ERROR, fn)
#define CML_PRERESTART_HOOK(fn) CML_HOOK_REGISTER_(".cmlhk$c", CML_HOOK_PRERESTART, fn)

#endif /* !CML_AGENT_BUILD */

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ------------------------------------------------------------------------------------------ */
/* C++ wrapper                                                                                */
/* ------------------------------------------------------------------------------------------ */

#if defined(__cplusplus) && !defined(CML_AGENT_BUILD)

#include <string>

namespace cml {

enum class ModuleOptions : uint32_t {
    None = CML_MODULE_NONE,
    AllImports = CML_MODULE_ALL_IMPORTS,
};

inline ModuleOptions operator|(ModuleOptions a, ModuleOptions b) {
    return static_cast<ModuleOptions>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

/* Result of queueing a request. `status` is CML_STATUS_OK when it was queued. */
struct Request {
    cml_status status;
    uint64_t id;
};

/* The broker's answer to a request. */
struct Reply {
    cml_status status; /* 0 = accepted, else a CMLnnnn number */
    std::string message;
};

/* A handle to a running agent. Copying it copies the handle, not the agent: destroy it exactly
 * once with cml::DestroyAgent(). A default-constructed or failed Agent is invalid and every call
 * on it returns CML_STATUS_INVALID_ARGUMENT (SyncPoint is a no-op). */
class Agent {
public:
    Agent() : handle_(NULL) {}
    explicit Agent(cml_agent handle) : handle_(handle) {}

    bool IsValid() const { return handle_ != NULL; }
    explicit operator bool() const { return handle_ != NULL; }
    cml_agent NativeHandle() const { return handle_; }

    Request EnableModule(const wchar_t* path, ModuleOptions options = ModuleOptions::AllImports) const {
        Request request;
        request.id = 0;
        request.status = cml_enable_module(handle_, path, static_cast<uint32_t>(options), &request.id);
        return request;
    }
    Request EnableModule(const std::wstring& path,
                         ModuleOptions options = ModuleOptions::AllImports) const {
        return EnableModule(path.c_str(), options);
    }

    Request DisableModule(const wchar_t* path) const {
        Request request;
        request.id = 0;
        request.status = cml_disable_module(handle_, path, &request.id);
        return request;
    }
    Request DisableModule(const std::wstring& path) const { return DisableModule(path.c_str()); }

    /* Waits up to timeout_ms for the answer; see cml_wait_request(). On CML_STATUS_OK `reply` is
     * filled (it may be NULL if you only care that an answer arrived). */
    cml_status WaitForRequest(const Request& request, uint32_t timeout_ms, Reply* reply = NULL) const {
        cml_reply raw;
        cml_status status;
        raw.status = 0;
        raw.message_length = 0;
        raw.message[0] = '\0';
        status = cml_wait_request(handle_, request.id, timeout_ms, &raw);
        if (status == CML_STATUS_OK && reply != NULL) {
            reply->status = raw.status;
            reply->message.assign(raw.message, raw.message_length);
        }
        return status;
    }

    void SyncPoint() const { cml_sync_point(handle_); }

    cml_status ScheduleReload() const { return cml_schedule_reload(handle_); }
    cml_status WaitForReload(uint32_t timeout_ms) const { return cml_wait_for_reload(handle_, timeout_ms); }
    cml_status ScheduleRestart() const { return cml_schedule_restart(handle_); }
    cml_status SetGlobalHotkeys(const cml_hotkeys& hotkeys) const {
        return cml_set_global_hotkeys(handle_, &hotkeys);
    }

    bool IsConnected() const { return cml_is_connected(handle_) != 0; }

private:
    cml_agent handle_;
    friend cml_status DestroyAgent(Agent& agent);
};

/* Immediate mode: patches are applied as soon as they are ready, on the agent's thread.
 * `status` (optional) receives CML_STATUS_OK or the reason the agent could not be created. */
inline Agent CreateDefaultAgent(const wchar_t* agent_dir, cml_status* status = NULL) {
    return Agent(cml_create_agent(agent_dir, CML_MODE_IMMEDIATE, status));
}

/* Synchronized mode: patches are applied inside Agent::SyncPoint(); call it regularly. */
inline Agent CreateSynchronizedAgent(const wchar_t* agent_dir, cml_status* status = NULL) {
    return Agent(cml_create_agent(agent_dir, CML_MODE_SYNCHRONIZED, status));
}

/* Stops the agent and invalidates `agent` (see cml_destroy_agent() for the threading contract).
 * Returns CML_STATUS_INVALID_ARGUMENT, leaving `agent` valid, if called from inside a call into
 * the same agent. */
inline cml_status DestroyAgent(Agent& agent) {
    const cml_status status = cml_destroy_agent(agent.handle_);
    if (status == CML_STATUS_OK) {
        agent.handle_ = NULL;
    }
    return status;
}

/* Full path of the module (exe or dll) that contains the code calling this function. */
inline std::wstring GetCurrentModulePath() {
    uint32_t capacity = 512;
    for (;;) {
        std::wstring buffer(capacity, L'\0');
        uint32_t length = 0;
        const cml_status status = cml_get_current_module_path(&buffer[0], capacity, &length);
        if (status == CML_STATUS_OK) {
            buffer.resize(length);
            return buffer;
        }
        if (status != CML_STATUS_INVALID_ARGUMENT || capacity >= 32768) {
            return std::wstring();
        }
        capacity *= 4;
    }
}

} /* namespace cml */

#endif /* __cplusplus && !CML_AGENT_BUILD */

#endif /* CHAMELEON_API_H */
