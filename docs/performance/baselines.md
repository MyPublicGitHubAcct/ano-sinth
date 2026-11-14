# CPU baselines

Benchmark results recorded over time, newest first. Phase 3 measures every optimisation against these. How to run, read, and record them: [benchmarking.md](benchmarking.md).

Compare only rows from the same machine and build. Percentages are of one core, for one channel, in real time at 48 kHz (20,833 ns per sample).

---

## 2026-11-13: M1.1 the 8 waveforms, as data

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark, on the working tree of the commit that adds the waveforms (it reports its parent, `35460ca`, as modified).
- **What changed:** the oscillators now read each waveform from a table of corners ([oscillator-waveforms.md](../dsp/oscillator-waveforms.md)) instead of a switch. New rows: every waveform at 440 Hz and 12 kHz, and the saw at 12 kHz.
- **Before and after, same session:** the parent commit's build was benchmarked straight after this run on the same machine, so the comparison below is not muddied by noise between sessions.

| Benchmark | before (ns / sample) | after | change |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz | 3.07 | 3.16 | +3% |
| minBLEP 64-tap, saw 5 kHz | 6.72 | 7.24 | +8% |
| minBLEP 64-tap, square 440 Hz | 3.52 | 3.71 | +5% |
| minBLEP 64-tap, square 5 kHz | 11.13 | 11.66 | +5% |
| minBLEP 64-tap, square 12 kHz | 21.61 | 23.64 | +9% |
| minBLEP 64-tap, triangle 440 Hz | 3.81 | 3.86 | +1% |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample | 3.95 | 4.43 | +12% |
| minBLEP 64-tap, 32 saws | 94.70 | 90.60 | −4% |
| minBLEP 32-tap, 32 saws | 90.41 | 92.04 | +2% |
| PolyBLEP 2-point, saw 440 Hz | 2.41 | 3.28 | +36% |
| PolyBLEP 2-point, square 440 Hz | 3.12 | 5.61 | +80% |
| PolyBLEP 2-point, 32 saws | 86.64 | 108.32 | +25% |
| PolyBLEP 4-point, saw 440 Hz | 2.50 | 5.19 | +108% |
| PolyBLEP 4-point, triangle 440 Hz | 4.18 | 8.90 | +113% |
| PolyBLEP 4-point, 32 saws | 77.71 | 164.63 | +112% |

Full run:

| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |
|---|---:|---:|---:|
| PolyBLEP 2-point, saw 440 Hz x512 | 1,678 | 3.28 | 0.016 |
| PolyBLEP 2-point, saw 5 kHz x512 | 1,841 | 3.59 | 0.017 |
| PolyBLEP 2-point, saw 12 kHz x512 | 1,859 | 3.63 | 0.017 |
| PolyBLEP 2-point, square 440 Hz x512 | 2,871 | 5.61 | 0.027 |
| PolyBLEP 2-point, square 5 kHz x512 | 3,082 | 6.02 | 0.029 |
| PolyBLEP 2-point, square 12 kHz x512 | 3,136 | 6.13 | 0.029 |
| PolyBLEP 2-point, HPulse 440 Hz x512 | 2,582 | 5.04 | 0.024 |
| PolyBLEP 2-point, HPulse 12 kHz x512 | 2,914 | 5.69 | 0.027 |
| PolyBLEP 2-point, QPulse 440 Hz x512 | 2,666 | 5.21 | 0.025 |
| PolyBLEP 2-point, QPulse 12 kHz x512 | 3,084 | 6.02 | 0.029 |
| PolyBLEP 2-point, triangle 440 Hz x512 | 2,721 | 5.31 | 0.026 |
| PolyBLEP 2-point, triangle 12 kHz x512 | 2,982 | 5.82 | 0.028 |
| PolyBLEP 2-point, sine 440 Hz x512 | 2,526 | 4.93 | 0.024 |
| PolyBLEP 2-point, sine 12 kHz x512 | 2,166 | 4.23 | 0.020 |
| PolyBLEP 2-point, TriSaw 440 Hz x512 | 2,917 | 5.70 | 0.027 |
| PolyBLEP 2-point, TriSaw 12 kHz x512 | 3,343 | 6.53 | 0.031 |
| PolyBLEP 2-point, saw 440 Hz, pitch set every sample x512 | 2,503 | 4.89 | 0.023 |
| PolyBLEP 2-point, 32 saws (4 osc x 8 unison) x512 | 55,458 | 108.32 | 0.520 |
| PolyBLEP 4-point, saw 440 Hz x512 | 2,660 | 5.19 | 0.025 |
| PolyBLEP 4-point, saw 5 kHz x512 | 2,884 | 5.63 | 0.027 |
| PolyBLEP 4-point, saw 12 kHz x512 | 2,852 | 5.57 | 0.027 |
| PolyBLEP 4-point, square 440 Hz x512 | 3,413 | 6.67 | 0.032 |
| PolyBLEP 4-point, square 5 kHz x512 | 3,907 | 7.63 | 0.037 |
| PolyBLEP 4-point, square 12 kHz x512 | 4,108 | 8.02 | 0.039 |
| PolyBLEP 4-point, HPulse 440 Hz x512 | 3,431 | 6.70 | 0.032 |
| PolyBLEP 4-point, HPulse 12 kHz x512 | 4,068 | 7.95 | 0.038 |
| PolyBLEP 4-point, QPulse 440 Hz x512 | 3,400 | 6.64 | 0.032 |
| PolyBLEP 4-point, QPulse 12 kHz x512 | 4,085 | 7.98 | 0.038 |
| PolyBLEP 4-point, triangle 440 Hz x512 | 4,555 | 8.90 | 0.043 |
| PolyBLEP 4-point, triangle 12 kHz x512 | 6,145 | 12.00 | 0.058 |
| PolyBLEP 4-point, sine 440 Hz x512 | 2,851 | 5.57 | 0.027 |
| PolyBLEP 4-point, sine 12 kHz x512 | 2,389 | 4.67 | 0.022 |
| PolyBLEP 4-point, TriSaw 440 Hz x512 | 4,718 | 9.21 | 0.044 |
| PolyBLEP 4-point, TriSaw 12 kHz x512 | 6,699 | 13.08 | 0.063 |
| PolyBLEP 4-point, saw 440 Hz, pitch set every sample x512 | 3,635 | 7.10 | 0.034 |
| PolyBLEP 4-point, 32 saws (4 osc x 8 unison) x512 | 84,293 | 164.63 | 0.790 |
| minBLEP 32-tap, saw 440 Hz x512 | 1,547 | 3.02 | 0.015 |
| minBLEP 32-tap, saw 5 kHz x512 | 2,987 | 5.83 | 0.028 |
| minBLEP 32-tap, saw 12 kHz x512 | 4,495 | 8.78 | 0.042 |
| minBLEP 32-tap, square 440 Hz x512 | 1,711 | 3.34 | 0.016 |
| minBLEP 32-tap, square 5 kHz x512 | 4,263 | 8.33 | 0.040 |
| minBLEP 32-tap, square 12 kHz x512 | 7,715 | 15.07 | 0.072 |
| minBLEP 32-tap, HPulse 440 Hz x512 | 1,598 | 3.12 | 0.015 |
| minBLEP 32-tap, HPulse 12 kHz x512 | 7,939 | 15.51 | 0.074 |
| minBLEP 32-tap, QPulse 440 Hz x512 | 1,686 | 3.29 | 0.016 |
| minBLEP 32-tap, QPulse 12 kHz x512 | 7,924 | 15.48 | 0.074 |
| minBLEP 32-tap, triangle 440 Hz x512 | 1,697 | 3.31 | 0.016 |
| minBLEP 32-tap, triangle 12 kHz x512 | 7,987 | 15.60 | 0.075 |
| minBLEP 32-tap, sine 440 Hz x512 | 4,072 | 7.95 | 0.038 |
| minBLEP 32-tap, sine 12 kHz x512 | 3,405 | 6.65 | 0.032 |
| minBLEP 32-tap, TriSaw 440 Hz x512 | 1,796 | 3.51 | 0.017 |
| minBLEP 32-tap, TriSaw 12 kHz x512 | 10,417 | 20.35 | 0.098 |
| minBLEP 32-tap, saw 440 Hz, pitch set every sample x512 | 2,157 | 4.21 | 0.020 |
| minBLEP 32-tap, 32 saws (4 osc x 8 unison) x512 | 47,125 | 92.04 | 0.442 |
| minBLEP 64-tap, saw 440 Hz x512 | 1,619 | 3.16 | 0.015 |
| minBLEP 64-tap, saw 5 kHz x512 | 3,706 | 7.24 | 0.035 |
| minBLEP 64-tap, saw 12 kHz x512 | 6,049 | 11.81 | 0.057 |
| minBLEP 64-tap, square 440 Hz x512 | 1,902 | 3.71 | 0.018 |
| minBLEP 64-tap, square 5 kHz x512 | 5,969 | 11.66 | 0.056 |
| minBLEP 64-tap, square 12 kHz x512 | 12,102 | 23.64 | 0.113 |
| minBLEP 64-tap, HPulse 440 Hz x512 | 1,856 | 3.62 | 0.017 |
| minBLEP 64-tap, HPulse 12 kHz x512 | 12,288 | 24.00 | 0.115 |
| minBLEP 64-tap, QPulse 440 Hz x512 | 1,890 | 3.69 | 0.018 |
| minBLEP 64-tap, QPulse 12 kHz x512 | 12,409 | 24.24 | 0.116 |
| minBLEP 64-tap, triangle 440 Hz x512 | 1,974 | 3.86 | 0.019 |
| minBLEP 64-tap, triangle 12 kHz x512 | 12,110 | 23.65 | 0.114 |
| minBLEP 64-tap, sine 440 Hz x512 | 3,964 | 7.74 | 0.037 |
| minBLEP 64-tap, sine 12 kHz x512 | 3,468 | 6.77 | 0.033 |
| minBLEP 64-tap, TriSaw 440 Hz x512 | 2,012 | 3.93 | 0.019 |
| minBLEP 64-tap, TriSaw 12 kHz x512 | 16,698 | 32.61 | 0.157 |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample x512 | 2,270 | 4.43 | 0.021 |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) x512 | 46,386 | 90.60 | 0.435 |

### Observations

- **minBLEP costs what it did.** At ordinary pitches it is within noise of the hard-coded switch (32 saws: 2.8 ns per oscillator-sample), and it is 5–12% slower where corners come often (5 and 12 kHz) and when the pitch is set every sample. Two changes got it there; without them it was about 60% slower:
  - a single "distance to the next corner" compare per sample;
  - two versions of the loop body, with and without the sine.
- **Cost grows with the residual adds per cycle.** At 12 kHz, 64 taps: saw 11.8 ns (1 per cycle); square, HPulse, QPulse, and triangle 23.6–24.2 ns (2); TriSaw 32.6 ns (3, because its corner at 0 both jumps and bends). The TriSaw at 12 kHz is the new worst case. 512 voices of it would take about 80% of a core at 48 kHz. Adding the step and ramp residuals of one corner in a single pass would bring it closer to the square (a Phase 3 item).
- **The sine costs 6.8–7.7 ns,** most of it `std::sin` (about 4 ns), against 3.2 ns for a saw. A polynomial sine is a Phase 3 candidate. It would need about degree 9 to keep the sine's harmonics below −100 dB.
- **The PolyBLEP reference got slower, by 25–110%.** PolyBLEP tests every corner, ahead and behind, on every sample, so reading the corners from the table instead of constants costs it on every sample. It is a test-only reference, kept for its quality numbers, which are unchanged. The D6 entry below still holds its hard-coded CPU figures, and those are what the D6 choice was made on.

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
