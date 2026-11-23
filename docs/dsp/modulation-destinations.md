# Modulation destinations

A destination is the control a route moves. [modulation.md](modulation.md) decided each per-voice destination's rate and how sums are formed, and [modulation-sources.md](modulation-sources.md) built the sources and wired Pitch, Mix, Cutoff, Reso and the LFOs' Rate and Gain into `dsp::Voice`. This page is M1.5 task 4. It covers the destinations left: **Volume, Pan and Phase**, each through its units, its decided rate and its clamp. It also covers what the rest of the voice needs around them: the slot's knobs while its gains are computed, what a change of the routes does, Noise, and the skip test.

- **Theory:** adding before a knob's law and clamping before it; Phase's wrap and an unclamped Pitch; a gain computed outside its smoother, and keeping two paths in step; Pan's law on the grid; stepping or crossfading a change of the routes; why a Noise slot needs its own path; proving a modulated slot silent.
- **Sylenth1:** what is known, from task 1 and two developer threads found for this task (inventory §6, [The destinations in detail](../reference/sylenth1-features.md#the-destinations-in-detail), M24–M27).
- **Stage 1:** the candidates, prototyped on the library's own classes and measured; the recommendations; what stage 2 needs, and what this pushes back on.
- **The decision and stage 2:** every recommendation chosen; Volume, Pan and Phase as built in `OscillatorSlot` and `dsp::Voice`, with the crossfade of route changes; two flaws stage 2 found in the prototype (both block-size dependences) and fixed; the tests, the deliberate breaks, and CPU.

Read [modulation.md](modulation.md) and [modulation-sources.md](modulation-sources.md) first, then [oscillator-controls.md](oscillator-controls.md) (Volume, Pan, Inv and their 10 ms ramps), [oscillator-phase.md](oscillator-phase.md) (Phase and Retrig), [oscillator-unison.md](oscillator-unison.md) (the unison voices' positions and the Noise mix), and [part-solo-and-skipping.md](part-solo-and-skipping.md) (the skip test and the mono path).

## Theory

### Adding before the law, clamping before the law

Sylenth1's modulation adds in the knob's units ([modulation.md](modulation.md#additive-modulation-in-knob-units)). For a destination with a knob, the value is

> law(clamp(knob + Σ amount × source)).

The clamp holds the sum to the knob's own range (inventory F24, M9). It sits before the law, so modulation can never reach a value the knob can't. Each destination differs only in its law and its range:

- **Volume:** linear (O4), 0–10, 10 is unity. Below 0 the clamp gives silence. The alternative is a negative gain, which inverts the waveform; that is what Sylenth1's v2 engine did.
- **Pan:** 0–10 with 5 the centre. Within the range the law is equal power at each unison voice's position (O6, O8). Past the hard sides the clamp holds the position there.
- **Phase:** 10 units a cycle. A phase has no ends, so its sum **wraps** rather than clamps: −2.5 is 270°, and +10 is no change at all.
- **Pitch:** an octave per unit, and **no clamp** (M9). Pitch has no knob range to hold it to. The question is only what the oscillator does at the extremes. Above Nyquist ours plays the waveform's mean (O17). Below about 10⁻³⁸ Hz the frequency rounds to 0 and the oscillator stops, holding its value.

### A gain computed outside its smoother

`OscillatorSlot` folds Volume, Pan and Inv into one gain per channel for each unison voice. It ramps those gains over 10 ms in a `StereoGain`, so a knob turn never clicks and a settled slot costs one multiply-add per channel per sample. Modulating Volume per sample, or Pan on the grid, means computing those gains outside the `StereoGain`. The slot then has **two paths** to the same output, and it switches between them whenever a route to it appears or goes:

1. **The knob itself must still ramp on the modulated path.** If the modulated path reads the knob's target, a knob turn while a route exists steps the gain at once and clicks. The ramp can live in two places:
   - **in the slot**, as its own 10 ms ramps in knob units, started where its gain ramps start (one source of truth);
   - **in the engine**, as a global ramp read at absolute samples, as Mix's is (two sources of truth, one per path).
2. **The paths must agree where they meet.** At a switch, the modulated path's gain at a sum of 0 should be the unmodulated path's gain. That holds only if each path's ramps were moved on while the other path rendered. A `StereoGain` that is frozen while the modulated path runs comes back where it stopped, not where it should be.
3. **Unison's fades must survive.** When the voice count changes while the slot sounds, the voices that come or go fade over the gain ramp ([oscillator-unison.md](oscillator-unison.md)). On the modulated path that fade needs a ramp of its own, a per-voice presence from 0 to 1, or voices pop in and out.
4. **Pan's law between grid points.** A `StereoGain` ramp interpolates the two channel gains linearly, a chord of the equal-power curve: a centre-to-side swing dips by up to 3 dB mid-ramp. Pan modulated on the grid computes the law at each grid point and interpolates between them. While the Pan knob ramps, the two paths therefore differ, by the chord against the curve.

### Stepping or crossfading a change of the routes

The routes are compiled at the voice's grid points ([modulation.md](modulation.md#as-built-the-model)). A change of the patch therefore reaches each voice at its next grid point. This covers an amount turned, a destination picked, a preset loaded, or an amount automated. Each destination's sum then jumps by the difference between the old routes' contribution and the new routes'. The two answers:

- **A step.** It is exact and costs nothing, but the click is the destination's response to the jump. A new route on Mix at −5 is a gain step by half.
- **A crossfade.** Over the next N segments, each sum moves linearly from what the old routes give to what the new routes give, both from the sources as they are now. The old routes' sources keep running until it ends, and a further change waits for its end. This covers an amount and a menu change the same way, with nothing new in the engine.

A crossfade has three properties worth stating:

- **Its bound is free.** A sum part-way through lies between the old routes' sum and the new routes' sum. The larger of the two upper bounds is therefore a bound on it.
- **The lag is fixed per change.** Under automation, though, changes queue behind the crossfade in progress, so the lag can reach about two crossfades.
- **Its alternative needs more machinery.** The common alternative is to ramp the *amount* as a knob (10 ms, in the parameter layer). That doesn't cover a destination or source picked from a menu, which steps whatever the amount does.

### A Noise slot's mix

A Noise slot doesn't render a noise per unison voice. It mixes two independent draws into the two channels with three gains (left, cross, right). Those gains give the power and the left/right correlation that N voices at their gains would have (`unisonNoiseMix`, [oscillator-unison.md](oscillator-unison.md)). The mix is homogeneous of degree 1 in the voices' gains: scaling every voice by g scales the mix by g. So Volume factors out of it, and a modulated Volume can multiply a mix computed at unit volume. Pan changes each voice's position, so the mix has to be recomputed (three square roots). Doing that at the grid points and interpolating between them puts Pan on Noise at the same rate as Pan elsewhere.

### Proving a modulated slot silent

The skip test skips a slot whose output is exact zeros ([part-solo-and-skipping.md](part-solo-and-skipping.md)). A slot at Volume 0 is silent unless its Volume routes can lift it. With the clamp at 0, the slot is silent over a segment exactly when

> the Volume knob's largest value over the segment + Σ amount × (its source's upper bound there) ≤ 0.

A linear knob ramp is largest at one of its ends. The sources' bounds are task 3's ([modulation-sources.md](modulation-sources.md#as-built)). A bound that is too small skips audio; one that is too large only costs CPU. Phase doesn't affect silence. Pan doesn't either, since the equal-power law never gives zero on both channels.

## Sylenth1: what is known

Task 1 gave each destination's units and its clamp as working hypotheses (inventory M8, M9). For this task, two threads on the developer's support forum add the following ([The destinations in detail](../reference/sylenth1-features.md#the-destinations-in-detail)):

- **Volume below 0 mutes in v3.** "In v2 the osc inverses the wave amplitude at negative volume, in v3 it will mute negative volume" (Lennard, 6 May 2026). A user's presets with both oscillator Volumes at 0 and an LFO "set to modulate AB Volume at 10" with Offset −10 were silent in v3. The developer: "the volume will modulate between -10 and 0 and never reach above 0, so indeed the output will be muted". That confirms M9 for Volume (a clamp at 0, as X3 for Mix). It also fits the units M8 and M6 give: an LFO at Offset −10 spans −1 to 0 as a source, so −10 to 0 at amount 10.
- **Pan works on Noise.** A user in 2023: "When using Osc 2 with noise waveform, and setting one of the LFOs to control pan, it just doesn't work." The developer replied "Works fine here", and the thread ends without a preset. Separately, the developer named "a bug in v3.073 in the noise osc in the v2engine", fixed in v3.075 (28 April 2026).
- **Phase as PWM.** Tutorials use Phase A as a destination for the saw-and-inverted-saw trick, an LFO on Phase A with the second saw inverted. A forum poster notes it is exact only when the oscillators restart at each note (Retrig).
- **Nothing on what a route's change does.** No source says whether an amount's change, or a destination picked on a held note, steps or ramps. v3.008's "Clicks and pops during automation issue fixed" is about host automation in general (M24).

## The candidates

Stage 1 prototypes the destinations in `tests/`, on the library's own classes:

- **`DestVoice`** ([destination_voice.hpp](../../tests/dsp/modulation/destination_voice.hpp)) is a copy of `dsp::Voice` as built (task 3).
- **`DestSlot`** ([destination_slot.hpp](../../tests/dsp/modulation/destination_slot.hpp)) is a copy of `OscillatorSlot`, grown from task 2's `ModOscillatorSlot`.
- **`DestEngine`** ([destination_support.hpp](../../tests/dsp/modulation/destination_support.hpp)) is task 3's `LibraryEngine` with these voices, over the real `VoiceAllocator`. It adds knob events that set a slot's Volume or Pan on every voice at the event's sample.

What every candidate shares is the decision:

- Volume and Phase in the folded per-sample sums.
- Pan's sums at each grid point, ramped from the previous grid point's sum to this one's (a segment late).
- Each clamp (Volume and Pan to [0, 10]) and Phase's wrap.
- A Pan route takes its part off the mono path.
- Phase:
  - **Folded into the phase step:** each sample's change of the offset is added to the oscillator's phase step. Where the step would leave [0, Nyquist), the change is made as a band-limited jump instead.
  - **At a note from silence,** the onset lands at Phase plus the Phase sum as the sources stand at the note-on.
  - **A retrigger** lands at Phase plus the offset in force.
  - **When the routes go,** the offset returns to 0 through the sum.
  - **A skip** carries the offset's change into the phase it adds up.

| Question | Candidates |
|---|---|
| **1. The knobs under modulation** (Volume, Pan) | (a) the knobs' targets, unramped (task 2's prototype); (b) **the slot's own 10 ms ramps in knob units**, started where its gain ramps start; (c) global ramps per slot in the engine, read at absolute samples, as Mix's |
| **2. A change of the routes** (an amount, a destination, a source) | (a) a step at the voice's next grid point (as built); (b) a crossfade of the sums over 1 segment; (c) a crossfade over 15 segments, 10 ms at 48 kHz |
| **3. Volume and Pan on a Noise slot** | (a) ignored (task 2's prototype rendered Noise unmodulated); (b) the mix at unit volume from the pan positions at the grid points, interpolated, times the volume per sample |
| **4. The skip test with a Volume route** | never silent while a route reaches the part; silent from the knob's larger end plus the routes' bound (task 2's decision); and, to show the tests catch it, a deliberately small bound (the sources' values at the grid point) |
| **5. Phase: a step of the offset** | folded into one sample's phase step (the decision) against a band-limited jump |
| **6. Pitch at its extremes** | unclamped (M9), checked at ±160 octaves |

Behind every candidate:

- **The slot's gain ramps move on whichever path renders.** `SkippableGain` is `StereoGain` with `skip()`.
- **Each unison voice has a presence ramp** for its fades.
- **The slot's own knob ramps advance in step.**

So the slot can switch paths at any grid point.

## What they measured

The candidates were measured by the hidden `[destinations-report]` ([destination_report.cpp](../../tests/dsp/modulation/destination_report.cpp), under a second) and the `[destinations]` benchmarks ([destination_benchmark.cpp](../../tests/dsp/benchmarks/destination_benchmark.cpp)). The default run checks the invariants under `[destinations-model]` ([destination_candidates_test.cpp](../../tests/dsp/modulation/destination_candidates_test.cpp), 6 test cases; [Tests](#tests)).

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[destinations-report]"
tools/bench.sh "[destinations]"
```

### The prototype against the library

`DestVoice` with no Volume, Pan or Phase route, against `dsp::Voice`, through `VoiceAllocator`:

| Patch | host blocks of 1 | of 37 | of 4096 |
|---|---|---|---|
| init | identical | identical | identical |
| task 3's busy patch (Pitch, Mix, Cutoff, the LFOs; every source) | identical | identical | identical |

The default run checks the same under three candidate sets (each knob source, the step and the 10 ms crossfade, both Noise rules, both skip rules).

### Question 1: the knobs under modulation

The setup: A1 a sine at A3, filters bypassed, one note. A route from the wheel, left at 0, puts the slot on the modulated path with a sum of exactly 0. Any difference from no route is therefore the knob's.

**A knob step while modulated** (filters.md's click: energy above 1 kHz in a Hann 4096 after the step, dB re an instant unit step):

| Knob step at 0.5 s | no route (the slot's gain ramps) | (a) targets | (b) the slot's ramps | (c) global ramps |
|---|---:|---:|---:|---:|
| Volume 10 → 2 | −46.1 | **−20.8** | −46.1 | −46.1 |
| Pan 5 → 10 | −46.5 | **−21.2** | −46.1 | −46.1 |

**A route appearing or going while the knob ramps.** The knob moves 3 ms before the grid point at which the route appears or goes. The step is the largest difference from no route at all, after the switch, over 50 ms, in dB re the sine's peak:

| Case | (a) targets | (b) the slot's ramps | (c) global ramps |
|---|---:|---:|---:|
| Volume 10 → 2, the route appears | −6.6 | **−139.5** | −55.6 |
| Volume 10 → 2, the route goes | exact 0 | exact 0 | exact 0 |
| Pan 5 → 10, the route appears | −4.7 | −20.4 | −20.4 |
| Pan 5 → 10, the route goes | exact 0 | exact 0 | exact 0 |

**A note on a voice that was idle while the knob moved.** The knob moves with no note sounding, and a note starts 2 ms later. The silent route is set from the start; the reference has none. The figure is the largest difference over the note's first 20 ms, dB re its peak:

| Case | (a) targets | (b) the slot's ramps | (c) global ramps |
|---|---:|---:|---:|
| Volume 10 → 2 | exact 0 | exact 0 | **+8.7** |
| Pan 5 → 10 | exact 0 | exact 0 | **−5.8** |

- **(a) clicks on every knob turn while a route exists**: −20.8 dB, against −46.1 for a ramp. A route that appears mid-ramp steps by −6.6 dB.
- **(b) is the unmodulated path at every switch**: −139.5 dB on Volume (float rounding), and exact on leaving. It agrees on an idle voice too, because the slot's ramps and gain ramps settle together.
- **(c) has two truths.** An idle voice's slot settles at once (nothing is heard yet), while the engine's ramp is still moving. A note starting then is +8.7 dB off with a route against without one. Its reads also sit a sample off the slot's (−55.6 dB at a switch, the read convention of `valueAfter(at + i)` against `next()`).
- **Leaving the modulated path is exact for every candidate.** The slot's gain ramps were moved on with it (`skip()`).
- **Pan mid-ramp is the one inexact switch**, at −20.4 dB under (b) and (c): the chord of `StereoGain`'s ramp against the law at the grid points. It needs a route to appear or go within 10 ms of a Pan knob move.

### Question 2: a change of the routes

**A route appearing on a held note.** Velocity 127 (a source of 1) to the destination, set mid-segment at 0.5 s. The click is measured at the grid point that takes it. For Cutoff, the sine sits at the cutoff (Cutoff A 0 plus the Filter Control's 5.6, 221 Hz), and +3 takes it to 1,770 Hz.

| Route | (a) step | (b) crossfade, 1 segment | (c) crossfade, 10 ms |
|---|---:|---:|---:|
| Mix A −5 | −11.5 | −23.4 | **−48.3** |
| Volume A −5 | −11.5 | −23.4 | **−48.3** |
| Pan A +5 | −19.6 | −19.6 | **−44.2** |
| Pitch A +0.10 (1.2 semitones) | −46.5 | −54.5 | **−78.7** |
| Phase A +2.5 (90°) | −12.3 | −9.6 | **−39.0** |
| Cutoff A +3 | −15.7 | −15.7 | **−28.0** |

**An amount automated in 7-bit steps.** Velocity 127 on Mix A (or Volume A). The amount goes from 0 to −10 in 64 steps of 20/127, one every 3 ms (a fast turn, 0.19 s), each falling mid-segment. Zipper is the click measure averaged over 8 points of the sweep. Lag runs from the last step to the sum's arrival; Mix reaches its 0 there, so the output is exactly 0 once it arrives.

| | (a) step | (b) crossfade, 1 segment | (c) crossfade, 10 ms |
|---|---:|---:|---:|
| zipper, Mix A | −31.7 | −43.5 | **−77.5** |
| zipper, Volume A | −31.7 | −43.5 | **−77.5** |
| lag of an isolated change (samples at 48 kHz) | 1–32 | 1–32 + 32 | 1–32 + 480 |
| lag of the sweep's last step, Mix A (samples) | 25 | 56 | 536 (11.2 ms) |

- **The step clicks wherever a gain moves**: −11.5 dB for Mix and Volume, as loud as task 3's late route (−22.8 dB on a saw through the filter, a different setup).
- **One segment is too short.** On Pan and Cutoff it is the step itself, because they take one value per grid point and the blend reaches 1 at the first one. On Phase it is worse than the step (−9.6 dB): a 90° change over 0.67 ms is a 375 Hz blip of frequency.
- **10 ms takes every click down by 12–37 dB and the zipper by 46 dB.** Pan and Cutoff, held per grid point, move in 15 steps. The 7-bit sweep arrives 11.2 ms after its last step (one crossfade and part of the one in progress) and follows the sweep throughout.
- **Cutoff keeps the most (−28.0 dB).** The filter takes its cutoff once per segment ([filter-control.md](filter-control.md)), so a crossfade is a staircase there. The Cutoff knob's own 10 ms ramp, read the same way, behaves the same.

### Question 3: Volume and Pan on a Noise slot

A1 Noise, 4 unison voices at Stereo 0.5, filters bypassed. The wheel is at 127, so a route adds its amount, compared with the knob turned there and no route. The figures are left and right RMS over 0.5 s, dB re the knob's:

| Case | (a) ignored | (b) mixed |
|---|---|---|
| Volume 0 + 5, against Volume 5 | silent / silent | 0.00 / −0.01 |
| Pan 5 + 5, against Pan 10 | +8.34 / −2.69 | 0.00 / 0.00 |

Ignored, a Volume route on Noise does nothing at all, and a Pan route leaves the noise at its knob. That is the user's 2023 report, which the developer couldn't reproduce. Mixed is the knob's level and position within 0.01 dB. It isn't the knob to the bit, because the volume multiplies the mix rather than going inside it.

### Question 4: the Volume skip test

A1 and B1 saws, own filters. B1 sits at Volume 0 with one route lifting it. The test plays 16 notes of 0.2 s, one every 0.25 s, with releases of 50 ms. *Skipped* counts B1's grid points skipped (silent, or its part not taken) out of those at which it sounded. *Accuracy* is the bound's render against never skipping, dB re the render:

| Route on Volume B | never: skipped | the routes' bound: skipped | the bound against never |
|---|---:|---:|---:|
| LFO 1 sine 8 Hz, +10 (±5 units) | 0.0% | **49.9%** | −96.9 |
| LFO 1 pulse 8 Hz, Offset −10, at −10 (0 to +10) | 0.0% | **49.6%** | −67.0 |
| Mod Env 1 pluck (decay 50 ms, sustain 0), +10 | 0.0% | **80.0%** | exact 0 |
| Velocity (1 to 127), +10 | 0.0% | 0.0% | exact 0 |

- **The bound skips what can't be heard**: half an LFO's cycle, and everything after a pluck. Never skipping keeps rendering a slot that the clamp holds at 0.
- **What a skip costs is the band-limited jump's residual at its return** (−67 dB at worst, where the pulse lifts the slot). It is exact where the slot returns at a note-on.
- **A deliberately small bound skips audio.** Taking the sources' values at the grid point skips the segments in which a pulse falls, and the default run's test catches it.
- **CPU:** 16 voices of the pluck patch cost 2,083.6 ns per sample with the bound, against 3,235.1 never skipping (−36%).

### Question 5: Phase, a step of the offset

One saw at 1,090 Hz (off any simple ratio to 48 kHz). The offset steps at one sample: either folded into that sample's phase step (the decision), or made as a band-limited jump (a retrigger to the same phase, which is what the decision does where the step would leave [0, Nyquist)). The figure is their difference's energy in a Hann 1024 centred on the step, dB re the jump's energy there:

| Step | below 16 kHz | above 16 kHz | made as a jump? |
|---|---:|---:|---|
| 45° | −41.2 | −45.8 | no |
| 90° | −35.2 | −39.8 | no |
| 170° | −29.4 | −33.1 | no |

Folding a step into one sample's phase step amounts to a band-limited jump smeared over that sample. It stays within −29 to −41 dB of the jump, and the difference grows with the step. Steps are rare at the source: LFO edges slew over 1 ms, envelopes and controllers ramp, and under question 2's crossfade a route's arrival is spread over 10 ms. What remains is an envelope with no attack. Nothing here argues against the decision.

### Question 6: Pitch at its extremes

A1 a saw, LP24 at the init cutoff, with 16 routes at +10 (or −10) from sources at 1: ±160 octaves on the note.

| Routes | every sample finite | peak after 50 ms |
|---|---|---:|
| 16 × +10 | yes | 0.0000 |
| 16 × −10 | yes | 0.7071 |

- **Unclamped is safe.** Up, the frequency overflows to infinity, which `incrementFor` takes as Nyquist, so the saw plays its mean: silence.
- **Down, it underflows to 0 Hz and stops.** It holds the value it had: DC, here 1.0 panned, through the low-pass. An oscillator at 0 Hz does exactly that. The DC goes with the note's amp envelope. Whether Sylenth1 does the same is M27.

### Real-time terms

Every candidate gives the same output at host blocks of 1, 37 and 4096, with no allocation, on a busy patch:

- Volume, Pan and Phase routes from moving sources;
- a Noise slot;
- unison;
- knob and patch changes mid-note;
- steals.

`DestVoice` holds everything in fixed arrays: two more per-sample sums per part, the crossfade's old routes and sums, and per slot two knob ramps, eight presence ramps and the cached pan gains.

### CPU

`tools/bench.sh "[destinations]"`, `relwithdebinfo`, Apple M1 Ultra (virtualised), Apple Clang 21.0.0. One voice is held at the init patch (A1 one saw, A3) past its attacks, 512 samples per run at 48 kHz. Figures are the median of three runs (spread 1.0–5.3%), in ns per sample per voice. The full table is in [baselines.md](../performance/baselines.md#2026-11-23-m15-the-destinations-stage-1-prototypes).

| One voice (ns / sample) | |
|---|---:|
| `dsp::Voice`, no routes | 128.9 |
| the prototype, no routes | 131.4 |
| Volume A: 4 / 16 routes | 143.3 / 153.8 |
| Phase A: 4 / 16 routes | 138.1 / 146.1 |
| Pan A: 4 / 16 routes | 250.9 / 254.8 |
| Volume A, 4 routes, knobs (a) / (b) / (c) | 143.5 / 144.7 / 145.6 |
| 16 routes, every per-voice destination | 259.8 |
| the same, a change at every grid point: (a) step / (c) crossfade 10 ms | 268.5 / 282.8 |
| the same, no change, (c) crossfade 10 ms | 259.2 |
| Noise: no routes / Volume, 4 routes, (a) ignored / (b) mixed | 272.1 / 272.2 / 283.5 |
| 4 × 8 unison: no routes / Volume AB, 4 routes / Pan AB, 4 routes | 655.8 / 741.0 / 720.4 |

- **Volume and Phase cost little.** Volume adds 12–22 ns per voice over no routes, and Phase 7–15 ns.
- **Pan costs leaving the mono path**, about 120 ns, as task 2 found.
- **The knob sources cost the same within the noise.** (c)'s read of a global ramp per sample is the dearest.
- **A crossfade costs nothing while no change runs**, and +14.3 ns per voice (+5.3%) while one always runs, the worst case (old and new routes summed). Without one, the patch changes at a step cost +8.7 ns (recompiling).
- **The prototype with no routes is 2.5 ns over `dsp::Voice` (+1.9%).** That is the knob and presence ramps advanced on the unmodulated path. Stage 2 must not pay it (below).

## Recommendations

*Stage 1's recommendations, as written. Where only the demo can settle a question (M24–M27), the recommendation is the working hypothesis, and the measurement is what makes it the safer default. Chosen by the project owner on 2026-11-23, every one ([The decision](#the-decision)).*

| # | Question | Recommendation | Why | Alternatives | Settled by |
|---|---|---|---|---|---|
| 1 | **The knobs under modulation** | **(b) The slot's own 10 ms ramps in knob units**, started wherever its gain ramps start; the slot's **gain ramps move on while the modulated path renders** (`StereoGain::skip()`); **a presence ramp per unison voice** fades voices in and out on that path. The slot takes Volume and Pan in **knob units**, with the Volume law (linear, 10 = unity, O4) in `libs/dsp` beside `mixLaw`. | One source of truth: −139.5 dB from the unmodulated path at a switch, exact on leaving, exact on a voice that was idle while the knob moved. A knob turn while modulated is as smooth as without (−46.1 dB). No new knobs in `VoiceKnobs`. | (c) global ramps: smooth, but two truths that disagree on an idle voice (+8.7 dB). (a) targets: clicks at every knob turn (−20.8 dB). | (ours) |
| 2 | **A change of the routes** (amounts, destinations, sources) | **(c) A crossfade over 10 ms**: ⌈10 ms × fs / 32⌉ segments (15 at 48 kHz, 14 at 44.1, 30 at 96). At the grid point where the compiled routes change, each sum moves from the old routes' to the new routes' (per sample for Pitch, Mix, Volume and Phase, at each grid point for the others). The old routes' sources keep running until it ends, a further change is taken at its end, and each bound is the larger of the old routes' and the new routes'. **So M1.9 sends amounts as they are, with no ramp of its own.** | Clicks down by 12–37 dB on every destination (−48.3 on Mix and Volume, against −11.5). 7-bit amount automation's zipper from −31.7 to −77.5 dB. One mechanism for an amount, a destination, a source and a preset change. Costs nothing while nothing changes, +5.3% while a crossfade runs. | (a) step: free, clicks. (b) one segment: nothing on Pan or Cutoff, worse on Phase. An amount ramped by the parameter layer: doesn't cover a menu change. | M24 |
| 3 | **Volume and Pan on Noise** | **(b) mixed**: the mix at unit volume from the pan positions at the grid points, interpolated, times the volume per sample; two draws per sample on that path, so a skip draws what rendering would. | Within 0.01 dB of the knob's level and position. Ignored does nothing, the user's 2023 report, which the developer couldn't reproduce ("Works fine here"). | (a) ignored. | M25 |
| 4 | **The Volume skip test** | **Task 2's decision as measured**: a slot is silent over a segment where the Volume knob's larger end plus the routes' bound can't lift the clamp above 0. | Skips 50–80% of a lifted slot's grid points where never skipping skips none; within −67 dB of never skipping (the return's jump), exact at a note-on; −36% CPU on 16 voices of a pluck patch. A deliberately small bound is caught. | Never skipping (exact, slower). | (ours) |
| 5 | **Phase** | **As decided** (folded per sample, a band-limited jump where the step leaves [0, Nyquist)), plus what the decision left open: the onset of a note from silence at Phase plus the Phase sum at the note-on; a retrigger at Phase plus the offset in force; the offset back to 0 through the sum when the routes go; a skip carries the offset. | A folded step is within −29 to −41 dB of a band-limited jump; steps are rare at the source and, with recommendation 2, at a route's arrival. The four rules are exact in the default run (Phase at +2.5 is Phase 90° to the bit; a skipped slot comes back at the rendered one's phase). | A jump for every change above a threshold. | M26 |
| 6 | **The clamps** | **Volume and Pan clamped to [0, 10] before their laws, Phase wrapping, Pitch unclamped** (M9), as the registry has them. | The developer: v3 "will mute negative volume". Each clamp is the knob's own value to the bit in the default run; Pitch is finite at ±160 octaves. | (none) | M9, M27 |

### What the library needs (stage 2)

- **`OscillatorSlot`:**
  - **Volume and Pan in knob units**, with `volumeLaw` (a `constexpr` like `mixLaw`). Its own 10 ms knob ramps, started where the gain ramps start.
  - **`StereoGain::skip()`**, and a presence ramp per unison voice set by `setVoices()`.
  - **The modulated `process()` and `skip()`** taking the call's sums: Volume per sample, Pan's two grid-point sums, Phase per sample, and the call's position in the segment.
  - **Phase:** `setPhaseOffset()` for a note from silence.
  - **The skip test:** `isSilentUnder()` and `canSkipUnder()`.
  - **The Noise path.**
  - **The init patch at today's cost.** The knob and presence ramps can't be advanced on the unmodulated path. Advance them lazily instead: count the samples since the modulated path last ran and `skip()` them on entering it, an O(1) catch-up and exact. The prototype's +2.5 ns per voice must go.
- **`dsp::Voice`:**
  - Volume and Phase in the folded per-sample sums; Pan's before and now at each grid point.
  - The slots' note-on after the sources, so a note from silence knows its Phase sum.
  - A slot's offset returning to 0 when its routes go.
  - `allCentred()` false for a part with a Pan route or a Pan sum returning to 0.
  - The Volume bound in `partProduces()` and the slots' skip test. `mixModulationBound()` generalised to any target.
  - The crossfade of recommendation 2: the old routes kept with their sources until it ends, further changes deferred, sums blended per sample and at each grid point, bounds the larger. Its length from the sample rate.
- **Interop (M1.9):** amounts sent as they are, with no ramp; Volume and Pan parameters in knob units.
- **Tests, as task 3's stage 2 did:**
  - bit for bit this prototype in its decided configuration at host blocks of 1, 37 and 4096, with no allocation;
  - the init patch bit-identical, and no routes costing nothing measurable;
  - each destination in its own units and each clamp, to the bit against the knob;
  - the skip test against never skipping, with a small bound caught;
  - the crossfade's bound against its sums.

### What this pushes back on

- **Task 3's open question** (PLAN, modulation-sources.md): whether a route's amount ramps or steps. Recommendation 2 answers it in the voice, for menu changes too. So M1.9 needs no ramp on the amounts.
- **Task 2's prototype slot ignored Noise** for Volume and Pan. It only tested its rates on a sine.
- **Task 2's "What the library needs"** had the modulated slot read its knobs as global ramps, as Mix does. Measured, that disagrees with the slot's own gain ramps on a voice that was idle while the knob moved. The slot's own ramps don't.
- **`StereoGain`** gains `skip()`. A gain ramp frozen while another path renders comes back where it stopped.
- **The modulated Pan path and `StereoGain` differ while the Pan knob ramps** (the chord against the law, −20.4 dB at a switch then). This is accepted, since it needs a route change within 10 ms of a Pan knob move.
- **The registry** is unchanged. Its rows already carry every rate, unit and range the prototype reads.
- **The inventory:** M8 and M9 annotated, and M24–M27 new.

## The decision

Chosen by the project owner on 2026-11-23: every recommendation (Decisions log). Stage 2 builds them into `OscillatorSlot` and `dsp::Voice`, bit for bit the stage-1 prototype in its decided configuration at host blocks of 1, 37 and 4096, with no allocation.

- **The knobs under modulation:** the slot's own 10 ms ramps in knob units, started where its gain ramps start. Its gain ramps move on while the modulated path renders (`StereoGain::skip()`), and a presence ramp per unison voice fades voices in and out there. The slot takes Volume and Pan in knob units, with the Volume law (linear, 10 = unity) in `libs/dsp` as `volumeLaw`. The new ramps are advanced lazily, so the init patch costs what it did.
- **A change of the routes:** a crossfade of the sums over ⌈10 ms × fs / 32⌉ segments, from the grid point where the compiled routes change. It is per sample for Pitch, Mix, Volume and Phase, and at each grid point for the others. The old routes' sources run until it ends, a further change is taken at its end, and each bound is the larger of the old routes' and the new routes'. M1.9 sends amounts as they are.
- **Volume and Pan on Noise:** the mix at unit volume from the pan positions at the grid points, interpolated, times the volume per sample, with two draws per sample on that path.
- **The Volume skip test:** a slot is silent over a segment where the Volume knob's larger end plus the routes' bound can't lift the clamp above 0.
- **Phase:** folded per sample, with a band-limited jump where the step leaves [0, Nyquist). A note from silence starts at Phase plus the Phase sum at the note-on, and a retrigger lands at Phase plus the offset in force. The offset returns to 0 through the sum when the routes go, and a skip carries it.
- **The clamps:** Volume and Pan clamped to [0, 10] before their laws, Phase wrapping, Pitch unclamped.

**The inventory (M24–M27):** M25–M27 are now our working hypotheses, built in stage 2 and checked against the demo in M1.13. M24's crossfade is our own choice, made for clicks, which the demo can only show to differ from Sylenth1.


## As built

Stage 2 (2026-11-23) built the decision into `libs/dsp`, test-first. It is bit for bit the stage-1 prototype in its decided configuration (`DestVoice` with `decidedAt()`: the slot's own knob ramps, the crossfade over ⌈10 ms × fs / 32⌉ segments, Noise mixed, the Volume bound), except for two flaws stage 2 found. Both break the invariant that the output doesn't depend on the host's blocks, so they are fixed in the prototype too, and the stage-1 report reproduces unchanged.

- **[volume_law.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/volume_law.hpp): `volumeLaw`, `volumeUnits()`, `volumeGain()`.** This is Volume's law beside `mixLaw`: the knob plus its modulation clamped to [0, 10] (NaN is silence), then linear, with 10 as unity.
- **[slot_modulation.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/slot_modulation.hpp): `SlotModulation`,** what a voice tells a slot for one call: Volume's sum per sample, Pan's two grid-point sums, the call's position in the segment, and Phase's sum per sample.
- **[stereo_gain.hpp](../../libs/dsp/include/anoesis/dsp/control/stereo_gain.hpp): `StereoGain::skip()`.** It advances both gain ramps without rendering, exactly as `process()` would.
- **[oscillator_slot.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/oscillator_slot.hpp): the modulated path.** `process()` and `skip()` overloads take a `SlotModulation`:
  - **Knob ramps.** Its own Volume and Pan ramps, in knob units (10 ms, started where the gain ramps start).
  - **Volume** per sample through `volumeGain()`, signed by Inv.
  - **Pan's gains** at the segment's two grid points, interpolated.
  - **Presence:** a ramp per unison voice for `setVoices()`'s fades.
  - **Phase:** folded per sample, with a band-limited jump where the step would leave [0, Nyquist). A retrigger lands at Phase plus the offset in force, and a note from silence at Phase plus `setPhaseOffset()`'s value; `skip()` carries the offset, and `trackPhase()` follows it while no voice sounds.
  - **Noise:** the mix at unit volume from the pan positions at the grid points, interpolated, times the volume per sample.
  - **The skip test:** `isSilentUnder()` and `canSkipUnder()`.
  - **Lazy ramps.** The knob and presence ramps are advanced **lazily**: the plain path only counts its samples (`lagging`), and `catchUp()` skips them in closed form, before anything reads them or sets them. The setters keep their arguments (`setVolume(gain)`, `setPan(position)`), and the ramps hold gain × 10 and (position + 1) × 5, which is knob units under the linear law. The test files call these setters about twenty times, and through them the slot is the prototype to the bit.
- **[voice.hpp](../../libs/dsp/include/anoesis/dsp/voice/voice.hpp): the destinations in `dsp::Voice`:**
  - **Sums.** Volume and Phase join Pitch and Mix in the folded per-sample sums. Pan's sums are kept at the previous and the current grid point.
  - **Note-on.** The slots' note-on now comes after the sources', so a note from silence knows its Phase sum.
  - **The crossfade.** It runs on any change of the routes at a grid point (`routeCrossfadeSegments()` in [route_compiler.hpp](../../libs/dsp/include/anoesis/dsp/modulation/route_compiler.hpp), its length set at `prepare()`). The old routes and their sources are kept until it ends, a further change waits for its end, sums are blended per sample and at each grid point, and each bound is the larger of the old routes' and the new ones' (`modulationBound()`, which generalises `mixModulationBound()`).
  - **The skip test and the mono path.** The skip test reads each part's Volume bound, and a Pan route takes its part off the mono path.
  - **A fast path.** A part with no Volume, Pan or Phase modulation calls the slots' plain methods (below).

### What differs from the prototype, and why

- **Found: a silent slot's Phase offset depended on the host's blocks.** A slot with no voice sounding isn't called, so its offset in force stopped where its last call ended. A call runs on to the end of its chunk, so with host blocks of 4096 that was up to 31 samples later than with blocks of 1. When unison brought voices back, they started at Phase plus that stale offset, so the output after it differed between host block sizes. The random scenario at 44.1 kHz found it, with B2 going to 0 voices and back under Phase routes. **The fix:** the voice tells such a slot its Phase sum each chunk (`trackPhase()`), so the offset is always the sum at the previous sample.
- **Found: Noise on its modulated path drew past the point where its voices went silent.** It drew two numbers every sample it was called. Going to 0 voices, it is called until its gain ramps finish, and a long call runs on past that sample, so every later noise sample shifted with the host's blocks. The first fix tried, copying the plain path's rule ("draw while the mix isn't settled at zero"), silenced a slot at Volume 0 that a route lifts, because that mix includes the Volume knob. **The fix:** draw while any voice's presence is above 0, checked per sample, and a modulated skip draws by the same rule.
- **Left out: the prototype's return of the Phase offset to 0 when its routes go.** Under the decided crossfade, a route that goes fades its sum to exactly 0 on the last sample, so the offset is 0 with it. The separate path was needed only when changes stepped, and no test could reach it. A test pins the return itself: once the crossfade and the oscillator's band-limiting residuals are past, the note is the one that never had the route, to the bit.
- **Added: a fast path for a part with no Volume, Pan or Phase modulation.** Building each slot's `SlotModulation` and taking the modulated tests on every chunk cost the init patch about 2.5 ns per voice (paired against the build before this stage: median +2.5, six of seven pairs above). Such a part's `SlotModulation` would be empty, which is the plain path to the bit, so it now calls the plain methods. Against the build before this stage, over 14 interleaved pairs, the medians are 130.1 and 130.3 ns: no routes costs nothing measurable.
- **Only the decided configuration:** no knob-source, amount, Noise or skip switches, and no skip counters.

### Tests (stage 2)

Under `[modulation]` in the default run, there are 11 test cases in [voice_destinations_test.cpp](../../tests/dsp/modulation/voice_destinations_test.cpp). They drive `LibraryDestEngine`, stage 1's engine over `dsp::Voice` ([destination_support.hpp](../../tests/dsp/modulation/destination_support.hpp)), with the same events as the prototype, plus a unison-count event.

| Test | What it pins down | Tolerance and why |
|---|---|---|
| The prototype as decided | `dsp::Voice` against `DestVoice` with `decidedAt()`, through the real `VoiceAllocator`, at host blocks of 1, 37 and 4096: stage 1's busy patch with unison counts changing mid-note; a Pan move on a slot skipped at Volume 0 and a Volume route lifting it 1 ms later; random scenarios at 44.1 and 96 kHz (Volume, Pan, Phase, Pitch, Mix and Cutoff routes appearing and changing mid-note, knob and unison changes, Noise) | bit for bit; 0 allocations |
| Phase through unison's return | the random scenario that found the first flaw (B2 a saw) at host blocks of 1 and 37 against 4096 | bit for bit |
| A Phase route that goes | after the crossfade and the band-limiting residuals, the note that never had the route | bit for bit |
| No such route, no change mid-note | the init patch and task 3's busy patch against the prototype stepping (stage 1 pinned it to `dsp::Voice` before this stage) | bit for bit |
| Units and clamps | the stage-1 cases on the library: Volume and Pan against the knob, both clamps, Phase at +2.5, +10 and −2.5 | bit for bit |
| The crossfade | 14, 15 and 30 segments at 44.1, 48 and 96 kHz; a new route on Mix at −10 brings exact silence on the crossfade's last sample, not before | exact |
| Volume 0 held and lifted | the note stays busy and is heard once a route lifts it | exact |
| Noise | Volume and Pan routes move a 4-voice Noise slot to the knob's level and position | 0.05 dB (two noise draws) |
| Phase through a skip | a skipped slot's offset and every unison voice's phase equal a rendered one's, and the offset isn't 0 | exact |
| `volumeLaw` | unity, the clamps, NaN | exact (`WithinULP(…, 0)`) |
| `StereoGain::skip()` | the gains where processing leaves them, at 1, 37, 100 and 512 samples | exact |

**Red first.** Against inert stubs (the overloads ignoring their modulation, `skip()` doing nothing, the crossfade one segment long), 6 of the first 9 cases failed. Three passed:
- the init patch, as it must;
- `volumeLaw`, written in final form;
- Phase through a skip, which passed for the wrong reason: the stub's offset read 0 on both slots. It now also checks that the rendered offset isn't 0, which the stub fails.

**Task 3's tests.** Two of its comparisons with its own prototype now differ where the decision says they should, and assert that: the scenario whose routes change mid-note (now crossfaded), and the one with Volume, Pan and Phase routes (now acting). Its "running only what is read" case for a free LFO routed mid-note now differs from running everything, because the route crossfades in. "A free LFO's copy" still pins the copy itself.

**Deliberate breaks.** 17, each built and run on its own in a runner of just these tests. All 17 were caught, none only at compile time.

| Break | Caught by |
|---|---|
| `StereoGain::skip()` advances nothing | `StereoGain::skip()`, the prototype |
| The crossfade 5 ms long | the crossfade, the prototype |
| A change of the routes steps | the crossfade, a Phase route that goes, the prototype |
| The crossfade's bound from the new routes only | the crossfade, the prototype |
| A silent slot's Phase offset not tracked | Phase through unison's return |
| The fast path taken by a part with Volume, Pan or Phase routes | Noise, a Phase route that goes, Volume 0 lifted, units and clamps, the prototype |
| A skip's samples not counted for the lazy ramps | the prototype (the Pan move on a skipped slot) |
| Modulated Noise draws whether or not a voice sounds | the prototype |
| Volume not clamped below 0 (v2's inversion) | `volumeLaw`, the prototype |
| Pan not clamped | the prototype |
| A note from silence ignores the Phase sum | units and clamps, the prototype |
| The modulated path's ramps not caught up before a setter | the prototype |
| Presence ignored on the modulated path | Phase through unison's return, the prototype |
| A Pan route keeps its part on the mono path | units and clamps |
| The Volume bound taken as 0 | Noise, Volume 0 lifted, units and clamps, the prototype |
| Pan held at the grid point | the prototype |
| The crossfade's product fused into its sum | the prototype |

### CPU (stage 2)

`tools/bench.sh "[voice-destinations]"` ([destination_benchmark.cpp](../../tests/dsp/benchmarks/destination_benchmark.cpp)), `relwithdebinfo`, Apple M1 Ultra (virtualised), Apple Clang 21.0.0. Stage 1's held voice and routes. Figures are the median of three runs (spread up to 8.6%; one prototype run of 769.9 ns came from a host stall), in ns per sample. The prototype in its decided configuration ran in the same runs. Full table: [baselines.md](../performance/baselines.md#2026-11-23-m15-the-destinations-as-built).

| One voice (ns / sample) | `dsp::Voice` | the prototype (decided) | stage 1 |
|---|---:|---:|---:|
| no routes | 131.7 | 130.0 | 131.4 |
| Volume A, 4 routes | 145.6 | 144.9 | 143.3 |
| Pan A, 4 routes | 252.2 | 252.8 | 250.9 |
| Phase A, 4 routes | 140.8 | 142.5 | 138.1 |
| 16 routes, every destination | 259.7 | 263.1 | 259.8 |
| 16 routes, a change at every grid point (always crossfading) | 286.7 | 283.2 | 282.8 |

- **No routes costs nothing against the voice before this stage.** The commit before it (c35c5f1) was built in a separate worktree and run interleaved with this build, 14 times each: 130.3 against 130.1 ns (medians). The paired differences run from −9.4 to +8.1 ns, median +1.3, 8 of 14 above zero: within the noise.
- **Routes cost what stage 1 measured**, within the noise, and the library runs level with the prototype.

## Tests

Stage 2's tests are above ([Tests (stage 2)](#tests-stage-2)): 11 cases under `[modulation]`, with `dsp::Voice` the prototype to the bit. Stage 1's prototypes are checked in the default run under `[destinations-model]` ([destination_candidates_test.cpp](../../tests/dsp/modulation/destination_candidates_test.cpp), 6 test cases, about 12 s in debug):

| Test | What it pins down | Tolerance and why |
|---|---|---|
| No Volume, Pan or Phase route | `DestVoice` against `dsp::Voice` (task 3's `LibraryEngine`) on the init patch and task 3's busy patch, at host blocks of 1, 37 and 4096, under three candidate sets | bit for bit: the prototype is a copy where the library is built |
| Any host block size | a busy patch on the new destinations (Volume, Pan and Phase from moving sources, Noise, unison, knob and patch changes mid-note, steals) under 9 candidate sets covering every value of every switch, at host blocks of 1 and 37 against 4096 | bit for bit; 0 allocations |
| Units and clamps | with a source of 1: Volume 2 + 3 is Volume 5, 0 + 7 is 7, 8 + 5 is 10, Volume B leaves A alone, 3 − 8 is silence; Pan 5 + 2.5 is 7.5, 3 + 10 is 10, 3 − 10 is 0; Phase +2.5 is Phase 90°, +10 is no change, −2.5 is 270°; Pitch at ±160 octaves finite | bit for bit against the knob; silence exact |
| Volume 0 lifted | a note held at Volume 0 stays busy until a route lifts it at 9000, then sounds; at −5 it stays silent | exact silence; busy at every block |
| The small bound | a pulse LFO on Volume A at −10 from Volume 5: the routes' bound within 10⁻⁶ of never skipping (energy ratio), a bound from the grid point's values more than 10⁻³ off | the bound's skip differs only by the return's band-limited residual |
| Phase through a skip | a slot skipped for 30 segments under moving Phase modulation, then rendered: its offset and every unison voice's phase equal a slot rendered throughout | exact (uint32) |

The measurements are the hidden `[destinations-report]`, and the CPU numbers are `[!benchmark][destinations]` and [baselines.md](../performance/baselines.md#2026-11-23-m15-the-destinations-stage-1-prototypes).

## References

- LennarDigital forum, "Some of my older patches no longer sound" (April–May 2026): Lennard on v2's inversion and v3's mute at negative volume, an LFO at Offset −10 on AB Volume, and the v3.073 noise-oscillator bug fixed in v3.075. [p=9234315](https://www.kvraudio.com/forum/viewtopic.php?p=9234315), [p=9239260](https://www.kvraudio.com/forum/viewtopic.php?p=9239260) (the thread).
- LennarDigital forum, "Sylenth- 2 issues" (August 2023): a user's report that Pan modulation does nothing on a Noise oscillator, and the developer's "Works fine here". [p=8720643](https://www.kvraudio.com/forum/viewtopic.php?p=8720643), [p=8720854](https://kvraudio.com/forum/viewtopic.php?p=8720854).
- Attack Magazine, a Sylenth1 tutorial using an LFO on Phase A with an inverted, phase-shifted second saw ([attackmagazine.com/?p=26440](https://www.attackmagazine.com/?p=26440)); a KVR thread on the trick's dependence on Retrig ([p=6232409](https://www.kvraudio.com/forum/viewtopic.php?p=6232409)).
- [modulation.md](modulation.md), [modulation-sources.md](modulation-sources.md), [oscillator-controls.md](oscillator-controls.md), [oscillator-unison.md](oscillator-unison.md), [part-solo-and-skipping.md](part-solo-and-skipping.md), [filter-control.md](filter-control.md).
- V. Välimäki and A. Huovilainen, "Antialiasing Oscillators in Subtractive Synthesis", IEEE Signal Processing Magazine 24(2), 2007: band-limited steps and slope changes, the basis of the min-BLEP oscillator's pitch step and phase jump ([band-limited-oscillators.md](band-limited-oscillators.md)).
