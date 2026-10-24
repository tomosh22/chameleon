#include "cml/pdb/pdb.h"

#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"

TEST_CASE("cml_pdb links and reports its name") {
    CHECK(std::string(cml::pdb::library_name()) == "cml_pdb");
}

TEST_CASE("raw_pdb rejects things that are not PDBs") {
    CHECK_FALSE(cml::pdb::is_valid_pdb_image(nullptr, 0));

    const std::vector<std::uint8_t> tiny(16, 0);
    CHECK_FALSE(cml::pdb::is_valid_pdb_image(tiny.data(), tiny.size()));

    const std::vector<std::uint8_t> zeros(4096, 0);
    CHECK_FALSE(cml::pdb::is_valid_pdb_image(zeros.data(), zeros.size()));
}
