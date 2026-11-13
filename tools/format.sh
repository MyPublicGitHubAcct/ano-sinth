#!/usr/bin/env bash
# Formats project C++ with clang-format, or checks it with --check.
#
#   tools/format.sh           format every project C++ file in place
#   tools/format.sh --check   report unformatted files and exit 1 (CI, pre-commit)
#   tools/format.sh --check FILE...   check only the given files
#
# The clang-format major version is pinned: different majors format the same
# code differently. Override the binary with CLANG_FORMAT=/path/to/clang-format.
set -euo pipefail

REQUIRED_MAJOR=23
cd "$(git rev-parse --show-toplevel)"

find_clang_format() {
    if [[ -n "${CLANG_FORMAT:-}" ]]; then echo "$CLANG_FORMAT"; return; fi
    for candidate in clang-format "clang-format-${REQUIRED_MAJOR}" /opt/homebrew/opt/llvm/bin/clang-format /usr/local/opt/llvm/bin/clang-format; do
        if command -v "$candidate" >/dev/null 2>&1; then
            local major
            major=$("$candidate" --version | sed -E 's/.*version ([0-9]+).*/\1/')
            if [[ "$major" == "$REQUIRED_MAJOR" ]]; then echo "$candidate"; return; fi
        fi
    done
    echo "error: clang-format ${REQUIRED_MAJOR}.x not found. Install it (brew install llvm, or" >&2
    echo "       pip install clang-format==${REQUIRED_MAJOR}.*) or set CLANG_FORMAT." >&2
    exit 2
}

CF=$(find_clang_format)

mode=fix
if [[ "${1:-}" == "--check" ]]; then mode=check; shift; fi

if [[ $# -gt 0 ]]; then
    files=("$@")
else
    files=()
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files --cached --others --exclude-standard -- \
            'libs/*.cpp' 'libs/*.hpp' 'libs/*.h' \
            'plugin/*.cpp' 'plugin/*.hpp' 'plugin/*.h' \
            'tests/*.cpp' 'tests/*.hpp' 'tests/*.h')
fi

[[ ${#files[@]} -eq 0 ]] && exit 0

if [[ "$mode" == check ]]; then
    "$CF" --dry-run --Werror "${files[@]}"
else
    "$CF" -i "${files[@]}"
fi
