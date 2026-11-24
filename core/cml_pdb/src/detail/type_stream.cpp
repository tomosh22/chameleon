#include "detail/type_stream.h"

#include <algorithm>

#include "cml/pdb/diag_ids.h"
#include "detail/le.h"

namespace cml::pdb::detail {

namespace {

constexpr std::uint32_t kHeaderBytes = 56;
// The table of a 4 GiB stream at one entry per 8 KiB is about 4 MiB; anything far above that is not a table.
constexpr std::uint32_t kMaxTableBytes = 64u << 20;

}  // namespace

Result<std::unique_ptr<TypeStream>> TypeStream::open(const Msf& msf, std::uint32_t stream, const std::string& pdb_path, const char* what) {
    const auto fail = [&](const char* why) {
        return Diag::make(diag::kBadTypeStream, std::string("The ") + what + " stream of '" + pdb_path + "' is unusable (" + why + "); type information is unavailable.",
                          "Rebuild the module so the linker regenerates the PDB.");
    };
    const std::uint32_t size = msf.stream_size(stream);
    if (size < kHeaderBytes) {
        return fail("stream missing or shorter than its header");
    }
    CountedBuffer scratch;
    std::size_t got = 0;
    const std::uint8_t* h = msf.read_range(stream, 0, kHeaderBytes, scratch, got);
    if (h == nullptr || got < kHeaderBytes) {
        return fail("the header cannot be read");
    }
    std::unique_ptr<TypeStream> out(new TypeStream());
    out->msf_ = &msf;
    out->stream_ = stream;
    out->header_size_ = rd32(h + 4);
    out->type_begin_ = rd32(h + 8);
    const std::uint32_t type_end = rd32(h + 12);
    out->record_bytes_ = rd32(h + 16);
    const std::uint32_t hash_stream = rd16(h + 20);
    const std::uint32_t table_offset = rd32(h + 40);
    const std::uint32_t table_bytes = rd32(h + 44);
    if (out->header_size_ < kHeaderBytes || out->header_size_ > size || out->record_bytes_ > size - out->header_size_) {
        return fail("header sizes exceed the stream");
    }
    if (out->type_begin_ < 0x1000u || type_end < out->type_begin_) {
        return fail("the type index range is invalid");
    }
    out->stats_.opened = true;
    out->stats_.record_bytes = out->record_bytes_;

    // The type-index offset table. Any defect in it means "no table": the stream is then one chunk.
    Vec<Chunk> chunks;
    bool table_ok = hash_stream != 0xFFFFu && table_bytes >= 8 && table_bytes % 8 == 0 && table_bytes <= kMaxTableBytes &&
                    static_cast<std::uint64_t>(table_offset) + table_bytes <= msf.stream_size(hash_stream);
    if (table_ok) {
        CountedBuffer table_scratch;
        std::size_t table_got = 0;
        const std::uint8_t* t = msf.read_range(hash_stream, table_offset, table_bytes, table_scratch, table_got);
        table_ok = t != nullptr && table_got == table_bytes;
        if (table_ok) {
            const std::uint32_t n = table_bytes / 8;
            chunks.resize(n);
            for (std::uint32_t i = 0; i < n && table_ok; ++i) {
                const std::uint32_t index = rd32(t + static_cast<std::size_t>(i) * 8);
                const std::uint32_t offset = rd32(t + static_cast<std::size_t>(i) * 8 + 4);
                if (i == 0) {
                    table_ok = index == out->type_begin_ && offset == 0;
                } else {
                    table_ok = index > chunks[i - 1].first_index && offset > chunks[i - 1].offset;
                }
                table_ok = table_ok && index < type_end && offset < out->record_bytes_;
                chunks[i].first_index = index;
                chunks[i].offset = offset;
            }
            for (std::uint32_t i = 0; i < n && table_ok; ++i) {
                const bool last = i + 1 == n;
                chunks[i].size = (last ? out->record_bytes_ : chunks[i + 1].offset) - chunks[i].offset;
                chunks[i].expected = last ? kUnknownCount : chunks[i + 1].first_index - chunks[i].first_index;
            }
        }
    }
    if (!table_ok) {
        chunks.clear();
        chunks.resize(1);
        chunks[0].first_index = out->type_begin_;
        chunks[0].offset = 0;
        chunks[0].size = out->record_bytes_;
    }
    out->stats_.sparse = table_ok && chunks.size() > 1;
    out->stats_.chunks = static_cast<std::uint32_t>(chunks.size());
    out->chunks_ = std::move(chunks);
    return out;
}

void TypeStream::load(Chunk& chunk) const {
    chunk.state = kBad;
    if (chunk.size == 0) {
        chunk.state = chunk.expected == kUnknownCount || chunk.expected == 0 ? kReady : kBad;
        return;
    }
    CountedBuffer scratch;
    std::size_t got = 0;
    const std::uint8_t* data = msf_->read_range(stream_, static_cast<std::uint64_t>(header_size_) + chunk.offset, chunk.size, scratch, got);
    if (data == nullptr || got < chunk.size) {
        return;
    }
    // First pass counts, so the offset table is allocated once and exactly.
    std::uint32_t count = 0;
    for (int pass = 0; pass < 2; ++pass) {
        std::uint32_t off = 0;
        std::uint32_t n = 0;
        while (off < chunk.size) {
            if (chunk.size - off < 4) {
                break;  // a trailing fragment too short for a record header
            }
            const std::uint32_t reclen = rd16(data + off);
            if (reclen < 2 || reclen > chunk.size - off - 2) {
                return;  // a record leaves the chunk
            }
            if (pass == 1) {
                chunk.offsets.push_back(off);
            }
            ++n;
            off += 2 + reclen;
        }
        if (pass == 0) {
            count = n;
            if (chunk.expected != kUnknownCount && count != chunk.expected) {
                return;  // the table and the records disagree
            }
            chunk.offsets.reserve(count);
        }
    }
    if (scratch.data() != nullptr && data == scratch.data()) {
        chunk.owned = std::move(scratch);
        chunk.data = chunk.owned.data();
        stats_.bytes_copied += chunk.size;
    } else {
        chunk.data = data;
    }
    chunk.state = kReady;
    ++stats_.chunks_loaded;
    stats_.bytes_loaded += chunk.size;
}

bool TypeStream::find(std::uint32_t index, TypeRecord& out) const {
    if (index < type_begin_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.lookups;
    // The last chunk whose first index is <= index.
    auto it = std::upper_bound(chunks_.begin(), chunks_.end(), index, [](std::uint32_t v, const Chunk& c) { return v < c.first_index; });
    if (it == chunks_.begin()) {
        return false;
    }
    Chunk& chunk = *(it - 1);
    if (chunk.state == kNotLoaded) {
        load(chunk);
    }
    if (chunk.state != kReady) {
        return false;
    }
    const std::uint32_t i = index - chunk.first_index;
    if (i >= chunk.offsets.size()) {
        return false;
    }
    const std::uint32_t off = chunk.offsets[i];
    const std::uint32_t reclen = rd16(chunk.data + off);  // validated by load(): 2 <= reclen, record inside the chunk
    out.kind = rd16(chunk.data + off + 2);
    out.body = chunk.data + off + 4;
    out.body_len = reclen - 2;
    return true;
}

TypeStreamStats TypeStream::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

}  // namespace cml::pdb::detail
