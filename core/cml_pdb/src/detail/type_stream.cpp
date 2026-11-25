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
    out->type_end_ = type_end;
    out->stats_.opened = true;
    out->stats_.record_bytes = out->record_bytes_;

    // The type-index offset table. Any defect in it means "no table": the stream is then one chunk.
    bool table_ok = hash_stream != 0xFFFFu && table_bytes >= 8 && table_bytes % 8 == 0 && table_bytes <= kMaxTableBytes &&
                    static_cast<std::uint64_t>(table_offset) + table_bytes <= msf.stream_size(hash_stream);
    if (table_ok) {
        CountedBuffer table_scratch;
        std::size_t table_got = 0;
        const std::uint8_t* t = msf.read_range(hash_stream, table_offset, table_bytes, table_scratch, table_got);
        table_ok = t != nullptr && table_got == table_bytes;
        if (table_ok) {
            // Validated before anything is allocated: the first entry is (first index, 0), indices and offsets rise
            // strictly and stay inside the stream.
            const std::uint32_t n = table_bytes / 8;
            std::uint32_t previous_index = 0;
            std::uint32_t previous_offset = 0;
            for (std::uint32_t i = 0; i < n && table_ok; ++i) {
                const std::uint32_t index = rd32(t + static_cast<std::size_t>(i) * 8);
                const std::uint32_t offset = rd32(t + static_cast<std::size_t>(i) * 8 + 4);
                table_ok = i == 0 ? (index == out->type_begin_ && offset == 0) : (index > previous_index && offset > previous_offset);
                table_ok = table_ok && index < type_end && offset < out->record_bytes_;
                previous_index = index;
                previous_offset = offset;
            }
            if (table_ok) {
                out->entries_.reserve(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    Entry e;
                    e.first_index = rd32(t + static_cast<std::size_t>(i) * 8);
                    e.offset = rd32(t + static_cast<std::size_t>(i) * 8 + 4);
                    out->entries_.push_back(e);
                }
            }
        }
    }
    if (!table_ok) {
        Entry whole;
        whole.first_index = out->type_begin_;
        out->entries_.assign(1, whole);
    }
    out->stats_.sparse = out->entries_.size() > 1;
    out->stats_.chunks = static_cast<std::uint32_t>(out->entries_.size());
    return out;
}

void TypeStream::load(std::size_t entry, Chunk& chunk) const {
    chunk.bad = true;
    const bool last = entry + 1 == entries_.size();
    const std::uint32_t begin = entries_[entry].offset;
    const std::uint32_t size = (last ? record_bytes_ : entries_[entry + 1].offset) - begin;
    // How many records the table promises (unknown for the last chunk, whose end is the end of the stream).
    const std::uint32_t expected = last ? 0u : entries_[entry + 1].first_index - entries_[entry].first_index;
    if (size == 0) {
        chunk.bad = !last;  // an empty stream is fine; an empty chunk between two entries cannot be (offsets rise)
        return;
    }
    CountedBuffer scratch;
    std::size_t got = 0;
    const std::uint8_t* data = msf_->read_range(stream_, static_cast<std::uint64_t>(header_size_) + begin, size, scratch, got);
    if (data == nullptr || got < size) {
        return;
    }
    // First pass counts, so the offset table is allocated once and exactly.
    for (int pass = 0; pass < 2; ++pass) {
        std::uint32_t off = 0;
        std::uint32_t count = 0;
        while (off < size) {
            if (size - off < 4) {
                break;  // a trailing fragment too short for a record header
            }
            const std::uint32_t reclen = rd16(data + off);
            if (reclen < 2 || reclen > size - off - 2) {
                return;  // a record leaves the chunk
            }
            if (pass == 1) {
                chunk.offsets.push_back(off);
            }
            ++count;
            off += 2 + reclen;
        }
        if (pass == 0) {
            if (!last && count != expected) {
                return;  // the table and the records disagree
            }
            chunk.offsets.reserve(count);
        }
    }
    if (scratch.data() != nullptr && data == scratch.data()) {
        chunk.owned = std::move(scratch);
        chunk.data = chunk.owned.data();
        stats_.bytes_copied += size;
    } else {
        chunk.data = data;
    }
    chunk.bad = false;
    ++stats_.chunks_loaded;
    stats_.bytes_loaded += size;
}

bool TypeStream::find(std::uint32_t index, TypeRecord& out) const {
    if (index < type_begin_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.lookups;
    // The last entry whose first index is <= index (entries_[0].first_index == type_begin_).
    const auto it = std::upper_bound(entries_.begin(), entries_.end(), index, [](std::uint32_t v, const Entry& e) { return v < e.first_index; });
    if (it == entries_.begin()) {
        return false;
    }
    const std::size_t entry = static_cast<std::size_t>(it - entries_.begin()) - 1;
    auto found = loaded_.find(entry);
    if (found == loaded_.end()) {
        found = loaded_.try_emplace(entry).first;
        load(entry, found->second);
    }
    const Chunk& chunk = found->second;
    if (chunk.bad) {
        return false;
    }
    const std::uint32_t i = index - entries_[entry].first_index;
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

bool TypeStream::walk(const std::function<void(std::uint32_t index, const TypeRecord& record)>& visit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t visited = 0;
    for (std::size_t entry = 0; entry < entries_.size(); ++entry) {
        // A chunk that is already loaded is used; any other is read for this walk only and not kept (a walk of the whole
        // stream must not hold all of it in memory afterwards).
        Chunk scratch;
        const auto cached = loaded_.find(entry);
        const Chunk* chunk = &scratch;
        if (cached != loaded_.end()) {
            chunk = &cached->second;
        } else {
            // (load() counts what it reads as kept: chunks, bytes loaded and copied. This chunk is not kept, so none of it
            // stays in the statistics, which describe what the stream holds.)
            const TypeStreamStats before = stats_;
            load(entry, scratch);
            stats_.chunks_loaded = before.chunks_loaded;
            stats_.bytes_loaded = before.bytes_loaded;
            stats_.bytes_copied = before.bytes_copied;
        }
        if (chunk->bad) {
            return false;
        }
        for (std::size_t i = 0; i < chunk->offsets.size(); ++i) {
            const std::uint32_t off = chunk->offsets[i];
            const std::uint32_t reclen = rd16(chunk->data + off);
            TypeRecord record;
            record.kind = rd16(chunk->data + off + 2);
            record.body = chunk->data + off + 4;
            record.body_len = reclen - 2;
            visit(entries_[entry].first_index + static_cast<std::uint32_t>(i), record);
            ++visited;
        }
    }
    return visited == static_cast<std::uint64_t>(type_end_ - type_begin_);
}

TypeStreamStats TypeStream::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

}  // namespace cml::pdb::detail
