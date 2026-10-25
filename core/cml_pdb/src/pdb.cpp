#include "cml/pdb/pdb.h"

#include "PDB.h"

namespace cml::pdb {

const char* library_name() noexcept {
    return "cml_pdb";
}

bool is_valid_pdb_image(const void* data, std::size_t size) noexcept {
    if (data == nullptr) {
        return false;
    }
    return PDB::ValidateFile(data, size) == PDB::ErrorCode::Success;
}

}  // namespace cml::pdb
