#!/bin/bash
# WP-59 (R-67): rebuilds the inputs of test_bitmask_inline.cpp from src/ (Linux, LLVM 18: clang-cl and lld-link).
#   <case>.exe, <case>.pdb  the old image: support.cpp + <case>.old.cpp, linked with /DEBUG and no CRT
#   <case>.patch.obj         the patch: <case>.new.cpp
# Every file is compiled with /Zc:threadSafeInit- (clang-cl's -fno-threadsafe-statics): the statics of the inline
# function counter() share one external bitmask ??_B?1??counter@@YAHXZ@51.
# Paths in the debug information are mapped to C:\wp59, so that the files do not depend on where they were built.
# Usage: tests/unit/bind/wp59/build.sh <work dir> [clang-cl] [lld-link]
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
CL=("$CLANG_CL" --target=x86_64-pc-windows-msvc /O2 /Ob2 /Z7 /std:c++20 /c /GS- /Zc:threadSafeInit- '-fdebug-compilation-dir=C:\wp59' "/clang:-fdebug-prefix-map=$W=C:\\wp59" /clang:-gno-codeview-command-line)
"${CL[@]}" support.cpp /Fosupport.obj
for c in b1_other b2_front b3_edit b4_late b5_new; do
  "${CL[@]}" "$c.old.cpp" "/Fo$c.old.obj"
  "${CL[@]}" "$c.new.cpp" "/Fo$c.new.obj"
  "$LLD_LINK" /DEBUG /NODEFAULTLIB /ENTRY:main /SUBSYSTEM:console '/pdbsourcepath:C:\wp59' "/OUT:$c.exe" "/PDB:$c.pdb" "/PDBALTPATH:$c.pdb" support.obj "$c.old.obj"
  cp "$c.exe" "$c.pdb" "$HERE"/
  cp "$c.new.obj" "$HERE/$c.patch.obj"
done
