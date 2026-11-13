#!/usr/bin/env bash
# Points git at the versioned hooks in tools/hooks (pre-commit, commit-msg).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
git config core.hooksPath tools/hooks
echo "Git hooks installed from tools/hooks."
