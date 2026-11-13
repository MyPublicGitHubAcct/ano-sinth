# White noise and reproducible randomness

Sylenth1's oscillators include a noise waveform. The LFOs and the arpeggiator have random modes, and drift (v3) and unison phase can be randomised. All of these need random numbers, and **reproducible** random numbers make every one of them testable. This page covers the generator in `libs/dsp` and the noise source built on it.

## Theory

### White noise

White noise is a sequence of independent, identically distributed samples. Independence makes its autocorrelation an impulse, and the Fourier transform of an impulse is flat. So the **power spectrum is flat**: equal power per hertz from DC to Nyquist. That is all "white" means. The amplitude *distribution* is a separate choice: uniform, Gaussian, or even ±1 (binary noise) are all white if the samples are independent.

Uniform noise on (−1, 1) has variance 1/3, so an RMS level of 1/√3 ≈ 0.577 (−4.77 dBFS) and a peak of 1. It never clips at full scale, which is why synth noise oscillators usually use it. A Gaussian would have unbounded peaks.

### Pseudo-random generators

A computer generates *pseudo*-random numbers: a deterministic state machine whose outputs pass statistical tests for randomness. For audio, the requirements are modest. The statistical quality must be good enough that no pattern is audible or measurable at −100 dB. It must be fast, and its state must be small. It does not need to be cryptographically secure.

- **Linear congruential generators** (x = a·x + c mod m) are tiny but have poor low bits and visible lattice structure. The classic `rand()` is one of these.
- **Mersenne Twister** (`std::mt19937`) has excellent statistics but 2.5 KB of state and a slow refill step. Its output sequence is specified by the standard, but the distributions built on it (`std::uniform_real_distribution`) are not, so the floats differ between standard libraries.
- **xorshift-family generators** (Marsaglia 2003; Blackman and Vigna's xoshiro/xoroshiro, 2018) use only shifts, rotates, XORs and an add, with 128–256 bits of state. They pass BigCrush and PractRand, apart from known weaknesses in the lowest bits of the `+` variants, which don't matter when only the top bits are used.

### Seeding

A seed has to be spread over the whole state. If you seed a 128-bit xoshiro state with "1" directly, it starts in a near-zero state and its first outputs are poor. SplitMix64, a 64-bit counter passed through a strong bit mixer, is the seeding function the xoshiro authors recommend. Its output is a bijection of the counter, so the all-zero state, which xoshiro must never enter, cannot be produced.

## How it is applied here

[libs/dsp/include/anoesis/dsp/math/random.hpp](../../libs/dsp/include/anoesis/dsp/math/random.hpp): `Random` is **xoshiro128+** (128-bit state, period 2¹²⁸ − 1), seeded by SplitMix64.

- `nextUint32()` gives raw 32-bit output, for random choices such as the arpeggiator's random mode and LFO sample-and-hold.
- `nextBipolar()` takes the **top 24 bits** k and returns (2k + 1 − 2²⁴)/2²⁴. The 2²⁴ possible values are odd multiples of 2⁻²⁴, symmetric around zero, with mean exactly 0 over the full set. None is exactly 0, and all are exactly representable in float, so there is no rounding and the conversion can't differ between platforms.
- `nextUnipolar()` returns k/2²⁴ ∈ [0, 1).
- `reset()` restarts the sequence, and `setSeed()` changes the seed. Seed 0 is valid.

xoshiro128+ uses only 32-bit operations, so it is as cheap on a Cortex-M4 as on a desktop. (PCG32, the other common choice, needs a 64-bit multiply.)

[libs/dsp/include/anoesis/dsp/generators/white_noise.hpp](../../libs/dsp/include/anoesis/dsp/generators/white_noise.hpp): `WhiteNoise` is a `BlockProcessor` ([lifecycle](processor-lifecycle.md)) that overwrites every channel with `nextBipolar()` noise. It draws **sample-major**: every channel of sample n, then sample n + 1. That has two consequences:

- the output is bit-identical however a host splits its blocks, which an earlier channel-major draft would not have been (each chunk would have given channel 0 a different stretch of the sequence);
- the channels are independent, so stereo noise is decorrelated.

### Determinism as a feature

Everything up to the float conversion is integer arithmetic, and the conversion is exact. A given seed therefore produces **the same bits on every compiler and CPU**. Two things follow:

- golden-file renders of patches with noise can be compared exactly, not within a tolerance;
- the known-answer test pins the algorithm. An accidental change to the generator would silently change every patch that uses noise, and it fails that test first.

## Trade-offs and alternatives

- **xoshiro128+ versus xoshiro128\*\*/++.** The `**` and `++` scramblers make all 32 bits strong, at the cost of an extra multiply or add-and-rotate. We use the top 24 bits only, so `+` is enough and cheapest.
- **Uniform versus Gaussian noise.** Gaussian noise (Box–Muller, or a sum of uniforms) sounds identical once filtered. It costs more and has unbounded peaks. Pink and other coloured noises are filters applied to white noise; if needed, they belong to Phase 2.
- **`<random>`.** Rejected: its float distributions are implementation-defined, so results would differ between libc++, libstdc++, and MSVC. That breaks golden files and cross-platform tests.

## Tests

- [tests/dsp/math/random_test.cpp](../../tests/dsp/math/random_test.cpp):
  - the same seed gives the same 10,000 outputs, and a neighbouring seed differs almost everywhere;
  - `reset()` and `setSeed()` restart the sequence;
  - **known-answer values** for seeds 0 and 1, cross-checked against an independent Python port of SplitMix64 and xoshiro128+;
  - `nextBipolar()` is in (−1, 1) and never 0, and `nextUnipolar()` is in [0, 1).
- [tests/dsp/generators/white_noise_test.cpp](../../tests/dsp/generators/white_noise_test.cpp):
  - the lifecycle contract holds;
  - chunked and whole-block renders are bit-identical;
  - statistics over 2²⁰ samples with fixed seeds, so the test is deterministic. Each bound is 5 standard deviations of its estimator:

    | Statistic | Expected | Measured (seed 2026) | Bound |
    |---|---|---|---|
    | mean | 0 | 7.8e-4 | 2.8e-3 |
    | variance | 1/3 | 0.33351 | ±1.5e-3 |
    | lag-1 autocorrelation | 0 | 4.2e-4 | 4.9e-3 |
    | χ² over 64 bins (63 d.o.f.) | 63 | 70.4 | 119 |
    | stereo cross-correlation (2¹⁸ samples, seed 3) | 0 | 9.3e-4 | 9.8e-3 |

  - the Welch spectrum is flat to within 0.043 dB across 15 bands (bound 0.25 dB), measured with the [harness](measuring-dsp.md);
  - a stereo render matches its golden file, `tests/golden/dsp/white_noise_seed1_stereo.wav`, bit for bit.

## References

- D. Blackman and S. Vigna, "Scrambled Linear Pseudorandom Number Generators", *ACM Transactions on Mathematical Software* 47(4), 2021; reference code and notes at <https://prng.di.unimi.it/> (public domain).
- G. Steele, D. Lea, and C. Flood, "Fast Splittable Pseudorandom Number Generators" (SplitMix), OOPSLA 2014.
- G. Marsaglia, "Xorshift RNGs", *Journal of Statistical Software* 8(14), 2003.
- P. L'Ecuyer and R. Simard, "TestU01: A C Library for Empirical Testing of Random Number Generators", *ACM TOMS* 33(4), 2007.
