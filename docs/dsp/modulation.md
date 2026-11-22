# Modulation: sources, destinations, and amounts

Modulation moves a control over time without anyone turning its knob. A source (an envelope, an LFO, velocity, the mod wheel) produces a value, and a route adds that value, scaled by an amount, to a destination such as pitch, cutoff or volume. This page covers M1.5. So far it holds task 1, which is research only:

- **Theory:** fixed slots against a matrix; additive modulation in knob units, and what that means for each kind of knob; scaling a source; polyphonic and monophonic sources and destinations, and the four ways they meet; control rate against audio rate; order and feedback when sources are also destinations; and bounds for skipping.
- **Sylenth1:** what is known about its sources, destinations and amounts, and what that means for the build.

Tasks 2–5 will add the candidates, the measurements, the recommendations, the decision and the as-built sections.

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

  "A", "B" and "AB" are probably part A's two oscillators (or filter A), part B's, and all of them (M3). KVR counts 24, and the menu's order and labels are unconfirmed (M1). The destinations do not include envelope stages (AL8), Main Volume (X7), Detune, Octave or Fine, Drive, Keytrack, any amount, or any effect parameter other than the two above. Users asked for envelope stages and more targets between 2007 and 2021, and no changelog added any after v1.20.
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
- **What is open.** The menus (M1, M2), what A, B and AB select (M3), the amount's display and law (M4, M5), the LFO's scale (M6), each source's range and each destination's units (M7, M8), where each destination clamps (M9), per-voice sources on global destinations (M10), cycles (M11), each destination's update rate (M12), and 7-bit controller steps (M13). M1 and M2 need only screenshots of the demo's popups. The project owner can take them at any time, and the clean-room rule allows it.

### What this means for the build

- **Task 2's registries** need rows for 27 destinations and 11 sources. Each destination row carries a group, a scope, a unit per amount, and a knob range for the clamp. Each source row carries a scope, a range, and a bound. Rows are data, so M1's answer can change the list without changing code. The persisted IDs are ours and are separate from the menu order.
- **Two destinations are global** (PhsrFreq, DistAmnt), and **three of the misc sources are global** (ModWheel, Aftertouch, Step Vlcty). Task 2 must give the per-voice-to-global rule a recommendation even though Sylenth1's is unknown (M10).
- **Some sources are also destinations**: the four LFO controls. That is the only possible cycle (M11), since no envelope or amount is a destination.
- **Phase as a destination** is new work for the oscillator slot (a running phase offset, band-limited), and the most expensive per-voice destination, as the manual warns.
- **The LFO's factor of 2** must be settled before task 3 builds the LFO source (M6). Until the demo answers, halving at the source matches every number the developer gave.

## Tests

Task 1 is research and adds no code. Tasks 2–4 add the tests: each source's values against its definition and its bound against its values, each destination's units and clamp, the init patch rendering bit-identically to before, and host blocks of 1, 37 and 4096 samples giving the same output to the bit.

## References

- LennarDigital, *Sylenth1 User Manual* v2.5 (2015), pp. 5, 7, 8, 19–23, 30.
- LennarDigital, [Sylenth1 changelog](https://lennardigital.com/sylenth1/whatsnew.php) (v3.000–v3.075). v1.1 notes ([mirror](https://www.svartling.net/2006/11/lennardigital-sylenth1-v11-pc.html), 2006). KVR news for [v1.20](https://www.kvraudio.com/news/lennardigital_releases_sylenth1_v1_20_6809) and [v1.21](https://www.kvraudio.com/news/lennardigital_updates_sylenth1_to_v1_21_7065) (2007).
- Lennard (the developer) on KVR: [p=4006903](https://www.kvraudio.com/forum/viewtopic.php?p=4006903) and [p=4008220](https://www.kvraudio.com/forum/viewtopic.php?p=4008220) (2010, pitch amounts); [p=4239542](https://www.kvraudio.com/forum/viewtopic.php?p=4239542) (2010, a negative amount); thread [t=461365](https://www.kvraudio.com/forum/viewtopic.php?t=461365) (2016, additive modulation, Mix's scale, keytrack).
- P. I. Nerseth, [FXPSylenth1Preset.bt](https://github.com/perivar/AudioVSTToolbox/blob/master/PresetConverter/Assorted/FXPSylenth1Preset.bt), an 010 Editor template for Sylenth1 preset files (revision 1.3). Used only for the names of the destinations and sources.
- [KVR product page](https://www.kvraudio.com/product/sylenth1-by-lennardigital) ("16 Modulation slots with 24 different selectable modulation parameters each").
- Tutorials: [Attack Magazine](https://www.attackmagazine.com/technique/synth-secrets/pitched-percussion/) (2015), [Audiotent](https://www.audiotent.com/blogs/production-tips/5-things-you-need-to-know-about-sylenth1). Users on KVR, cited with permalinks in the inventory.
