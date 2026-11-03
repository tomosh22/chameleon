#include "cml/core/module_symbols.h"
#include "cml/core/source_baseline.h"

#include <filesystem>
#include <optional>
#include <utility>

#include "cml/core/pipeline_diag_ids.h"
#include "cml/pe/pe.h"
#include "cml/util/log.h"

namespace cml::core {

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kScnMemExecute = 0x20000000u;

std::shared_ptr<const ExportIndex> parse_exports(const std::string& path) {
    auto index = std::make_shared<ExportIndex>();
    Result<pe::OwnedImage> image = pe::load_image_file(path);
    if (!image) {
        index->error = image.error();
        return index;
    }
    Result<pe::ExportTable> table = image->image.exports();
    if (!table) {
        index->error = table.error();
        return index;
    }
    index->ok = true;
    index->by_name.reserve(table->entries.size());
    for (const pe::Export& e : table->entries) {
        if (!e.has_name || e.rva == 0 || e.is_forwarder || e.name.empty()) {
            continue;
        }
        ExportInfo info;
        info.rva = e.rva;
        const pe::SectionHeader* sec = image->image.section_for_rva(e.rva);
        info.is_function = sec != nullptr && (sec->characteristics & kScnMemExecute) != 0;
        index->by_name.emplace(std::string(e.name), info);
    }
    return index;
}

}  // namespace

std::shared_ptr<const ExportIndex> ExportCache::get(const std::string& path) {
    std::uint64_t size = 0;
    long long mtime = 0;
    const bool have_stamp = stat_file(path, &size, &mtime);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(path);
        if (it != entries_.end() && have_stamp && it->second.size == size && it->second.mtime == mtime) {
            return it->second.index;
        }
    }
    std::shared_ptr<const ExportIndex> index = parse_exports(path);  // outside the lock: reads a whole file
    std::lock_guard<std::mutex> lock(mutex_);
    ++parses_;
    entries_[path] = Entry{size, mtime, index};
    return index;
}

std::size_t ExportCache::parses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return parses_;
}

FileProcessModules::FileProcessModules(std::vector<LoadedModule> modules, LoadedModule patched, std::shared_ptr<ExportCache> cache,
                                       std::shared_ptr<const pe::OwnedImage> patched_image)
    : modules_(std::move(modules)),
      patched_(std::move(patched)),
      cache_(cache ? std::move(cache) : std::make_shared<ExportCache>()),
      patched_image_(std::move(patched_image)) {}

std::optional<SymbolAddress> FileProcessModules::iat_slot(std::string_view, std::string_view import_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!iat_loaded_) {
        iat_loaded_ = true;
        // The module's file is usually already loaded by the pipeline (ModuleState::image): reuse it.
        std::optional<Result<pe::OwnedImage>> loaded;
        if (!patched_image_) {
            loaded.emplace(pe::load_image_file(patched_.path));
        }
        const pe::Image* image = patched_image_ ? &patched_image_->image : (*loaded ? &(*loaded)->image : nullptr);
        if (image == nullptr) {
            skipped_.push_back(Diag::make(pipeline::diag::kModuleImage, "cannot read the module '" + patched_.path + "' to find its import slots: " + loaded->error().message,
                                          "Check that the module file still exists and is a valid x64 image."));
        } else {
            Result<std::vector<pe::ImportModule>> imports = image->imports();
            if (!imports) {
                skipped_.push_back(imports.error());
            } else {
                for (const pe::ImportModule& m : *imports) {
                    for (const pe::ImportEntry& e : m.entries) {
                        if (!e.by_ordinal && !e.name.empty()) {
                            iat_.emplace(std::string(e.name), e.iat_slot_rva);
                        }
                    }
                }
            }
        }
    }
    const auto it = iat_.find(std::string(import_name));
    if (it == iat_.end()) {
        return std::nullopt;
    }
    SymbolAddress a;
    a.va = patched_.base + it->second;
    a.is_function = false;  // the slot is data (an 8-byte pointer)
    a.origin = "IAT of " + patched_.path;
    return a;
}

std::optional<SymbolAddress> FileProcessModules::find_export(std::string_view name) const {
    if (name.empty()) {
        return std::nullopt;
    }
    const std::string key(name);
    for (const LoadedModule& m : modules_) {
        if (m.path == patched_.path || m.base == patched_.base) {
            continue;
        }
        std::shared_ptr<const ExportIndex> index = cache_->get(m.path);
        if (!index->ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            bool seen = false;
            for (const Diag& d : skipped_) {
                seen = seen || d.message.find(m.path) != std::string::npos;
            }
            if (!seen) {
                skipped_.push_back(Diag::make(pipeline::diag::kModuleImage, "cannot read the exports of '" + m.path + "': " + index->error.message,
                                              "The module is skipped when resolving symbols; check that the file exists."));
            }
            continue;
        }
        const auto it = index->by_name.find(key);
        if (it == index->by_name.end()) {
            continue;
        }
        SymbolAddress a;
        a.va = m.base + it->second.rva;
        a.is_function = it->second.is_function;
        a.origin = m.path;
        CML_LOG(Trace, "modules: export '%s' found in %s at 0x%llx", key.c_str(), m.path.c_str(), static_cast<unsigned long long>(a.va));
        return a;
    }
    return std::nullopt;
}

std::vector<Diag> FileProcessModules::skipped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return skipped_;
}

}  // namespace cml::core
