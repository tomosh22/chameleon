# Fixture `bigobj`: an object that needs the /bigobj COFF format (32-bit section numbers).
#
#   main.cpp            TICK loop (100 ms), prints `TICK n=<k> value=<bigobj_sum(k)>`.
#   bigobj_funcs.cpp    generated at generate time into the build tree: N tiny extern "C"
#                       functions, each in its own COMDAT section (/Gy is on for MSVC-style
#                       compilers, see cml_add_fixture).
#
# cl: /bigobj makes the compiler always emit the ANON_OBJECT_HEADER_BIGOBJ format, so a small N
# is enough.
# clang-cl: accepts /bigobj but ignores it (verified with clang 18); LLVM switches to the bigobj
# format only when an object has more than 65279 sections (below that it writes a regular object
# whose 16-bit section numbers run up to 65279). Every function costs it 2 sections in
# RelWithDebInfo and 4 in Debug (.text, .xdata/.pdata, .debug$S). N = 66000 exceeds 65279 even at
# ONE section per function, so the object is bigobj in every configuration (Release, MinSizeRel too).
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CML_BIGOBJ_FUNCTION_COUNT 66000)
else()
    set(CML_BIGOBJ_FUNCTION_COUNT 200)
endif()

set(_cml_bigobj_src "extern int g_bigobj_sink;\n")
foreach(_i RANGE 1 ${CML_BIGOBJ_FUNCTION_COUNT})
    string(APPEND _cml_bigobj_src "extern \"C\" int bigobj_fn_${_i}(int x) { return x + ${_i} + g_bigobj_sink; }\n")
endforeach()
string(APPEND _cml_bigobj_src "extern \"C\" int bigobj_sum(int x) {\n    int total = x;\n")
foreach(_i RANGE 1 ${CML_BIGOBJ_FUNCTION_COUNT} 50)
    string(APPEND _cml_bigobj_src "    total += bigobj_fn_${_i}(x);\n")
endforeach()
string(APPEND _cml_bigobj_src "    return total;\n}\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bigobj_funcs.cpp" CONTENT "${_cml_bigobj_src}")

cml_add_fixture(bigobj SOURCES main.cpp "${CMAKE_CURRENT_BINARY_DIR}/bigobj_funcs.cpp")
if(MSVC)
    target_compile_options(fixture_bigobj_objs PRIVATE /bigobj)
endif()

add_test(NAME fixture.bigobj.runs COMMAND fixture_bigobj --ticks 3)
set_tests_properties(fixture.bigobj.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=[0-9]+"
    TIMEOUT 30)
