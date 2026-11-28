#!/bin/bash
# WP-57 spike (removed with spike/wp57/ once the WP is built): writes the cases of gen.py into <work dir> and builds, per case,
# the old image (support.cpp + <case>.old.cpp, lld-link /DEBUG, no CRT) and the patch object (<case>.new.cpp).
# clang 18 clang-cl /O2 /Ob2 /Z7; std::sort is libstdc++ 13's (header only, /std:c++17: its C++20 headers need __int128).
# Usage: spike/wp57/build.sh <work dir>; prints the case names.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:?work directory}
rm -rf "$W"; mkdir -p "$W"
W=$(cd "$W" && pwd)
cp "$HERE"/src/* "$W"/
CASES=$(python3 -I "$HERE/gen.py" "$W")
cd "$W"
INC=(-imsvc /usr/include/c++/13 -imsvc /usr/include/x86_64-linux-gnu/c++/13 -imsvc /usr/include -imsvc /usr/include/x86_64-linux-gnu -Wno-#warnings)
CL=(clang-cl-18 --target=x86_64-pc-windows-msvc /O2 /Ob2 /Z7 /std:c++17 /c /GS- "${INC[@]}" '-fdebug-compilation-dir=C:\wp57' "/clang:-fdebug-prefix-map=$W=C:\\wp57" /clang:-gno-codeview-command-line)
"${CL[@]}" support.cpp /Fosupport.obj
for c in $CASES; do
  "${CL[@]}" "$c.old.cpp" "/Fo$c.old.obj"
  "${CL[@]}" "$c.new.cpp" "/Fo$c.patch.obj"
  lld-link /DEBUG /NODEFAULTLIB /ENTRY:main /SUBSYSTEM:console '/pdbsourcepath:C:\wp57' "/OUT:$c.exe" "/PDB:$c.pdb" "/PDBALTPATH:$c.pdb" support.obj "$c.old.obj"
done
echo "$CASES"
