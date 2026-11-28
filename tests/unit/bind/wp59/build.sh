#!/bin/bash
# WP-59 (R-67): rebuilds the inputs of test_bitmask_inline.cpp from src/ (Linux, LLVM 18: clang-cl and lld-link).
#   <case>.exe, <case>.pdb  the old image: support.cpp + <case>.old.cpp, linked with /DEBUG and no CRT
#   <case>.patch.obj         the patch: <case>.new.cpp
# The b* cases are compiled with /Zc:threadSafeInit- (clang-cl's -fno-threadsafe-statics): the statics of the inline
# function counter() share one external bitmask ??_B?1??counter@@YAHXZ@51. The t* cases use thread-safe statics (the
# default): each static of counter() has an external guard ?$TSS<n>@?1??counter@@YAHXZ@4HA.
# Paths in the debug information are mapped to C:\wp59, so that the files do not depend on where they were built.
# Usage: tests/unit/bind/wp59/build.sh <work dir> [clang-cl] [lld-link]
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
CL=("$CLANG_CL" --target=x86_64-pc-windows-msvc /O2 /Ob2 /Z7 /std:c++20 /c /GS- '-fdebug-compilation-dir=C:\wp59' "/clang:-fdebug-prefix-map=$W=C:\\wp59" /clang:-gno-codeview-command-line)
"${CL[@]}" support.cpp /Fosupport.obj
for c in b1_other b2_front b3_edit b4_late b5_new b6_const b7_both b8_tls t1_front t2_single t3_other t4_edit t5_remove; do
  FLAGS=()
  case "$c" in b*) FLAGS=(/Zc:threadSafeInit-) ;; esac
  "${CL[@]}" "${FLAGS[@]}" "$c.old.cpp" "/Fo$c.old.obj"
  "${CL[@]}" "${FLAGS[@]}" "$c.new.cpp" "/Fo$c.new.obj"
  "$LLD_LINK" /DEBUG /NODEFAULTLIB /ENTRY:main /SUBSYSTEM:console '/pdbsourcepath:C:\wp59' "/OUT:$c.exe" "/PDB:$c.pdb" "/PDBALTPATH:$c.pdb" support.obj "$c.old.obj"
  cp "$c.exe" "$c.pdb" "$HERE"/
  cp "$c.new.obj" "$HERE/$c.patch.obj"
done
