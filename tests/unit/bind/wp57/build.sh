#!/bin/bash
# WP-57 (R-59): rebuilds the inputs of test_inlined_owners.cpp from src/ (Linux, LLVM 18: clang-cl and lld-link, with the
# libstdc++ 13 headers for std::sort: header only, /std:c++17 because its C++20 headers need __int128 under clang-cl).
#   <case>.exe, <case>.pdb  the old image: support.cpp + <case>.old.cpp, linked with /DEBUG and no CRT
#   <case>.patch.obj         the patch: <case>.new.cpp
# Paths in the debug information are mapped to C:\wp57, so that the files do not depend on where they were built.
# Usage: tests/unit/bind/wp57/build.sh <work dir> [clang-cl] [lld-link]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:?work directory}
CLANG_CL=${2:-clang-cl}
LLD_LINK=${3:-lld-link}
rm -rf "$W"
mkdir -p "$W"
cp "$HERE"/src/*.cpp "$HERE"/src/*.h "$W"/
W=$(cd "$W" && pwd)
cd "$W"
INC=(-imsvc /usr/include/c++/13 -imsvc /usr/include/x86_64-linux-gnu/c++/13 -imsvc /usr/include -imsvc /usr/include/x86_64-linux-gnu -Wno-#warnings)
CL=("$CLANG_CL" --target=x86_64-pc-windows-msvc /O2 /Ob2 /Z7 /std:c++17 /c /GS- "${INC[@]}" '-fdebug-compilation-dir=C:\wp57' "/clang:-fdebug-prefix-map=$W=C:\\wp57" /clang:-gno-codeview-command-line)
"${CL[@]}" support.cpp /Fosupport.obj
for c in s1_sort_other s3_once_other s7_r54_samecode s8_both_other s10_r54_keep_sig s11_benign_sig s12_r54_layout s13_callee_changed s14_callee_size s15_callee_same; do
  "${CL[@]}" "$c.old.cpp" "/Fo$c.old.obj"
  "${CL[@]}" "$c.new.cpp" "/Fo$c.new.obj"
  "$LLD_LINK" /DEBUG /NODEFAULTLIB /ENTRY:main /SUBSYSTEM:console '/pdbsourcepath:C:\wp57' "/OUT:$c.exe" "/PDB:$c.pdb" "/PDBALTPATH:$c.pdb" support.obj "$c.old.obj"
  cp "$c.exe" "$c.pdb" "$HERE"/
  cp "$c.new.obj" "$HERE/$c.patch.obj"
done
