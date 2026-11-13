# How we measure DSP quality

DSP bugs are usually quiet. A filter can be 3 dB off, an oscillator can alias at −60 dB, a resampler can droop 0.5 dB at 18 kHz, and the plugin still makes sound. The only reliable defence is to **measure**. This page explains the measurement toolkit in [tests/support/](../../tests/support/), the theory each tool relies on, and how to use them in a test.

## Theory

### The DFT, the FFT, and bins

The discrete Fourier transform of N samples gives N complex values X[k], one per **bin**, spaced fs/N apart. Bin k holds the component at frequency k·fs/N. For a real signal, bins N/2+1 … N−1 mirror bins 1 … N/2−1, so only bins 0 … N/2 (DC to Nyquist) carry information.

The **FFT** is any O(N log N) algorithm for the DFT. The radix-2 Cooley–Tukey algorithm reorders the samples into bit-reversed order, then combines pairs of half-size transforms in log₂N stages of "butterflies". It needs N to be a power of two.

### Leakage, windows, and coherent sampling

The DFT implicitly treats its N samples as one period of an infinitely repeating signal. If a sine doesn't complete a whole number of cycles in N samples, the repetition has a discontinuity, and the sine's energy **leaks** into every bin. Two remedies:

- **Coherent sampling.** Choose the test frequency so the sine completes exactly an integer number of cycles. Then there is no discontinuity, no leakage, and the whole tone lands in a single bin, even with no window. Choosing an **odd** cycle count with a power-of-two N goes further. The count is then coprime with N, so every harmonic h·f₀, *and every alias of a harmonic* (h·f₀ folded at Nyquist), also lands exactly on its own bin. The measurement becomes exact.
- **Windowing.** Taper the block towards zero at both ends to remove the discontinuity. Each window trades main-lobe width (frequency resolution) against sidelobe level (dynamic range):

  | Window | Highest sidelobe | Main lobe half-width |
  |---|---|---|
  | Rectangular | −13 dB | 1 bin |
  | Hann | −31 dB | 2 bins |
  | 4-term Blackman–Harris | −92 dB | 4 bins |
  | Kaiser, β = 20 | about −190 dB | 7 bins |

  Use coherent sampling whenever you control the stimulus. Use a window when you don't, or when the processed signal's frequencies aren't exact multiples.

### Power normalisation

The spectrum is reported as **power per bin**, normalised as P[k] = c·|X[k]|² / (N·Σw²), with c = 2 except at DC and Nyquist. This normalisation has two useful properties:

- the bins of any signal **sum to its mean square** (Parseval's theorem), so the power in a band is just a sum of bins;
- a sine of amplitude A puts A²/2 into its main lobe, **whatever the window and wherever its frequency falls between bins**. So "the power within ±lobe bins of f" is the power of the tone at f. Scalloping loss (a tone between bins reading low) disappears, because the lobe *sum* is measured, not the peak bin.

### The measurements

- **THD (total harmonic distortion):** √(Σ harmonic powers h = 2 … H) / √(fundamental power). Harmonics above Nyquist aren't harmonics in the output: they are aliases, measured separately.
- **Aliasing:** in a periodic signal with fundamental f₀, every legitimate component is at a multiple of f₀ below Nyquist. Mark those bins (± the main lobe) and DC. *Everything else* is aliasing plus the noise floor. Two figures summarise it:
  - the total non-harmonic power relative to the harmonic power;
  - the **worst single alias** relative to the fundamental, which is the figure that predicts audibility.
- **Frequency response, by sine.** Feed a sine, let transients settle, then correlate input and output with e^(−iωn) under the same window. The ratio of the two correlations is H(e^(iω)): magnitude and phase. Any window bias appears in both and cancels. A Kaiser β = 20 window keeps the sine's own negative-frequency image (at −ω) from leaking in. It works for any process, linear or not.
- **Frequency response, by impulse.** For a linear time-invariant system, the FFT of its impulse response *is* its frequency response, at every bin at once. This is the fastest way to measure passband ripple over thousands of points.

### Golden files

A **golden-file** (or snapshot) test renders something offline and compares it with a reference file committed to the repository. It catches *any* change in output, including ones nobody thought to write a specific test for. It is the regression net for whole patches (M1.8) and for Phase 3 optimisations. The cost is that every *intended* change must be reviewed and re-recorded.

## How it is applied here

All of this lives in `anoesis_test_support` ([tests/support/](../../tests/support/)). It is test-only code: double precision throughout, free to allocate and throw, and no JUCE. Include it as `<anoesis/test/...>`.

| Header | Provides |
|---|---|
| [signals.hpp](../../tests/support/include/anoesis/test/signals.hpp) | `sine`, `impulse`, `constant`, and `coherentFrequency(target, rate, N)`, which gives the nearest frequency with an odd number of cycles in N samples |
| [fft.hpp](../../tests/support/include/anoesis/test/fft.hpp) | `fftInPlace` (radix-2, iterative, twiddles from `std::polar` so their error doesn't grow with N) and `realFft`. Decision D5: hand-written, no dependency. |
| [window.hpp](../../tests/support/include/anoesis/test/window.hpp) | `rectangularWindow`, `hannWindow`, `blackmanHarrisWindow`, `kaiserWindow(n, β)`. Each returns its samples and its main-lobe half-width. All are periodic. |
| [spectrum.hpp](../../tests/support/include/anoesis/test/spectrum.hpp) | `powerSpectrum` and `averagedPowerSpectrum` (Welch) with the normalisation above; `Spectrum::powerAround(f, bins)` |
| [measure.hpp](../../tests/support/include/anoesis/test/measure.hpp) | `measureThd`, `measureAliasing` (with an optional upper frequency, for example 20 kHz), `measureResponseAt`, `impulseResponseSpectrum`, `toDb`, `powerToDb` |
| [golden.hpp](../../tests/support/include/anoesis/test/golden.hpp) | `writeWav`/`readWav` (32-bit float WAV) and `compareWithGolden` |

A typical spectral test:

```cpp
const auto f0 = anoesis::test::coherentFrequency(2500.0, 48000.0, 8192);
const auto output = renderOscillator(f0, 8192);                    // the thing under test
const auto alias = anoesis::test::measureAliasing(output, f0, 48000.0,
                                                  anoesis::test::rectangularWindow(8192), 20000.0);
CAPTURE(alias.worstAliasDb, alias.worstAliasHz);
CHECK(alias.worstAliasDb < -100.0);  // justify the bound in a comment
```

### Golden files

Golden files live in [tests/golden/](../../tests/golden/), one WAV per render, named after the test (for example `dsp/white_noise_seed1_stereo.wav`). They are 32-bit float WAVs, so any audio editor can open them.

- **Comparing:** `compareWithGolden(name, buffer, sampleRate, options)` reports pass or fail with a message saying where the worst difference is. On failure it writes the actual render to `build/<preset>/test-output/<name>.wav`, so the two can be compared by ear or in an editor.
- **Tolerance:** `options.tolerance` is the largest allowed |difference| per sample. Use **0** for integer-exact processes such as noise, so any change at all is caught. For float DSP use a small tolerance such as 1e-6 (−120 dB). Compilers may fuse a multiply and an add into one FMA instruction (Apple Clang does by default on arm64, MSVC doesn't), and that changes the last bits of the result.
- **Creating or updating:** run the test with `ANOESIS_UPDATE_GOLDEN=1`. Listen to the new file, look at it, and commit it with a message saying why the output changed:

  ```sh
  ANOESIS_UPDATE_GOLDEN=1 ./build/debug/tests/dsp/anoesis_dsp_tests "[golden]"
  ```

  Never update golden files just to make a failing test pass without understanding the difference. That defeats their purpose.

## Trade-offs and alternatives

- **Hand-written FFT versus a library** (decision D5). Power-of-two sizes suffice because the harness chooses its own lengths. Double precision puts the FFT's round-off (about 1e-15·log₂N) some 150 dB below float's noise floor, so it never limits a measurement. Its self-tests against a naive DFT prove it correct. pocketfft or KissFFT would add speed and arbitrary sizes, which we don't need, at the cost of a dependency.
- **Measuring with sines versus with sweeps.** Exponential sine sweeps (Farina's method) separate the linear response from each harmonic order in one pass, and are the norm in acoustics. Steady-state sines are simpler and exact for what we measure. Sweeps may be added if nonlinear filters need per-order analysis (M1.3).
- **Golden files versus property tests.** Property tests ("THD below X") survive intended changes; golden files don't. Golden files catch what nobody thought to assert. We use both: properties for the specification, golden files as the regression net.
- **WAV versus raw binary.** WAV costs a 58-byte header and makes every golden file inspectable in any audio tool.

## Tests

The measurement code has its own executable, `anoesis_test_support_tests`, because a wrong measurement silently invalidates every test built on it.

- [tests/support/test/fft_test.cpp](../../tests/support/test/fft_test.cpp): matches a naive DFT for N = 1 … 1024 (error < 1e-9); inverse ∘ forward is the identity (< 1e-12); an impulse gives a flat spectrum; a cosine at bin 10 gives N/2 in bins 10 and N−10 and nothing else; Parseval holds; non-power-of-two sizes throw.
- [tests/support/test/spectrum_test.cpp](../../tests/support/test/spectrum_test.cpp): signal generators; `coherentFrequency` gives odd integer cycles; the windows' coherent gains (1, 0.5, 0.35875); a sine of amplitude 0.5 has power 0.125 bin-centred under a rectangular window and between bins under Blackman–Harris (to 1e-3); the bins sum to the mean square to 1e-12.
- [tests/support/test/measure_test.cpp](../../tests/support/test/measure_test.cpp):

  | Check | Known answer | Measured |
  |---|---|---|
  | THD of a pure float sine | float noise floor | −177.7 dB |
  | THD with a 3rd harmonic at 1% and a 5th at 0.1% | 0.0100499 (−39.96 dB) | to 1e-5 (coherent), 2e-3 (Blackman–Harris, off-bin) |
  | worst alias of an additive (band-limited) sawtooth | none | −174 dB |
  | worst alias of a naive sawtooth at 2.5 kHz | the 10th harmonic folded: −20 dB at 22.98 kHz | −20.00 dB at 22,980 Hz |
  | an injected non-harmonic tone | −80 dB | −80 ± 0.01 dB, at the right frequency; excluded by a 5 kHz upper limit |
  | response of y = (x[n] + x[n−1])/2 | cos(πf/fs), phase −πf/fs | to 1e-6 by sine; to 1e-7 by impulse at all 513 bins |

- [tests/support/test/golden_test.cpp](../../tests/support/test/golden_test.cpp): WAV round-trips bit for bit; missing and malformed files are rejected. The golden flow is tested end to end: a missing file fails with instructions; update mode writes the file; an identical render matches; a difference within tolerance passes; one beyond it fails, reporting "channel 1, sample 50" and saving the actual output; a different shape or rate fails.

First uses on real DSP, in [tests/dsp/generators/white_noise_test.cpp](../../tests/dsp/generators/white_noise_test.cpp):

- white noise's Welch spectrum is flat to within **0.043 dB** across 15 bands (bound 0.25 dB);
- the noise generator matches its golden render, `tests/golden/dsp/white_noise_seed1_stereo.wav`, **bit for bit**.

## References

- F. J. Harris, "On the Use of Windows for Harmonic Analysis with the Discrete Fourier Transform", *Proceedings of the IEEE* 66(1), 1978. The window table, Blackman–Harris coefficients, and scalloping loss.
- J. F. Kaiser and R. W. Schafer, "On the use of the I₀-sinh window for spectrum analysis", *IEEE Trans. ASSP* 28(1), 1980.
- J. W. Cooley and J. W. Tukey, "An Algorithm for the Machine Calculation of Complex Fourier Series", *Mathematics of Computation* 19, 1965.
- P. D. Welch, "The use of fast Fourier transform for the estimation of power spectra", *IEEE Trans. Audio and Electroacoustics* 15(2), 1967.
- IEEE Std 1241-2010, *Terminology and Test Methods for Analog-to-Digital Converters*: coherent sampling, THD, and SINAD.
- A. Farina, "Simultaneous measurement of impulse response and distortion with a swept-sine technique", AES 108th Convention, 2000.
