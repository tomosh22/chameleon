#!/bin/bash
# WP-60 (R-57): rebuilds the inputs of test_inlined_helpers.cpp from src/ (Linux, LLVM 18: clang-cl and lld-link).
#   <case>.exe, <case>.pdb  the old image: support.cpp + <case>.old.cpp, linked with /DEBUG and no CRT
#   <case>.patch.obj         the patch: <case>.new.cpp
# Paths in the debug information are mapped to C:\wp60, so that the files do not depend on where they were built.
# Usage: tests/unit/bind/wp60/build.sh <work dir> [clang-cl] [lld-link]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:?work directory}
CLANG_CL=${2:-clang-cl}
LLD_LINK=${3:-lld-link}
# The work directory is deleted first: never this directory, one inside it, one that holds it, `.` or `/` (as wp57/build.sh).
W_ABS=$(realpath -m -- "$W")
case "$W" in
  . | ./ | / ) echo "build.sh: refusing work directory '$W'" >&2; exit 2 ;;
esac
if [ "$W_ABS" = "/" ] || [ "$W_ABS" = "$(pwd -P)" ] || [ "$W_ABS" = "$HERE" ] || [ "${W_ABS#"$HERE"/}" != "$W_ABS" ] || [ "${HERE#"$W_ABS"/}" != "$HERE" ]; then
  echo "build.sh: refusing work directory '$W' ($W_ABS): it is /, the current directory, $HERE, inside it, or holds it" >&2
  exit 2
fi
rm -rf "$W"
mkdir -p "$W"
cp "$HERE"/src/*.cpp "$HERE"/src/*.h "$W"/
W=$(cd "$W" && pwd)
cd "$W"
CL=("$CLANG_CL" --target=x86_64-pc-windows-msvc /O2 /Ob2 /Z7 /std:c++17 /c /GS- '-fdebug-compilation-dir=C:\wp60' "/clang:-fdebug-prefix-map=$W=C:\\wp60" /clang:-gno-codeview-command-line)
"${CL[@]}" support.cpp /Fosupport.obj
for c in h1_r57 h2_r57_samecode h3_helper_other h4_helper_edit h5_outofline h6_outofline_x h7_internal_class h8_external_member h10_external_free h11_member_template h12_header_changed h13_external_global h14_two_owners; do
  "${CL[@]}" "$c.old.cpp" "/Fo$c.old.obj"
  "${CL[@]}" "$c.new.cpp" "/Fo$c.new.obj"
  "$LLD_LINK" /DEBUG /NODEFAULTLIB /ENTRY:main /SUBSYSTEM:console '/pdbsourcepath:C:\wp60' "/OUT:$c.exe" "/PDB:$c.pdb" "/PDBALTPATH:$c.pdb" support.obj "$c.old.obj"
  cp "$c.exe" "$c.pdb" "$HERE"/
  cp "$c.new.obj" "$HERE/$c.patch.obj"
done
