# The voice: parts, filter routing, and the level into the filter

A Sylenth1 voice is two **parts**, A and B, each with two oscillators, a filter and an amp envelope (inventory [§1](../reference/sylenth1-features.md#1-architecture-and-signal-flow-pp-89)). Each filter's **Input Select** chooses what it filters: its own part, both parts (AB), or nothing (`--`). The two parts then go to the mixer (Mix A, Mix B), the master effects, and Main Volume. Everything up to the mixer is per voice. This page covers M1.4 task 1:

- the theory: what a voice composes, where an amplifier can sit relative to a saturating filter, how sums of oscillators grow, gain staging into a saturator, buffer layouts and in-place processing, chunked and stage-major schedules, a mono path, and where an oversampling boundary can sit;
- what is known about Sylenth1's voice, including evidence the inventory didn't have: the developer's 2015 answer on which envelopes act when a filter takes the other part, a user's report that an unrouted part is silent even with its filter on Bypass, the Input Select's options, the Mix's internal scale, and velocity in the init patch;
- the candidate structures prototyped here, on the real DSP classes;
- what they measured: each reading of the routing, the level each kind of patch presents to the filter and the Drive at which the filter's saturation reaches 1%, the knee against Volume, the engines' equality to the bit at every block size, memory, and CPU;
- the recommendations, the project owner's decision (every recommendation), and `dsp::Voice` as built, with the tests that pin it.

Read [filters.md](filters.md) (the filter, its saturators, and its 32-sample grid), [filter-control.md](filter-control.md#how-a-voice-uses-it-m14) (how a voice feeds each filter), [oscillator-unison.md](oscillator-unison.md) (the stack's level and headroom), and [envelopes.md](envelopes.md) first.

## Theory

### What a voice composes

A voice is the per-note copy of the synthesis path. Here it holds four `OscillatorSlot`s (each up to 8 unison voices, stereo), two `LadderFilter`s (stereo), two `Adsr`s (the amp envelopes), and one `PitchDrift` shared by the four slots (decided 2026-11-14). Two mod envelopes and two LFOs per voice joined it in M1.5 task 3 (built 2026-11-23, [modulation-sources.md](modulation-sources.md#as-built)), with the modulation routes compiled per voice; they run only while a route reads them. Global state lives outside it: the knob ramps (the parameter layer, M1.9), the master effects, and Main Volume.

The voice's work for a block is a fixed order: render the oscillators, compute the envelopes, build each filter's input, run the filters, apply the amp envelopes, add the result to the output. The questions are what each filter's input and output connect to, where the gains sit, and how to lay that out in memory and time.

### Routing as a gain matrix

With two parts and two filters, each filter's input is a weighted sum of the two parts: x_A = a_AA · p_A + a_AB · p_B, and likewise for B. The Input Select picks a row of weights: own part (1, 0), AB (g, g), none (0, 0). The weights g are the **summing law**:

- **Plain sum** (g = 1). Two identical, in-phase parts give +6.02 dB. Two unrelated parts add in power: +3.01 dB.
- **Halved** (g = 0.5). AB of two identical parts is as loud as one. Uncorrelated parts come out 3 dB quieter than one.
- **Equal power** (g = 1/√2). AB of two uncorrelated parts has one part's power. In phase, +3 dB.

On a linear filter, a scale on the input only changes the output level. On a **saturating** filter it changes the sound, because the saturators see the scaled level.

### Where the amplifier sits

An amp envelope is a gain, a VCA. Before a linear filter or after it makes almost no difference: a slowly varying gain nearly commutes with a linear filter (exactly, if it is constant). Before a **saturating** filter it makes a large difference:

- **After the filter** (VCF → VCA, the classic layout): the filter always sees the oscillators at full level. Its saturation, and anything resonating, are the same whatever the envelope does, and the envelope only scales the result.
- **Before the filter** (VCA → VCF): as the envelope falls, the filter is driven less, so the tone gets cleaner as the note decays. A self-oscillating filter keeps ringing after the VCA closes.

When a filter takes the *other* part, the other part's envelope can sit in either place, or nowhere.

### Gain staging into a saturator

A saturator is linear for small inputs and compresses large ones. tanh(x) ≈ x − x³/3, so a sine of amplitude a gets a third harmonic of about a²/12 of the fundamental: 1% at a ≈ 0.35. Where the oscillators' level sits against that knee decides where Drive "bites". If one oscillator at full Volume already sits past the knee, every patch is distorted at Drive 0, and a unison stack, 4–30 dB louder, is distorted a lot. The level into the filter is therefore a voice question, not only a mixer one. The mixer comes after the filter and can't undo it.

In a transistor ladder the knee also depends on the cutoff. Each stage integrates the difference t(y₍ᵢ₋₁₎) − t(yᵢ), so far below the cutoff every stage follows the one before and the saturations largely cancel. The distortion comes from the part of the signal near and above the cutoff. Measured on ours with stage 2's tests: a 220 Hz sine at a peak of 2 is distorted 0.23% with the cutoff at 0.495 fs, against several percent at the default 4,677 Hz. So a knee is only comparable at a stated cutoff, and V8's protocol fixes it at the defaults.

The level of a sum depends on correlation. N copies in phase have N times the peak (+20 log₁₀ N dB). N detuned or free-running copies have √N times the RMS, with peaks somewhere between √N and N times one copy's, depending on how the phases line up.

### Buffer layouts

A filter processes its buffer in place. If filter A filters part A's buffer in place, part A's oscillators are gone for any other consumer. That's fine unless filter B takes AB. Two layouts follow:

- **Per-part buffers**: render each part into its own buffer, and filter in place there whenever no other filter needs that part. Otherwise build the filter's input in a scratch buffer. This saves a copy in the common case (A into A, B into B), at the cost of an ordering rule.
- **Per-filter buffers**: always build each filter's input in its own buffer from the parts. That is one copy (or one sum) per filter per sample, and has no ordering rule.

### Chunked and stage-major schedules

The filter must be fed at each of the voice's 32-sample grid points, counted from its note-on (filter-control.md). The other stages don't need the grid. Two schedules:

- **Chunked (grid)**: every stage runs on chunks that stop at the voice's grid, at most 32 samples. Scratch is a fixed 32 samples per buffer, with nothing to size, so it suits STM32. There is one call per stage per chunk.
- **Stage-major (block)**: each stage runs over the whole piece of the host block (between events), and only the filter loop stops at grid points. Scratch is the maximum block size per buffer, sized in `prepare()`. Fewer calls, more memory, and the working set leaves L1 at large blocks.

If every stage is independent of how its work is split, the two give the same output to the bit. The slots, `Adsr`, `PitchDrift`, and `LadderFilter` are each tested for that.

### A mono path

The slot's output is stereo: each unison voice is panned. If every voice of every slot sits exactly at the centre, left and right are the same to the bit (`panGains(0)` is sin(π/4) on both sides), and so is everything downstream. A voice can then filter one channel and copy it. The filter is the expensive part, so this nearly halves a centred patch. It stays exact only while the patch stays centred. Leaving mono mid-note (a pan change, Pan modulation) needs the filter's channel-1 state set from channel 0's at that sample, which is exact because they were equal until then. Returning to mono mid-note isn't exact.

### Where an oversampling boundary can sit

Oversampling a nonlinearity by N means running it at N times the rate between an upsampler and a decimator. M1.3 decided that 2× and 4× apply **over the whole voice** (filters.md; M1.8). The oscillators then render directly at N× (they are band-limited at any rate), so no upsampler is needed. What's left is where the decimator sits:

- **Per voice**: each voice decimates its own stereo output. That is 16 decimators at 16 voices.
- **On the summed voice bus**: voices add at N× and one decimator runs before the master effects. Decimation is linear, so this is the same up to rounding, at a sixteenth of the cost, but everything per voice that follows the filter (the amp envelope, Mix, its modulation) runs at N×.

## Sylenth1: what is known

The full source table with ratings is in the inventory: [§1](../reference/sylenth1-features.md#voice-structure-and-routing) (V1–V13), [§3](../reference/sylenth1-features.md#3-filters-pp-5-1618) (Input Select), [§4](../reference/sylenth1-features.md#envelopes) (E13, E14), [§5](../reference/sylenth1-features.md#5-mixer-p-18) and [§7](../reference/sylenth1-features.md#7-voicing-and-keyboard-pp-1112-29). In short:

- **The manual.**
  - p. 8: "Each line represents a stereo audio signal. The outputs of the two oscillators on each part are added together, and run into the filter sections afterwards. It is also possible to run the output of the oscillators from part A into the filter of part B, as indicated by the dashed crossing lines. The outputs of the filters are then shaped by the amplitude envelope generators and enter the mixer afterwards."
  - p. 16: "Usually the output of the oscillators from Part A will be filtered by Filter A, but it is also possible to route the outputs of oscillators B to Filter A, and vice versa… If it is set to A, only oscillators A1 and A2 will be filtered. If set to AB, all oscillators from part A and B will be routed to the filter. If set to --, this means the filter does not have any input."
  - p. 16: "Bypass – This setting bypasses the filter." p. 30: bypass makes filters "stop processing".
  - p. 18: "The sounds produced by the oscillators and filters from Part A and B are mixed together using the Mixer panel. Mix A sets the volume of Part A".
  - p. 6: Sylenth1 "will also automatically turn off any parts that are not used". p. 30: "For every polyphonic note, almost a whole copy of the audio path is made (except for the master effects)".
- **The developer (KVR).**
  - 10 Dec 2015, on routing both parts through filter A: "Routing part A and B both through filter A, will make osc B route through amp env B, through filter A through amp env A, so both envelopes." "You can set attack to 0 and release to max if you want to 'bypass' amp env B." The routing exists "so that you still have amplitude control over both oscillators while only using 1 filter". ([t=452171, p=6323764](https://www.kvraudio.com/forum/viewtopic.php?p=6323764))
  - 3 and 6 Jan 2016: "Set filter A input to A and set filter B input also to A"; then, told that filter B offers "only none, B or AB": "Set filter B to input AB… You're not using oscs B anyway", and "If you want to run both parts through both filters, just set both filters to input AB." ([p=6345725](https://www.kvraudio.com/forum/viewtopic.php?p=6345725), [p=6349083](https://www.kvraudio.com/forum/viewtopic.php?p=6349083))
  - 2016, on the Mix knobs: they are part volumes before the effects ("Yes and yes"). They "internally scale from 0 to +15" and are "displayed as 0 to 10 just because that's a more intuitive scale", "a leftover from an early version"; "Other parameters like Volume A/B are using the correct scale of 0 to 10"; "all modulation in Sylenth1 is additive"; "A negative volume simply inverts the waveform". ([t=461365, p=6443419](https://www.kvraudio.com/forum/viewtopic.php?p=6443419), [p=6589750](https://www.kvraudio.com/forum/viewtopic.php?p=6589750), [p=6593444](https://www.kvraudio.com/forum/viewtopic.php?p=6593444))
- **Users.**
  - 3rdCultureKid, 6 Dec 2015 (v3.0.0.9): "I can have Filter B set to bypass but unless I have Part B routed through it I get no sound from Part B." ([p=6320383](https://www.kvraudio.com/forum/viewtopic.php?p=6320383)) The developer answers the thread without contradicting it.
  - fluffy_little_something, 6 Jan 2016: filter B can't be set to "A only, only to none, B or AB" ([p=6348734](https://www.kvraudio.com/forum/viewtopic.php?p=6348734)). MusicRadar (2015): each filter "by default is fed only the output of the two oscillators in that Part"; "choose AB or BA. To kill the input into the filter, select None"; "The output of each filter routes to its Part's Amp Envelope".
  - Velocity in the init patch: "I do not seem to be able to get any velocity changes", answered "You need to assign velocity to something first… Use the Misc 1 section" (KVR t=329806, 2011). The same advice appears in 2014 (t=423612) and 2017 (t=481791: no velocity amount on the envelopes).
- **Unknown.** Whether the crossing part's envelope acts before the filter or only on its contribution; which Mix acts on a crossing part; how AB sums; the absolute level an oscillator presents to the filter (O5); the init patch's oscillator and mixer settings; the amp envelope's gain law (E4).

So, rated in the inventory:

- **A filter taking the other part applies both envelopes to it** (the developer, explicitly: high). The order he gives, "osc B → amp env B → filter A → amp env A", puts amp env B *before* filter A on that path, and only amp env A on part A's own path.
- **A part no filter selects is silent** (medium). One user's report with the developer answering the thread, and his framing that the routing exists so the oscillators keep amplitude control "while only using 1 filter". The same report says **Bypass still obeys the Input Select**: a bypassed filter on the wrong input passes nothing (medium).
- **Each filter offers its own part, AB, or `--`.** Filter B has no "A" (medium: one user, uncontradicted by the developer, who then suggests AB; MusicRadar's "AB or BA… None"). The manual's "and vice versa" is then only through AB.
- **AB is a sum.** Nothing says it is scaled (low–medium: within a part the oscillators "are added together", and AB routes "all oscillators from part A and B"; no source on a scale).
- **Velocity doesn't reach the amp in the init patch** (medium: three user threads). Nothing describes the amp envelope's gain law.
- The filter is **stereo per voice** (high: p. 8, and the developer's "full stereo" in 2006).

## The candidates

In [voice_candidates.hpp](../../tests/dsp/voice/voice_candidates.hpp), one `PrototypeVoice` on the real classes. Each filter is fed through the Filter Control at its own grid points by the decided feed in [filter_control_support.hpp](../../tests/dsp/filters/filter_control_support.hpp) (`feedFilter`, refactored out of the stand-in so both use it), reading the global knob ramps (`KnobRamps`) ahead with `valueAfter`. The patch's four slots render at the voice's note plus drift, in drift's runs.

**Readings** (what Sylenth1 does). The voice produces each of these from flags, so the report can render them side by side:

| | Reading | What it does |
|---|---|---|
| Q1 | **I1** filter's part's envelope only | filter A's output × env A; part B's oscillators enter filter A as they are |
| | **I2** source's envelope, then the filter's | part B's oscillators × env B, then filter A, then × env A (the developer's sentence, literally); part A's own path × env A only |
| | **U1** unselected silent / **U2** unselected dry | a part no filter reads is not heard / is heard unfiltered through its own envelope |
| | **Bypass obeys the select** / **passes its own part** | a bypassed filter passes its Input Select's sum unfiltered / its own part whatever the select says |
| Q2 | **AB law**: plain sum / 0.5 / 1/√2 | the weights g on each part when the input is AB; own-part input always has weight 1 |

**Engines** (how to build it). They must agree to the bit:

| Engine | What it does |
|---|---|
| **Per-part** layout | each filter in place on its own part's buffer where the other filter doesn't read that part, otherwise into a scratch buffer; in-place inputs built last |
| **Per-filter** layout | every filter's input built in its own buffer |
| **Grid** schedule | every stage in chunks that stop at the voice's grid: fixed scratch of 32 samples × 10 buffers |
| **Block** schedule | each stage over the whole piece of the host block, the filter loop split at the grid: scratch of maxBlockSize × 10, sized in `prepare()` |
| **Mono path** | decided at the note-on: if every sounding unison voice's gains are equal left and right to the bit, the inputs, the filters, and the envelope multiply run one channel, written to both |
| **Skip unheard** | a part that nothing hears isn't rendered (task 5, measured only) |

The render order in every engine: clear the part buffers; the four slots added into their part's buffer in drift's runs; both envelopes' levels; each filter's input (the select's weights, the crossing part through its envelope under I2); the filters in place, each grid point fed for the end of the coming segment; each filter's output × its part's envelope added to the voice's stereo output. Mix A/B, velocity, and the voice's output bus are task 4's and M1.5's. Here they are at unity, and the voice adds into one stereo bus.

Two patches are used throughout. **Init** is the evidence's guess (V9): A1 one saw at Volume 10, the rest off, both filters LP24 on their own parts at the Filter Control's defaults (4,677 Hz, k 0, drive ×1), Warm on, both envelopes a gate. **Worst** is 4 oscillators × 8 unison saws (Detune 5, Stereo 10, free-running, B a fifth up), both filters on AB, drive ×4, Warm on.

## What they measured

By the hidden `[voice-report]` ([voice_report.cpp](../../tests/dsp/voice/voice_report.cpp), about 8 s) and the `[voice]` benchmarks ([voice_benchmark.cpp](../../tests/dsp/benchmarks/voice_benchmark.cpp)), on the prototypes and the library's classes as built:

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[voice-report]"
tools/bench.sh "[voice]"
```

### Question 1: what each reading does

Part B only (B1 a saw at 110 Hz). The output's RMS early (10–50 ms) and late (200–300 ms):

| Patch | Reading | Early | Late |
|---|---|---:|---:|
| A: AB, B: `--`, env B decays to 0 in 100 ms | I1 | −8.1 dB | −8.1 dB |
| | **I2 (the developer)** | −21.8 dB | **silent (exact 0)** |
| A: A, B: `--` (part B unselected) | **U1** | **silent** | **silent** |
| | U2 | −7.9 dB | −7.8 dB |
| A: A, B: `--` and Bypass | **obeys the select** | **silent** | **silent** |
| | passes its own part | −7.9 dB | −7.8 dB |
| A: A, B: B and Bypass | either | −7.9 dB | −7.8 dB |
| A: `--`, B: `--` | U1 / U2 | silent / −7.9 dB | silent / −7.8 dB |

The readings separate cleanly, so each has a demo protocol that a single listen settles (V2, V4, V5). Under I2 a part whose envelope has closed contributes exactly nothing to the other filter. Under I1 it keeps sounding through filter A at full level after its own envelope ends, which is what the 2015 poster expected and the developer explained is not the case. A `--` filter produces exact zeros, and skipping it is exact as long as it starts from rest.

### Question 2: the level into the filter

Saws at A2 (110 Hz), Volume 10 (unity), centred (each channel 0.7071 of the mono sum), 48 kHz. *In phase*: Retrig on, Phase 0, Detune 0. *Free*: Retrig off (seeded random phases), Detune 5 (0.548 semitones at 8 voices), the oscillators 0, +7, −5, +3 cents. Peak and RMS of one channel over 1 s. Then the filter at the Filter Control's defaults (LP24, 4,677 Hz, k 0): its **distortion at drive ×1**, the RMS of the output less the linear filter's (the same filter at −60 dB, scaled back) over the linear output's RMS, and **the drive at which that reaches 1%** (−40 dB), with the Drive knob under F8's placeholder (0 to +24 dB):

| Input | AB law | Phase | Peak | RMS | Warm on: at ×1 | 1% at | Warm off: at ×1 | 1% at |
|---|---|---|---:|---:|---:|---:|---:|---:|
| 1 saw | | | 1.01 (+0.1 dB) | 0.408 (−7.8 dB) | 4.75% | −7.2 dB | 2.47% | −3.8 dB |
| 2 saws in a part | | in phase | 2.03 (+6.1 dB) | 0.815 | 15.0% | −13.2 dB | 11.0% | −9.8 dB |
| | | free | 1.98 (+5.9 dB) | 0.678 | 12.2% | −11.8 dB | 8.5% | −8.4 dB |
| AB, 1 saw per part | **plain sum** | in phase | 2.03 (+6.1 dB) | 0.815 | 15.0% | −13.2 dB | 11.0% | −9.8 dB |
| | | free | 1.52 (+3.6 dB) | 0.516 | 9.7% | −10.6 dB | 6.0% | −7.3 dB |
| | 0.5 | in phase | 1.01 (+0.1 dB) | 0.408 | 4.75% | −7.2 dB | 2.47% | −3.8 dB |
| | | free | 0.76 (−2.4 dB) | 0.258 | 2.8% | −4.6 dB | 1.35% | −1.3 dB |
| | 1/√2 | in phase | 1.43 (+3.1 dB) | 0.577 | 8.7% | −10.2 dB | 5.2% | −6.8 dB |
| | | free | 1.07 (+0.6 dB) | 0.365 | 5.3% | −7.6 dB | 2.8% | −4.3 dB |
| AB, 2 saws per part | **plain sum** | in phase | 4.05 (+12.2 dB) | 1.631 | 35.3% | −19.2 dB | 37.9% | −15.9 dB |
| | | free | 2.75 (+8.8 dB) | 0.977 | 23.9% | −15.4 dB | 23.5% | −12.1 dB |
| | 0.5 / 1/√2 | free | 1.37 / 1.94 | 0.488 / 0.691 | 8.2% / 14.7% | −9.4 / −12.4 dB | 4.7% / 10.9% | −6.1 / −9.1 dB |
| 8-voice unison | | in phase | 8.10 (+18.2 dB) | 3.261 | 58.3% | −25.2 dB | 65.4% | −21.9 dB |
| | | free | 4.35 (+12.8 dB) | 1.155 | 27.7% | −16.7 dB | 25.2% | −13.4 dB |
| AB, 4 osc × 8 unison | **plain sum** | in phase | 32.4 (+30.2 dB) | 13.05 | 87.4% | below −36 dB | 90.8% | −33.9 dB |
| | | free | 9.19 (+19.3 dB) | 2.524 | 57.7% | −25.2 dB | 64.4% | −21.9 dB |
| | 0.5 / 1/√2 | free | 4.59 / 6.50 | 1.262 / 1.785 | 35.9% / 46.7% | −19.1 / −22.1 dB | 39.6% / 52.8% | −15.9 / −18.9 dB |

(The full table, with every law in phase and free, is in the report's output.)

- **One oscillator at Volume 10 is already past the knee.** Its band-limited saw peaks at 1.44, centred 1.01 per channel. At drive ×1 the filter distorts it by 4.75% with Warm on and 2.47% off, and the filter stays under 1% only with the input 7.2 dB (Warm off: 3.8 dB) lower. Under F8's placeholder, where the Drive knob runs from 0 to +24 dB, **every row bites before the knob's bottom**: the Drive knob only ever adds to a saturation that is already there.
- **A free 8-voice unison is 28% distorted at drive ×1, and the worst case 58%.** The stack's +12.8 dB (free; +18.2 in phase) goes straight into the saturators, because unison has no normalisation (O15).
- **The summing law moves AB by its gain**, at most 6 dB: plain sum puts AB of one saw per part exactly where two saws in one part are (in phase, bit for bit), 0.5 puts it where one saw is, and 1/√2 lands between. Free, the plain sum is +3.6 dB over one saw, not +6, because the phases don't line up.
- **Warm off is cleaner at moderate levels and harder at extreme ones.** The cubic is closer to linear near 0 and clips at |x| = 1.5, so it distorts more than tanh once the peaks pass about 4 (4 saws in phase, 8 unison in phase, the free worst case), and about as much on a free 8-voice unison (25% against 28%).

### The knee against Volume

A1 a sine at 220 Hz, one voice, centred, through the same filter at drive ×1. THD (harmonics 2–10) of the output, Blackman-Harris over 2¹⁵ samples:

| Volume knob | Peak at the filter | Warm on | Warm off |
|---:|---:|---:|---:|
| 1.0 | 0.071 | 0.022% | 0.010% |
| 2.5 | 0.177 | 0.140% | 0.062% |
| 5.0 | 0.354 | 0.570% | 0.257% |
| 7.5 | 0.530 | 1.325% | 0.610% |
| 10.0 | 0.707 | 2.461% | 1.174% |

1% THD at **Volume 6.56** with Warm on (−3.7 dB re Volume 10) and **9.34** off (−0.6 dB). The THD follows a² (×4 per doubling of Volume) as tanh's series says, so the knee is one number per saturator. This is a measurement that can be repeated on Sylenth1: the Volume at which the same patch reaches 1% there, against ours, is the scale between its internal level and ours (V8), whatever the oscillators' absolute level, if the saturators share a shape (F9). It is the demo measurement that settles question 2.

### The engines: the same output, no allocation

Six patches (init; worst; worst centred; worst with A: AB, B: `--`; with A: A, B: B; with A: A, B: AB), each under I1 and I2, rendered by all 8 engines (per-part or per-filter, grid or block, with or without the mono path) at host blocks of 1, 37 and 4096. Each render is 500 ms with a note-off at 300 ms + 7 samples (off the grid and off every block boundary), compared with per-part, grid, block 4096:

| | Renders | Identical to the bit | Allocations | Mono path taken | Left = right |
|---|---:|---:|---:|---|---|
| each of the 12 patch × reading rows | 24 | 24 | 0 | init and worst centred | yes, to the bit |

All 288 renders are identical to their reference to the bit, and none allocates after `prepare()`. The mono path is taken exactly where every voice is centred, and there left equals right to the bit. The block split at note events, with the voice's grid counted from its note-on, makes the output independent of the host's block size, as the stages' own tests predicted.

### Memory per voice

| Engine | The voice object | Scratch at maxBlockSize 512 | at 4096 |
|---|---:|---:|---:|
| grid (32-sample chunks) | 23,464 B | 1,280 B | 1,280 B |
| block (whole pieces) | 23,464 B | 20,480 B | 163,840 B |

Of the voice object, the four slots are 22,240 B (5.6 KB each, 8 unison oscillators with their minBLEP rings), the two filters 720 B, the envelopes 224 B, and the drift 88 B. At 16 voices and blocks of 4096, the block schedule's scratch is 2.6 MB against the grid's 20 KB.

### CPU

`tools/bench.sh "[voice]"` (`relwithdebinfo`, Apple M1 Ultra, virtualised; two runs, the second shown, the first within 3%). One voice with its note held at the sustain, 512 samples per run at 48 kHz:

| Benchmark | ns per sample per voice | 16 voices, share of a core |
|---|---:|---:|
| Init patch, per-part, grid, stereo (filter B running on part B's silence) | 361 | 28% |
| Init patch, filter B at `--` (not run) | 235 | 18% |
| Init patch, mono path | 186 | 14% |
| Init patch, per-filter, grid | 358 | |
| Init patch, per-part, block | 361 | |
| Worst case, per-part, grid | 691 | 53% |
| Worst case, per-filter, grid | 689 | |
| Worst case, per-part, block | 682 | |
| Worst case, per-filter, block | 681 | |
| Worst case centred (Stereo 0), stereo path | 703 | |
| Worst case centred, mono path | 410 | 32% |
| Init patch at 96 kHz (1024 samples per 512 out) | 667 (per output sample) | 51% |

- **The layouts and schedules are within noise of each other** (−1.5% to +0.3% between them, against 3% between runs). The filter dominates: at 95–125 ns per channel-sample it is four channels per voice, against about 3 ns per saw. The copy the per-part layout saves is lost in that.
- **A filter on silence still costs more than half a sounding one**: 126 ns per sample for its two channels, against about 225 for filter A sounding, and 35% of the init voice. The Newton solve runs whatever the input. Running filter B at the init patch, where part B has no oscillators, is the largest single saving task 5 can make. "Unused parts are switched off" (p. 6) must include the filter, not only the oscillators, which already cost nothing at 0 voices.
- **The mono path saves 49% at the init patch and 42% at the worst case centred**, the share of the filters' second channel. It is exact (above).
- **The voice at 2×** costs 1.85 times the voice at 1× per output sample, before any decimator. The filter's per-grid work is amortised over twice the samples.

## Recommendations

*Stage 1's recommendations, as written. The project owner chose every one ([The decision](#the-decision)), and stage 2 built them ([As built](#as-built)).*

| # | Question | Recommendation | Why |
|---|---|---|---|
| 1a | **Which envelopes act on a part crossing into the other filter** | **I2: the source part's amp envelope before the filter, the filter's part's after.** Part B into filter A: env A · F_A(p_A + env B · p_B). Own paths: F_X then env X. | The developer's own words (high); I1 contradicts them (measured above: under I1 part B sounds on at full level after its envelope closes). Applying env B to B's contribution *after* filter A instead isn't possible through one shared saturating filter, which doesn't split into contributions, so "through amp env B, through filter A" fixes the order. V2's second step confirms it by ear: with filter A driven, B's timbre cleans up as env B falls. |
| 1b | **What a `--` filter outputs; unselected oscillators** | **`--`: nothing, and the filter isn't run. Unselected oscillators are silent (U1)**, and not rendered (task 5). | The manual ("does not have any input"); the 2015 report (medium). Exact: a filter from rest on zeros outputs zeros. A filter switched to `--` while ringing stops at once in this reading; V6 settles whether Sylenth1 lets it ring out. |
| 1c | **Input Select under Bypass** | **Bypass passes the Input Select's sum unfiltered** (and the crossing part through its envelope, as 1a). | The 2015 report: Filter B on Bypass, no sound from Part B unless routed (medium). |
| 1d | **Input Select's options** | **Per filter: `--`, its own part, AB.** As data, an enum of three values. | One user and MusicRadar; the developer suggesting AB where "A" was missing (medium). V1 lists them on the demo. If the demo has "B" on filter A, it is one more row of weights. |
| 2a | **How AB sums** | **A plain sum (g = 1)**, the same as the oscillators within a part. | The manual's "added together" for a part, and "all oscillators from part A and B will be routed to the filter". Nothing suggests a scale. AB of one saw per part is then exactly two saws in one part, the simplest reading. V7 settles it at the bypass in one level measurement (+6.02 / 0 / +3.01 dB). |
| 2b | **The level into the filter** | **Keep today's chain for now**: Volume 10 is unity, centred is −3 dB per channel (O4, O6), the parts add (2a), so one saw presents a peak of 1.01 and an RMS of 0.41 per channel. **Add one named constant to the voice, the filter's input level** (a gain before each filter and its inverse after, so the clean level is unchanged and only the knee moves), **1 until V8 is measured.** | Measured: at this level the filter's saturation bites below the Drive knob's bottom for every patch (one saw 4.75% at drive ×1; a free unison 28%). That may be Sylenth1's sound ("will add harmonic distortion when the signal levels are high", the developer, 2007) or a level ours puts 4–7 dB too hot. Only the demo tells: V8 compares the Volume at 1% THD on a sine (ours: 6.56 Warm on, 9.34 off). A gain before and its inverse after is the one change that moves where drive bites without touching O4/O5, F8, or the mixer. Its cost is one multiply per channel-sample per filter. It also scales the self-oscillation level (F7) by 1/g, so F7's −12 dBFS is re-measured with it. |
| 3 | **The amp envelope** | **A linear gain of the `Adsr`'s level**, applied after the filter (and, under 1a, before it on a crossing part). **Velocity doesn't reach the amp**; it is a misc source (M1.5). | No source gives a law. E4's protocol (sustain 5 is −6.02 dB if linear) settles the envelope and the gain together (E13). Three user threads: velocity does nothing in the init patch (E14). |
| 4 | **Stereo and the mono path** (task 5's stage 1 re-measured it: [part-solo-and-skipping.md](part-solo-and-skipping.md#the-mono-path); built 2026-11-22: [as built](part-solo-and-skipping.md#as-built)) | **The voice is stereo, and the mono path goes into task 5** with the skipping: decided at the note-on, left as soon as any gain stops being centred, by setting the filters' channel-1 state from channel 0's (a small `LadderFilter` addition), never re-entered within the note. | It is exact (288 of 288 renders) and saves 42–49% for centred patches, which include the init patch and most basses. It is a skip of duplicated work, like task 5's unused parts, and needs the voice's structure (a channel count per note), so Phase 3 is late for it. The alternative is Phase 3, with the measurement kept. |
| 5 | **The voice's structure** | **Per-filter buffers, the grid schedule, fixed scratch (`std::array`, 32 samples × 10 buffers, 1.3 KB), no allocation anywhere.** Render order: oscillators into their parts (drift's runs) → both envelopes → each filter's input → the filters, fed at each grid point for the segment's end → × each part's envelope → the output. The caller splits its blocks at note events; the voice's grid counts from its note-on. | Every engine gives the same output, and their CPU is within noise. So choose on simplicity and memory. Per-filter has no ordering rule (per-part's in-place inputs must be built last, a rule that is easy to break when Phase 2 adds routings). The grid schedule needs nothing sized in `prepare()`, takes 1.3 KB instead of 20–164 KB per voice (STM32), and is the chunking the filter needs anyway. |
| 6 | **Oversampling** | **Record only (M1.8):** the whole voice at N× (decided in M1.3), its oscillators rendering directly at N×, **one decimator on the summed voice bus** before the master effects, not one per voice. The voice's Mix and envelopes then run at N×. | Decimation is linear, so one on the sum is the same up to rounding at a sixteenth of the cost. The voice at 2× costs 1.85× per output sample. The developer's "already oversamples several parts internally by 2 or 4x" is most likely the effects (F10). |

### What this pushes back on

- **O5 (Volume's absolute level)** gets a sharper protocol, V8: the knee against Volume, which doesn't need the absolute output level at all and isn't affected by the mixer's or the effects' levels.
- **F8 (Drive)**: under its placeholder (0 to +24 dB) and today's level, the knob's bottom is already past the knee for every patch. If V8 finds Sylenth1's knee higher, the input level (2b) moves, and F8 stands. If V8 finds it the same, Sylenth1's filter is never clean on a full-level saw, and F8 also stands.
- **O15 (unison level)**: no normalisation means a free 8-voice unison enters the filter at +12.8 dB peak over one saw, 28% distorted at drive ×1. O15's measurement on the demo (RMS against the voice count) gains weight: if Sylenth1 normalises, this changes a lot.
- **The inventory's "A, B, AB, or `--`"** for each filter becomes "its own part, AB, or `--`" (V1), and "vice versa" means through AB.
- **E11** (whether a held note at sustain 0 frees its voice): untouched, for task 2.

## The decision

Chosen by the project owner on 2026-11-21: every recommendation above (Decisions log).

- **Routing**: each filter's Input Select is its own part, AB, or `--`. **AB is a plain sum.**
- **Envelopes**: a part crossing into the other filter goes through **its own amp envelope before that filter**, and **the filter's part's envelope after**. A filter's own part goes through the filter, then its envelope.
- **What isn't heard isn't run**: a `--` filter passes nothing and isn't run. A part no filter reads is silent and its oscillators aren't rendered. **A bypassed filter still takes its Input Select**, and passes that sum unfiltered.
- **The level**: unchanged. Volume 10 is unity, centred is −3 dB per channel, the parts add. **One filter input level** (a gain before each filter, its inverse after) **at 1 until V8 is measured.**
- **The amp envelope** is a linear gain of the `Adsr`'s level. **Velocity doesn't reach the amp** (it is a misc source, M1.5).
- **The mono path goes to task 5**, with a filter channel-state copy to leave it mid-note.
- **Structure**: per-filter buffers, the 32-sample grid schedule, fixed scratch, no allocation.
- **Oversampling**: recorded for M1.8, as one decimator on the summed voice bus.

## As built

### The class

[voice.hpp](../../libs/dsp/include/anoesis/dsp/voice/voice.hpp): `Voice<Taps>`, `FilterInput` (`none`, `own`, `both`), and `VoiceFilterKnobs`. The knobs struct holds the six global knob ramps as `const LinearSmoother*`, positioned at the block's first sample (null reads as 0), and the Keytrack knob.

- **What it holds:** 4 `OscillatorSlot`s (`slot(0..3)`: A1, A2, B1, B2), 2 `LadderFilter`s, 2 `Adsr`s (`envelope(0..1)`), and a `PitchDrift`.
- **Seeds:** the constructor's seed sets every random sequence: slot s gets seed ⊕ 0x9E37…·(s + 1), and the drift seed ⊕ 0x3C6E…. Give each voice its own seed.
- **The filters' controls are the voice's:** `setFilterInput`, `setFilterBypassed`, `setFilterResponse`, `setFilterDrive`, `setWarm`, and `setFilterInputLevel`. A level outside [10⁻³, 10³], or NaN, counts as 1. `filter(part)` is read-only, for measurement.
- **Lifecycle:** `prepare()`, `reset()` (as freshly prepared), `noteOn(note, knobs)`, `noteOff()`, `stop()`, `isActive()`, and `process(out, knobs)`, which adds into channels 0 and 1. Changed in M1.4 task 2 ([voice-allocation.md](voice-allocation.md#as-built-the-voices-part)): a note-on on a sounding voice keeps the filters and the grid; `stop()` silences a finished voice without restarting its random sequences; `isActive()` is true only while a filter with oscillators in its input has its part's envelope running (it was: until both amp envelopes reach 0).
- **Size:** about 25 KB at 64 taps, with the scratch inside (10 buffers of 32 samples). Nothing allocates, `prepare()` included.

How `process()` runs:
- **Chunks:** it works in chunks that stop at the voice's grid, counted from the note-on.
- **At each grid point:**
  - pending Input Select and Bypass changes apply. A filter that comes on, from `--` or out of Bypass, is fed for this sample and `reset()`, so it starts from rest on the voice's grid.
  - each running filter is fed through the Filter Control for the end of the coming segment:
    - `filterCutoffOctaves` with keytrack (`keytrackOctaves`, computed once for both filters) goes into `setCutoffModulation`, with `setCutoff` held at the anchor;
    - `filterResonanceK` goes into `setResonanceModulation`, with `setResonance(0)`;
    - a non-finite value keeps the last finite one.
- **Per chunk:**
  1. The heard parts' slots are added into their buffers, at the note plus drift, in drift's runs.
  2. Both envelopes run.
  3. Each running filter's input is built in its own buffer: its own part, or its own part plus the other through the other's envelope, then × the input level when it isn't 1.
  4. The filter runs in place.
  5. Its output × its part's envelope (× 1/level) is added to `out`.
- **No fused multiply-add on the crossing part:** its product sits in its own statement, so it is rounded before the sum on every compiler.

Two things were settled in the build:
- **Routing changes apply at the voice's next grid point.** A filter is then always on the voice's grid. Leaving Bypass through `LadderFilter::setBypassed` mid-segment would restart the filter's own grid off the voice's, so it would read feeds meant for another segment's end.
- **The voice feeds only running filters.** A bypassed or `--` filter isn't fed, and is fed for its starting sample when it comes on.

What it leaves to later tasks:
- the per-voice modulation terms of the Filter Control (Cutoff X / AB, Reso X / AB): supplied by M1.5 task 3 (built 2026-11-23, [modulation-sources.md](modulation-sources.md#as-built)), the sums of the sources as they stand at each grid point;
- the note's glide, bend, and master tune (task 3; built 2026-11-21, [legato-portamento.md](legato-portamento.md#as-built));
- Mix and the output bus (task 4; built 2026-11-22, [mixer.md](mixer.md#as-built): each filter's output times its part's envelope and its part's Mix, per sample, into the voice's stereo output; no bus);
- skipping filters on silence, and the mono path (task 5; stage 1 measured both on the current voice, [part-solo-and-skipping.md](part-solo-and-skipping.md): a filter at rest on zeros skipped to the bit, the mono path exact in 72 of 72 renders once its output stage uses the stereo path's expression, 47.5% saved on the init patch; built 2026-11-22, [as built](part-solo-and-skipping.md#as-built): the skips planned per grid segment, a fed filter at gain 0 frozen, Solo's gains, and the mono path left with `LadderFilter::copyChannel()`);
- voice allocation (task 2). What a retrigger of a sounding voice does, `stop()`, and `isActive()` are decided and built ([voice-allocation.md](voice-allocation.md#as-built-the-voices-part)).

This completes M1.4's last bullet, the feed through the Filter Control, except for its modulation terms, which M1.5 task 3 added.

### Tests

[voice_test.cpp](../../tests/dsp/voice/voice_test.cpp), 13 tests. The stage-1 prototype, kept in [voice_candidates.hpp](../../tests/dsp/voice/voice_candidates.hpp) for the alternatives (I1, unselected dry, Bypass on its own part, the AB laws, the per-part layout, the block schedule, the mono path), is the reference in its decided configuration:

| Test | What it pins down | Tolerance and why |
|---|---|---|
| Matches the prototype | six routings (own/own, AB/`--`, own/AB, AB/AB, `--`/own, own/AB with B bypassed) on the worst patch, with a Cutoff step and a Resonance step mid-note (off the grid) and a note-off off the grid | bit for bit: the same classes, seeds, and arithmetic in the same order |
| `--` on both | silence | exact zeros |
| An unselected part | silence, its slot never run, its filter at rest | exact |
| Both envelopes on a crossing part | either envelope closing silences B1 through filter A | exact zeros from 200 ms (the filter's tail at 4,677 Hz is flushed below FLT_MIN within a few ms) |
| The crossing envelope acts before the filter | envelope B at 0.25 against 1, a 220 Hz sine at drive ×2.83 | THD below half (after the filter the ratio would be exactly 1) |
| AB a plain sum | A1 and B1 the same saw into a bypassed AB filter | exactly twice A1 alone (0 ULP) |
| Bypass takes its select | B bypassed on `--` silent; on B exactly the bare slot | exact |
| The input level | bypassed: 0.3 against 1 within 2 ULP (three half-ulp roundings); NaN, 0, −1, inf bit-identical to 1. Driven: 0.25 halves the THD or better | derived as stated |
| Block sizes 1/37/4096 | two routings, a knob ramp, keytrack, a note-off off the grid; no allocation from `prepare()` on | bit for bit; 0 allocations |
| Changes at the next grid point | an Input Select change at 2005 is the one at 2016; switched back on from `--` it starts from rest (against off from the start); leaving Bypass the same, on the grid | bit for bit |
| The Filter Control feed | Cutoff A 2, B 4, the Filter Control 3, Keytrack 10 at note 57: 4.5675 Hz × 2⁹ and × 2¹¹ after the note-on and after a block | 2⁻¹⁹ + 2⁻²⁴ relative, as filter_control_test.cpp |
| NaN from the knobs | the cutoff stays where it was; the output finite and sounding | 0 ULP |
| The lifecycle | `isActive()` through the release; `reset()` as freshly prepared; another seed differs | bit for bit |

Red against an inert stub: 11 of 13 failed. The two that passed check for silence, which an inert voice also gives. Then 20 deliberate breaks of the routing, the envelopes, the sum, the skipping, the grid deferral, the restarts, the feed, the NaN guards, the level, the drift, the seed, and reset. The first pass caught 14; 3 didn't compile as written, and 2 survived. One survivor was the NaN guard on k: the filter takes a NaN k as 0 without complaint, so the test now compares against a voice whose knobs stay finite. The other was `reset()` keeping the drift's sequence: `PitchDrift` is off unless set, so no test exercised it, and the test patches now turn it on at the plugin's 1.5 cents. With those, all 20 are caught.

### Results, as built

The `[voice-report]` adds a section for the library: `dsp::Voice` against the prototype's decided configuration, drift on, for the six engine patches at host blocks of 1, 37 and 4096. It is **identical to the bit in 18 of 18 renders, with 0 allocations from `prepare()` on**. Every stage-1 table reproduces to the printed digit.

**CPU** (`tools/bench.sh "[voice]"`, `relwithdebinfo`, Apple M1 Ultra, virtualised; per voice):

| Benchmark | ns per sample | 16 voices, share of a core |
|---|---:|---:|
| `dsp::Voice`, init patch | 358 | 27% |
| `dsp::Voice`, init patch, filter B at `--` | 229 | 18% |
| `dsp::Voice`, worst case | 688 | 53% |
| the prototype's per-filter grid rows, the same session | 360 / 697 | |

The built voice costs what the prototype's per-filter grid engine did, within noise. Task 5 has the next saving: a filter whose input has nothing sounding still costs 126–129 ns, and the mono path roughly halves centred patches. Task 5's stage 1 measured both on 16 voices ([part-solo-and-skipping.md](part-solo-and-skipping.md#cpu)): the init patch 360 ns per voice, 240 with the filter on silence skipped, 126 with the mono path too.

## References

- LennarDigital, *Sylenth1 User Manual*, v2.5, 2015, pp. 6, 8–9, 16–18, 29–30.
- The developer on KVR: [t=452171 (2015–16)](https://www.kvraudio.com/forum/viewtopic.php?t=452171), [t=461365 (2016)](https://www.kvraudio.com/forum/viewtopic.php?t=461365), [t=518048 (2019)](https://www.kvraudio.com/forum/viewtopic.php?t=518048). Users: KVR [t=329806 (2011)](https://www.kvraudio.com/forum/viewtopic.php?t=329806), [t=423612 (2014)](https://www.kvraudio.com/forum/viewtopic.php?t=423612), [t=481791 (2017)](https://www.kvraudio.com/forum/viewtopic.php?t=481791). MusicRadar, [*How to master Sylenth1's filters and amplitude envelopes*](https://www.musicradar.com/tuition/tech/how-to-master-sylenth1s-filters-and-amplitude-envelopes-630711) and [*parts and oscillators*](https://www.musicradar.com/tuition/tech/how-to-master-sylenth1s-parts-and-oscillators-630710) (2015). Every source is rated in the [feature inventory](../reference/sylenth1-features.md#voice-structure-and-routing).
- U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011, ch. 4 (nonlinear processing, the order of gain and saturation).
- [filters.md](filters.md) (the saturators and the grid), [filter-control.md](filter-control.md#how-a-voice-uses-it-m14) (the voice's feed), [oscillator-unison.md](oscillator-unison.md) (the stack's level), [oscillator-controls.md](oscillator-controls.md) (Volume and pan), [envelopes.md](envelopes.md), [oversampling.md](oversampling.md).
