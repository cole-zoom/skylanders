#!/usr/bin/env bash
# Apply dolphin-web/patches/ to a Dolphin checkout.
#
# Most patches apply at the Dolphin root, but not all: 0003 patches SFML,
# which is a SUBMODULE, so its paths are relative to the submodule root rather
# than Dolphin's. Applying everything at the top level fails on that one with
# "No such file or directory", which is how CI first found out that the
# patches had only ever been applied by hand.
#
# The mapping lives here rather than in the workflow so that CI and a human
# setting up a checkout do the same thing.
#
# Usage:  scripts/apply-patches.sh [dolphin-dir]
set -euo pipefail
cd "$(dirname "$0")/.."
DOLPHIN="${1:-dolphin}"
PATCHES="$(pwd)/dolphin-web/patches"

[ -d "$DOLPHIN" ] || { echo "no Dolphin checkout at $DOLPHIN" >&2; exit 1; }

# Where each patch applies, relative to the Dolphin checkout.
patch_root() {
  case "$1" in
    0003-sfml-emscripten-platform.patch) echo "Externals/SFML/SFML" ;;
    *)                                   echo "." ;;
  esac
}

applied=0 skipped=0
for p in "$PATCHES"/*.patch; do
  base="$(basename "$p")"
  root="$(patch_root "$base")"
  dir="$DOLPHIN/$root"

  if [ ! -d "$dir" ]; then
    echo "FAILED  $base — $dir does not exist (submodules not fetched?)" >&2
    exit 1
  fi

  if git -C "$dir" apply --check "$p" 2>/dev/null; then
    git -C "$dir" apply "$p"
    echo "applied $base  (in $root)"
    applied=$((applied + 1))
  elif git -C "$dir" apply --reverse --check "$p" 2>/dev/null; then
    # Already applied. Re-running setup is routine, and failing on it would
    # make this script usable exactly once per checkout.
    echo "skipped $base  (already applied)"
    skipped=$((skipped + 1))
  else
    echo "FAILED  $base  (in $root) — does not apply and is not already applied" >&2
    exit 1
  fi
done

echo "$applied applied, $skipped already present"
