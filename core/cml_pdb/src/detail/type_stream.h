#pragma once

// WP-37: lazy reader of one type stream of a PDB (TPI = stream 2, IPI = stream 4).
//
// open() reads the 56-byte stream header and the "type index offsets" table of the hash stream: one (type index,
// offset) pair for roughly every 8 KiB of records, written by link.exe, lld-link and the compiler's own PDB writer.
// No type record is read. A lookup finds the chunk between two table entries, reads just that chunk (in place when its
// blocks are consecutive in the file, else one copy of the chunk) and walks its records once. A PDB with a very large
// type stream therefore costs what is looked at, not the stream size.
//
// A stream without a usable table is one chunk: the first lookup walks all of it (what cml_pdb did before).
//
// Untrusted input: the header, the table and every record are bounds-checked; a chunk whose records do not add up to
// what the table promises is "bad" and its indices are simply not found. Memory is proportional to the table (8 bytes
// per entry, as in the file) plus the chunks actually read.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "cml/pdb/type_records.h"
#include "cml/util/result.h"
#include "detail/counting.h"
#include "detail/msf.h"

namespace cml::pdb::detail {

class TypeStream {
public:
    // `what` is "TPI" or "IPI" (messages). CML1430 when the stream is missing or its header is invalid.
    static Result<std::unique_ptr<TypeStream>> open(const Msf& msf, std::uint32_t stream, const std::string& pdb_path, const char* what);

    bool find(std::uint32_t index, TypeRecord& out) const;
    // WP-46 (R-54): calls `visit` for every record in index order. Chunks that were not loaded yet are read for the walk only
    // and not kept. False when a chunk is malformed or the records do not add up to the header's index range (the walk then
    // saw less than the stream holds). `visit` must not call back into this stream.
    bool walk(const std::function<void(std::uint32_t index, const TypeRecord& record)>& visit) const;
    TypeStreamStats stats() const;
    std::uint32_t type_begin() const noexcept { return type_begin_; }
    std::uint32_t type_end() const noexcept { return type_end_; }  // one past the last index the header promises (WP-46)

private:
    TypeStream() = default;

    // One entry of the offset table: the chunk [offset, next entry's offset) starts with record `first_index`.
    struct Entry {
        std::uint32_t first_index = 0;
        std::uint32_t offset = 0;  // within the record area
    };
    // A chunk that was read.
    struct Chunk {
        bool bad = false;
        const std::uint8_t* data = nullptr;
        CountedBuffer owned;         // when the chunk's blocks are not consecutive in the file
        Vec<std::uint32_t> offsets;  // of each record header within the chunk
    };

    void load(std::size_t entry, Chunk& chunk) const;

    const Msf* msf_ = nullptr;
    std::uint32_t stream_ = 0;
    std::uint32_t header_size_ = 0;
    std::uint32_t type_begin_ = 0;
    std::uint32_t type_end_ = 0;
    std::uint32_t record_bytes_ = 0;
    Vec<Entry> entries_;  // by first_index; at least one

    mutable std::mutex mutex_;
    mutable std::map<std::size_t, Chunk> loaded_;  // by entry; nodes are stable, so returned record bytes stay valid
    mutable TypeStreamStats stats_;
};

}  // namespace cml::pdb::detail
