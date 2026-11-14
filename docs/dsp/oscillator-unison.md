# Oscillator unison: detune, stereo spread, and the voice count

Each of Sylenth1's four oscillators can play up to 8 **unison voices**: copies of itself, spread in pitch by **Detune** and across the stereo field by **Stereo**, with **Voices** 0 turning the oscillator off (inventory [§2](../reference/sylenth1-features.md#unison)). This is the "supersaw" sound: a stack of slightly detuned saws that beat against each other and sound wide and thick. This page covers:

- why detuned copies sound big, and how the level of a stack grows with the voice count;
- the laws a unison needs: how voices are spaced in pitch, how the knob maps to that, how voices are placed in stereo, how the oscillator's Pan combines with the spread, and how loud the stack is;
- what is known about Sylenth1's laws, and the working hypotheses built here;
- noise with unison, as one source mixed to sound like many;
- how it is built, measured, and tested, and what Phase 3 needs from the layout.

Read [oscillator-controls.md](oscillator-controls.md) and [oscillator-phase.md](oscillator-phase.md) first.

## Theory

### Why detuned copies sound big

Two oscillators at frequencies f₁ and f₂ = f₁ + Δf sum to a tone whose amplitude swells and fades at the **beat rate** |Δf|: sin a + sin b = 2 cos((a − b)/2) sin((a + b)/2). Every harmonic k beats too, at k·Δf, so the upper harmonics move faster than the fundamental. With N voices there are N(N − 1)/2 pairs, each beating at its own rate. The spectrum stops being a set of steady lines and becomes clusters of lines whose amplitudes wander independently. The ear hears that as **chorus**: one thick, moving tone rather than N separate ones. A slow spread (a few cents) gives a gentle shimmer; a wide one (a semitone or more) a dissonant, detuned cloud.

Detune can be linear in **hertz** (every voice offset by a fixed number of Hz) or in **semitones** (every voice offset by a fixed frequency ratio). In hertz, the beat rate is the same on every key, so low notes sound detuned and high notes barely move relative to their pitch. In semitones, the beat rate scales with the pitch, so the stack keeps the same character across the keyboard. Synthesizer detune is almost always in semitones (or cents), and the measurements of Sylenth1 say it is too (see [below](#sylenth1-what-is-known)).

### Spacing the voices

With the outermost voices at ±s semitones, the inner voices can be:

- **evenly spaced:** voice k of N at s·(2k/(N − 1) − 1). Simple, symmetric, and every neighbouring pair beats at the same rate ratio. With odd N the centre voice is in tune.
- **unevenly spaced:** for example the Roland JP-8000's Super Saw, 7 saws whose analysed offsets at full detune are roughly ±2%, ±6%, and ±11% of the frequency, the inner pair much closer together than the outer (Szabo, 2010). Close inner voices give slow beating near the pitch; wide outer ones give the brightness.
- **random:** each voice at a random offset within the spread, per patch or per note. Natural-sounding, but not repeatable.

The spread itself is usually a **curved** function of the knob, so that the useful small detunes get most of the knob's travel. A quadratic curve gives a quarter of the maximum at half the knob; the JP-8000's curve is close to a high-order polynomial, flat at first and rising steeply at the end.

Some synths also make the maximum depend on the voice count. More voices fill the same spread more densely, so a smaller spread can sound as wide.

### Placing the voices in stereo

A unison stack is widened by panning the voices apart. Each voice gets a pan position, and each position is converted to a pair of channel gains by a pan law ([oscillator-controls.md](oscillator-controls.md#pan-laws)). A **width** scales the positions from all-centre to fully spread. Two questions decide the image:

- **Which voice goes where.** If the lower-pitched voices all went left and the higher ones right, the stack would sound lopsided, with its pitch centroid different in each ear. Pairing each voice at −d with the voice at +d on the opposite side keeps both sides balanced: each ear hears the same spread of pitches.
- **How positions combine with the oscillator's own pan.** The voices can be **offset** by the pan, with positions past the hard sides clipped, so the stack moves and squashes against the side. The pan can act as a **balance** on the finished stereo pair, which fades the far side and so loses level. Or the spread can be **squeezed** into the room left between the pan and the nearer side, so the image keeps its shape but narrows to nothing at hard pan.

### How loud a stack is

Each voice's waveform adds to the others'. How they add depends on their phases:

- **In phase** (all voices identical: retriggered at the same phase, with no detune): the samples add exactly, so N voices are N times one voice: +6.02 dB per doubling, and N times the peak.
- **Independent phases** (free-running, or detuned): the cross terms average to zero, so the **power** adds: N voices have N times the power of one, +3.01 dB per doubling. The peak is lower than N times one voice and varies.

A synth can leave this alone (each voice at full level), or normalise by **1/N** (constant peak in phase, but a free-running stack gets quieter as voices are added: −9 dB at 8) or by **1/√N** (constant power for independent voices, but an in-phase stack still grows by +9 dB at 8). The JP-8000 has a separate Mix knob that turns the centre saw down as the detuned ones come up.

### Noise: one source that sounds like many

Detune and phase mean nothing to white noise. N independent noise voices, each added into the bus with its gains (lₖ, rₖ), are a stereo noise whose left and right channels have the covariance

  C = [ Σlₖ²  Σlₖrₖ ]
      [ Σlₖrₖ  Σrₖ² ]

times the power of one voice. A pair of channels with the same covariance comes from just **two** independent sources n₁, n₂, mixed by any matrix S with S·Sᵀ = C. The symmetric square root (S = Sᵀ, S² = C) mixes both sources into both channels. For a 2×2 matrix it has a closed form (by Cayley–Hamilton, C² = tr(C)·C − det(C)·I):

  S = (C + √det C · I) / √(tr C + 2√det C).

Two draws per sample then give exactly the power of each channel and the left/right correlation that N separately panned noise voices would, at any width and pan, for N up to 8. At Stereo 0 the voices are all centred, det C = 0, and both channels are the same signal. At full width with voices split evenly, C is diagonal and the channels are independent.

## Sylenth1: what is known

The manual (v2.5, pp. 14–15) gives the controls only: Voices 0–8 (0 turns the oscillator off), Detune spreads the voices' pitch, Stereo spreads them across the stereo field (it "needs more than 1 voice"), and Retrig restarts "every voice". Everything else comes from third-party sources, with their confidence:

| Topic | What the source says | Source | Confidence |
|---|---|---|---|
| Detune law | Linear in semitones, not hertz; a 5-voice example at ±2 semitones: −2, −1, 0, +1, +2. | BlitBit, [KVR thread 518035](https://www.kvraudio.com/forum/viewtopic.php?t=518035&start=15), 10 Jan 2019 | Low–medium: one poster, whose graphs cannot be viewed |
| Knob curve and maximum | About 3.3219 · x² semitones each way for knob position x ∈ [0, 1]; about 0.8 semitones at half the knob. The voice count is not stated. | Same post | Low–medium |
| Maximum against the voice count | "The maximum detune is dependent on the number of voices." Sine, Detune and Stereo at maximum, A = 110 Hz: with 8 voices the highest is at about 125 Hz (about 2.2 semitones); with 2 voices it is "far away from 125 Hz". | BlitBit, same thread, 12 Jan 2019 | Low: read from a graph |
| Stereo order | At Stereo 10, every voice but the centre one is hard left or right: C, LR, LCR, LRLR, LRCLR, LRRLLR, LRLCRLR, LRRLRLLR for 1 to 8 voices. | Same post | Low–medium |
| Stereo order, read by pitch | Every order is antisymmetric: position k and position N + 1 − k are always on opposite sides, and the first is always L. That fits each order listing the voices from the lowest pitch to the highest (the poster was reading coloured spectra), with each ±d pair split left and right. | Our reading of the orders | Medium for the pattern, low for the reading |
| Level | "Sylenth gives all voices the same amplitude." | Same post | Low–medium |
| Level against the count | With Retrig on and Detune 0, "weakest with 1 voice, loudest with 8": identical voices add. Rules out 1/N; does not separate none from 1/√N. | secretfound and phreaque, [KVR thread 452081](https://www.kvraudio.com/forum/viewtopic.php?p=6319341), Dec 2015 | Medium |
| Free-running phases | The start phase of each voice seems random when free-running; the poster guesses Sylenth1 constrains the randomness. | BlitBit, 12 Jan 2019 | Low (a guess) |
| Pan with Stereo, the Stereo knob's scaling, the pan law per voice, noise | No source. | | None |

The cross-post in the LennarDigital forum ([thread 518038](https://www.kvraudio.com/forum/viewtopic.php?f=94&t=518038)) repeats the first summary, with no reply from the developer.

## The laws built here (decided 2026-11-14)

Each is a **working hypothesis**, recorded with the demo measurement that settles it in the inventory (O8, O13–O16, M1.13). Each is one table or one function in [unison.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/unison.hpp), so a measurement that disagrees changes that, not the slot. The DSP takes physical units; the knob laws belong to the parameter table (M1.9).

| Law | As built | Where |
|---|---|---|
| **Detune** | Linear in semitones, evenly spaced: voice k of N at s·(2k/(N − 1) − 1), lowest first, where s (`setDetune`) is the outermost voice's offset in semitones. | `unisonLayout(N).detune`, `OscillatorSlot::setDetune` |
| **Detune knob** (parameter table) | s = 3.3219 · x² · g(N): BlitBit's curve, with a voice-count factor g from 1 at 2 voices to 0.66 at 8 (from 2.2/3.32), linear in N between. At Detune 10: 3.32, 3.14, 2.95, 2.76, 2.57, 2.39, 2.20 semitones for 2 to 8 voices. | M1.9 |
| **Stereo order** | BlitBit's orders, by pitch from the lowest voice: each ±d pair split left and right, the lowest voice left. | `unisonLayout(N).side` |
| **Stereo width** | Position = Pan + width × side, with width ∈ [0, 1] (`setStereo`); the Stereo knob maps linearly (knob / 10). | `unisonPosition` |
| **Pan law per voice** | Equal power (sin/cos), as for Pan (O6). | `panGains` |
| **Pan with Stereo (O8)** | An **offset**: each voice moves with Pan, and positions past ±1 are clipped. | `unisonPosition` |
| **Level** | Every voice at full level; no normalisation for the count. | `OscillatorSlot` |
| **Retrig** | Every voice restarts at Phase. Free-running voices from silence each draw their own seeded random phase, in voice order. | `OscillatorSlot::noteOn` |
| **Noise** | One source per slot: two draws per sample, mixed by the symmetric square root of the voices' covariance. Voices and Stereo apply; Detune, Phase, and Retrig do not. | `unisonNoiseMix`, `OscillatorSlot` |

**Pan with Stereo, by example.** Two voices at Stereo 10 and Pan +0.5: the lower voice moves from −1 to −0.5 and the upper one from +1 to +1.5, clipped to +1. Under the equal-power law the lower voice's gains are 0.924 left and 0.383 right. A balance on the pair would give 0.5 and 0, and the squeeze would give 0.707 and 0.707. Those three numbers are what O8's measurement reads from the demo.

**Headroom.** With no normalisation, the worst case is 8 voices in phase, panned hard: each voice at unity on one channel. A saw's band-limited peak is 1.44 (the minimum-phase step overshoots), so the channel peaks at 11.4 (+21 dBFS); the narrow pulses at high pitch reach 13.1. Inside the voice everything is float, so nothing clips. Where 0 dBFS lands for a typical patch is settled with the mixer in M1.4 (inventory O5).

## How it is applied here

### The pieces

| Piece | Where | What it does |
|---|---|---|
| `maxUnisonVoices`, `UnisonLayout`, `unisonLayout(N)` | [unison.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/unison.hpp) | The detune fractions and stereo sides of N voices, as a `constexpr` table built from the reported order strings. The detune fractions are exactly antisymmetric, so ±d pairs are mirror images bit for bit. |
| `unisonPosition(pan, width, side)` | unison.hpp | O8: Pan + width × side, clipped; the width clamped to [0, 1]; NaN is 0 and the centre. |
| `NoiseMix`, `unisonNoiseMix(voices, inverted)` | unison.hpp | The 2×2 symmetric square root above, in float. det C is clamped at 0 against rounding; only s² enters S², so an error in s cancels. |
| `OscillatorSlot<Taps>` | [oscillator_slot.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/oscillator_slot.hpp) | Up to 8 voices, each a `MinBlepOscillator` and a `StereoGain`, plus one `WhiteNoise` and three `LinearSmoother`s for the noise mix. New controls: `setVoices`, `setDetune`, `setStereo`. New queries: `voices()`, `soundingVoices()`, `isVoiceRunning(voice)`, `phaseIncrement(voice)`, `voiceGains(voice)`. |

### Per-voice state

The slot holds fixed arrays for the maximum of 8 voices: the oscillators, their gains, their detune ratios, and a small state per voice (off, on, or fading out). Nothing is allocated after construction, and the whole slot is about 5 KB at 64 taps (504 bytes per oscillator, 128 per `StereoGain`).

- **Pitch.** Each voice's detune is a frequency **ratio**, 2^(offset/12), computed when the detune or the voice count changes (8 `exp2` at most). A block's pitch is converted once, `noteToFrequency(note + tuning)`, and each voice plays it times its ratio. With a pitch per sample, the centre frequencies of a 64-sample chunk are converted once and shared by the voices, so 8 voices cost one `exp2` per sample, not eight. A voice with no detune has a ratio of exactly 1, so a 1-voice slot is bit for bit what it was before unison.
- **Gains.** Each voice's `StereoGain` folds volume, invert, and its own pan position into one gain per channel, ramped over 10 ms. A control change sets the targets of the current voices: `setPan` and `setStereo` move positions, `setVolume` and `setInverted` the shared parts. The `sin` of the pan law runs only then.
- **Rendering.** Per 64-sample chunk, each sounding voice renders into a mono buffer on the stack and is added into the bus with its own gains. Voices are rendered one at a time, so each oscillator's state stays in registers for its whole chunk.

### Changing the voice count

A voice count can change while a note sounds (an automated Voices knob, or a preset change). The slot keeps it click-free:

- **Removed voices fade out** over the gain ramp (10 ms), then their oscillators stop. `soundingVoices()` counts them until the ramp ends.
- **Added voices fade in.** If a note is held, they start as a note-on would start them (at Phase, or at their own random phase), from a gain of 0. A voice caught while still fading out is ramped back up and keeps its oscillator.
- **Voices that stay keep running.** Their detune and position change to the new layout: the pitch at once (the phase is continuous, so it does not click), the gains over the ramp.
- **Before the first sample** after a note-on, `prepare()`, `reset()`, or `stop()`, nothing has been heard, so changes are immediate: a slot given 7 voices between a note-on and its first sample renders exactly as one prepared with 7 (tested).
- **When a fade ends is exact.** A voice counts as finished from the sample its ramp lands, not from the next chunk boundary, so the output never depends on how the block is split.

### Noise

With Noise selected, the oscillators are not rendered at all. The slot draws two values per sample from its `WhiteNoise` and mixes them with the three ramped coefficients. The coefficients are recomputed with every control change, from the current voices' target gains. Invert negates them, so it is still the exact negation. With 0 voices the mix ramps to 0 and then nothing is drawn. The sample at which that happens is exact, so a render is the same however it is split into blocks. The noise is seeded with the slot's seed XOR a fixed constant (`noiseSeedSalt`), so it is reproducible and independent of the phase draws.

### SIMD across voices (Phase 3)

The manual's note that "one oscillator with 4 voices is cheaper than 4 oscillators with 1 voice" (p. 30) suggests Sylenth1 vectorises across unison voices. This task keeps one scalar `MinBlepOscillator` per voice, so `restart()` and `jumpTo()` are reused unchanged, and leaves the vectorised form to Phase 3. What it found:

- **The slot's interface does not need to change.** A lane-parallel oscillator bank can replace the array of oscillators behind `setVoices`, `setDetune`, and the per-voice gain arrays.
- **A shared ring position.** Every voice advances its residual ring by one per sample, so a bank can keep one position and interleave the rings as [taps][voices]: reading every voice's next residual is one 8-wide load. But `restart()` sets its ring position to 0. A per-lane restart would have to clear that lane's column instead.
- **Corners are per lane.** Each voice crosses its corners at different samples, so the one-compare fast path becomes "did any lane cross", with a masked slow path. At high pitch some lane crosses on almost every sample, so the gain there is small.
- **Scalar state that needs a lane form:** the 64-bit distance to the next corner (`untilCorner`), the sine's phase history, and the pending-jump record.
- **Gains vectorise already.** Each voice's gain loop runs over 64 samples and vectorises across samples.

A cheaper Stereo 0 path, summing all voices in mono and applying one gain, was tried and dropped. It changes float rounding at the point where the gains settle, and that point would then depend on the block split. The existing guarantee that output is independent of the block size would break.

## Trade-offs and alternatives

| Alternative | Why not (or not yet) |
|---|---|
| **Constant maximum detune for every voice count** (g = 1) | Simpler, but contradicts the only direct statement we have ("dependent on the number of voices"). The factor lives in the parameter table, so either answer is a one-line change after M1.13. |
| **Constant spacing between neighbours** (the outer voices move out as voices are added) | The wrong direction: the 8-voice maximum is reported smaller, not larger. |
| **JP-8000-style uneven spacing** | Evidence for a different synth. Even spacing matches the one Sylenth1 example. A different spacing is a new row in the layout table. |
| **Detune linear in hertz** | Contradicted by the measurements, and a stack would change character across the keyboard. |
| **Alternating sides (LRLR…)** | Contradicted by the reported orders for 6 and 8 voices. |
| **Width as mid/side on the summed stack** | Cheaper (one pair of gains), but it cannot place individual voices, so O8's offset could not be expressed. |
| **Pan as a balance, or a squeeze** (O8) | Equally cheap. The offset is the simplest implementation and keeps each voice's power constant. The measurement above tells them apart. |
| **Normalising by 1/N or 1/√N** | 1/N is ruled out by the 2015 report; 1/√N is not, and O15 separates it from none (+3.01 against +6.02 dB per doubling in phase). |
| **8 real noise voices** | Exact, at 8 draws per sample instead of 2, for a difference only the amplitude histogram could show (8 uniform sources sum to nearly Gaussian noise; two give a trapezoidal mix). |
| **Noise ignoring Voices and Stereo** | Cheapest, but Voices 8 would then be quieter than 8 saws, and Stereo would do nothing to it. |
| **A summed-mono fast path at Stereo 0** | See above: it breaks block-size independence. |
| **Unison inside `MinBlepOscillator`** | It would make the mono source stereo and mix control logic into the per-sample code. The slot keeps the oscillator a single voice. |

## Results

From the `[unison-report]` run ([unison_report.cpp](../../tests/dsp/oscillators/unison_report.cpp), under a second in `relwithdebinfo`), 64 taps, no oversampling.

### Pitches

48 kHz, sines at A2 (110 Hz), Detune 10 by the provisional knob law. Each voice's increment, and its line in a 2¹⁸-point FFT, against its target in double:

| Voices | Spread (st) | Outer voice (Hz) | Offsets (st, lowest first) | Worst increment error (cents) | Worst measured (cents) |
|---:|---:|---:|---|---:|---:|
| 1 | — | 110.00 | 0 | 0.00007 | 0.009 |
| 2 | 3.32 | 133.27 | ±3.32 | 0.00002 | 0.008 |
| 3 | 3.13 | 131.83 | ±3.13, 0 | 0.00007 | 0.011 |
| 4 | 2.95 | 130.40 | ±2.95, ±0.98 | 0.00013 | 0.010 |
| 5 | 2.76 | 128.99 | ±2.76, ±1.38, 0 | 0.00011 | 0.011 |
| 6 | 2.57 | 127.60 | ±2.57, ±1.54, ±0.51 | 0.00011 | 0.008 |
| 7 | 2.38 | 126.22 | ±2.38, ±1.59, ±0.79, 0 | 0.00014 | 0.009 |
| 8 | 2.19 | 124.85 | ±2.19, ±1.57, ±0.94, ±0.31 | 0.00014 | 0.010 |

The voices play their pitch to within 0.00014 cents (the detune ratio's `exp2` and one multiply in float, on top of the slot's own pitch accuracy). The measured column is limited by the FFT's peak interpolation, not by the oscillator.

### Stereo image

8 sines, Detune 1.5 st, Pan centre: each voice's left and right amplitude read from the spectrum.

| Width | Sides (lowest first) | Left amplitudes | Worst error against the law |
|---:|---|---|---:|
| 0 | LRRLRLLR | all 0.707 | 1.1·10⁻⁸ |
| 0.5 | LRRLRLLR | 0.924 on the left voices, 0.383 on the right | 1.4·10⁻⁸ |
| 1 | LRRLRLLR | 1 and 0 | 3.3·10⁻⁶ |

### Level

Saw at 55 Hz, Stereo 0, in dB relative to one voice. In phase: Retrig on, Detune 0. Free-running: Retrig off, Detune 0.3 st, the mean power of 32 seeds.

| Voices | In phase: RMS | In phase: peak | Free-running: RMS | Free-running: worst peak |
|---:|---:|---:|---:|---:|
| 1 | 0 | 1.02 | 0 | 1.02 |
| 2 | +6.02 | 2.03 | +3.16 | 1.93 |
| 4 | +12.04 | 4.06 | +6.00 | 3.02 |
| 8 | +18.06 | 8.12 | +9.15 | 4.57 |

Exactly +6.02 dB per doubling in phase, and +3 dB per doubling free-running, as the theory predicts with no normalisation. Peaks of one channel, 8 voices in phase, Pan hard right: saw 11.4 at A1 and 10.4 at C7; square 11.5; HPulse 11.5 at A1 rising to 13.1 at C9; TriSaw 9.7.

### Aliasing

44.1 kHz, 8 saws, Detune 0.5 st, Retrig on: the worst non-harmonic line below 20 kHz, relative to one voice's fundamental, against the worst of the 8 voices rendered alone:

| Note | Worst single voice (dB) | 8 voices (dB) | Difference |
|---:|---:|---:|---:|
| 60 | −132.9 | −127.2 | +5.7 |
| 84 | −115.5 | −113.3 | +2.2 |
| 96 | −109.8 | −109.8 | 0 |
| 102 | −106.6 | −104.3 | +2.3 |
| 108 | −103.9 | −103.9 | 0 |
| 114 | −100.6 | −98.3 | +2.3 |

The stack adds no aliasing of its own: at every bin it is within the coherent sum of its voices' spectra (tested). Where it reads higher than one voice, two voices' aliases share a bin. Retriggered at the same phase, they add in phase, up to +6.02 dB.

### Noise

One source per slot, 2¹⁷ samples. The RMS of each channel and the left/right correlation, against N uniform noise voices at the voices' gains:

| Voices | Width | RMS (left) | Expected | Correlation | Expected |
|---:|---:|---:|---:|---:|---:|
| 1 | any | 0.407 | 0.408 | 1.000 | 1 |
| 4 | 0 | 0.815 | 0.816 | 1.000 | 1 |
| 4 | 0.5 | 0.815 | 0.816 | 0.706 | 0.707 |
| 4 | 1 | 0.815 | 0.816 | −0.002 | 0 |
| 8 | 0 | 1.152 | 1.155 | 1.000 | 1 |
| 8 | 1 | 1.153 | 1.155 | −0.002 | 0 |

### CPU

Nanoseconds per sample, 64 taps, 48 kHz, `relwithdebinfo`, two runs in one session ([baselines](../performance/baselines.md)). Saw, Detune 0.2 st, Stereo 0.5:

| Voices | 440 Hz | per voice | 12 kHz | per voice |
|---:|---:|---:|---:|---:|
| 1 | 3.30–3.69 | 3.3–3.7 | 13.2–13.5 | 13.2–13.5 |
| 2 | 6.60–6.76 | 3.3–3.4 | 27.6–37.1 | 13.8–18.5 |
| 4 | 13.3–13.9 | 3.3–3.5 | 52.9–58.6 | 13.2–14.7 |
| 8 | 25.1–28.6 | 3.1–3.6 | 106–115 | 13.2–14.4 |

- **A voice costs one oscillator and its gain:** 3.1–3.7 ns at 440 Hz. Nothing is shared between voices yet; that is the Phase 3 work above.
- **A whole note,** 4 oscillators of 8 voices (110–880 Hz) with their stereo gains, costs 102–105 ns per sample: 0.5% of one core. That is within 10% of 32 bare mono oscillators (92–93 ns).
- **0 voices** costs 0.13–0.15 ns per sample (70 ns per block). **Noise** at 8 voices costs 2.2–2.4 ns: two draws and four multiply-adds, whatever the count.
- **A pitch per sample** with 8 voices adds about 11 ns per sample: one `exp2` per sample, shared by the voices, plus each voice's multiply and increment. For one voice it got cheaper than before: 6.1–7.0 ns against 8.3–8.9, because a chunk's frequencies are now converted in one loop.
- **A one-voice slot** is about 4% slower settled and 13% slower while a gain ramp runs than before unison (3.28 against 3.16 ns, and 4.72 against 4.17, in a standalone probe of the same loop). That is the voice loop's state checks, and the gain's call through the array.

## Tests

- [unison_layout_test.cpp](../../tests/dsp/oscillators/unison_layout_test.cpp):
  - detune fractions for 1–8 voices are evenly spaced from −1 to +1 (to 10⁻⁷), exactly antisymmetric, with an odd count's centre voice at exactly 0;
  - stereo sides match the reported orders, each ±d pair on opposite sides, the lowest voice left; out-of-range counts clamp to 8;
  - `unisonPosition` offsets by Pan, clips at ±1, clamps the width, and treats NaN as 0 and the centre;
  - `unisonNoiseMix` squares to the voices' covariance for 200 random gain sets (to 2·10⁻⁶ of the total power); one voice hard left is exactly the left source; 8 centred voices give equal coefficients (identical channels); 4 left and 4 right give independent channels of twice the amplitude; Invert negates exactly; no voices is silence.
- [unison_slot_test.cpp](../../tests/dsp/oscillators/unison_slot_test.cpp):
  - **pitches:** every voice's increment for 1–8 voices at spreads 0, 0.05, 1, 3.32, 12, and −2 semitones is within 2·10⁻⁶ (plus the fixed-point unit) of its double-precision target; with no detune, or a NaN detune, all voices play the 1-voice pitch exactly;
  - **rendered pitches and levels:** in an FFT of 2–8 sine voices, every line is within 0.05 cents of its voice's increment, and every voice has amplitude 0.707 per channel at Stereo 0 (to 2·10⁻³). Two voices at Stereo 10 are one per channel, and `measureFrequency` reads each to 10⁻⁸, the lower voice on the left;
  - **stereo gains:** for 1–8 voices, 3 widths, 5 pans, and both polarities, every voice's gains are exactly volume × the equal-power law at `unisonPosition`; O8's case gives 0.924 / 0.383 and 0 / 1; the rendered left and right amplitude of each of 8 voices matches its gains (to 2·10⁻³);
  - **level:** with Retrig on and no detune, N voices are N times one voice (to 10⁻⁶) for saw, square, and sine;
  - **Retrig on:** a note-on from silence and a retrigger while sounding match bare oscillators at the slot's increments and gains, each restarted or jumped to Phase (to 10⁻⁶), for saw, triangle, and sine;
  - **Retrig off:** each voice from silence starts at the slot's seeded random phase, drawn in voice order (to 10⁻⁶); a second note-on leaves every voice running; the same seed renders bit for bit the same, another does not. Over 64 seeds, two free-running voices' correlation averages 0 (within 0.2; σ = 0.056) and is never 1; 8 free-running voices have 8 times one voice's power (within 20%); retriggered, the two channels are identical;
  - **noise:** one voice hard left is exactly every other draw of the slot's noise sequence, the right channel exactly 0; for 4 combinations of voices, width, and pan, each channel's power is within 3% of N voices' and the correlation within 0.02 of theirs; Stereo 0 gives identical channels; Stereo changed on its own afterwards decorrelates the channels; Detune, Phase, Retrig, note-ons, and the note change nothing (bit for bit); Inv negates exactly; 0 voices is silence; and the noise does not depend on the block size, through 0 voices and back (the draws stop at the exact sample the mix reaches 0);
  - **0 voices** renders exact silence for saw, sine, and noise, with no sounding voices and no oscillator ever run (nothing is rendered); counts clamp to 0–8;
  - **voice-count changes mid-note** (4 → 8 → 3 → 0 → 6 voices): no allocation; no second difference above the smooth bound (7·10⁻³, against 0.01 or more for a click); removed voices sound for the fade, then stop and their oscillators are no longer run; 0 voices after its fade is exact silence with no oscillator running; voices brought back sound;
  - a voice brought back halfway through its fade-out keeps its oscillator and ramps back up, with no click;
  - a voice count set between a note-on and its first sample renders exactly as a slot prepared with it, retriggered or free-running;
  - **aliasing:** at MIDI 90, 100, and 108 at 44.1 kHz, the 8-voice stack is, at every unmasked bin, within the coherent sum of its voices rendered alone; its worst line is within 6.02 dB of the worst single voice's, and below −88 dB;
  - **extremes:** 8 voices at spreads 0, 3.32, ±48, 10⁶, NaN, and ±∞ semitones, at notes −100 to 140, for saw, HPulse, sine, and noise, panned hard: finite, below 8 × 2.7, no subnormal samples, and exact silence after fading to 0 voices;
  - a pitch per sample equals the per-block pitch bit for bit, and the output does not depend on the block size (1 to 3000, with a voice-count change at a fixed sample);
  - **no allocation** across every control, note-on, process call, `stop()`, `reset()`, and `prepare()` (with the [allocation counter](processor-lifecycle.md#tests));
  - the lifecycle contract, with 8 voices, note-ons every 300 samples, and the voice count changing every 700, retriggered and free-running, for saw and noise.
- The existing [oscillator_slot_test.cpp](../../tests/dsp/oscillators/oscillator_slot_test.cpp) runs unchanged on the 1-voice default, including its bit-for-bit comparisons with the bare oscillator. Only "Noise is silent until unison" is gone, replaced by the noise tests above.
- [unison_report.cpp](../../tests/dsp/oscillators/unison_report.cpp) (`[unison-report]`, hidden) prints the tables above.

Every new test was checked to fail with a deliberate break in the code it covers: 24 breaks, run against the unison and slot tests and then restored. The first pass caught 15 of them. Of the other 9, two breaks did not compile and were rewritten. Six exposed gaps, each closed by a new test or check: a fading voice brought back, the noise's block-size independence, Stereo alone updating the noise, the faded and the 0-voice oscillators no longer being run (hence `isVoiceRunning`), and a NaN detune. The last break, `stop()` keeping fading voices, changed nothing observable, so that code was redundant and was removed. All 23 remaining breaks are caught.

## References

- LennarDigital, *Sylenth1 User Manual* v2.5, 2015, pp. 14–15 (the controls) and p. 30 (unison cost). Sources and open questions: the [feature inventory](../reference/sylenth1-features.md#unison).
- BlitBit, "Sylenth1 – Unison Detune Spread?", [KVR Audio forum, thread 518035](https://www.kvraudio.com/forum/viewtopic.php?t=518035&start=15), January 2019: the detune, stereo, and level measurements.
- [KVR Audio forum, thread 452081](https://www.kvraudio.com/forum/viewtopic.php?p=6319341), "Question: 1 voice or 8 voices?", December 2015: level against the voice count.
- A. Szabo, *How to Emulate the Super Saw*, KTH, 2010: the JP-8000 Super Saw's detune offsets, knob curve, and mix law, measured. Read here through a [third-party port](https://gist.github.com/audionerd/fe50790b7601cba65ddd855caffb05ad) of its formulas; the thesis PDF at KTH is no longer online.
- M. A. Gerzon, "Panpot Laws for Multispeaker Stereo", AES Convention 92, 1992: constant-power panning.
- J. O. Smith III, *Mathematics of the Discrete Fourier Transform*, online book, W3K Publishing, 2007: beating, and sums of sinusoids.
- G. H. Golub, C. F. Van Loan, *Matrix Computations*, 4th ed., Johns Hopkins, 2013, ch. 9 (functions of matrices): matrix square roots (the 2×2 closed form follows from Cayley–Hamilton).
