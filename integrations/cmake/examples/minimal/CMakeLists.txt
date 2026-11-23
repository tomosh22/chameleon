# Minimal Chameleon example: one executable.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=<chameleon install prefix>
#   cmake --build build
#   <prefix>/bin/cml_broker check build/minimal.exe
cmake_minimum_required(VERSION 3.24)
project(ChameleonExampleMinimal LANGUAGES CXX)

find_package(Chameleon CONFIG REQUIRED)

add_executable(minimal main.cpp)
target_compile_features(minimal PRIVATE cxx_std_20)
chameleon_enable(minimal)
