#!/usr/bin/env bash
# Runs clang-tidy over project sources using a build's compile_commands.json.
#
#   tools/tidy.sh [BUILD_DIR]     default BUILD_DIR: build/debug
#
# Only project files (libs/, plugin/, tests/) are analysed; JUCE and Catch2
# sources compiled into our targets are skipped. Run it after a build, since
# some include paths (JuceLibraryCode) are generated during the build.
#
# The clang-tidy major version is pinned like clang-format's. Override the
# binary with CLANG_TIDY=/path/to/clang-tidy.
set -euo pipefail

REQUIRED_MAJOR=23
cd "$(git rev-parse --show-toplevel)"
root="$(pwd)"
build_dir="${1:-build/debug}"

if [[ ! -f "$build_dir/compile_commands.json" ]]; then
    echo "error: $build_dir/compile_commands.json not found. Configure and build first (cmake --workflow --preset debug)." >&2
    exit 2
fi

find_clang_tidy() {
    if [[ -n "${CLANG_TIDY:-}" ]]; then echo "$CLANG_TIDY"; return; fi
    for candidate in /opt/homebrew/opt/llvm/bin/clang-tidy /usr/local/opt/llvm/bin/clang-tidy clang-tidy "clang-tidy-${REQUIRED_MAJOR}"; do
        if command -v "$candidate" >/dev/null 2>&1; then
            local major
            major=$("$candidate" --version | sed -nE 's/.*version ([0-9]+).*/\1/p' | head -n1)
            if [[ "$major" == "$REQUIRED_MAJOR" ]]; then echo "$candidate"; return; fi
        fi
    done
    echo "error: clang-tidy ${REQUIRED_MAJOR}.x not found. Install it (brew install llvm, or" >&2
    echo "       pip install clang-tidy==${REQUIRED_MAJOR}.*) or set CLANG_TIDY." >&2
    exit 2
}
CT=$(find_clang_tidy)

extra_args=()
if [[ "$(uname)" == Darwin ]]; then
    # Apple's compiler finds the SDK by itself, so compile_commands.json has no
    # -isysroot. A non-Apple clang-tidy needs it spelled out.
    extra_args+=("--extra-arg=-isysroot$(xcrun --show-sdk-path)")
fi

files=$(python3 -I - "$build_dir/compile_commands.json" "$root" <<'PY'
import json, os, re, sys
db, root = sys.argv[1], sys.argv[2]
pattern = re.compile(re.escape(root) + r"/(libs|plugin|tests)/.*\.cpp$")
seen = []
for entry in json.load(open(db)):
    path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
    if pattern.match(path) and path not in seen:
        seen.append(path)
print("\n".join(seen))
PY
)

[[ -z "$files" ]] && { echo "tidy: no project sources in $build_dir"; exit 0; }

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
echo "tidy: $(echo "$files" | wc -l | tr -d ' ') files with $("$CT" --version | sed -nE 's/.*(version [0-9.]+).*/\1/p' | head -n1)"
echo "$files" | tr '\n' '\0' | xargs -0 -n1 -P "$jobs" "$CT" --quiet -p "$build_dir" "${extra_args[@]}"
echo "tidy: OK"
