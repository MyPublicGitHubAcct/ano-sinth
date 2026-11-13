#!/usr/bin/env bash
# Fails if any private identity value appears in tracked files or in commit
# messages. The values themselves never live in the repo; they come from:
#
#   1. environment variables BRUTALIS_MANUFACTURER_CODE, BRUTALIS_PLUGIN_CODE,
#      BRUTALIS_BUNDLE_ID (CI reads these from GitHub Actions secrets), or
#   2. cmake/Identity.local.cmake (gitignored) on a developer machine.
#
#   tools/check-identity-leak.sh            check tracked files + all commit messages
#   tools/check-identity-leak.sh --staged   check the staged changes only (pre-commit)
#
# The 4-character codes are matched as whole words, so the product and company
# names (which contain them as prefixes) don't trigger false positives.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

read_local() {
    local name=$1 file=cmake/Identity.local.cmake
    [[ -f "$file" ]] || return 0
    sed -nE "s/^[[:space:]]*set\(${name}[[:space:]]+\"([^\"]*)\".*/\1/p" "$file" | tail -n1
}

manufacturer="${BRUTALIS_MANUFACTURER_CODE:-$(read_local BRUTALIS_MANUFACTURER_CODE)}"
plugin="${BRUTALIS_PLUGIN_CODE:-$(read_local BRUTALIS_PLUGIN_CODE)}"
bundle="${BRUTALIS_BUNDLE_ID:-$(read_local BRUTALIS_BUNDLE_ID)}"

values=()
for v in "$manufacturer" "$plugin" "$bundle"; do
    [[ -n "$v" ]] && values+=("$v")
done

if [[ ${#values[@]} -eq 0 ]]; then
    echo "check-identity-leak: no private values available (no env vars, no cmake/Identity.local.cmake); skipping."
    exit 0
fi

# The placeholders are public; never treat them as secrets.
placeholder_file=cmake/Identity.example.cmake
filtered=()
for v in "${values[@]}"; do
    if grep -qF "\"$v\"" "$placeholder_file"; then continue; fi
    filtered+=("$v")
done
[[ ${#filtered[@]} -eq 0 ]] && { echo "check-identity-leak: only placeholder values configured; nothing to check."; exit 0; }

patterns=()
for v in "${filtered[@]}"; do patterns+=(-e "$v"); done

leaked=0
if [[ "${1:-}" == "--staged" ]]; then
    if git diff --cached -U0 --no-color | grep '^+' | grep -qwF "${patterns[@]}"; then
        echo "check-identity-leak: a private identity value is in the staged changes." >&2
        leaked=1
    fi
else
    # Print only file names, never the matching lines: CI logs are public.
    if files=$(git grep -l -w -F "${patterns[@]}" -- . 2>/dev/null) && [[ -n "$files" ]]; then
        echo "check-identity-leak: private identity value found in tracked files:" >&2
        echo "$files" >&2
        leaked=1
    fi
    if git rev-parse --verify -q HEAD >/dev/null; then
        if git log --format='%H %B' | grep -qwF "${patterns[@]}"; then
            echo "check-identity-leak: private identity value found in a commit message." >&2
            leaked=1
        fi
    fi
fi

if [[ $leaked -ne 0 ]]; then
    echo "Remove the value (use the BRUTALIS_* CMake variables instead). See docs/development/product-identity.md." >&2
    exit 1
fi
echo "check-identity-leak: OK (${#filtered[@]} values checked)."
