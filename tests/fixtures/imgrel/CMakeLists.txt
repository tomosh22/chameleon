# Fixture `imgrel` (WP-36, ADR-0042): a C translation unit whose functions address globals image-relatively under cl
# with optimisation: a switch with a jump table that reads and writes global arrays, and a function indexing two global
# arrays (`[reg + index*scale + sym@IMGREL]`, IMAGE_REL_AMD64_ADDR32NB on a displacement; corpus finding F-2). The edit
# changes both functions; the patched code must read and write the storage of the running image. In Debug (/Od) and under
# clang-cl there are no such relocations and the fixture passes through the ordinary REL32 path.
# Needs the C language (enabled on Windows by tests/CMakeLists.txt), like c_only.
if(CMAKE_C_COMPILER_LOADED)
    cml_add_fixture(imgrel C SOURCES main.c imgrel.c)

    add_test(NAME fixture.imgrel.runs COMMAND fixture_imgrel --ticks 12)
    set_tests_properties(fixture.imgrel.runs PROPERTIES
        PASS_REGULAR_EXPRESSION "TICK n=11 key=1 count=12 seed=4252 fp=43 fl=7 g3=4244 wide=4328"
        TIMEOUT 30)
endif()
