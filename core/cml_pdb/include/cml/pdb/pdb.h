#pragma once

#include <cstddef>

namespace cml::pdb {

const char* library_name() noexcept;

// True when [data, data + size) holds a structurally valid MSF/PDB super block
// (wraps raw_pdb's ValidateFile). Does not parse any stream.
bool is_valid_pdb_image(const void* data, std::size_t size) noexcept;

}  // namespace cml::pdb
