# Oscillator drift

An analog oscillator never holds its pitch exactly. Its pitch wanders by a few cents, slowly, and each voice of a polyphonic synth wanders on its own. Chords beat gently, and two notes at the same pitch slowly swell and cancel. Sylenth1 added an emulation of this in v3.072 (inventory [§2, Drift](../reference/sylenth1-features.md#drift)). This page covers:

- where drift comes from in analog circuits, and what it sounds like;
- the ways to model it, and what each model's statistics and spectrum are;
- what is known about Sylenth1's drift;
- the candidate models prototyped and measured here, and the one chosen;
- how it is built, and the tests that pin it down.

Read [oscillator-controls.md](oscillator-controls.md) (pitch as one fractional note), [oscillator-unison.md](oscillator-unison.md), and [oscillator-range.md](oscillator-range.md) (pitch steps) first.

> **Status.** Evidence gathered, models measured, and the choice made (2026-11-14, Decisions log). Building it is the next commit; the sections **How it is applied here** and **Tests** describe the build once it lands.

## Theory

### Where drift comes from

A voltage-controlled oscillator's pitch is set by an exponential converter: a transistor pair whose collector current is I₀·e^(V/V_T), with V_T = kT/q, the thermal voltage, about 26 mV at room temperature. The converter's scale, and so the pitch, depends on the temperature T. Synthesizer designs compensate (a tempco resistor, a heated chip, matched pairs), but never perfectly. What is left:

- **Temperature.** The circuit warms up after power-on and follows the room and the instrument's own heat, over seconds to minutes. A fraction of a degree moves the pitch by cents.
- **Component noise.** Transistors and resistors have flicker (1/f) noise. Its power rises toward low frequencies, so it shows as slow, random wander rather than a hiss.
- **Calibration spread.** Each voice's oscillators are trimmed separately, never exactly alike: a static offset per voice of a few cents.
- **Supply and loading.** Ripple and load changes move the pitch a little, faster and smaller.

In a polyphonic analog synth every voice has its own oscillators, so **each voice drifts independently**. That is what drift sounds like: not a pitch error (a few cents is near the threshold of hearing for a single note), but **beating between voices**. Two voices at 440 Hz, 1.5 cents apart, beat at 440·(2^(1.5/1200) − 1) = 0.38 Hz: a slow swell and fade that makes chords and doubled notes sound alive. Inside a hardware voice, the oscillators drift independently of each other too.

### Models

A drift model is a random signal d(t), the pitch deviation in cents, added to the note. What distinguishes models is its **distribution** (how far it goes), its **spectrum** (how fast it moves), and whether it **stays bounded** over hours.

| Model | Definition | Spectrum | Distribution and bound |
|---|---|---|---|
| **Random walk** | d ← d + w, w white | 1/f² at every frequency | Variance grows without limit: the pitch wanders off. Never usable alone. |
| **One-pole (leaky) noise**, the Ornstein–Uhlenbeck process | d ← a·d + g·w | Flat below the corner f_c, then −20 dB/decade (a Lorentzian) | Gaussian, stationary. Its path is rough: its increments over a short time Δ grow as √Δ, so it has fast, small movement at every scale. |
| **Two-pole noise** | two one-poles in cascade | Flat below f_c, then −40 dB/decade | Gaussian, stationary, and smooth: its slope is continuous. |
| **Interpolated sample and hold** | a new random target every 1/r seconds, approached linearly or along a cosine | sinc⁴-shaped lobes (linear) or steeper (cosine), with nulls at multiples of r | Bounded exactly by the targets' range. The regular period is a rhythm. |
| **Sum of slow sines** | Σ A sin(2πf_k t + φ_k) with random phases | Lines at the f_k | Bounded. Deterministic once the phases are drawn: a slow, aperiodic LFO. |
| **1/f (pink) noise**, by the Voss–McCartney algorithm | K random rows, row k redrawn every 2^(k+1) ticks, summed | −10 dB/decade over K octaves, flat below | Nearly Gaussian, bounded by K rows. Long memory: it wanders slowly over minutes. |
| **Static offset per note** | one random draw at note-on | none within a note | Bounded. Models calibration spread, not wander. |

For the two noise models with a corner f_c, the variance below f is a closed form:

- One-pole: (2/π)·atan(f/f_c). Half lies below f_c, but 99% only below 64 f_c: a one-pole drift at 0.5 Hz still moves at 30 Hz.
- Two-pole: [atan x + x/(1 + x²)]/(π/2), with x = f/f_c. Half lies below **0.442 f_c**, and 99% below **3.37 f_c**.

The two-pole's autocorrelation in continuous time is ρ(τ) = (1 + ωτ)·e^(−ωτ), with ω = 2πf_c. Updated every Δ seconds with a = e^(−ωΔ), it is exactly ρ[k] = a^k·(1 + k·(1 − a²)/(1 + a²)).

### Applying drift to a band-limited oscillator

The deviation is added to the note (in semitones, d/100), and the note is converted to a phase increment ([oscillator-controls.md](oscillator-controls.md)). There are two costs:

- **Converting the note.** A note per sample costs an `exp2` per sample per slot, plus setting every unison voice's increment. Holding the drift for a block of samples costs one conversion per block.
- **Pitch steps.** A change of increment of `bandLimitedPitchStep` (2¹⁸ units, 6.1·10⁻⁵ cycles per sample) or more is band-limited by a ramp residual, about 15 ns. Smaller changes are left as they are, at an error of at most |slope| × change × 2.79 samples ([oscillator-range.md](oscillator-range.md#pitch-steps-one-band-limited-bend-deferred-to-the-next-sample)). A relative pitch change makes the largest change of increment at the top of the range, so that is where a drift model meets the threshold.

A drift held for N samples is a staircase in pitch. It modulates every partial at fs/N, which puts sidebands at f ± k·fs/N. Their level grows with the partial's frequency, the depth, and the speed of the drift.

## Sylenth1: what is known

The full source table, with confidence ratings, is in the inventory ([§2, Drift](../reference/sylenth1-features.md#drift)). In short:

- **Official:** "Added oscillator drift emulation (oscdrift in Settings.xml)", v3.072 (9 November 2022). The setting is global: the developer considered a preset parameter and kept the global one.
- **The developer, on what it acts on:** "It's not oscillator specific, but voice specific rather, like global tuning. The oscs should stay in sync between each other, but the voices may drift." So a voice's oscillators drift together, and voices drift apart. Unlike hardware, Sylenth1 keeps the oscillators inside a voice in step. That keeps the manual's two-saw PWM trick at a constant width, and an in-phase unison stack at exactly N times one voice.
- **Users:** the file reads `<GlobalTuning frequency="440" oscdrift="0.015"/>`. The default is 0.015, and drift is **on by default**. 0 turns it off, and larger values give "wilder drift". It is "very subtle", and one user heard "only a simple static detuning".
- **Unknown:** the unit of 0.015, the rate, whether it moves within a note, what happens at a note-on, and whether unison voices share it. Open questions O18–O25, each with the demo measurement that settles it in M1.13.

## The options and the decision

Measured by the hidden `[drift-report]` and `[drift-long-report]` ([drift_report.cpp](../../tests/dsp/oscillators/drift_report.cpp)) and the `[drift]` benchmarks ([drift_benchmark.cpp](../../tests/dsp/benchmarks/drift_benchmark.cpp)), on prototypes of each model ([drift_candidates.hpp](../../tests/dsp/oscillators/drift_candidates.hpp)). Every model is normalised to the same standard deviation σ and given one rate r = 0.5 Hz: the corner of the one- and two-pole noise; the target rate of the sample and hold; the middle of the three sines (0.618, 1, 1.618 × r); the clock of the 1/f noise, at 4r with 8 rows. Each starts from its stationary distribution, as a process that has been running unheard would be. The prototypes and this report can be rerun from the commit that recorded this decision.

### Spectrum and distribution

σ = 1 cent, updated at 750 Hz (48 kHz / 64), one hour.

| Model | σ (cents) | Peak (cents) | Kurtosis | Half the variance below | 99% below | PSD at r × 0.1 / 1 / 10 / 100 (dB) | Variance above 20 Hz |
|---|---:|---:|---:|---:|---:|---|---:|
| One-pole noise | 0.999 | 4.72 | 2.97 | 0.50 Hz | 31.8 Hz | 0 / −3.4 / −20.6 / −40.5 | −18.0 dB |
| **Two-pole noise** | 1.003 | 4.05 | 2.95 | **0.23 Hz** | **1.68 Hz** | 0 / −6.3 / −40.5 / −80.2 | −51.7 dB |
| Sample and hold, linear | 1.005 | 2.12 | 2.06 | 0.09 Hz | 0.33 Hz | 0 / −44.0 / −63.6 / −103.6 | −71.9 dB |
| Sample and hold, cosine | 1.005 | 2.00 | 1.99 | 0.10 Hz | 0.35 Hz | 0 / −31.9 / −90.4 / −150.4 | −113.7 dB |
| Sum of 3 sines | 1.000 | 2.28 | 2.50 | 0.50 Hz | 0.82 Hz | lines | −139.5 dB |
| 1/f noise | 1.026 | 3.88 | 2.94 | 0.03 Hz | 0.69 Hz | 0 / −17.1 / −55.1 / −97.1 | −65.8 dB |
| Static per note (100,000 notes) | 1.001 | 1.73 | 1.80 | | | | none |

- The two-pole measures as the theory says: half its variance below 0.442 × 0.5 = 0.221 Hz (measured 0.229) and 99% below 3.37 × 0.5 = 1.69 Hz (measured 1.68), falling 40 dB per decade.
- The one-pole's rough path keeps 1.6% of its variance (−18 dB) above 20 Hz, where pitch movement is heard as roughness, not drift.
- A kurtosis of 3 is Gaussian. The sample and hold's 2.0 and the static offset's 1.8 mean they spend more of their time near their limits.

### Ten hours

σ = 1 cent, at the control rate: the one-minute windows' means and standard deviations.

| Model | Peak | σ, first / last hour | Mean, first / last hour | Worst one-minute mean | One-minute σ range | Beyond 3σ |
|---|---:|---:|---:|---:|---:|---:|
| One-pole noise | 4.79 | 0.999 / 0.997 | −0.012 / 0.026 | 0.29 | 0.86–1.18 | 2,718 ppm |
| **Two-pole noise** | 5.11 | 1.003 / 0.994 | −0.017 / 0.036 | 0.41 | 0.77–1.29 | 2,935 ppm |
| Sample and hold, linear | 2.12 | 1.005 / 1.016 | 0.023 / −0.015 | 0.67 | 0.64–1.27 | 0 |
| Sample and hold, cosine | 2.00 | 1.005 / 1.013 | 0.022 / −0.014 | 0.63 | 0.66–1.23 | 0 |
| Sum of 3 sines | 2.45 | 1.000 / 1.000 | 0.000 / 0.000 | 0.02 | 0.98–1.02 | 0 |
| 1/f noise | 3.88 | 1.026 / 1.016 | 0.215 / −0.010 | 1.67 | 0.48–1.49 | 1,685 ppm |

No model wanders off: σ is the same in the first and the last hour for each. The Gaussian models reach 5σ in ten hours, about as a Gaussian should (2,700 ppm beyond 3σ, against 2,700 for a true Gaussian). The 1/f noise's long memory shows as the slowest-moving minute means.

### Independence of voices

Two streams from adjacent seeds (n and n + 1; `Random` spreads every seed through SplitMix64), 64 pairs of 10 minutes: the |correlation| between them.

| Model | Mean | Worst |
|---|---:|---:|
| One-pole noise | 0.020 | 0.058 |
| **Two-pole noise** | 0.032 | 0.092 |
| Sample and hold, linear / cosine | 0.048 / 0.044 | 0.124 / 0.117 |
| Sum of 3 sines | **0.301** | **0.931** |
| 1/f noise | 0.097 | 0.239 |

For independent two-pole streams the expected spread is σ_r ≈ √(2.5/(ωT)) = 0.036 over 10 minutes, a mean |r| of 0.029: the measured 0.032 is that. The slower models (sample and hold, 1/f) have fewer independent stretches in 10 minutes, so their spread is larger. They are independent too.

**The sum of sines is not.** Every voice has sines at the same frequencies, so two voices whose random phases happen to be close stay correlated forever (|r| up to 0.93): they drift in step. Randomising the frequencies per voice would fix that, at the cost of the model's simplicity.

### Against the pitch-step threshold

A voice at 0.44 fs (21.1 kHz at 48 kHz), the drift added to its note in float, 60 s: the largest change of increment between samples, in units of `bandLimitedPitchStep`, and how many changes per second reach it.

| Model | σ 1.5 cents, 0.5 Hz: every sample / held per 64 / linear per 64 | σ 5 cents, 2 Hz | σ 20 cents, 5 Hz (stress) |
|---|---|---|---|
| One-pole noise | 0.13 / **1.07 (4.4/s)** / 0.02 | 0.85 / **7.7 (637/s)** / 0.12 | **5.7 (38,787/s)** / **53 (733/s)** / 0.84 |
| **Two-pole noise** | 0.008 / 0.11 / 0.008 | 0.027 / **1.41 (2.7/s)** / 0.027 | 0.24 / **13 (588/s)** / 0.21 |
| Sample and hold, linear | 0.008 / 0.02 / 0.008 | 0.008 / 0.21 / 0.008 | 0.04 / **2.4 (246/s)** / 0.04 |
| Sample and hold, cosine | 0.008 / 0.03 / 0.008 | 0.008 / 0.31 / 0.008 | 0.06 / **3.5 (225/s)** / 0.06 |
| Sum of 3 sines | 0.008 / 0.07 / 0.008 | 0.020 / 0.92 / 0.020 | 0.15 / **9.1 (618/s)** / 0.15 |
| 1/f noise | 0.008 / 0.02 / 0.008 | 0.008 / 0.27 / 0.008 | 0.05 / **2.7 (315/s)** / 0.05 |

- 0.008 is the floor: one float rounding step of a note near 129 (1.5·10⁻⁵ semitones) moves the increment by 0.008 of the threshold.
- At the default depth, every smooth model held per 64 samples stays under the threshold. Only the one-pole crosses it, from its rough path.
- Crossing is harmless: each crossing is an exact band-limited bend, at about 15 ns. The two-pole held at 5 cents and 2 Hz costs 2.7 × 15 ns a second. Only the one-pole per sample under stress, crossing on most samples, would cost real CPU.

### Holding the drift for 64 samples: sidebands

A sine with the drift updated every 64 samples (750 Hz), 2¹⁸ samples at 48 kHz: the power at f ± 750k Hz (k = 1…4) relative to the carrier, in dB, held / interpolated linearly.

| Model | σ 1.5 cents, 0.5 Hz: 1 kHz | 5 kHz | 21.1 kHz | σ 5 cents, 2 Hz: 1 kHz | 5 kHz | 21.1 kHz |
|---|---:|---:|---:|---:|---:|---:|
| One-pole noise | −103 / −129 | −87 / −118 | −70 / −91 | −86 / −112 | −67 / −90 | −46 / −54 |
| **Two-pole noise** | **−119** / −138 | **−105** / −134 | **−92** / −114 | **−97** / −134 | **−82** / −116 | **−69** / −91 |
| Sample and hold, linear | −127 / −135 | −113 / −132 | −101 / −112 | −112 / −152 | −98 / −138 | −86 / −108 |
| Sample and hold, cosine | −126 / −141 | −112 / −129 | −100 / −113 | −112 / −131 | −98 / −126 | −85 / −108 |
| Sum of 3 sines | −118 / −135 | −104 / −135 | −92 / −115 | −95 / −143 | −81 / −129 | −69 / −112 |
| 1/f noise | −132 / −138 | −119 / −134 | −107 / −119 | −105 / −132 | −91 / −124 | −78 / −106 |

At the default depth, holding the two-pole for 64 samples puts its sidebands 105 dB below a 5 kHz partial, and 92 dB below one at 21 kHz. The level grows by 6 dB per doubling of the partial's frequency, and with the depth times the rate.

### CPU

Nanoseconds per sample, 48 kHz, `relwithdebinfo` (`tools/bench.sh "[drift]"`).

| Generator | Every sample | At 64, interpolated linearly per sample |
|---|---:|---:|
| One-pole noise | 3.68 | 0.71 |
| Two-pole noise | 7.41 | 2.26 |
| Sample and hold, linear / cosine | 3.54 / 3.59 | 2.24 / 3.56 |
| Sum of 3 sines | 7.44 | 0.83 |
| 1/f noise | 3.55 | 2.23 |

At a control rate, the generator itself costs 1/64 of its per-sample cost, under 0.12 ns. What remains in the second column is the prototype's per-sample interpolation, a division and a branch, which a production version would not need.

Applying it to saws at 110–880 Hz, Detune 0.2 st, Stereo 0.5, two-pole noise:

| Voice | No drift (a note per block) | Drift interpolated per sample (a note per sample) | Drift held per 64 (a note per 64 samples) |
|---|---:|---:|---:|
| 1 slot, 1 unison voice | 3.09 | 8.32 (+169%) | 3.46 (+12%) |
| 1 slot, 8 unison voices | 24.3 | 38.6 (+59%) | 25.9 (+7%) |
| **4 slots × 8 voices (a whole note)** | **101.3** | **151.7 (+50%)** | **103.5 (+2%)** |

A note per sample costs an `exp2` per sample per slot and an increment update per unison voice. Holding for 64 samples costs one of each per 64 samples.

### The decision

Chosen by the project owner on 2026-11-14 (PLAN.md, Decisions log):

| Question | Decision | Why |
|---|---|---|
| **Model** | **Two-pole noise**: white noise through two one-poles in cascade at the same corner, started from its stationary distribution | Smooth (−40 dB/decade, −52 dB above 20 Hz), Gaussian, stationary over hours, independent across seeds, and with no rhythm. The cosine sample and hold is cleaner spectrally and bounded exactly, but steps to a new target at a regular rate; the sum of sines locks voices together; the one-pole is rough; the 1/f noise wanders over minutes. |
| **Default depth and rate** | **σ = 1.5 cents, corner 0.5 Hz** (half the variance below 0.22 Hz) | Reads `oscdrift = 0.015` as a standard deviation in semitones (O19), slow enough that a short note sounds statically detuned (O20), as users describe. Both are hypotheses for M1.13. |
| **Independence and seeding** | **One drift per polyphonic voice, shared by its 4 oscillators and all their unison voices.** Each voice's generator is seeded from the instance seed and its voice index (spread by SplitMix64). | The developer's statement. Shared drift also keeps the two-saw PWM trick and in-phase unison exact. |
| **Note-on** | From silence, a fresh draw from the stationary distribution; while sounding (legato, a retrigger), it continues | A voice's drift has been "running unheard", as O10's free-running phase has. No pitch jump at a legato note (O22). |
| **Update rate** | **Every 64 samples, held**, on a grid counted from the generator's start, not per block. When the voice passes a note per sample anyway (glide, pitch modulation), the drift is added to it. | +2% for a whole note against +50% per sample; sidebands −105 dB at 5 kHz; no threshold crossings at the default depth. The grid makes the output independent of the block size. |
| **Where it lives** | A `PitchDrift` generator in `libs/dsp` that takes σ in cents and the corner in Hz. The voice (M1.4) owns one and adds its offset to the note it gives every slot. `OscillatorSlot` is unchanged. Depth 0 gives offsets of exactly 0, so drift off is bit-identical. | |
| **Plugin default** | **On, at σ = 1.5 cents**, as in Sylenth1. A global setting (not per preset), in interop/plugin (M1.9). | Sylenth1's default is on (inventory). Tests and golden renders set the depth they need explicitly. |

## How it is applied here

Built in the next commit.

## Tests

Built in the next commit.

## References

- LennarDigital, [Sylenth1 version history](https://lennardigital.com/sylenth1/whatsnew.php), v3.072, 2022; the developer's and users' posts in KVR threads [589147](https://www.kvraudio.com/forum/viewtopic.php?t=589147) and [590403](https://www.kvraudio.com/forum/viewtopic.php?t=590403). Sources and open questions: the [feature inventory](../reference/sylenth1-features.md#drift).
- G. E. Uhlenbeck and L. S. Ornstein, "On the Theory of the Brownian Motion", *Physical Review* 36, 1930: the one-pole (Ornstein–Uhlenbeck) process.
- N. J. Kasdin, "Discrete Simulation of Colored Noise and Stochastic Processes and 1/f^α Power Law Noise Generation", *Proceedings of the IEEE* 83(5), 1995: filtered noise, its discrete form, and its statistics.
- R. F. Voss and J. Clarke, "'1/f noise' in music: Music from 1/f noise", *Journal of the Acoustical Society of America* 63(1), 1978; M. Gardner, "Mathematical Games: White and brown music, fractal curves and one-over-f fluctuations", *Scientific American* 238(4), 1978: the Voss algorithm for 1/f noise.
- P. Horowitz and W. Hill, *The Art of Electronics*, 3rd ed., Cambridge, 2015, ch. 2: the Ebers–Moll model and the temperature dependence of an exponential converter.
- J. O. Smith III, *Mathematics of the Discrete Fourier Transform*, online book, W3K Publishing, 2007: beating, and Welch's method for power spectra.
- B. C. J. Moore, *An Introduction to the Psychology of Hearing*, 6th ed., 2012, ch. 6: frequency discrimination.
