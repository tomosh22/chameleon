# WP-32 smoke application: the smallest program that uses Chameleon the way docs/QUICKSTART.md describes. Built by
# tools/package/smoke.ps1 against an UNZIPPED package (never against the build tree):
#
#   cmake -S smoke -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=<unzipped package>
#
# With SMOKE_USE_PACKAGE=ON (default) the flags and the agent header come from find_package(Chameleon) and
# chameleon_enable(). SMOKE_USE_PACKAGE=OFF applies the flags of docs/BUILD_REQUIREMENTS.md by hand and takes the header
# from <CHAMELEON_ROOT>/include; it exists for a package without the CMake integration (and shows what the helper does).
cmake_minimum_required(VERSION 3.24)
project(ChameleonSmoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SMOKE_USE_PACKAGE "Use find_package(Chameleon) and chameleon_enable()" ON)
set(CHAMELEON_ROOT "" CACHE PATH "Unzipped package (only for SMOKE_USE_PACKAGE=OFF)")

add_executable(smoke_app main.cpp logic.cpp)

if(SMOKE_USE_PACKAGE)
    find_package(Chameleon REQUIRED)
    chameleon_enable(smoke_app)
else()
    if(NOT CHAMELEON_ROOT)
        message(FATAL_ERROR "set CHAMELEON_ROOT to the unzipped package when SMOKE_USE_PACKAGE=OFF")
    endif()
    target_include_directories(smoke_app PRIVATE "${CHAMELEON_ROOT}/include")
    if(MSVC)
        target_compile_options(smoke_app PRIVATE /Gy /hotpatch /Ob0 /Zi)
        target_link_options(smoke_app PRIVATE /DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN)
    endif()
endif()
