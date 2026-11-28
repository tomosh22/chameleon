#!/bin/bash
# WP-56 (R-66): rebuilds the inputs of test_nested_inlined.cpp from src/ (Linux, LLVM 18: clang-cl and lld-link), as
# tests/unit/bind/wp53/build.sh does for WP-53's.
#   <case>.exe, <case>.pdb  the old image: support.cpp + <case>.old.cpp, linked with /DEBUG and no CRT
#   <case>.patch.obj         the patch: <case>.new.cpp
# Paths in the debug information are mapped to C:\wp56, so that the files do not depend on where they were built.
# Usage: tests/unit/bind/wp56/build.sh <work dir> [clang-cl] [lld-link]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:?work directory}
CLANG_CL=${2:-clang-cl}
LLD_LINK=${3:-lld-link}
# The work directory is deleted first: refuse one that is the current directory, '/', this directory, inside it or above it.
A=$(realpath -m -- "$W")
H=$(realpath -- "$HERE")
C=$(realpath -- .)
if [ "$A" = "/" ] || [ "$A" = "$C" ] || [ "$A" = "$H" ] || [[ "$A" == "$H"/* ]] || [[ "$H" == "$A"/* ]]; then
  echo "build.sh: refusing work directory '$W' ($A): it is '.', '/', $H, inside it or containing it" >&2
  exit 2
fi
rm -rf "$W"
mkdir -p "$W"
cp "$HERE"/src/*.cpp "$HERE"/src/*.h "$W"/
W=$(cd "$W" && pwd)
cd "$W"
CL=("$CLANG_CL" --target=x86_64-pc-windows-msvc /O2 /Ob2 /Z7 /std:c++20 /c /GS- '-fdebug-compilation-dir=C:\wp56' "/clang:-fdebug-prefix-map=$W=C:\\wp56" /clang:-gno-codeview-command-line)
"${CL[@]}" /DSPIKE_NOCRT support.cpp /Fosupport.obj
for c in n1_r66_other n2_r66_edited n3_r61_deep; do
  "${CL[@]}" "$c.old.cpp" "/Fo$c.old.obj"
  "${CL[@]}" "$c.new.cpp" "/Fo$c.new.obj"
  "$LLD_LINK" /DEBUG /NODEFAULTLIB /ENTRY:main /SUBSYSTEM:console '/pdbsourcepath:C:\wp56' "/OUT:$c.exe" "/PDB:$c.pdb" "/PDBALTPATH:$c.pdb" support.obj "$c.old.obj"
  cp "$c.exe" "$c.pdb" "$HERE"/
  cp "$c.new.obj" "$HERE/$c.patch.obj"
done
