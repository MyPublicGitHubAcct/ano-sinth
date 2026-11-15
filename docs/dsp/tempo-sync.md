# Tempo sync

Tempo sync makes a time-based parameter follow the host's tempo: an LFO that cycles once per bar, a delay that echoes on the dotted eighth, an arpeggiator that steps in sixteenth-note triplets. Sylenth1 has one global **Sync** switch that does this for the LFO rates, the delay times, and the arpeggiator's Time (inventory [§8](../reference/sylenth1-features.md#8-tempo-sync-pp-12-20)). This page covers:

- note values and why they should be exact fractions, not a table of floats;
- how a host describes time (tempo, position in quarter notes, transport), and what it leaves out;
- two ways to sync an LFO: free-running at the synced rate, or locked to the host's position, and the hybrids between them;
- what is known about Sylenth1's sync;
- the candidate models prototyped here, and what each one measured: the value list, exactness, drift over hours, tempo changes and ramps, loops, locates, transport start and stop, block sizes, subnormals, allocation, and CPU;
- the options for the decision.

Read [lfos.md](lfos.md) (the 64-bit phase, slewed jumps, the click metric) and [processor-lifecycle.md](processor-lifecycle.md) first.

## Theory

### Note values

A synced value is a note length. Musicians write lengths as fractions of a whole note: 1/4 is a quarter note (a crotchet), 1/8 an eighth, and 2/1 two whole notes. Two modifiers multiply them:

- **Dotted (D):** 3/2 of the length. A dotted eighth is 3/16 of a whole note.
- **Triplet (T):** 2/3 of the length, so three of them fill the time of two. A triplet eighth is 1/12, and three of them make a quarter note.

The tempo is counted in **beats per minute** (BPM). In practice a host's beat is always a quarter note, whatever its time signature, because hosts report position in quarter notes. One quarter note lasts 60 / BPM seconds, so a value of *L* whole notes lasts:

```text
seconds = 4 · L · 60 / BPM       samples = seconds · fs       hertz = 1 / seconds
```

Sylenth1 calls a whole note a "bar" ("one bar contains four beats", manual p. 12), so 8/1 is eight bars of 4/4 even when the host is in 3/4.

### Exact fractions

A note value is a rational number: a straight base (8/1 … 1/64) times 1, 2/3, or 3/2. Represented as a reduced fraction of two integers, every value and every identity between values is exact: 3 × 1/8T *is* 1/4, and 2 × 1/8D *is* 3 × 1/8. Converting to seconds, hertz, samples, or a phase increment then rounds **once**, at the end, from exact inputs.

A table of floating-point multipliers rounds earlier, and the roundings show up in two places:

- **Comparing values.** 2/3 has no exact binary representation, so 3 × (base × 0.6666667) may or may not equal the straight value, depending on the base and the tempo.
- **Long-running positions.** An arpeggiator places its *k*-th step at *k* × (samples per step). If the step length is a rounded float, the error grows with *k*; if positions are accumulated by adding a float step, they drift by whole seconds, because a float position has a resolution of 32 samples by the time it reaches 3 hours at 44.1 kHz. Counting steps as integers and computing each position from the exact fraction (⌈k · num / den⌉) has no drift at all.

### How a host describes time

At the start of every audio block, the host reports (VST3's process context, AU's host callbacks, JUCE's `AudioPlayHead`):

- the **tempo** in BPM;
- the **position** in quarter notes since the song's start (**PPQ**, pulses per quarter note: a double, not an integer count of pulses);
- whether the **transport** is playing, and often the loop points, the time signature, and the position of the last bar line.

It does **not** report what happens inside the block. A tempo that ramps during the block is reported as its value at the block's start; the next block's position reveals how far the music actually moved. A loop or a locate (the user clicking the timeline) shows up as a position that doesn't follow from the last one. When the transport is stopped, the position usually freezes, and the tempo is still reported. A standalone app has no host at all.

### Free-running against phase-locked

A synced LFO needs a rate and a phase. The rate is simple: hertz = BPM / (60 · beats per cycle). The phase is a choice:

- **Free-running at the synced rate.** The LFO starts where something starts it (a note-on, for a retriggered LFO) and accumulates its phase from the rate. It follows tempo changes, block by block, and never looks at the position. A loop or a locate doesn't touch it, so it never jumps. Its phase relative to the bar is set by when the note was played, so a note played off the beat gets an LFO that is off the beat too, as a hardware synth would.
- **Locked to the transport.** The phase is computed from the position: frac(PPQ / beats per cycle), so a 1/1 LFO is at its start on every bar line however the song got there (played, looped, located, rendered from bar 37). Renders are the same wherever playback starts. But every discontinuity in the position (playback starting, a loop whose length isn't a whole number of cycles, a locate) is a jump in the LFO, which clicks unless it is slewed.
- **Hybrids** run free at the synced rate and correct the phase towards a reference at each block: by setting it (a jump), by slewing to it, or by nudging the rate for a while, as a phase-locked loop does. The reference can be the bar grid (a locked LFO without the per-block sets) or the note-on plus the quarter notes played since (a free-running LFO that cannot drift).

**Drift.** A free-running LFO accumulates its phase, so any error in its rate becomes a phase error that grows with time. Three sources: rounding the rate (in hertz as a float, the error is 2⁻²⁴ of the rate, enough for 0.016 cycles in three hours at 48 Hz); the tempo's integral inside a block (a ramp is seen as a staircase, and the LFO falls behind by about half a block's tempo change per block); and the host itself, if it accumulates its position in double rather than computing it from its sample count. A locked LFO has no drift of its own, but it inherits the host's.

## Sylenth1: what is known

The full source table, with confidence ratings and the open questions S1–S14, is in the inventory ([§8](../reference/sylenth1-features.md#8-tempo-sync-pp-12-20)). In short:

- **Official:** one Sync button on the top bar "forces several timing parameters like LFO rates and delay times to be synchronized with the host tempo"; values are "bar parts", "one bar contains four beats", T is triplet and D dotted (manual p. 12). A Free LFO is "not retriggered… nor will it sync to the host tempo" (p. 20). The specification: "Sync button for syncing LFO and delay parameters to host tempo." The changelog: v3.040 "Fixes Arpeggiator host tempo sync problem", v3.050 adds Swing. Nothing about transport, position, or a standalone.
- **The developer (KVR):** 8/1 is 8 bars, 32 beats; a triplet is 2/3 of the length and dotted 3/2; 2/1T is 16/3 beats and 2/1D 12 (2010). In v1.21 the LFO followed the tempo even with Sync off, by design, so a rate between two synced values stays in sync (2007). And: "Sylenth1 locks to host tempo, but not to song position… It'll start the arpeggiator at the moment you press a key" (2017).
- **Users:** the arpeggiator's Time shows milliseconds with Sync off; one switch unsyncs the LFOs and the delay together; values in use include 1/4, 1/16, 1/8D, 8T and 16T, and 1/32 at 140 BPM. 16/1 was requested in 2010, so it didn't exist then. A synced sine starts at its peak on every note.
- **Unknown:** the full value list and its order; whether the chorus and phaser rates are synced; whether Sync off is still tempo-relative; what a stopped transport, a ramp, a loop, or the time signature do. Each is an open question (S1–S14) with the demo measurement that settles it in M1.13.

So the evidence points to **tempo, not position**: the phase starts at the note-on and runs at the synced rate.

## The candidates and what they measured

Measured by the hidden `[sync-report]` and `[sync-long-report]` ([tempo_sync_report.cpp](../../tests/dsp/control/tempo_sync_report.cpp)) and the `[sync]` benchmarks ([tempo_sync_benchmark.cpp](../../tests/dsp/benchmarks/tempo_sync_benchmark.cpp)), on test-only prototypes ([tempo_sync_candidates.hpp](../../tests/dsp/control/tempo_sync_candidates.hpp)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[sync-report]"        # about 1 s
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[sync-long-report]"   # three hours of every model, about 12 s
tools/bench.sh "[sync]"
```

### Note values as data

The prototypes represent a note value as a `Ratio` (two 64-bit integers, always reduced) of whole notes and a `NoteKind` (straight, triplet, dotted). `wholeNotes()` and `quarterNotes()` are exact. `seconds()`, `hertz()`, `samples()`, and `phaseIncrement()` take plain values, the tempo in BPM and the sample rate, and round once. The LFO's increment is cycles per sample × 2⁶⁴, computed as (BPM · den) / (60 · fs · num) in double and rounded to the nearest integer: two roundings of 2⁻⁵³ before the last one.

The value list is data: the 10 straight bases from 8/1 to 1/64, each with its dotted and triplet value.

| Order | Every step shorter than the last? |
|---|---|
| **By length**: 8/1D, 8/1, 4/1D, 8/1T, 4/1, 2/1D, 4/1T, 2/1, 1/1D, 2/1T, 1/1, 1/2D, 1/1T, 1/2, 1/4D, 1/2T, 1/4, … 1/64, 1/64T | Yes, strictly. No two values are equal (a dotted value is 9/4 of a triplet one, never a power of two). |
| **By base**: 8/1D, 8/1, 8/1T, 4/1D, 4/1, 4/1T, … | No: 9 of the steps get longer (1/2T = 1/3, then 1/4D = 3/8). |

At 120 BPM the list runs from 24 s (8/1D, 0.042 Hz) to 20.8 ms (1/64T, 48 Hz). At 20 BPM 8/1D is a cycle every 144 s (0.0069 Hz); at 300 BPM 1/64T is 120 Hz, past the free rate's placeholder top of 100 Hz (L6). The full table, with seconds and samples, is in the report.

### Exactness

The developer's examples are exact: 8/1 = 32 beats, 2/1 = 8, 2/1T = 16/3, 2/1D = 12.

Nine identities in samples at 93 BPM and 44.1 kHz (a tempo at which the lengths aren't dyadic). Exact fractions get every one right. A table of multipliers (2/3 and 3/2 rounded to the type) gets the products right at this tempo, but **adding** lengths misses: 96 × 1/64T ≠ 1/1 as a sum in both double and float, and 9 × 1/8T ≠ 1/2D as a double sum.

The test that matters is an arpeggiator at 1/8T, 93 BPM, 44.1 kHz for three hours: 50,220 steps of exactly 294000/31 samples, so a step lands exactly on a sample every 31 steps. Each step starts at the first sample at or after its time.

| Step positions from | Steps on the wrong sample | Worst |
|---|---:|---:|
| Exact `Ratio` (⌈k · num / den⌉ in integers) | 0 | 0 |
| k × samples per step, double table | 0 | 0 |
| k × samples per step, float table | 46,364 | 7 samples |
| A float position, the step added each time | 50,057 | 228,341 samples (5.2 s) |

A double product happens to survive this case. It rounds once per step, though, and nothing guarantees that a rounding never pushes a whole-sample position past an integer. The exact form costs one 64-bit multiply and divide per step.

### The models

One renderer, `SyncedLfo<Model>`, runs every model. It reads a periodic row of `lfoShapes` at a 64-bit phase and slews a jump as `Lfo` does: linearly from the last output, over 1 ms or 1/16 cycle if that is shorter. At each block's start, the model takes the host's clock (tempo, PPQ, playing) and returns a plan: the increment for the block, and optionally a jump.

| Model | Rate | Phase |
|---|---|---|
| **Rate only** | `phaseIncrement(note, bpm, fs)`, cached until the tempo changes | Starts at the note-on and accumulates (L12) |
| **Rate only, float Hz** | As today's `Lfo::setFrequency(float)`: the rate in hertz rounded to a float, then the increment | As above |
| **Locked** | As rate only | While playing, set at every block to the bar grid, frac(PPQ / beats per cycle), cycles counted from PPQ 0. Unslewed if the set is under 2⁻²⁰ cycle, slewed if larger |
| **Hybrid (bar grid)** | As rate only, nudged | While playing, the error against the bar grid is left alone under 2⁻²⁴ cycle, nudged away over the block (the increment changed by error / block length, at most 1/16 of the rate) up to 1/64 cycle, and a slewed jump beyond that |
| **Note-anchored** | As rate only, nudged | L12's semantics with the host's tempo integral: at the first playing block it records its offset from the grid; afterwards the error against grid + offset is nudged away as above. A position that doesn't follow from the last block (more than 1/64 quarter note out: a loop, a locate, a start) re-anchors, so the phase runs on |

Every model free-runs at the host's tempo while the transport is stopped.

### Drift over one minute and three hours

The largest phase error in cycles, sampled every 64 blocks of 512 and at the end, against the exact phase computed in integers. The worst over 120, 174, and 99.95 BPM, a note-on at PPQ 0, playing throughout. Two hosts: one computes its position from its sample count in double, and one adds each block's length in double. One cycle is 16 s at 8/1 and 120 BPM, and 20.8 ms at 1/64T.

| Model | Host | 8/1, 1 min | 1/64T, 1 min | 8/1, 3 h | 1/64T, 3 h |
|---|---|---:|---:|---:|---:|
| Rate only | either | 2.1·10⁻¹³ | 2.9·10⁻¹³ | 3.8·10⁻¹¹ | 5.2·10⁻¹¹ |
| Rate only, float Hz | either | 1.8·10⁻⁷ | 9.2·10⁻⁵ | 3.2·10⁻⁵ | **1.6·10⁻²** |
| Locked | computed | 4.8·10⁻¹⁶ | 7.0·10⁻¹³ | 1.1·10⁻¹³ | 1.1·10⁻¹⁰ |
| Locked | accumulated | 2.0·10⁻¹² | 1.5·10⁻⁹ | 7.1·10⁻⁸ | 5.5·10⁻⁵ |
| Hybrid, note-anchored | computed | 2.1·10⁻¹³ | 2.9·10⁻¹³ | 3.8·10⁻¹¹ | 5.2·10⁻¹¹ |
| Hybrid, note-anchored | accumulated | 2.1·10⁻¹³ | 2.9·10⁻¹³ | 6.0·10⁻⁸ | 5.5·10⁻⁵ |

(Worst over 44.1, 48, 96, and 192 kHz; the report has each rate.)

- **The 64-bit increment from exact inputs doesn't drift**: 5·10⁻¹¹ cycles in three hours is about a nanosecond at 1/64T.
- **Today's float-hertz API drifts**: 0.016 cycles in three hours at 1/64T (0.33 ms), and 3·10⁻⁵ at 8/1 (0.5 ms). It is harmless at a few minutes, but it grows linearly, so a synced LFO needs an exact increment.
- **A locked LFO inherits the host's drift**: a host that accumulates its position moves 5.5·10⁻⁵ cycles (1 µs) from the true grid in three hours at 192 kHz, and the locked LFO follows it. The hybrids follow it too, once the error passes their 2⁻²⁴-cycle deadband.

### Events

48 kHz, blocks of 512, a sine LFO, note-on at sample 0. The host splits a block at each event, as hosts do at loop points. **Step** is the largest sample-to-sample change from 8 samples before the event to 4096 after, against the sine's own largest slope. **Click** is the LFO's energy above 1 kHz around the event, in dB re an instant unit step (the LFO report's metric). Jumps are phase sets after the first block.

| Event | Value | Rate only, note-anchored | Locked | Hybrid |
|---|---|---|---|---|
| Tempo step 120 → 140 | 1/4 | no jump; step 0.95×, −115 dB | 557 unslewed sets; no click | no jump |
| Tempo step 120 → 140 | 1/64T | no jump; −68.5 dB (the 48 Hz sine's own leakage, the same for all) | 558 sets; no click | no jump |
| 4-bar loop | 1/4 (divides the loop) | nothing | nothing | nothing |
| 4-bar loop | 1/4D (1/3 cycle off) | nothing | **slewed jump: 179× slope, −15.1 dB** | the same |
| 4-bar loop | 8/1 (half a cycle off) | nothing | **5093× slope, −12.6 dB** | the same |
| Locate 8 → 33.37 | 1/4 | nothing | **0.37 cycle: 135×, −14.0 dB** | the same |
| Transport starts at PPQ 5.3 | 1/4 | nothing | **0.30 cycle: 105×, −16.2 dB** | the same |
| Transport starts at PPQ 5.3 | 1/4D | nothing | **0.20 cycle: 57×, −25.1 dB** | the same |
| Transport stops | 1/4 | nothing | nothing (runs free) | nothing |

The models that track the bar grid click at every discontinuity in the position, even with the 1 ms slew: −12.6 to −25 dB re a unit step, the size of an LFO edge (lfos.md). That is the price of the lock. The rate-only and note-anchored models never jump.

**A tempo ramp** (120 → 160 BPM over 6 s, reported at each block's start), against "note-on phase + quarter notes played", in cycles:

| Model | 1/4, blocks of 512 | 1/64T, blocks of 32 | 128 | 512 | 2048 | 4096 |
|---|---:|---:|---:|---:|---:|---:|
| Rate only (and float Hz) | 3.6·10⁻³ (1.8 ms) | 5.3·10⁻³ | 2.1·10⁻² | 8.5·10⁻² (1.8 ms) | 0.34 | 0.5 (≥ 10 ms) |
| Locked, hybrid, note-anchored | 6.3·10⁻⁶ | 5.9·10⁻⁷ | 9.5·10⁻⁶ | 1.5·10⁻⁴ | 2.4·10⁻³ | 9.7·10⁻³ |

The rate-only error is the staircase: the tempo held for a block while the host's ramps. It grows with the block size and, over a ramp, by about half a block's tempo change per block, so 1.8 ms after a 6 s ramp at 512 samples, and more than 10 ms at 4096 (0.5 cycles, the most a phase error can show). It is a lag behind the note-on's grid, not the bar's: an LFO started off the beat was never on the bar grid. The models that read the position correct it at the next block, leaving only the error inside one block.

### Block sizes, subnormals, allocation

The same scenario rendered in blocks of 1, 37, and 4096, split at the host's events: a sine at 1/16 and a saw at 1/4D (whose edges show a one-sample shift as a difference of 2).

| Model | Steady | Tempo step | Ramp | Loop, locate, start, stop |
|---|---|---|---|---|
| Rate only (and float Hz) | identical | identical | sine 0.70, saw 2.0 | identical |
| Locked | sine 1.7·10⁻⁷, saw 2.0 | the same | sine 1.2·10⁻², saw 2.0 | sine 1.7·10⁻⁷, saw 2.0 |
| Hybrid, note-anchored | identical | identical | sine 1.0·10⁻², saw 2.0 | identical |

- **Under a ramp nothing is identical across block sizes**, because the host reports a different staircase for each size. The models that read the position stay within 10⁻² of each other; rate only differs by up to 0.7 (its lag grows with the block size).
- **Locked is never identical**: its per-block set to the host's double position differs from the accumulated phase in the last bits, which moves a saw's edge by a sample.
- **Rate only and the hybrids are identical everywhere else.** The hybrids' deadband (2⁻²⁴ cycle) is what keeps them identical: below it they never correct.

No model produced a subnormal output, and none allocated while processing 2000 blocks of tempo changes, jumps, starts, and stops (`AllocationCounter`).

### CPU

Nanoseconds per sample, 48 kHz, `relwithdebinfo`, Apple M1 Ultra (`tools/bench.sh "[sync]"`), 1/8D at 128 BPM, playing. The renderer's sine (one `std::sin` per sample) is about 2.6 ns of each figure and is the same for all. The difference between the two columns is the per-block work.

| Model | Blocks of 512 | Blocks of 32 | Per block |
|---|---:|---:|---:|
| Rate only | 2.59 | 2.86 | ≈ 9 ns |
| Rate only, float Hz (no cache) | 2.75 | 4.41 | ≈ 53 ns |
| Locked | 2.84 | 5.65 | ≈ 90 ns |
| Hybrid | 2.80 | 5.46 | ≈ 85 ns |
| Note-anchored | 2.97 | 5.01 | ≈ 65 ns |

`phaseIncrement()` and `gridPhase()` cost 3.6 ns per call. Everything here is per LFO slot per block: the increment and the grid phase are the same for every voice unless the rate is modulated per voice. Even at blocks of 32, a position-reading model adds about 3 ns per sample per slot, against the 12.8 ns per sample of a 16-voice synth's 32 LFOs (lfos.md).

### How each model fits `Lfo`

`Lfo` takes its rate through `setFrequency(float hz)` and starts a cycle through `restart()` and `retrigger()`. It has no way to set the phase, and it commits a new rate on a curved row at its next 32-sample grid point.

- **Rate only** needs one addition: an exact rate input, `setPhaseIncrement(std::uint64_t)` (or a `setFrequency(double)`), because the float path drifts (above). The voice computes `phaseIncrement(note, bpm, fs)` once per block per slot and passes it on. `process(out, events)`, retriggers at the note-on, and Free (a copy of a global LFO) stay as they are. A tempo change on a curved row lands at the next grid point, up to 31 samples late; the phase stays continuous.
- **Locked** needs a `setPhase()` that is silent for tiny sets and slewed for large ones, called every block. Every set restarts the curved rows' interpolation grid, which makes them depend on the block size. It also fights Free: a free LFO copied at the note-on is not a transport-locked one.
- **Hybrid and note-anchored** need the exact rate input every block (the nudge), `phase()` to measure the error (it exists), and, for the hybrid, a slewed `jumpTo(phase)`: a retrigger to an arbitrary phase. A nudge on a curved row also lands at the next grid point, but the next block measures the error again, so it still converges.

## Options for the decision

*To be decided by the project owner. The decision and its date go here and in PLAN.md's Decisions log.*

| Question | Recommendation | Why |
|---|---|---|
| **Model** | **Rate only**, with an **exact increment** from a `NoteValue`: `phaseIncrement(note, bpm, fs)` once per block per slot, into a new `Lfo::setPhaseIncrement()`. The phase starts at the note-on (Free off) or follows the global free LFO (Free on), as built. | It is what the evidence describes: the developer's "locks to host tempo, but not to song position", a synced sine starting at its peak on every note, Free the only mode that runs on. It never jumps, so loops, locates, and transport starts can't click. It is bit-identical across block sizes except under a ramp, drifts 5·10⁻¹¹ cycles in three hours, costs about 9 ns per block, and needs one small addition to `Lfo`. Its cost: a ramp leaves it behind the note-on's grid by about half a block's tempo change per block (1.8 ms after 6 s at 512 samples). |
| **Alternative model** | **Note-anchored**, if a ramp's lag matters. | The same semantics with no lag: it corrects towards "note-on phase + quarter notes played" from the host's position, re-anchoring at loops and starts so it never jumps. Its costs: about 65 ns per block, a deadband and two tolerances to justify and test, and it needs the position, which the standalone build doesn't have. |
| **Bar-locked LFOs** | Not for Part 1. If wanted, **the hybrid** as a Part 2 option ("lock to bar"), not the per-block lock. | Sylenth1 doesn't lock (above). A lock clicks at every loop, locate, and start (−12.6 to −25 dB even slewed). The hybrid gives the same lock with identical output across block sizes, where the per-block set never is. |
| **Note values** | **Exact fractions** (`Ratio` × straight, triplet, or dotted) in `libs/dsp`, with conversions to seconds, hertz, samples, and the 64-bit increment that take plain values (BPM, sample rate). Long-running positions (the arpeggiator's steps, M1.6) counted as integers, ⌈k · num / den⌉, never accumulated. | A float table puts 46,364 of 50,220 arpeggiator steps on the wrong sample in three hours (by up to 7), and a float accumulator is 5 s out. The fractions cost nothing per sample. The delay (M1.7) and the arpeggiator (M1.6) use the same conversions. |
| **Value list** | **30 values, 8/1D to 1/64T**: every straight value from 8/1 to 1/64 with its dotted and triplet value, **stepped and ordered by length** (strictly monotonic). Data in the parameter table (M1.9), so S1's measurement changes a list, not code. A parameter may use a sub-range: the delay's longest value is limited by its buffer (M1.7). | 8/1, 2/1T, 2/1D (the developer), 1/32 (2007), 1/16, 1/8D, 1/4, 8T, 16T (users) are attested. 16/1 didn't exist in 2010. 1/64 and 1/64T are a guess (1/64T at 120 BPM is 48 Hz, inside the free range). Ordering by length makes the knob monotonic, which grouping by base doesn't (9 reversals). |
| **What Sync covers** | **One global switch** for **LFO 1 and 2 Rate** (when Free is off), **Delay L and R**, and **Arp Time**. Not the chorus Rate or the phaser's LFO Rate. A **Free LFO ignores Sync** (Hz). With Sync off, rates and times are **absolute** (Hz, ms), not tempo-relative. | The manual, the specification, Audiotent, and two users on the arpeggiator. The chorus and phaser are never mentioned with Sync (S2). Sync off as absolute follows the manual and the specification, against the developer's 2007 description of v1.21 (S4): a measurement settles it, and it changes only how M1.9 maps the knob. |
| **Transport stopped, no host** | Run at the **tempo the host reports, whether or not the transport plays**, and ignore the position (rate only reads none). With no tempo (the standalone build, or a host that reports none) use **120 BPM**; if a host reports a tempo of 0, a negative one, or NaN, keep the last valid tempo. | Sylenth1's LFOs and arpeggiator are played live with the transport stopped, and the arpeggiator starts at the key press (the developer). Rate only has nothing to do at a stop, start, loop, or locate. Sylenth1 has no standalone, so 120 BPM (the common host default) is our choice (S13). |
| **Where it lives** | `libs/dsp`: the note values, conversions, and value list in a new `control/note_value.hpp`; `Lfo::setPhaseIncrement()`. `libs/interop` (M1.9): reading the tempo (and, for the alternative, the position and transport) from `AudioPlayHead`, and the fallbacks above. The plugin: Sync, Free, and each parameter's range of the list. | The design constraints: plain values into `libs/dsp`, no JUCE; the same conversions for the delay and the arpeggiator. |

## How it is applied here

*Written once the model is built (M1.2).*

## Tests

*Written once the model is built (M1.2).* The report's measurements become tests with derived tolerances: the value list (count, order, exact lengths, the developer's examples), the triplet and dotted identities, the conversions against exact fractions (seconds, hertz, samples, the increment within half a unit), the arpeggiator's step positions over three hours, drift over three hours bounded by the increment's rounding, phase continuity at a tempo step, block sizes 1/37/4096, the fallback tempo, subnormals, and allocation.

## References

- LennarDigital, *Sylenth1 User Manual*, v2.5, 2015, pp. 12, 20–25; [specifications](https://www.lennardigital.com/sylenth1/specs.php); [version history](https://lennardigital.com/sylenth1/whatsnew.php) (v3.040, v3.050). The developer's and users' posts on KVR are listed in the [feature inventory](../reference/sylenth1-features.md#8-tempo-sync-pp-12-20), with confidence ratings.
- Steinberg, *VST 3 SDK*: `Vst::ProcessContext` (tempo, `projectTimeMusic` in quarter notes, bar position, transport state, valid flags), reported once per block. JUCE, `AudioPlayHead::PositionInfo` (the same information, per block).
- D. Goldberg, "What every computer scientist should know about floating-point arithmetic", *ACM Computing Surveys* 23(1), 1991: rounding in products and accumulated sums.
- R. L. Graham, D. E. Knuth, and O. Patashnik, *Concrete Mathematics*, 2nd ed., Addison-Wesley, 1994, ch. 3: floors and ceilings of k · n / d, and why an integer position has no drift.
- F. M. Gardner, *Phaselock Techniques*, 3rd ed., Wiley, 2005: correcting a phase towards a reference by nudging the frequency (the hybrids).
- C. Roads, *The Computer Music Tutorial*, MIT Press, 1996: tempo, note values, and synchronization.
