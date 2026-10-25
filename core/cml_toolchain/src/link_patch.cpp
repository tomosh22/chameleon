// The linker driver, portable part (WP-09): family detection from the module, the link command line and its checks,
// response files, linker output summary, and reading the linked patch (PDB publics and PE header).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "cml/pe/pe.h"
#include "cml/toolchain/command_line.h"
#include "cml/toolchain/link_patch.h"
#include "cml/util/log.h"
#include "cml/util/path.h"
#include "fs_util.h"

namespace cml::toolchain {

namespace {

using detail::ascii_lower;

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

bool contains_ci(std::string_view haystack, std::string_view needle) {
    return detail::lower(haystack).find(detail::lower(needle)) != std::string::npos;
}

std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

std::uint32_t read_u32(std::span<const std::byte> b, std::size_t off) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(b[off])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[off + 1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[off + 2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[off + 3])) << 24);
}

bool is_linker(ToolFamily f) noexcept {
    return f == ToolFamily::Link || f == ToolFamily::LldLink;
}

}  // namespace

// ---- 1. family detection ----------------------------------------------------------------------------------

PeLinkerFacts read_pe_linker_facts(std::span<const std::byte> bytes) noexcept {
    PeLinkerFacts facts;
    if (bytes.size() < 0x40 || bytes[0] != std::byte{'M'} || bytes[1] != std::byte{'Z'}) {
        return facts;
    }
    const std::uint32_t lfanew = read_u32(bytes, 0x3C);
    if (lfanew < 0x40 || static_cast<std::uint64_t>(lfanew) + 24 + 4 > bytes.size()) {
        return facts;
    }
    if (read_u32(bytes, lfanew) != 0x00004550u) {  // "PE\0\0"
        return facts;
    }
    const std::size_t opt = static_cast<std::size_t>(lfanew) + 24;
    facts.major = std::to_integer<unsigned>(bytes[opt + 2]);
    facts.minor = std::to_integer<unsigned>(bytes[opt + 3]);
    facts.valid = true;
    // The Rich header sits between the DOS stub and the PE signature; "Rich" is its end marker.
    for (std::size_t i = 0x40; i + 4 <= lfanew; ++i) {
        if (bytes[i] == std::byte{'R'} && bytes[i + 1] == std::byte{'i'} && bytes[i + 2] == std::byte{'c'} &&
            bytes[i + 3] == std::byte{'h'}) {
            facts.rich_header = true;
            break;
        }
    }
    return facts;
}

PdbLinkerFacts read_pdb_linker_facts(const pdb::PdbIndex& index) {
    PdbLinkerFacts facts;
    for (std::uint32_t ci = 0; ci < index.compiland_count(); ++ci) {
        const pdb::Compiland& c = index.compiland(ci);
        if (trim(c.object_name) != "* Linker *") {
            continue;
        }
        facts.linker_module_found = true;
        facts.has_compile3 = c.has_compile3;
        if (c.has_compile3) {
            facts.compile_version = std::string(c.compile.version);
            for (std::size_t k = 0; k < 4; ++k) {
                facts.backend_version[k] = c.compile.backend[k];
            }
        }
        if (c.env_source == pdb::EnvSource::EnvBlock) {
            facts.has_env_block = true;
            for (const pdb::EnvPair& kv : c.env) {
                if (kv.first == "exe") {
                    facts.exe_path = std::string(kv.second);
                } else if (kv.first == "cmd") {
                    facts.command = std::string(kv.second);
                }
            }
        } else {
            // S_BUILDINFO form: the build tool is the linker.
            Result<pdb::BuildEnv> env = index.build_env(ci);
            if (env && env->source != pdb::EnvSource::None) {
                if (const auto tool = env->compiler()) {
                    facts.exe_path = std::string(*tool);
                }
                if (const auto cmd = env->command()) {
                    facts.command = std::string(*cmd);
                }
            }
        }
        break;
    }
    return facts;
}

Result<LinkerDetection> decide_linker_family(const PeLinkerFacts& pe, const PdbLinkerFacts* pdb,
                                             const DetectLinkerOptions& options) {
    LinkerDetection d;
    char buf[256];

    // ---- the PE ------------------------------------------------------------------------------------------
    if (pe.valid) {
        std::snprintf(buf, sizeof(buf), "PE: linker version %u.%u, Rich header %s", pe.major, pe.minor,
                      pe.rich_header ? "present" : "absent");
        d.evidence.push_back(buf);
        if (pe.rich_header) {
            d.from_pe = ToolFamily::Link;
            d.evidence.push_back("PE: Rich header present: written by link.exe, never by lld-link -> link");
        } else {
            d.from_pe = ToolFamily::LldLink;
            d.evidence.push_back("PE: no Rich header: not link.exe -> lld-link");
        }
        d.version = {pe.major, pe.minor, 0, 0};
    } else {
        d.evidence.push_back("PE: no usable PE32/PE32+ header");
    }

    // ---- the PDB -----------------------------------------------------------------------------------------
    if (pdb == nullptr) {
        d.evidence.push_back("PDB: not available");
    } else if (!pdb->linker_module_found) {
        d.evidence.push_back("PDB: no `* Linker *` compiland");
    } else {
        std::snprintf(buf, sizeof(buf), "PDB: `* Linker *` compiland: compile3=%s version=\"%s\" env_block=%s exe=\"%s\"",
                      pdb->has_compile3 ? "yes" : "no", pdb->compile_version.c_str(), pdb->has_env_block ? "yes" : "no",
                      pdb->exe_path.c_str());
        d.evidence.push_back(buf);
        const ToolFamily by_exe = family_from_path(pdb->exe_path);
        if (is_linker(by_exe)) {
            d.from_pdb = by_exe;
            d.evidence.push_back(std::string("PDB: linker executable name -> ") + to_string(by_exe));
        } else if (contains_ci(pdb->compile_version, "llvm") || contains_ci(pdb->compile_version, "lld")) {
            d.from_pdb = ToolFamily::LldLink;
            d.evidence.push_back("PDB: S_COMPILE3 version string names LLVM/LLD -> lld-link");
        } else if (contains_ci(pdb->compile_version, "microsoft") || contains_ci(pdb->compile_version, "link")) {
            d.from_pdb = ToolFamily::Link;
            d.evidence.push_back("PDB: S_COMPILE3 version string names Microsoft/LINK -> link");
        } else if (pdb->has_env_block) {
            d.from_pdb = ToolFamily::LldLink;
            d.evidence.push_back("PDB: S_ENVBLOCK in the linker module (only lld-link writes one, WP-04 evidence) -> lld-link");
        } else {
            d.evidence.push_back("PDB: the linker module does not identify the linker");
        }
        if (!d.version[0] && pdb->has_compile3) {
            d.version = pdb->backend_version;
        }
    }

    // ---- decision ----------------------------------------------------------------------------------------
    if (options.override_family != ToolFamily::Unknown) {
        if (!is_linker(options.override_family)) {
            return Diag::make(diag::kLinkerFamilyUnknown,
                              std::string("the linker override '") + to_string(options.override_family) + "' is not a linker",
                              "Use 'link' or 'lld-link'.");
        }
        d.family = options.override_family;
        d.overridden = true;
        d.evidence.push_back(std::string("override: the user chose ") + to_string(d.family));
        return d;
    }
    if (d.from_pdb != ToolFamily::Unknown && d.from_pe != ToolFamily::Unknown && d.from_pdb != d.from_pe) {
        d.warnings.push_back(Diag::make(
            diag::kLinkerEvidenceConflict,
            std::string("the PDB says the module was linked by ") + to_string(d.from_pdb) + " but the PE says " +
                to_string(d.from_pe) + "; using the PDB",
            "If the PDB belongs to another build, rebuild the module; or choose the linker explicitly."));
    }
    d.family = d.from_pdb != ToolFamily::Unknown ? d.from_pdb : d.from_pe;
    if (d.family == ToolFamily::Unknown) {
        return Diag::make(diag::kLinkerFamilyUnknown, "neither the PE nor the PDB of the module identifies its linker",
                          "Choose the linker explicitly (link or lld-link).");
    }
    return d;
}

Result<LinkerDetection> detect_module_linker(const std::string& pe_path, const pdb::OpenOptions& pdb_options,
                                             const DetectLinkerOptions& options) {
    Result<std::vector<std::byte>> bytes = pe::read_file(pe_path);
    if (!bytes) {
        return Diag::make(diag::kCannotReadModule, "cannot read the module '" + pe_path + "': " + bytes.error().message,
                          "Check that the module file exists and is readable.");
    }
    const PeLinkerFacts pe_facts = read_pe_linker_facts(*bytes);
    bytes = std::vector<std::byte>();

    // The index maps the PDB read-only for as long as it lives (ADR-0017): it is dropped before this function returns,
    // so nothing here can make a later relink of the module fail with ERROR_USER_MAPPED_FILE.
    PdbLinkerFacts pdb_facts;
    bool have_pdb = false;
    std::vector<Diag> notes;
    {
        pdb::OpenOptions opts = pdb_options;
        opts.use_cache = false;
        auto index = pdb::PdbIndex::open_for_pe(pe_path, opts);
        if (index) {
            pdb_facts = read_pdb_linker_facts(**index);
            have_pdb = true;
        } else {
            notes.push_back(Diag::make(diag::kCannotReadModule,
                                       "the PDB of '" + pe_path + "' could not be opened: " + index.error().message,
                                       "The linker is detected from the PE alone; check that the PDB is next to the module."));
        }
    }
    Result<LinkerDetection> decided = decide_linker_family(pe_facts, have_pdb ? &pdb_facts : nullptr, options);
    if (!decided) {
        return decided;
    }
    for (Diag& n : notes) {
        decided->warnings.push_back(std::move(n));
    }
    for (const std::string& line : decided->evidence) {
        CML_LOG(Debug, "toolchain: linker detection for %s: %s", pe_path.c_str(), line.c_str());
    }
    return decided;
}

// ---- 2. the command line ----------------------------------------------------------------------------------

std::string format_base_argument(std::uint64_t base) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(base));
    return buf;
}

namespace {

struct OptionParts {
    bool is_option = false;
    std::string name;   // upper-cased, up to the first ':'
    std::string value;  // after the first ':' (original case), "" when absent
    bool has_value = false;
};

OptionParts split_option(const std::string& arg) {
    OptionParts p;
    if (arg.size() < 2 || (arg[0] != '/' && arg[0] != '-')) {
        return p;
    }
    p.is_option = true;
    const std::size_t colon = arg.find(':', 1);
    if (colon == std::string::npos) {
        p.name = upper(std::string_view(arg).substr(1));
    } else {
        p.name = upper(std::string_view(arg).substr(1, colon - 1));
        p.value = arg.substr(colon + 1);
        p.has_value = true;
    }
    return p;
}

std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find(',', start);
        if (end == std::string::npos) {
            end = s.size();
        }
        out.push_back(upper(std::string_view(s).substr(start, end - start)));
        start = end + 1;
    }
    return out;
}

Diag forbidden(const std::string& arg, const std::string& why) {
    return Diag::make(diag::kForbiddenLinkerOption, "the linker option '" + arg + "' is not allowed for a patch: " + why,
                      "Remove it; section locators need /OPT:NOREF /OPT:NOICF and a complete, fixed-address link (ADR-0024).");
}

}  // namespace

Result<void> check_extra_link_args(const std::vector<std::string>& extra_args) {
    // Response files are expanded first: an @file must not smuggle in /OPT:REF.
    std::vector<std::string> args = extra_args;
    bool any_response = false;
    for (const std::string& a : args) {
        any_response = any_response || (a.size() > 1 && a[0] == '@');
    }
    if (any_response) {
        Result<std::vector<std::string>> expanded = expand_response_files(args, "", default_file_reader());
        if (!expanded) {
            return Diag::make(diag::kBadLinkRequest, "cannot expand a response file in the extra linker arguments: " + expanded.error().message,
                              "Check that the response file exists and is readable.");
        }
        args = std::move(*expanded);
    }
    for (const std::string& arg : args) {
        const OptionParts p = split_option(arg);
        if (!p.is_option) {
            continue;
        }
        if (p.name == "OPT") {
            for (const std::string& tok : split_commas(p.value)) {
                if (tok == "REF") {
                    return forbidden(arg, "/OPT:REF removes unreferenced COMDATs and the publics that locate them");
                }
                if (tok == "ICF" || tok.rfind("ICF=", 0) == 0) {
                    return forbidden(arg, "/OPT:ICF folds identical sections, so two sections can share one address");
                }
            }
            continue;
        }
        if (p.name.rfind("FORCE", 0) == 0) {
            return forbidden(arg, "/FORCE can leave symbols unresolved or duplicated and produces jumps into nowhere");
        }
        if (p.name == "INCREMENTAL" && upper(p.value) != "NO") {
            return forbidden(arg, "incremental linking pads and thunks functions");
        }
        if (p.name == "DYNAMICBASE" && upper(p.value) != "NO") {
            return forbidden(arg, "the patch is loaded at its reserved address and must not be relocated");
        }
        if (p.name == "FIXED" && p.has_value && upper(p.value) == "NO") {
            return forbidden(arg, "the patch must be fixed at its reserved address");
        }
        static const char* const kReserved[] = {"OUT",   "PDB",   "BASE",           "DLL",   "ENTRY",
                                                "NOENTRY", "MACHINE", "DEBUG",      "FUNCTIONPADMIN", "FIXED",
                                                "NOLOGO", "DRIVER"};
        for (const char* r : kReserved) {
            if (p.name == r) {
                return Diag::make(diag::kReservedLinkerOption, "the linker option '" + arg + "' is set by the patch linker driver",
                                  "Remove it; use the patch request's fields (output name, base) instead.");
            }
        }
        if (p.name == "NODEFAULTLIB" && !p.has_value) {
            return Diag::make(diag::kReservedLinkerOption, "the linker option '" + arg + "' is set by the patch linker driver",
                              "Remove it; the driver always passes /NODEFAULTLIB.");
        }
    }
    return {};
}

Result<std::vector<std::string>> build_link_command(const LinkCommandSpec& spec) {
    if (!is_linker(spec.family)) {
        return Diag::make(diag::kBadLinkRequest, "the link request names no linker family", "Use link or lld-link.");
    }
    if (spec.objects.empty()) {
        return Diag::make(diag::kBadLinkRequest, "the link request has no object files", "Pass the patch objects.");
    }
    if (spec.out_dll.empty() || spec.out_pdb.empty()) {
        return Diag::make(diag::kBadLinkRequest, "the link request has no output DLL or PDB name", "Name both outputs.");
    }
    if (spec.base == 0 || (spec.base & 0xFFFFull) != 0) {
        return Diag::make(diag::kBadLinkRequest,
                          "the image base " + format_base_argument(spec.base) + " is zero or not a multiple of 64 KiB",
                          "Reserve the patch address range on a 64 KiB boundary.");
    }
    Result<void> extras = check_extra_link_args(spec.extra_args);
    if (!extras) {
        return extras.error();
    }
    std::vector<std::string> args = {"/nologo",
                                     "/DLL",
                                     "/NOENTRY",
                                     "/NODEFAULTLIB",
                                     "/MACHINE:X64",
                                     "/DEBUG:FULL",
                                     "/INCREMENTAL:NO",
                                     "/OPT:NOREF",
                                     "/OPT:NOICF",
                                     "/FUNCTIONPADMIN",
                                     "/FIXED",
                                     "/DYNAMICBASE:NO",
                                     "/BASE:" + format_base_argument(spec.base),
                                     "/PDB:" + spec.out_pdb,
                                     "/OUT:" + spec.out_dll};
    if (!spec.ignore_warnings.empty()) {
        std::string ignore = "/IGNORE:";
        for (std::size_t i = 0; i < spec.ignore_warnings.size(); ++i) {
            ignore += (i == 0 ? "" : ",") + std::to_string(spec.ignore_warnings[i]);
        }
        args.push_back(std::move(ignore));
    }
    for (const std::string& a : spec.extra_args) {
        args.push_back(a);
    }
    for (const std::string& o : spec.objects) {
        args.push_back(o);
    }
    return args;
}

Result<std::vector<std::byte>> make_padding_binding_object() {
    const coff::BindSymbol pad{std::string(kBindingPadSymbol), coff::BindKind::Data};
    Result<coff::ByteVector> bytes = coff::make_binding_object(std::span<const coff::BindSymbol>(&pad, 1));
    if (!bytes) {
        return bytes.error();
    }
    return std::vector<std::byte>(bytes->begin(), bytes->end());
}

bool needs_response_file(std::string_view program, std::span<const std::string> args, std::size_t limit) {
    const std::string line = quote_argument(program) + " " + join_command_line(args);
    return line.size() > limit;
}

std::vector<std::byte> render_response_file(std::span<const std::string> args) {
    std::string text;
    for (std::size_t i = 0; i < args.size(); ++i) {
        text += quote_argument(args[i]);
        text += "\r\n";
    }
    std::vector<std::byte> out;
    out.reserve(2 + text.size() * 2);
    out.push_back(std::byte{0xFF});
    out.push_back(std::byte{0xFE});
    auto put16 = [&out](std::uint32_t u) {
        out.push_back(static_cast<std::byte>(u & 0xFF));
        out.push_back(static_cast<std::byte>((u >> 8) & 0xFF));
    };
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = 0xFFFD;
        std::size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size() && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size() && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(text[i + 2]) & 0xC0) == 0x80) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size() && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(text[i + 2]) & 0xC0) == 0x80 && (static_cast<unsigned char>(text[i + 3]) & 0xC0) == 0x80) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
            len = 4;
        }
        i += len;
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            cp -= 0x10000;
            put16(0xD800 + (cp >> 10));
            put16(0xDC00 + (cp & 0x3FF));
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            put16(0xFFFD);
        } else {
            put16(cp);
        }
    }
    return out;
}

LinkOutputSummary summarize_link_output(std::string_view output) {
    LinkOutputSummary s;
    std::size_t pos = 0;
    while (pos <= output.size()) {
        std::size_t eol = output.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = output.size();
        }
        const std::string line = trim(output.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line.rfind(">>>", 0) == 0) {
            continue;
        }
        const bool msvc_error = line.find("error LNK") != std::string::npos;
        const bool msvc_warning = line.find("warning LNK") != std::string::npos;
        const bool lld_error = line.find("lld-link: error:") != std::string::npos || line.find(": error: ") != std::string::npos;
        const bool lld_warning = line.find("lld-link: warning:") != std::string::npos || line.find(": warning: ") != std::string::npos;
        if (msvc_error || lld_error) {
            s.errors.push_back(line);
            static const char kMsvc[] = "unresolved external symbol ";
            static const char kLld[] = "undefined symbol: ";
            const std::size_t m = line.find(kMsvc);
            if (m != std::string::npos) {
                std::string rest = line.substr(m + sizeof(kMsvc) - 1);
                std::string name;
                if (!rest.empty() && rest[0] == '"') {
                    // "int __cdecl foo(void)" (?foo@@YAHXZ) referenced in function ...
                    const std::size_t close = rest.find("\" (", 1);
                    if (close != std::string::npos) {
                        const std::size_t end = rest.find(')', close + 3);
                        if (end != std::string::npos) {
                            name = rest.substr(close + 3, end - (close + 3));
                        }
                    }
                }
                if (name.empty()) {
                    const std::size_t ref = rest.find(" referenced");
                    name = trim(ref == std::string::npos ? std::string_view(rest) : std::string_view(rest).substr(0, ref));
                }
                s.unresolved.push_back(name);
            } else {
                const std::size_t l = line.find(kLld);
                if (l != std::string::npos) {
                    s.unresolved.push_back(trim(std::string_view(line).substr(l + sizeof(kLld) - 1)));
                }
            }
        } else if (msvc_warning || lld_warning) {
            s.warnings.push_back(line);
        }
    }
    return s;
}

// ---- 3. the linked patch ----------------------------------------------------------------------------------

const SectionPlacement* LinkedPatch::find_section(std::uint32_t object, std::uint32_t section_index) const noexcept {
    const auto it = std::lower_bound(sections.begin(), sections.end(), std::make_pair(object, section_index),
                                     [](const SectionPlacement& p, const std::pair<std::uint32_t, std::uint32_t>& key) {
                                         return std::make_pair(p.object, p.section_index) < key;
                                     });
    if (it == sections.end() || it->object != object || it->section_index != section_index) {
        return nullptr;
    }
    return &*it;
}

Result<LinkedPatch> read_linked_patch(const ReadPatchRequest& request) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t0 = Clock::now();

    LinkedPatch out;
    out.dll_path = request.dll_path;
    out.pdb_path = request.pdb_path;

    // ---- the DLL header ----------------------------------------------------------------------------------
    {
        Result<pe::OwnedImage> image = pe::load_image_file(request.dll_path);
        if (!image) {
            return Diag::make(diag::kPatchImageUnreadable,
                              "cannot read the patch DLL '" + request.dll_path + "': " + image.error().message,
                              "Check that the linker produced the DLL and that it is a valid x64 image.");
        }
        out.base = image->image.optional_header().image_base;
        out.image_size = image->image.optional_header().size_of_image;
    }
    if (request.base != 0 && out.base != request.base) {
        return Diag::make(diag::kBaseMismatch,
                          "the patch DLL '" + request.dll_path + "' has ImageBase " + format_base_argument(out.base) +
                              " instead of the requested " + format_base_argument(request.base),
                          "Link with /FIXED /BASE at a free address; do not pass options that change the base.");
    }

    // ---- the PDB -----------------------------------------------------------------------------------------
    // The index memory-maps the PDB; it is created in this scope and destroyed at its end, before the caller can relink
    // the same PDB path (ADR-0017). The process-wide cache is not used so that nothing else keeps the mapping alive.
    {
        pdb::OpenOptions opts;
        opts.use_cache = false;
        auto index = pdb::PdbIndex::open_pdb(request.pdb_path, opts);
        if (!index) {
            return Diag::make(diag::kPatchPdbUnreadable,
                              "cannot open the patch PDB '" + request.pdb_path + "': " + index.error().message,
                              "Check that the linker wrote the PDB (/DEBUG:FULL /PDB:...).");
        }
        auto publics = (*index)->all_publics();
        if (!publics) {
            return Diag::make(diag::kPatchPdbUnreadable,
                              "cannot read the publics of the patch PDB '" + request.pdb_path + "': " + publics.error().message,
                              "Check that the linker wrote a complete PDB.");
        }
        std::unordered_map<std::string_view, std::uint32_t> rva;
        rva.reserve(publics->size());
        for (const pdb::SymbolView& s : *publics) {
            if (s.rva != pdb::kNoRva) {
                rva.emplace(s.name, s.rva);
            }
        }

        for (std::uint32_t oi = 0; oi < request.objects.size(); ++oi) {
            const PatchObjectInput& obj = request.objects[oi];
            for (const coff::SectionLocator& loc : obj.locators) {
                if (loc.kind == coff::LocatorKind::None) {
                    continue;
                }
                const auto it = rva.find(loc.symbol_name);
                if (it == rva.end() || it->second < loc.offset) {
                    return Diag::make(diag::kLocatorMissing,
                                      "the locator '" + loc.symbol_name + "' of section " + std::to_string(loc.section_index) +
                                          " of '" + obj.path + "' is not a public of the patch PDB",
                                      "Link with /OPT:NOREF /OPT:NOICF and pass the anchored object (coff::add_anchors).");
                }
                SectionPlacement p;
                p.object = oi;
                p.section_index = loc.section_index;
                p.kind = loc.kind;
                p.symbol = loc.symbol_name;
                p.symbol_rva = it->second;
                p.section_rva = it->second - loc.offset;
                out.sections.push_back(std::move(p));
            }
        }
        std::sort(out.sections.begin(), out.sections.end(), [](const SectionPlacement& a, const SectionPlacement& b) {
            return std::make_pair(a.object, a.section_index) < std::make_pair(b.object, b.section_index);
        });

        for (const coff::BindSymbol& b : request.bindings) {
            if (b.kind == coff::BindKind::Function) {
                const std::string slot = coff::slot_name(b.name);
                const auto s = rva.find(slot);
                const auto t = rva.find(b.name);
                if (s == rva.end() || t == rva.end()) {
                    return Diag::make(diag::kSlotMissing,
                                      std::string("the ") + (s == rva.end() ? "slot '" + slot : "thunk '" + b.name) +
                                          "' is not a public of the patch PDB '" + request.pdb_path + "'",
                                      "Link binding.obj with the patch and make sure the symbol is in the bind list.");
                }
                out.slot_rvas[b.name] = s->second;
                out.thunk_rvas[b.name] = t->second;
            } else {
                const auto d = rva.find(b.name);
                if (d == rva.end()) {
                    return Diag::make(diag::kSlotMissing,
                                      "the data placeholder '" + b.name + "' is not a public of the patch PDB '" + request.pdb_path + "'",
                                      "Link binding.obj with the patch and make sure the symbol is in the bind list.");
                }
                out.data_rvas[b.name] = d->second;
            }
        }
        for (const std::string& f : request.new_functions) {
            const auto it = rva.find(f);
            if (it == rva.end()) {
                return Diag::make(diag::kFunctionMissing, "the function '" + f + "' is not a public of the patch PDB '" + request.pdb_path + "'",
                                  "Check that the patch object defines it with external linkage.");
            }
            out.function_rvas[f] = it->second;
        }
    }
    out.pdb_read_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

}  // namespace cml::toolchain
