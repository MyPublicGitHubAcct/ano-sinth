# CPU baselines

Benchmark results recorded over time, newest first. Phase 3 measures every optimisation against these. How to run, read, and record them: [benchmarking.md](benchmarking.md).

Compare only rows from the same machine and build. Percentages are of one core, for one channel, in real time at 48 kHz (20,833 ns per sample).

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
