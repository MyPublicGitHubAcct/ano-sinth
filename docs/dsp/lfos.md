# LFOs

A low-frequency oscillator (LFO) is a modulation source: a slow, repeating (or random) control signal that moves a destination such as pitch (vibrato), volume (tremolo), cutoff (a filter sweep or a wobble), or pan. Sylenth1 has two, each with a waveform, a rate, a gain, an offset, and a Free switch (inventory [§6, LFOs](../reference/sylenth1-features.md#lfos)). This page covers:

- what an LFO has to get right that an audio oscillator doesn't, and the other way round;
- what is known about Sylenth1's LFOs;
- the candidate models prototyped here, and what each one measured: phase accuracy over hours, discontinuities and clicks, smear at audio rate, retriggers, the evaluation rate, the random waveforms, rate changes, block sizes, denormals, allocation, and CPU;
- the options for the decision.

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

Measured by the hidden `[lfo-report]` and `[lfo-long-report]` ([lfo_report.cpp](../../tests/dsp/control/lfo_report.cpp)) and the `[lfo]` benchmarks ([lfo_benchmark.cpp](../../tests/dsp/benchmarks/lfo_benchmark.cpp)), on test-only prototypes ([lfo_candidates.hpp](../../tests/dsp/control/lfo_candidates.hpp)):

```sh
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

## Options for the decision

*To be decided by the project owner. The decision and its date go here and in PLAN.md's Decisions log.*

| Question | Recommendation | Why |
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

**Tempo sync (the next task).** The DSP takes the rate in Hz, so a synced rate is the tempo in beats per second divided by the note value's length in beats, computed by the caller when the tempo or note value changes. A transport-locked phase (L12) needs one more input: a phase to jump to, computed from the host's position (PPQ) as the fractional part of PPQ / beats per cycle. That is a double on the host side, converted once into the 64-bit phase. Jumping there at playback start or a loop is a retrigger-like event through the slew. Small corrections while playing (the accumulator against the host clock) are better made as a tiny rate nudge than as a jump. Both need nothing beyond `setFrequency` and a `setPhase` that routes through the edge treatment.

## How it is applied here

*Written once the model is built (M1.2).*

## Tests

*Written once the model is built (M1.2).* The report's measurements become tests with derived tolerances: the table's values and start phases, phase accuracy at 0.01 Hz to audio rate (frequency error bounded by 2⁻³² of a cycle per sample), the slew's length and shape at edges, S&H steps and retriggers, the 1/16-cycle cap, the first sample after a reset, interpolation error per 32 samples against its bound, sample-accurate retriggers, block sizes 1/37/4096, rate changes, the random rows' statistics and determinism, the Lorenz step against an unfused reference, subnormals, allocation, and the lifecycle contract.

## References

- LennarDigital, *Sylenth1 User Manual*, v2.5, 2015, pp. 12, 19–20, 30, 32; [version history](https://lennardigital.com/sylenth1/whatsnew.php); v1.1, v1.20, and v1.21 release notes. The developer's and users' posts on KVR are listed in the [feature inventory](../reference/sylenth1-features.md#lfos), with confidence ratings.
- E. Brandt, "Hard sync without aliasing", *Proc. ICMC*, 2001: minBLEP. V. Välimäki and A. Huovilainen, "Antialiasing oscillators in subtractive synthesis", *IEEE Signal Processing Magazine* 24(2), 2007: PolyBLEP.
- E. N. Lorenz, "Deterministic nonperiodic flow", *Journal of the Atmospheric Sciences* 20(2), 1963: the attractor, and its sensitivity to initial conditions.
- D. Goldberg, "What every computer scientist should know about floating-point arithmetic", *ACM Computing Surveys* 23(1), 1991: rounding in accumulated sums.
- ISO/IEC 14882:2023, [expr.pre] (floating-point expressions may be evaluated with more precision than their type, which permits contraction within an expression), and the compilers' `-ffp-contract` and `/fp:contract` documentation.
- C. Roads, *The Computer Music Tutorial*, MIT Press, 1996: LFOs, sample and hold, and control rates.
