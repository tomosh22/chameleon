#include "cml/core/source_baseline.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "cml/core/pipeline_diag_ids.h"
#include "cml/util/hash.h"
#include "cml/util/log.h"

namespace cml::core {

namespace fs = std::filesystem;

namespace {

std::string lower_slashes(std::string_view path) {
    std::string out(path);
    for (char& c : out) {
        if (c == '/') {
            c = '\\';
        } else if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool hash_of(const std::string& path, std::uint64_t* hash) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return false;
    }
    *hash = cml::hash64(bytes.data(), bytes.size());
    return true;
}

}  // namespace

bool stat_file(const std::string& path, std::uint64_t* size, long long* mtime) {
    std::error_code ec;
    const fs::path p(path);
    const std::uintmax_t s = fs::file_size(p, ec);
    if (ec) {
        return false;
    }
    const fs::file_time_type t = fs::last_write_time(p, ec);
    if (ec) {
        return false;
    }
    *size = static_cast<std::uint64_t>(s);
    *mtime = static_cast<long long>(t.time_since_epoch().count());
    return true;
}

bool is_system_path(std::string_view path) {
    static const char* const kFragments[] = {
        "\\windows kits\\",      "\\microsoft visual studio\\", "\\program files\\llvm\\", "\\program files (x86)\\llvm\\",
        "\\program files\\microsoft visual studio\\", "\\vc\\tools\\msvc\\", "\\vc\\tools\\llvm\\",
        "/usr/include/",         "/usr/lib/",
    };
    const std::string p = lower_slashes(path);
    for (const char* f : kFragments) {
        const std::string frag = lower_slashes(f);
        if (p.find(frag) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool stamp_file(const std::string& path, FileStamp* out) {
    FileStamp s;
    if (!stat_file(path, &s.size, &s.mtime) || !hash_of(path, &s.hash)) {
        return false;
    }
    *out = s;
    return true;
}

void SourceBaseline::take(const std::vector<std::string>& files, const BaselineOptions& options, std::vector<Diag>* warnings) {
    std::map<std::string, FileStamp> taken;
    std::size_t done = 0;
    std::size_t last_reported = 0;
    for (const std::string& file : files) {
        if (options.cancelled && options.cancelled()) {
            break;
        }
        ++done;
        if (!options.track_system && is_system_path(file)) {
            continue;
        }
        FileStamp s;
        if (stamp_file(file, &s)) {
            taken[file] = s;
        } else {
            std::error_code ec;
            if (fs::exists(fs::path(file), ec)) {
                if (warnings != nullptr) {
                    warnings->push_back(Diag::make(pipeline::diag::kBaselineFile, "cannot read the source file '" + file + "' for the baseline",
                                                   "Changes to this file will not be detected; check its permissions."));
                }
            }
            // A file that does not exist (a generated file that was cleaned, a path of another machine) is not tracked.
        }
        if (options.progress && (done - last_reported >= 64 || done == files.size())) {
            last_reported = done;
            options.progress(done, files.size());
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    stamps_ = std::move(taken);
}

SourceBaseline::SweepResult SourceBaseline::sweep() const {
    std::map<std::string, FileStamp> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = stamps_;
    }
    SweepResult out;
    for (const auto& entry : snapshot) {
        ++out.checked;
        std::uint64_t size = 0;
        long long mtime = 0;
        if (!stat_file(entry.first, &size, &mtime)) {
            out.missing.push_back(entry.first);
            continue;
        }
        if (size == entry.second.size && mtime == entry.second.mtime) {
            continue;
        }
        ++out.hashed;
        std::uint64_t hash = 0;
        if (!hash_of(entry.first, &hash)) {
            out.missing.push_back(entry.first);
            continue;
        }
        if (hash != entry.second.hash) {
            out.changed.push_back(entry.first);
            out.stamps[entry.first] = FileStamp{hash, size, mtime};
        }
    }
    return out;
}

void SourceBaseline::accept(const std::map<std::string, FileStamp>& stamps) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : stamps) {
        const auto previous = stamps_.find(entry.first);
        if (previous != stamps_.end() && originals_.count(entry.first) == 0) {
            originals_[entry.first] = previous->second;
        }
        stamps_[entry.first] = entry.second;
    }
}

BaselineSnapshot SourceBaseline::session_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    BaselineSnapshot out;
    out.stamps = stamps_;
    out.originals = originals_;
    return out;
}

std::size_t SourceBaseline::restore(const BaselineSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : snapshot.stamps) {
        const auto mine = stamps_.find(entry.first);
        if (mine != stamps_.end()) {
            mine->second = entry.second;
        }
    }
    std::size_t restored = 0;
    for (const auto& entry : snapshot.originals) {
        const auto mine = stamps_.find(entry.first);
        if (mine == stamps_.end()) {
            continue;
        }
        originals_[entry.first] = entry.second;
        // Size and time that no file can have: the next sweep must read the file and compare its hash with the original.
        mine->second = FileStamp{entry.second.hash, ~static_cast<std::uint64_t>(0), -1};
        ++restored;
    }
    return restored;
}

std::size_t SourceBaseline::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stamps_.size();
}

std::vector<std::string> SourceBaseline::files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(stamps_.size());
    for (const auto& entry : stamps_) {
        out.push_back(entry.first);
    }
    return out;
}

bool SourceBaseline::tracks(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stamps_.count(path) != 0;
}

}  // namespace cml::core
