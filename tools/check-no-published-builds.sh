#!/usr/bin/env bash
# Fails if any GitHub Actions workflow could publish a downloadable build.
#
# Policy (PLAN.md, 2026-11-13): GitHub hosts source only. CI builds and tests
# the plugin, but no compiled binary is ever made downloadable from GitHub:
# no workflow artifacts, no release assets, no packages, no Pages deploys.
# Binaries contain JUCE (AGPLv3 or commercial) and the real plugin identity,
# and distributing them is a separate decision. See docs/development/ci.md.
#
#   tools/check-no-published-builds.sh
#
# This catches publishing steps in workflow files. It cannot stop a person from
# uploading files by hand (gh release upload, the web UI); the policy covers that.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

[[ -d .github ]] || exit 0

# Steps and commands that make files downloadable from GitHub.
pattern='actions/upload-artifact|upload-pages-artifact|actions/deploy-pages'
pattern+='|action-gh-release|upload-release-asset|create-release'
pattern+='|gh[[:space:]]+release|gh[[:space:]]+api[^#]*/releases'
pattern+='|docker[[:space:]]+push|npm[[:space:]]+publish|packages:[[:space:]]*write|contents:[[:space:]]*write'

# Comment lines are ignored so workflows can explain the policy.
if matches=$(git grep -nIE "$pattern" -- .github | grep -vE '^[^:]+:[0-9]+:[[:space:]]*#'); then
    echo "check-no-published-builds: these lines would publish build output from GitHub:" >&2
    echo "$matches" >&2
    echo "GitHub must not host downloadable builds (see docs/development/ci.md)." >&2
    exit 1
fi
echo "check-no-published-builds: no publishing steps in workflows."
