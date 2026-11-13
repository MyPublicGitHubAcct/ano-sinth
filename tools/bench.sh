#!/usr/bin/env bash
# Runs the [!benchmark] test cases in an optimised build (relwithdebinfo) and
# prints a Markdown table: time per 512-sample block, per sample, and the share
# of one CPU core that one channel would use in real time at 48 kHz.
#
#   tools/bench.sh                 every benchmark
#   tools/bench.sh "[oversampling]"   only those that also match this Catch2 spec
#
# How to read and record the results: docs/performance/benchmarking.md
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

spec="[!benchmark]${1:-}"
build=build/relwithdebinfo
cmake --preset relwithdebinfo >/dev/null
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests >/dev/null

xml="$build/benchmarks.xml"
"$build/tests/dsp/anoesis_dsp_tests" "$spec" --reporter "xml::out=$xml" --benchmark-samples 100 >/dev/null

if [[ "$(uname)" == Darwin ]]; then
    cpu=$(sysctl -n machdep.cpu.brand_string)
else
    cpu=$(sed -nE 's/^model name\s*:\s*//p' /proc/cpuinfo | head -n1)
fi
compiler=$(sed -nE 's/^CMAKE_CXX_COMPILER:[A-Z]+=(.*)/\1/p' "$build/CMakeCache.txt")
version=$("$compiler" --version 2>/dev/null | head -n1 || true)

echo "Machine: $cpu ($(uname -sm))"
echo "Compiler: $version"
echo "Build: RelWithDebInfo, commit $(git rev-parse --short HEAD)$(git diff --quiet || echo ' (modified)')"
echo
python3 -I tools/bench_table.py "$xml"
