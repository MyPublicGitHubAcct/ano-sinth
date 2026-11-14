# Oscillator drift

An analog oscillator never holds its pitch exactly. Its pitch wanders by a few cents, slowly, and each voice of a polyphonic synth wanders on its own. Chords beat gently, and two notes at the same pitch slowly swell and cancel. Sylenth1 added an emulation of this in v3.072 (inventory [§2, Drift](../reference/sylenth1-features.md#drift)). This page covers:

- where drift comes from in analog circuits, and what it sounds like;
- the ways to model it, and what each model's statistics and spectrum are;
- what is known about Sylenth1's drift;
- the candidate models prototyped and measured here, and the one chosen;
- how it is built, and the tests that pin it down.

Read [oscillator-controls.md](oscillator-controls.md) (pitch as one fractional note), [oscillator-unison.md](oscillator-unison.md), and [oscillator-range.md](oscillator-range.md) (pitch steps) first.

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

Measured at commit `264f362` by the hidden `[drift-report]` and `[drift-long-report]` and the `[drift]` benchmarks, on test-only prototypes of each model (`tests/dsp/oscillators/drift_candidates.hpp` at that commit). The prototypes were removed once the choice was built, and the report now measures `PitchDrift` (**Results** below); checking out that commit reruns every figure in this section. Every model is normalised to the same standard deviation σ and given one rate r = 0.5 Hz: the corner of the one- and two-pole noise; the target rate of the sample and hold; the middle of the three sines (0.618, 1, 1.618 × r); the clock of the 1/f noise, at 4r with 8 rows. Each starts from its stationary distribution, as a process that has been running unheard would be.

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

### The generator

`PitchDrift` ([pitch_drift.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/pitch_drift.hpp)) is the whole of it: a `Random`, two state variables, and a few coefficients: about 100 bytes.

- **The model.** Uniform white noise w (scaled by √3 to unit variance) through two one-poles in cascade at the corner f_c: y₁ ← a·y₁ + g·w, y₂ ← a·y₂ + (1 − a)·y₁, with a = e^(−2π f_c Δ) and Δ = 64/fs. The coefficients are computed in double whenever the rate or the sample rate changes, from the closed-form stationary moments of the recursion, so that y₂ has unit variance exactly. The state is kept at unit σ; the offset is depth × y₂ / 100 semitones. So the depth scales the drift and nothing else, and changing it never disturbs the random sequence.
- **Held per 64 samples, on its own grid.** `updateInterval` is 64. The grid is counted from the last note-on from silence (or `reset()`), never from a block boundary, so how a render is split into blocks changes nothing. The coefficients use the update rate fs/64, so the drift has the same statistics in seconds at every sample rate.
- **Note-ons.** `noteOn()` from silence draws (y₁, y₂) jointly from the stationary distribution, through the Cholesky factor of its covariance, with approximately Gaussian draws (the sum of four uniforms), and restarts the grid. While sounding it does nothing: a legato note or a retrigger continues the drift. `stop()` marks the voice finished; `reset()` also restarts the random sequence from the seed.
- **Off.** Depth 0, NaN, negative, or below `minDepthCents` (10⁻³ cents) is off. Off, `process()` makes one run of the whole block at exactly 0, so a note plus the offset is the note, bit for bit. The state still advances, so turning the drift on later starts from where the sequence would be.
- **Seeds.** Each voice gives its drift its own seed (the instance seed plus the voice index, say); `Random` spreads every seed through SplitMix64, so adjacent seeds give independent streams. The seed is XORed with `seedSalt`, so a drift and a slot given the same seed do not share a sequence.
- **Safety.** Rates are clamped to 0.001–50 Hz (NaN is the default) and depths to 1200 cents. `flushDenormal()` guards the state, as for any recursion ([denormals.md](denormals.md)). Nothing is allocated.

### How a voice uses it

The voice (M1.4) owns one `PitchDrift` and adds its offset to the note it gives each of its 4 oscillator slots. `OscillatorSlot` is unchanged.

```cpp
// The usual case: a note per block. The drift splits the block into runs of
// constant offset, at most 64 samples long, on its own grid.
drift.process(numSamples, [&](std::size_t start, std::size_t length, float semitones) {
    for (auto& slot : slots)
        slot.process(bus.subBlock(start, length), note + semitones);
});

// A voice that passes a note per sample anyway (glide, pitch modulation):
drift.addTo(notes);  // adds the held offsets in place
for (auto& slot : slots)
    slot.process(bus, notes);
```

Both give the same pitch path. The first converts each slot's note once per run (an `exp2` per 64 samples); the second once per sample, which the voice pays for its glide or modulation anyway.

The plugin's drift setting (M1.9) is global, not part of a preset, and on by default at `defaultDepthCents` (1.5) and `defaultRateHz` (0.5). In the library the depth starts at 0, so tests and other uses opt in.

### Which content belongs to the pitch path

A drifting oscillator's spectrum has three things beside each harmonic that a constant pitch does not, all of them the pitch path's own:

- the harmonic spread over its deviation, a few cents;
- the FM skirt of the drift's tail, which falls as f⁻⁶ in power away from the harmonic (the drift's −40 dB/decade, and a further 1/f² from frequency to phase): −81 dB in a 50 Hz band 50 Hz from a 4.2 kHz fundamental at the default depth, as the narrowband estimate predicts;
- the hold's sidebands at ±k·fs/64.

The oscillator adds one more thing: the slow error of each held step. A step is a change of increment, and below `bandLimitedPitchStep` it is not band-limited, by design ([oscillator-range.md](oscillator-range.md#pitch-steps-one-band-limited-bend-deferred-to-the-next-sample)). To separate that from the rest, the test and the report render the same held pitch path at 4× the rate and decimate it with the library's `Oversampler`: that reference has the pitch path's content and practically none of the oscillator's own error or aliasing. What the 1× render has beyond it is what the oscillator adds.

## Results, as built

From the `[drift-report]` (about 2 s) and `[drift-long-report]` (0.1 s), in `relwithdebinfo` ([drift_report.cpp](../../tests/dsp/oscillators/drift_report.cpp)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[drift-report]"
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[drift-long-report]"
```

### Statistics at every sample rate

The default depth and rate (σ 1.5 cents, corner 0.5 Hz), one hour of updates. Theory: half the variance below 0.221 Hz and 99% below 1.69 Hz; the density at 0.05 / 0.5 / 5 / 50 Hz at 0 / −6.0 / −40.0 / −80.0 dB.

| Rate | Update rate | σ (cents) | Mean | Peak | Kurtosis | Half below | 99% below | Density (dB) | Above 20 Hz |
|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|
| 44.1 kHz | 689 Hz | 1.485 | 0.012 | 6.26 | 3.11 | 0.231 Hz | 1.73 Hz | 0 / −6.0 / −39.9 / −79.6 | −51.4 dB |
| 48 kHz | 750 Hz | 1.483 | 0.008 | 6.27 | 3.12 | 0.229 Hz | 1.72 Hz | 0 / −5.5 / −39.6 / −79.4 | −51.5 dB |
| 96 kHz | 1500 Hz | 1.501 | 0.011 | 6.17 | 3.10 | 0.229 Hz | 1.69 Hz | 0 / −5.8 / −40.2 / −80.1 | −51.7 dB |
| 192 kHz | 3000 Hz | 1.511 | 0.033 | 6.48 | 3.09 | 0.229 Hz | 1.74 Hz | 0 / −6.2 / −40.1 / −80.0 | −51.6 dB |

The same drift, in seconds, at every rate, and as the theory says. The peak is about 4.2σ in an hour.

**Note-ons from silence.** 100,000 notes started from silence: the starting offsets have σ 1.498 cents, mean −0.002, peak 6.13, kurtosis 2.83. That is the drift's own distribution: a note starts where a drift that had been running would be, not in tune.

**Independence.** 64 pairs of voices seeded n and n + 1, 10 minutes each: mean |r| 0.031, worst 0.092, as independent streams give (about 0.029 and 0.11).

**Ten hours** (σ 1 cent): peak 4.86σ; σ 0.989 in the first hour and 0.999 in the last; means 0.006 and −0.018; the worst one-minute mean 0.52σ; one-minute σ from 0.77 to 1.28; 2,816 ppm beyond 3σ, against a Gaussian's 2,700. Nothing wanders.

### The pitch-step threshold and the hold

A voice at 0.44 fs (21.1 kHz at 48 kHz), held per 64 samples, 60 s:

| Depth, rate | Largest change of increment / `bandLimitedPitchStep` | Changes that reach it |
|---|---:|---:|
| 1.5 cents, 0.5 Hz (default) | 0.10 | none |
| 5 cents, 2 Hz | 1.29 | 2.2 per second |
| 20 cents, 5 Hz | 15.2 | 583 per second |

At the default depth every held step takes the unbanded path, even at the top of the range. Each crossing is an exact band-limited bend, at about 15 ns: 2.2 a second costs nothing measurable.

The hold's sidebands on a sine (f ± 750k Hz, k = 1…4, relative to the carrier, 48 kHz):

| Depth, rate | 1 kHz | 5 kHz | 21.1 kHz |
|---|---:|---:|---:|
| 1.5 cents, 0.5 Hz (default) | −116.1 dB | −102.1 dB | −89.6 dB |
| 5 cents, 2 Hz | −95.1 dB | −80.8 dB | −68.2 dB |

### What the oscillator adds

A one-voice slot at fs / 10.5 (4.2 kHz at 44.1 kHz, so every alias falls halfway between two harmonics), against the same slot on the same held pitch path at 4× and decimated. The power below 20 kHz away from the harmonics (each masked by twice its peak deviation plus 20 Hz), relative to the fundamental:

| Waveform | Drift | 1× | 4× reference | Excess (what the 1× oscillator adds) |
|---|---|---:|---:|---:|
| Saw | none | −101.5 dB | −101.5 dB | under 0 (within −120 dB) |
| Saw | 1.5 cents, 0.5 Hz | −82.2 dB | −82.4 dB | −95.5 dB |
| Saw | 5 cents, 2 Hz | −67.5 dB | −67.5 dB | −93.1 dB |
| Triangle | none | −102.0 dB | −102.0 dB | −138.1 dB |
| Triangle | 1.5 cents, 0.5 Hz | −84.2 dB | −84.2 dB | −106.4 dB |
| Triangle | 5 cents, 2 Hz | −68.1 dB | −68.1 dB | −93.9 dB |

- **With no drift the reference matches the oscillator** to within −120 dB, so the reference is valid. The −101.5 dB both share is common to the 1× and the 4× render, so it is not the 1× oscillator's aliasing (most likely the window's leakage around the strong harmonics).
- **With drift, the power away from the harmonics rises by 20 to 35 dB, and the reference rises with it.** That is the pitch path's own content: the FM skirts and the hold's sidebands.
- **What the oscillator adds is −95 dB at the default depth** (saw), the slow error of held steps below `bandLimitedPitchStep`. Split into frequency bands, it is broadband (−100 to −116 dB per 2 kHz band at the default depth), not concentrated at the alias positions. It is 20 dB below the −74 dB that the bend path allows smooth modulation ([oscillator-range.md](oscillator-range.md)).

### CPU

Nanoseconds per sample, 48 kHz, `relwithdebinfo`, `tools/bench.sh "[oscillators]"` ([baselines](../performance/baselines.md)). Saws at 110–880 Hz, Detune 0.2 st, Stereo 0.5, drift at the default:

| | No drift (a note per block) | Drift held per 64 (default) | Drift added to a note per sample |
|---|---:|---:|---:|
| The generator alone | | 0.17 | 0.19 |
| 1 slot, 1 voice | 2.99 | 3.50 (+17%) | 6.52 (+118%) |
| 1 slot, 8 voices | 23.4 | 23.9 (+2%) | 35.4 (+51%) |
| **A whole note: 4 slots × 8 voices** | **103.5** | **101.5 (within noise)** | **147.2 (+42%)** |

- **Held, a whole note costs what it did.** The generator is 0.17 ns per sample, and each slot converts its note 8 times per 512 samples instead of once.
- **A lone single-voice slot pays +17% held**: 8 calls of 64 samples instead of 1 of 512, for an oscillator that costs only 3 ns per sample.
- **A note per sample costs 40–50% more**, for the `exp2` per sample per slot and the increment per unison voice. A voice that passes a note per sample for its glide or modulation pays that anyway; the drift adds 0.19 ns.
- Every other oscillator row is within noise of the parent commit, measured in the same session.

## Tests

[pitch_drift_test.cpp](../../tests/dsp/oscillators/pitch_drift_test.cpp) (`[drift]`), each tolerance derived in the test from the two-pole's statistics:

- **Off.** Off by default. With drift off a note per sample is untouched bit for bit, `process()` makes one run at exactly +0, and turning it on and off again gives 0 once more. A slot rendered through the drift's runs with drift off equals one rendered without it, bit for bit, for all 8 waveforms with unison.
- **Depth.** Over an hour at 44.1, 48, 96, and 192 kHz, depths 1.5 and 10 cents, corners 0.5 and 2 Hz: σ within 5% of the depth and the mean within 0.1σ (the estimates spread by 1.1% and 0.019σ). The depth scales the offsets and nothing else.
- **Rate.** The autocorrelation at ωτ = 0.5, 1, 2, 4 is (1 + ωτ)e^(−ωτ), within 0.08 (the estimate spreads by 0.021), at every sample rate and both corners: the rate is in seconds.
- **Spectrum.** Falls 39.5 dB from 4 to 40 times the corner (±2 dB), flat below it, half the variance below 0.442 times the corner and 1% above 3.37 times.
- **Distribution.** Kurtosis 3 ± 0.3; over an hour the peak is between 3 and 6σ.
- **Nothing accumulates.** Six hours: every hour's σ within 10% of the depth, every hour's mean within 0.1σ.
- **Seeds.** The same seed gives the same offsets; 16 pairs of adjacent seeds differ, each |r| under 0.15 and the mean under 0.06 over 10 minutes.
- **Note-ons.** 20,000 notes from silence start with σ within 3% of the depth and mean 0. A note-on while sounding changes nothing, bit for bit. From silence the grid restarts at the note-on. `reset()` restarts the sequence.
- **The hold and blocks.** The offset changes only on the grid, whatever the blocks; a note per sample through `addTo()` is bit-identical in blocks of 1, 37, and 4096; a slot with 8 voices and drift is bit-identical in blocks of 1, 37, and 4096.
- **Controls** clamp, NaN is safe, and the defaults are 1.5 cents and 0.5 Hz. **No subnormal** offsets at the smallest and largest depths and rates, at every sample rate; below the smallest depth it is exactly 0. **No allocation** in any call.
- **A voice's oscillators drift together:** two slots of one voice, one inverted, cancel to float rounding under a 20-cent drift; two voices with adjacent seeds differ. **Noise** is unaffected, bit for bit.
- **No aliasing beyond the pitch path's own:** a saw and a triangle at fs / 10.5, against the 4× reference above. Without drift the reference matches to 10⁻¹¹ of the fundamental; with drift the 1× render's excess stays under −90 dB at the default depth and −85 dB at 5 cents and 2 Hz (measured −95.5 and −93.1), and the excess at the alias positions is no larger than the excess overall.

[oscillator_range_test.cpp](../../tests/dsp/oscillators/oscillator_range_test.cpp): **the slot stress test now runs with drift**, at 20 cents and 5 Hz (13 times the default depth and 10 times its rate), added per sample through `addTo()` and told of every note-on. It is finite, under the same bound, free of subnormals, and bit-identical in blocks of 1, 37, and 4096 at all four rates for all 8 waveforms.

**Every new test was checked to fail first.** Against an inert stub with the same interface, 10 of the 19 drift tests failed; the other 9 pin behaviour the stub shares (off, Noise, blocks), and the breaks below check them. Then 16 deliberate breaks in `PitchDrift`, each caught, each restored:

| Break | Tests that failed |
|---|---|
| off not special-cased (runs of 64 at 0) | off |
| the depth scaled by 1.41 | depth; nothing accumulates; distribution; note-ons |
| the update rate taken as the sample rate (no /64) | depth; rate; spectrum; distribution; accumulation; seeds |
| the update rate fixed at 48 kHz / 64 | rate (at the other sample rates) |
| a one-pole instead of the two-pole | depth; rate; spectrum; distribution; accumulation |
| no leak on the second pole (a random walk) | depth; rate; spectrum; distribution; accumulation; seeds |
| a note from silence starts at 0 | note-ons |
| a note-on while sounding redraws | note-ons |
| the grid not restarted at a note-on | note-ons |
| the grid restarted at every `process()` | the hold and blocks; the slot with drift and blocks; the slot stress test (56 block-size comparisons) |
| the seed ignored | seeds; voices drift apart; spectrum |
| no minimum depth | no subnormals; controls |
| a NaN rate not caught | controls |
| a jitter of 0.5 cent on each held run | no aliasing beyond the pitch path's own; the hold and blocks; the slot with drift and blocks; depth scaling; spectrum |
| an allocation at a note-on | no allocation |

The Noise test and the oscillators-cancel half of the voice test check the composition (the slot ignores the note for Noise; one drift drives a voice's slots), which no break in `PitchDrift` can change.

The first version of the aliasing test compared the strongest line away from the harmonics with and without drift, and failed at −78 dB: the drift's FM skirt, which a fixed mask did not cover. Measured by bands it was the pitch path's own content. Hence the 4× reference.

## References

- LennarDigital, [Sylenth1 version history](https://lennardigital.com/sylenth1/whatsnew.php), v3.072, 2022; the developer's and users' posts in KVR threads [589147](https://www.kvraudio.com/forum/viewtopic.php?t=589147) and [590403](https://www.kvraudio.com/forum/viewtopic.php?t=590403). Sources and open questions: the [feature inventory](../reference/sylenth1-features.md#drift).
- G. E. Uhlenbeck and L. S. Ornstein, "On the Theory of the Brownian Motion", *Physical Review* 36, 1930: the one-pole (Ornstein–Uhlenbeck) process.
- N. J. Kasdin, "Discrete Simulation of Colored Noise and Stochastic Processes and 1/f^α Power Law Noise Generation", *Proceedings of the IEEE* 83(5), 1995: filtered noise, its discrete form, and its statistics.
- R. F. Voss and J. Clarke, "'1/f noise' in music: Music from 1/f noise", *Journal of the Acoustical Society of America* 63(1), 1978; M. Gardner, "Mathematical Games: White and brown music, fractal curves and one-over-f fluctuations", *Scientific American* 238(4), 1978: the Voss algorithm for 1/f noise.
- P. Horowitz and W. Hill, *The Art of Electronics*, 3rd ed., Cambridge, 2015, ch. 2: the Ebers–Moll model and the temperature dependence of an exponential converter.
- J. O. Smith III, *Mathematics of the Discrete Fourier Transform*, online book, W3K Publishing, 2007: beating, and Welch's method for power spectra.
- B. C. J. Moore, *An Introduction to the Psychology of Hearing*, 6th ed., 2012, ch. 6: frequency discrimination.
