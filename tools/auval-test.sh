#!/usr/bin/env bash
# Validates a built Audio Unit with Apple's auval. Used by the CTest test
# "auval:AU" (tests/auval/CMakeLists.txt); it can also be run by hand.
#
#   tools/auval-test.sh <path/to/Plugin.component> <type>
#
#   type: AU type code, e.g. aumu (instrument) or aufx (effect)
#   env:  BRUTALIS_AU_MANUFACTURER, BRUTALIS_AU_SUBTYPE  (the 4-char codes)
#
# auval only finds AUs installed in a Components folder. So this script:
#   1. moves any already-installed component of the same name aside;
#   2. copies the built component into ~/Library/Audio/Plug-Ins/Components;
#   3. refreshes the AU registry and runs `auval -strict -v`;
#   4. always removes the test copy, restores the original (if any), and
#      refreshes the registry again, even on failure or Ctrl-C.
#
# The identity codes are private. They are masked in auval's output before it
# reaches logs (whole-word match, so the product name stays readable).
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <path/to/Plugin.component> <type>" >&2
    exit 2
fi
src="$1"
type="$2"
manufacturer="${BRUTALIS_AU_MANUFACTURER:?BRUTALIS_AU_MANUFACTURER is not set}"
subtype="${BRUTALIS_AU_SUBTYPE:?BRUTALIS_AU_SUBTYPE is not set}"

if [[ "$(uname)" != Darwin ]]; then
    echo "auval-test: macOS only" >&2
    exit 2
fi
if [[ ! -d "$src" ]]; then
    echo "auval-test: component not found: $src" >&2
    exit 1
fi

components_dir="$HOME/Library/Audio/Plug-Ins/Components"
name="$(basename "$src")"
dest="$components_dir/$name"
backup=""

refresh_registry() {
    # The registrar caches the component list; killing it forces a rescan.
    # It restarts on demand. `auval -a` triggers the rescan right away.
    killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
    auval -a >/dev/null 2>&1 || true
}

cleanup() {
    rm -rf "$dest"
    if [[ -n "$backup" && -d "$backup" ]]; then
        mv "$backup" "$dest"
        rmdir "$(dirname "$backup")" 2>/dev/null || true
        echo "auval-test: restored the previously installed $name"
    fi
    refresh_registry
}

mkdir -p "$components_dir"
if [[ -e "$dest" ]]; then
    backup_dir="$(mktemp -d "${TMPDIR:-/tmp}/auval-test-backup.XXXXXX")"
    backup="$backup_dir/$name"
    mv "$dest" "$backup"
    echo "auval-test: moved the already-installed $name aside (it is restored afterwards)"
fi
trap cleanup EXIT INT TERM

cp -R "$src" "$dest"
refresh_registry

mask() {
    perl -pe 'BEGIN { ($m, $s) = splice(@ARGV, 0, 2) } s/\b\Q$m\E\b/<manufacturer>/g; s/\b\Q$s\E\b/<subtype>/g' \
        "$manufacturer" "$subtype"
}

set +e
auval -strict -v "$type" "$subtype" "$manufacturer" 2>&1 | mask
status=${PIPESTATUS[0]}
set -e

if [[ $status -ne 0 ]]; then
    echo "auval-test: FAILED (auval exit $status)" >&2
fi
exit "$status"
