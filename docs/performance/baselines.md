# CPU baselines

Benchmark results recorded over time, newest first. Phase 3 measures every optimisation against these. How to run, read, and record them: [benchmarking.md](benchmarking.md).

Compare only rows from the same machine and build. Percentages are of one core, for one channel, in real time at 48 kHz (20,833 ns per sample).

---

## 2026-11-13: M1.1 oscillator anti-aliasing prototypes (D6)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark, at commit `984d868` (the run was on its working tree just before it was committed).
- **Noise:** a second run differed by up to 12% on single rows (PolyBLEP 4-point square 5 kHz: 4.86 and 3.94 ns) and by 5% on the 32-oscillator rows.
- **Since then:** the minBLEP and PolyBLEP benchmarks still exist under the same names (minBLEP in `anoesis::dsp`, PolyBLEP as a test-only reference). The wavetable was removed after D6; its rows can be rerun from commit `984d868`.
- **What runs:** one oscillator, one 512-sample block per benchmark, at 48 kHz, through `next()` one sample at a time. Nothing is vectorised yet. The "32 saws" rows are one note's worth (4 oscillators × 8 unison voices at 110 to 880 Hz, ±20 cents): divide by 32 for the cost per oscillator. Quality measurements and the choice: [band-limited-oscillators.md](../dsp/band-limited-oscillators.md).

| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |
|---|---:|---:|---:|
| PolyBLEP 2-point, saw 440 Hz x512 | 1,287 | 2.51 | 0.012 |
| PolyBLEP 2-point, saw 5 kHz x512 | 1,406 | 2.75 | 0.013 |
| PolyBLEP 2-point, square 440 Hz x512 | 1,495 | 2.92 | 0.014 |
| PolyBLEP 2-point, square 5 kHz x512 | 1,821 | 3.56 | 0.017 |
| PolyBLEP 2-point, square 12 kHz x512 | 1,698 | 3.32 | 0.016 |
| PolyBLEP 2-point, triangle 440 Hz x512 | 1,794 | 3.50 | 0.017 |
| PolyBLEP 2-point, saw 440 Hz, pitch set every sample x512 | 2,001 | 3.91 | 0.019 |
| PolyBLEP 2-point, 32 saws (4 osc x 8 unison) x512 | 40,537 | 79.17 | 0.380 |
| PolyBLEP 4-point, saw 440 Hz x512 | 1,261 | 2.46 | 0.012 |
| PolyBLEP 4-point, saw 5 kHz x512 | 1,488 | 2.91 | 0.014 |
| PolyBLEP 4-point, square 440 Hz x512 | 1,459 | 2.85 | 0.014 |
| PolyBLEP 4-point, square 5 kHz x512 | 2,489 | 4.86 | 0.023 |
| PolyBLEP 4-point, square 12 kHz x512 | 2,169 | 4.24 | 0.020 |
| PolyBLEP 4-point, triangle 440 Hz x512 | 2,214 | 4.32 | 0.021 |
| PolyBLEP 4-point, saw 440 Hz, pitch set every sample x512 | 2,304 | 4.50 | 0.022 |
| PolyBLEP 4-point, 32 saws (4 osc x 8 unison) x512 | 39,833 | 77.80 | 0.373 |
| minBLEP 32-tap, saw 440 Hz x512 | 1,469 | 2.87 | 0.014 |
| minBLEP 32-tap, saw 5 kHz x512 | 2,718 | 5.31 | 0.025 |
| minBLEP 32-tap, square 440 Hz x512 | 1,639 | 3.20 | 0.015 |
| minBLEP 32-tap, square 5 kHz x512 | 3,926 | 7.67 | 0.037 |
| minBLEP 32-tap, square 12 kHz x512 | 6,710 | 13.11 | 0.063 |
| minBLEP 32-tap, triangle 440 Hz x512 | 1,831 | 3.58 | 0.017 |
| minBLEP 32-tap, saw 440 Hz, pitch set every sample x512 | 1,901 | 3.71 | 0.018 |
| minBLEP 32-tap, 32 saws (4 osc x 8 unison) x512 | 48,380 | 94.49 | 0.454 |
| minBLEP 64-tap, saw 440 Hz x512 | 1,602 | 3.13 | 0.015 |
| minBLEP 64-tap, saw 5 kHz x512 | 3,464 | 6.76 | 0.032 |
| minBLEP 64-tap, square 440 Hz x512 | 1,848 | 3.61 | 0.017 |
| minBLEP 64-tap, square 5 kHz x512 | 5,683 | 11.10 | 0.053 |
| minBLEP 64-tap, square 12 kHz x512 | 11,368 | 22.20 | 0.107 |
| minBLEP 64-tap, triangle 440 Hz x512 | 1,899 | 3.71 | 0.018 |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample x512 | 1,976 | 3.86 | 0.019 |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) x512 | 47,092 | 91.98 | 0.441 |
| Wavetable (mipmapped), saw 440 Hz x512 | 1,200 | 2.34 | 0.011 |
| Wavetable (mipmapped), saw 5 kHz x512 | 1,165 | 2.28 | 0.011 |
| Wavetable (mipmapped), square 440 Hz x512 | 1,218 | 2.38 | 0.011 |
| Wavetable (mipmapped), square 5 kHz x512 | 1,279 | 2.50 | 0.012 |
| Wavetable (mipmapped), square 12 kHz x512 | 1,252 | 2.45 | 0.012 |
| Wavetable (mipmapped), triangle 440 Hz x512 | 1,238 | 2.42 | 0.012 |
| Wavetable (mipmapped), saw 440 Hz, pitch set every sample x512 | 1,772 | 3.46 | 0.017 |
| Wavetable (mipmapped), 32 saws (4 osc x 8 unison) x512 | 38,783 | 75.75 | 0.364 |

### Observations

- **At ordinary pitches, all five cost about the same:** 2.3 to 2.9 ns per oscillator-sample in the 32-saw bank (PolyBLEP 2.4, minBLEP 2.9, wavetable 2.3). The per-sample overhead (a scalar loop with a phase carried from sample to sample, and a switch on the shape) dominates. So does the difference between 32 and 64 minBLEP taps (2.95 and 2.87 ns): at 440 Hz an edge comes once every 109 samples.
- **minBLEP's cost grows with pitch; the others' doesn't.** Each edge adds `Taps` multiply-adds. A 12 kHz square has two edges every 4 samples: 13.1 ns per sample at 32 taps, 22.2 ns at 64, against 3.3 (PolyBLEP 2-point), 4.2 (4-point), and 2.5 (wavetable). The worst case is 512 voices of high squares: about 32% (32 taps) or 55% (64 taps) of one core at 48 kHz.
- **Setting the pitch every sample** costs 0.8 to 1.4 ns more: least for minBLEP (+0.8), which needs no reciprocal (it divides only at an edge), then the wavetable (+1.1, a level lookup), then PolyBLEP (+1.4, a reciprocal per sample).
- **First Phase 3 candidates:** the minBLEP residual add uses a masked ring index (`(position + i) & (Taps - 1)`), which blocks vectorisation; a double-length linear buffer would let the 32 or 64 multiply-adds per edge run 4 wide. Across unison voices, PolyBLEP vectorises with no gathers, the wavetable needs gathers, and minBLEP needs per-lane scatter of the edge events.

---

## 2026-11-13: M1.0 foundations (first baseline)

- **Machine:** Apple M1 Ultra, **virtualised** (`machdep.cpu.brand_string` reports "Apple M1 Ultra (Virtual)"), macOS arm64.
- **Compiler:** Apple clang 21.0.0 (clang-2100.3.34.2).
- **Build:** `relwithdebinfo` (`-O2 -g`), commit `M1.0h` (the commit that added the harness). `tools/bench.sh`, 100 samples per benchmark.
- **Noise:** two runs differed by up to about 10% (for example `fastExp2` 175 and 194 ns). Treat differences below about 15% as noise on this machine.

| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |
|---|---:|---:|---:|
| std::exp2 x512 | 914 | 1.79 | 0.009 |
| fastExp2 x512 | 175 | 0.34 | 0.002 |
| std::tanh x512 | 1,269 | 2.48 | 0.012 |
| fastTanh x512 | 228 | 0.45 | 0.002 |
| LinearSmoother ramping x512 | 407 | 0.80 | 0.004 |
| ExponentialSmoother ramping x512 | 1,903 | 3.72 | 0.018 |
| LinearSmoother settled x512 | 23 | 0.05 | 0.000 |
| WhiteNoise x512 | 574 | 1.12 | 0.005 |
| Oversampler 2x up+down x512 | 8,555 | 16.71 | 0.080 |
| Oversampler 4x up+down x512 | 13,711 | 26.78 | 0.129 |
| one-pole, normal state x512 | 1,035 | 2.02 | 0.010 |
| one-pole, denormal state, IEEE mode x512 | 1,021 | 1.99 | 0.010 |
| one-pole, denormal state, flush-to-zero x512 | 1,076 | 2.10 | 0.010 |

### Observations

- **Fast math pays off:** `fastExp2` is 5.2× faster than `std::exp2`, and `fastTanh` 5.6× faster than `std::tanh`, at the accuracy in [fast-math.md](../dsp/fast-math.md). Both loops vectorise; the library calls don't.
- **Oversampling** costs about 17 ns (2×) and 27 ns (4×) per sample per channel, round trip. That is 0.08% and 0.13% of a core. Stereo 4× distortion is about 0.26%, before the distortion itself. The time goes into serial allpass chains (each section depends on the previous one), which SIMD across channels or branches could overlap.
- **Denormals cost nothing on Apple Silicon:** the same loop takes about 2.0 ns per sample with normal state, denormal state, or flush-to-zero. This is the expected ARM behaviour ([denormals.md](../dsp/denormals.md)). The x86 penalty, the reason the guard exists, still needs measuring on an x86 machine.
- **First Phase 3 candidate: `ExponentialSmoother` is 4.7× slower than `LinearSmoother`** (3.72 against 0.80 ns per sample). The hypothesis: in `fill()`, the output span is `float*` and could alias the smoother's own `float` members. So the compiler reloads and stores `distance` and `current` around every write, which puts a store-to-load round trip into the loop-carried multiply chain. Copying the state into locals for the loop should fix it. This is recorded here, not fixed, so Phase 3 starts with a measured before.
