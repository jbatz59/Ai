#!/usr/bin/env bash
# Fast type-check of individual source files with the MinGW cross compiler (no linking).
#   tools/check.sh src/mem/pattern.cpp [more.cpp ...]
# Exit code != 0 on any error. Warnings are printed.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${CXX:-x86_64-w64-mingw32-g++-posix}"
GEN="$ROOT/build/generated"
mkdir -p "$GEN"
if [ ! -f "$GEN/cg_version.h" ]; then
  sed -e 's/@CG_VERSION_STRING@/1.0.0/' -e 's/@PROJECT_VERSION_MAJOR@/1/' -e 's/@PROJECT_VERSION_MINOR@/0/' \
      -e 's/@PROJECT_VERSION_PATCH@/0/' "$ROOT/src/version.h.in" > "$GEN/cg_version.h"
fi
FLAGS=(-std=c++20 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type
  -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -DJSON_USE_IMPLICIT_CONVERSIONS=1
  -I"$ROOT/src" -I"$GEN" -I"$ROOT/third_party/imgui" -I"$ROOT/third_party/imgui/backends"
  -I"$ROOT/third_party/minhook/include" -I"$ROOT/third_party/minhook/src/hde" -I"$ROOT/third_party/lua"
  -I"$ROOT/third_party/nlohmann/include")
rc=0
for f in "$@"; do
  case "$f" in
    *.c) x86_64-w64-mingw32-gcc-posix -std=c11 -fsyntax-only -Wall "${FLAGS[@]:7}" "$f" || rc=1 ;;
    *)   "$CXX" "${FLAGS[@]}" "$f" || rc=1 ;;
  esac
done
[ $rc -eq 0 ] && echo "check.sh: OK ($# file(s))"
exit $rc
