#!/bin/bash
# WP-53 (R-61): rebuilds the inputs of test_inlined_mirror.cpp from src/ (Linux, LLVM 18: clang-cl and lld-link).
#   <case>.exe, <case>.pdb  the old image: support.cpp + <case>.old.cpp, linked with /DEBUG and no CRT
#   <case>.patch.obj         the patch: <case>.new.cpp
# Paths in the debug information are mapped to C:\wp53, so that the files do not depend on where they were built.
# Usage: tests/unit/bind/wp53/build.sh <work dir> [clang-cl] [lld-link]
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
CL=("$CLANG_CL" --target=x86_64-pc-windows-msvc /O2 /Ob2 /Z7 /std:c++20 /c /GS- '-fdebug-compilation-dir=C:\wp53' "/clang:-fdebug-prefix-map=$W=C:\\wp53" /clang:-gno-codeview-command-line)
"${CL[@]}" /DSPIKE_NOCRT support.cpp /Fosupport.obj
for c in c1_wp40_once c3_r61 c4_r61_newlambda c5_r61_other c7_r61_const c8_wp40_three c10_r61_block c11_wp40_other c12_r61_samecode; do
  "${CL[@]}" "$c.old.cpp" "/Fo$c.old.obj"
  "${CL[@]}" "$c.new.cpp" "/Fo$c.new.obj"
  "$LLD_LINK" /DEBUG /NODEFAULTLIB /ENTRY:main /SUBSYSTEM:console '/pdbsourcepath:C:\wp53' "/OUT:$c.exe" "/PDB:$c.pdb" "/PDBALTPATH:$c.pdb" support.obj "$c.old.obj"
  cp "$c.exe" "$c.pdb" "$HERE"/
  cp "$c.new.obj" "$HERE/$c.patch.obj"
done
