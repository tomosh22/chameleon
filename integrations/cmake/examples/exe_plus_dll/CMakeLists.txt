# Chameleon example: an executable and a DLL it calls. Both go through chameleon_enable(), so both can be reloaded.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=<chameleon install prefix>
#   cmake --build build
#   <prefix>/bin/cml_broker check build/app.exe
#   <prefix>/bin/cml_broker check build/greeter.dll
cmake_minimum_required(VERSION 3.24)
project(ChameleonExampleExePlusDll LANGUAGES CXX)

find_package(Chameleon CONFIG REQUIRED)

add_library(greeter SHARED greeter.cpp)
target_compile_features(greeter PRIVATE cxx_std_20)
target_compile_definitions(greeter PRIVATE GREETER_BUILD)
target_include_directories(greeter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
chameleon_enable(greeter)

add_executable(app main.cpp)
target_compile_features(app PRIVATE cxx_std_20)
target_link_libraries(app PRIVATE greeter)
chameleon_enable(app)
