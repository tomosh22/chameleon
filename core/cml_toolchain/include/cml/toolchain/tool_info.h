#pragma once

// Toolchain detection: family and version of cl, clang-cl, link and lld-link from the binary itself
// (plan section 4.4, "fall back to the toolchain detected from the running broker's environment").
//
// Detection runs the tool with a harmless argument and parses what it prints (cl: no arguments,
// prints its banner; link: /? ; clang-cl and lld-link: --version). When that fails the Windows
// version resource of the file is used. parse_tool_banner() is pure and tested on every host.

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "cml/util/result.h"

namespace cml::toolchain {

enum class ToolFamily { Unknown, Cl, ClangCl, Clang, Link, LldLink };

const char* to_string(ToolFamily family) noexcept;
// "cl.exe", "clang-cl.exe", "link.exe", "lld-link.exe", "clang.exe"; "" for Unknown.
const char* default_file_name(ToolFamily family) noexcept;
// From the file name only ("C:\x\Clang-CL.EXE" -> ClangCl).
ToolFamily family_from_path(std::string_view path) noexcept;

struct ToolInfo {
    ToolFamily family = ToolFamily::Unknown;
    std::string path;
    std::array<unsigned, 4> version{};  // 19.44.35207.1 / 18.1.8.0
    std::string version_text;           // "19.44.35207.1" / "18.1.8"
    std::string target;                 // "x64", "x86", "x86_64-pc-windows-msvc" (what the banner says)
    std::string banner;                 // the text the version was parsed from
    bool from_version_resource = false;

    unsigned major() const noexcept { return version[0]; }
};

// Parses a banner printed by one of the tools. `hint` (usually family_from_path) decides between
// ClangCl and Clang, since both print "clang version N". Family is Unknown when nothing matches.
ToolInfo parse_tool_banner(std::string_view banner, ToolFamily hint = ToolFamily::Unknown);

// Parses "19.44.35207.1" / "18.1.8" into four numbers; false when there is no leading number.
bool parse_version_numbers(std::string_view text, std::array<unsigned, 4>& out) noexcept;

// Searches `extra_dirs` and then PATH for `file_name` ("cl.exe", or "cl": ".exe" is appended on
// Windows when there is no extension). CML1510 when not found.
Result<std::string> locate_executable(std::string_view file_name, const std::vector<std::string>& extra_dirs = {});
// locate_executable(default_file_name(family)).
Result<std::string> locate_tool(ToolFamily family, const std::vector<std::string>& extra_dirs = {});

// Runs the tool (Windows only; CML1511 elsewhere) and falls back to its version resource.
Result<ToolInfo> detect_tool(const std::string& path);
// locate_tool + detect_tool.
Result<ToolInfo> detect_tool_in_environment(ToolFamily family, const std::vector<std::string>& extra_dirs = {});

// The Windows file version resource ("14.44.35207.0"); nullopt-like empty array result on failure.
Result<std::array<unsigned, 4>> read_file_version(const std::string& path);

}  // namespace cml::toolchain
