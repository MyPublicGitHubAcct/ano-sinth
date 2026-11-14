# Oscillator phase: start phase, retrigger, and free-running

Each of Sylenth1's oscillators has a **Phase** knob (0–360°) and a **Retrig** switch (inventory [§2](../reference/sylenth1-features.md#phase-and-retrig)). With Retrig on, every note-on restarts the oscillator at Phase. With Retrig off, the oscillator runs freely and Phase does nothing. This page covers:

- what a phase reset is, in signal terms;
- how to make one band-limited when it happens while the oscillator is sounding: the hard-sync technique, at a sub-sample time;
- why the sine needs different handling from the straight segments;
- free-running and the working hypothesis for where a free-running note starts;
- how all of this is built, measured, and tested.

Read [band-limited-oscillators.md](band-limited-oscillators.md), [oscillator-waveforms.md](oscillator-waveforms.md), and [oscillator-controls.md](oscillator-controls.md) first.

## Theory

### Phase, and what Phase 0 means

An oscillator's phase is how far it is through its cycle, as a fraction t ∈ [0, 1). Here it is stored as a 32-bit fixed-point number, 2³² per cycle ([phase.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/phase.hpp)). The Phase control picks the **start phase** of a retriggered note: 90° is t = ¼, 180° is t = ½.

For Phase to mean the same thing for every waveform, every waveform must be defined from the same origin. In [waveform.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/waveform.hpp) each shape is written over t ∈ [0, 1) with its cycle starting at t = 0:

| Waveform | At Phase 0 (t = 0) | At Phase 90 | At Phase 180 |
|---|---|---|---|
| Saw | −1, rising | −0.5, rising | 0, rising |
| Pulse | +1 (high first) | +1 | −1 (just after the fall) |
| Triangle | −1, turning upward | 0, rising | +1, turning downward |
| Sine | 0, rising | +1 | 0, falling |

These are working hypotheses (inventory W6), to be measured against the demo in M1.13. A measurement that disagrees changes the waveform table, and Phase keeps its meaning: an offset from each waveform's origin.

**A phase setting exactly on a corner lands just after it.** At Phase 180 the square is already low. The corner behind the start is not an edge: nothing jumped, because the note starts there.

### Why retrigger matters

- **A repeatable attack.** A retriggered note starts at the same point of the waveform every time, so its transient is the same on every note. That suits basses, plucks, and anything percussive. A free-running note starts wherever the oscillator happens to be, so its first cycle, and how it combines with the envelope's attack, differs from note to note.
- **A fixed relationship between oscillators.** Two oscillators at the same pitch, both retriggered, keep the phase offset that their Phase knobs set. This is how Sylenth1 gets PWM-style pulses from two saws, one inverted (manual p. 15; [oscillator-controls.md](oscillator-controls.md#invert-and-pwm-from-two-saws)): the pulse width is the phase difference. With Retrig off, the offset, and so the width, would be random on every note.
- **Free-running gives variety.** An analog oscillator never stops, so every note starts at an arbitrary phase, and several oscillators or unison voices start at unrelated phases. A stack of detuned saws then sounds full from its first cycle, instead of starting in phase and sweeping (a "flam" at every note-on). Users describe Retrig off as the setting for pads and supersaws, and Retrig on for basses ([Sonic Academy forum](https://forums.sonicacademy.com/t/phase-settings-on-oscilators/26166)).

### A phase reset is an edge

Resetting the phase from t_old to t_new at time t₀ makes the output jump in **value** by J = x(t_new) − x(t_old) and in **slope** by K = x′(t_new) − x′(t_old). That is exactly what a waveform's corner does ([oscillator-waveforms.md](oscillator-waveforms.md)). A naive reset therefore aliases just like a naive saw's edge: its spectrum falls only as 1/f above Nyquist and folds back. **Hard sync** is the same operation done once per period of a master oscillator. Brandt introduced minBLEP for hard sync without aliasing (2001).

There are two situations:

- **From silence.** A new voice starts with nothing before it, so the "old" waveform is 0: J = x(t_new), K = x′(t_new). This is the band-limited *onset* that minBLEP already used after `reset()`, now started at any phase.
- **While sounding.** A note-on reaches an oscillator that is still producing output: a stolen voice, a retriggered mono note, or (Part 2) hard sync. Resetting it to silence would make two errors. It would cut the edges already in flight short, by dropping their residuals. And it would add a step from the current output down to 0, followed by a second step up to the new waveform. Instead, the reset is one more edge, from the old waveform to the new one, and the residuals already in the ring stay.

### A band-limited jump at a sub-sample time

minBLEP models the phase as moving linearly between samples ([band-limited-oscillators.md](band-limited-oscillators.md)). Each corner crossed between samples n − 1 and n gets its residuals at its exact position there: `since` samples before sample n. A jump fits the same model. If it happens `since` samples before sample n (0 ≤ since < 1, so 0 for a note-on at a sample boundary), it splits the step from φ_{n−1} to φ_n into two parts:

1. **The old trajectory up to the jump.** It moves from φ_{n−1} to φ_from = φ_n − since·Δφ. Corners it crosses are edges as usual, timed relative to sample n.
2. **The jump itself.** A step residual of height J and a ramp residual of height K·Δφ (the bend per sample), both at `since`.
3. **The new trajectory after it.** It moves from the target to target + since·Δφ, which is sample n's phase. Corners it crosses are edges. A corner exactly at the target is behind the start and is not one.

The integer split since·Δφ is computed in 64-bit fixed point, so a jump to where the oscillator already is splits the step without changing anything.

**Why the ramp residual works for the straight segments.** The minimum-phase residuals delay every edge by the kernel's group delay d (about 1–2 samples). The straight segments are evaluated undelayed, and their output subtracts slope × d, which delays a straight line exactly. When a corner changes the slope by K, that compensation term steps by −K·d. So the ramp table is the integrated step residual *plus* d, which cancels the term's step (see [band-limited-oscillators.md](band-limited-oscillators.md)). A jump's bend is corrected by the same table, for the same reason.

### Why the sine needs different residuals

The sine has no corners. It is delayed by being *evaluated* at the phase d samples ago, read from a short phase history ([oscillator-waveforms.md](oscillator-waveforms.md)). For a straight line, "evaluate d late" and "subtract slope × d" are the same thing. For a sine they agree only to first order in 2π·f·d/fs: at 12 kHz and 48 kHz, d samples are about 0.4 of a cycle, and the linear model is far off.

So the sine's jump is handled where it actually happens in the output. The phase history keeps the **true trajectory**: old up to the jump, new after it, and each history entry records a jump inside its step. The delayed sine therefore switches from the old waveform to the new one exactly when the jump comes out of the delay, at x = d after it, where x is the time since the jump. The ideal output is the kernel h applied to the naive switched signal. Writing g = sin_new − sin_old and expanding g around the jump time:

  ideal − naive = g(t₀)·(BLEP(x) − u(x − d)) + g′(t₀)·(BLAMP(x) − (x − d)·u(x − d)) + (remainder)

where u is the unit step, BLEP and BLAMP are the band-limited step and ramp, and the remainder starts at x = d with zero value and zero slope. The table's rows are BLEP(x) − 1 (the step residual) and BLAMP(x) − x + d (the ramp residual). So the sine's residuals are the table's rows plus, for the taps before x = d, **1 for the step and x − d for the ramp**. Those are one or two taps at most.

What is left is a jump in the sine's **second derivative**, which two residual orders cannot correct. Its aliasing grows with the slave's pitch (see [Results](#results)). A third residual table, the integral of the ramp (a band-limited parabola), would remove it. Hard sync of a high sine is a Part 2 feature, and the table would be added there if it is wanted.

The first version treated the sine like a straight segment: the undelayed J and K with the table's rows, and the history rewritten at the jump. It was correct at low pitches but **unbounded at high ones**: under random jumps the output peaked at 3.4 at 5 kHz and 21 at 24 kHz. In hard sync at 1760 Hz and 48 kHz, with the slave at 5.8 kHz, it aliased worse than a naive jump (−20.2 against −29.8 dB). The stress test now pins the peak below 3.

### Free-running, and where it starts

With Retrig off, a note-on leaves a sounding oscillator alone. The phase is continuous, so there is no discontinuity to band-limit. The open question is where an oscillator starts when it has been **silent**: a new voice, or one that has finished. A hardware free-running oscillator has been running unheard all along, so its phase at the next note is arbitrary. The options:

| Option | Behaviour | Cost | Verdict |
|---|---|---|---|
| Continue from 0 | Every new voice starts at phase 0 | none | Behaves like Retrig on at Phase 0 for every fresh voice, so detuned unison voices start in step: the opposite of what free-running is for. |
| **Seeded random phase** | Each start from silence draws a phase from the slot's `Random` | one draw per note | **Chosen.** Statistically the same as an oscillator that kept running at an unknown pitch, and reproducible: the same seed and the same notes give the same render. |
| Keep idle voices running | Every voice's oscillators run all the time | the whole oscillator bank, always | What hardware does. It would cost CPU for silent voices, and the result is still an arbitrary phase per note. |

This is a **working hypothesis** (inventory O10), with low–medium confidence. One user describes Retrig off as starting "at random start point" ([Sonic Academy forum](https://forums.sonicacademy.com/t/phase-settings-on-oscilators/26166)), and MusicRadar's tutorial says the oscillator otherwise "cycl[es] freely regardless of note input". Neither says whether Sylenth1 keeps a running phase per voice or draws one per note. A black-box measurement would tell: render the same note several times in a row, Retrig off, and compare the start phases.

## How it is applied here

### The pieces

| Piece | Where | What it does |
|---|---|---|
| `phaseFromDegrees(degrees)` | [phase.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/phase.hpp) | Degrees to the fixed-point phase, wrapped into one cycle (−90 is ¾, 450 is ¼). Exact for 360/2^k; NaN and the infinities give 0. Called by the slot, so the oscillator only ever sees cycles. |
| `MinBlepOscillator::restart(phase)` | [min_blep.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/min_blep.hpp) | From silence at `phase`: the band-limited onset, starting at exactly 0. `reset()` is `restart(0)`. |
| `MinBlepOscillator::jumpTo(phase, since = 0)` | min_blep.hpp | The band-limited jump, `since` samples before the next sample. On a silent oscillator it is `restart()` at the phase the jump's trajectory reaches at the next sample: `phase` plus `since` × the *first sample's* increment, applied at that sample, so a pitch set after the call counts ([oscillator-range.md](oscillator-range.md#note-ons-at-a-sub-sample-time)). |
| `isRunning()`, `nextPhase()` | min_blep.hpp | Whether the oscillator has produced a sample since its last restart, and the phase of its next sample. |
| `OscillatorSlot::setPhaseDegrees`, `setRetrigger`, `setRandomSeed`, `noteOn(since = 0)`, `stop` | [oscillator_slot.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/oscillator_slot.hpp) | The controls, and the note-on that applies them. `since` places the note-on that far before the next sample, for events finer than a sample; every voice gets it through `jumpTo()`. |

The oscillator API is **per oscillator**, which means per unison voice. The slot's `noteOn()` applies the table below to every unison voice, reusing `restart` and `jumpTo` unchanged: with Retrig on, every voice restarts at Phase; with Retrig off, every voice from silence draws its own random phase, in voice order ([oscillator-unison.md](oscillator-unison.md)). Hard sync (Part 2) calls `jumpTo(phase, since)` with the master's sub-sample wrap time.

### What a note-on does

| | Oscillator silent (new or stopped voice) | Oscillator sounding (stolen voice, mono retrigger) |
|---|---|---|
| **Retrig on** | `restart(Phase)`: the onset at Phase | `jumpTo(Phase)`: a band-limited jump at the note-on's sample |
| **Retrig off** | `restart(random)`: a seeded random phase | nothing: the phase runs on |

- **`noteOn()` acts between two `process()` calls.** A voice splits its block at each event, so the note-on is sample-accurate. A note-on in the middle of a block gives bit for bit the same output as one at a block boundary (tested).
- **Phase is read at the note-on.** Turning the knob during a held note changes nothing until the next retrigger.
- **`stop()` is for a voice that has finished.** It silences the oscillator, so the next note starts from silence and not as a jump, and it finishes any gain ramp. It does *not* rewind the random sequence, so successive free-running notes draw different phases. `reset()` is `stop()` plus rewinding the sequence, so a reset slot is exactly a freshly prepared one (the lifecycle contract).
- **Each slot needs its own seed** (`setRandomSeed`), or two free-running slots would start in step. Within a slot, the unison voices draw consecutive values, so they start at independent phases (tested: two free-running voices' correlation averages 0 over 64 seeds).
- **The default is Retrig on at Phase 0**, which is how the slot behaved before Phase and Retrig existed. The patch default is the parameter table's business (M1.9).

### Inside the oscillator

- **The per-sample path is unchanged.** A jump does its work at once, in `jumpTo()`: it adds the residuals of the corners crossed in the split step and of the jump itself. It then sets the arriving step to 0 and the distance to the next corner to 0. The next sample's existing one-compare check falls into the slow path, which crosses nothing (the step is 0) and finds the new segment and the next corner. No per-sample branch was added.
- **The scan for crossed corners** (`addCrossing`) is shared by the per-sample path and the jump. It takes the span of phase to check and a time offset, so the jump's two partial steps reuse it. On the per-sample path it computes exactly what the old code did, bit for bit.
- **The sine's history** stores, for each sample, its phase, the step that led to it, and (after a jump) where the old trajectory would have been and when the jump came. The delayed phase is read from the old or the new trajectory, depending on which side of the jump it falls. Without jumps the arithmetic is the same as before, so the sine's existing timing test is unchanged.

## Trade-offs and alternatives

| Alternative | Measured or argued | Verdict |
|---|---|---|
| **Reset to silence on every note-on** (what `reset()` did) | Cuts residuals short and steps to 0. In hard sync at 440 Hz, 44.1 kHz: −40.8 dB worst alias for the saw, against −108.9 dB for the jump. | Rejected for a sounding oscillator. Kept for a silent one, where it is exact. |
| **A naive jump** (set the phase, no residuals) | −24.6 dB in the same case. | Rejected. |
| **A short crossfade** from the old phase to the new | Removes the click but smears the attack over the fade, and the faded mix of two phases is not the waveform. Retrig exists to give a sharp, repeatable start. | Rejected. |
| **No bend correction** (the jump's step only) | The triangle and TriSaw jump from a falling to a rising slope. In hard sync at 440 Hz, 48 kHz: triangle −44.5 dB against −122.5 dB with the bend; at 1760 Hz −20.4 dB, worse than naive (the slope × delay term then steps unbanded). | Rejected. |
| **The sine like a straight segment** | Unbounded at high pitch (peak 21 at 24 kHz); worse than naive in sync at 1760 Hz (−20.2 dB). | Rejected for the delayed-naive residuals above. |
| **A third residual order** (band-limited parabola) for the sine | Would remove the sine's remaining 2nd-derivative error (−51 to −55 dB with the slave at 6.5 kHz). Costs a third table and a third residual add per sine jump. | Deferred to Part 2 hard sync. A note-on retrigger is far rarer: at 8 Hz it measures −147 dB. |
| **`noteOn(offset)` inside `process()`** | Needs an event list in the slot. Splitting the block in the voice is how JUCE's synthesiser already renders between events. | Splitting chosen. The oscillator still takes a sub-sample `since` for hard sync. |
| **Free-run: continue from 0, or keep idle voices running** | See [above](#free-running-and-where-it-starts). | Seeded random phase chosen, as a hypothesis (O10). |
| **The onset from silence for the sine** | It still leaves the sine's bend uncorrected ([oscillator-waveforms.md](oscillator-waveforms.md)). The delayed-naive residuals could correct it, but the onset would then no longer start at exactly 0. | Left as is. |

## Results

From the `[phase-report]` run ([oscillator_phase_report.cpp](../../tests/dsp/oscillators/oscillator_phase_report.cpp), under a second in `relwithdebinfo`), 64 taps, no oversampling.

### Retrigger aliasing, measured as hard sync

A retrigger once per master period: the oscillator runs at 3.7 times the master's rate and is reset to Phase 0, so each jump is from t = 0.7. That is a jump in value for the saw and pulses, and a jump and a bend for the triangle, sine, and TriSaw. The master periods are not whole numbers of samples, so the jumps fall between samples and the aliasing lands between the harmonics, where it can be measured. Worst alias below 20 kHz, in dB relative to an ideal saw's fundamental, at 44.1 kHz (48 kHz is within 4 dB of every cell):

| Waveform | Master 8.7 Hz (a note-on rate) | 110 Hz | 441 Hz | 1760 Hz |
|---|---|---|---|---|
| Saw | −141.2 / −57.3 / −73.8 | −120.2 / −35.6 / −52.6 | −108.9 / −24.6 / −40.8 | −98.4 / −12.7 / −24.5 |
| Pulse | −134.9 / −50.8 / −70.0 | −113.5 / −29.4 / −47.5 | −102.8 / −18.1 / −37.6 | −92.6 / −10.4 / −20.8 |
| HPulse | −135.1 / −50.8 / −70.0 | −113.8 / −30.2 / −48.3 | −104.7 / −20.9 / −36.6 | −96.1 / −9.4 / −24.2 |
| QPulse | −135.5 / −51.1 / −70.0 | −114.1 / −28.9 / −48.3 | −103.5 / −19.3 / −34.4 | −91.7 / −6.1 / −22.8 |
| Triangle | −151.9 / −72.9 / −75.5 | −135.2 / −50.7 / −53.0 | −120.2 / −36.9 / −39.1 | −102.7 / −24.1 / −28.8 |
| Sine | −146.8 / −75.0 / −75.1 | −124.3 / −53.3 / −54.2 | −88.1 / −41.2 / −43.0 | −51.3 / −28.2 / −24.1 |
| TriSaw | −146.1 / −62.1 / −74.6 | −124.9 / −40.3 / −52.8 | −113.6 / −29.4 / −39.9 | −102.8 / −17.8 / −28.4 |

Each cell is **band-limited jump / naive jump / restart from silence**.

- **The band-limited jump is 78–87 dB cleaner than a naive one** for every piecewise-linear waveform, at every master rate. Even syncing at 1760 Hz (the slave at 6.5 kHz), the worst alias stays below −91 dB, which matches the oscillator's own static aliasing ([oscillator-waveforms.md](oscillator-waveforms.md)).
- **Restarting from silence is barely better than a naive jump** (from 4 dB worse to 20 dB better) and 27–76 dB worse than the band-limited jump. It band-limits the step up from 0, but the cut down to 0, and the residuals it drops, are not band-limited.
- **The sine is limited by its second derivative** at high slave pitches (−51.3 dB with the slave at 6.5 kHz), as the theory predicts. At note-on rates it is −147 dB.

### Peaks

Peak output under 300 jumps to random phases at random sub-sample times, 1 to 16 samples apart (48 kHz): at most 2.61 (the square at 21 kHz), 1.92 for the sine, against 1.44–2.06 for the waveforms running freely. Two overshooting edges close together add, as they do for the narrow pulses ([band-limited-oscillators.md](band-limited-oscillators.md)).

| Waveform | 1 kHz | 5 kHz | 12 kHz | 21 kHz | 24 kHz |
|---|---:|---:|---:|---:|---:|
| Saw | 1.49 | 1.33 | 1.07 | 1.23 | 0.91 |
| Pulse | 1.84 | 1.80 | 1.78 | 2.61 | 1.85 |
| HPulse | 1.89 | 1.76 | 2.26 | 2.06 | 1.76 |
| QPulse | 1.78 | 1.96 | 1.81 | 1.60 | 1.57 |
| Triangle | 1.35 | 1.18 | 1.09 | 1.43 | 1.30 |
| Sine | 1.48 | 1.34 | 1.91 | 1.79 | 1.92 |
| TriSaw | 1.19 | 0.99 | 0.81 | 0.95 | 0.75 |

### CPU

Nanoseconds per sample, 64 taps, 48 kHz, `relwithdebinfo`, two runs in one session ([baselines](../performance/baselines.md)):

| Waveform, 440 Hz | Without jumps | Retriggered every 64 samples | Per jump |
|---|---:|---:|---:|
| Saw | 2.97–2.98 | 3.10–3.17 | 8–13 ns |
| Square | 3.41–3.44 | 3.77–3.84 | 21–27 ns |
| Triangle | 3.46–3.50 | 3.95–3.98 | 31 ns |
| Sine | 8.15–8.22 | 8.73–8.80 | 33–42 ns |

- **A jump costs roughly its residual adds:** one 64-tap step residual for a saw, a ramp as well where the slope changes, and for the sine two more `sin`/`cos` pairs. One retrigger per note is not measurable: a slot retriggered every block runs at the same speed as a settled one (3.11 against 3.16–3.25 ns).
- **As hard sync,** a 1 kHz saw jumped every 44 samples, between samples, costs 4.7 ns per sample.
- **The per-sample path is unchanged:** the oscillator rows at 440 Hz are within 0–3% of the parent commit, measured in the same session. Rows dominated by corner crossings (12 kHz) are 2–6% slower: the shared crossing scan adds a time offset to each crossing, and the sine's history entries are wider.

## Tests

- [oscillator_phase_test.cpp](../../tests/dsp/oscillators/oscillator_phase_test.cpp), for minBLEP at 32 and 64 taps:
  - `phaseFromDegrees`: 0, 45, 90, 180, 270 exactly; 360, 450, −90, and −720 wrap; 1° and 359.9° to float precision; a tiny negative angle wraps below a whole cycle or to 0, never past it; NaN and ±∞ give 0;
  - `restart()` at 7 phases, on and between corners, for all 7 periodic waveforms: the first sample is exactly 0, and from then on the output is the waveform from that phase, delayed by `latencySamples()`, within 10⁻⁴ away from edges;
  - `restart(0)` is `reset()`, bit for bit;
  - a jump mid-note to 5 phases at `since` 0, 0.375, and 0.9: after its residual, the output is the waveform from the new phase at time n − since, within 10⁻⁴, and stays within the candidate's peak;
  - a jump to where the phase already is changes nothing: bit for bit at a sample, within 10⁻⁵ between samples, at 55 Hz, 1 kHz, and 12 kHz. So a retrigger never drops the oscillator to silence or loses residuals;
  - a jump is exactly a natural edge: the square jumping from ¼ to ¾ equals a square crossing its edge at ½ at the same instant (a step of −2), and the triangle doing the same equals a triangle crossing its corner (a bend of −8), offset by 1, within 10⁻⁶, at `since` 0 and 0.375;
  - `jumpTo` on a silent oscillator is `restart()`, bit for bit, including between samples;
  - `reset()` after jumps restarts identically;
  - finite, below 3, and free of subnormals under dense random jumps from 0 Hz to Nyquist;
  - hard sync at 440 Hz (slave 1.63 kHz, 48 kHz): saw, square, and triangle alias below −80 dB, and at least 30 dB less than a naive jump.
- [oscillator_slot_test.cpp](../../tests/dsp/oscillators/oscillator_slot_test.cpp):
  - **the O7 PWM trick through the new API:** two saws, both retriggered, one inverted at Phase 90, sum to the ideal 25% pulse (within 2·10⁻⁴), with a mean within 10⁻⁶ of 0, no 4th or 8th harmonic, and a fundamental of (4/π) sin(π/4). Retriggering both mid-note restores the same pulse;
  - with Retrig on, a note-on starts every waveform at Phase 0, 90, 180, and 270, from silence and while sounding: the first cycle after it matches the ideal waveform shifted by Phase, within 10⁻⁴;
  - a retrigger is the oscillator's `restart` or `jumpTo`, bit for bit;
  - a Phase change during a held note does nothing until the next note-on, which then jumps to it;
  - with Retrig off, a note-on leaves a sounding oscillator bit for bit untouched, and Phase is ignored;
  - with Retrig off, a note from silence starts at the seeded random phase. `reset()` reproduces it, a stopped voice draws the next phase, and another seed gives another;
  - after `stop()`, the next note-on is an onset from silence;
  - a note-on inside a block equals the same note-on at a block boundary, bit for bit, on both channels, with Retrig on and off and a volume ramp in flight;
  - the lifecycle contract, with note-ons every 300 samples, retriggered and free-running.
- [oscillator_phase_report.cpp](../../tests/dsp/oscillators/oscillator_phase_report.cpp) (`[phase-report]`, hidden) prints the tables above.

Every new test was checked to fail with a deliberate break in the code it covers (24 breaks, at least one per test; the guard against NaN in `phaseFromDegrees` prevents undefined behaviour in a float-to-integer cast, so its break shows only in the UBSan build), then the code was restored.

## References

- LennarDigital, *Sylenth1 User Manual* v2.5, 2015, pp. 14–15: Phase, Retrig, and the two-saw PWM trick. Sources and open questions: the [feature inventory](../reference/sylenth1-features.md#phase-and-retrig).
- E. Brandt, "Hard Sync Without Aliasing", Proc. ICMC, 2001: minBLEP, introduced for exactly this jump.
- F. Esqueda, V. Välimäki, S. Bilbao, "Rounding Corners with BLAMP", Proc. DAFx-16, 2016: the band-limited ramp for slope discontinuities, including those of hard sync.
- V. Välimäki, A. Huovilainen, "Antialiasing Oscillators in Subtractive Synthesis", IEEE Signal Processing Magazine 24(2), 2007: a survey of band-limiting methods and their treatment of sync.
- T. Stilson, J. O. Smith, "Alias-Free Digital Synthesis of Classic Analog Waveforms", Proc. ICMC, 1996: why discontinuities alias, and the band-limited impulse train.
