#include "cml/core/module_state.h"

#include <algorithm>

namespace cml::core {

namespace {
constexpr std::uint64_t kGranule = 0x10000;  // 64 KiB: the allocation granularity images are placed on
std::uint64_t align_up(std::uint64_t v) { return (v + kGranule - 1) & ~(kGranule - 1); }
}  // namespace

std::optional<SymbolAddress> SharedRegistry::find(std::string_view decorated_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registry_.find(decorated_name);
}

std::optional<SymbolAddress> SharedRegistry::replace(std::string name, SymbolAddress address) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<SymbolAddress> previous = registry_.find(name);
    registry_.add(std::move(name), std::move(address));
    return previous;
}

void SharedRegistry::restore(const std::string& name, const std::optional<SymbolAddress>& previous) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (previous) {
        registry_.add(name, *previous);
    } else {
        registry_.remove(name);
    }
}

std::size_t SharedRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registry_.size();
}

void Reservation::set_blocks(std::vector<ipc::AddressBlock> blocks) {
    blocks_ = std::move(blocks);
    next_.clear();
    for (const ipc::AddressBlock& b : blocks_) {
        next_.push_back(b.base);
    }
    reserved_ = true;
}

std::uint64_t Reservation::next_base(std::uint64_t max_size) const {
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        const std::uint64_t candidate = align_up(next_[i]);
        const std::uint64_t end = blocks_[i].base + blocks_[i].size;
        if (candidate >= blocks_[i].base && candidate < end && end - candidate >= max_size) {
            return candidate;
        }
    }
    return 0;
}

void Reservation::consume(std::uint64_t base, std::uint64_t size) {
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (base >= blocks_[i].base && base < blocks_[i].base + blocks_[i].size) {
            next_[i] = std::max(next_[i], align_up(base + size) + kGranule);
            return;
        }
    }
}

std::uint64_t Reservation::remaining() const {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        const std::uint64_t end = blocks_[i].base + blocks_[i].size;
        total += next_[i] < end ? end - next_[i] : 0;
    }
    return total;
}

bool ModuleState::is_hooked(std::uint64_t old_va) {
    std::lock_guard<std::mutex> lock(mutex);
    return hooked.count(old_va) != 0;
}

}  // namespace cml::core
