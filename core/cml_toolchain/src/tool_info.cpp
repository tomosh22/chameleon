#include "cml/toolchain/tool_info.h"

#include <cstdlib>

#include "cml/toolchain/diag_ids.h"
#include "cml/toolchain/invoke.h"
#include "cml/util/log.h"
#include "cml/util/path.h"
#include "fs_util.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace cml::toolchain {

const char* to_string(ToolFamily family) noexcept {
    switch (family) {
        case ToolFamily::Unknown: return "unknown";
        case ToolFamily::Cl: return "cl";
        case ToolFamily::ClangCl: return "clang-cl";
        case ToolFamily::Clang: return "clang";
        case ToolFamily::Link: return "link";
        case ToolFamily::LldLink: return "lld-link";
    }
    return "unknown";
}

const char* default_file_name(ToolFamily family) noexcept {
    switch (family) {
        case ToolFamily::Cl: return "cl.exe";
        case ToolFamily::ClangCl: return "clang-cl.exe";
        case ToolFamily::Clang: return "clang.exe";
        case ToolFamily::Link: return "link.exe";
        case ToolFamily::LldLink: return "lld-link.exe";
        case ToolFamily::Unknown: break;
    }
    return "";
}

ToolFamily family_from_path(std::string_view p) noexcept {
    std::string name = detail::lower(path::base_name(p));
    if (detail::ends_with(name, ".exe")) {
        name.resize(name.size() - 4);
    }
    if (name == "cl") return ToolFamily::Cl;
    if (name == "clang-cl") return ToolFamily::ClangCl;
    if (name == "clang" || name == "clang++") return ToolFamily::Clang;
    if (name == "link") return ToolFamily::Link;
    if (name == "lld-link") return ToolFamily::LldLink;
    return ToolFamily::Unknown;
}

bool parse_version_numbers(std::string_view text, std::array<unsigned, 4>& out) noexcept {
    out = {};
    std::size_t i = 0;
    std::size_t part = 0;
    bool any = false;
    while (i < text.size() && part < 4) {
        if (text[i] < '0' || text[i] > '9') {
            break;
        }
        unsigned v = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            if (v < 100000000u) {
                v = v * 10 + static_cast<unsigned>(text[i] - '0');
            }
            ++i;
        }
        out[part++] = v;
        any = true;
        if (i < text.size() && text[i] == '.') {
            ++i;
        } else {
            break;
        }
    }
    return any;
}

namespace {

// The run of [0-9.] characters after `marker`, or "".
std::string version_after(std::string_view text, std::string_view marker) {
    const std::size_t pos = text.find(marker);
    if (pos == std::string_view::npos) {
        return {};
    }
    std::size_t i = pos + marker.size();
    const std::size_t start = i;
    while (i < text.size() && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.')) {
        ++i;
    }
    std::string v(text.substr(start, i - start));
    while (!v.empty() && v.back() == '.') {
        v.pop_back();
    }
    return v;
}

std::string word_after(std::string_view text, std::string_view marker) {
    const std::size_t pos = text.find(marker);
    if (pos == std::string_view::npos) {
        return {};
    }
    std::size_t i = pos + marker.size();
    const std::size_t start = i;
    while (i < text.size() && text[i] != ' ' && text[i] != '\r' && text[i] != '\n' && text[i] != '\t') {
        ++i;
    }
    return std::string(text.substr(start, i - start));
}

}  // namespace

ToolInfo parse_tool_banner(std::string_view banner, ToolFamily hint) {
    ToolInfo info;
    info.banner = std::string(banner);
    std::string v;
    if (!(v = version_after(banner, "C/C++ Optimizing Compiler Version ")).empty()) {
        info.family = ToolFamily::Cl;
        info.target = word_after(banner, " for ");
    } else if (!(v = version_after(banner, "Incremental Linker Version ")).empty()) {
        info.family = ToolFamily::Link;
    } else if (!(v = version_after(banner, "clang version ")).empty()) {
        info.family = hint == ToolFamily::ClangCl ? ToolFamily::ClangCl : ToolFamily::Clang;
        info.target = word_after(banner, "Target: ");
    } else if (banner.substr(0, 4) == "LLD " || banner.find("\nLLD ") != std::string_view::npos ||
               banner.find("LLD ") != std::string_view::npos) {
        v = version_after(banner, "LLD ");
        if (!v.empty()) {
            info.family = ToolFamily::LldLink;
        }
    }
    if (!v.empty()) {
        info.version_text = v;
        parse_version_numbers(v, info.version);
    }
    return info;
}

Result<std::string> locate_executable(std::string_view file_name, const std::vector<std::string>& extra_dirs) {
    std::string name(file_name);
#if defined(_WIN32)
    if (name.find('.') == std::string::npos) {
        name += ".exe";
    }
#endif
    std::vector<std::string> dirs = extra_dirs;
#if defined(_WIN32)
    std::wstring wide(32768, L'\0');
    const DWORD n = GetEnvironmentVariableW(L"PATH", wide.data(), static_cast<DWORD>(wide.size()));
    std::string path_env;
    if (n > 0 && n < wide.size()) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(n), nullptr, 0, nullptr, nullptr);
        path_env.resize(static_cast<std::size_t>(len > 0 ? len : 0));
        if (len > 0) {
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(n), path_env.data(), len, nullptr, nullptr);
        }
    }
    constexpr char kSep = ';';
#else
    const char* env = std::getenv("PATH");
    const std::string path_env = env ? env : "";
    constexpr char kSep = ':';
#endif
    std::size_t start = 0;
    while (start <= path_env.size()) {
        std::size_t end = path_env.find(kSep, start);
        if (end == std::string::npos) {
            end = path_env.size();
        }
        std::string dir = path_env.substr(start, end - start);
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') {
            dir = dir.substr(1, dir.size() - 2);
        }
        if (!dir.empty()) {
            dirs.push_back(dir);
        }
        start = end + 1;
    }
    for (const std::string& dir : dirs) {
        std::string candidate = dir;
        if (!candidate.empty() && !path::is_separator(candidate.back())) {
#if defined(_WIN32)
            candidate.push_back('\\');
#else
            candidate.push_back('/');
#endif
        }
        candidate += name;
        if (detail::file_exists(candidate)) {
            return candidate;
        }
    }
    return Diag::make(diag::kToolNotFound, "'" + name + "' was not found in the given directories or on PATH",
                      "Run from a developer command prompt (vcvars64.bat) or add the toolchain's bin directory to PATH.");
}

Result<std::string> locate_tool(ToolFamily family, const std::vector<std::string>& extra_dirs) {
    const char* name = default_file_name(family);
    if (name[0] == '\0') {
        return Diag::make(diag::kToolNotFound, "no file name is known for an unknown toolchain family",
                          "Pass the full path of the tool.");
    }
    return locate_executable(name, extra_dirs);
}

#if defined(_WIN32)

Result<std::array<unsigned, 4>> read_file_version(const std::string& path) {
    const std::wstring wide = detail::to_path(path).wstring();
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(wide.c_str(), &handle);
    if (size == 0) {
        return Diag::make(diag::kToolDetectFailed, "'" + path + "' has no version resource",
                          "Check that the path names the compiler or linker binary.");
    }
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(wide.c_str(), 0, size, data.data())) {
        return Diag::make(diag::kToolDetectFailed, "cannot read the version resource of '" + path + "'",
                          "Check that the path names the compiler or linker binary.");
    }
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &len) || fixed == nullptr ||
        len < sizeof(VS_FIXEDFILEINFO)) {
        return Diag::make(diag::kToolDetectFailed, "the version resource of '" + path + "' is malformed",
                          "Check that the path names the compiler or linker binary.");
    }
    return std::array<unsigned, 4>{HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
                                   HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS)};
}

Result<ToolInfo> detect_tool(const std::string& path) {
    if (!detail::file_exists(path)) {
        return Diag::make(diag::kToolNotFound, "'" + path + "' does not exist", "Check the path of the tool.");
    }
    const ToolFamily hint = family_from_path(path);
    ProcessSpec spec;
    spec.exe = path;
    if (hint == ToolFamily::Link) {
        spec.args = {"/?"};
    } else if (hint != ToolFamily::Cl) {
        spec.args = {"--version"};
    }
    spec.timeout_ms = 30000;
    ToolInfo info;
    Result<ProcessResult> run = run_process(spec);
    if (run) {
        info = parse_tool_banner(run->output, hint);
    } else {
        CML_LOG(Debug, "toolchain: running '%s' failed: %s", path.c_str(), run.error().format().c_str());
    }
    info.path = path;
    if (info.family == ToolFamily::Unknown) {
        info.family = hint;
    }
    if (info.version_text.empty()) {
        Result<std::array<unsigned, 4>> fv = read_file_version(path);
        if (fv) {
            info.version = *fv;
            info.version_text = std::to_string(info.version[0]) + "." + std::to_string(info.version[1]) + "." +
                                std::to_string(info.version[2]) + "." + std::to_string(info.version[3]);
            info.from_version_resource = true;
        }
    }
    if (info.family == ToolFamily::Unknown || info.version_text.empty()) {
        return Diag::make(diag::kToolDetectFailed, "cannot determine family and version of '" + path + "'",
                          "Check that the path names cl, clang-cl, link or lld-link.");
    }
    CML_LOG(Debug, "toolchain: %s is %s %s (%s)", path.c_str(), to_string(info.family), info.version_text.c_str(),
            info.from_version_resource ? "version resource" : "banner");
    return info;
}

#else

Result<std::array<unsigned, 4>> read_file_version(const std::string& path) {
    return Diag::make(diag::kToolDetectFailed, "version resources are only available on Windows ('" + path + "')",
                      "Run on Windows.");
}

Result<ToolInfo> detect_tool(const std::string& path) {
    return Diag::make(diag::kToolDetectFailed, "tool detection needs Windows ('" + path + "')", "Run on Windows.");
}

#endif

Result<ToolInfo> detect_tool_in_environment(ToolFamily family, const std::vector<std::string>& extra_dirs) {
    Result<std::string> path = locate_tool(family, extra_dirs);
    if (!path) {
        return path.error();
    }
    return detect_tool(*path);
}

}  // namespace cml::toolchain
