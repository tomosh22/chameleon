#include "detail/msf.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "cml/pdb/diag_ids.h"
#include "detail/le.h"

namespace cml::pdb::detail {

namespace {

constexpr std::size_t kSuperBlockSize = 56;
constexpr char kMagic7[32] = {'M', 'i', 'c', 'r', 'o', 's', 'o', 'f', 't', ' ', 'C', '/', 'C', '+', '+', ' ',
                              'M', 'S', 'F', ' ', '7', '.', '0', '0', '\r', '\n', 0x1A, 'D', 'S', 0, 0, 0};
constexpr char kMagic2[32] = {'M', 'i', 'c', 'r', 'o', 's', 'o', 'f', 't', ' ', 'C', '/', 'C', '+', '+', ' ',
                              'p', 'r', 'o', 'g', 'r', 'a', 'm', ' ', 'd', 'a', 't', 'a', 'b', 'a', 's', 'e'};

std::string quoted(const MappedFile& f) {
    return "'" + f.path() + "'";
}

Diag corrupt_directory(const MappedFile& f, const char* why) {
    return Diag::make(diag::kCorruptStreamDirectory,
                      "The PDB stream directory of " + quoted(f) + " is corrupt (" + why + ").",
                      "Rebuild the module so the linker regenerates the PDB; if it was copied, copy it again.");
}

}  // namespace

Result<std::unique_ptr<Msf>> Msf::open(std::shared_ptr<const MappedFile> file) {
    const MappedFile& f = *file;
    const std::uint8_t* base = f.data();
    const std::uint64_t file_size = f.size();

    if (file_size < kSuperBlockSize) {
        return Diag::make(diag::kNotAPdb, quoted(f) + " is too small (" + std::to_string(file_size) + " bytes) to be a PDB.",
                          "Point Chameleon at the .pdb file the linker produced.");
    }
    if (std::memcmp(base, kMagic7, sizeof kMagic7) != 0) {
        if (std::memcmp(base, kMagic2, sizeof kMagic2) == 0) {
            return Diag::make(diag::kUnsupportedPdbFormat,
                              quoted(f) + " is an old MSF 2.00 PDB (Visual C++ 6 or earlier).",
                              "Only MSF 7.00 PDBs (Visual Studio 2002 and later, clang-cl) are supported.");
        }
        return Diag::make(diag::kNotAPdb, quoted(f) + " does not start with the PDB 7.00 signature.",
                          "Point Chameleon at the .pdb file the linker produced.");
    }

    const std::uint32_t bsize = rd32(base + 32);
    const std::uint32_t free_block_map = rd32(base + 36);
    const std::uint32_t bcount = rd32(base + 40);
    const std::uint32_t dir_bytes = rd32(base + 44);
    const std::uint32_t block_map_addr = rd32(base + 52);

    const bool pow2 = bsize != 0 && (bsize & (bsize - 1)) == 0;
    if (!pow2 || bsize < 512 || bsize > 65536 || bcount == 0 || free_block_map > 2) {
        return Diag::make(diag::kUnsupportedPdbFormat,
                          quoted(f) + " has an invalid MSF super block (block size " + std::to_string(bsize) +
                              ", " + std::to_string(bcount) + " blocks).",
                          "The file is damaged; rebuild the module so the linker regenerates the PDB.");
    }
    const std::uint64_t expected = static_cast<std::uint64_t>(bcount) * bsize;
    if (expected > file_size) {
        return Diag::make(diag::kTruncatedPdb,
                          quoted(f) + " is truncated: the super block describes " + std::to_string(expected) +
                              " bytes but the file has " + std::to_string(file_size) + ".",
                          "Copy the PDB again or rebuild the module; the file was cut short.");
    }
    if (block_map_addr >= bcount) {
        return corrupt_directory(f, "block map address out of range");
    }
    // The directory must fit the file (and is limited to 1 GiB, far above any real PDB).
    if (dir_bytes < 4 || dir_bytes > expected || dir_bytes > (1u << 30)) {
        return corrupt_directory(f, "directory size out of range");
    }

    const std::uint32_t dir_block_count = static_cast<std::uint32_t>((static_cast<std::uint64_t>(dir_bytes) + bsize - 1) / bsize);
    const std::uint64_t map_offset = static_cast<std::uint64_t>(block_map_addr) * bsize;
    if (map_offset + static_cast<std::uint64_t>(dir_block_count) * 4 > expected) {
        return corrupt_directory(f, "block map outside the file");
    }

    // Gather the directory bytes.
    CountedBuffer dir(dir_bytes);
    {
        std::uint32_t remaining = dir_bytes;
        std::uint8_t* out = dir.data();
        for (std::uint32_t i = 0; i < dir_block_count; ++i) {
            const std::uint32_t b = rd32(base + map_offset + static_cast<std::uint64_t>(i) * 4);
            if (b >= bcount) {
                return corrupt_directory(f, "directory block index out of range");
            }
            const std::uint32_t n = std::min(remaining, bsize);
            std::memcpy(out, base + static_cast<std::uint64_t>(b) * bsize, n);
            out += n;
            remaining -= n;
        }
    }

    const std::uint8_t* d = dir.data();
    const std::uint32_t stream_count = rd32(d);
    // 4 + 4*stream_count bytes of sizes must be present.
    if (static_cast<std::uint64_t>(stream_count) * 4 + 4 > dir_bytes) {
        return corrupt_directory(f, "stream count exceeds directory size");
    }

    std::unique_ptr<Msf> msf(new Msf());
    msf->file_ = std::move(file);
    msf->block_size_ = bsize;
    msf->block_count_ = bcount;
    msf->sizes_.resize(stream_count);
    msf->first_block_.resize(static_cast<std::size_t>(stream_count) + 1);

    std::uint64_t total_blocks = 0;
    for (std::uint32_t i = 0; i < stream_count; ++i) {
        const std::uint32_t size = rd32(d + 4 + static_cast<std::size_t>(i) * 4);
        msf->sizes_[i] = size;
        msf->first_block_[i] = total_blocks;
        if (size != kNilSize) {
            total_blocks += (static_cast<std::uint64_t>(size) + bsize - 1) / bsize;
        }
    }
    msf->first_block_[stream_count] = total_blocks;
    const std::uint64_t list_offset = 4 + static_cast<std::uint64_t>(stream_count) * 4;
    if (list_offset + total_blocks * 4 > dir_bytes) {
        return corrupt_directory(f, "block lists exceed directory size");
    }
    msf->blocks_.resize(static_cast<std::size_t>(total_blocks));
    const std::uint8_t* lists = d + list_offset;
    for (std::uint64_t i = 0; i < total_blocks; ++i) {
        const std::uint32_t b = rd32(lists + i * 4);
        if (b >= bcount) {
            return corrupt_directory(f, "stream block index out of range");
        }
        msf->blocks_[static_cast<std::size_t>(i)] = b;
    }
    return msf;
}

std::uint32_t Msf::stream_size(std::uint32_t stream) const noexcept {
    if (stream >= sizes_.size() || sizes_[stream] == kNilSize) {
        return 0;
    }
    return sizes_[stream];
}

bool Msf::stream_exists(std::uint32_t stream) const noexcept {
    return stream < sizes_.size() && sizes_[stream] != kNilSize;
}

const std::uint32_t* Msf::blocks_of(std::uint32_t stream) const noexcept {
    return blocks_.data() + first_block_[stream];
}

StreamData Msf::read_stream(std::uint32_t stream) const {
    const std::uint32_t size = stream_size(stream);
    if (size == 0) {
        return StreamData();
    }
    const std::uint32_t* blocks = blocks_of(stream);
    const std::uint64_t nblocks = (static_cast<std::uint64_t>(size) + block_size_ - 1) / block_size_;
    bool contiguous = true;
    for (std::uint64_t i = 1; i < nblocks; ++i) {
        if (blocks[i] != blocks[i - 1] + 1) {
            contiguous = false;
            break;
        }
    }
    const std::uint8_t* base = file_->data();
    if (contiguous) {
        return StreamData(base + static_cast<std::uint64_t>(blocks[0]) * block_size_, size);
    }
    CountedBuffer buf(size);
    std::uint8_t* out = buf.data();
    std::uint32_t remaining = size;
    for (std::uint64_t i = 0; i < nblocks; ++i) {
        const std::uint32_t n = std::min(remaining, block_size_);
        std::memcpy(out, base + static_cast<std::uint64_t>(blocks[i]) * block_size_, n);
        out += n;
        remaining -= n;
    }
    return StreamData(std::move(buf));
}

const std::uint8_t* Msf::read_range(std::uint32_t stream, std::uint64_t offset, std::size_t length,
                                    CountedBuffer& scratch, std::size_t& got) const {
    got = 0;
    const std::uint32_t size = stream_size(stream);
    if (size == 0 || offset >= size || length == 0) {
        return nullptr;
    }
    const std::uint64_t avail = size - offset;
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(avail, length));
    const std::uint32_t* blocks = blocks_of(stream);
    const std::uint64_t first = offset / block_size_;
    const std::uint64_t last = (offset + n - 1) / block_size_;
    bool contiguous = true;
    for (std::uint64_t i = first + 1; i <= last; ++i) {
        if (blocks[i] != blocks[i - 1] + 1) {
            contiguous = false;
            break;
        }
    }
    const std::uint8_t* base = file_->data();
    const std::uint64_t in_block = offset % block_size_;
    if (contiguous) {
        got = n;
        return base + static_cast<std::uint64_t>(blocks[first]) * block_size_ + in_block;
    }
    if (scratch.size() < n) {
        scratch = CountedBuffer(n);
    }
    std::uint8_t* out = scratch.data();
    std::size_t remaining = n;
    std::uint64_t block = first;
    std::uint64_t pos = in_block;
    while (remaining > 0) {
        const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, block_size_ - pos));
        std::memcpy(out, base + static_cast<std::uint64_t>(blocks[block]) * block_size_ + pos, chunk);
        out += chunk;
        remaining -= chunk;
        ++block;
        pos = 0;
    }
    got = n;
    return scratch.data();
}

}  // namespace cml::pdb::detail
