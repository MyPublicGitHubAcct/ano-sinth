# Oscillator range: from 0.01 Hz to beyond Nyquist

Sylenth1's oscillators are specified as "stable from below 0.01 Hz up to Nyquist" (inventory [§2](../reference/sylenth1-features.md#2-oscillators-pp-5-1415)). This page covers:

- what "stable" has to mean at each end of that range, and why each end is hard;
- what an oscillator should do when its pitch goes *past* Nyquist, which happens easily (a high note, a positive octave, and the widest unison detune push the top voices above fs/2);
- what a pitch *step* does to a band-limited oscillator;
- how this codebase handles each of these, the measurements, and the tests that pin them down.

Read [band-limited-oscillators.md](band-limited-oscillators.md), [oscillator-waveforms.md](oscillator-waveforms.md), [oscillator-phase.md](oscillator-phase.md), and [oscillator-unison.md](oscillator-unison.md) first.

## What "stable" means here

An oscillator slot (`OscillatorSlot`, every waveform, 0–8 unison voices) is stable over its range when all of these hold, at 44.1, 48, 96, and 192 kHz:

1. **It reaches the range.** Any pitch from 0 Hz up arrives through the slot's note input. 0.01 Hz is note −115.79 at A4 = 440 Hz.
2. **Its pitch is exact to the fixed-point floor**, measured over whole cycles, down to 0.01 Hz.
3. **Its output is finite and under a derived peak bound**, under the harshest conditions it can meet:
   - per-sample sweeps from 0.01 Hz to beyond Nyquist and back, a fast vibrato across Nyquist, and pitch steps;
   - note-ons at random sub-sample times with Retrig on and off;
   - voice-count changes in the middle of fades, Inv toggling, and the widest detune and stereo spread.
4. **No subnormal numbers** appear, in the output or in the oscillators' state.
5. **The output does not depend on the block size**, bit for bit.
6. **Nothing accumulates.** DC, level, and pitch do not drift over hours.
7. **Above Nyquist it plays the band-limited ideal**, the waveform's mean, and gets there without a click.
8. **A pitch step is band-limited**, so it does not click.

## Theory

### At the bottom: what goes wrong at very low pitches

- **Float phase resolution.** A phase kept as a float in [0, 1) has steps of 2⁻²⁴ ≈ 6·10⁻⁸ just below 1. At 0.01 Hz and 48 kHz the increment is 2.1·10⁻⁷ cycles, only 3 or 4 of those steps. So each addition rounds by up to half a step: a pitch error of tens of percent, which also changes across the cycle. Double precision moves the problem far down, but it accumulates rounding over hours and costs a lot on an FPU without doubles (a Cortex-M4F).
- **Evaluating a phase in float.** Even with the phase stored exactly, converting a phase near 1 to a float to evaluate the waveform rounds it to 2⁻²⁴ cycles. That is an error in *value* of up to slope × 2⁻²⁵ per sample, not an error in pitch, because it doesn't accumulate.
- **Accumulated residuals.** Some oscillators keep a running sum: a BLIT integrated into a saw, or a leaky integrator. Every rounding error stays in that sum. With a leak, it shows as a DC that depends on history; without one, it is a drift. Long low notes, with few edges to renormalise against, show it most.
- **Denormals.** Recursive state that decays toward zero passes through the subnormal range, which is slow on x86 ([denormals.md](denormals.md)). An oscillator with no recursion can still produce tiny products, such as a residual tap times a tiny height.
- **Long periods.** At 0.01 Hz a cycle is 100 s: 4.8 million samples at 48 kHz, 19.2 million at 192 kHz. Pitch and phase have to be measured over whole cycles, so the tests are long, and an hour of simulated time is needed to show the absence of drift.

### At the top: above Nyquist

A periodic waveform with fundamental f₀ is a sum of harmonics at k·f₀ (k = 1, 2, …) plus its mean, the DC term. Band-limiting it to fs/2 keeps the harmonics below fs/2 and drops the rest. Once f₀ itself is above fs/2, every harmonic is dropped, and **only the mean is left**. The ideal is silence for the waveforms with no DC (saw, square, triangle, sine, TriSaw), and a constant for those with one: the 25% HPulse at −0.5 and the 12.5% QPulse at −0.75.

An oscillator can't simply run on above fs/2, because a phase step of more than half a cycle per sample is indistinguishable from a step *backwards* of less than half a cycle. A fundamental at 0.6 fs would come out as one at 0.4 fs. That is aliasing of the fundamental itself. Something has to be done instead. The options, measured, are below.

### Pitch steps: a bend in time

With a continuous phase, a pitch step doesn't make the waveform jump. It changes its **slope in time** at once: a straight segment with slope s per cycle runs at s·c₀ per sample before the step and s·c₁ after it. That is a corner in time of height b = s·(c₁ − c₀), a *bend*. Like any bend, it has a spectrum that falls only as 1/f² and aliases unless it is band-limited ([oscillator-waveforms.md](oscillator-waveforms.md)).

minBLEP makes it worse. The straight segments are computed undelayed, and the kernel's delay (2.79 samples at 64 taps) is put back by subtracting slope × delay from them ([band-limited-oscillators.md](band-limited-oscillators.md)). When the slope changes, that correction changes at once with it, while the true delayed signal turns the corner only `delay` samples later. Left alone, a pitch step is therefore a **step of b × 2.79 in the output**: a click.

For example, a glide-less legato jump from 1 kHz to 2 kHz at 48 kHz gives a triangle step of 0.23. A step from 0.01 Hz to 0.42 fs gives 5.4, against the triangle's normal peak of 1. The cure is the one used for every other bend: add the band-limited ramp residual (BLAMP) at the step. The BLAMP table already contains the delay compensation, so the correction then turns the corner smoothly with the rest.

Square and pulse waveforms are flat between their edges, so they have no slope to bend. The sine is evaluated from its own phase history at the delayed time, so it follows any pitch path exactly.

## How it is applied here

### Reaching 0.01 Hz: the note input is enough

The slot takes its pitch as one fractional note per block or per sample (`process(out, note)`, `process(out, notes)`). It adds the tuning and converts the sum once with `noteToFrequency` at the master tune ([oscillator-controls.md](oscillator-controls.md)). Bend, portamento, pitch modulation, and the master tune (M1.4, M1.5) all arrive as part of that note.

- 0.01 Hz at A4 = 440 Hz is note 69 + 12·log₂(0.01/440) = **−115.79**. A float holds a note of that size to 7.6·10⁻⁶ semitones.
- The tuning alone reaches only about 0.64 Hz (MIDI 0, −3 octaves, −7, −1 fine), but the note input goes lower, down to 0 Hz (note −∞).

So no pitch-offset or frequency input is needed. A test drives the slot from note −115.79 and below at every rate.

### The fixed-point phase, and its floor

The phase is a 32-bit integer, 2³² per cycle ([phase.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/phase.hpp)). It wraps by itself, exactly, and has the same resolution everywhere in the cycle, so the float-phase problem never arises. The increment is truncated, so a pitch is up to one unit, fs/2³², flat. At 0.01 Hz the floor is:

| Rate | One unit | Increment at 0.01 Hz | Floor |
|---|---:|---:|---:|
| 44.1 kHz | 1.03·10⁻⁵ Hz | 973 | 1.78 cents |
| 48 kHz | 1.12·10⁻⁵ Hz | 894 | 1.93 cents |
| 96 kHz | 2.24·10⁻⁵ Hz | 447 | 3.87 cents |
| 192 kHz | 4.47·10⁻⁵ Hz | 223 | 7.72 cents |

These figures are for a 0.01 Hz LFO-like oscillator. At 1 Hz and above the floor is under 0.08 cents at every rate.

### Evaluating the phase in float

The waveform is evaluated in float from the integer phase: `toCycles(phase − segmentPhase)` for the straight segments, and `sin(2π · toCycles(delayedPhase))` for the sine. Near the end of a cycle, `toCycles` rounds to 2⁻²⁴ cycles. Measured at 0.01 Hz over a whole cycle, against the waveform evaluated in double at the exact delayed phase, the error is at most 6.0·10⁻⁷ for the sine (−124 dB) and under 9·10⁻⁸ (−141 dB) for the straight segments. It is a rounding of each sample's value, so it doesn't accumulate.

### Nothing accumulates

`MinBlepOscillator` has no running sum. Its whole state is:

- the integer phase;
- a ring of 64 residual taps, each read once and then cleared;
- 8 samples of sine phase history (integers);
- a handful of flags and segment values, recomputed at every corner.

So whatever ran before (hours of a note, jumps, pitch changes), the output is a function of the last 72 samples' phases alone. An oscillator restarted at the same phase 72 samples earlier gives the same output, bit for bit, after any history. That is the long-run guarantee: no rounding error, DC, or level can build up. The test checks exactly this, and the report measures it over hours.

### Denormals

The residual table's entries and the heights they are scaled by are normal floats, so a product could only be subnormal if both were tiny. Measured under the full stress run, neither the output nor any residual in flight is ever subnormal. A test enforces this, and checks the state through the read-only `MinBlepOscillator::residuals()` and `OscillatorSlot::oscillator(voice)`. A residual made subnormal on purpose fails it.

### Above Nyquist: the fade about the mean

Chosen from the options below:

- `nyquistFadeGain(increment)` ([min_blep.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/min_blep.hpp)) is 1 up to `nyquistFadeStart` (0.45 cycles per sample), then ½(1 + cos(π·x)) across the band to 0 at Nyquist, and 0 above it.
- `WaveShape::mean()` ([waveform.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/waveform.hpp)) is the waveform's mean over a cycle, worked out from its segments in double when the shape is built: −0.5 for the HPulse, −0.75 for the QPulse, 0 for the rest.
- `MinBlepOscillator` computes the gain g and the offset (1 − g)·mean whenever the increment or the shape changes. Increments still clamp at Nyquist, where g is 0. While g < 1, each output becomes offset + g·y.

On the per-sample path this is **one predictable branch** on a member flag, `finishing`, which is false for any pitch below 0.45 fs once an onset has settled. The fade and the onset's ringing (next point) share that branch, in `finish()`.

**The onset still starts at 0.** A voice that starts from silence inside the band or above Nyquist must not switch its offset on as a hard step: that would be a DC click of up to 0.75. So for the first 64 samples after an onset the offset is scaled by 1 + r(n), r being the table's step residual. The mean thus switches on with the same band-limited step as the waveform. The existing test that every waveform's first sample after an onset is 0 caught this before the fix.

Above Nyquist the output is **exactly** the mean, from the first sample. 0·y is exactly 0, and the offset is exactly the mean.

### Pitch steps: one band-limited bend, deferred to the next sample

`MinBlepOscillator::setIncrement` compares the new increment with the old one. A change of `bandLimitedPitchStep` (2¹⁸ units, 6.1·10⁻⁵ cycles, 2.9 Hz at 48 kHz) or more while the oscillator runs is a pitch step.

- **Deferred to the next sample.** The bend is added on the next sample, in the corner scan (`scanCorners()`), on whichever segment that sample is in, with height slope × (c_new − c_old). A `jumpTo()` between the same two samples may have moved the phase to another segment. Computing the bend at the call would put it on the segment *before* the jump, which peaked at 9.0 on the triangle under random jumps. Either order of the two calls now gives the same output, bit for bit, and a test checks it.
- **No new per-sample work.** The scan is forced by zeroing `untilCorner`, as `jumpTo()` does, so the per-sample path gains no branch. `setIncrement` gains one subtraction and one compare.
- **Smaller changes are left as they are.** They are off by at most |slope| × change × delay, 6.8·10⁻⁴ (−63 dB) at the triangle's slope, decaying within a few samples. Smooth per-sample modulation changes the increment by less than that each sample: an ordinary 6 Hz vibrato at 440 Hz by about 10⁻⁶ cycles. So it adds no residuals and costs what it did. Its slow error stays: a wobble at the modulation rate, −74 dB below 18 kHz on the saw for a 13 Hz vibrato across Nyquist, which is the worst case measured.
- **Why not band-limit every change?** A residual on every sample costs 64 multiply-adds. The first version used a threshold 8 times lower, which a fast per-sample vibrato (±1 semitone at 384 Hz) crossed on every sample: the bare oscillator went from 4.2 to 17 ns per sample, and an 8-voice slot from 36 to 135. The project owner chose the higher threshold (PLAN.md, Decisions log).

### Note-ons at a sub-sample time

`OscillatorSlot::noteOn(since)` takes the note-on's position before the next sample, in [0, 1), for events finer than a sample (an arpeggiator step, a sync source). It hands `since` to every voice's `MinBlepOscillator::jumpTo(Phase, since)`.

- **A sounding voice** jumps at that time.
- **A voice starting from silence** has nothing to jump from. Its phase moves on by since × *the first sample's* increment. That increment isn't known when `noteOn()` is called, because the slot sets each voice's pitch in `process()`. So `jumpTo()` from silence now stores `since`, and the onset's scan applies it. Before, it used the increment in force at the call.

## Above Nyquist: the options and the decision

Measured by the `[range-report]` at commit 9fef36a, against the oscillator as it was then, with each alternative prototyped on top of it. That commit's report reproduces every figure in this section.

### What the clamp did

Before the fade, `incrementFor` and `MinBlepOscillator::setIncrement` clamped any pitch above fs/2 to an increment of exactly half a cycle, and the oscillator played that increment. At that increment the phase alternates between two values, so the output is periodic in 2 samples: exactly **dc + a·(−1)ⁿ**, a stationary tone at Nyquist. Its level a depends on which two phases the oscillator lands on, which depends on where it was when the pitch crossed fs/2.

Measured for every waveform at a Nyquist increment. The voice was started at 256 phases, and separately arrived at by a per-sample sweep from 0.45 fs to 0.55 fs from 256 random phases. Measured after 128 samples, over 4096. In cycles per sample, every rate is the same.

| Waveform | Band-limited ideal | DC | Nyquist tone: min / median / max over start phases | Max, arriving by a sweep | Peak |
|---|---|---:|---:|---:|---:|
| Saw | silence | 0 | −52.7 / −12.9 / −9.9 dBFS | −9.9 dBFS | 0.68 |
| Square | silence | 0 | −46.7 / −6.9 / **−3.9** | −3.9 | 1.37 |
| HPulse | −0.5 | −0.500 | −49.7 / −9.9 / −6.9 | −6.9 | 1.48 |
| QPulse | −0.75 | −0.750 | −55.1 / −15.2 / −12.3 | −12.3 | 1.28 |
| Triangle | silence | 0 | −50.6 / −10.8 / −7.8 | −7.8 | 0.87 |
| Sine | silence | 0 | −47.3 / −3.0 / **0.0** | 0.0 | 2.00 |
| TriSaw | silence | 0 | −59.6 / −14.8 / −11.8 | −11.8 | 0.55 |

So a clamped voice had the right DC, but on top of it a tone at fs/2 at up to full scale, where the ideal has nothing. Its level was decided by chance. The sine is the worst case, because the minBLEP kernel doesn't filter it: it is evaluated directly, at a delayed phase.

### Level as the fundamental approaches Nyquist, before the fade

Below fs/2, the minBLEP output is the waveform filtered by the kernel, whose transition band is centred on Nyquist. So every straight-segment waveform already rolls off over the last few percent before it. RMS about the mean, relative to the ideal fundamental (above fs/4 the band-limited waveform is its fundamental alone), 48,000 samples:

| Waveform | 0.30 fs | 0.40 | 0.45 | 0.47 | 0.48 | 0.49 | 0.495 | 0.499 fs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Saw, square, HPulse, QPulse, triangle, TriSaw | 0.0 dB | 0.0 | 0.0 | −0.2 | −0.8 | −2.6 | −4.1 | −5.6 |
| Sine | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

The kernel is flat to 0.45 fs. The sine never rolls off, so its full level ran straight into the clamp.

### The options

Each option was prototyped as a gain on one minBLEP voice's output, as a function of its target pitch, and measured two ways:

- **Above Nyquist:** the RMS about the waveform's mean once the target has been above fs/2 for 10 ms. The ideal is 0.
- **10 Hz–18 kHz:** the power in that band about the mean, relative to a full-scale sine. The tone itself never falls in this band, so it is what the option's transitions put into the audible range.

Each was run on two pitch paths at 48 kHz, worst of 16 start phases, for the saw, the HPulse (DC −0.5), and the sine:

1. a sweep, linear in pitch, from 0.40 fs to 0.60 fs and back, about 1 s each way;
2. a 13 Hz vibrato of ±1 semitone around 0.485 fs (0.458 to 0.514 fs), crossing Nyquist 26 times a second.

| Option | Sweep: above Nyquist (saw / HPulse / sine) | Sweep: 10 Hz–18 kHz | Vibrato: 10 Hz–18 kHz |
|---|---|---|---|
| Clamp | −10.0 / −7.0 / −0.2 dB | −95.4 / −92.4 / −118.3 dB | −66.8 / −91.7 / −110.4 dB |
| Mute above Nyquist, at once | 0 / −6.0 / 0 (linear) | −75.8 / **−43.9** / −66.1 | −46.1 / **−9.6** / −36.4 |
| Mute above Nyquist, 10 ms ramp | 0 / −6.0 / 0 | −95.4 / **−45.2** / −115.6 | −67.8 / **−10.6** / −88.2 |
| Fade the output, 0.45 fs → 0.5 fs | 0 / −6.0 / 0 | −96.7 / **−62.0** / −119.4 | −73.7 / **−11.5** / −125.3 |
| **Fade about the mean, 0.45 fs → 0.5 fs** | **0 / 0 / 0** | **−96.7 / −93.7 / −119.4** | **−73.7 / −95.8 / −125.3** |

"0" in the first column means silence about the mean, exactly the ideal. The HPulse's −6.0 dB under the mutes and the plain fade is its DC of −0.5 being removed, which the ideal keeps. The fade is a raised cosine of the pitch, gain = ½(1 + cos(π·(c − 0.45)/0.05)) for c = f/fs in the band.

- **Clamp.** Click-free, because nothing switches. But it leaves the stationary Nyquist tone shown above.
- **Mute at once.** Clicks on every crossing.
- **Mute with a ramp, and fading the whole output.** Clean for the DC-free waveforms. But both take the pulses' DC with the AC. Each crossing then steps the DC (−45 dB), and a vibrato across the band wobbles it at the vibrato rate (−10 to −12 dB in band, clearly audible).
- **Fade about the mean.** Output = mean + g·(y − mean). The DC never moves, so the pulses are as clean as the rest, and above Nyquist the output *is* the band-limited ideal. Because it is a pure function of the pitch, it holds no state to depend on the block size.

The saw's −67 to −74 dB under the vibrato is not the option's: it is the oscillator's own slow error under per-sample pitch changes (see Pitch steps), a wobble at the vibrato rate. It is the same for every option, give or take the fade's attenuation of the voice.

### Downstream: filters and distortion

The filter (M1.3) and the effects (M1.7) don't exist yet, so this is measured with a stand-in and reasoned about from their planned designs.

**A driven saturator at 4× oversampling.** A 220 Hz saw at full scale plus one voice of the same waveform targeted at 1.1·fs/2, summed and driven through tanh(drive·x) inside the library's `Oversampler` at 4×, 48 kHz. The figure is the power below 20 kHz that is not a harmonic of 220 Hz, relative to the 220 Hz fundamental: intermodulation between the note and the upper voice.

| Upper voice | Drive 1 | Drive 4 | Drive 16 |
|---|---:|---:|---:|
| Muted (the note alone: every mute or fade option) | −81.7 dB | −84.1 dB | −48.2 dB |
| Saw, clamped (a loud start phase) | −49.3 | −39.1 | −33.2 |
| Saw, clamped (a quiet start phase) | −65.4 | −61.3 | −48.2 |
| HPulse, clamped | −47.3 | −37.8 | −32.0 |
| Sine, clamped | −41.5 | −31.3 | −25.4 |

A Nyquist tone on its own only folds back onto DC and Nyquist through a static nonlinearity. Mixed with a note, though, it produces sum and difference tones at fs/2 ± k·f, which land across the audible band, here up to 53 dB above what the note alone produces.

**Filters.** A TPT (zero-delay-feedback) lowpass discretised by the bilinear transform has a zero at exactly z = −1, so it nulls an exact fs/2 tone. The planned BP, HP, and Bypass modes pass it, though. The filter's drive stages sit before and inside the ladder, so they intermodulate it exactly like the saturator above. Each clamped voice also uses up to 1.0 of headroom, times up to 8 unison voices.

Under any of the mute or fade options, the filter and the effects see only what an ideal band-limited oscillator would give them.

### The decision

**Fade about the mean, from 0.45 fs to Nyquist, with a raised cosine; above Nyquist a voice plays its waveform's mean alone.** Chosen by the project owner on 2026-11-14 from these measurements (PLAN.md, Decisions log).

- The fade band starts at 0.45 fs: 19.8 kHz at 44.1 kHz, 21.6 kHz at 48 kHz. That is where the kernel's own roll-off begins, so the fade only completes a roll-off the straight-segment waveforms already have. For the sine, it adds that roll-off.
- At 96 and 192 kHz the band is far above hearing.

How it is built is described under **How it is applied here** above, and what it measures under **Results** below.

## Results, as built

From the `[range-report]` (about 3 s) and `[range-long-report]` (about 40 s), in `relwithdebinfo` ([oscillator_range_report.cpp](../../tests/dsp/oscillators/oscillator_range_report.cpp)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[range-report]"
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[range-long-report]"
```

### Above Nyquist and through the fade band

Every waveform at 0.55 fs, after 0.3 fs, from 256 start phases: the output equals `mean()` **exactly**, on every sample of 4096 after the crossing (largest deviation 0).

The level through the band, RMS about the mean relative to the ideal fundamental (dB, 48 kHz):

| | 0.45 fs | 0.46 | 0.47 | 0.475 | 0.48 | 0.49 | 0.495 | 0.499 fs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| The fade's gain alone | 0.0 | −0.9 | −3.7 | −6.0 | −9.2 | −20.4 | −32.2 | −60.1 |
| Sine (not filtered by the kernel) | 0.0 | −0.9 | −3.7 | −6.0 | −9.2 | −20.4 | −32.2 | −60.1 |
| Every other waveform (kernel × fade) | 0.0 | −0.9 | −3.9 | −6.4 | −10.1 | −23.0 | −36.3 | −65.7 |

Below 0.45 fs nothing changes: 0.0 dB at 0.30 and 0.40 fs, as before.

The range test's two paths at 48 kHz, worst of 16 start phases: the power from 10 Hz to 18 kHz about the mean, relative to a full-scale sine, and the largest |output − mean| once the target has been above Nyquist for 10 ms:

| Waveform | Sweep 0.40 → 0.60 → 0.40 fs | 13 Hz vibrato across Nyquist | Above Nyquist |
|---|---:|---:|---:|
| Saw | −95.4 dB | −74.3 dB | 0 |
| Square | −94.9 | −95.1 | 0 |
| HPulse | −94.0 | −94.9 | 0 |
| QPulse | −94.3 | −93.2 | 0 |
| Triangle | −89.2 | −82.5 | 0 |
| Sine | −99.9 | −107.1 | 0 |
| TriSaw | −94.3 | −79.7 | 0 |

The sloped waveforms' vibrato figures are the slow error of per-sample pitch changes below `bandLimitedPitchStep`: a wobble at 13 Hz, not a click.

**Downstream.** The same saturator as above (220 Hz saw plus one voice, tanh at 4× oversampling): a voice above Nyquist now adds nothing. A saw or sine at 0.55 fs gives exactly the note alone's −81.7 / −84.1 / −48.2 dB at drive 1 / 4 / 16. The HPulse, whose DC shifts the curve, gives −83.1 / −86.5 / −51.3. The clamp gave −25 to −65 dB here.

A sine still *sounding* near Nyquist intermodulates as any ultrasonic content would: −29 dB at 0.47 fs (faded by 3.7 dB) and −53 dB at 0.44 fs, at drive 1. That is the signal a band-limited oscillator really has there. Keeping it out of the audible band is the job of the saturator's oversampling and of the filter, not the oscillator's.

### The bottom of the range

A sine through the slot at note −115.79 (0.01 Hz), Retrig on at Phase 359, rendered for 1.01 cycles. Its two upward zero crossings are compared with where the integer phase puts them:

| Rate | Increment | Period | First crossing, error | Second, error | Pitch error | Floor |
|---:|---:|---:|---:|---:|---:|---:|
| 44.1 kHz | 973 | 4,414,149.3 samples | −0.002 samples | +0.002 | −1.628 cents | 1.777 cents |
| 48 kHz | 894 | 4,804,214.0 | −0.040 | −0.034 | −1.519 | 1.934 |
| 96 kHz | 447 | 9,608,428.0 | −0.081 | −0.066 | −1.519 | 3.865 |
| 192 kHz | 223 | 19,259,943.0 | −0.562 | −0.594 | −5.397 | 7.722 |

- **Period.** The crossings land within 0.6 samples of the fixed-point prediction, out of up to 19 million per cycle: 3·10⁻⁸ of a cycle. Each crossing is limited by the delayed sine's float resolution near the wrap. 2⁻²⁴ cycles is 256 phase units: 0.29 samples at 48 kHz (increment 894), 1.15 at 192 kHz (increment 223).
- **Pitch.** Always flat, and always within one fixed-point unit of the target, as the floor predicts.

The float evaluation at 0.01 Hz and 48 kHz, over one whole cycle, against the waveform in double at the exact delayed phase:

| Waveform | Saw | Triangle | Sine | TriSaw |
|---|---:|---:|---:|---:|
| Largest error | 8.8·10⁻⁸ (−141 dB) | 6.3·10⁻⁸ (−144 dB) | 6.0·10⁻⁷ (−124 dB) | 6.4·10⁻⁸ (−144 dB) |

### Long runs

**12.4 hours at 0.01 Hz.** A bare oscillator at 48 kHz (increment 894, whose phase sequence repeats only after 2³¹ samples), run for 2³¹ samples. Every whole cycle, from one wrap of the integer phase to the next, was measured:

| Waveform | Cycles | DC, first / last | RMS, first / last | Largest change from the first cycle: DC / RMS | Cycle lengths |
|---|---:|---:|---:|---:|---:|
| Saw | 446 | 1.8·10⁻⁸ / −1.9·10⁻⁷ | 0.5773502 / 0.5773501 | 2.1·10⁻⁷ / 1.2·10⁻⁷ | 4,804,213–4,804,214 |
| Sine | 446 | 4.7·10⁻¹² / 1.9·10⁻¹² | 0.7071068 / 0.7071068 | 1.4·10⁻¹¹ / 7.4·10⁻⁸ | 4,804,213–4,804,214 |
| HPulse | 446 | −0.5000000 / −0.5000000 | 0.8660253 / 0.8660254 | 1.0·10⁻⁷ / 6.0·10⁻⁸ | 4,804,213–4,804,214 |

The ideal RMS values are 1/√3 = 0.5773503, 1/√2 = 0.7071068, and √3/2 = 0.8660254. Every cycle is 4,804,213 or 4,804,214 samples long, as 2³²/894 = 4,804,214.0 says. The changes are float rounding of a cycle's samples, with no trend.

**One hour near Nyquist.** A bare oscillator at 20 kHz, 48 kHz (0.417 fs, below the fade band), for 172.8 million samples. The second second (the first holds the onset) against the last:

| Waveform | DC | RMS | Pitch (sine, from zero crossings) |
|---|---:|---:|---:|
| Saw | 7.6·10⁻⁸ / 5.8·10⁻⁸ | 0.4500318 / 0.4500334 | |
| Sine | 1.7·10⁻⁸ / 1.6·10⁻⁸ | 0.7071068 / 0.7071068 | 19,999.99939 / 19,999.99962 Hz (increment: 19,999.99952) |

The saw's RMS varies between 0.4500317 and 0.4500342 from one second to the next, over the first 600 seconds as over the last. That is the sample grid sliding across its band-limited edges, not a trend. The sine's measured pitch is within 1.2·10⁻⁸ of its increment's: the measurement's limit at 2.4 samples per cycle.

### Peaks under the stress run

| Waveform | One voice, note-ons | One voice, random jumps | The slot (up to 8 voices, full width) |
|---|---:|---:|---:|
| Saw | 1.43 | 1.55 | 4.60 |
| Square | 2.15 | 2.33 | 5.29 |
| HPulse | 2.20 | 2.28 | 5.51 |
| QPulse | 1.91 | 2.03 | 4.90 |
| Triangle | 1.34 | 1.47 | 4.60 |
| Sine | 1.26 | 1.58 | 3.26 |
| Noise | | | 2.00 |
| TriSaw | 1.26 | 1.29 | 4.60 |

- **One voice** is a bare oscillator on the stress pitch path at −3.32, 0, and +3.32 semitones. It is retriggered to phase 0 at each note-on event at its sub-sample time, and in a second run jumped to a random phase every 1 to 16 samples. Largest |sample| over the four rates.
- **The worst single voice is 2.33**, inside the 2.61 measured under random jumps at fixed pitches ([oscillator-phase.md](oscillator-phase.md)).
- **Without band-limited pitch steps** the same run measured 5.77 (the triangle, at the path's step from 0.01 Hz to 0.42 fs). With the first, pre-jump version of the bend it measured 9.03.
- **The bound.** The test bounds the slot by 8 voices × 2.7 = 21.6. Each voice is under 2.7, the fade only moves a sample toward a mean of at most 0.75, each channel's gain is at most 1, and at most 8 oscillators sound, counting those fading out. Noise is at most 8 voices' power, √(2·8) = 4 per channel.

## Trade-offs and alternatives

- **Where the fade starts.** At 0.45 fs the kernel's own roll-off is just beginning (−0.2 dB at 0.47 fs). A later start (0.48 fs) would keep more of a 21–22 kHz fundamental at 44.1 kHz, but make the fade steeper, so fast modulation across it would put more into the band. An earlier one would dull 19–20 kHz fundamentals for no audible gain. The start is one constant, `nyquistFadeStart`.
- **A raised cosine, not a straight line.** The gain's slope is 0 at both ends, so a sweep into the band has no corner in its envelope. The test pins the shape. A linear fade, tried as a deliberate break, fails it.
- **Fading in the slot instead of the oscillator.** That would leave the bare oscillator clamping, and it would have to repeat the pitch in a second place. The fade belongs with band-limiting: a band-limited oscillator *is* its mean above Nyquist.
- **Skipping the work of a voice above Nyquist.** Such a voice outputs a constant, but it still runs at a Nyquist increment, adding residuals it then multiplies by 0: 14 ns per sample. Skipping that while keeping its phase is a Phase 3 item. The fade itself costs nothing measurable below 0.45 fs.
- **Band-limiting every pitch change** would make smooth modulation exact too, at 64 multiply-adds a sample whenever modulation is fast. The threshold makes steps exact and leaves smooth modulation's slow error, −74 dB at worst as measured. A different structure, evaluating the straight segments at the delayed phase from a history (as the sine is), would make every pitch path exact with no residuals. It would change the core loop for every waveform, so it is left for Phase 3 to measure.
- **Rounding the increment rather than truncating** would halve the fixed-point floor, to 3.9 cents at 0.01 Hz and 192 kHz. Truncation keeps `incrementFor` monotonic and branch-free, and the floor at 0.01 Hz matters only for an LFO-like use of an oscillator. Left as is ([oscillator-controls.md](oscillator-controls.md)).
- **A 64-bit phase** would make the floor negligible everywhere, at the cost of 64-bit arithmetic on every sample (slow on a Cortex-M4F) and wider sine history. Not needed: the 32-bit floor is under 0.08 cents from 1 Hz up.
- **A frequency input on the slot**, for pitches below what notes reach. Notes reach 0 Hz already, and every pitch source adds in notes, so it would be a second path to keep exact.

## CPU

Nanoseconds per sample, 64 taps, 48 kHz, `relwithdebinfo`, `tools/bench.sh "[oscillators]"`. The parent and this commit were measured alternately in one session ([baselines](../performance/baselines.md)).

| Case | Parent | This commit |
|---|---:|---:|
| Saw 440 Hz, constant pitch | 3.03–3.06 | 2.90 |
| Saw / square / TriSaw 12 kHz | 12.4–12.6 / 23.8–23.9 / 33.8 | 7.9–8.4 / 13.9–15.3 / 18.7–19.2 |
| Saw in the fade band (0.47 fs) / above Nyquist (0.6 fs) | 24.6 / 22.9 (the clamp) | 14.0–14.7 |
| Saw 440 Hz, a 6 Hz vibrato set every sample | 4.90 | 4.77 |
| Saw 440 Hz, a fast vibrato (±1 semitone at 384 Hz) set every sample | 4.22–4.23 | 4.25–4.36 |
| Saw 440 Hz, a pitch step every 64 samples | 3.28 | 3.45–3.54 |
| Saw, 20 Hz to 20 kHz in one block (a step on most samples) | 7.14 | 15.2 |
| Unison slot, 8 voices, 440 Hz, pitch per sample | 35.7–36.1 | 39.0–41.1 |
| Unison slot, 8 voices, 12 kHz | 107.6–107.8 | 68.7–69.9 |

- **Constant pitch is unchanged below 0.45 fs.** The fade and the onset's ringing share one predictable branch, and the pitch-step check is in `setIncrement`, not in the per-sample loop.
- **High pitches are 25–45% faster.** `addResidual()` now adds each residual in two contiguous runs, up to the ring's end and on from its start, instead of through a wrapping index, so clang vectorises it. That helps every corner and every jump.
- **A pitch every sample costs about 0.4 ns more per voice** (+3–15%): the change and fade checks in `setIncrement`. With the first, lower threshold, the fast vibrato row measured 17 ns, and the project owner chose the threshold that keeps it at 4.3.
- **A pitch step costs one residual**, about 15 ns, when it happens. A sweep that steps on most samples (10 octaves in 512 samples) pays it almost every sample.
- **A voice above Nyquist still costs a Nyquist-rate saw** (14 ns, less than the clamp's 23 thanks to the vectorised adds), although it outputs a constant (see Trade-offs).

## Tests

[oscillator_range_test.cpp](../../tests/dsp/oscillators/oscillator_range_test.cpp) (`[range]`), with the shared stress run in [range_support.hpp](../../tests/dsp/oscillators/range_support.hpp):

- **The Nyquist fade's law:** 1 up to 0.45 fs, 0 at and above Nyquist (raw increments up to 2³² − 1), a raised cosine against double across the band (1001 points, to 10⁻⁶), never rising, and continuous.
- **Above Nyquist, every waveform plays exactly its mean**, bit for bit, from the first sample. This holds at 4 rates, at targets from 0.5 fs to 10³⁰ fs, after a note below Nyquist, and across a jump. Infinity is the mean too.
- **Through the band, the sine's level is the fade's gain** (a least-squares fit at its exact frequency, to 10⁻⁵), and every waveform keeps its mean (to 3·10⁻⁴) from 0.30 fs to 0.499 fs.
- **A sweep and a fast vibrato across Nyquist** put less than the measured worst + 6 dB into 10 Hz–18 kHz, for every waveform from two start phases.
- **The slot reaches 0.01 Hz and below through its note input.** At 4 rates, the increment is the target to within the derived bound, at 0.01, 0.0123, 0.001, and 10⁻⁵ Hz. The tuning reaches it too.
- **At 0.01 Hz, the slot's period and phase are its increment's, over whole cycles.** At 44.1 and 48 kHz, both upward zero crossings land within 0.5 samples of the integer phase's prediction, and the pitch is within one fixed-point unit of the target. 96 and 192 kHz take 10 and 20 million samples a cycle, so the report measures them.
- **Nothing accumulates.** For every waveform at four pitches (0.01 Hz, 0.0123 fs, 0.3125 fs, and 0.47 fs, in the fade band), after 20 random jumps and 100,000 samples, the phase has moved by exactly 100,000 × the increment, mod 2³². The next 56 samples equal, bit for bit, those of an oscillator restarted at the same phase 72 samples earlier.
- **The whole slot under stress**, at 4 rates, all 8 waveforms including Noise, in blocks of 4096, 37, and 1, with these events at random samples:
  - note-ons at random sub-sample times;
  - Retrig flipped;
  - the voice count changed between 0 and 8, often mid-fade;
  - Inv toggled.

  The pitch path holds at 0.01 Hz, sweeps per sample to 0.6 fs (beyond Nyquist even before the ±3.32-semitone detune), back down, then steps to a 40 Hz vibrato around 0.485 fs. The test checks that:
  - the output is finite and under the derived bound;
  - no output sample and no residual in flight is subnormal;
  - the output is bit-identical for every block size.

[band_limited_oscillator_test.cpp](../../tests/dsp/oscillators/band_limited_oscillator_test.cpp), at 32 and 64 taps:

- **A pitch step is exactly a band-limited bend.** For saw, triangle, and TriSaw, mid-segment steps between 10⁻⁴ and 2·10⁻³ cycles per sample leave a deviation from the ideal delayed trajectory equal to b·(R(n) + min(n − delay, 0)), from the table's ramp residual, to 2·10⁻⁶. The sine's deviation is 0.
- **A pitch step and a jump between the same two samples**, in either order, give the same output bit for bit (200 random pairs per waveform).
- **A step from 0.01 Hz to 0.42 fs** never exceeds the oscillator's own peak, from 16 phases.

[waveform_test.cpp](../../tests/dsp/oscillators/waveform_test.cpp): every waveform's `mean()` is its ideal mean, and a shape beyond the table, a sine, and Noise give theirs.

[oscillator_slot_test.cpp](../../tests/dsp/oscillators/oscillator_slot_test.cpp): `noteOn(since)` is the bare oscillator's `jumpTo(Phase, since)` bit for bit, from silence and while sounding.

**Every new test was checked to fail with a deliberate break** in the code it covers (13 breaks, each restored afterwards):

| Break | Tests that failed |
|---|---|
| `mean()` returns 0 | waveform mean; above Nyquist |
| a linear fade | fade law; through the band |
| a fade that takes the DC (no offset) | above Nyquist; through the band; sweep and vibrato |
| no fade (the old clamp) | above Nyquist; through the band |
| pitch steps not band-limited | pitch step |
| the bend on the segment before a jump | pitch step (the either-order check) |
| the onset's advance at the jump call, not the first sample | pitch step; slot sub-sample note-on |
| the residual ring not cleared after a read | nothing accumulates; slot stress |
| the increment rounded up a unit | reaches 0.01 Hz; period and phase at 0.01 Hz |
| Phase off by 1/4096 cycle | period and phase at 0.01 Hz |
| subnormal residuals | slot stress |
| a fade-out's end checked per block, not per chunk | slot stress (block-size independence) |
| `noteOn()` ignoring `since` | slot sub-sample note-on |

The sweep-and-vibrato test is not a pitch-step detector. Smooth vibrato sits below `bandLimitedPitchStep` by design, and the pitch-step test covers steps.

The existing test that every waveform starts from silence at exactly 0 caught the fade's first version, which switched its offset on as a hard step.

## References

- J. O. Smith III, *Spectral Audio Signal Processing*, online book, W3K Publishing, 2011: sampling, aliasing, and the Fourier series of periodic waveforms.
- E. Brandt, "Hard Sync Without Aliasing", ICMC 2001: minBLEP, and minBLAMP for bends.
- V. Zavalishin, *The Art of VA Filter Design*, rev. 2.1.2, Native Instruments, 2020, ch. 3: the bilinear transform and its zero at Nyquist for TPT lowpasses.
- T. Stilson and J. O. Smith, "Alias-Free Digital Synthesis of Classic Analog Waveforms", ICMC 1996: BLIT, and the leaky integrators whose accumulated error this design avoids.
- D. Goldberg, "What Every Computer Scientist Should Know About Floating-Point Arithmetic", *ACM Computing Surveys* 23(1), 1991: float spacing near 1, and why a float phase accumulator drifts.
