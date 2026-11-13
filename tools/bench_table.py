#!/usr/bin/env python3
"""Turns Catch2's XML benchmark results into a Markdown table.

    python3 tools/bench_table.py build/relwithdebinfo/benchmarks.xml

Benchmark names ending in "xN" process N samples per run; the table divides by
N for ns/sample and compares that with the real-time budget at 48 kHz (one
sample every 20833 ns) for one channel.
"""
import re
import sys
import xml.etree.ElementTree as ElementTree

BUDGET_NS_PER_SAMPLE = 1e9 / 48000.0


def main(path):
    root = ElementTree.parse(path).getroot()
    print("| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |")
    print("|---|---:|---:|---:|")
    for result in root.iter("BenchmarkResults"):
        name = result.get("name")
        mean = float(result.find("mean").get("value"))
        match = re.search(r"x(\d+)$", name)
        samples = int(match.group(1)) if match else 1
        per_sample = mean / samples
        share = 100.0 * per_sample / BUDGET_NS_PER_SAMPLE
        print(f"| {name} | {mean:,.0f} | {per_sample:.2f} | {share:.3f} |")


if __name__ == "__main__":
    main(sys.argv[1])
