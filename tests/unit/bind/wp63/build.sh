#!/bin/bash
# WP-63 (R-69): rebuilds the inputs of test_guard_code.cpp from src/ (Linux, LLVM 18: clang-cl and lld-link), as
# tests/unit/bind/wp59/build.sh does for WP-59's (whose b4_late, b6_const and b7_both test_guard_code.cpp reads too).
#   <case>.exe, <case>.pdb  the old image: support.cpp + <case>.old.cpp, linked with /DEBUG and no CRT
#   <case>.patch.obj         the patch: <case>.new.cpp
# The n* cases are compiled with /Zc:threadSafeInit- (clang-cl's -fno-threadsafe-statics: one bitmask ??_B per inline
# function), the s* cases with thread-safe statics (an external ?$TSS<n> per static); the *_dtor cases with /EHsc (a static
# with a destructor, and the EH funclets that clear or abort its guard).
# Paths in the debug information are mapped to C:\wp63, so that the files do not depend on where they were built.
# Usage: tests/unit/bind/wp63/build.sh <work dir> [clang-cl] [lld-link]
# The work dir is deleted and made again: it must not be /, this directory, inside it, or a directory that contains it.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd -P)
W=${1:?work directory}
CLANG_CL=${2:-clang-cl}
LLD_LINK=${3:-lld-link}
mkdir -p "$W"
W=$(cd "$W" && pwd -P)
case "$W" in
  /) echo "build.sh: refusing work dir '$W': it is /" >&2; exit 2 ;;
esac
case "$W/" in
  "$HERE/"*) echo "build.sh: refusing work dir '$W': it is $HERE or inside it" >&2; exit 2 ;;
esac
case "$HERE/" in
  "$W/"*) echo "build.sh: refusing work dir '$W': it contains $HERE" >&2; exit 2 ;;
esac
rm -rf "$W"
mkdir -p "$W"
cp "$HERE"/src/*.cpp "$HERE"/src/*.h "$W"/
cd "$W"
CL=("$CLANG_CL" --target=x86_64-pc-windows-msvc /O2 /Ob2 /Z7 /std:c++20 /c /GS- '-fdebug-compilation-dir=C:\wp63' "/clang:-fdebug-prefix-map=$W=C:\\wp63" /clang:-gno-codeview-command-line)
"${CL[@]}" /EHsc support.cpp /Fosupport.obj
for c in s1_const s2_both n3_dtor s3_dtor n4_inl n5_kept n6_addr; do
  FLAGS=()
  case "$c" in n*) FLAGS+=(/Zc:threadSafeInit-) ;; esac
  case "$c" in *_dtor) FLAGS+=(/EHsc) ;; esac
  "${CL[@]}" "${FLAGS[@]}" "$c.old.cpp" "/Fo$c.old.obj"
  "${CL[@]}" "${FLAGS[@]}" "$c.new.cpp" "/Fo$c.new.obj"
  "$LLD_LINK" /DEBUG /NODEFAULTLIB /ENTRY:main /SUBSYSTEM:console '/pdbsourcepath:C:\wp63' "/OUT:$c.exe" "/PDB:$c.pdb" "/PDBALTPATH:$c.pdb" support.obj "$c.old.obj"
  cp "$c.exe" "$c.pdb" "$HERE"/
  cp "$c.new.obj" "$HERE/$c.patch.obj"
done
