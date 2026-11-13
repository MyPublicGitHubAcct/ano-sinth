# Oversampling: 2× and 4× with polyphase IIR halfbands

Sylenth1 runs its distortion at 4× the sample rate and its phaser and chorus at 2×. Since v3 it also offers oversampling options for the whole engine. This page explains why a nonlinear process needs oversampling, how the halfband filters that make it cheap work, and what `anoesis::dsp::Oversampler` measures.

## Theory

### Why nonlinear processing aliases

A linear filter can only change the levels and phases of frequencies already present. A **nonlinear** one creates new ones. Pass a sine at f through a waveshaper and harmonics appear at 2f, 3f, 5f, and so on, potentially far above Nyquist (fs/2). In discrete time, frequencies above Nyquist don't vanish. They **fold back**: a component at fs/2 + Δ appears at fs/2 − Δ. These **aliases** are not harmonically related to the input, and they sound like inharmonic, metallic grit. Sweep the input and they move in the opposite direction, which is easy to hear.

**Oversampling** makes room. Run the nonlinearity at N·fs, so harmonics up to N·fs/2 are represented correctly, then low-pass filter away everything above the original band before returning to fs. Most of what would have aliased is removed instead of folded.

### Interpolation and decimation

- **Upsampling by 2** inserts a zero between every pair of samples. This creates a mirror image of the spectrum above the original Nyquist: content at f gets an image at fs − f. A low-pass filter at the new rate removes the images (the *interpolation* filter). A gain of 2 restores the level the zeros removed.
- **Downsampling by 2** low-pass filters first, so nothing remains above the target Nyquist (the *anti-aliasing* or *decimation* filter), then keeps every second sample.
- **4×** is two 2× stages. The second stage's filter has a much easier job: the band to keep (0–20 kHz) is far from its images (68.2 kHz and up at a 44.1 kHz base), so it needs only a few coefficients.

### Halfband filters

Both filters cut off at a quarter of the high rate. A **halfband** filter is symmetric about that point: its passband edge and stopband edge sit equally far either side of fs/4. Halfbands have a special structure that halves their cost.

- **FIR halfbands** have every second coefficient equal to zero.
- **IIR halfbands** split exactly into two allpass branches: H(z) = ½[A₀(z²) + z⁻¹A₁(z²)], where each Aᵢ is a chain of first-order allpass sections in z², (a + z⁻²)/(1 + a z⁻²). Allpass filters have unit gain at every frequency. The magic is in how the two branches' phases add and cancel: in phase in the passband, opposed in the stopband.

The **polyphase** trick runs each branch at the *low* rate, where z² becomes z:

- an interpolator computes output sample 2n from branch 0 and output 2n+1 from branch 1, both fed by input n;
- a decimator feeds the pair's two samples to the two branches and averages.

Neither ever multiplies the zeros, nor computes the samples that would be thrown away. The cost is one multiply per coefficient per low-rate sample.

The coefficients come from elliptic filter theory. The halfband elliptic filter's poles lie on the imaginary axis, so each conjugate pair becomes one allpass coefficient (Valenzuela and Constantinides, 1983). A useful consequence: the two branches are **power-complementary**, so |H|² + |H_c|² = 1. Passband ripple δ_p and stopband rejection δ_s are then tied together: δ_p ≈ δ_s²/2. At 100 dB of rejection, the passband ripple is around 10⁻¹⁰ dB, in effect perfectly flat.

## How it is applied here

### The filters

[tools/design/halfband_iir.py](../../tools/design/halfband_iir.py) computes the elliptic coefficients with Jacobi theta-function series, in pure Python. It then **verifies** each design by evaluating the exact polyphase structure on a dense grid, including with the coefficients rounded to float. It picks the fewest coefficients that reach 100 dB.

| Stage | Passband (normalised to the high rate) | Stopband | Coefficients | Ripple | Rejection (float coefficients) |
|---|---|---|---|---|---|
| 1 (1× ↔ 2×) | 0 – 0.2268 (0–20 kHz at 44.1k) | 0.2732 – 0.5 (24.1 kHz up) | 8 (4 per branch) | 1.7e-10 dB | 104.0 dB |
| 2 (2× ↔ 4×) | 0 – 0.1134 (0–20 kHz at 88.2k) | 0.3866 – 0.5 (68.2 kHz up) | 4 (2 per branch) | 1.3e-12 dB | 125.2 dB |

The band edges scale with the base rate. At 48 kHz the passband reaches 21.8 kHz.

[libs/dsp/include/anoesis/dsp/resampling/halfband.hpp](../../libs/dsp/include/anoesis/dsp/resampling/halfband.hpp) holds:

- the two coefficient tables;
- `AllpassChain<N>`, sections y = a(x − y₁) + x₁;
- `HalfbandUpsampler<N>`: even output from branch 0, odd output from branch 1;
- `HalfbandDownsampler<N>`, which takes the odd-phase output y[n] = ½[A₀(v[2n+1]) + A₁(v[2n])]. Both branches then see the same pair, and neither needs an extra delay;
- `groupDelayAtDc()`: the branches have equal magnitude at DC, so the halfband's group delay there is the average of the branch delays. Each section contributes 2(1 − a)/(1 + a), and branch 1 one more sample.

### The oversampler

[libs/dsp/include/anoesis/dsp/resampling/oversampler.hpp](../../libs/dsp/include/anoesis/dsp/resampling/oversampler.hpp) and [libs/dsp/src/oversampler.cpp](../../libs/dsp/src/oversampler.cpp):

```cpp
anoesis::dsp::Oversampler oversampler{anoesis::dsp::OversamplingFactor::four};
oversampler.prepare(spec);                                // base-rate spec; allocates here only
distortion.prepare(oversampler.oversampledSpec());        // 4x the rate and block size

// per block, on the audio thread:
auto inner = oversampler.upsample(block);                 // a view of 4 x block.numSamples()
distortion.process(inner);                                // in place, at 4x
oversampler.downsample(block);                            // back into the caller's block
```

- `prepare()` allocates the oversampled buffer (channels × maxBlockSize × ratio). It is the only allocation.
- **4× runs both stages per sample**: one input becomes two through stage 1, and each of those becomes two through stage 2. So there is no intermediate 2× buffer. Decimation mirrors this.
- `OversamplingFactor::none` copies, so the same code path works when oversampling is switched off. `setFactor()` takes effect at the next `prepare()`, since the buffer size changes.
- `latencySamples()` is the round trip's group delay at DC in base-rate samples, for host delay compensation. Upsampling delays by τ (the halfband's DC group delay, in high-rate samples), and odd-phase decimation by τ − 1, so one stage's round trip is 2τ − 1 high-rate samples. The result: **3.18 samples at 2×, 4.30 at 4×**.
- Effects that mix dry and wet should mix *inside* the oversampled domain, so both paths share the same filters and phase.

## Trade-offs and alternatives

| | Polyphase IIR halfband (chosen) | Linear-phase FIR halfband |
|---|---|---|
| Cost for ≥ 100 dB with this transition band | 8 + 4 first-order sections | about 70+ taps for stage 1 alone (half of them zero) |
| Latency | 3.2 (2×) / 4.3 (4×) samples | about 35 samples per stage pair; the reported latency changes when oversampling is toggled |
| Passband phase | non-linear, increasingly so near 20 kHz | linear |
| Passband ripple | ~10⁻¹⁰ dB (power-complementary) | set by the design, typically 10⁻³–10⁻² dB |
| Fixed-point / STM32 | first-order sections, very cheap | many multiply-accumulates |

Phase non-linearity confined to the top octave is inaudible in a synth's effects chain, and dry/wet mixing happens inside the oversampled domain where both paths share it. Low, fixed latency and low cost decided it. A linear-phase FIR would suit a mastering-grade process. If one is ever needed, it can be added beside the IIR stages (Phase 2).

Other choices:

- **Two cascaded 2× stages versus a direct 4× filter.** The cascade lets stage 2 be cheap: 4 coefficients instead of the many a single filter with stage 1's transition band would need at 4×.
- **100 dB of rejection.** The stopband floor sits below 16-bit quantisation (96 dB) and well under any audible threshold for aliasing that is masked by the signal itself. Each further ~12–13 dB costs one more stage-1 coefficient. The design script makes that trade a one-line change.
- **Float coefficients and state.** These are measured, not assumed: the float implementation keeps 104 dB and better than 2e-6 dB of ripple. Allpass sections are robust to coefficient rounding, since each stays exactly allpass whatever its coefficient.

## Tests

[tests/dsp/resampling/oversampler_test.cpp](../../tests/dsp/resampling/oversampler_test.cpp) measures the actual float implementation with the [harness](measuring-dsp.md), at a 44.1 kHz base rate:

| Measurement | 2× | 4× | Bound |
|---|---|---|---|
| passband ripple of the round trip, 0–20 kHz (impulse method, 16384 points) | 8.9e-7 dB | 1.5e-6 dB | 1e-4 dB |
| largest deviation from 0 dB in the passband | 8.8e-7 dB | 9.6e-7 dB | 1e-4 dB |
| image rejection, tone at 1 kHz | 109.4 dB | 111.1 dB | ≥ 100 dB |
| image rejection, tone at 10 kHz | 127.3 dB | 125.4 dB | ≥ 100 dB |
| image rejection, tone at 17 kHz | 104.2 dB | 104.2 dB | ≥ 100 dB |
| image rejection, tone at 20 kHz | 104.5 dB | 104.5 dB | ≥ 100 dB |
| alias rejection, tone at 24.2 kHz | 106.0 dB | 106.0 dB | ≥ 100 dB |
| alias rejection, 30 kHz | 108.2 dB | 108.2 dB | ≥ 100 dB |
| alias rejection, 40 kHz | 107.6 dB | 107.8 dB | ≥ 100 dB |
| alias rejection, 44 kHz | 128.6 dB | 131.4 dB | ≥ 100 dB |
| alias rejection, 60 kHz / 87 kHz | — | 154.4 / 131.4 dB | ≥ 100 dB |
| latency (phase delay at 20 Hz) | 3.1825 samples | 4.3035 samples | `latencySamples()` ± 0.01 |

Also tested:

- the lifecycle contract (reset and re-prepare are bit-identical to fresh) for 2× and 4×;
- factor 1 is an exact pass-through;
- `oversampledSpec()` and `setFactor()` semantics;
- channel independence;
- stability on a full-scale square wave (peak < 1.5).

During TDD, an unfiltered stub (zero-stuffing up, plain decimation down) failed the image and alias tests with only 6 dB of rejection. This confirmed that the tests detect a missing filter.

## References

- P. P. Vaidyanathan, *Multirate Systems and Filter Banks* (1993), ch. 4–5: polyphase structures and IIR halfbands from allpass branches.
- R. A. Valenzuela and A. G. Constantinides, "Digital signal processing schemes for efficient interpolation and decimation", *IEE Proceedings G* 130(6), 1983.
- fred harris, *Multirate Signal Processing for Communication Systems* (2004), ch. 10: recursive polyphase filters.
- Laurent de Soras, *HIIR* library documentation (2005): a practical write-up of the same elliptic halfband design.
- Vesa Välimäki et al., "Antialiasing for Nonlinearities: Acoustic Modeling and Synthesis Applications", *Acta Acustica* (2019): why and how much to oversample nonlinear audio processes.
