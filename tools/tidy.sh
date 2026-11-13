#!/usr/bin/env bash
# Runs clang-tidy over project sources using a build's compile_commands.json.
#
#   tools/tidy.sh [BUILD_DIR]     default BUILD_DIR: build/debug
#
# Only project files (libs/, plugin/, tests/) are analysed; JUCE and Catch2
# sources compiled into our targets are skipped. Each file is analysed once.
# Run it after a build, since some include paths (JuceLibraryCode) are
# generated during the build.
#
# The plugin's shared-code target (plugin/CMakeFiles/brutalis.dir) is skipped:
# JUCE compiles it with generated -DJucePlugin_* command-line macros that
# clang-tidy would check as if they were ours (e.g. a bundle ID containing '-'
# trips bugprone-macro-parentheses). The same plugin sources are analysed
# through brutalis_plugin_tests, which compiles them without those macros.
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

# Write a filtered compilation database: project files only, one entry per
# file, plugin shared-code target excluded (see above).
filtered_db_dir=$(mktemp -d)
trap 'rm -rf "$filtered_db_dir"' EXIT
files=$(python3 -I - "$build_dir/compile_commands.json" "$root" "$filtered_db_dir/compile_commands.json" <<'PY'
import json, os, re, sys
db, root, out = sys.argv[1], sys.argv[2], sys.argv[3]
pattern = re.compile(re.escape(root) + r"/(libs|plugin|tests)/.*\.cpp$")
kept, seen = [], set()
for entry in json.load(open(db)):
    path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
    output = entry.get("output", "").replace(os.sep, "/")
    if not pattern.match(path) or path in seen:
        continue
    if "plugin/CMakeFiles/brutalis.dir/" in output:
        continue
    seen.add(path)
    kept.append(entry)
json.dump(kept, open(out, "w"), indent=1)
print("\n".join(os.path.normpath(os.path.join(e["directory"], e["file"])) for e in kept))
PY
)

[[ -z "$files" ]] && { echo "tidy: no project sources in $build_dir"; exit 0; }

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
echo "tidy: $(echo "$files" | wc -l | tr -d ' ') files with $("$CT" --version | sed -nE 's/.*(version [0-9.]+).*/\1/p' | head -n1)"
echo "$files" | tr '\n' '\0' | xargs -0 -n1 -P "$jobs" "$CT" --quiet -p "$filtered_db_dir" "${extra_args[@]}"
echo "tidy: OK"
