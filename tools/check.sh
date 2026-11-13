#!/usr/bin/env bash
# The local gate: run this before every commit (macOS, for now).
# It configures, builds, and runs all tests (including pluginval on the built
# VST3 and auval on the AU, which is installed temporarily), then runs clang-tidy, the format check, and the identity-leak check.
# It stops at the first failure.
#
#   tools/check.sh          debug build + tests, tidy, format, leak check
#   tools/check.sh --asan   also run the ASan/UBSan build and tests
#
# CI on GitHub runs only for major/minor version tags (vX.Y.0), so this script
# is what keeps main green day to day.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

step() { printf '\n==> %s\n' "$*"; }

step "Debug: configure, build, test"
cmake --workflow --preset debug

if [[ "${1:-}" == "--asan" ]]; then
    step "ASan/UBSan: configure, build, test"
    cmake --workflow --preset asan
fi

step "clang-tidy"
tools/tidy.sh build/debug

step "Format check"
tools/format.sh --check

step "Identity leak check"
tools/check-identity-leak.sh

printf '\nAll checks passed.\n'
