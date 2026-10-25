#include "run_dirs.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <system_error>

#include "cml/ipc/control_ids.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cmltest {

namespace fs = std::filesystem;

namespace {

cml::Diag usage(const std::string& what) {
    return cml::Diag::make(cml::ids::kHarnessUsage, what,
                           "cmltest only manages directories it created itself (they contain " + std::string(kRunMarkerFile) + ").");
}

std::string utc_timestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm parts = {};
#if defined(_WIN32)
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &parts);
    return buffer;
}

unsigned long process_id() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

}  // namespace

cml::Result<fs::path> create_run_directory(const fs::path& base, const std::string& scenario) {
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) {
        return usage("cannot create the directory '" + base.string() + "': " + ec.message());
    }
    const std::string stem = "cmltest-" + scenario + "-" + utc_timestamp() + "-p" + std::to_string(process_id());
    for (int attempt = 0; attempt < 100; ++attempt) {
        const fs::path dir = base / (attempt == 0 ? stem : stem + "-" + std::to_string(attempt));
        // create_directory reports false (no error) when it already exists: never reuse one.
        if (!fs::create_directory(dir, ec)) {
            if (ec) {
                return usage("cannot create '" + dir.string() + "': " + ec.message());
            }
            continue;
        }
        std::ofstream marker(dir / kRunMarkerFile, std::ios::binary);
        marker << "created by cmltest; only directories with this file are ever deleted by it\n";
        marker.close();
        if (!marker) {
            return usage("cannot write the marker file in '" + dir.string() + "'");
        }
        return dir;
    }
    return usage("cannot find an unused run directory name below '" + base.string() + "'");
}

cml::Result<void> remove_run_directory(const fs::path& dir) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) {
        return usage("refusing to delete '" + dir.string() + "': not a directory");
    }
    if (!fs::is_regular_file(dir / kRunMarkerFile, ec)) {
        return usage("refusing to delete '" + dir.string() + "': it has no " + kRunMarkerFile + " marker, cmltest did not create it");
    }
    const fs::path canonical = fs::weakly_canonical(dir, ec);
    if (!ec && canonical == canonical.root_path()) {
        return usage("refusing to delete the filesystem root");
    }
    ec.clear();
    fs::remove_all(dir, ec);
    if (ec) {
        return usage("cannot delete '" + dir.string() + "': " + ec.message());
    }
    return cml::Result<void>();
}

}  // namespace cmltest
