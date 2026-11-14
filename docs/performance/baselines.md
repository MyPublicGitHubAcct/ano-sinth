# CPU baselines

Benchmark results recorded over time, newest first. Phase 3 measures every optimisation against these. How to run, read, and record them: [benchmarking.md](benchmarking.md).

Compare only rows from the same machine and build. Percentages are of one core, for one channel, in real time at 48 kHz (20,833 ns per sample).

---

## 2026-11-14: M1.1 oscillator drift

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark.
- **What changed** ([oscillator-drift.md](../dsp/oscillator-drift.md)): a new `PitchDrift` generator (two-pole noise, updated every 64 samples). Nothing in the oscillators or the slot changed. New rows: the generator alone, and drift applied to a voice's slots, held per 64 samples (the default) or added to a note per sample.
- **Before and after, same session:** the parent (`264f362`, the measurement commit, whose library code is identical) and then this tree. Every existing row is within −10% to +10% of the parent (the run-to-run noise of this machine), except one: "noteToFrequency every sample + inline stereo gain" read 23.43 on the parent against 9.21 now; earlier sessions measured it at 9.15–9.51, so the parent's reading is the outlier.

| Benchmark (ns / sample) | parent | this commit |
|---|---:|---:|
| minBLEP 64-tap, saw 440 Hz | 3.17 | 2.92 |
| minBLEP 64-tap, TriSaw 12 kHz | 17.80 | 17.97 |
| Oscillator slot, saw 440 Hz, settled | 3.28 | 3.31 |
| Oscillator slot, saw 440 Hz, pitch per sample | 6.41 | 6.45 |
| Unison slot, saw 440 Hz, 8 voices | 25.21 | 24.78 |
| Unison slot, saw 440 Hz, 8 voices, pitch per sample | 40.12 | 38.61 |
| Unison: 32-voice note (4 slots x 8 voices) into one bus | 102.89 | 104.01 |

New rows (drift at the default, 1.5 cents and 0.5 Hz; saws at 110–880 Hz, Detune 0.2 st, Stereo 0.5):

| Benchmark (ns / sample) | this commit |
|---|---:|
| Drift: the generator alone, `process()` | 0.17 |
| Drift: the generator alone, `addTo()` a note per sample | 0.19 |
| Drift: 1 slot x 1 voice: no drift / held per 64 / a note per sample | 2.99 / 3.50 / 6.52 |
| Drift: 1 slot x 8 voices: no drift / held per 64 / a note per sample | 23.44 / 23.93 / 35.44 |
| Drift: 4 slots x 8 voices: no drift / held per 64 / a note per sample | 103.48 / 101.45 / 147.22 |

### Observations

- **Held per 64 samples, a whole note costs what it did without drift** (101.5 against 103.5 ns, within noise). The generator is 0.17 ns per sample, and each slot converts its note 8 times per 512 samples instead of once.
- **A lone one-voice slot pays +17% held:** 8 process calls of 64 samples instead of one of 512, on an oscillator of 3 ns per sample.
- **A note per sample costs a whole note +42%,** for the `exp2` per sample per slot. A voice with per-sample glide or pitch modulation pays that anyway; the drift adds 0.19 ns.
- The six candidate models' costs (measured at `264f362`) are in [oscillator-drift.md](../dsp/oscillator-drift.md#cpu).

---

## 2026-11-14: M1.1 oscillator range (the Nyquist fade, pitch steps)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark.
- **What changed** ([oscillator-range.md](../dsp/oscillator-range.md)):
  - `MinBlepOscillator` fades its output about the waveform's mean from 0.45 fs, through one predictable branch per sample (`finishing`) that is false below 0.45 fs once an onset has settled.
  - `setIncrement` checks the size of the change: a pitch step of 6.1·10⁻⁵ cycles or more adds one ramp residual, at the next sample's corner scan.
  - `addResidual()` adds in two contiguous runs instead of through a masked ring index, so clang vectorises it.
  - New rows: a saw in the fade band (0.47 fs) and above Nyquist (0.6 fs); a 6 Hz vibrato set every sample; a pitch step every 64 samples; a sweep of 10 octaves in one block (a step on most samples).
- **Before and after, same session:** the parent (`9fef36a`), this tree, the parent again (exported with `git archive`, `external/` and `.cache/` symlinked, built `relwithdebinfo`), then this tree twice more. The parent was also run once with this commit's benchmark file, for the new rows. The threshold was first 8 times lower; the rows marked † were measured with it, before the project owner chose the final threshold.

| Benchmark (ns / sample) | parent (two runs) | this commit (two runs) | change |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz | 3.03, 3.06 | 2.90, 2.90 | −5% to −4% |
| minBLEP 64-tap, triangle 440 Hz | 3.74, 3.44 | 3.31, 3.31 | −11% to −4% |
| minBLEP 64-tap, sine 440 Hz | 8.69, 8.45 | 8.54, 8.33 | −4% to +1% |
| minBLEP 64-tap, saw 5 kHz | 7.15, 7.52 | 5.38, 4.85 | −36% to −25% |
| minBLEP 64-tap, saw 12 kHz | 12.43, 12.61 | 8.38, 7.94 | −37% to −33% |
| minBLEP 64-tap, square 12 kHz | 23.93, 23.77 | 15.30, 13.87 | −42% to −36% |
| minBLEP 64-tap, triangle 12 kHz | 24.20, 24.40 | 14.10, 13.98 | −43% to −42% |
| minBLEP 64-tap, TriSaw 12 kHz | 33.84, 33.85 | 18.74, 19.23 | −45% to −43% |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample (±1 st at 384 Hz) | 4.23, 4.22 | 4.36, 4.25 († 17.34, 16.86) | 0% to +3% |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) | 96.33, 94.67 | 93.32, 91.04 | −5% to −1% |
| Oscillator slot, saw 440 Hz, settled | 3.22, 3.22 | 3.37, 3.23 | 0% to +5% |
| Oscillator slot, saw 440 Hz, pitch per sample | 6.31, 6.19 | 6.56, 6.47 († 19.25, 19.04) | +3% to +6% |
| Oscillator slot, TriSaw 12 kHz, settled | 33.44, 33.95 | 19.51, 18.78 | −45% to −42% |
| Unison slot, saw 440 Hz, 8 voices | 25.86, 25.94 | 25.46, 24.88 | −4% to −2% |
| Unison slot, saw 12 kHz, 8 voices | 107.76, 107.60 | 69.85, 68.72 | −36% to −35% |
| Unison slot, saw 440 Hz, 8 voices, pitch per sample | 35.74, 36.08 | 41.13, 38.97 († 136.82, 134.79) | +8% to +15% |
| Unison: 32-voice note into one bus | 102.83, 101.18 | 104.27, 104.89 | +1% to +4% |
| minBLEP 64-tap, saw 1 kHz synced to a 1.09 kHz master | 4.77, 4.82 | 4.46, 4.58 | −7% to −4% |

New rows, with the parent measured once on the same benchmark file:

| Benchmark (ns / sample) | parent | this commit (two runs) |
|---|---:|---:|
| minBLEP 64-tap, saw at 0.47 fs (in the Nyquist fade) | 24.58 | 14.11, 14.15 |
| minBLEP 64-tap, saw at 0.6 fs (above Nyquist: its mean; the parent plays the clamped Nyquist tone) | 22.85 | 14.71, 13.99 |
| minBLEP 64-tap, saw 440 Hz, a 6 Hz vibrato set every sample | 4.90 | 4.77, 4.77 |
| minBLEP 64-tap, saw 440 Hz, a pitch step every 64 samples | 3.28 | 3.54, 3.45 |
| minBLEP 64-tap, saw, 20 Hz to 20 kHz in one block (a step on most samples) | 7.14 | 15.19, 15.23 |

### Observations

- **Constant pitch is unchanged below the fade band.** The fade costs one predictable branch, and the pitch-step check sits in `setIncrement`, outside the per-sample loop.
- **High notes are 25–45% faster**, at every waveform but the sine (which has no edges), because the residual add vectorises now. That is the "masked ring index" item from D6's Phase 3 list, done with two contiguous loops rather than a double-length buffer. Unison at 12 kHz gains the same: 8 voices 108 → 69 ns.
- **A pitch every sample costs about 0.4 ns more per voice** (+3 to +15%), for the change and fade checks in `setIncrement`. With the first, lower threshold, a fast vibrato crossed it on every sample and paid a residual there: the † figures.
- **A pitch step costs one residual when it happens:** +0.2 ns per sample at one step every 64 samples. A sweep that steps on most samples pays it almost every sample (7.1 → 15.2 ns).
- **A voice in the fade band or above Nyquist** costs what a Nyquist-rate saw does (14 ns), less than the parent's clamped tone (23–25 ns) thanks to the vectorised adds, although it outputs a constant. Skipping it is a Phase 3 item.
- `Unison slot, 0 voices` reads 0.14 against 0.13 ns per sample: 5 ns per block, the resolution of the measurement.

---

## 2026-11-14: M1.1 unison

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark, on the working tree of the commit that adds unison (it reports its parent, `1aa1b42`, as modified).
- **What changed:** `OscillatorSlot` now holds 8 voices (a `MinBlepOscillator`, a `StereoGain`, a detune ratio, and a state each), one `WhiteNoise`, and three smoothers for the noise mix ([oscillator-unison.md](../dsp/oscillator-unison.md)). It is about 5 KB at 64 taps, against 680 bytes before. A block renders voice by voice per 64-sample chunk. With a pitch per sample, a chunk's centre frequencies are converted once, before the voices. `MinBlepOscillator` is unchanged. New rows: the slot with 1, 2, 4, and 8 voices (saw, Detune 0.2 st, Stereo 0.5, at 440 Hz and 12 kHz), 8 voices with a pitch per sample, 0 voices, noise at 8 voices, and a 32-voice note (4 slots of 8 voices, 110–880 Hz) into one bus.
- **Before and after, same session:** this tree, then the parent (exported with `git archive`, `external/` symlinked, built `relwithdebinfo`), then this tree again; then the slot rows twice more, alternating.

| Benchmark | parent (ns / sample) | this commit (two runs) | change |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz | 3.08 | 3.05, 3.29 | −1% to +7% |
| minBLEP 64-tap, square 440 Hz | 3.50 | 3.63, 3.51 | 0% to +4% |
| minBLEP 64-tap, triangle 440 Hz | 3.59 | 3.60, 3.63 | 0% to +1% |
| minBLEP 64-tap, sine 440 Hz | 8.26 | 8.72, 8.36 | +1% to +6% |
| minBLEP 64-tap, saw 12 kHz | 12.53 | 13.03, 12.56 | 0% to +4% |
| minBLEP 64-tap, square 12 kHz | 23.61 | 22.83, 23.32 | −3% to −1% |
| minBLEP 64-tap, TriSaw 12 kHz | 33.34 | 32.01, 34.37 | −4% to +3% |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) | 93.48 | 93.21, 92.45 | −1% to 0% |
| Oscillator slot, saw 440 Hz, settled | 3.29 (3.25, 3.64 later) | 3.28, 3.51 (3.52, 3.34 later) | within noise |
| Oscillator slot, saw 440 Hz, volume ramping | 18.13 (17.75, 18.53 later) | 4.65, 4.74 (5.06, 5.09 later) | a harness artefact in the parent; see below |
| Oscillator slot, saw 440 Hz, pitch per sample | 8.32 (8.37, 8.85 later) | 6.59, 6.12 (6.75, 6.98 later) | −21% to −26% |
| Oscillator slot, TriSaw 12 kHz, settled | 34.31 (38.47, 36.28 later) | 32.87, 34.77 (36.28, 34.65 later) | within noise |
| Oscillator slot, 32 saws into one bus (32 one-voice slots) | 119.17 (120.59, 120.37 later) | 105.08, 106.49 (107.25, 113.67 later) | −6% to −12%: layout, see below |

**Unison**, from the same two runs:

| Unison slot, saw | 440 Hz (ns / sample) | per voice | 12 kHz (ns / sample) | per voice |
|---|---:|---:|---:|---:|
| 1 voice | 3.30, 3.69 | 3.3–3.7 | 13.17, 13.45 | 13.2–13.5 |
| 2 voices | 6.76, 6.60 | 3.3–3.4 | 37.06, 27.57 | 13.8–18.5 |
| 4 voices | 13.32, 13.90 | 3.3–3.5 | 52.92, 58.61 | 13.2–14.7 |
| 8 voices | 25.11, 28.57 | 3.1–3.6 | 105.64, 115.26 | 13.2–14.4 |

| Other unison rows | ns / sample |
|---|---:|
| 8 voices, 440 Hz, pitch per sample | 36.35, 39.89 |
| 0 voices | 0.13, 0.15 |
| Noise, 8 voices, Stereo 0.5 | 2.23, 2.42 |
| 32-voice note (4 slots x 8 voices, 110–880 Hz) into one bus | 101.79, 105.31 |

### Observations

- **Unison costs one oscillator plus its gain per voice.** About 3.1–3.7 ns per voice at 440 Hz and 13–15 ns at 12 kHz, the same per voice as a one-voice slot: nothing is shared between voices yet (Phase 3). A whole 32-voice note, 4 oscillators of 8 voices with their stereo gains, costs 102–105 ns per sample: 0.5% of a core. That is within 10% of 32 bare mono oscillators (92–93 ns), and below 32 one-voice slots (105–114 ns).
- **0 voices costs nothing measurable:** 70 ns per block, the pitch conversion and the scan of 8 voice states. **Noise** with 8 voices costs 2.2–2.4 ns per sample: two draws and four multiply-adds, whatever the voice count.
- **A pitch per sample got cheaper.** The slot now converts a chunk's 64 centre frequencies in one loop before any voice renders, instead of between oscillator samples: 6.1–7.0 ns against 8.3–8.9 for the parent. With 8 voices it adds about 11 ns per sample (one `exp2` and 8 multiplies and `incrementFor`s), not 8 `exp2`.
- **The parent's volume-ramping row is a harness artefact.** In this session it measured 17.8–18.5 ns in every run, where the same code recorded 4.59 ns at its own commit (below). A standalone probe of exactly that loop (clang `-O2`, the minimum of 200 runs of 100 blocks) gives the parent 4.17 ns and this commit 4.72 ns, ramping, and 3.16 against 3.28 ns settled. So a one-voice slot is about 4% slower settled and 13% slower while a gain ramp runs: the voice loop's state checks and the gain's call through the array. The ramp lasts 10 ms per control change. The bus is not the cause: the probe gives the same numbers whether or not it is cleared between blocks.
- **The 32 one-voice slots are faster, by layout.** Each slot grew from 680 bytes to about 5 KB, so the benchmark's vector of 32 slots is laid out differently. The 680-byte layout was already shown to swing this row by 17% (the entry below). The unison rows' slots are allocated once, in an array.

Full run (the second of the two):

| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz, bare oscillator (mono) x512 | 1,569 | 3.07 | 0.015 |
| minBLEP 64-tap, saw 440 Hz, bare oscillator + inline stereo gain x512 | 1,758 | 3.43 | 0.016 |
| Oscillator slot, saw 440 Hz, settled x512 | 1,798 | 3.51 | 0.017 |
| Oscillator slot, saw 440 Hz, volume ramping x512 | 2,428 | 4.74 | 0.023 |
| minBLEP 64-tap, saw 440 Hz, noteToFrequency every sample + inline stereo gain x512 | 4,624 | 9.03 | 0.043 |
| Oscillator slot, saw 440 Hz, pitch per sample x512 | 3,134 | 6.12 | 0.029 |
| minBLEP 64-tap, TriSaw 12 kHz, bare oscillator + inline stereo gain x512 | 17,240 | 33.67 | 0.162 |
| Oscillator slot, TriSaw 12 kHz, settled x512 | 17,804 | 34.77 | 0.167 |
| Oscillator slot, 32 saws (4 osc x 8 unison) into one bus x512 | 54,525 | 106.49 | 0.511 |
| PolyBLEP 2-point, saw 440 Hz x512 | 1,653 | 3.23 | 0.015 |
| PolyBLEP 2-point, saw 5 kHz x512 | 1,870 | 3.65 | 0.018 |
| PolyBLEP 2-point, saw 12 kHz x512 | 1,727 | 3.37 | 0.016 |
| PolyBLEP 2-point, square 440 Hz x512 | 2,623 | 5.12 | 0.025 |
| PolyBLEP 2-point, square 5 kHz x512 | 2,878 | 5.62 | 0.027 |
| PolyBLEP 2-point, square 12 kHz x512 | 2,937 | 5.74 | 0.028 |
| PolyBLEP 2-point, HPulse 440 Hz x512 | 2,645 | 5.17 | 0.025 |
| PolyBLEP 2-point, HPulse 12 kHz x512 | 2,743 | 5.36 | 0.026 |
| PolyBLEP 2-point, QPulse 440 Hz x512 | 2,681 | 5.24 | 0.025 |
| PolyBLEP 2-point, QPulse 12 kHz x512 | 2,915 | 5.69 | 0.027 |
| PolyBLEP 2-point, triangle 440 Hz x512 | 2,621 | 5.12 | 0.025 |
| PolyBLEP 2-point, triangle 12 kHz x512 | 2,833 | 5.53 | 0.027 |
| PolyBLEP 2-point, sine 440 Hz x512 | 2,372 | 4.63 | 0.022 |
| PolyBLEP 2-point, sine 12 kHz x512 | 2,144 | 4.19 | 0.020 |
| PolyBLEP 2-point, TriSaw 440 Hz x512 | 2,824 | 5.51 | 0.026 |
| PolyBLEP 2-point, TriSaw 12 kHz x512 | 3,138 | 6.13 | 0.029 |
| PolyBLEP 2-point, saw 440 Hz, pitch set every sample x512 | 2,396 | 4.68 | 0.022 |
| PolyBLEP 2-point, 32 saws (4 osc x 8 unison) x512 | 52,509 | 102.56 | 0.492 |
| PolyBLEP 4-point, saw 440 Hz x512 | 2,464 | 4.81 | 0.023 |
| PolyBLEP 4-point, saw 5 kHz x512 | 2,822 | 5.51 | 0.026 |
| PolyBLEP 4-point, saw 12 kHz x512 | 2,780 | 5.43 | 0.026 |
| PolyBLEP 4-point, square 440 Hz x512 | 3,303 | 6.45 | 0.031 |
| PolyBLEP 4-point, square 5 kHz x512 | 3,884 | 7.59 | 0.036 |
| PolyBLEP 4-point, square 12 kHz x512 | 4,051 | 7.91 | 0.038 |
| PolyBLEP 4-point, HPulse 440 Hz x512 | 3,354 | 6.55 | 0.031 |
| PolyBLEP 4-point, HPulse 12 kHz x512 | 4,199 | 8.20 | 0.039 |
| PolyBLEP 4-point, QPulse 440 Hz x512 | 3,378 | 6.60 | 0.032 |
| PolyBLEP 4-point, QPulse 12 kHz x512 | 4,288 | 8.37 | 0.040 |
| PolyBLEP 4-point, triangle 440 Hz x512 | 4,108 | 8.02 | 0.039 |
| PolyBLEP 4-point, triangle 12 kHz x512 | 4,940 | 9.65 | 0.046 |
| PolyBLEP 4-point, sine 440 Hz x512 | 2,773 | 5.42 | 0.026 |
| PolyBLEP 4-point, sine 12 kHz x512 | 2,429 | 4.74 | 0.023 |
| PolyBLEP 4-point, TriSaw 440 Hz x512 | 4,446 | 8.68 | 0.042 |
| PolyBLEP 4-point, TriSaw 12 kHz x512 | 6,302 | 12.31 | 0.059 |
| PolyBLEP 4-point, saw 440 Hz, pitch set every sample x512 | 3,419 | 6.68 | 0.032 |
| PolyBLEP 4-point, 32 saws (4 osc x 8 unison) x512 | 86,072 | 168.11 | 0.807 |
| minBLEP 32-tap, saw 440 Hz x512 | 1,545 | 3.02 | 0.014 |
| minBLEP 32-tap, saw 5 kHz x512 | 2,894 | 5.65 | 0.027 |
| minBLEP 32-tap, saw 12 kHz x512 | 4,775 | 9.33 | 0.045 |
| minBLEP 32-tap, square 440 Hz x512 | 1,778 | 3.47 | 0.017 |
| minBLEP 32-tap, square 5 kHz x512 | 4,309 | 8.42 | 0.040 |
| minBLEP 32-tap, square 12 kHz x512 | 8,332 | 16.27 | 0.078 |
| minBLEP 32-tap, HPulse 440 Hz x512 | 1,631 | 3.19 | 0.015 |
| minBLEP 32-tap, HPulse 12 kHz x512 | 8,739 | 17.07 | 0.082 |
| minBLEP 32-tap, QPulse 440 Hz x512 | 1,775 | 3.47 | 0.017 |
| minBLEP 32-tap, QPulse 12 kHz x512 | 8,419 | 16.44 | 0.079 |
| minBLEP 32-tap, triangle 440 Hz x512 | 1,677 | 3.28 | 0.016 |
| minBLEP 32-tap, triangle 12 kHz x512 | 8,548 | 16.69 | 0.080 |
| minBLEP 32-tap, sine 440 Hz x512 | 4,394 | 8.58 | 0.041 |
| minBLEP 32-tap, sine 12 kHz x512 | 3,734 | 7.29 | 0.035 |
| minBLEP 32-tap, TriSaw 440 Hz x512 | 1,875 | 3.66 | 0.018 |
| minBLEP 32-tap, TriSaw 12 kHz x512 | 10,824 | 21.14 | 0.101 |
| minBLEP 32-tap, saw 440 Hz, pitch set every sample x512 | 2,306 | 4.50 | 0.022 |
| minBLEP 32-tap, 32 saws (4 osc x 8 unison) x512 | 49,356 | 96.40 | 0.463 |
| minBLEP 64-tap, saw 440 Hz x512 | 1,683 | 3.29 | 0.016 |
| minBLEP 64-tap, saw 5 kHz x512 | 3,684 | 7.20 | 0.035 |
| minBLEP 64-tap, saw 12 kHz x512 | 6,430 | 12.56 | 0.060 |
| minBLEP 64-tap, square 440 Hz x512 | 1,798 | 3.51 | 0.017 |
| minBLEP 64-tap, square 5 kHz x512 | 6,055 | 11.83 | 0.057 |
| minBLEP 64-tap, square 12 kHz x512 | 11,942 | 23.32 | 0.112 |
| minBLEP 64-tap, HPulse 440 Hz x512 | 1,866 | 3.64 | 0.017 |
| minBLEP 64-tap, HPulse 12 kHz x512 | 12,045 | 23.52 | 0.113 |
| minBLEP 64-tap, QPulse 440 Hz x512 | 1,837 | 3.59 | 0.017 |
| minBLEP 64-tap, QPulse 12 kHz x512 | 11,970 | 23.38 | 0.112 |
| minBLEP 64-tap, triangle 440 Hz x512 | 1,857 | 3.63 | 0.017 |
| minBLEP 64-tap, triangle 12 kHz x512 | 12,959 | 25.31 | 0.121 |
| minBLEP 64-tap, sine 440 Hz x512 | 4,280 | 8.36 | 0.040 |
| minBLEP 64-tap, sine 12 kHz x512 | 3,809 | 7.44 | 0.036 |
| minBLEP 64-tap, TriSaw 440 Hz x512 | 2,108 | 4.12 | 0.020 |
| minBLEP 64-tap, TriSaw 12 kHz x512 | 17,598 | 34.37 | 0.165 |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample x512 | 2,231 | 4.36 | 0.021 |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) x512 | 47,334 | 92.45 | 0.444 |
| Unison slot, saw 440 Hz, 1 voice x512 | 1,892 | 3.69 | 0.018 |
| Unison slot, saw 440 Hz, 2 voices x512 | 3,381 | 6.60 | 0.032 |
| Unison slot, saw 440 Hz, 4 voices x512 | 7,117 | 13.90 | 0.067 |
| Unison slot, saw 440 Hz, 8 voices x512 | 14,628 | 28.57 | 0.137 |
| Unison slot, saw 12 kHz, 1 voice x512 | 6,888 | 13.45 | 0.065 |
| Unison slot, saw 12 kHz, 2 voices x512 | 14,114 | 27.57 | 0.132 |
| Unison slot, saw 12 kHz, 4 voices x512 | 30,008 | 58.61 | 0.281 |
| Unison slot, saw 12 kHz, 8 voices x512 | 59,013 | 115.26 | 0.553 |
| Unison slot, saw 440 Hz, 8 voices, pitch per sample x512 | 20,425 | 39.89 | 0.191 |
| Unison slot, 0 voices x512 | 77 | 0.15 | 0.001 |
| Unison slot, noise, 8 voices x512 | 1,241 | 2.42 | 0.012 |
| Unison: 32-voice note (4 slots x 8 voices) into one bus x512 | 53,918 | 105.31 | 0.505 |
| minBLEP 64-tap, saw 440 Hz, retrigger every 64 samples x512 | 1,781 | 3.48 | 0.017 |
| minBLEP 64-tap, square 440 Hz, retrigger every 64 samples x512 | 2,109 | 4.12 | 0.020 |
| minBLEP 64-tap, triangle 440 Hz, retrigger every 64 samples x512 | 2,267 | 4.43 | 0.021 |
| minBLEP 64-tap, sine 440 Hz, retrigger every 64 samples x512 | 4,739 | 9.26 | 0.044 |
| minBLEP 64-tap, TriSaw 440 Hz, retrigger every 64 samples x512 | 2,127 | 4.15 | 0.020 |
| minBLEP 64-tap, saw 1 kHz synced to a 1.09 kHz master x512 | 2,475 | 4.83 | 0.023 |
| Oscillator slot, saw 440 Hz, retriggered every block x512 | 1,821 | 3.56 | 0.017 |

---

## 2026-11-14: M1.1 retrigger and phase

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark, on the working tree of the commit that adds Phase and Retrig (it reports its parent, `0fa1524`, as modified).
- **What changed:** `MinBlepOscillator` gained `restart(phase)` and the band-limited `jumpTo(phase, since)` ([oscillator-phase.md](../dsp/oscillator-phase.md)). The per-sample fast path is unchanged. The corner scan of the slow path is now shared with the jump (it takes a span and a time offset), and the sine's phase history records each sample's step and any jump. `OscillatorSlot` gained Phase, Retrig, a `Random`, and `noteOn()`. New rows: each waveform at 440 Hz retriggered every 64 samples, a saw hard-synced at fractional times, and a slot retriggered every block.
- **Before and after, same session:** this tree, then the parent (exported with `git archive`, `external/` symlinked, built `relwithdebinfo`), then this tree again; the slot rows then three more times, alternating.

| Benchmark | parent (ns / sample) | this commit (two runs) | change |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz | 2.96 | 2.98, 2.97 | 0% to +1% |
| minBLEP 64-tap, square 440 Hz | 3.42 | 3.41, 3.44 | 0% to +1% |
| minBLEP 64-tap, triangle 440 Hz | 3.42 | 3.50, 3.46 | +1% to +2% |
| minBLEP 64-tap, sine 440 Hz | 8.00 | 8.15, 8.22 | +2% to +3% |
| minBLEP 64-tap, saw 12 kHz | 11.99 | 12.63, 12.33 | +3% to +5% |
| minBLEP 64-tap, square 12 kHz | 22.15 | 23.17, 23.06 | +4% to +5% |
| minBLEP 64-tap, TriSaw 12 kHz | 30.95 | 32.96, 32.43 | +5% to +6% |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample | 4.09 | 4.20, 4.09 | 0% to +3% |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) | 94.39 | 91.23, 91.04 | −3% to −4% |
| Oscillator slot, saw 440 Hz, settled | 3.10 (3.12–3.30 later) | 3.25, 3.16 (3.16–3.25 later) | within noise |
| Oscillator slot, TriSaw 12 kHz, settled | 31.06 (31.99–33.36 later) | 33.41, 33.41 (33.38–36.21 later) | +3% to +8% |
| Oscillator slot, 32 saws into one bus | 99.87 (99.55–104.44 later) | 117.83, 116.21 (116.28–120.03 later) | +15% to +20%: a layout artefact, see below |

**The cost of a jump**, from each retrigger row against its twin without jumps in the same run (8 jumps per 512-sample block):

| Waveform, 440 Hz | Without jumps | Retriggered every 64 samples | Per jump |
|---|---:|---:|---:|
| Saw | 2.98, 2.97 | 3.10, 3.17 | 8–13 ns |
| Square | 3.41, 3.44 | 3.84, 3.77 | 21–27 ns |
| Triangle | 3.50, 3.46 | 3.98, 3.95 | 31 ns |
| Sine | 8.15, 8.22 | 8.80, 8.73 | 33–42 ns |
| TriSaw | 3.79, 3.83 | 4.25, 3.96 | 8–30 ns |

| Other rows | ns / sample |
|---|---:|
| minBLEP 64-tap, saw 1 kHz synced to a 1.09 kHz master (a jump every 44 samples, between samples) | 4.71, 4.67 |
| Oscillator slot, saw 440 Hz, retriggered every block | 3.11, 3.11 (settled: 3.25, 3.16) |

### Observations

- **The per-sample path costs what it did.** Every oscillator row at 440 Hz is within 0–3% of the parent, and the 32-oscillator bank is 3–4% faster. Rows dominated by the slow path (12 kHz) are 2–6% slower in both runs. The shared corner scan adds a time offset to each crossing's sub-sample time, and the sine's history now writes 16 bytes per sample instead of 4.
- **A jump costs about the residual adds it makes:** 8–13 ns for the saw (one step residual, 64 taps), 20–30 ns with a bend as well, and 33–42 ns for the sine, which also evaluates sin and cos twice. One retrigger per note is invisible: a slot retriggered every block measures the same as a settled one. As hard sync (Part 2), a jump every 44 samples puts a 1 kHz saw at 4.7 ns per sample.
- **The slot bank's +15–20% is memory layout, not code.** A single slot is unchanged, the bare 32-oscillator bank is faster, and the extra state (the history's wider entries, `Random`) is never touched by a saw's per-sample loop. A probe outside the harness (32 slots, minimum of 200 runs, clang `-O2`) puts this commit's bank at 106 ns per sample, whatever padding is added to each slot. The parent's bank measures 107 ns with 16–240 bytes of padding and 115–125 ns without padding or with 500 bytes. So the bank's cost moves by up to 17% with nothing but where the slots fall in memory: the benchmark's vector grows by `push_back`, and `sizeof(OscillatorSlot)` went from 536 to 680 bytes. Placing per-voice state deliberately (alignment, one allocation per voice) is a Phase 3 item, and the unison task should reserve its oscillator storage up front.

Full run (the second of the two):

| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz, bare oscillator (mono) x512 | 1,543 | 3.01 | 0.014 |
| minBLEP 64-tap, saw 440 Hz, bare oscillator + inline stereo gain x512 | 1,719 | 3.36 | 0.016 |
| Oscillator slot, saw 440 Hz, settled x512 | 1,617 | 3.16 | 0.015 |
| Oscillator slot, saw 440 Hz, volume ramping x512 | 2,349 | 4.59 | 0.022 |
| minBLEP 64-tap, saw 440 Hz, noteToFrequency every sample + inline stereo gain x512 | 4,584 | 8.95 | 0.043 |
| Oscillator slot, saw 440 Hz, pitch per sample x512 | 4,140 | 8.09 | 0.039 |
| minBLEP 64-tap, TriSaw 12 kHz, bare oscillator + inline stereo gain x512 | 17,423 | 34.03 | 0.163 |
| Oscillator slot, TriSaw 12 kHz, settled x512 | 17,105 | 33.41 | 0.160 |
| Oscillator slot, 32 saws (4 osc x 8 unison) into one bus x512 | 59,498 | 116.21 | 0.558 |
| PolyBLEP 2-point, saw 440 Hz x512 | 1,639 | 3.20 | 0.015 |
| PolyBLEP 2-point, saw 5 kHz x512 | 1,812 | 3.54 | 0.017 |
| PolyBLEP 2-point, saw 12 kHz x512 | 1,680 | 3.28 | 0.016 |
| PolyBLEP 2-point, square 440 Hz x512 | 2,555 | 4.99 | 0.024 |
| PolyBLEP 2-point, square 5 kHz x512 | 2,873 | 5.61 | 0.027 |
| PolyBLEP 2-point, square 12 kHz x512 | 2,709 | 5.29 | 0.025 |
| PolyBLEP 2-point, HPulse 440 Hz x512 | 2,592 | 5.06 | 0.024 |
| PolyBLEP 2-point, HPulse 12 kHz x512 | 2,664 | 5.20 | 0.025 |
| PolyBLEP 2-point, QPulse 440 Hz x512 | 2,612 | 5.10 | 0.024 |
| PolyBLEP 2-point, QPulse 12 kHz x512 | 2,763 | 5.40 | 0.026 |
| PolyBLEP 2-point, triangle 440 Hz x512 | 2,562 | 5.00 | 0.024 |
| PolyBLEP 2-point, triangle 12 kHz x512 | 2,792 | 5.45 | 0.026 |
| PolyBLEP 2-point, sine 440 Hz x512 | 2,225 | 4.35 | 0.021 |
| PolyBLEP 2-point, sine 12 kHz x512 | 2,080 | 4.06 | 0.019 |
| PolyBLEP 2-point, TriSaw 440 Hz x512 | 2,745 | 5.36 | 0.026 |
| PolyBLEP 2-point, TriSaw 12 kHz x512 | 3,031 | 5.92 | 0.028 |
| PolyBLEP 2-point, saw 440 Hz, pitch set every sample x512 | 2,326 | 4.54 | 0.022 |
| PolyBLEP 2-point, 32 saws (4 osc x 8 unison) x512 | 52,447 | 102.44 | 0.492 |
| PolyBLEP 4-point, saw 440 Hz x512 | 2,440 | 4.76 | 0.023 |
| PolyBLEP 4-point, saw 5 kHz x512 | 2,667 | 5.21 | 0.025 |
| PolyBLEP 4-point, saw 12 kHz x512 | 2,614 | 5.11 | 0.025 |
| PolyBLEP 4-point, square 440 Hz x512 | 3,180 | 6.21 | 0.030 |
| PolyBLEP 4-point, square 5 kHz x512 | 3,646 | 7.12 | 0.034 |
| PolyBLEP 4-point, square 12 kHz x512 | 3,787 | 7.40 | 0.036 |
| PolyBLEP 4-point, HPulse 440 Hz x512 | 3,165 | 6.18 | 0.030 |
| PolyBLEP 4-point, HPulse 12 kHz x512 | 3,732 | 7.29 | 0.035 |
| PolyBLEP 4-point, QPulse 440 Hz x512 | 3,214 | 6.28 | 0.030 |
| PolyBLEP 4-point, QPulse 12 kHz x512 | 4,011 | 7.83 | 0.038 |
| PolyBLEP 4-point, triangle 440 Hz x512 | 4,022 | 7.86 | 0.038 |
| PolyBLEP 4-point, triangle 12 kHz x512 | 6,181 | 12.07 | 0.058 |
| PolyBLEP 4-point, sine 440 Hz x512 | 2,619 | 5.12 | 0.025 |
| PolyBLEP 4-point, sine 12 kHz x512 | 2,188 | 4.27 | 0.021 |
| PolyBLEP 4-point, TriSaw 440 Hz x512 | 4,127 | 8.06 | 0.039 |
| PolyBLEP 4-point, TriSaw 12 kHz x512 | 5,480 | 10.70 | 0.051 |
| PolyBLEP 4-point, saw 440 Hz, pitch set every sample x512 | 3,267 | 6.38 | 0.031 |
| PolyBLEP 4-point, 32 saws (4 osc x 8 unison) x512 | 77,894 | 152.14 | 0.730 |
| minBLEP 32-tap, saw 440 Hz x512 | 1,455 | 2.84 | 0.014 |
| minBLEP 32-tap, saw 5 kHz x512 | 2,743 | 5.36 | 0.026 |
| minBLEP 32-tap, saw 12 kHz x512 | 4,351 | 8.50 | 0.041 |
| minBLEP 32-tap, square 440 Hz x512 | 1,617 | 3.16 | 0.015 |
| minBLEP 32-tap, square 5 kHz x512 | 4,256 | 8.31 | 0.040 |
| minBLEP 32-tap, square 12 kHz x512 | 7,852 | 15.34 | 0.074 |
| minBLEP 32-tap, HPulse 440 Hz x512 | 1,636 | 3.20 | 0.015 |
| minBLEP 32-tap, HPulse 12 kHz x512 | 7,866 | 15.36 | 0.074 |
| minBLEP 32-tap, QPulse 440 Hz x512 | 1,600 | 3.12 | 0.015 |
| minBLEP 32-tap, QPulse 12 kHz x512 | 7,951 | 15.53 | 0.075 |
| minBLEP 32-tap, triangle 440 Hz x512 | 1,603 | 3.13 | 0.015 |
| minBLEP 32-tap, triangle 12 kHz x512 | 7,911 | 15.45 | 0.074 |
| minBLEP 32-tap, sine 440 Hz x512 | 4,206 | 8.21 | 0.039 |
| minBLEP 32-tap, sine 12 kHz x512 | 3,638 | 7.11 | 0.034 |
| minBLEP 32-tap, TriSaw 440 Hz x512 | 1,707 | 3.33 | 0.016 |
| minBLEP 32-tap, TriSaw 12 kHz x512 | 10,627 | 20.76 | 0.100 |
| minBLEP 32-tap, saw 440 Hz, pitch set every sample x512 | 2,045 | 3.99 | 0.019 |
| minBLEP 32-tap, 32 saws (4 osc x 8 unison) x512 | 47,044 | 91.88 | 0.441 |
| minBLEP 64-tap, saw 440 Hz x512 | 1,519 | 2.97 | 0.014 |
| minBLEP 64-tap, saw 5 kHz x512 | 3,562 | 6.96 | 0.033 |
| minBLEP 64-tap, saw 12 kHz x512 | 6,315 | 12.33 | 0.059 |
| minBLEP 64-tap, square 440 Hz x512 | 1,762 | 3.44 | 0.017 |
| minBLEP 64-tap, square 5 kHz x512 | 5,788 | 11.30 | 0.054 |
| minBLEP 64-tap, square 12 kHz x512 | 11,806 | 23.06 | 0.111 |
| minBLEP 64-tap, HPulse 440 Hz x512 | 1,810 | 3.53 | 0.017 |
| minBLEP 64-tap, HPulse 12 kHz x512 | 11,730 | 22.91 | 0.110 |
| minBLEP 64-tap, QPulse 440 Hz x512 | 1,724 | 3.37 | 0.016 |
| minBLEP 64-tap, QPulse 12 kHz x512 | 11,683 | 22.82 | 0.110 |
| minBLEP 64-tap, triangle 440 Hz x512 | 1,774 | 3.46 | 0.017 |
| minBLEP 64-tap, triangle 12 kHz x512 | 11,874 | 23.19 | 0.111 |
| minBLEP 64-tap, sine 440 Hz x512 | 4,208 | 8.22 | 0.039 |
| minBLEP 64-tap, sine 12 kHz x512 | 3,628 | 7.09 | 0.034 |
| minBLEP 64-tap, TriSaw 440 Hz x512 | 1,960 | 3.83 | 0.018 |
| minBLEP 64-tap, TriSaw 12 kHz x512 | 16,603 | 32.43 | 0.156 |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample x512 | 2,092 | 4.09 | 0.020 |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) x512 | 46,615 | 91.04 | 0.437 |
| minBLEP 64-tap, saw 440 Hz, retrigger every 64 samples x512 | 1,624 | 3.17 | 0.015 |
| minBLEP 64-tap, square 440 Hz, retrigger every 64 samples x512 | 1,931 | 3.77 | 0.018 |
| minBLEP 64-tap, triangle 440 Hz, retrigger every 64 samples x512 | 2,024 | 3.95 | 0.019 |
| minBLEP 64-tap, sine 440 Hz, retrigger every 64 samples x512 | 4,470 | 8.73 | 0.042 |
| minBLEP 64-tap, TriSaw 440 Hz, retrigger every 64 samples x512 | 2,026 | 3.96 | 0.019 |
| minBLEP 64-tap, saw 1 kHz synced to a 1.09 kHz master x512 | 2,393 | 4.67 | 0.022 |
| Oscillator slot, saw 440 Hz, retriggered every block x512 | 1,593 | 3.11 | 0.015 |

---

## 2026-11-13: M1.1 oscillator controls (tuning, volume, pan, invert)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark, on the working tree of the commit that adds the controls (it reports its parent, `1fae0ad`, as modified).
- **What changed:** a new layer, `OscillatorSlot` ([oscillator-controls.md](../dsp/oscillator-controls.md)), around an unchanged oscillator (it only gained a `phaseIncrement()` getter). New rows: the slot settled, mid-ramp, with a pitch per sample, at 12 kHz, and as a 32-oscillator bank; and in the same benchmark case, the bare oscillator at exactly the slot's pitch, both as it is (mono) and with the same stereo gain written inline. The inline version is the fair baseline, because the slot must produce two channels.
- **Before and after, same session:** the parent commit (exported with `git archive`, `external/` symlinked, built `relwithdebinfo`) was benchmarked straight after this run, and once earlier in the session. Every oscillator row is within −3% to +5% of the parent's, which is within noise.

| Benchmark | parent (ns / sample) | this commit | change |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz | 3.20 | 3.16 | −1% |
| minBLEP 64-tap, square 12 kHz | 22.54 | 23.49 | +4% |
| minBLEP 64-tap, sine 440 Hz | 8.10 | 8.48 | +5% |
| minBLEP 64-tap, TriSaw 12 kHz | 32.49 | 32.63 | 0% |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample | 4.19 | 4.34 | +4% |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) | 93.50 | 96.75 | +3% |

**The slot against the bare oscillator**, same run (and the range over three runs in this session):

| Case | Bare oscillator, mono out | Bare + the same stereo gain inline | Oscillator slot |
|---|---:|---:|---:|
| Saw 440 Hz, settled | 3.10 (3.04–3.26) | 3.53 (3.53–4.01) | 3.40 (3.24–3.43) |
| Saw 440 Hz, volume ramping | | | 4.65 (4.65–5.14) |
| Saw 440 Hz, pitch per sample (one `noteToFrequency` each) | | 9.51 (9.15–9.51) | 9.13 (8.39–9.13) |
| TriSaw 12 kHz, settled | | 33.93 (32.63–33.93) | 32.44 (32.10–33.58) |
| 32 saws into one bus | 96.75 (mono sum, row above) | | 105.13 (103.99–112.73) |

- **At ordinary pitches the slot costs about 0.2–0.3 ns per sample over the bare mono oscillator** (+6–10%), for two output channels with volume, pan, and polarity. It is **cheaper than writing the same stereo gain inline** (3.40 against 3.53 ns). The slot stages 64 samples on the stack and then applies the gains in a separate loop, which clang vectorizes (width 4, interleaved twice: `-Rpass=loop-vectorize` on `stereo_gain.hpp`). The inline loop is tied to the oscillator's sample-by-sample state and is not vectorized.
- **A volume ramp adds about 1.3–1.7 ns per sample** while it lasts (10 ms): two `LinearSmoother` steps, about 0.8 ns each in the M1.0 baseline, in a loop that cannot vectorize because each step advances the smoother.
- **A pitch per sample costs about 6 ns more than a pitch per block**, almost all of it the `std::exp2` in `noteToFrequency` (the bare oscillator's "pitch set every sample" row uses precomputed frequencies). `fastExp2` (3·10⁻⁷ relative, 5·10⁻⁴ cents) would save most of it. A Phase 3 candidate: it would make the per-sample path differ from the per-block one by that much.
- **512 voices** of the settled slot would take 512 × 3.40 = 1,741 ns of every 20,833 ns sample period at 48 kHz: 8.4% of a core, against 7.6% for bare mono oscillators.

Full run:

| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz, bare oscillator (mono) x512 | 1,589 | 3.10 | 0.015 |
| minBLEP 64-tap, saw 440 Hz, bare oscillator + inline stereo gain x512 | 1,805 | 3.53 | 0.017 |
| Oscillator slot, saw 440 Hz, settled x512 | 1,742 | 3.40 | 0.016 |
| Oscillator slot, saw 440 Hz, volume ramping x512 | 2,380 | 4.65 | 0.022 |
| minBLEP 64-tap, saw 440 Hz, noteToFrequency every sample + inline stereo gain x512 | 4,869 | 9.51 | 0.046 |
| Oscillator slot, saw 440 Hz, pitch per sample x512 | 4,676 | 9.13 | 0.044 |
| minBLEP 64-tap, TriSaw 12 kHz, bare oscillator + inline stereo gain x512 | 17,374 | 33.93 | 0.163 |
| Oscillator slot, TriSaw 12 kHz, settled x512 | 16,612 | 32.44 | 0.156 |
| Oscillator slot, 32 saws (4 osc x 8 unison) into one bus x512 | 53,826 | 105.13 | 0.505 |
| PolyBLEP 2-point, saw 440 Hz x512 | 1,736 | 3.39 | 0.016 |
| PolyBLEP 2-point, saw 5 kHz x512 | 1,823 | 3.56 | 0.017 |
| PolyBLEP 2-point, saw 12 kHz x512 | 1,724 | 3.37 | 0.016 |
| PolyBLEP 2-point, square 440 Hz x512 | 2,621 | 5.12 | 0.025 |
| PolyBLEP 2-point, square 5 kHz x512 | 3,020 | 5.90 | 0.028 |
| PolyBLEP 2-point, square 12 kHz x512 | 2,887 | 5.64 | 0.027 |
| PolyBLEP 2-point, HPulse 440 Hz x512 | 2,619 | 5.11 | 0.025 |
| PolyBLEP 2-point, HPulse 12 kHz x512 | 2,870 | 5.61 | 0.027 |
| PolyBLEP 2-point, QPulse 440 Hz x512 | 2,809 | 5.49 | 0.026 |
| PolyBLEP 2-point, QPulse 12 kHz x512 | 2,834 | 5.53 | 0.027 |
| PolyBLEP 2-point, triangle 440 Hz x512 | 2,908 | 5.68 | 0.027 |
| PolyBLEP 2-point, triangle 12 kHz x512 | 2,977 | 5.81 | 0.028 |
| PolyBLEP 2-point, sine 440 Hz x512 | 2,382 | 4.65 | 0.022 |
| PolyBLEP 2-point, sine 12 kHz x512 | 2,026 | 3.96 | 0.019 |
| PolyBLEP 2-point, TriSaw 440 Hz x512 | 3,588 | 7.01 | 0.034 |
| PolyBLEP 2-point, TriSaw 12 kHz x512 | 3,251 | 6.35 | 0.030 |
| PolyBLEP 2-point, saw 440 Hz, pitch set every sample x512 | 2,998 | 5.86 | 0.028 |
| PolyBLEP 2-point, 32 saws (4 osc x 8 unison) x512 | 54,680 | 106.80 | 0.513 |
| PolyBLEP 4-point, saw 440 Hz x512 | 2,439 | 4.76 | 0.023 |
| PolyBLEP 4-point, saw 5 kHz x512 | 2,893 | 5.65 | 0.027 |
| PolyBLEP 4-point, saw 12 kHz x512 | 3,199 | 6.25 | 0.030 |
| PolyBLEP 4-point, square 440 Hz x512 | 3,509 | 6.85 | 0.033 |
| PolyBLEP 4-point, square 5 kHz x512 | 3,904 | 7.62 | 0.037 |
| PolyBLEP 4-point, square 12 kHz x512 | 3,767 | 7.36 | 0.035 |
| PolyBLEP 4-point, HPulse 440 Hz x512 | 3,217 | 6.28 | 0.030 |
| PolyBLEP 4-point, HPulse 12 kHz x512 | 3,857 | 7.53 | 0.036 |
| PolyBLEP 4-point, QPulse 440 Hz x512 | 4,499 | 8.79 | 0.042 |
| PolyBLEP 4-point, QPulse 12 kHz x512 | 4,124 | 8.06 | 0.039 |
| PolyBLEP 4-point, triangle 440 Hz x512 | 4,001 | 7.81 | 0.038 |
| PolyBLEP 4-point, triangle 12 kHz x512 | 5,156 | 10.07 | 0.048 |
| PolyBLEP 4-point, sine 440 Hz x512 | 2,759 | 5.39 | 0.026 |
| PolyBLEP 4-point, sine 12 kHz x512 | 2,388 | 4.66 | 0.022 |
| PolyBLEP 4-point, TriSaw 440 Hz x512 | 4,384 | 8.56 | 0.041 |
| PolyBLEP 4-point, TriSaw 12 kHz x512 | 5,604 | 10.95 | 0.053 |
| PolyBLEP 4-point, saw 440 Hz, pitch set every sample x512 | 3,336 | 6.52 | 0.031 |
| PolyBLEP 4-point, 32 saws (4 osc x 8 unison) x512 | 82,671 | 161.47 | 0.775 |
| minBLEP 32-tap, saw 440 Hz x512 | 1,543 | 3.01 | 0.014 |
| minBLEP 32-tap, saw 5 kHz x512 | 2,661 | 5.20 | 0.025 |
| minBLEP 32-tap, saw 12 kHz x512 | 4,511 | 8.81 | 0.042 |
| minBLEP 32-tap, square 440 Hz x512 | 1,799 | 3.51 | 0.017 |
| minBLEP 32-tap, square 5 kHz x512 | 4,083 | 7.98 | 0.038 |
| minBLEP 32-tap, square 12 kHz x512 | 7,941 | 15.51 | 0.074 |
| minBLEP 32-tap, HPulse 440 Hz x512 | 1,691 | 3.30 | 0.016 |
| minBLEP 32-tap, HPulse 12 kHz x512 | 7,869 | 15.37 | 0.074 |
| minBLEP 32-tap, QPulse 440 Hz x512 | 1,669 | 3.26 | 0.016 |
| minBLEP 32-tap, QPulse 12 kHz x512 | 7,950 | 15.53 | 0.075 |
| minBLEP 32-tap, triangle 440 Hz x512 | 1,717 | 3.35 | 0.016 |
| minBLEP 32-tap, triangle 12 kHz x512 | 7,956 | 15.54 | 0.075 |
| minBLEP 32-tap, sine 440 Hz x512 | 4,405 | 8.60 | 0.041 |
| minBLEP 32-tap, sine 12 kHz x512 | 3,739 | 7.30 | 0.035 |
| minBLEP 32-tap, TriSaw 440 Hz x512 | 1,806 | 3.53 | 0.017 |
| minBLEP 32-tap, TriSaw 12 kHz x512 | 9,758 | 19.06 | 0.091 |
| minBLEP 32-tap, saw 440 Hz, pitch set every sample x512 | 2,148 | 4.19 | 0.020 |
| minBLEP 32-tap, 32 saws (4 osc x 8 unison) x512 | 61,712 | 120.53 | 0.579 |
| minBLEP 64-tap, saw 440 Hz x512 | 1,615 | 3.16 | 0.015 |
| minBLEP 64-tap, saw 5 kHz x512 | 3,679 | 7.19 | 0.034 |
| minBLEP 64-tap, saw 12 kHz x512 | 6,483 | 12.66 | 0.061 |
| minBLEP 64-tap, square 440 Hz x512 | 1,838 | 3.59 | 0.017 |
| minBLEP 64-tap, square 5 kHz x512 | 5,935 | 11.59 | 0.056 |
| minBLEP 64-tap, square 12 kHz x512 | 12,027 | 23.49 | 0.113 |
| minBLEP 64-tap, HPulse 440 Hz x512 | 1,853 | 3.62 | 0.017 |
| minBLEP 64-tap, HPulse 12 kHz x512 | 11,489 | 22.44 | 0.108 |
| minBLEP 64-tap, QPulse 440 Hz x512 | 1,838 | 3.59 | 0.017 |
| minBLEP 64-tap, QPulse 12 kHz x512 | 12,084 | 23.60 | 0.113 |
| minBLEP 64-tap, triangle 440 Hz x512 | 1,844 | 3.60 | 0.017 |
| minBLEP 64-tap, triangle 12 kHz x512 | 11,987 | 23.41 | 0.112 |
| minBLEP 64-tap, sine 440 Hz x512 | 4,340 | 8.48 | 0.041 |
| minBLEP 64-tap, sine 12 kHz x512 | 3,758 | 7.34 | 0.035 |
| minBLEP 64-tap, TriSaw 440 Hz x512 | 2,032 | 3.97 | 0.019 |
| minBLEP 64-tap, TriSaw 12 kHz x512 | 16,706 | 32.63 | 0.157 |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample x512 | 2,223 | 4.34 | 0.021 |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) x512 | 49,538 | 96.75 | 0.464 |

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
