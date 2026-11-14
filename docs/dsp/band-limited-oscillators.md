# Band-limited oscillators: choosing the anti-aliasing method (D6)

Sylenth1's oscillators are specified as alias-free from 0.01 Hz to Nyquist, and a note can stack 4 oscillators × 8 unison voices, up to 512 oscillator voices in all. This page explains why a naive digital sawtooth aliases, describes three families of fixes (PolyBLEP, minBLEP, and mipmapped wavetables), and reports how five prototypes of them measured. It ends with the choice: **minBLEP with 64 taps** (decision D6).

## Theory

### Why a naive waveform aliases

A continuous sawtooth has harmonics at every multiple of its fundamental f₀, with amplitudes 2/(πk). They never stop. Computing `2t − 1` once per sample *samples* that continuous signal, and every harmonic above Nyquist (fs/2) folds back to |k·f₀ − m·fs|. These aliases are not harmonics of f₀. They sound like inharmonic grit, and they move the wrong way when the pitch moves. A naive 2.5 kHz saw at 48 kHz has its worst alias at −20 dB relative to the fundamental: the folded 10th harmonic (the harness's own self-test, [measuring-dsp.md](measuring-dsp.md)).

The harmonics fall off as 1/k because of the waveform's *jumps*. A waveform with only *bends* (a triangle) has harmonics falling as 1/k², so it aliases much less but still audibly at high notes. Everything between the edges is a straight line, which contains no high frequencies at all. So the aliasing comes entirely from the edges, and that is where every method below works.

### Band-limited steps (BLEP) and ramps (BLAMP)

Write the waveform as straight segments plus a sum of unit steps H(x) scaled by the jump heights Jₖ, and of unit ramps scaled by the slope changes Kₖ. Low-pass filtering the continuous waveform with a kernel h before sampling it would remove the aliasing. Because filtering is linear, it can be done edge by edge:

- A filtered step is the running integral of h. The **step residual** r(x) = ∫h − H(x) is what must be added to the naive step to turn it into the filtered one. It is short if h is short.
- A filtered ramp is the integral of the filtered step, so the **ramp residual** is the running integral of r.

Then output(n) = naive(n) + Σ Jₖ·r(n − τₖ) + Σ Kₖ·r₂(n − τₖ), where τₖ is the edge's time to sub-sample precision. Two properties follow:

- The output is the waveform filtered by h and then sampled. So the stopband of h sets the aliasing, and its passband sets the high-frequency **droop** of the harmonics we keep.
- The cost is per edge, not per sample, and the correction is exact for any edge timing. That makes pitch changes on every sample (FM, portamento) work without special handling.

### PolyBLEP and PolyBLAMP

Choose h to be a low-order **B-spline**, so the residuals are short polynomials (Välimäki and Huovilainen 2007; Välimäki, Pekonen, and Nam 2012; Esqueda, Välimäki, and Bilbao 2016):

| Kernel | Span | Step residual, d samples from the edge (before: +, after: −) | Response |
|---|---|---|---|
| Linear B-spline (the classic PolyBLEP) | ±1 sample | (1 − d)²/2 | sinc²(f/fs) |
| Cubic B-spline (4-point) | ±2 samples | ½ − 2d/3 + d³/3 − d⁴/8 for d < 1; (2 − d)⁴/24 for d < 2 | sinc⁴(f/fs) |

The residuals need no memory and no tables, and they are zero-phase. But a B-spline is a poor low-pass. sinc² is down only 6 dB at 20 kHz (a 44.1 kHz rate), and it has its nulls at multiples of fs, not across the band just above Nyquist. So harmonics just above Nyquist fold back almost unattenuated. Raising the order buys stopband but costs passband: sinc⁴ is down 12.7 dB at 20 kHz.

### minBLEP and minBLAMP

Brandt (2001) used a proper low-pass instead: a **windowed sinc**, many samples long, with a deep stopband. A linear-phase sinc is symmetric, so its residual starts *before* the edge. A real-time oscillator would then need a lookahead, and with it latency. Brandt's trick is to convert the kernel to **minimum phase**, which keeps the magnitude response but puts all the energy as early as possible, so the residual starts at the edge and only rings afterwards. The conversion uses the real cepstrum: take the log magnitude spectrum, transform it, fold the negative quefrencies onto the positive ones, and exponentiate back (Oppenheim and Schafer, ch. 13).

The residual is tabulated, oversampled (here 64 points per sample), and read with linear interpolation at the edge's fractional position. Each edge adds `Taps` residual samples into a small ring buffer, and each output sample reads and clears one slot.

Minimum phase has two side effects that the implementation must handle:

- **Group delay.** The filtered edges arrive about 2–3 samples late (the kernel's group delay at DC, τ), but the naive straight segments do not. A sloped segment then sits τ·slope too high: a DC offset on the saw that grows with pitch, and a staircase on the triangle. The fix is to subtract τ·slope from the output, and to add τ to the integrated ramp table so it ends at exactly zero. The output is then the waveform filtered by h, delayed by τ, with no offset.
- **Overshoot.** A minimum-phase step delivers its energy at once, so it overshoots more than a linear-phase one: about 37% here, against Gibbs's 9%.

### Mipmapped wavetables

Store one cycle of each waveform, built additively from its Fourier series. Keep a stack of versions ("mipmap levels"), each with fewer harmonics, and at each pitch read the richest level whose top harmonic does not alias audibly. A common refinement lets harmonics go above Nyquist as long as they fold back above 20 kHz, so up to fs − 20 kHz. The cost per sample is a table read and an interpolation. The price is memory (a set of tables for every waveform), bandwidth that varies with the note (with octave-spaced levels the top harmonic sits anywhere between half the limit and the limit), interpolation error, and a jump in the waveform whenever the pitch crosses from one level to the next.

### Oversampling

Any method can also run at 2× or 4× the rate and be decimated (see [oversampling.md](oversampling.md)). Only harmonics beyond 2·fs − 20 kHz (2×) can then alias into the audible band, and the halfband filter removes everything between 20 kHz and the base Nyquist. For an oscillator this is expensive, though: the filter and the rest of the voice must run at the high rate too, because the decimation can only happen once the voices are summed.

## How it was measured

### The prototypes

All five were built with one interface (`prepare`, `reset`, `setWaveform`, `setPulseWidth`, `setFrequency`, `setIncrement`, `next`, `latencySamples`), and each produces a saw, a pulse, and a triangle. They were measured as prototypes in commit `984d868`, in `anoesis::dsp::prototype`. After the decision, each went one of three ways:

| Candidate | Where it is now | Notes |
|---|---|---|
| minBLEP 32 and 64 taps (**chosen**) | [min_blep.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/min_blep.hpp), [min_blep_table.cpp](../../libs/dsp/src/min_blep_table.cpp), in `anoesis::dsp` | `MinBlepOscillator<Taps>` with a shared `MinBlepTable`; `minBlepTaps` = 64 records the decision. The kernel is a Kaiser-windowed sinc cut off at Nyquist, with β from Kaiser's formula for a full transition width of 9.3% of the rate (20 to 24.1 kHz at 44.1 kHz): β 4.6 at 32 taps, β 9.3 at 64. |
| PolyBLEP 2-point and 4-point | [tests/dsp/oscillators/reference/poly_blep.hpp](../../tests/dsp/oscillators/reference/poly_blep.hpp), test-only (`anoesis::test::reference`) | `PolyBlepOscillator<Points>`. Kept as the reference for minBLEP, for three reasons. It is the closed-form, table-free baseline that the report and the benchmarks print next to minBLEP, so later changes to minBLEP (the 8 waveforms, vectorisation, the tap count) are judged against the alternative D6 rejected, on the same harness. Its residual polynomials are an independent analytic check of the BLEP and BLAMP formulation. And it costs a single header with no build or runtime footprint in the library. |
| Mipmapped wavetables | Deleted (in commit `984d868`) | 11 levels (1024, 512, …, 1 harmonics) of 2048 samples for the saw and the triangle, with linear interpolation; the pulse was the difference of two saws. Deleted because it adds nothing as a reference that its measurements here don't already record: a different structure (tables, level selection, a bank object) that no later work builds on, and 180 KB of tables built at start-up. To rerun it, check out `984d868`. |

Shared details, in [phase.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/phase.hpp) and [waveform.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/waveform.hpp):

- **Phase is a 32-bit fixed-point accumulator** (2³² units per cycle). A float phase cannot reach 0.01 Hz: at 48 kHz the increment, 2·10⁻⁷ cycles, is only 3 or 4 float steps near t = 1, so the pitch would be off by tens of percent. Fixed point resolves 1.1·10⁻⁵ Hz, wraps by itself, and needs no double precision (a Cortex-M4F has no double FPU). The same accumulator carries over into the chosen method.
- **A pulse edge is located with one subtraction.** In fixed point, `phase − widthPhase` wraps by itself, so the distance to the falling edge needs no modulo.
- **minBLEP starts each note as a band-limited onset.** Its delay compensation assumes the residuals of earlier edges are in the ring buffer, and after `reset()` they are not. A first version measured spikes of up to 4.6 on the first samples of a high triangle. Now the first sample adds a step and a bend from silence to the waveform's value and slope, so the output starts at exactly 0 and is the band-limited version of the gated waveform.

### The measurements

[antialiasing_report.cpp](../../tests/dsp/oscillators/antialiasing_report.cpp) is a hidden test (`[d6-report]`, about 40 s in `relwithdebinfo` with all five candidates). It measures each candidate with the harness and prints the tables below. It now covers minBLEP and the PolyBLEP reference; the wavetable column comes from commit `984d868`:

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[d6-report]"
```


- **Aliasing:** every MIDI note 0–127 at 44.1 and 48 kHz, at 1×, 2×, and 4×. For the oversampled cases the oscillator runs at the high rate and `Oversampler::downsample` brings it back. Each render is 65,536 samples at an increment of exactly C·2¹⁶ (C odd, the nearest coherent frequency), so the render is periodic and a rectangular window has no leakage. `measureAliasing` reports the strongest non-harmonic component below 20 kHz, relative to the fundamental.
- **Droop:** `measureHarmonics` (added to the harness for this) gives each harmonic's amplitude. Each is compared with the ideal 2/(πk).
- **Pitch modulation:** linear FM, f₀(1 + 0.25 sin 2πgt), with g = 6.6 Hz and f₀ both exact multiples of a coherent base frequency. Every true component of the FM signal then lies on a multiple of g, and aliases fall between them. The worst one is reported relative to an ideal saw's fundamental. One limit: an artefact that repeats exactly with the vibrato (such as a wavetable level switch at the same point of each cycle) lands on the grid and is not counted. That is why the level-switch jump is reported separately.
- **DC, memory, and CPU:** the mean over coherent renders, `sizeof` and table sizes, and [oscillator_benchmark.cpp](../../tests/dsp/benchmarks/oscillator_benchmark.cpp) through `tools/bench.sh "[oscillators]"`.

## Results

All results are at 1× (no oversampling) unless stated otherwise. All levels are in dB relative to the fundamental.

### Aliasing

The worst alias below 20 kHz in the top two octaves (MIDI 104–127, 3.3–12.5 kHz), and over MIDI 0–103:

| Saw | PolyBLEP 2-pt | PolyBLEP 4-pt | minBLEP 32 | minBLEP 64 | Wavetable |
|---|---:|---:|---:|---:|---:|
| 44.1 kHz | −14.1 / −28.4 | −22.3 / −38.8 | −66.5 / −72.4 | **−91.0 / −102.5** | −120.3 / −68.6 |
| 44.1 kHz, 2× | −40.8 / −50.4 | −64.8 / −74.0 | −84.8 / −94.4 | −103.9 / −114.3 | −108.2 / −68.2 |
| 44.1 kHz, 4× | −59.0 / −70.5 | −95.0 / −107.0 | −94.8 / −105.5 | −115.2 / −125.0 | −102.1 / −69.1 |
| 48 kHz | −19.7 / −30.3 | −29.9 / −41.5 | −69.8 / −77.6 | **−93.0 / −103.8** | −114.3 / −68.1 |
| 48 kHz, 2× | −43.3 / −53.7 | −69.6 / −79.0 | −85.2 / −96.1 | −104.7 / −116.7 | −108.2 / −68.1 |

| 44.1 kHz, 1× | PolyBLEP 2-pt | PolyBLEP 4-pt | minBLEP 32 | minBLEP 64 | Wavetable |
|---|---:|---:|---:|---:|---:|
| Square | −19.0 / −29.5 | −28.5 / −39.9 | −66.5 / −72.4 | −92.1 / −102.8 | −122.9 / −68.6 |
| Triangle | −28.6 / −48.6 | −38.0 / −59.0 | −76.0 / −93.2 | −94.1 / −118.1 | −132.4 / −128.3 |

- **PolyBLEP** is weakest exactly where it matters, at the top of the keyboard. That follows from its kernel: a 5 kHz saw at 48 kHz has its 6th harmonic at 30 kHz, sinc² attenuates it by only 15 dB, and it folds to 18 kHz at 1/6 × −15 dB ≈ −28.7 dB (measured: −28.3 dB). Oversampling helps, but reaching −90 dB takes 4× with the 4-point kernel.
- **minBLEP** improves by about 25 dB from 32 to 64 taps (Kaiser stopband about 51 dB and 93 dB). Its worst case is always in the top octave.
- **The wavetable** is clean at the top, where its levels hold only a few harmonics. Its worst case is at bass notes (MIDI 17–52): linear interpolation of a 2048-sample table that holds up to 1024 harmonics. Oversampling doesn't help with that.

### Passband droop (saw)

| | PolyBLEP 2-pt | PolyBLEP 4-pt | minBLEP 32 / 64 | Wavetable |
|---|---:|---:|---:|---:|
| Worst harmonic up to 10 kHz, MIDI 0–127, 44.1 kHz | −1.5 | −3.0 | 0.0 | −52.7 (bass notes) |
| Worst harmonic up to 20 kHz | −6.3 | −12.7 | 0.0 | missing |
| 110 Hz saw, harmonic at 19 kHz | −5.7 | −11.3 | 0.0 | missing |
| The same at 2× | −1.3 | −2.7 | 0.0 | −0.2 |

- **minBLEP is flat to 20 kHz** (to 0.05 dB), because its kernel's transition band starts at 20 kHz.
- **PolyBLEP's droop** is sinc² or sinc⁴, audible as a duller top on bright patches unless it is compensated.
- **The wavetable** loses up to an octave of top harmonics, depending on where the note falls between two levels (the bandwidth runs from about 12 to 24 kHz). Below about 20 Hz the 2048-sample table cannot hold the harmonics above 8–10 kHz at all.

### Pitch changed every sample

Under a ±25% vibrato at 6.6 Hz, saw carrier at 1978 Hz and at 4945 Hz:

| 44.1 kHz | PolyBLEP 2-pt | PolyBLEP 4-pt | minBLEP 32 | minBLEP 64 | Wavetable |
|---|---:|---:|---:|---:|---:|
| 1× | −50.7 / −48.0 | −60.7 / −57.6 | −92.2 / −89.2 | −123.6 / −118.8 | −61.6 / −148.6 |
| 2× | −72.0 / −69.2 | −96.1 / −92.6 | −116.6 / −113.6 | −135.2 / −131.7 | −71.4 / −67.1 |

- The BLEP methods do as well under modulation as they do at a steady pitch. minBLEP times each edge with the increment that actually crossed it, so its edge positions stay exact under any modulation.
- The wavetable's level changes are a separate problem. A glide that crosses from one level to the next (from 128 to 64 harmonics, for example) makes the output jump by up to **0.42**: a click, unless adjacent levels are crossfaded, which doubles the reads.

### Stability, DC, memory, and CPU

| | PolyBLEP 2-pt | PolyBLEP 4-pt | minBLEP 32 | minBLEP 64 | Wavetable |
|---|---:|---:|---:|---:|---:|
| Largest mean, saw and triangle, 1 kHz to Nyquist | 5·10⁻¹¹ | 1·10⁻¹⁰ | 6·10⁻⁸ | 2·10⁻⁷ | 4·10⁻¹⁶ |
| Peak level, saw / 10% pulse / triangle, 0 Hz to fs | 1.00 / 1.00 / 1.00 | 1.00 / 1.00 / 1.00 | 1.35 / 1.66 / 1.17 | 1.37 / 1.73 / 1.15 | 1.18 / 1.29 / 1.00 |
| Bytes per oscillator | 36 | 36 | 176 | 304 | 48 |
| Shared tables | none | none | 16.6 KB | 33.3 KB | 180 KB (saw and triangle; about 90 KB per extra waveform) |
| Latency | 0 | 0 | 1.83 samples | 2.79 samples | 0 |
| ns per oscillator-sample, 32-saw bank | 2.5 | 2.4 | 3.0 | 2.9 | 2.4 |
| ns per sample, 12 kHz square | 3.3 | 4.2 | 13.1 | 22.2 | 2.5 |

All five stay finite, bounded, and free of denormals for every shape from 0 Hz to fs (clamped to Nyquist), at 44.1 and 48 kHz. CPU is from [baselines.md](../performance/baselines.md) (Apple M1 Ultra, virtualised). The 32-saw bank is one note's worth (4 oscillators × 8 unison voices at 110–880 Hz). At ordinary pitches the per-sample overhead dominates, so all five cost about the same. minBLEP's cost grows with the edge rate.

## The choice: minBLEP, 64 taps

- **Quality.** It is the only candidate that is clean at the top of the keyboard *and* flat in the passband without oversampling. Its worst alias is −91 dB at 44.1 kHz (−93 dB at 48 kHz), and −102 dB below MIDI 104. Getting there with PolyBLEP needs a 4× voice, with 4× the filter cost. The wavetable trades bandwidth, bass-note quality, glide clicks, and memory for its clean top end.
- **CPU.** At ordinary pitches it costs the same as PolyBLEP (about 3 ns per oscillator-sample, so 512 voices at 48 kHz take about 7% of a core). The cost rises only for very high notes: 22 ns for a 12 kHz square, the worst case.
- **Modulation.** It is exact for per-sample pitch changes (−124 dB under the vibrato), which FM, portamento, and pitch envelopes will need.
- **Memory.** 304 bytes per oscillator and 33 KB of shared tables: 156 KB of state for all 512 voices. On STM32 the tables would be generated offline into flash (the builder uses double precision), and the state for a few dozen voices fits in RAM.

## Trade-offs and open items

- **Taps are a quality setting.** The table is built at initialisation, so 32 taps (−66.5 dB, 176 B, 16.6 KB, 13 ns at 12 kHz) stays available. Sylenth1 v3 has separate real-time and render quality settings (inventory §15), and a 32-tap real-time / 64-tap render split would mirror that.
- **Overshoot.** Minimum phase overshoots by about 37% on a saw (9% for linear phase). That costs headroom ahead of the filter's drive stages, and drive will respond to it. Whether Sylenth1's oscillators overshoot like this is not known; M1.13 can measure it against the demo. A linear-phase BLEP with a lookahead of half the taps (about 0.7 ms at 64 taps) would remove the extra overshoot. It wasn't measured, because the aliasing would be the same.
- **Latency.** 2.79 samples of group delay. It is the same for every oscillator, so mixing them causes no comb filtering, and it is far too small to report to the host.
- **High-pitch cost and SIMD (Phase 3).** The residual add uses a masked ring index, which blocks vectorisation. A double-length linear buffer would let the 64 multiply-adds per edge run 4 or 8 wide. Across unison voices the edges fall at different samples, so SIMD across voices needs per-lane edge handling.
- **Waveforms.** The next M1.1 task builds the 8 waveforms. Every piecewise-linear one (pulses of any width, saw/triangle hybrids) is a list of jumps and bends for the same machinery. A sine needs no correction, and noise is generated separately.

## Tests

- [band_limiting_residuals_test.cpp](../../tests/dsp/oscillators/band_limiting_residuals_test.cpp) covers the building blocks:
  - The minBLEP tables start at −1, end at exactly 0, start the ramp table at the group delay, and continue smoothly from row to row.
  - The PolyBLEP residuals cancel the naive step (±½ at the edge), integrate to zero, and are continuous where the polynomial pieces meet, and the ramp residual is the running integral of the step residual.
  - (Until `984d868`: the wavetable levels matched the Fourier series, and the level selection kept folded harmonics above 20 kHz.)
- [band_limited_oscillator_test.cpp](../../tests/dsp/oscillators/band_limited_oscillator_test.cpp) runs the same tests on minBLEP (32 and 64 taps) and the PolyBLEP reference:
  - The output follows the ideal waveform away from edges (allowing for latency).
  - A 5 kHz saw's worst alias is below −27 dB.
  - The output stays finite, bounded, and denormal-free from 0 Hz to fs, for every shape, at 44.1 and 48 kHz.
  - The output has no DC (and the pulse has its ideal 2w − 1).
  - Re-setting the same increment every sample changes nothing, and the vibrato's worst alias is below −50 dB.
  - `reset()` reproduces the output exactly.
- [measure_test.cpp](../../tests/support/test/measure_test.cpp) covers `measureHarmonics`: known amplitudes, the frequency limit, and a band-limited saw at exactly 2/(πk).

## References

- T. Stilson and J. O. Smith, "Alias-Free Digital Synthesis of Classic Analog Waveforms", Proc. ICMC, 1996. (BLIT: the band-limited impulse train view of the problem.)
- E. Brandt, "Hard Sync Without Aliasing", Proc. ICMC, 2001. (minBLEP: minimum-phase band-limited steps.)
- V. Välimäki and A. Huovilainen, "Antialiasing Oscillators in Subtractive Synthesis", IEEE Signal Processing Magazine 24(2), 2007.
- V. Välimäki, J. Pekonen, and J. Nam, "Perceptually informed synthesis of bandlimited classical waveforms using integrated polynomial interpolation", J. Acoust. Soc. Am. 131(1), 2012. (PolyBLEP from B-spline kernels.)
- F. Esqueda, V. Välimäki, and S. Bilbao, "Rounding Corners with BLAMP", Proc. DAFx-16, 2016. (PolyBLAMP for bends.)
- A. V. Oppenheim and R. W. Schafer, *Discrete-Time Signal Processing*, 3rd ed., Pearson, 2010, ch. 13 (the cepstrum and minimum-phase reconstruction).
- J. F. Kaiser, "Nonrecursive digital filter design using the I₀-sinh window function", Proc. IEEE ISCAS, 1974.
