#pragma once

// Umbrella header for cml_pdb.
//
//   cml::pdb::PdbIndex::open_pdb(path)        index a PDB by path
//   cml::pdb::PdbIndex::open_for_pe(pe_path)  find and validate the PDB of an image, then index it
//
// See docs/wp/WP-04.md for the data model, the lazy-loading rules and the diagnostic ids.

#include <cstddef>

#include "cml/pdb/diag_ids.h"
#include "cml/pdb/index.h"
#include "cml/pdb/types.h"

namespace cml::pdb {

const char* library_name() noexcept;

// True when [data, data + size) holds a structurally valid MSF/PDB super block
// (wraps raw_pdb's ValidateFile). Does not parse any stream.
bool is_valid_pdb_image(const void* data, std::size_t size) noexcept;

}  // namespace cml::pdb
