#!/bin/bash
# Recursively bundle a macOS .app's non-Qt dynamic-library closure into
# Contents/Frameworks and rewrite every dependency reference to
# @executable_path/../Frameworks/<name>, so the app runs on a Mac without
# Homebrew installed. This complements macdeployqt, which owns Qt.
#
#   Usage: macos_bundle_libs.sh <app-bundle> [extra-search-dir ...]
#
# macdeployqt copies libgdal and a shallow layer of its dependencies into
# Frameworks but does not recurse the full tree (it misses e.g. libgeos, OpenEXR's
# libIex). We finish the job deterministically: for every dependency of every
# non-Qt binary already in the bundle, copy the library in (if absent) and rewrite
# the reference to an explicit @executable_path path — no rpath chains, so nothing
# depends on load-time rpath search. Qt frameworks (*.framework) and OS libraries
# (/usr/lib, /System) are left untouched. Safe to re-run on an already-processed
# bundle (idempotent), which is why it replaces dylibbundler (whose rpath rewiring
# is fragile and breaks on a clean build).
#
# Written for the stock macOS /bin/bash (3.2): indexed arrays + a temp "seen" file
# (handles paths with spaces; associative arrays would need bash 4).

set -uo pipefail

APP="${1:?usage: macos_bundle_libs.sh <app-bundle> [search-dir ...]}"
shift || true
FW="$APP/Contents/Frameworks"
mkdir -p "$FW"

# Directories searched (by basename) for deps referenced via @rpath/@loader_path
# or a bare name; absolute references are used directly. The bundle's own
# Frameworks dir comes first so an already-bundled copy always wins; the caller
# passes Homebrew's lib dir(s) for the rest of the closure.
SEARCH_DIRS=("$FW" "$@")

SEEN="$(mktemp)"
trap 'rm -f "$SEEN"' EXIT

# Print an absolute source path for a dependency reference, or nothing.
resolve() {
  local ref="$1" base d
  base="${ref##*/}"
  [ -f "$FW/$base" ] && { printf '%s\n' "$FW/$base"; return; }
  case "$ref" in /*) [ -f "$ref" ] && { printf '%s\n' "$ref"; return; };; esac
  for d in "${SEARCH_DIRS[@]}"; do
    [ -n "$d" ] && [ -f "$d/$base" ] && { printf '%s\n' "$d/$base"; return; }
  done
}

# Seed the worklist with the flat dylibs macdeployqt placed + the executable(s).
queue=()
for f in "$FW"/*.dylib;           do [ -e "$f" ] && queue+=("$f"); done
for e in "$APP"/Contents/MacOS/*; do [ -f "$e" ] && queue+=("$e"); done

missing=0
i=0
while [ "$i" -lt "${#queue[@]}" ]; do
  lib="${queue[$i]}"; i=$((i + 1))
  grep -Fxq "$lib" "$SEEN" && continue
  printf '%s\n' "$lib" >> "$SEEN"
  base="${lib##*/}"

  chmod u+w "$lib" 2>/dev/null || true
  # Normalise the install id of libraries that live in Frameworks.
  case "$lib" in
    "$FW"/*) install_name_tool -id "@executable_path/../Frameworks/$base" "$lib" 2>/dev/null || true;;
  esac

  # otool -L: line 1 is "<file>:"; for a dylib line 2 is its own id. Skip the
  # header (tail -n +2) and any dep whose basename equals this file's (the id).
  while IFS= read -r dep; do
    [ -z "$dep" ] && continue
    case "$dep" in
      /usr/lib/*|/System/*) continue;;   # OS libraries — never bundle
      *.framework/*)        continue;;   # Qt frameworks — macdeployqt owns them
    esac
    dbase="${dep##*/}"
    [ "$dbase" = "$base" ] && continue
    target="$FW/$dbase"
    if [ ! -f "$target" ]; then
      src="$(resolve "$dep")"
      if [ -z "$src" ]; then
        echo "  ! could not locate $dep (needed by $base)" >&2
        missing=$((missing + 1))
        continue
      fi
      cp -f "$src" "$target"
      chmod u+w "$target"
    fi
    install_name_tool -change "$dep" "@executable_path/../Frameworks/$dbase" "$lib" 2>/dev/null || true
    queue+=("$target")
  done < <(otool -L "$lib" | tail -n +2 | sed -e 's/ (compatibility.*$//' -e 's/^[[:space:]]*//')
done

if [ "$missing" -gt 0 ]; then
  echo "macos_bundle_libs: WARNING — $missing dependency(ies) could not be located (see above)." >&2
fi
echo "macos_bundle_libs: non-Qt closure complete in $FW"
