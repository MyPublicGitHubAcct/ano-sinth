#!/usr/bin/env python3
"""Independent reference for anoesis::dsp::Random (SplitMix64 -> xoshiro128+).

    python3 tools/reference/xoshiro128plus.py              # seeds 0 and 1, 4 outputs each
    python3 tools/reference/xoshiro128plus.py 2026 -n 8    # any seeds, any count

Prints the first raw 32-bit outputs for each seed as a C++ std::array
initialiser, ready to paste into the known-answer test in
tests/dsp/math/random_test.cpp.

Why it exists: a known-answer test is only as good as its expected values. If
they were copied from the C++ implementation's own output, the test would pin
whatever the code happened to do, bugs included. This port is written from the
published reference code (prng.di.unimi.it: splitmix64.c and xoshiro128plus.c),
not from random.hpp, in a different language with arbitrary-precision integers
masked explicitly to 32/64 bits. When both agree, the C++ is very likely right.

Seeding matches random.hpp: two SplitMix64 outputs, each split low word first,
fill the four 32-bit state words s[0..3].

Pure Python (standard library only), so it runs anywhere. Doc: docs/dsp/white-noise.md
"""
import argparse

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF


def splitmix64(x):
    """One SplitMix64 step. Returns (new counter, output)."""
    x = (x + 0x9E3779B97F4A7C15) & MASK64
    z = x
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return x, z ^ (z >> 31)


def seed_state(seed):
    """128-bit xoshiro state from a 64-bit seed, as random.hpp builds it."""
    x, first = splitmix64(seed & MASK64)
    _, second = splitmix64(x)
    return [first & MASK32, first >> 32, second & MASK32, second >> 32]


def rotl(x, k):
    return ((x << k) | (x >> (32 - k))) & MASK32


def next_uint32(s):
    """One xoshiro128+ step; updates s in place and returns the output."""
    result = (s[0] + s[3]) & MASK32
    t = (s[1] << 9) & MASK32
    s[2] ^= s[0]
    s[3] ^= s[1]
    s[1] ^= s[2]
    s[0] ^= s[3]
    s[2] ^= t
    s[3] = rotl(s[3], 11)
    return result


def outputs(seed, count):
    s = seed_state(seed)
    return [next_uint32(s) for _ in range(count)]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("seeds", nargs="*", type=int, default=[0, 1])
    parser.add_argument("-n", "--count", type=int, default=4)
    args = parser.parse_args()
    for seed in args.seeds:
        values = ", ".join(f"0x{v:08X}U" for v in outputs(seed, args.count))
        print(f"seed {seed}: {{{values}}}")


if __name__ == "__main__":
    main()
