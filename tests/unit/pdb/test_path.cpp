#include <string>

#include "cml/pdb/pdb.h"
#include <ostream>  // doctest streams string_view operands; MSVC needs the full definition

#include "doctest.h"

using cml::pdb::is_absolute_path;
using cml::pdb::normalize_path;

TEST_CASE("normalize_path: separators, case, dots") {
    CHECK(normalize_path("C:\\Proj\\Src\\Logic.CPP") == "c:\\proj\\src\\logic.cpp");
    CHECK(normalize_path("c:/proj/src/logic.cpp") == "c:\\proj\\src\\logic.cpp");
    CHECK(normalize_path("C:\\proj\\src\\..\\src\\.\\logic.h") == "c:\\proj\\src\\logic.h");
    CHECK(normalize_path("C:\\proj\\\\src\\\\logic.h") == "c:\\proj\\src\\logic.h");
    CHECK(normalize_path("C:\\proj\\src\\") == "c:\\proj\\src");
    CHECK(normalize_path("C:\\") == "c:\\");
    CHECK(normalize_path("C:\\..\\..\\x") == "c:\\x");  // cannot climb above the root
    CHECK(normalize_path("\\\\?\\C:\\Foo\\Bar") == "c:\\foo\\bar");
    CHECK(normalize_path("\\\\?\\UNC\\Server\\Share\\F.h") == "\\\\server\\share\\f.h");
    CHECK(normalize_path("\\\\Server\\Share\\a\\..\\F.h") == "\\\\server\\share\\f.h");
    CHECK(normalize_path("") == "");
}

TEST_CASE("normalize_path: relative paths and cwd") {
    CHECK(normalize_path("..\\include\\rel.h") == "..\\include\\rel.h");  // no cwd: stays relative
    CHECK(normalize_path("a\\..\\..\\b") == "..\\b");
    CHECK(normalize_path("..\\include\\rel.h", "D:\\work\\sub") == "d:\\work\\include\\rel.h");
    CHECK(normalize_path("Rel.cpp", "D:\\Work\\Sub\\") == "d:\\work\\sub\\rel.cpp");
    CHECK(normalize_path("./x/../y.h", "c:/w") == "c:\\w\\y.h");
    // An absolute path ignores the cwd.
    CHECK(normalize_path("E:\\abs\\f.h", "D:\\work") == "e:\\abs\\f.h");
    // A rooted path keeps the cwd's drive.
    CHECK(normalize_path("\\abs\\f.h", "D:\\work") == "d:\\abs\\f.h");
    // A relative cwd is not usable.
    CHECK(normalize_path("f.h", "rel\\dir") == "f.h");
}

TEST_CASE("is_absolute_path") {
    CHECK(is_absolute_path("C:\\x"));
    CHECK(is_absolute_path("c:/x"));
    CHECK(is_absolute_path("\\\\server\\share"));
    CHECK_FALSE(is_absolute_path("x\\y"));
    CHECK_FALSE(is_absolute_path("..\\x"));
    CHECK_FALSE(is_absolute_path("C:x"));
    CHECK_FALSE(is_absolute_path(""));
}

TEST_CASE("PdbId text form and ordering") {
    cml::pdb::PdbId a;
    a.guid = {0x3C, 0x83, 0xC3, 0x8C, 0x00, 0x87, 0x0D, 0xEB, 0x4C, 0x4C, 0x44, 0x20, 0x50, 0x44, 0x42, 0x2E};
    a.age = 7;
    CHECK(a.to_string() == "8CC3833C-8700-EB0D-4C4C-44205044422E/7");
    cml::pdb::PdbId b = a;
    CHECK(a == b);
    b.age = 8;
    CHECK(a != b);
    CHECK(a < b);
    CHECK_FALSE(a.is_zero());
    CHECK(cml::pdb::PdbId().is_zero());
}

TEST_CASE("optimization_hint_from_command: last /O option wins") {
    using cml::pdb::OptimizationHint;
    using cml::pdb::optimization_hint_from_command;
    CHECK(optimization_hint_from_command("/c /Zi /Od foo.cpp") == OptimizationHint::Disabled);
    CHECK(optimization_hint_from_command("/c /O2 /Ob1 foo.cpp") == OptimizationHint::Speed);
    CHECK(optimization_hint_from_command("/Od /O2") == OptimizationHint::Speed);
    CHECK(optimization_hint_from_command("/O1") == OptimizationHint::Size);
    CHECK(optimization_hint_from_command("/Ox") == OptimizationHint::Full);
    CHECK(optimization_hint_from_command("\"-cc1\" \"-O0\" \"-x\" \"c++\"") == OptimizationHint::Disabled);
    CHECK(optimization_hint_from_command("/c /Zi foo.cpp") == OptimizationHint::Unknown);
    CHECK(optimization_hint_from_command("/Ob0 /Oy-") == OptimizationHint::Unknown);
    CHECK(optimization_hint_from_command("") == OptimizationHint::Unknown);
}
