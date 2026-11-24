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
// what the table promises is "bad" and its indices are simply not found.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
    TypeStreamStats stats() const;
    std::uint32_t type_begin() const noexcept { return type_begin_; }

private:
    TypeStream() = default;

    enum : std::uint8_t { kNotLoaded = 0, kReady = 1, kBad = 2 };
    static constexpr std::uint32_t kUnknownCount = 0xFFFFFFFFu;

    struct Chunk {
        std::uint32_t first_index = 0;
        std::uint32_t offset = 0;    // of the chunk within the record area
        std::uint32_t size = 0;
        std::uint32_t expected = kUnknownCount;  // records the table promises (unknown for the last chunk)
        std::uint8_t state = kNotLoaded;
        const std::uint8_t* data = nullptr;
        CountedBuffer owned;         // when the chunk's blocks are not consecutive in the file
        Vec<std::uint32_t> offsets;  // of each record header within the chunk
    };

    void load(Chunk& chunk) const;

    const Msf* msf_ = nullptr;
    std::uint32_t stream_ = 0;
    std::uint32_t header_size_ = 0;
    std::uint32_t type_begin_ = 0;
    std::uint32_t record_bytes_ = 0;

    mutable std::mutex mutex_;
    mutable Vec<Chunk> chunks_;  // by first_index
    mutable TypeStreamStats stats_;
};

}  // namespace cml::pdb::detail
