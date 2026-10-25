#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "cml/util/result.h"
#include "detail/counting.h"
#include "detail/mapped_file.h"

namespace cml::pdb::detail {

// Bytes of one MSF stream. Zero-copy (points into the mapping) when the stream's blocks are
// consecutive in the file; otherwise an owned, counted copy. The mapping must outlive it.
class StreamData {
public:
    StreamData() noexcept = default;
    StreamData(const std::uint8_t* view, std::size_t size) noexcept : data_(view), size_(size) {}
    explicit StreamData(CountedBuffer&& owned) noexcept
        : data_(owned.data()), size_(owned.size()), owned_(std::move(owned)) {}
    StreamData(StreamData&& o) noexcept : data_(o.data_), size_(o.size_), owned_(std::move(o.owned_)) {
        o.data_ = nullptr;
        o.size_ = 0;
    }
    StreamData& operator=(StreamData&& o) noexcept {
        if (this != &o) {
            owned_ = std::move(o.owned_);
            data_ = o.data_;
            size_ = o.size_;
            o.data_ = nullptr;
            o.size_ = 0;
        }
        return *this;
    }
    StreamData(const StreamData&) = delete;
    StreamData& operator=(const StreamData&) = delete;

    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool owned() const noexcept { return owned_.data() != nullptr; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    CountedBuffer owned_;
};

// MSF 7.00 container (the PDB file layout): super block, stream directory, stream block lists.
// All structure is validated at open(); afterwards stream reads cannot go out of bounds.
class Msf {
public:
    static constexpr std::uint32_t kNilSize = 0xFFFFFFFFu;

    static Result<std::unique_ptr<Msf>> open(std::shared_ptr<const MappedFile> file);

    std::uint32_t block_size() const noexcept { return block_size_; }
    std::uint32_t block_count() const noexcept { return block_count_; }
    std::uint32_t stream_count() const noexcept { return static_cast<std::uint32_t>(sizes_.size()); }
    // 0 for nil or out-of-range streams.
    std::uint32_t stream_size(std::uint32_t stream) const noexcept;
    bool stream_exists(std::uint32_t stream) const noexcept;

    // Whole stream (see StreamData). Empty for nil / out-of-range streams.
    StreamData read_stream(std::uint32_t stream) const;

    // [offset, offset+length) of a stream, clamped to the stream size. Returns a pointer into the
    // mapping when the range lies within consecutive blocks, otherwise fills `scratch` and returns
    // its data. `got` receives the clamped length.
    const std::uint8_t* read_range(std::uint32_t stream, std::uint64_t offset, std::size_t length,
                                   CountedBuffer& scratch, std::size_t& got) const;

private:
    Msf() = default;
    const std::uint32_t* blocks_of(std::uint32_t stream) const noexcept;

    std::shared_ptr<const MappedFile> file_;
    std::uint32_t block_size_ = 0;
    std::uint32_t block_count_ = 0;
    Vec<std::uint32_t> sizes_;          // per stream, kNilSize for nil
    Vec<std::uint64_t> first_block_;    // per stream: index into blocks_
    Vec<std::uint32_t> blocks_;         // all streams' block lists, concatenated
};

}  // namespace cml::pdb::detail
