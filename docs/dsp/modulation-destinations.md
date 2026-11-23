# Modulation destinations

A destination is the control a route moves. [modulation.md](modulation.md) decided each per-voice destination's rate and how sums are formed, and [modulation-sources.md](modulation-sources.md) built the sources and wired Pitch, Mix, Cutoff, Reso and the LFOs' Rate and Gain into `dsp::Voice`. This page is M1.5 task 4. It covers the destinations left: **Volume, Pan and Phase**, each through its units, its decided rate and its clamp. It also covers what the rest of the voice needs around them: the slot's knobs while its gains are computed, what a change of the routes does, Noise, and the skip test.

- **Theory:** adding before a knob's law and clamping before it; Phase's wrap and an unclamped Pitch; a gain computed outside its smoother, and keeping two paths in step; Pan's law on the grid; stepping or crossfading a change of the routes; why a Noise slot needs its own path; proving a modulated slot silent.
- **Sylenth1:** what is known, from task 1 and two developer threads found for this task (inventory §6, [The destinations in detail](../reference/sylenth1-features.md#the-destinations-in-detail), M24–M27).
- **Stage 1:** the candidates, prototyped on the library's own classes and measured; the recommendations; what stage 2 needs, and what this pushes back on.

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

*Stage 1's recommendations. Where only the demo can settle a question (M24–M27), the recommendation is the working hypothesis, and the measurement is what makes it the safer default.*

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

## Tests

Stage 1's prototypes are checked in the default run under `[destinations-model]` ([destination_candidates_test.cpp](../../tests/dsp/modulation/destination_candidates_test.cpp), 6 test cases, about 12 s in debug):

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
