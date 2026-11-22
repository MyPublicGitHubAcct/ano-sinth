# Modulation: sources, destinations, and amounts

Modulation moves a control over time without anyone turning its knob. A source (an envelope, an LFO, velocity, the mod wheel) produces a value, and a route adds that value, scaled by an amount, to a destination such as pitch, cutoff or volume. This page covers M1.5:

- **Theory:** fixed slots against a matrix; additive modulation in knob units, and what that means for each kind of knob; scaling a source; polyphonic and monophonic sources and destinations, and the four ways they meet; control rate against audio rate; order and feedback when sources are also destinations; and bounds for skipping.
- **Sylenth1** (task 1): what is known about its sources, destinations and amounts, and what that means for the build.
- **The model** (task 2): the candidates for the registries, the routing, each destination's rate, the order through cycles, a per-voice source on a global destination, and each source's bound; what they measured; the recommendations; and the project owner's decision (every recommendation, 2026-11-22).

Task 2's stage 2 and tasks 3–5 will add the as-built sections.

Read [lfos.md](lfos.md) and [envelopes.md](envelopes.md) (the sources), [keytracking.md](keytracking.md) (the keytrack source), [filter-control.md](filter-control.md) (the first destination built with modulation inputs, and the clamp), [mixer.md](mixer.md) (Mix, a destination applied per sample), and [part-solo-and-skipping.md](part-solo-and-skipping.md) (why each source must bound its value) first.

## Theory

### Fixed slots or a matrix

A modulation system says which sources can reach which destinations, and how much. There are two common shapes:

- **A matrix:** a list of N rows, each choosing a source, a destination and an amount (sometimes a second source that scales the amount). Any source can reach any destination, and the list's length is the only limit.
- **Fixed slots:** each source owns a few routes. Sylenth1 gives each of its 8 sources (2 mod envelopes, 2 LFOs, 4 selectable misc sources) exactly 2, so 16 in all. The panel shows a source and its routes together, which makes a patch easy to read. The cost is flexibility: a source can't reach a third destination, and a destination's own amount can't be modulated.

Underneath, the two shapes compute the same thing. Each route contributes amount × source to its destination, and contributions to one destination add. Fixed slots are a matrix whose rows have a fixed source. So one engine can serve both. The fixed slots become 16 rows, the 4 misc rows have a selectable source, and Phase 2 can lengthen the list or free the sources without touching the evaluation.

### Additive modulation in knob units

The simplest rule, and Sylenth1's (the developer: "all modulation in Sylenth1 is additive… you add it to the knob value"), is:

> value = knob + Σ amount × source, in the knob's own units, then the knob's law.

What one unit of modulation does then depends on the knob's law:

- **A knob that is linear in its physical quantity** (a volume read as a linear gain, a pan position) gives amplitude modulation or panning that is linear in the source.
- **A knob that is linear in octaves** (pitch, a cutoff whose knob is an octave per unit, as ours is in [filter-control.md](filter-control.md)) gives exponential modulation. A sine on pitch is a vibrato that is symmetric in cents, not in Hz. An envelope on cutoff moves the cutoff by equal musical intervals.
- **A knob with a curved law** (an LFO rate that is exponential in its knob, a drive in dB) makes the same amount mean a ratio, not a step, in the physical quantity.

Because the units are the knob's, an amount means something different at each destination. One unit is an octave on pitch and a tenth of the range on Volume. So each destination must carry its own scale, and the parameter table and the modulation engine must agree on it.

**Clamping** matters because the sum can leave the knob's range. Sixteen routes at ±10 can push a 0–10 knob to ±160. There are three places a clamp can sit:

1. **On the sum, before the knob's law.** Modulation can't push past what the knob itself reaches. This is our rule (inventory F24, built for the filters' Cutoff and Resonance by `clampToKnob`).
2. **After the law, on the physical value.** For example, a cutoff limited only by Nyquist.
3. **Nowhere.** The destination's DSP has to survive anything.

A clamp before the law also changes a modulation's shape. A sine pushed past the top flattens, which a listener hears as the modulation "sticking". Phase is the exception: it wraps, since 370° is 10°.

### Scaling a source

Most sources have a natural range: an envelope runs from 0 to 1, a MIDI controller from 0 to 127. A route's amount scales that range. An LFO adds its own depth control (Gain) and a DC shift (Offset) before the route, so its output is gain × (wave + offset). Gain and amount then multiply, and either one can set the depth. Gain matters because it is shared by both of the LFO's routes, and because Gain itself can be a destination, which gives amplitude-modulated modulation.

A source is **unipolar** (0 to 1: an envelope, velocity, the mod wheel) or **bipolar** (−1 to 1: an LFO at Offset 0). A positive amount on a unipolar source only raises a destination. To move it both ways, the knob has to sit in the middle. Offset turns a bipolar LFO into a unipolar one. A negative amount inverts any source.

### Polyphonic and monophonic

A source is **per voice** if each note has its own value. Velocity, keytrack, envelopes, and an LFO that restarts at each note are per voice. A source is **global** if there is one value for the instrument: the mod wheel, channel aftertouch, a free-running LFO, the arpeggiator's step velocity. Destinations split the same way. The oscillators, filters and per-voice gains are per voice. The master effects are global. The four combinations:

| Source → destination | What it means | How it is computed |
|---|---|---|
| Per voice → per voice | Each note moves its own sound (a filter envelope). | In the voice, per voice. |
| Global → per voice | Every note moves together (the wheel's vibrato). | Once, read by each voice. Ours reads global ramps at absolute sample times, as `PitchBend` is, so each voice reads the same value at the same sample, independent of how the block is split. |
| Global → global | One value moves one effect. | Once. |
| Per voice → global | Many values, one destination: it needs a rule. | One of: the **newest** voice's value (it jumps at each note-on and follows the last note); the **oldest** held voice's; a **reduction** over voices (the sum grows with the chord, the maximum and the mean don't); or a **single global copy** of the source (an envelope triggered by the first note-on of a phrase, the last velocity). |

The last row has no free answer. The newest voice is what a mono synth does, but in a chord the effect jumps at each new note. A sum makes a chord distort harder than a single note. A global copy needs its own retrigger rules. This is one reason a synth may offer only global sources on its effects.

### Control rate and audio rate

A destination can take its value at every sample or on a control grid of N samples:

- **Per sample** is exact and needs no smoothing. It costs a multiply-add per route per sample per voice, plus whatever the destination does with a new value. That second part is the real cost: a filter computes tan() for its coefficient, and a pitch change has to keep a band-limited oscillator band-limited.
- **On a grid, held,** costs 1/N as much, but the staircase it makes is a zipper: sidebands at multiples of fs/N around every partial, from a step every N samples.
- **On a grid, ramped** (linear between grid points) removes the staircase. It is exact for straight lines, and it trails a corner or a fast curve by up to one segment.

Which rate suits a destination depends on how its error is heard. Mix moves an audio-rate gain, so a grid zips at once (−13 dB added on a 40 Hz LFO, [mixer.md](mixer.md)), while per sample cost nothing measurable. Our filters already take their cutoff and resonance at 32-sample grid points ([filter-control.md](filter-control.md)). The pitch is already per sample while it moves ([legato-portamento.md](legato-portamento.md)). Every grid here is counted from the voice's note-on, so the output never depends on the host's block size.

### Order and feedback

When a source is also a destination (an LFO's rate or gain), routes can form a cycle: LFO 1 → LFO 2 Rate while LFO 2 → LFO 1 Gain. A cycle has no order in which every value is computed from current inputs, so one link has to read a value from the past. There are two choices:

- **A fixed evaluation order,** in which sources that are also destinations are updated first and read their own modulation from the previous control period. The delay is one grid segment.
- **A one-sample delay** on every route into a source. This works per sample, and costs that.

Either is fine if it is deterministic and independent of the block size. A self-route (LFO 1 → LFO 1 Rate) is the smallest cycle.

### Bounds for skipping

Our voice skips work it can prove silent ([part-solo-and-skipping.md](part-solo-and-skipping.md)). A route into a factor that the proof reads (Mix, an oscillator's Volume) could make a "silent" part audible. So every source must give an upper bound on its value over a stretch of samples, and every such route adds amount × bound to the proof. A bound that is too large only costs CPU. A bound that is too small skips audio, which is a bug.

## Sylenth1: what is known

The full source table with ratings is in the inventory: [§6, Sources, destinations, and amounts](../reference/sylenth1-features.md#sources-destinations-and-amounts) (M1–M13), with AL8, X7 and L1 annotated. The complete lists come from one third-party file: a template for reading Sylenth1's v2.x preset files. Every other source agrees with it. We take only the names the menus show from it. Our IDs and preset format are our own (D7), and they don't follow its numbering. In short:

- **The slots.** 2 mod envelopes, 2 LFOs and 4 misc slots with a selectable source, each with 2 routes, for 16 in all (manual pp. 19–20). Each route has an amount knob from **−10 to +10**, 0 at the centre. A template gives the range, and the developer's "−10 for example" confirms it. Tutorials show it with two decimals ("4.00").
- **The rule.** Modulation adds amount × source to the knob's value, in the knob's units (the developer, 2016). Mix uses its internal 0–15 scale. Two routes to one destination add.
- **Pitch's scale.** "At 1.0 it'll pitch up 1 octave and at -1.0 it'll pitch down 1 octave", said of a mod envelope (the developer, 2010). So a full amount on an envelope is 10 octaves.
- **The LFO's scale, a factor of 2.** The developer's LFO numbers ("amount… exactly 1.0… Gain is 10… exactly 1 octave", and at Gain 3.33 "4 notes in total: 2 notes up and 2 notes down") make a Sine at Gain 10 a source of **±0.5**, not ±1. Our `Lfo` outputs gain × (wave + offset), which is ±1 there and up to ±2 with Offset. So either the LFO is halved where it becomes a source, or pitch has a different scale for LFOs than for envelopes. The first is simpler, and it makes an LFO at Offset +10 span 0–1, exactly an envelope's range. That is the working hypothesis (M6). It changes the bound the PLAN gives for an LFO ("up to ±2") to ±1.
- **The destinations** (27, and "None"):

  | Group | Entries | Scope | Units, hypothesis (M8) | Where it lands in ours |
  |---|---|---|---|---|
  | Oscillators | Volume A, B, AB | per voice | Volume knob, 0–10 (the developer) | `OscillatorSlot`'s gain. The skip test's `isSilent()` must add each route's bound (PLAN M1.5 task 4). |
  | | Pitch A, B, AB | per voice | octaves (the developer) | the voice's pitch input, per sample while it moves |
  | | Phase A, B, AB | per voice | Phase knob, 10 for 360° | a new phase-offset input on the slot. Today Phase is only a start phase, but modulating it moves a running oscillator (the manual's "PWM-like effects", p. 15). A moving offset is phase modulation, and a band-limited oscillator must jump or bend for it without aliasing. |
  | | Pan A, B, AB | per voice | Pan knob, 0–10 | the slot's pan |
  | Filters | Cutoff A, B, AB | per voice | Cutoff knob (an octave per unit in ours) | the Filter Control's modulation term, built and 0 today |
  | | Reso A, B, AB | per voice | Resonance knob | likewise |
  | Misc | PhsrFreq | **global** | the phaser's CenterFreq knob | M1.7 |
  | | Mix A, B, AB | per voice | Mix, internal 0–15 (the developer) | `mixGain` per sample, `mixModulationBound()` |
  | | LFO1Rate, LFO1Gain, LFO2Rate, LFO2Gain | per voice (each voice has its own LFOs, L8) | the knobs' units | `Lfo::setFrequency`, its gain |
  | | DistAmnt | **global** | the distortion's Amount knob | M1.7 |

  "A", "B" and "AB" are probably part A's two oscillators (or filter A), part B's, and all of them (M3). The demo's popups confirm the list (M1, settled 2026-11-22): the 27 in the template's order, in three columns (Oscillators, Filters, Misc), labelled "Volume A and B" rather than "AB", and the same in every slot. KVR's 24 doesn't match. The destinations do not include envelope stages (AL8), Main Volume (X7), Detune, Octave or Fine, Drive, Keytrack, any amount, or any effect parameter other than the two above. Users asked for envelope stages and more targets between 2007 and 2021, and no changelog added any after v1.20.
- **The misc sources** (11, and "None"): Velocity, ModWheel, KeyTrack, AmpEnv A, AmpEnv B, ModEnv 1, ModEnv 2, LFO 1, LFO 2, Aftertouch, Step Vlcty. Aftertouch came in v1.1 and Step Vlcty in v1.20. Nothing suggests polyphonic aftertouch or a random value per note. Their scopes and ranges:

  | Source | Scope | Range, hypothesis (M7) | In ours |
  |---|---|---|---|
  | Velocity | per voice | v / 127 | `Voice::velocity()`, ramped over a glide (K3) |
  | ModWheel | global | CC 1 / 127 | a global ramp, read like `PitchBend` |
  | KeyTrack | per voice | (pitch − 9) / 120, unclamped | `keytrackSource` (F20) |
  | AmpEnv A, AmpEnv B | per voice | 0–1, each part's own envelope (AL9) | the voice's two `Adsr`s |
  | ModEnv 1, ModEnv 2 | per voice | 0–1 | two new `Adsr`s per voice |
  | LFO 1, LFO 2 | per voice; a free one copies a global one at the note-on (L8) | ±0.5 at Gain 10 and Offset 0 (M6) | `Lfo`, halved |
  | Aftertouch | global (channel pressure) | / 127 | a global ramp |
  | Step Vlcty | global (the arpeggiator's) | the step's velocity, 0–1, and its "hold" (M1.6) | M1.6 |

- **Per voice and global.** The manual says pitch and phase modulation are evaluated per voice (p. 30). Only two destinations are global, PhsrFreq and DistAmnt, and nothing says what a per-voice source does there (M10). The developer's own wah uses the phaser's internal LFO.
- **What is open.** What A, B and AB select (M3), the amount's display and law (M4, M5), the LFO's scale (M6), each source's range and each destination's units (M7, M8), where each destination clamps (M9), per-voice sources on global destinations (M10), cycles (M11), each destination's update rate (M12), and 7-bit controller steps (M13). The menus (M1, M2) are settled: the project owner's screenshots of the demo's popups (2026-11-22) show the template's entries in the template's order. The source menu (M2): "-", Velocity, Mod Wheel, KeyTrack, Amp Env A, Amp Env B, Mod Env 1, Mod Env 2, LFO 1, LFO 2, Aftertouch, Step Velocity.

### What this means for the build

- **Task 2's registries** need rows for 27 destinations and 11 sources. Each destination row carries a group, a scope, a unit per amount, and a knob range for the clamp. Each source row carries a scope, a range, and a bound. Rows are data, so M1's answer can change the list without changing code. The persisted IDs are ours and are separate from the menu order.
- **Two destinations are global** (PhsrFreq, DistAmnt), and **three of the misc sources are global** (ModWheel, Aftertouch, Step Vlcty). Task 2 must give the per-voice-to-global rule a recommendation even though Sylenth1's is unknown (M10).
- **Some sources are also destinations**: the four LFO controls. That is the only possible cycle (M11), since no envelope or amount is a destination.
- **Phase as a destination** is new work for the oscillator slot (a running phase offset, band-limited), and the most expensive per-voice destination, as the manual warns.
- **The LFO's factor of 2** must be settled before task 3 builds the LFO source (M6). Until the demo answers, halving at the source matches every number the developer gave.

## The candidates

Task 2's stage 1 prototypes the model in `tests/`, on the library's own classes. The model has six parts:

| Question | Candidates |
|---|---|
| **1. The registries** ([registry_candidates.hpp](../../tests/dsp/modulation/registry_candidates.hpp)) | (a) `constexpr` descriptor tables indexed by an enum; (b) descriptors with a function per entry; (c) a compile-time type list, one type per destination and source |
| **2. Routing** | the 16 slots compiled into a list of live routes whenever the patch changes, summed per target and part (one candidate, measured for exactness and cost) |
| **3. The rate per destination** (Pitch, Phase, Volume, Pan, LFO Rate, LFO Gain) | per sample; at the voice's 32-sample grid points, held; at the grid points, ramped linearly from the previous grid point's value to this one's (one segment late, since the sources can't be read ahead) |
| **4. Order and feedback** | segment order: each LFO's rate and gain set at its grid point from the sources as they stand there, its wave rendered for the whole segment ahead (a delay of up to one segment through a cycle); sample order: the LFOs run sample by sample, each sample's rate and gain from the sources at the sample before (a delay of one sample) |
| **5. A per-voice source on a global destination** | the newest voice's value; the oldest held voice's; the sum over voices; a global copy of the source (an envelope gated by the first key down and the last key up, the last note-on's velocity) |
| **6. Each source's bound over a segment** ([source_bounds.hpp](../../tests/dsp/modulation/source_bounds.hpp)) | an envelope: its range [0, 1], PLAN's rule (0 when idle or held at 0, else [0, 1]), or by stage (between its level and where the stage heads); an LFO: its range (±1.125), the wave's ends and crests over the phases the segment covers (PLAN's rule, which ignores slews), or exact, from the segment rendered ahead; a linear source (velocity, keytrack, a global ramp): its two ends |

**The descriptors** ([modulation_model.hpp](../../tests/dsp/modulation/modulation_model.hpp)). One row per destination, "None" and the 27 of task 1. Each row holds:

- a **key**, the persisted ID (`"osc.pitch.ab"`). It is a string, so it can't be the template's numbering, and it is separate from both the internal order and the menu's (M1);
- the label and the group;
- the scope and the evaluation rate;
- the **target** and the **parts** it acts on (Volume AB is the volume target on both parts);
- the units per unit of amount (a knob unit, or 12 semitones for Pitch);
- the knob range for F24's clamp, a wrap for Phase, or none for Pitch.

One row per source, "None" and the 11 of task 1, each with a key, a label, a scope, a range and a **bound kind** (none, linear, envelope, LFO). `static_assert`s check every row: unique keys in a fixed alphabet, ranges in order, a global destination neither per sample nor on a part. Each candidate adds its own checks. In (a), the enum agrees with the rows. In (b) and (c), every destination's function or type adds into its own target and parts and nowhere else, checked by running it in a constant expression.

**Cutoff AB and Reso AB are their own targets.** The Filter Control's law, as decided in M1.3 task 6, clamps each control together with its own modulation. Cutoff AB is therefore the Filter Control's term, and it isn't Cutoff A plus Cutoff B where a clamp acts. Every other AB destination is the same target on both parts.

**The voice.** [modulation_candidates.hpp](../../tests/dsp/modulation/modulation_candidates.hpp) holds `ModVoice`, a copy of `dsp::Voice` as built. It runs on `ModOscillatorSlot` ([mod_oscillator_slot.hpp](../../tests/dsp/modulation/mod_oscillator_slot.hpp)), a copy of `OscillatorSlot` that takes Volume, Pan and Phase modulation, each as a constant, a line across the segment, or a value per sample. With no routes, `ModVoice` is `dsp::Voice` to the bit (measured below). It adds:

- **The sources**, as task 3 would build them:
  - two mod envelopes (`Adsr`) per voice;
  - two LFOs (`Lfo`) per voice, restarted at a note-on from silence and retriggered on a sounding voice (Free isn't modelled). An LFO is the `Lfo`'s wave at gain 1 and offset 0, scaled outside as 0.5 × gain × (wave + offset) (M6), so a route can move its Gain without the `Lfo`'s own 10 ms gain ramp;
  - velocity, keytrack, and the amp envelopes;
  - ModWheel and Aftertouch as ramps read at absolute samples, and Step Velocity as a constant.
- **The routes**, compiled with registry (a) at a note-on and at the first grid point after the patch changes.
- **A rate per target.**
- **The two LFO orders.**
- **The bounds.** Each route to Mix adds amount × its source's bound to `mixModulationBound()`, the seam M1.4 task 5 left. A slot at Volume 0 counts as silent only while its part's Volume routes can't lift it.
- **Cutoff and Reso** are fed at the grid points as M1.3 decided. They read the sources as they stand there or, for the overshoot question, the LFOs at the segment's end.
- **Mix** is per sample, as M1.4 decided.

`ModEngine` runs the real `VoiceAllocator` over 16 of these voices. Deliverable 5's stand-in global destination is a gain on the absolute 32-sample grid, fed from the voices by each rule.

## What they measured

The candidates were measured by the hidden `[modulation-report]` ([modulation_report.cpp](../../tests/dsp/modulation/modulation_report.cpp), about 2 s) and the `[modulation]` benchmarks ([modulation_benchmark.cpp](../../tests/dsp/benchmarks/modulation_benchmark.cpp)). The default run checks the invariants under `[modulation-model]` ([modulation_test.cpp](../../tests/dsp/modulation/modulation_test.cpp)):

- the three registries agree on 200 random patches;
- an empty slot or an amount of 0 adds an exact 0;
- keys, not labels or positions, find each destination;
- every envelope and LFO bound holds over random segments;
- the prototype with no routes is the library to the bit;
- a busy patch gives the same output at host blocks of 1, 37 and 4096, with no allocation, at every rate in both LFO orders.

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[modulation-report]"
tools/bench.sh "[modulation]"
```

### The prototype against the library

`ModVoice` with no routes against `dsp::Voice`, through `VoiceAllocator` (Polyphony 2, two steals, releases of 50 ms), 0.5 s:

| Patch | host blocks of 512 | of 37 |
|---|---|---|
| init (A1 saw) | identical | identical |
| A1 and B1 saws, own filters | identical | identical |
| 4 × 8 unison saws, resonant | identical | identical |

The envelopes now render before the slots (they are sources), which changes nothing: nothing they compute depends on the slots.

### Question 1: the registries

Each candidate's data (bytes, arm64; strings extra):

| | (a) table | (b) functions | (c) types |
|---|---:|---:|---:|
| a destination descriptor | 56 | 16 + 56 | 56 |
| the destination table | 1,568 | 448 + 1,568 | 224 + 1,568 |
| a compiled route | 8 | 24 | 8 |
| a voice's 16 compiled routes | 136 | 392 | 136 |

**Code size.** Each candidate is compiled on its own (the `registry_size_*.cpp` probes: compile 16 routes, accumulate them, and one bound), with `clang++ -std=c++23 -Os -fno-exceptions -fno-rtti`, then measured with `size -m`:

| | `__text` | constant data | strings | total |
|---|---:|---:|---:|---:|
| (a) table, with keys and labels | 576 | 2,144 | 807 | 3,527 |
| **(a) table, the DSP's fields only** | **568** | **572** | **0** | **1,140** |
| (b) functions | 1,032 | 2,784 | 807 | 4,623 |
| (c) types | 892 | 38 | 0 | 930 |

- **(c) folds every descriptor into code**: no table survives. But its dispatch is a chain of comparisons.
- **(a) and (b) carry the whole table, strings and all**, because the DSP reads rows that also hold the keys and labels. The DSP needs only 20 bytes of each destination's 56: target, parts, scope, rate, units, range. A table of those fields alone (`dspDestinations`) is 1,140 bytes in all, close to (c).
- **STM32 not measured:** no ARM toolchain is installed (`arm-none-eabi-g++`), so these are arm64 numbers. The ratios are the useful part.

**Speed** (benchmarks, ns per call):

| | (a) | (b) | (c) |
|---|---:|---:|---:|
| accumulate 16 routes | 13.97 | 39.48 | 42.55 |
| 16 bounds, in a runtime order | 18.93 | 33.72 | 29.40 |

(a)'s sums are a loop over plain data, with nothing to dispatch. (b) calls through a pointer per route. (c) walks a fold of comparisons.

**What Phase 2 edits:**
- **(a):** a row and an enum value, both checked against each other at compile time. A source of a new *kind* of bound also edits one 4-way switch on the kind.
- **(b):** a row with its functions.
- **(c):** a type and one more entry in the list.

None of them edits a switch on the destination or the source.

### Question 2: routing

- **No routes is the library to the bit** (above) and costs nothing measurable: 128.7 against 130.2 ns per voice.
- **An empty slot or an amount of 0 adds nothing.** It drops out when the patch is compiled (`compile()` keeps only live routes), so nothing is summed for it. The default run checks that in all three registries.
- **Routes add per target and part.** Two routes to Mix A add, as the developer's "+15 (in 2 mod dest slots)" requires.
- **Routes cost little**: 4 routes into one destination add 4–12 ns per voice and 16 routes 10–22 ns, the destination's own work included (CPU below).

### Question 3: the rate per destination

**Zipper on a 40 Hz LFO.** The setup:

- A1 a sine at A3 (220 Hz), both filters bypassed, drift off, one note;
- LFO 1 a sine at 40 Hz, Gain 10, on the destination.

The measure is filters.md's click: energy above 1 kHz in a Hann 4096, dB re an instant unit step, averaged over 0.3, 0.5, 0.7 and 0.9 s. *Added* is the same measure on the difference from per sample:

| Destination (depth) | per sample (its own) | grid, held | grid, ramped | added: held | added: ramped |
|---|---:|---:|---:|---:|---:|
| Pitch A (±¼ octave) | −72.4 | −41.0 | −72.4 | **−40.9** | −84.2 |
| Phase A (±18°) | −50.7 | −17.6 | −50.7 | **−17.5** | −65.8 |
| Volume A (5 ± 5) | −46.6 | −13.3 | −46.6 | **−13.3** | −61.8 |
| Pan A (centre ± 5, hard left to hard right) | −42.7 | −9.4 | −41.0 | **−9.4** | −45.5 |
| LFO 2 Rate ±1 unit (LFO 2 at 3.2 Hz on Pitch A, ±¼ octave) | −87.4 | −87.4 | | **exact 0** | |
| LFO 2 Gain 5 ± 5 (LFO 2 at 3.2 Hz on Volume A, 5 ± 5) | −55.5 | −22.3 | −55.5 | **−22.3** | −65.9 |

- **Held zips on every audio destination**: −9 to −41 dB added. Volume's −13.3 is the mixer's −13 for Mix.
- **Ramped adds −45 to −84 dB.** Pan ramps its two gains between the grid points rather than its position, so its law is linear between them: −45.5 dB.
- **LFO Rate per sample is held, to the bit.** `Lfo` takes a new rate on a curved row (the sine) only at its own grid point ([lfos.md](lfos.md)). A voice's LFO restarts with the voice, so that grid is the voice's.

**The same output at any host block size.** The same patches, with ModEnv 1 on Cutoff A, velocity on Mix A, and a sequence (Polyphony 2: four notes, a steal, releases of 50 ms), 1 s at host blocks of 1 and 37 against 4096:

| Destination | per sample | grid, held | grid, ramped | allocations |
|---|---|---|---|---:|
| Pitch A, Phase A, Volume A, Pan A, LFO 2 Gain | 2 of 2 | 2 of 2 | 2 of 2 | 0 |
| LFO 2 Rate | 2 of 2 | 2 of 2 | | 0 |

**Found while building it:** the slot must not reset its oscillators' phase step at the start of each `process()` call while Phase is modulated per sample. The reset makes one extra pitch change per call, which can queue a band-limited bend. So the output depended on how the host split its blocks: renders at blocks of 1 differed from 4096. The prototype sets the step once per sample instead, and the default run's block-size test pins it.

**Phase: does it stay band-limited?** The setup:

- one unison voice of a saw, filters bypassed, drift off;
- LFO 1 a sine at 40 Hz on Phase A;
- the carrier off any simple ratio to 40 Hz and to the sample rate, so that aliases miss the modulation's lines.

The measure is the energy below 20 kHz off the phase modulation's lines (k·fc ± m·fm), dB re the total: what aliasing and zipper add. The unmodulated column is the oscillator's own aliasing:

| Case | unmodulated | per sample | grid, held | grid, ramped | jumps (per sample) |
|---|---:|---:|---:|---:|---:|
| 1,090 Hz, ±18° | −98.5 | −62.3 | −29.5 | −62.3 | 0 |
| 1,090 Hz, ±180° | −98.5 | −43.4 | −12.0 | −43.4 | 1 |
| 55.6 Hz, ±180° | n/a | n/a | n/a | n/a | 25,473 of 70,336 |

- **Per sample and ramped treat Phase as frequency modulation.** Each sample's change of offset goes into the phase step, so it stays band-limited by the oscillator's own pitch-step rule, and both measure the same. What remains off the lines is the LFO's own interpolation and the oscillator's response to a moving step.
- **Held is a band-limited jump every 32 samples**, and that zips: −29.5 and −12.0 dB.
- **The oscillator can't run backwards.** Where the offset falls faster than the note's own phase advances, the step would be negative. For ±180° at 40 Hz that happens below 126 Hz. The prototype then makes the change as a band-limited jump: 36% of the samples on a 55.6 Hz note. At that density the lines cover the whole band, so the off-line measure can't separate anything (n/a).

### The fast-sweep overshoot (filter-control.md, recommendation 8)

filter-control.md's patch through the voice:

- a 110 Hz saw;
- Cutoff A 5 + Control 5, with LFO 1 on Cutoff A through both of its routes at +10 (±10 units);
- Resonance A and Control 10, with ModEnv 1 (sustain 1) on Reso A at +10 (k 12 under the per-control clamp);
- Warm on, LP24.

The peak from 0.2 s to 2 s, with the cutoff fed from the sources as they stand at each grid point, or from the LFO's value at the segment's end (it is rendered ahead):

| LFO | 48 kHz: as they stand | 48 kHz: segment's end | 44.1 kHz: as they stand | 44.1 kHz: segment's end |
|---|---:|---:|---:|---:|
| held (Gain 0) | 0.37 | 0.37 | 0.36 | 0.36 |
| 0.5 Hz | 0.88 | 0.78 | 1.14 | 1.67 |
| 5 Hz | 4.24 | 2.27 | 12.00 | 11.46 |

**The rate doesn't remove the overshoot.** Reading the LFO at the segment's end moves the peak by between −46% and +46%. The sample rate moves it by ×2.8. At 44.1 kHz it is filter-control.md's 13.10 again (12.00). It is the ladder's response to the sweep, as filter-control.md found, and its peak depends on where the sweep falls against the resonance.

### Question 4: order and feedback

The test signal is A1 a sine at A3, filters bypassed, with notes at 0 and 0.63 s, released at 1.25 and 1.67 s, over 2 s. The two orders:

- **Segment order:** LFO Rate held and Gain ramped, on the grid.
- **Sample order:** both per sample.

*Differs* is the RMS of the difference between the two orders' renders, re the sample-order render, from 0.2 s.

| Case | segment order: same at blocks 1, 37 | sample order: same at 1, 37 | differs | allocations |
|---|---|---|---:|---:|
| a cycle: LFO 1 → LFO 2 Rate (+5), LFO 2 → LFO 1 Gain (+10), both on Pitch A | 2 of 2 | 2 of 2 | −6.9 dB | 0 |
| a self-route: LFO 1 (saw) → LFO 1 Rate (+5) and Pitch A | 2 of 2 | 2 of 2 | +3.3 dB | 0 |

**The delay through a route into a source** (M11's protocol): LFO 1 S&H at 2.03 Hz, so that its steps fall anywhere in a segment, to LFO 2 Rate (+5), with LFO 2 a pulse on Pitch A. The measure is samples from each of LFO 1's steps to LFO 2's rate changing, over 8 steps:

| Order | min | mean | max |
|---|---:|---:|---:|
| segment | 1 | 14.0 | 30 |
| sample | 1 | 1.0 | 1 |

- **Both orders are deterministic and independent of the block size.** Neither allocates.
- **They sound different through a cycle**: −6.9 dB apart, and the self-route's two renders drift apart altogether (+3.3 dB). The delay is part of the loop, so neither is the "right" sound. M11's demo protocol measures Sylenth1's delay.
- **Segment order costs nothing measurable.** Sample order costs 23 ns per voice more with one route, and 68–95 ns more with 16 routes (CPU below). It also takes away what rendering ahead gives: the LFOs' exact bounds, and a Cutoff that can read an LFO at the segment's end.

### Question 5: a per-voice source on a global destination

The sequence: four notes 0.4 s apart from 0.1 s (velocities 100, 60, 127, 80), released 0.4 s apart from 2.0 s in the order played. The source is ModEnv 1 (attack 20 ms, decay 200 ms, sustain 0.5, release 300 ms) or velocity. It is read from the voices at each 32-sample point of the absolute grid, onto a stand-in global gain (5 + 5 × value, ramped over each segment) on a fixed 220 Hz sine. The measures:

- *Step*: the largest change between two grid points, from just before an event to 50 ms after (the second to the fourth note-on; every note-off).
- *Held*: the value 0.35 s after each note-on.
- *After release*: the value 0.1 s after each note-off.
- *Click*: filters.md's measure at every note-on and note-off.

| Source | Rule | step at a note-on | at a note-off | held: 1 / 2 / 3 / 4 notes | after each release | click |
|---|---|---:|---:|---|---|---:|
| ModEnv 1 | newest voice | **0.438** | 0.008 | 0.50 / 0.50 / 0.50 / 0.50 | 0.50 / 0.50 / 0.50 / 0.05 | −38.6 |
| | oldest held voice | 0.000 | 0.008 | 0.50 / 0.50 / 0.50 / 0.50 | 0.50 / 0.50 / 0.50 / 0.05 | −69.8 |
| | sum over voices | 0.062 | 0.008 | **0.50 / 1.00 / 1.50 / 2.00** | 1.55 / 1.05 / 0.55 / 0.05 | −66.3 |
| | **a global copy** | **0.000** | 0.008 | 0.50 / 0.50 / 0.50 / 0.50 | 0.50 / 0.50 / 0.50 / 0.05 | **−69.8** |
| velocity | newest voice | 0.528 | 0.000 | 0.79 / 0.47 / 1.00 / 0.63 | 0.63 / 0.63 / 0.63 / 0.63 | −36.6 |
| | oldest held voice | 0.000 | **0.528** | 0.79 / 0.79 / 0.79 / 0.79 | 0.47 / 1.00 / 0.63 / 0.63 | −36.6 |
| | sum over voices | 1.000 | 0.000 | **0.79 / 1.26 / 2.26 / 2.89** | 2.89 / 2.10 / 1.63 / 0.63 | −38.9 |
| | a global copy (the last note's) | 0.528 | 0.000 | 0.79 / 0.47 / 1.00 / 0.63 | 0.63 / 0.63 / 0.63 / 0.63 | −36.6 |

- **The newest voice jumps at every note-on.** Each new envelope starts from 0, a step of 0.44 from the sustain, and it clicks at −38.6 dB.
- **The oldest held voice jumps when a key goes up.** Velocity changes by 0.53 at the release of the oldest note.
- **The sum grows with the chord**: four notes give four times one note's value (2.0, 2.89), so the effect depends on how many keys are down.
- **A global copy is smooth through note-ons and note-offs**, independent of the chord, and defined when nothing sounds (−69.8 dB). It is one envelope, gated by the first key down and released by the last. For velocity it is the last note's, which is the newest voice's.

### Question 6: each source's bound

**Against the source's values over random segments.** Each bound is taken at a random point for the next 1–32 samples, with events only between segments (as the skip test allows: an event re-tests).

- Envelopes: 60 random ADSRs, 200 segments each, with random gates and sustain changes.
- LFOs: every waveform at random rates (0.1 Hz to 819 Hz), gains and offsets, with random retriggers.

| Source | Bound | segments | too small | worst excess | mean width |
|---|---|---:|---:|---:|---:|
| envelope | its range [0, 1] | 12,000 | 0 | 0 | 1.000 |
| | PLAN's: 0 when idle or held at 0, else [0, 1] | 12,000 | 0 | 0 | 0.875 |
| | **by stage** | 12,000 | **0** | 0 | **0.397** |
| LFO | its range (±1.125) | 26,400 | 0 | 0 | 2.250 |
| | **PLAN's: the wave's ends and crests** | 26,400 | **1,173** | **0.997** | 0.192 |
| | **exact: the segment rendered ahead** | 26,400 | **0** | 0 | **0.017** |

- **PLAN's LFO bound is too small**: in 4.4% of segments, by up to 0.997. It reads the waveform's row and misses the slews: a retrigger slews from wherever the LFO was, and an edge slews across the jump. A bound that is too small skips audio, so this one is a bug, and the test finds it.
- **Rendering the segment ahead makes the LFO's bound exact**, and 132× narrower than its range.
- **By stage** is 2.2× narrower than PLAN's envelope rule, and never too small.
- **The LFO's range is ±1.125, not ±1.** The halving (M6) makes a Sine at Gain 10 ±0.5, and Offset +10 makes 0 to +1. A slew can carry the wave 0.25 past ±1 ([lfos.md](lfos.md)), which halves to 0.125. The trials here never reached it (largest 1.0000), but a bound must hold in the worst case.

**How much each bound lets the skip test skip.** The setup:

- A1 and B1 saws, own filters;
- Mix B at 0, through Step Velocity × −10 on Mix B, since `ModEngine` has no Mix knobs;
- one route lifting Mix B;
- 16 notes of 0.2 s, one every 0.25 s.

*Skipped* is part B's outputs skipped for their gain, of those tested at grid points:

| Route on Mix B | the sources' ranges | tight |
|---|---:|---:|
| LFO 1 sine 8 Hz, +10 (±5 internal) | 0.3% | 52.8% |
| LFO 1 pulse 8 Hz, Offset −10, at −10 (0 to +10) | 0.3% | 60.5% |
| ModEnv 1 pluck (decay 50 ms, sustain 0), +10 | 0.3% | 75.1% |

The ranges skip only the gaps between notes, where the amp envelope proves it.

### Real-time terms

Every candidate gives the same output at host blocks of 1, 37 and 4096, with no allocation while processing (the tables above). `ModVoice` holds everything in fixed arrays: 12 sources' chunk buffers, 14 targets' per-sample sums, two LFO segments, and the copies of the LFOs taken at each grid point. Nothing is allocated, `prepare()` included. It is 33,688 bytes against `dsp::Voice`'s 25,424 (+8.3 KB): mostly the per-sample sums (3.5 KB) and the source buffers (1.5 KB), which stage 2 can size to the routes a patch can have.

### CPU

`tools/bench.sh "[modulation]"`, `relwithdebinfo`, Apple M1 Ultra (virtualised). One voice held at the init patch, past the attacks, 512 samples per run at 48 kHz, the median of three runs (spread 0.4–6.6%), ns per sample per voice. For each destination and rate, 4 routes (an envelope, an LFO, velocity, the mod wheel) or all 16. For LFO Rate and Gain, LFO 1 also sits on Pitch A, and that route alone is their 0. The full table is in [baselines.md](../performance/baselines.md#2026-11-22-m15-the-modulation-model-stage-1-prototypes).

| Destination | per sample: 4 / 16 routes | held | ramped |
|---|---:|---:|---:|
| (none: `dsp::Voice` / `ModVoice`) | 130.2 / 128.7 | | |
| Pitch A | 139.7 / 144.8 | 132.6 / 138.8 | 136.6 / 140.3 |
| Phase A | 140.8 / 146.9 | 134.4 / 138.2 | 136.1 / 141.5 |
| Volume A | 140.1 / 151.1 | 135.9 / 145.1 | 134.3 / 145.1 |
| Pan A | 261.6 / 247.9 | 248.4 / 231.3 | 248.5 / 234.6 |
| LFO 1 Rate (0 / 4 / 16) | 158.6 / 170.9 / 235.6 | 135.4 / 140.9 / 141.0 | |
| LFO 1 Gain (0 / 4 / 16) | 159.3 / 164.7 / 207.7 | 136.2 / 137.1 / 139.4 | 138.7 / 136.5 / 141.7 |
| 16 routes, mixed, at the recommended rates | 273.5 | | |
| 16 voices: no routes / 16 routes, mixed | 2,032.9 / 4,114.6 per sample | | |

- **Each destination's own work is a few ns per voice.** Per sample costs 3–6 ns more than ramped for Pitch, Phase and Volume.
- **A Pan route costs 100–135 ns per voice, at every rate**, because a part whose Pan moves isn't centred, and the voice leaves its mono path ([part-solo-and-skipping.md](part-solo-and-skipping.md)). The stereo init patch costs about 240 ns per voice. The pan law per sample adds about 13 ns on top, at one unison voice.
- **LFOs in sample order cost 23 ns per voice more than on the grid with one route, and 68–95 ns more with 16 routes into the gain or the rate.** Most of it is `Lfo::setFrequency()` and a one-sample `process()` every sample. In segment order the LFOs cost nothing measurable.
- **The mixed patch doubles 16 voices**, mostly through its two Pan routes.

## Recommendations

*Stage 1's recommendations, as written. Where only the demo can settle a question (M1–M13), the recommendation is the working hypothesis, and the measurement is what makes it the safer default. Chosen by the project owner on 2026-11-22, every one ([The decision](#the-decision)).*

| # | Question | Recommendation | Why | Alternatives | Settled by |
|---|---|---|---|---|---|
| 1 | **The registries** | **(a) `constexpr` tables indexed by an enum, holding only the DSP's fields in `libs/dsp`.** A destination row holds its target, parts, scope, rate, units per amount and knob range. A source row holds its scope, range and bound kind. **The keys and labels live with the parameter table (M1.9)**, beside the presets that save them, and a `static_assert` there ties each key to its enum value. | The fastest at every job (16 routes summed in 14 ns, against 39 and 43). 1,140 bytes with no strings, against 930 for (c), 3,527 for (a) with its strings, and 4,623 for (b). An 8-byte compiled route, against (b)'s 24. The data is the behaviour, with no switch on a destination or a source. Every row is checked at compile time. `libs/dsp` stays free of strings it doesn't read, which matters on STM32. | (c) types: smallest, but a fold of comparisons per route (43 ns), and every row a type. (b) functions: an indirect call per route, the largest and slowest. | (ours) |
| 2 | **The persisted ID** | **A string key per destination and source** (`"osc.pitch.ab"`, `"lfo.1"`): stable once released, never reused, never a position. "None" is `"none"`. | Presets outlive the menu's order (M1 may still change it) and Phase 2's additions. A string can't be mistaken for the template's numbering. The enum's order is internal and free to change. | A number per row, which is smaller but can be confused with an index. | M1.10 (D7) |
| 3 | **Routing** | **The 16 slots, compiled into a list of live routes** at a note-on and at the first grid point after the patch changes. An empty slot or an amount of 0 drops out. **Sums per target and part**: AB is both parts, except **Cutoff AB and Reso AB, which are the Filter Control's own terms** (the M1.3 law). | No routes is the library to the bit, and costs nothing measurable (128.7 against 130.2 ns). Recompiling at a grid point keeps a mid-block patch change independent of the host's blocks. | Compile on every change at once (not measured: a change mid-segment would then take effect where the host's block falls). | M3 |
| 4 | **Pitch** | **Per sample**, through the slots' per-sample note path (the glide's) | Exact. Held zips at −40.9 dB added. Ramped is −84.2 dB, but it trails by a segment and saves only 3–5 ns per voice. | Ramped (cheaper by 3%). | M12 |
| 5 | **Phase** | **Per sample**: each sample's change of offset added to the oscillator's phase step, which is phase modulation as frequency modulation. **Where the step would go negative or past Nyquist, a band-limited jump.** | It stays band-limited (−62.3 dB off the lines at ±18°, as ramped). Held is a jump every segment: −17.5 dB added, −29.5 off the lines. Per sample costs about 5 ns per voice more than ramped. The manual evaluates pitch and phase per voice, and this puts them on one path. | Ramped (−65.8 dB added). Negative steps in `MinBlepOscillator` (it runs only forwards today). | M12 |
| 6 | **Volume** | **Per sample**, as Mix (M1.4) | Exact, and consistent with Mix. Held zips at −13.3 dB added (Mix's −13). Per sample costs about 6 ns per voice more than ramped. | Ramped (−61.8 dB added). | M12 |
| 7 | **Pan** | **On the grid, ramped**: each unison voice's two gains computed at the grid points and interpolated linearly. **A Pan route takes its part off the mono path.** | −45.5 dB added at a full-depth 40 Hz sweep. The law (a sine per unison voice) once per segment, not per sample: up to 32 sines per sample per voice at 4 × 8 unison. Leaving the mono path is what costs (about 110 ns per voice on the init patch), and it is the same at every rate. | Per sample (exact, +13 ns per voice at one unison voice, scaling with the count). Held (−9.4 dB: no). | M12 |
| 8 | **LFO Rate and Gain, and the order** | **Segment order.** At each grid point, each LFO's rate (held) and gain (ramped from the previous point's value) are set from the sources as they stand there, the LFOs at their last output. Its wave is rendered for the whole segment ahead. A retrigger mid-segment renders the rest again from a copy taken at the grid point. **The delay through a cycle or a self-route is up to one segment** (1–30 samples, 14 on average). | Per-sample Rate *is* held for a sine (exact 0 apart), since `Lfo`'s curved rows take a rate only at their grid point. Gain ramped adds −65.9 dB, held −22.3. Costs nothing measurable, where sample order costs 23–95 ns per voice more. Deterministic at any block size. It makes the LFOs' bounds exact, and lets Cutoff read an LFO at the segment's end if wanted. | Sample order (a delay of one sample; per-sample `setFrequency()`). | M11, M12 |
| 9 | **Cutoff and Reso** | **As decided in M1.3**, at the grid points into the Filter Control's modulation inputs, **with the sources as they stand at the grid point.** | Reading an LFO at the segment's end moves the fast-sweep overshoot by −46% to +46% but doesn't remove it. The sample rate moves it by ×2.8. Reading every source the same way keeps one rule. | The LFOs at the segment's end (exact for them, not for envelopes). | F30 |
| 10 | **A per-voice source on a global destination** (PhsrFreq, DistAmnt) | **Each per-voice source has a global copy, which global destinations read**: an envelope gated by the first key down and released by the last (legato within a phrase); the last note-on's velocity; a free LFO's global instance (L8); the last note's keytrack. The destinations are evaluated on the absolute 32-sample grid, ramped. **A requirement on M1.7.** | No step at note-ons or note-offs (−69.8 dB), independent of the chord, defined when nothing sounds, and computed once rather than read from 16 voices. | The newest voice (a jump of 0.44 at each note-on, −38.6 dB). The oldest held voice (jumps when a key goes up). The sum (×4 with a four-note chord). | M10 |
| 11 | **Each source's bound** (the skip test) | **An envelope by stage**: between its level and where the stage heads (1 in the attack, the sustain in the decay and sustain, 0 in the release, [0, 0] when idle). **An LFO exact**, from its segment rendered ahead (gain and offset as intervals). **A linear source, its two ends.** The rest, their range. **A slot at Volume 0 is silent only while its Volume routes can't lift it.** | Never too small in 38,400 random segments. 2.5× (envelope; 2.2× against PLAN's rule) and 132× (LFO) narrower than the ranges, so the skip test skips 53–75% of part B's segments where the ranges skip 0.3%. PLAN's LFO rule is too small in 4.4% of segments, by up to 0.997, because it misses slews. | Every source its range (exact, never skips a modulated output). PLAN's envelope rule (correct, wider). | (ours) |

### What the library needs (stage 2)

- **`libs/dsp`:** the DSP rows (`DspDestination`, the sources' kinds and ranges) and the enums; `compile()`, `accumulate()` and the bound per kind; `constexpr` throughout, with `static_assert`s on the rows.
- **`OscillatorSlot`:** Volume, Pan and Phase modulation inputs, as the prototype's `ModSignal`.
  - Volume and Pan: gains computed outside the `StereoGain` while modulated, with the knobs read as global ramps (as the Mix knobs are) instead of the slot's own 10 ms ramps.
  - Phase: a running offset that a retrigger keeps, so a retrigger lands on Phase plus the offset in force.
  - **The step set once per sample while Phase moves**, never re-set at a call's start (the block-size bug above).
- **`Lfo`:** nothing new for segment order. The voice renders the segment ahead, keeps a copy at each grid point, and applies gain and offset itself. The gain could move into an input of `Lfo`'s own instead of `setGain()`'s 10 ms ramp.
- **`dsp::Voice`:**
  - the sources (task 3);
  - the compiled routes, recompiled at grid points;
  - the sums per target and part;
  - Pitch per part, per sample;
  - the slots' modulation;
  - the Filter Control terms from the grid sums;
  - Mix's term per sample;
  - `mixModulationBound()` and the Volume test from the bounds;
  - Pan routes in `allCentred()`.
- **Interop (M1.9):** the keys and labels, each tied to its enum value at compile time.
- **M1.7:** rule 10's global copies, read by the global destinations on the absolute grid.

### What this pushes back on

- **PLAN task 3's LFO bound** ("its largest value there: its ends, or its crest if one falls inside: exact") and **part-solo-and-skipping.md's recommendation 9** ("±1 if a crest falls inside"): too small across slews. The segment rendered ahead is exact.
- **The LFO's range**: the plan's "up to ±2" became ±1 with M6's halving. It is **±1.125** with a slew. Inventory M6 is annotated.
- **M3's hypothesis** that Cutoff AB and Reso AB are "the same as one route to A plus one to B": under the Filter Control law decided in M1.3 task 6, AB is the Filter Control's own term, clamped with that knob, so the two differ wherever a clamp acts. Inventory M3 is annotated.
- **Drive is not a destination** (task 1), so it is struck from task 2's open rates in PLAN.md.
- **lfos.md (L11):** a curved row takes a new rate only at its own grid point, so per-sample Rate modulation on a sine is per segment anyway. A modulated Gain can't go through `setGain()`, whose 10 ms ramp would lag it.
- **part-solo-and-skipping.md's mono path:** a Pan route ends it for its part, about 110 ns per voice on the init patch.
- **filter-control.md, recommendation 8:** the modulation's read point doesn't remove the overshoot, and the peak moves ×2.8 between 44.1 and 48 kHz, so M1.8's golden renders should fix the sample rate.
- **`MinBlepOscillator` runs only forwards.** Deep phase modulation on low notes becomes band-limited jumps (36% of the samples of a 55.6 Hz note at ±180°). Whether that is heard against Sylenth1's is a question for M1.13.

## The decision

Chosen by the project owner on 2026-11-22: every recommendation (Decisions log). Stage 2 builds the model in `libs/dsp`, bit for bit the stage-1 prototype at host blocks of 1, 37 and 4096, with no allocation.

- **The registries:** `constexpr` tables indexed by an enum, holding only the DSP's fields in `libs/dsp`. A destination row holds its target, parts, scope, rate, units per amount and knob range. A source row holds its scope, range and bound kind. The keys and labels live with the parameter table (M1.9), tied to the enum at compile time.
- **The persisted ID:** a string key per destination and source (`"osc.pitch.ab"`, `"lfo.1"`, `"none"`), stable once released, never reused, never a position.
- **Routing:** the 16 slots compiled into a list of live routes at a note-on and at the first grid point after the patch changes; an empty slot or an amount of 0 drops out. Sums per target and part: AB is both parts, except Cutoff AB and Reso AB, which are the Filter Control's own terms.
- **Rates:**
  - Pitch, Phase, Volume and Mix per sample;
  - Phase as a running offset folded into the oscillator's phase step, set once per sample while it moves, with a band-limited jump where the step would leave [0, Nyquist);
  - Pan ramped on the grid (each unison voice's gains at the grid points, interpolated); a Pan route takes its part off the mono path;
  - Cutoff and Reso as M1.3 decided, with the sources as they stand at the grid point.
- **LFO Rate and Gain, and the order:** segment order. At each grid point each LFO's rate (held) and gain (ramped) are set from the sources as they stand there, and its wave is rendered for the whole segment ahead; a retrigger mid-segment renders the rest again from a copy taken at the grid point. The delay through a cycle or a self-route is up to one segment.
- **A per-voice source on a global destination:** each per-voice source has a global copy, which the global destinations read on the absolute 32-sample grid, ramped: an envelope gated by the first key down and released by the last, the last note-on's velocity, a free LFO's global instance, the last note's keytrack. A requirement on M1.7.
- **Each source's bound:** an envelope by stage; an LFO exact, from its segment rendered ahead; a linear source its two ends; the rest their range. A slot at Volume 0 is silent only while its Volume routes can't lift it.

## Tests

Task 1 is research and adds no code. Task 2's stage 1 adds the prototypes' checks under `[modulation-model]` in the default run ([modulation_test.cpp](../../tests/dsp/modulation/modulation_test.cpp)):

- the three registries compute the same sums and bounds on 200 random patches;
- an empty slot or an amount of 0 adds an exact 0;
- each destination is found by its key, not its label or position;
- each size probe compiles its candidate on its own;
- every envelope bound (range, PLAN's, by stage) holds over 12,000 random segments, and the LFO's range and exact bounds over 26,400;
- `ModVoice` with no routes is `dsp::Voice` to the bit, at host blocks of 512 and 37;
- a busy patch (12 routes, an LFO cycle and a self-route) gives the same output at host blocks of 1, 37 and 4096 at every rate in both LFO orders, with no allocation.

The compile-time checks are `static_assert`s on the rows and on each candidate. Tasks 3–4 add the library's tests: each source's values against its definition and its bound against its values, each destination's units and clamp, the init patch rendering bit-identically to before, and bit for bit the stage-1 prototype at host blocks of 1, 37 and 4096.

## References

- LennarDigital, *Sylenth1 User Manual* v2.5 (2015), pp. 5, 7, 8, 19–23, 30.
- LennarDigital, [Sylenth1 changelog](https://lennardigital.com/sylenth1/whatsnew.php) (v3.000–v3.075). v1.1 notes ([mirror](https://www.svartling.net/2006/11/lennardigital-sylenth1-v11-pc.html), 2006). KVR news for [v1.20](https://www.kvraudio.com/news/lennardigital_releases_sylenth1_v1_20_6809) and [v1.21](https://www.kvraudio.com/news/lennardigital_updates_sylenth1_to_v1_21_7065) (2007).
- Lennard (the developer) on KVR: [p=4006903](https://www.kvraudio.com/forum/viewtopic.php?p=4006903) and [p=4008220](https://www.kvraudio.com/forum/viewtopic.php?p=4008220) (2010, pitch amounts); [p=4239542](https://www.kvraudio.com/forum/viewtopic.php?p=4239542) (2010, a negative amount); thread [t=461365](https://www.kvraudio.com/forum/viewtopic.php?t=461365) (2016, additive modulation, Mix's scale, keytrack).
- P. I. Nerseth, [FXPSylenth1Preset.bt](https://github.com/perivar/AudioVSTToolbox/blob/master/PresetConverter/Assorted/FXPSylenth1Preset.bt), an 010 Editor template for Sylenth1 preset files (revision 1.3). Used only for the names of the destinations and sources.
- [KVR product page](https://www.kvraudio.com/product/sylenth1-by-lennardigital) ("16 Modulation slots with 24 different selectable modulation parameters each").
- Tutorials: [Attack Magazine](https://www.attackmagazine.com/technique/synth-secrets/pitched-percussion/) (2015), [Audiotent](https://www.audiotent.com/blogs/production-tips/5-things-you-need-to-know-about-sylenth1). Users on KVR, cited with permalinks in the inventory.
