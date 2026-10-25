# unit.pe: cml_pe reader tests.
#   test_pe_synthetic.cpp   hand-built PE32+ image (on-disk and in-memory layouts), malformed input
#   test_pe_fixture.cpp     fixture_basic.exe / fixture_bigobj.exe as linked by the cell's toolchain,
#                           plus (Windows) modules mapped into the test process itself
# cml_define_fixture_paths() comes from tests/unit/coff/CMakeLists.txt (added first).

cml_add_unit_test(pe
    SOURCES
        test_pe_synthetic.cpp
        test_pe_fixture.cpp
    LIBS cml_pe)
cml_define_fixture_paths(cml_test_pe)
