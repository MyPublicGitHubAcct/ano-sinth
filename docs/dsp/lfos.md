# LFOs

A low-frequency oscillator (LFO) is a modulation source: a slow, repeating (or random) control signal that moves a destination such as pitch (vibrato), volume (tremolo), cutoff (a filter sweep or a wobble), or pan. Sylenth1 has two, each with a waveform, a rate, a gain, an offset, and a Free switch (inventory [§6, LFOs](../reference/sylenth1-features.md#lfos)). This page covers:

- what an LFO has to get right that an audio oscillator doesn't, and the other way round;
- what is known about Sylenth1's LFOs;
- the candidate models prototyped here, and what each one measured: phase accuracy over hours, discontinuities and clicks, smear at audio rate, retriggers, the evaluation rate, the random waveforms, rate changes, block sizes, denormals, allocation, and CPU;
- the decision, how it is built, and the tests that pin it down.

Read [band-limited-oscillators.md](band-limited-oscillators.md) (minBLEP and PolyBLEP), [oscillator-waveforms.md](oscillator-waveforms.md) (waveforms as data), [oscillator-phase.md](oscillator-phase.md) (fixed-point phase, retriggers), [envelopes.md](envelopes.md) (the click metric), and [parameter-smoothing.md](parameter-smoothing.md) first.

## Theory

### What an LFO is, and how it differs from an oscillator

An LFO is an oscillator whose output is never heard directly. It is added, scaled, to a parameter. That changes what matters:

- **The range is wide and mostly slow.** A typical LFO runs from a cycle every ten seconds or more to a few tens of hertz. Many synths, Sylenth1 among them, let it reach audio rate, where it acts as FM or ring modulation.
- **Its waveforms are the oscillator's and more.** There are sine, triangle, saw, ramp (a falling saw), and pulses, plus random waveforms that an audio oscillator has no use for: **sample and hold** (a new random value each cycle, held) and **smoothed random** (random values joined by curves). Some synths add chaotic generators such as the Lorenz attractor.
- **Where it starts matters.** An LFO restarted at every note (retriggered) gives every note the same vibrato or filter sweep from the same point. A free-running one doesn't restart, so each note catches it wherever it is.
- **Its output has a gain and an offset.** The gain scales the wave and the offset shifts it, so a bipolar wave (±1) can become a unipolar one (0 to 1) that only raises a parameter.

### Discontinuities: why LFO steps click

A saw, a pulse, a sample-and-hold step, or a retrigger makes the LFO jump. The destination jumps with it, and a jump in a gain or a cutoff is a click: the audio is multiplied by a step, which spreads energy across the whole spectrum. It is the same event as an envelope with an attack of 0 ([envelopes.md](envelopes.md#discontinuities-and-clicks)).

Two different problems hide here, and they need different cures.

- **Aliasing.** A naive step sampled at the sample rate has energy above Nyquist that folds back. That matters only when the LFO itself is heard as a waveform, which means at audio rate. Band-limited steps (PolyBLEP, minBLEP) remove it, as they do in the oscillators. **They don't remove the click**, because a band-limited step is still a step everywhere below Nyquist, and the click lives below Nyquist.
- **The click.** A step's energy at 1–20 kHz is in band. The only way to reduce it is to make the edge slower: a **slew** (a short ramp over a fixed time, here 1 ms), or a crossfade between the old trajectory and the new one at a retrigger. That changes the waveform, so the slew must stay short against the cycle. At audio rate, 1 ms is longer than a whole cycle of a 1 kHz LFO.

### Phase accuracy

An LFO accumulates its phase over a long time, often the whole song when it runs free. Each representation fails in its own way:

- **float in [0, 1):** near 1 the spacing of floats is 6·10⁻⁸. Adding an increment of 2·10⁻⁷ (0.01 Hz at 48 kHz) rounds every step by up to 15%. The error depends on where the phase is, so it doesn't cancel, and the effective frequency is wrong by percents.
- **32-bit fixed point** (as the oscillators): every step is exact, but the increment is rounded once, to 2⁻³² of a cycle. At 0.01 Hz and 192 kHz the increment is about 224 units, so rounding is up to 0.2% of the frequency. The error is a constant frequency offset. It doesn't drift randomly, but it adds up: a phase error grows linearly with time.
- **double, or 64-bit fixed point (32.32):** both are exact enough for any length of time an LFO runs. A 64-bit integer add is two instructions on a 32-bit Cortex-M (add, add with carry), while double arithmetic has no hardware on a Cortex-M4F.

Phase accuracy matters most for an LFO locked to the host's tempo: a frequency error is a phase that drifts against the music. A transport-locked LFO (next task) derives its phase from the host's position instead of accumulating it.

### Control rate against audio rate

An LFO changes slowly, so it can be computed once per block of N samples and **held** (a staircase) or **interpolated** (straight lines between grid points). That saves work: one evaluation per N samples instead of one per sample. The cost is error.

- **Held:** a staircase of steps every N samples. On an amplitude it is a buzz at fs/N (1.5 kHz at N = 32 and 48 kHz). On pitch it is far less audible, because the oscillator's phase integrates the frequency and smooths the steps.
- **Linear:** the error is the curve's sag between grid points, which shrinks with N² and with the square of the LFO's rate.
- **Edges:** a step evaluated on a grid lands on the grid, up to N − 1 samples late. That is jitter for a rhythmic pulse LFO, and interpolating across an edge smears it.

Piecewise-linear waveforms (saw, ramp, triangle, pulses, TriSaw, sample and hold) are exactly straight between their corners. A model that computes them in runs from corner to corner is exact at the cost of a control-rate one. Only the curved ones (sine, smoothed random, Lorenz) need interpolation.

### Per voice against global

A **per-voice** LFO runs inside every voice. Each note can restart it (retrigger), and per-voice sources (velocity, keytrack, a mod envelope) can modulate its rate and gain. A **global** LFO runs once for the whole synth, so all voices move together, and costs 1/16 as much at 16 voices. A free-running LFO only behaves like a global one if every voice reads the same phase, so "free" and "global" are related but not the same. A per-voice LFO that takes the global phase at its note-on is free-running and can still be modulated per voice.

## Sylenth1: what is known

The full source table, with confidence ratings and the open questions, is in the inventory ([§6, LFOs](../reference/sylenth1-features.md#lfos)). In short:

- **Official:** two LFOs with "10 different waveforms" (the manual, v2.5), now "11" (the specification page). Rate, Gain ("changes the amplitude"), Offset ("a positive or negative constant value… If Offset is set to 0, the output wave will be centered around 0"), and Free ("will not be retriggered when a new note is played, nor will it sync to the host tempo"). Rates show in Hz with Sync off. v1.1 added sample and hold, the Free button, and "smoother LFO waveforms". v1.20 and v1.21 improved "LFO frequency modulation and anti-aliasing". LFO Rate and Gain are modulation destinations. No v3.x changelog line mentions the LFOs.
- **The developer (KVR, 2007–2010):** Gain 10 at a modulation amount of 1.0 moves the pitch by exactly an octave in total. Offset ±10 makes the wave one-sided ("4 notes up and 0 notes down"). There is no invert, so use a negative amount. A keytracked LFO rate "can be used to create ringmod and FM-like effects".
- **Third parties:** one list of the ten names (Sine, Saw, Ramp, Triangle, Pulse, HPulse, QPulse, TriSaw, Lorenz, S&H). One user saw a retriggered sine "always" start at its top peak. Users report that the rate reaches audio rate and that its minimum isn't slow enough.
- **Unknown:** the 11th waveform; the shapes; whether a non-free LFO is per voice and what a free one shares; the rate range and law; whether steps are smoothed; the evaluation rate. These are open questions L1–L14, each with the demo measurement that settles it in M1.13.

## The candidates and what they measured

Measured at commit `dae814e` by the hidden `[lfo-report]` and `[lfo-long-report]` and the `[lfo]` benchmarks, on test-only prototypes (`tests/dsp/control/lfo_candidates.hpp` at that commit). The report and the benchmarks now measure the built `Lfo` ([Results, as built](#results-as-built)). To rerun the candidates, build that commit:

```sh
git checkout dae814e
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[lfo-report]"        # about 25 s
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[lfo-long-report]"   # three hours of each accumulator, a few minutes
tools/bench.sh "[lfo]"
```

### The waveform table

The waveforms are data, as in the oscillators. Each row is a `Shape`: a kind, a `WaveShape` for the periodic kinds, and the phase a retrigger starts at. A new periodic waveform is one more row. The periodic rows reuse the oscillator's rows where the names match, and the Ramp is one new segment from +1 to −1.

| Row | Kind | Start value | Range | Mean |
|---|---|---:|---|---:|
| Sine | periodic | +1 (its peak) | ±1 | 0 |
| Saw | periodic, rising | −1 | ±1 | 0 |
| Ramp | periodic, falling | +1 | ±1 | 0 |
| Triangle | periodic | +1 (its peak) | ±1 | 0 |
| Pulse, HPulse, QPulse | periodic, 50 / 25 / 12.5% high | +1 | ±1 | 0, −0.5, −0.75 |
| TriSaw | periodic, (triangle + saw) / 2 | −1 | −1 to 0.5 | 0 |
| Lorenz | the x of a Lorenz attractor / 20, time scaled by the rate | wherever the attractor is | about ±0.97 | ≈ 0 |
| S&H | a uniform value from `Random` per cycle, held | a draw | (−1, 1) | ≈ 0 |
| Smooth | a uniform value per cycle, joined by smoothstep (3t² − 2t³) | a draw | (−1, 1) | ≈ 0 |

The start phases are hypotheses (L5): only the sine's has a source. "Smooth" is a candidate for the 11th waveform and has no source at all (L1). The output is gain × (wave + offset), unclipped, following the developer's numbers (L7).

### The models

One engine, `Lfo<PhaseKind, Edges>`, runs every row, so each table varies one axis. A jump is any step in the naive output: a corner with a jump (the saw's and pulses' edges), a sample-and-hold step, or a retrigger.

| Model | Phase | At a jump |
|---|---|---|
| Naive, float phase | float in [0, 1) | the step as it is |
| Naive, fixed point | 32-bit fixed point | the step as it is |
| Slew 1 ms | 32-bit | spread linearly over 1 ms (48 samples at 48 kHz) |
| Slew 1 ms, at most 1/16 cycle | 32-bit | as above, but never longer than 1/16 of a cycle |
| PolyBLEP | 32-bit | the 2-point polynomial band-limited step (one sample of latency) |
| MinBLEP steps | 32-bit | the oscillator's 64-tap minBLEP step residual (no BLAMP, no slope compensation) |
| Oscillator (minBLEP 64) | 32-bit | `MinBlepOscillator<64>` itself for the periodic rows: steps and bends band-limited, a retrigger as its `jumpTo()`; random rows as MinBLEP steps |
| Every N, held / linear | 32-bit | naive, evaluated once per N samples (`ControlRateLfo`) |

Every model follows the same rules. A control or a retrigger between two samples applies from the next one. A retrigger before the first sample (a note from silence) simply starts at the start phase. The first sample after a reset is the row's start value. Phase representation is compared on the accumulators alone, including double and 64-bit fixed point.

### Phase accuracy

Each accumulator ran from phase 0 at a constant rate for **three hours**, at 44.1, 48, 96, and 192 kHz (the worst is shown). The frequency error is the phase error divided by the cycles run.

| Rate | float | double | fixed 32 | fixed 64 (32.32) |
|---|---|---|---|---|
| 0.01 Hz | **8.0·10⁻²**, 8.6 cycles off | 2.8·10⁻¹⁰, 3·10⁻⁸ | 1.4·10⁻³, 0.15 cycles | 3.1·10⁻¹³, 3·10⁻¹¹ |
| 1 Hz | 1.7·10⁻³, **18 cycles** | 4.2·10⁻¹², 5·10⁻⁸ | 1.7·10⁻⁵, 0.18 | 4.4·10⁻¹⁵, 5·10⁻¹¹ |
| 100 Hz | 9.8·10⁻⁶, 11 cycles | 2.3·10⁻¹⁴, 3·10⁻⁸ | 6.0·10⁻⁸, 0.06 | 0, 0 |
| 2 kHz | 7.2·10⁻⁷, 15 cycles | 1.4·10⁻¹⁵, 3·10⁻⁸ | 7.5·10⁻⁹, 0.16 | 0, 0 |

- **Float accumulation is unusable for slow LFOs:** 8% off in frequency at 0.01 Hz, and 18 cycles behind after three hours at 1 Hz.
- **32-bit fixed point** has a constant frequency error from rounding the increment: 0.14% at 0.01 Hz, 1.7·10⁻⁵ at 1 Hz. That is inaudible for a free LFO. Against a tempo it is a phase drift of a fifth of a cycle in three hours.
- **Double and 64-bit fixed point** are exact for any practical length. Fixed 64 costs the same as fixed 32 on this machine (3.50 against 3.49 ns per sample for a saw), and on a Cortex-M it is an add with carry where double has no hardware.

### Discontinuities and clicks

48 kHz, about 11 s of each LFO, Welch-averaged (Hann 4096). **LFO:** the LFO's own power above 1 kHz. **Amplitude:** a 443.7 Hz sine at 0.5 + 0.5 × lfo, power above 2 kHz. **Cutoff:** a 443.7 Hz sine through a one-pole low-pass at 1 kHz × 2^(2 × lfo), power above 4 kHz. Each value is in dB against the naive fixed-point model. The naive model's own level, against the whole signal, is the first row. The carrier is not a whole number of cycles per second, so the LFO's edges meet it at varying phases. A first version at 440 Hz put every fixed-point edge on a carrier zero crossing, which flattered the fixed-point model by 5–12 dB.

| Model | Saw 1 Hz: LFO / amplitude / cutoff | Saw 20 Hz | Pulse 1 Hz | Pulse 20 Hz |
|---|---|---|---|---|
| *(naive fixed point, dB re its signal)* | *−32.2 / −41.0 / −72.7* | *−19.2 / −28.0 / −59.3* | *−34.0 / −40.1 / −56.0* | *−20.9 / −26.7 / −42.2* |
| Naive, float phase | −0.1 / 0.4 / −0.6 | 0.0 / 0.0 / 0.0 | −0.1 / 0.7 / 0.5 | 0.0 / 0.0 / 0.0 |
| **Slew 1 ms** (both variants) | **−18.6 / −22.3 / −22.1** | **−18.6 / −22.5 / −21.9** | **−18.6 / −22.3 / −33.4** | **−18.6 / −22.5 / −33.8** |
| PolyBLEP | −0.4 / −0.9 / −0.6 | −0.5 / −0.8 / −0.4 | −0.4 / −0.8 / −0.6 | −0.4 / −0.9 / −0.5 |
| MinBLEP steps | −0.2 / −0.6 / −0.6 | −0.2 / −0.3 / −0.8 | −0.2 / −0.5 / 0.9 | −0.2 / −0.3 / 1.2 |
| Oscillator (minBLEP 64) | −0.2 / −0.6 / −0.6 | −0.2 / −0.3 / −0.9 | −0.2 / −0.5 / 0.9 | −0.2 / −0.3 / 1.0 |

The saw's edges at 1 Hz, by octave band (dB against the naive model in the same band):

| Model | 1–2 kHz | 2–4 kHz | 4–8 kHz | 8–16 kHz | 16–24 kHz |
|---|---:|---:|---:|---:|---:|
| Slew 1 ms | −16.3 | −21.6 | −27.3 | −32.8 | −36.2 |
| PolyBLEP | 0.0 | −0.1 | −0.6 | −2.3 | −5.9 |
| MinBLEP steps, oscillator | 0.0 | 0.0 | −0.2 | −0.8 | −2.7 |

One edge as a click, with the envelope report's metric (energy above 1 kHz in a Hann 4096 window centred on the edge, dB re an instant step from 0 to 1). The saw's edge and the pulse's are steps of 2:

| Model | Saw edge | Pulse edge |
|---|---:|---:|
| Naive (float or fixed) | +6.0 | +6.0 |
| **Slew 1 ms** | **−12.6** | **−12.6** |
| PolyBLEP | +5.6 | +5.6 |
| MinBLEP steps, oscillator | +5.8 | +5.8 |

- **Band-limiting doesn't remove the click.** PolyBLEP and minBLEP take less than 1 dB off the in-band energy, and only touch the top octave, as the theory says. They are about aliasing.
- **A 1 ms slew takes 18.6 dB off the LFO's edges above 1 kHz**, 22 dB off a modulated amplitude, and 22–34 dB off a modulated cutoff. A single edge goes from +6 dB to −12.6 dB against a unit step, close to the envelope's ordinary stage corners (−33 to −38 dB, measured with a 10 ms attack inside the window). A pulse on the volume then gates like a 1 ms attack and release rather than a click.
- Float phase changes nothing here.

### Smear and aliasing at the top of the range

48 kHz, coherent frequencies near 100 Hz and 1 kHz, Blackman–Harris. Harmonics 1 and 5 against the ideal waveform's (dB), and the worst non-harmonic tone below 20 kHz (dB re the fundamental).

| Model | Saw 100 Hz: h1 / h5 / alias | Saw 1 kHz | Pulse 1 kHz |
|---|---|---|---|
| Naive (float or fixed) | 0.0 / 0.0 / −48.9 | 0.0 / 0.0 / −29.2 | 0.0 / 0.0 / −29.2 |
| Slew 1 ms | −0.1 / −4.0 / −53.4 | **−72.3 / −72.1 / +38.6** | −13.5 / −27.3 / −33.6 |
| **Slew 1 ms, at most 1/16 cycle** | −0.1 / −1.5 / −53.5 | **0.0 / −1.3 / −33.4** | 0.0 / −1.3 / −33.4 |
| PolyBLEP | 0.0 / 0.0 / −60.2 | 0.0 / −0.3 / −41.3 | 0.0 / −0.3 / −41.3 |
| MinBLEP steps, oscillator | 0.0 / 0.0 / −132.9 | 0.0 / 0.0 / −114.4 | 0.0 / 0.0 / −113.4 |

- **A fixed 1 ms slew destroys an audio-rate saw.** At 1 kHz the ramp takes the whole cycle and the saw's fundamental falls by 72 dB. Capped at 1/16 of a cycle (3 samples at 1 kHz) it keeps the shape within 1.3 dB at the 5th harmonic and cuts aliasing a little (−33 against −29 dB). The cap changes nothing at slow rates.
- **Only minBLEP makes an audio-rate LFO alias-free** (−114 dB), as it does the oscillators. The naive saw at 1 kHz has an alias 29 dB below its fundamental.

### Retrigger

48 kHz, 2 Hz. A sine retriggered at its trough jumps to its peak (+2), and a saw retriggered half-way jumps from 0 to −1. **Step:** the largest change between adjacent samples after the retrigger. **Click:** as above. **On amplitude:** a 443.7 Hz sine at 0.5 + 0.5 × lfo, power above 2 kHz in the same window, dB re the same sine gated on by a hard step.

| Model | Sine: step, click, on amplitude | Saw: step, click |
|---|---|---|
| Naive (float or fixed): a hard jump | 2.000, +6.0 dB, 0.0 dB | 1.000, 0.0 dB |
| **Slew 1 ms, or a 1 ms crossfade** | **0.042, −12.6, −20.3** | **0.021, −18.6** |
| Slew 5 ms, or a 5 ms crossfade | 0.008, −25.8, −34.4 | 0.004, −31.8 |
| PolyBLEP | 1.000, +5.6, −2.2 | 0.500, −0.5 |
| MinBLEP steps, or the oscillator's `jumpTo()` | 1.314, +5.8, −4.1 | 0.657, −0.2 |

- A hard jump clicks like a gate, and its band-limited forms do too, for the reason above.
- **A slew of the jump and a crossfade between the old and the new trajectory measure the same** at 1 and at 5 ms (to the 0.1 dB shown). At 2 Hz the old trajectory hardly moves in a few milliseconds. The slew needs no second phase and is the same mechanism as the edges, so a retrigger, a waveform's edge, and an S&H step are treated alike.

**The first samples after a reset** (a new voice; a sine starts at +1):

| Model | Samples 0, 1, 2, 3 |
|---|---|
| Every `Lfo` model | 1.0000, 1.0000, 1.0000, 1.0000 |
| Oscillator (minBLEP 64) | **0.0000, 0.0016, 0.0667, 0.4931** |

**The oscillator starts every note from 0** with its band-limited onset, as an audio oscillator should. For an LFO that is wrong: an LFO on the cutoff with an offset would sweep up from 0 at every note. Reusing the oscillator would need a second kind of restart.

### Evaluation rate

48 kHz, against the same LFO per sample (naive, fixed point). **Control:** the largest difference in the LFO's value (full scale ±1). **Spurs:** the strongest tone more than 400 Hz from a 1 kHz carrier, beyond the modulation's own sidebands, in dB re the signal. The LFO is on the carrier's amplitude (0.5 + 0.5 × lfo) or its pitch (±1 semitone). Per sample, the floor is −133 dB (amplitude) and −135 dB (pitch) for the sine, and −89 / −103 dB for the triangle (its own corners).

| LFO | Evaluation | Control error | Amplitude spur | Pitch spur |
|---|---|---:|---:|---:|
| Sine 5 Hz | every 32, held | 2.0·10⁻² | −60.2 | −93.4 |
| Sine 5 Hz | every 64, held | 4.1·10⁻² | −54.1 | −81.4 |
| Sine 5 Hz | every 16, linear | 1.4·10⁻⁵ | −121.6 | −134.8 |
| Sine 5 Hz | **every 32, linear** | **5.5·10⁻⁵** | **−109.7** | **−134.3** |
| Sine 5 Hz | every 64, linear | 2.2·10⁻⁴ | −97.6 | −122.7 |
| Sine 40 Hz | every 32, held | 1.6·10⁻¹ | −42.0 | −68.4 |
| Sine 40 Hz | every 64, held | 3.3·10⁻¹ | −35.8 | −56.4 |
| Sine 40 Hz | every 16, linear | 8.8·10⁻⁴ | −85.5 | −120.5 |
| Sine 40 Hz | **every 32, linear** | **3.5·10⁻³** | **−73.3** | **−102.4** |
| Sine 40 Hz | every 64, linear | 1.4·10⁻² | −60.8 | −84.0 |
| Triangle 5 Hz | every 32, held | 1.3·10⁻² | −61.3 | −93.2 |
| Triangle 5 Hz | every 16, 32, or 64, linear | 2·10⁻⁴ | −89.0 (the floor) | −102.9 (the floor) |

- **Held is audible on amplitude:** a 40 Hz sine held per 32 samples puts a tone 42 dB below the carrier at fs/32 from it. On pitch, holding is far gentler (−68 to −93 dB), because the oscillator integrates frequency.
- **Linear interpolation per 32 samples is clean:** spurs at −73 dB for a 40 Hz sine on amplitude and −110 dB at 5 Hz, and below −100 dB on pitch. The error grows as N² and as the rate squared.
- **A triangle interpolated between grid points is exact except near its corners.** Every piecewise-linear row is exact if computed in runs from corner to corner instead.
- **Edges on a grid jitter:** a saw or pulse evaluated every N samples moves its edge up to N − 1 samples late, 0.67 ms at N = 32. A rhythmic pulse gating the volume would wander by that much.

### The random rows

48 kHz, 10 Hz, 40 s per voice, 16 voices seeded 1–16. "Cycle-to-cycle r" is the correlation between values half a cycle into consecutive cycles. "Adjacent seeds" is the mean |r| between voices seeded n and n + 1.

| Row | Mean | σ | Min / max | Cycle-to-cycle r | Adjacent seeds, mean abs r | Same seed twice |
|---|---:|---:|---|---:|---:|---|
| S&H | 0.010 | 0.573 | −1.000 / 1.000 | −0.004 | 0.038 | bit-identical |
| Smooth | 0.010 | 0.494 | −1.000 / 1.000 | 0.487 | 0.040 | bit-identical |
| Lorenz | −0.002 | 0.396 | −0.967 / 0.967 | 0.143 | 0.023 | bit-identical |

- **S&H** is uniform (σ 0.573 against 0.577 for a uniform on (−1, 1)) and independent from cycle to cycle. **Smooth** is the same draws joined smoothly, so values half a cycle in are averages of neighbours (r 0.49, as theory gives 0.5). Voices with adjacent seeds are independent (|r| 0.02–0.04, the estimate's own noise at this length). The same seed gives the same output, bit for bit.
- **Lorenz, against its rate:** σ 0.39–0.40 and 0.35–0.42 zero crossings per cycle at every rate from 0.05 Hz to 500 Hz. The rate scales its time and nothing else, even at 0.05 Hz, where the attractor's steps are 7·10⁻⁷ time units (0.5% of samples repeat the previous value exactly there).
- **Lorenz is chaotic in its rounding.** The same steps computed with every product rounded on its own, as a compiler that doesn't contract `a * b + c` into a fused multiply-add computes them, part company with this build's after 30.7 s at 0.5 Hz, 3.7 s at 5 Hz, and 0.4 s at 50 Hz: **15–20 cycles at every rate.** Apple Clang on arm64 fuses within an expression by default. The x86-64 builds (GCC on Linux, MSVC on Windows, the release's x86-64 slice) target a baseline without an FMA instruction, so they can't. A Lorenz patch would therefore render differently on Apple Silicon and on x86-64 after a few cycles, which a golden file can't absorb with a tolerance. Writing each product as its own statement stops Clang's default contraction (it fuses only within an expression), so every target built today computes the same thing. A GCC build for arm64 contracts across statements by default and would also need `-ffp-contract=off`. A test can then pin the steps to an unfused reference, bit for bit.

### A rate change mid-cycle

48 kHz, 2 Hz to 7 Hz at sample 7000. The largest step between adjacent samples near the change, against the waveform's largest slope per sample at 7 Hz. 1.0 or below means no jump.

| Model | Sine | Triangle | Saw (between edges) |
|---|---:|---:|---:|
| Every `Lfo` model | 0.966 | 1.000 | 1.000 |
| Oscillator (minBLEP 64) | 0.966 | 1.123 | 1.123 |

Every phase accumulator is continuous across a rate change. The oscillator's band-limited bend overshoots by 12% for a few samples.

### Block sizes, denormals, allocation, lifecycle

| Model | Blocks of 1 / 37 / 4096 | Subnormal outputs / states | Allocations | Lifecycle contract |
|---|---|---|---:|---|
| Every model, including every 32 held and linear | bit-identical | 0 / 0 | 0 | passes |

The block-size script has retriggers (two on adjacent samples), rate changes from 0.25 to 333 Hz split at their samples, and every row with Sine, Saw, Pulse, S&H, Smooth, and Lorenz. The control-rate models count their grid from the last reset or retrigger, so they don't depend on the block size either. Subnormals were checked over 60 s at 0.5 Hz with a retrigger every second, on every row, outputs and internal state. Nothing in any model decays towards 0: the slew ends by count at exactly 0, and the minBLEP residuals are cleared after 64 samples.

### CPU

Nanoseconds per sample, 48 kHz, `relwithdebinfo`, Apple M1 Ultra (`tools/bench.sh "[lfo]"`). One LFO for a 512-sample block, and a 16-voice synth's 32 LFOs (two per voice, a sine and a saw). The `Lfo` prototypes run one sample at a time through a switch on the row's kind. The control-rate model fills each grid segment in one loop, as a built LFO would.

| Model | Sine | Saw | Pulse | S&H | Smooth | Lorenz | 32 LFOs |
|---|---:|---:|---:|---:|---:|---:|---:|
| Naive, float phase | 5.99 | 5.55 | 5.78 | 5.81 | 5.71 | 11.28 | 186 |
| Naive, double phase | 5.66 | 5.76 | | | | | 189 |
| Naive, fixed 32 | 4.89 | 3.49 | 3.76 | 3.57 | 3.62 | 10.70 | 140 |
| Naive, fixed 64 | 4.76 | 3.50 | | | | | 132 |
| Slew 1 ms | 5.49 | 4.70 | | 3.61 | | | 162 |
| PolyBLEP | 5.22 | 4.48 | | 3.47 | | | 154 |
| MinBLEP steps | 5.19 | 4.33 | | 3.46 | | | 151 |
| Oscillator (minBLEP 64) | 8.56 | 3.09 | 3.01 | | | | 179 |
| Every 32, held | 0.28 | 0.21 | | | | | 8.6 |
| **Every 32, linear** | **0.58** | **0.52** | | | | | **18.5** |
| Every 64, linear | 0.50 | 0.49 | | | | | 16.7 |

- **Per sample, every model costs 3–6 ns**, so 32 LFOs cost 130–190 ns per sample. That is more than one note's 32 oscillator voices (about 100 ns) and five times the 64 envelopes (25 ns).
- **Linear interpolation every 32 samples cuts that to 18.5 ns** for 32 LFOs (0.09% of a core). Holding saves another 10 ns, at the error measured above.
- The slew costs about 1 ns per sample on a saw. Fixed 64 costs what fixed 32 does. Float and double phase cost more than fixed point here (the float-to-integer conversion for the table lookup). Lorenz costs about 11 ns: a midpoint step of three coupled equations per sample.

## The decision

**Chosen by the project owner on 2026-11-14: every recommendation below** (PLAN.md, Decisions log). Building it changed one detail: a jump is slewed from the last output sample, not from the old segment extended to the new sample. The randomized stress test found the difference. At audio rate the extended segment overshoots by up to a sample's worth of slope (a 10 kHz saw reached 1.46), while the last output is always a value the LFO has really had.

| Question | Decided | Why |
|---|---|---|
| **Phase** | **64-bit fixed point (32.32)** | Exact for hours (5·10⁻¹¹ cycles in three), so a synced LFO can't drift. No double needed (Cortex-M4F), and it costs what 32 bits does. Float is out (8% off at 0.01 Hz). |
| **Waveform set** | The **11 rows above**: Vintage Synth's 10 as the oscillator's shapes plus a falling Ramp, Lorenz and S&H, and **Smooth** (smoothed random) as a provisional 11th. All bipolar (±1); start phases as in the table. Data rows, so M1.13 changes rows, not code. | The only list of names, with the count and v1.1's S&H. The oscillator's rows are reused where the names match (L2). |
| **Edges and retriggers** | **A linear slew of 1 ms, at most 1/16 of a cycle**, on every jump: a waveform's edge, an S&H step, a retrigger. A note from silence starts at the start value, with no slew. | 18.6–34 dB less in-band click than a naive step, and a single edge at −12.6 dB against a unit step. Band-limited steps take off under 1 dB. The cap keeps an audio-rate LFO's shape. v1.1 made the "LFO waveforms smoother". One mechanism, ended by count at exactly 0 (no denormals). |
| **Audio rate** | No band-limiting for now. MinBLEP steps are the measured option if L10 shows Sylenth1 is anti-aliased there. | Aliasing only matters when the LFO is heard as a waveform. MinBLEP gives −114 dB at 1 kHz against −33 dB, at about 1 ns per sample, but it doesn't help clicks and complicates control-rate evaluation. |
| **Evaluation** | **Per sample in runs**: the piecewise-linear rows (Saw, Ramp, Triangle, pulses, TriSaw, S&H) exactly, segment by segment. The curved rows (Sine, Smooth, Lorenz) **interpolated linearly every 32 samples** on a grid counted from the note-on. Edges and retriggers at their exact samples, with the slew. | Exact or −73 dB at worst (40 Hz sine on amplitude), and below −100 dB on pitch. About 0.6 ns per sample (18.5 ns for 32 LFOs, against 140 ns per sample). Edges aren't moved to a grid. Independent of block size. M1.5 can still take one value per block where a destination allows. |
| **Output, Gain, Offset** | out = gain × (wave + offset), **gain ∈ [0, 1], offset ∈ [−1, 1], unclipped** (range ±2). Gain and the rate can be modulated. | The developer's numbers: Offset ±10 makes the wave one-sided at any Gain. Gain 0 gives 0 whatever the offset (L7). M1.5 scales by destination: amount 1.0 at Gain 10 is an octave in total on pitch, so ±6 semitones per unit. |
| **Per voice and Free** | **One `Lfo` per voice per slot.** Free off: a note-on retriggers it. **Free on: at a note-on it takes a copy of a global free-running instance's state** (phase, random state, held value) instead of restarting. Unmodulated, every voice then follows the global one bit for bit; a voice's own rate modulation still applies. | The developer's keytracked-rate ringmod needs a rate per note (L8). A copy at the note-on gives "free" its shared phase without giving up per-voice modulation, and M1.5 can still run one shared instance when nothing modulates it per voice. |
| **Random rows** | S&H and Smooth from the seeded `Random`: a draw per cycle and at each retrigger, seeded per voice from the instance seed and the voice's index (as drift). **Lorenz with each product in its own statement**, pinned bit for bit by a test against an unfused reference, so every target renders the same. | Uniform, independent, deterministic (measured). Lorenz renders diverge after 15–20 cycles between builds that do and don't fuse multiply-adds. |
| **Knob laws (parameter table, M1.9)** | Working hypotheses, to be measured (L6, L7): **Rate exponential, 0.1 Hz to 100 Hz** (f = 0.1 × 1000^x for knob x ∈ [0, 1]); **Gain = knob / 10**; **Offset = knob / 10** for knob −10..10. | No source gives the range. Users find the minimum not slow enough and the top at audio rate. The developer uses Gain and Offset in tens. |

**Tempo sync (decided and built 2026-11-14, [tempo-sync.md](tempo-sync.md)).** This page's forecast was that a synced rate could arrive in Hz through `setFrequency`. Measured, it can't: a float in hertz drifts 0.016 cycles in three hours at 1/64T. A synced LFO therefore sets its rate as an exact 64-bit increment, `setPhaseIncrement(notePhaseIncrement(value, bpm, fs))`, once per block. Its phase starts at the note-on, as here, and never reads the host's position: Sylenth1 "locks to host tempo, but not to song position". So no `setPhase` was needed. A transport-locked phase was measured and rejected for Part 1, because it clicks at every loop, locate, and transport start.

## How it is applied here

### The LFO

`Lfo` ([lfo.hpp](../../libs/dsp/include/anoesis/dsp/control/lfo.hpp)) holds its settings, a 64-bit phase, the state of every kind of row (a segment, a held value, a pair of random values, a Lorenz state, a grid segment), a slew, two `LinearSmoother`s for gain and offset, and a `Random`. It is 328 bytes, a value type with no tables of its own and no allocation.

- **The table.** `lfoShapes` is a `constexpr` array of `LfoShape` rows: a kind (`periodic`, `sampleHold`, `smoothRandom`, `lorenz`), a `WaveShape` for the periodic ones, and a start phase. The Sine, Saw, Triangle, pulses, and TriSaw are the oscillator's own rows (`waveShapes`). The Ramp is one segment from +1 to −1. `isCurved()` sorts the rows into exact and interpolated. A new periodic waveform is a row; a new kind of generator is a case in four small switches.
- **The phase** is a 64-bit integer, 2⁶⁴ per cycle, whose top 32 bits are the oscillators' phase. It is set from the rate in double, at the control call only (one division and one rounding), and every sample adds it exactly. `setPhaseIncrement()` sets it directly instead, for tempo sync, and `prepare()` keeps an increment set that way. `phase()` and `phaseIncrement()` expose it.
- **Exact rows in runs.** The piecewise-linear rows and S&H render a run at a time, up to the sample before the next corner. The number of samples to the corner is one 64-bit division per corner, not a test per sample. Within a run each sample is the corner's value plus its slope times the phase since the corner: the same expression `WaveShape::valueAt` uses, so it is exact and the same however a render is split.
- **Curved rows on a grid.** Every 32 samples from the last (re)start, the row is evaluated at the next grid point: `sin` for the sine; smoothstep between the current pair for the smoothed random, which draws a new value for each cycle start the 32 steps pass; Lorenz steps of at most 0.02 time units for the attractor. The run in between is a straight line from the last grid value, which vectorizes. A rate change reaches a curved row at its next grid point, at most 31 samples later.
- **Jumps.** At a corner with a jump, a new S&H cycle, a retrigger, or a waveform change, the slew offset becomes the last output minus the new value. It then falls linearly to exactly 0 over the slew's length: round(1 ms × rate), or 1/16 of a cycle if that is shorter, at least one sample. A jump during a slew starts a new one from the current output. A jump smaller than 2⁻²⁴ is not slewed. Corners that only bend (the triangle's) need nothing.
- **Triggers.** `restart()` starts the cycle at once with no slew, for a voice from silence: the random rows draw new values, and the Lorenz row runs on. `retrigger()` starts the cycle with the jump slewed, for a sounding voice. Before the first sample it is `restart()`. `process(out, events)` applies `LfoEvent`s at their sample offsets.
- **Gain and offset.** out = gain × (wave + offset), gain clamped to [0, 1] and offset to [−1, 1]. A value below 2⁻²⁴ in size is 0, so no output can be subnormal. NaN is ignored. A change ramps linearly over 10 ms, like the oscillator's gains. Settled, the block is one multiply-add loop.
- **The Lorenz step** is `lorenzStep()`, a midpoint step with every product and sum its own statement, so no compiler fuses them (above). `reset()` settles a seeded start onto the attractor with 1000 steps (about 10 µs).
- **`next()`** is `process()` of one sample, so the two agree bit for bit. It costs about 6 ns a call (the run setup), so a block should go through `process()`.

### How a voice uses it

```cpp
// At a note-on (M1.4/M1.5): Free off restarts or retriggers the voice's own LFO;
// Free on copies the global free-running one, which then runs in step with it.
if (lfoFree)
    voiceLfo = globalLfo;                  // a value type: phase, random state, everything
else if (voiceWasSilent)
    voiceLfo.restart();
else
    voiceLfo.retrigger();

voiceLfo.setFrequency(rateHz * keytrackRatio);   // per voice, Sync off or Free on
// Sync on and Free off: an exact increment per block (tempo-sync.md)
// voiceLfo.setPhaseIncrement(notePhaseIncrement(syncValue, tempo.bpm(), fs));
voiceLfo.setGain(gainKnob / 10.0f + gainModulation);
voiceLfo.process(lfoBlock);                      // then scaled per destination (M1.5)
```

The knob laws live in the parameter table (M1.9). They are working hypotheses: Rate f = 0.1 Hz × 1000^x for knob x ∈ [0, 1]; Gain = knob / 10; Offset = knob / 10 for knob −10..10 (L6, L7).

*M1.5 task 2, stage 1 (2026-11-22; [modulation.md](modulation.md#question-3-the-rate-per-destination)) pushes back on the sketch above in two places:*

- *A modulated Gain can't go through `setGain()`: its 10 ms ramp would lag the modulation. The prototype runs the LFO at gain 1 and offset 0 and scales it outside, by 0.5 × gain × (wave + offset).*
- *Because a curved row takes a new rate only at its own grid point, per-sample Rate modulation of a sine is the same, to the bit, as setting it once per segment. The recommendation is to set it at the voice's grid points and render each segment ahead, which also makes the LFO's bound for the skip test exact.*

*As built in `dsp::Voice` (M1.5 task 3, 2026-11-23; [modulation-sources.md](modulation-sources.md#as-built)):* two `Lfo`s per voice, run only while a route reads them. At a note-on from silence a non-free one has its waveform set and is then restarted. Set after the restart, it would start at the previous waveform's start and slew into the new one, out of phase for the whole note: the stage-1 prototype's flaw, fixed in the library. A sounding voice retriggers it; a legato note leaves it alone. A free one copies the context's global instance at a note-on from silence, moved on by the voice's offset into the call, and keeps the copy through a steal. At each grid point its rate is set from the Rate knob plus LFO Rate's modulation, through `lfoRateHz()` (L6's placeholder law, in `modulation/source_context.hpp`), and its segment is rendered ahead at gain 1. The voice applies the gain (ramped from the previous grid point's value) and the offset itself: 0.5 × gain × (wave + offset). Tempo sync isn't in the voice yet (a requirement on M1.8 and M1.9).

## Results, as built

From the `[lfo-report]` ([lfo_report.cpp](../../tests/dsp/control/lfo_report.cpp)), which now measures `Lfo` against a naive reference (the same rows read at the same 64-bit phase every sample, with no slew and no grid), and the `[lfo]` benchmarks ([lfo_benchmark.cpp](../../tests/dsp/benchmarks/lfo_benchmark.cpp)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[lfo-report]"   # about 2 s
tools/bench.sh "[lfo]"
```

Every figure matches the chosen prototype's, measured the same way at 48 kHz:

| Measurement | Lfo, as built | Naive reference |
|---|---|---|
| Saw edges, 1 / 20 Hz: LFO / amplitude / cutoff, against naive | −18.6 / −22.3 / −22.0, −18.6 / −22.5 / −22.0 dB | 0 |
| Pulse edges, 1 / 20 Hz: the same | −18.6 / −22.3 / −33.5, −18.6 / −22.5 / −33.8 dB | 0 |
| One edge as a click, re a unit step | −12.6 dB | +6.0 dB |
| Saw at 1 kHz: h1 / h5 against naive, worst alias | −0.1 / −1.4 dB, −33.4 dB | −29.2 dB |
| Retrigger at a sine's trough: largest step, click, on amplitude | 0.042, −12.6 dB, −20.3 dB | 2.000, +6.0 dB, 0.0 dB |
| A rate change, 2 → 7 Hz: largest step re the slope | sine 0.961, triangle and saw 1.000 | 0.966, 1.000 |

The first samples after a restart are each row's start value, at once (sine and pulse 1.0000, saw −1.0000, triangle 1.0000).

**The curved rows' interpolation**, a sine against the ideal at every sample (spurs as in the evaluation-rate table above):

| Sine | Control error | Amplitude spur | Pitch spur | Ideal sine's own floor |
|---|---:|---:|---:|---|
| 0.5 Hz | 7.2·10⁻⁷ | −132.2 | −135.7 | −132.2, −135.7 |
| 5 Hz | 5.5·10⁻⁵ | −109.7 | −134.3 | −133.0, −134.8 |
| 40 Hz | 3.5·10⁻³ | −73.3 | −102.4 | −131.9, −134.6 |
| 200 Hz | 8.6·10⁻² | **−43.7** | −43.0 | −131.2, −42.0 |

The error grows with the square of the rate, so a curved row at a few hundred hertz is coarse: a 200 Hz sine on the amplitude has a tone 44 dB down. (Its pitch spur is the vibrato's own: the ideal sine's is −42.0 dB at that rate.) If audio-rate sine LFOs matter, the grid could shrink with the rate (at most 1/32 of a cycle, say) at a small cost. That is a Part 2 or Phase 3 item, not part of this decision.

**The random rows** (10 Hz, 40 s per voice, 16 voices): S&H σ 0.573, cycle-to-cycle r −0.002; Smooth σ 0.494, r 0.488; Lorenz σ 0.396. Adjacent seeds |r| 0.024–0.042. The same seed gives the same output, bit for bit. **Lorenz against its rate**: σ 0.36–0.40 and 0.34–0.44 zero crossings per cycle from 0.05 to 500 Hz.

CPU, nanoseconds per sample, 48 kHz, one 512-sample block ([baselines](../performance/baselines.md)):

| Row | `process()` |
|---|---:|
| Sine 3 Hz | 0.62 |
| Saw / triangle / pulse 3 Hz | 0.19 / 0.19 / 0.20 |
| S&H 3 Hz | 0.10 |
| Smooth / Lorenz 3 Hz | 0.54 / 0.65 |
| Saw 1 kHz (an edge, and a slew, every 48 samples) | 0.50 |
| Sine with its gain ramping | 1.53 |
| `next()` per sample, sine / saw | 6.18 / 6.92 |
| **32 LFOs (16 voices × 2), sine and saw** | **12.8** |

Computing in runs cut 32 LFOs from 140 ns per sample (the per-sample prototypes) to 12.8, which is 0.06% of a core and half the cost of the 64 envelopes. The exact rows are cheaper than the interpolated ones, because a run of a straight segment is one loop with no per-sample branch.

## Tests

[lfo_test.cpp](../../tests/dsp/control/lfo_test.cpp) (`[lfo]`), 22 tests. The references are computed in double from each waveform's definition (not from the table under test), at the exact phase the LFO reports. The tolerances are derived next to each test:

- **Linear rows:** the slope times the phase's conversion to float and its dropped low 32 bits, plus one rounding each for the product and the sum.
- **Interpolated sine:** the curvature bound 4π² · spacing² / 8, plus `sin`'s float error and the interpolation's arithmetic.
- **Statistics:** 5 standard errors.

The tests:

- **Lifecycle and defaults.** `Preparable`. `reset()` and a second `prepare()` reproduce a fresh LFO bit for bit, for every row. By default it is a 1 Hz sine at gain 1, offset 0, starting at +1.
- **The table.** The periodic rows equal the oscillator's, sample for sample. The Ramp is the saw negated. Each row has its kind and its start value, from a reset and from a `restart()` while sounding.
- **Exact rows.** Every sample of every piecewise-linear row at four rates and four frequencies (0.01 Hz to 333 Hz), away from its slews.
- **Curved rows.** The sine at four rates and five frequencies (0.01 Hz to 400 Hz) within the interpolation bound, and on the grid within `sin`'s own error.
- **The phase.** The increment is the nearest 2⁻⁶⁴ of a cycle. After 1.2 million samples the phase is exactly the start plus that many increments.
- **Slews.** A saw's edge at four rates and four frequencies (1 Hz to 3 kHz): exactly round(1 ms × rate) or 1/16 of a cycle, a straight line from the last sample before the edge.
- **Retrigger and restart.** For five rows at four rates: a retrigger continues from the last output, slews to the restarted cycle, and from then on equals a restarted LFO bit for bit. A restart equals a fresh LFO at once.
- **Sample-accurate triggers** at offsets 0, 1, 200, and 511.
- **A rate change** is continuous for four rows, and the LFO does move at the new rate.
- **Gain and offset.** The output is exactly gain × (wave + offset) for five pairs. A change ramps linearly over round(10 ms × rate) samples and lands exactly.
- **Controls** clamp, ignore NaN, and flush below 2⁻²⁴. An out-of-range waveform is the sine.
- **S&H.** One value per cycle, held, its steps slewed. Uniform over 20,000 cycles (mean and variance within 5 standard errors). Independent from cycle to cycle. A restart draws a new value.
- **Smooth.** Continuous (no step beyond its slope bound), within ±1, and it moves.
- **Lorenz.** `lorenzStep` equals an unfused reference (every operation rounded through a volatile store) bit for bit over 200,000 steps at four step sizes. The row stays within ±1, is continuous, and has the same zero-crossing rate per cycle within a factor 2 at 0.5, 5, and 50 Hz.
- **Seeds.** The same seed gives the same output; adjacent seeds are uncorrelated (|r| ≤ 0.2 over 2000 cycles, for S&H, Smooth, and Lorenz).
- **A copy** made mid-run continues exactly as the original (Free). Its own rate change moves it alone.
- **A waveform change** continues from the last output and slews to the new row at the same phase.
- **Block sizes.** A script of retriggers, restarts, and rate, gain, offset, and waveform changes, for every row at four rates, rendered in blocks of 1, 37, and 4096: bit-identical. `next()` equals `process()`.
- **No subnormals** for every row at three rates, with a gain of 10⁻³⁵ (flushed) and with a retrigger every 12,000 samples. **No allocation** in any control, trigger, process call, or copy.
- **Stress.** At each of the four rates, 200,000 samples of random waveforms, rates (0, past Nyquist, NaN, 0.01 Hz to 10 kHz), gains, offsets, and triggers: finite, within ±2.25 (the wave passes ±1 by at most the triangle's slope × 1/16 cycle), free of subnormals, and bit-identical in blocks of 1, 37, and 4096.

**Every test was checked to fail first.** Against an inert stub with the same interface, 16 of the 22 tests failed. The other 6 pin properties that silence also has (the lifecycle, sample-accurate triggers, block sizes, subnormals, allocation, stability), and the breaks below check them. Then 23 deliberate breaks in `Lfo`, each caught and each restored. Every test fails under at least one break.

| Break | Tests that failed |
|---|---|
| no slew: jumps are steps | slew, retrigger, waveform change, S&H |
| the slew not capped at 1/16 cycle | slew, S&H, exact rows, stress |
| a slew one sample too long | 5, including slew, retrigger, exact rows |
| a crossing's slew from the extended old segment (the first version) | slew |
| a retrigger restarts, with no slew | retrigger |
| a restart slews | retrigger, S&H, table |
| a waveform change not slewed | waveform change |
| a 32-bit phase increment | the phase |
| curved rows held, not interpolated | curved rows, rate change, Lorenz, Smooth |
| the grid not restarted by a retrigger | retrigger, lifecycle, table, stress |
| a fused multiply-add in the Lorenz step | Lorenz |
| S&H draws no value at a cycle start | S&H, seeds, copy |
| Smooth draws nothing at its cycle starts | Smooth, seeds |
| gain and offset jump instead of ramping | gain and offset, stress |
| output = gain × wave + offset | gain and offset, block sizes, stress |
| the sine starts at 0 / the triangle at its bottom / the Ramp rises | defaults, table, curved rows, retrigger / table / table, exact rows, waveform change |
| triggers one sample late | sample-accurate triggers |
| reset doesn't reseed | lifecycle |
| a tiny gain not flushed | controls, subnormals |
| no Nyquist clamp | controls |
| an allocation in `process()` | allocation |

## References

- LennarDigital, *Sylenth1 User Manual*, v2.5, 2015, pp. 12, 19–20, 30, 32; [version history](https://lennardigital.com/sylenth1/whatsnew.php); v1.1, v1.20, and v1.21 release notes. The developer's and users' posts on KVR are listed in the [feature inventory](../reference/sylenth1-features.md#lfos), with confidence ratings.
- E. Brandt, "Hard sync without aliasing", *Proc. ICMC*, 2001: minBLEP. V. Välimäki and A. Huovilainen, "Antialiasing oscillators in subtractive synthesis", *IEEE Signal Processing Magazine* 24(2), 2007: PolyBLEP.
- E. N. Lorenz, "Deterministic nonperiodic flow", *Journal of the Atmospheric Sciences* 20(2), 1963: the attractor, and its sensitivity to initial conditions.
- D. Goldberg, "What every computer scientist should know about floating-point arithmetic", *ACM Computing Surveys* 23(1), 1991: rounding in accumulated sums.
- ISO/IEC 14882:2023, [expr.pre] (floating-point expressions may be evaluated with more precision than their type, which permits contraction within an expression), and the compilers' `-ffp-contract` and `/fp:contract` documentation.
- C. Roads, *The Computer Music Tutorial*, MIT Press, 1996: LFOs, sample and hold, and control rates.
