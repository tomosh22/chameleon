// WP-55 (R-62): the verdicts on inline sites kept across the reloads of one module image (bind_plan.h, InlineVerdictCache).
#include "cml/core/bind_plan.h"

namespace cml::core::bind {

std::optional<CachedInlineVerdict> InlineVerdictCache::find(const pdb::PdbId& image, std::uint32_t compiland, std::uint32_t proc, std::uint32_t rva,
                                                            std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (image.is_zero() || image != image_) {
        return std::nullopt;
    }
    const auto it = entries_.find({compiland, proc});
    if (it == entries_.end() || it->second.proc_rva != rva || it->second.proc_name != name) {
        return std::nullopt;
    }
    ++hits_;
    return it->second;
}

void InlineVerdictCache::store(const pdb::PdbId& image, std::uint32_t compiland, std::uint32_t proc, CachedInlineVerdict verdict) {
    if (image.is_zero()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (image != image_) {
        entries_.clear();  // another image (the PDB was mapped again from a relinked module): nothing kept applies to it
        image_ = image;
    }
    entries_[{compiland, proc}] = std::move(verdict);
    ++stores_;
}

std::size_t InlineVerdictCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::uint64_t InlineVerdictCache::hits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hits_;
}

std::uint64_t InlineVerdictCache::stores() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stores_;
}

void InlineVerdictCache::count_kept() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++kept_;
}

void InlineVerdictCache::count_read() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++reads_;
}

std::uint64_t InlineVerdictCache::kept() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kept_;
}

std::uint64_t InlineVerdictCache::reads() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reads_;
}

}  // namespace cml::core::bind
