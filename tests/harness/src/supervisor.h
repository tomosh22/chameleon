#pragma once

// Process supervision for cmltest (Windows): every child runs in a kill-on-close job object, so
// nothing the harness starts can outlive it, whatever happens. Waits are bounded; before a child
// is killed because of a failure or timeout a minidump is written for every live child.

#if defined(_WIN32)

#include <windows.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cml/util/result.h"

namespace cmltest {

struct CaseInsensitiveLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const;
};

struct ChildSpec {
    std::string label;                      // "broker", "fixture", "cmake-build", ...
    std::filesystem::path exe;              // full path of the executable
    std::wstring arguments;                 // the command line after the (quoted) executable
    std::filesystem::path working_directory;  // empty = inherit
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> environment;  // added to / overriding the inherited one
    std::filesystem::path output_file;      // stdout and stderr go here (created); empty = NUL
};

class Supervisor {
public:
    Supervisor();
    ~Supervisor();  // kills every child still running
    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    // Starts a child inside the job. CML5415 when it cannot be started. Returns its id.
    cml::Result<std::size_t> start(const ChildSpec& spec);

    std::uint32_t pid(std::size_t id) const;
    const std::string& label(std::size_t id) const;
    // Exit code once the child has exited, nullopt while it runs.
    std::optional<std::uint32_t> exit_code(std::size_t id) const;
    // True if the child exited within timeout_ms.
    bool wait_exit(std::size_t id, std::uint32_t timeout_ms);

    // Writes <label>-<pid>.dmp for every child still running into `directory` (created if needed).
    // Failures are returned as text (CML5423) and do not stop the other dumps.
    struct DumpResult {
        std::vector<std::filesystem::path> files;
        std::vector<std::string> errors;
    };
    DumpResult dump_running(const std::filesystem::path& directory);

    void kill(std::size_t id);
    void kill_all();

private:
    struct Child {
        std::string label;
        HANDLE process = nullptr;
        std::uint32_t pid = 0;
    };
    HANDLE job_ = nullptr;
    std::vector<Child> children_;
};

// "C:\a b\c.exe" -> "\"C:\\a b\\c.exe\"" (just wraps in quotes; paths cannot contain quotes).
std::wstring quote_argument(const std::wstring& text);

}  // namespace cmltest

#endif  // _WIN32
