# Modulation sources

A modulation source is anything whose value a route reads: an envelope, an LFO, the note's velocity, its keytrack, a MIDI controller, the arpeggiator's step. [modulation.md](modulation.md) built the model the sources plug into: the registries, the routing, each destination's rate, and each source's bound. This page is M1.5 task 3, the sources themselves:

- **Theory:** polyphonic and monophonic sources and what each costs; how a source's value reaches a destination that reads it every sample; running a source only while something reads it; smoothing a 7-bit controller; bounding a source over a stretch of samples, and why a sum of two monotonic parts isn't monotonic.
- **Sylenth1:** what is known about each source (inventory §6, [The sources in detail](../reference/sylenth1-features.md#the-sources-in-detail), M14–M23).
- **Stage 1:** the candidates, prototyped on the library's own classes and measured; the recommendations; what the library needs in stage 2, and what this pushes back on.

Read [modulation.md](modulation.md) first, then [envelopes.md](envelopes.md), [lfos.md](lfos.md), [keytracking.md](keytracking.md) and [legato-portamento.md](legato-portamento.md) (the velocity ramp and the bend, the model for a global ramp read at absolute samples), and [part-solo-and-skipping.md](part-solo-and-skipping.md) (why each source must bound its value).

## Theory

### Polyphonic and monophonic sources

A **polyphonic** (per-voice) source has a value for each note: its velocity, its keytrack, its envelopes, an LFO that restarts with it. A **monophonic** (global) source has one value for the instrument: the mod wheel, channel aftertouch, a free-running LFO, the arpeggiator's step. [modulation.md](modulation.md#polyphonic-and-monophonic) covers what happens when the two kinds meet a destination. This page is about producing them:

- **A per-voice source runs in every voice that sounds.** Its cost is multiplied by the voice count, and it has to follow the voice's events: a note-on from silence, a retrigger of a sounding voice (a steal), a legato note, a note-off.
- **A global source runs once**, in the engine, and every voice reads it. The voices must read it at the same absolute sample, or two voices hear different values at the same moment, and a voice's value would depend on how the host split its blocks. Our bend (`PitchBend`) is the model: one object, given each new value at its event's absolute sample, read by each voice in closed form at its own absolute samples.
- **A free LFO is both.** The instrument has one, running all the time. A voice copies it at its note-on and then runs its own copy, so that per-voice modulation of its rate (KeyTrack on LFO Rate) works. Until something moves a copy's rate, the copy and the global instance stay in step.

### Getting a source's value to a per-sample destination

Pitch, Phase, Volume and Mix take their modulation every sample (modulation.md, recommendations 4–6). For each sample, each route to the destination adds amount × the source's value. There are two ways to organise that:

- **Buffered.** Each source a route reads is rendered for the whole chunk (up to 32 samples) into a buffer. Then each route runs a tight loop over its source's buffer, adding amount × value into the destination's buffer. The loops are short, regular and easy to vectorise. The cost is a buffer per source and a pass per route.
- **Inline.** For each sample, each source is evaluated into a small array of values, and the routes are summed from it. Nothing is buffered, but every sample walks every route, with its branches, and every source is evaluated through a dispatch.

A source that **doesn't move over the chunk** (a velocity that isn't ramping, a wheel that isn't moving, a keytrack with a steady pitch) needs no buffer at all. Its route adds one constant term to every sample, and the product is the same one either way, so folding it is exact.

### Running only what something reads

A source that no route reads changes nothing, so running it is waste. Running only the sources in use saves their cost, but a route can appear mid-note: a player picks a destination while holding a chord, or a preset changes. The source then has no history. There are three things it can do:

- **Wait for the next note.** Exact from then on, but the new route does nothing for the rest of the note.
- **Start now, from the note-on state.** An envelope attacks from 0 at that point, and an LFO restarts its cycle. This is cheap and immediate, but it isn't what an always-running source would have reached.
- **Reconstruct.** Replay the note's gates and run the source forward to where it would be. This is exact if every input since the note-on is known (and the source's own rate wasn't modulated). It costs time in proportion to the note's age, unless the source can jump ahead in closed form.

### Smoothing a 7-bit controller

MIDI's mod wheel (CC 1) and channel pressure send 7-bit values, 128 steps over the range. On Pitch at +1.00 (an octave over the wheel) one step is 9.45 cents. A pitch or a gain that jumps by a step at each message makes a click, and a slow sweep makes a train of them: zipper noise. There are three answers:

- **A linear ramp** from where the value stands to each new value, over a fixed time. It is exact in closed form at any sample, so it can be read at absolute samples like `PitchBend`. Its latency is fixed: it arrives in exactly the ramp's time.
- **A one-pole** (an exponential approach). It is smoother at the start of a move, but it never quite arrives: it reaches 90% in 2.3 time constants. It can also be evaluated in closed form at absolute samples.
- **A smoother advanced per host block** (the common shortcut: move a smoothed value once per `processBlock`). Its output depends on how the host splits its blocks, so it breaks our first invariant.

The ramp's length is a trade. A longer ramp is smoother, but every move lags by the ramp's length. The bend took 5 ms (legato-portamento.md). A 14-bit controller (CC 1 with CC 33 as its LSB) makes steps 128 times smaller, but few keyboards send it.

### Bounding a source

The skip test needs an upper bound on each factor it reads over the coming segment (part-solo-and-skipping.md). Each route adds amount × its source's interval (modulation.md, recommendation 11). Some kinds of source are easy:

- **A monotonic source** between events (a linear ramp, a glide that lands, an exponential approach) lies between its two ends.
- **A sum of monotonic parts is not monotonic.** A glide rising while a bend's ramp falls goes down and then up within a segment, and its minimum lies inside, below both ends. For such a sum, bound each part by its own ends and add the intervals: that holds, because each part does.

Keytrack is exactly such a sum: (glide + bend + master tune − 9) / 120.

## Sylenth1: what is known

The full table is the inventory's [The sources in detail](../reference/sylenth1-features.md#the-sources-in-detail), with M14–M23. E9, E10, L8, AL9, K3 and F20 are annotated. In short:

- **Mod envelopes:** "common ADSR envelopes … similar to the amplitude envelope" (manual p. 19), with no velocity control. A Mono Legato note retriggers no envelope (p. 29). Everything else is E9's hypothesis: the same as the amp envelopes in every way (M14).
- **A non-free LFO** restarts at a note (a user: "it always starts on the top peak of the wave"). Free means "will not be retriggered … nor will it sync to the host tempo". Nothing says what Free shares or when it is copied (M16, M17).
- **Velocity** slides with the pitch under portamento (the product description; K3, decided). It does nothing until routed (E14).
- **KeyTrack:** the developer's quantity (F20). The pitch it follows is ours, decided in M1.3 and built in M1.4: the key, the glide, the bend and the master tune (M19).
- **Mod Wheel:** CC 1 (the manual's CC map). Nothing on its resolution, on CC 33, or on smoothing (M20).
- **Aftertouch:** added in v1.1. A user in 2008 with channel aftertouch: "Sylenth1 did respond, increasing LFO1 and LFO2 gain". The developer: aftertouch is a MIDI message, not a knob, so it can't be automated as a parameter. Nothing on polyphonic aftertouch (M21).
- **AmpEnv A/B:** each part's own envelope (AL9, decided). It is the envelope alone, since velocity doesn't touch the amp envelopes (E14). Nothing says what happens on a part nothing hears (M22).
- **Step Velocity:** the developer in 2008: "The gate is indeed always on (unless you don't use it of course)"; "Velocity set to '-' will skip the step, any other value will modulate the destination with that exact value"; "The arp does not need the step values". So **the step sequencer runs as a modulation source whether or not the arpeggiator is on**, a gater in its own right. What clocks it, what a "-" or a hold step outputs, and whether a note restarts it are open (M23). That is new for M1.6.

## The candidates

Stage 1 prototypes the sources in `tests/dsp/modulation/` on the library's own classes. [source_candidates.hpp](../../tests/dsp/modulation/source_candidates.hpp) holds `SourceVoice`, a copy of `dsp::Voice` as built. It runs on the real `OscillatorSlot`, `LadderFilter`, `Adsr` and `Lfo`, and on the library's modulation model (`compile()`, `accumulate()`, the bound functions, `RouteCompiler`). With no routes it is `dsp::Voice` to the bit (question 7). It adds:

- **The sources**, as stage 2 would build them:
  - two mod envelopes (`Adsr`) per voice, gated as the amp envelopes (`gateOn`, `legatoGateOn`, `gateOff`);
  - two LFOs (`Lfo`) per voice in segment order (task 2's decision). An LFO is restarted from silence, retriggered on a sounding voice (the rest of the segment rendered again from a copy taken at the grid point), and left alone by a legato note. A free one copies the engine's global instance;
  - velocity (`velocityRamp`), keytrack (`keytrackSource` of the voice's keytrack pitch), and both amp envelopes;
  - ModWheel and Aftertouch read from the engine's `Controller`s at absolute samples, and Step Velocity from a global stand-in input.
- **The destinations the questions need:**
  - Pitch A/B/AB and Mix A/B/AB per sample;
  - Cutoff, Reso and the Filter Control's AB terms at the grid points, from the sources as they stand there;
  - LFO Rate (held) and Gain (ramped) in segment order;
  - `mixModulationBound()` from the sources' bounds.

  Volume, Phase and Pan are task 4's: their routes compile and are ignored.
- **The candidates, as switches** (`SourceSettings`), one question each (below).

`SourceEngine` runs the real `VoiceAllocator` over 16 `SourceVoice`s. It holds the engine's state the plan gives M1.8: the `modulation::Patch` (slot events at their samples), the two controllers, the free LFOs' global instances (moved on with each piece the engine renders), and Step Velocity.

| Question | Candidates |
|---|---|
| **1. How sources reach the per-sample destinations** | (a) **buffered**: each source a per-sample route reads rendered into a 32-sample chunk buffer (an envelope's own buffer, an LFO from its segment rendered ahead), then each route's loop over it; (b) **inline**: every sample, each such source evaluated into `SourceValues` (a mod envelope ticked with `next()`), then `accumulate()` over the per-sample routes; (c) **folded**: (a), with a source that is constant over the chunk added as one term and no buffer |
| **2. Running only what is used** | every source always; or only while `uses()` says a route reads it, and then, for a route that appears mid-note: the source at 0 until the **next note**; started at the **next grid point** from its note-on state; or **reconstructed** there from the note's elapsed time (its gates replayed, a free LFO copied) |
| **3. ModWheel and Aftertouch smoothing** | a step; a linear ramp over 1, 2, 5 or 10 ms; a one-pole with a time constant of 1, 2 or 5 ms (both in closed form at absolute samples); and, as a control, a one-pole advanced once per host block |
| **4. KeyTrack and velocity over a segment** | per sample, or held at the grid point; keytrack's bound from its own two ends, or from its parts' ends (the glide's plus the bend's) |
| **5. AmpEnv as a source on a skipped part** | an unheard part's amp envelope run always (`dsp::Voice` today) or only while it is a source; each against a voice with every skip turned off |
| **6. Each source's bound** | task 2's kinds (an envelope by stage, an LFO from its segment rendered ahead, a linear source its ends; keytrack from its parts), checked against every sample's value; and three deliberately small rules: keytrack from its own ends, every source its value at the chunk's start, an LFO from its segment's two ends |
| **7. No routes** | `SourceVoice` with no routes against `dsp::Voice`, under every candidate |

A free LFO at a steal has one more pair of candidates: the voice's copy **kept** (copied only at a note-on from silence) or **copied again** at every note-on.

## What they measured

The candidates were measured by the hidden `[sources-report]` ([source_report.cpp](../../tests/dsp/modulation/source_report.cpp), about 5 s) and the `[sources]` benchmarks ([source_benchmark.cpp](../../tests/dsp/benchmarks/source_benchmark.cpp)). The default run checks the invariants under `[sources-model]` ([source_candidates_test.cpp](../../tests/dsp/modulation/source_candidates_test.cpp), 1.6 s):

- no routes is the library to the bit under every candidate;
- the three deliveries agree to the bit at host blocks of 1, 37 and 4096, with no allocation;
- every bound holds over random scenarios at three rates, and the small ones fail;
- a reconstructed source is the always-run one to the bit.

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[sources-report]"
tools/bench.sh "[sources]"
```

### Question 7: no routes

`SourceVoice` with no routes against `dsp::Voice` through `VoiceAllocator` (Polyphony 2, two steals, releases of 50 ms), 0.5 s. LFO 1 is a free S&H and Mod Env 1 is set, so the sources exist and, under "always", run.

| Patch | folded, used only | buffered, every source always | inline, every source always | unheard amp envelope only as a source |
|---|---|---|---|---|
| init (A1 saw) | identical | identical | identical | identical |
| A1 and B1 saws | identical | identical | identical | identical |
| 4 × 8 unison, resonant | identical | identical | identical | identical |

The same at host blocks of 512 and of 37 (24 of 24), with no allocation. CPU, one voice held at the init patch (below): `dsp::Voice` 128.7 ns per sample, `SourceVoice` with no routes 128.7. Running only what is used costs nothing measurable.

### Question 1: how sources reach the per-sample destinations

**Exactness.** A busy patch: 13 routes reading every source into Pitch A/B/AB, Mix A/B/AB, Cutoff A, LFO 1 Rate and LFO 2 Gain. Polyphony 2 with steals, Slide glides, and the bend, the wheel, pressure and Step Velocity all moving, 0.54 s. Each render is compared with folded at host blocks of 4096:

| Delivery | sources run | host blocks of 1 | 37 | 4096 | allocations |
|---|---|---|---|---|---:|
| (a) buffered | always / used only | identical | identical | identical | 0 |
| (b) inline | always / used only | identical | identical | identical | 0 |
| (c) folded | always / used only | identical | identical | identical | 0 |

The three deliveries agree to the bit because each forms every product in a statement of its own, as `accumulate()` does (modulation.md, "bit for bit depends on statement structure"), and adds the routes in slot order, starting from 0.

**CPU** (`tools/bench.sh "[sources]"`, `relwithdebinfo`, Apple M1 Ultra, virtualised; one voice at the init patch, A3, past the attacks; the median of three runs, spread 0.8–6.6%; ns per sample per voice). 4 routes: Mod Env 1 → Pitch A, LFO 1 → Mix A, Velocity → Mix A, ModWheel → Pitch A. 16 routes: every slot, every source, into Pitch, Mix, Cutoff and LFO 2 Gain. *Moving*: the wheel and pressure get a new value every block and ramp over its first 5 ms.

| Delivery | 4 routes | 16 routes | 4 routes, moving | 16 routes, moving |
|---|---:|---:|---:|---:|
| (none: no routes) | 128.7 | | | |
| (a) buffered | 145.3 | 177.3 | 144.7 | 178.9 |
| (b) inline | 158.7 | 197.8 | 157.2 | 197.8 |
| **(c) folded** | **140.6** | **162.2** | **141.7** | **167.4** |

- **Folded is the cheapest everywhere**: 4 routes add 11.9 ns per voice, 16 routes 33.5. That is 28–31% less than buffered (16.6 and 48.6), which renders a buffer for each still source, and 52–60% less than inline (30.0 and 69.1).
- **Inline is the dearest**: every sample walks every route and dispatches on every source, and the mod envelopes are ticked one sample at a time.
- **Moving controllers cost folded little**: 1.1 ns with 4 routes and 5.2 with 16, where the moving sources fall back to buffers.

### Question 2: running only what is used

**What running every source costs**, with no routes:

| | used only | every source always | difference |
|---|---:|---:|---:|
| the mod envelopes at their sustains, sine and triangle LFOs | 128.7 | 131.0 | +2.3 (1.8%) |
| both mod envelopes in a 10 s attack, both LFOs on the Lorenz row | 128.2 | 132.2 | +4.0 (3.1%) |
| with 4 routes (folded) | 140.6 | 140.1 | within noise |

So two envelopes and two LFOs cost 2.3–4.0 ns per voice: 37–64 ns for 16 voices, on every patch, routed or not.

**A route that appears mid-note.** The setup:

- Polyphony 1, A1 a sine at A2, filters bypassed (so the click measure reads only transients), amp A sustain 1, release 0.3 s;
- the route appears at sample 24,013 (0.5 s, mid-segment), and the voice takes it at its next grid point;
- Mod Env 1: A 0.2 s, D 0.3 s, S 0.5, R 0.3 s. LFO 1: a sine at 3 Hz, Gain 10;
- host blocks of 37.

*Click* is filters.md's measure at the route's sample, dB re an instant unit step. *Differs* is the RMS of the difference from "always" over the 0.5 s after, dB re that render's RMS.

| Route | when | always: click | next note: click / differs | next grid point | reconstructed |
|---|---|---:|---|---|---|
| Mod Env 1 → Mix A, −5 | held, at its sustain | −22.8 | −88.9 / −9.5 | −77.1 / −16.6 | −22.8 / exact |
| Mod Env 1 → Mix A, −5 | in its attack (the key 0.4 s before) | −34.4 | −88.1 / −6.8 | −83.5 / −14.4 | −34.4 / exact |
| Mod Env 1 → Mix A, −5 | released 0.1 s before | −63.2 | −107.1 / −35.0 | −107.1 / −35.0 | −63.2 / exact |
| LFO 1 → Pitch A, +0.05 | held | −63.0 | −88.9 / −7.2 | −62.8 / −1.4 | −63.0 / exact |
| LFO 1 (free) → Pitch A, +0.05 | held | −63.0 | −88.9 / −7.2 | −63.0 / exact | −63.0 / exact |

- **The click is the route's own, not the source's.** With every source always running, a route that appears where its envelope stands at 0.5 steps Mix from 10 to 7.5 at once: −22.8 dB. That is the amount stepping, the same as any unsmoothed parameter step (a question for task 4 and M1.9, below).
- **Starting at the next grid point clicks less than always running**, since the envelope rises from 0 over its attack (−77.1 dB). But the envelope is then where it would be 0.5 s earlier for the rest of the note (−16.6 dB of difference). A non-free LFO restarts its cycle (−1.4 dB: wholly out of phase).
- **The next note leaves the route silent until then** (−6.8 to −9.5 dB of difference: the whole modulation is missing).
- **Reconstructing is exact** in every case, the steal before the route included (the default run pins it, envelope and LFO).
- **A free LFO is exact under both "next grid point" and "reconstructed"**: the global instance is where a running copy would be. That holds only once the copy is moved on to the voice's own sample (below).

**What a replay costs.** A note 10 s old is 480,000 samples, replayed in 32-sample pieces (the best of 5): an envelope 91 µs, a sine LFO 322 µs. A 32-sample segment lasts 667 µs at 48 kHz, so one voice's replay takes 14–48% of the time a segment has, and 16 voices replaying at the same grid point take 2.2–7.7 segments. The cost grows with the note's age, which no real-time path may do. An `Adsr` could jump ahead in closed form, because its levels come from a sample count. An `Lfo`'s periodic rows could too, from their fixed-point phase. Its S&H draws once per cycle, and its Lorenz row must integrate.

**Found: a free LFO's copy has to be moved on to the voice's own sample.** The engine moves the global instance on with each piece it renders, so the instance stands at the start of the allocator's call. A note-on falls there, since the engine splits its blocks at events. A late start falls at the voice's grid point, inside the piece: the first prototype copied there and was 7 samples behind, and the default run's reconstruction test failed. The copy now runs forward from the global instance by the voice's offset into the call (`copyFree(k, ahead)`). That is a requirement on M1.8.

**A free LFO at a steal.** Polyphony 1, A1 a sine, filters bypassed. LFO 1 is free, a sine at 5 Hz on Pitch A at +0.05, with KeyTrack on LFO 1 Rate at +5, so each voice's copy runs off the global instance. A1 sounds at 0, and a steal to A3 comes at 1.0 s:

| Free at a steal | click | without the LFO's route (the note change alone) | differs from "kept" |
|---|---:|---:|---:|
| kept (copied only at a note-on from silence) | −22.9 | −23.1 | (reference) |
| copied again at every note-on | −22.9 | −23.1 | −29.1 dB |

The re-copy's phase jump hides under the note change's own click here, but the two renders differ after it (−29.1 dB). A copy that is kept is continuous, and it matches "will not be retriggered".

### Question 3: ModWheel and Aftertouch smoothing

The setup:

- A1 a sine at A3, filters bypassed.
- *Zipper*: ModWheel on Pitch A at +1.00 (an octave over the wheel), swept 0 → 127 one value every 20 ms (960 samples, from sample 4,813, off the voices' grid). Each step is 1/127 octave, 9.45 cents. The same sweep on Mix A at −10 (unity to silence). The measure is filters.md's (energy above 1 kHz in a Hann 4096, dB re a unit step; the sine stays below 440 Hz), averaged over 16 points 0.1 s apart from 0.5 s.
- *Latency*: from a 0 → 127 message to 50% and 90% of the way.
- *Click*: that step at sample 24,013, on Pitch A at +1.00 and on Mix A at −10.
- *Blocks*: the sweep rendered at host blocks of 1, 37 and 4096, compared to the bit.

| Smoothing | zipper: Pitch | zipper: Mix | latency 50% / 90% (ms) | click: Pitch | click: Mix | blocks 1 / 37 / 4096 |
|---|---:|---:|---:|---:|---:|---|
| step | −62.0 | −50.0 | 0.02 / 0.02 | −19.5 | −13.3 | identical |
| linear, 1 ms | −74.3 | −63.9 | 0.52 / 0.94 | −31.4 | −23.9 | identical |
| linear, 2 ms | −78.1 | −71.9 | 1.02 / 1.83 | −35.4 | −34.8 | identical |
| **linear, 5 ms** | **−84.7** | **−77.1** | **2.52 / 4.52** | **−43.5** | **−38.1** | identical |
| linear, 10 ms | −87.0 | −82.5 | 5.02 / 9.02 | −51.4 | −43.2 | identical |
| one-pole, 1 ms | −76.7 | −68.4 | 0.73 / 2.33 | −40.7 | −30.1 | identical |
| one-pole, 2 ms | −81.7 | −74.4 | 1.42 / 4.65 | −46.9 | −36.2 | identical |
| one-pole, 5 ms | −86.2 | −82.2 | 3.50 / 11.54 | −55.0 | −44.2 | identical |
| one-pole per host block, 5 ms | −63.0 | −46.5 | 0.02 / 10.69 | −26.2 | −4.1 | **differs** |

- **A step zips**: −62.0 dB on Pitch and −50.0 on Mix, and a full move clicks at −19.5 and −13.3.
- **A 5 ms linear ramp takes the zipper down 22.7 dB on Pitch and 27.1 on Mix**, and the step's click by 24.0 and 24.8, at a fixed 4.5 ms to 90%. That is the bend's ramp (−113.8 dB on a 2 Hz vibrato in legato-portamento.md, a gentler test than this sweep).
- **10 ms buys 2.3–5.4 dB more for twice the lag.**
- **A one-pole is no better**: at 2 ms it zips more than linear 5 ms (−81.7 against −84.7 dB) and still takes longer to 90% (4.65 against 4.52 ms). It also never arrives, so it is never steady and a still wheel is never folded.
- **Every closed form read at absolute samples is independent of the host's blocks.** The per-block one-pole isn't. It also clicks at −4.1 dB on Mix: at host blocks of 512 its first block jumps 88% of the way at once.

### Question 4: KeyTrack and velocity over a segment

**Monotonic within a segment?** The test runs section 6's random scenarios: Slide glides, a bend to either end 0–200 samples after each note-on, and legato and stolen notes, over 1.68 million samples. The bound is taken at each grid point and at each call's start, for the rest of the segment:

| Source | bound | samples outside | worst excess |
|---|---|---:|---:|
| KeyTrack | **its parts' ends** (the glide's interval plus the bend's, plus the master tune) | **0** | 0 |
| KeyTrack | its own two ends | **98** | 0.000376 (4.51 cents) |
| Velocity | its two ends | 0 | 0 |

- **Keytrack isn't monotonic within a segment** when a glide and a bend's ramp move in opposite directions and one of them ends inside it: its own ends are too small 98 times.
- **Its parts each are monotonic** (a linear glide that may land, a linear 5 ms ramp that may end), so their intervals added hold.
- **Velocity is one linear ramp** (`Glide`), restarted only at events, which re-test. Its ends hold.

**Per sample or held at the grid point.** *Added* is filters.md's click on the difference between held and per sample, averaged over four points in the move:

| Source | the setup | per sample: its own click | held: added |
|---|---|---:|---:|
| Velocity | Mono Legato, Portamento 0.2 s, A3 at velocity 127, then a legato A4 at 1 (the velocity ramps over the glide); A1 a saw; Velocity on Mix A at −10 | 5.9 (the saw's own harmonics) | **−40.9** |
| KeyTrack | A1 a sine, Slide 0.3 s, A2 then A4 at 0.5 s, a bend to full down at 0.55 s; KeyTrack on Pitch A at +1.00 | −77.5 | −75.5 |

Held velocity on a gain zips (−40.9 dB). That is the mixer's held-grid zipper, which is why Mix is per sample. Held keytrack on pitch is a staircase of small pitch steps (−75.5 dB). Per sample costs nothing extra when the source is still, because folded adds it as one term.

### Question 5: AmpEnv as a source on a skipped part

Part A is A1, a saw. Amp Env B (A 0.3 s, S 1, R 0.4 s) drives Pitch A at +0.1, or Mix A at −5. Three notes play (Polyphony 2, a steal) for 1 s at host blocks of 37. *Skips on* is the voice as built (task 5's plan). *Nothing skipped* runs every filter and slot.

| Part B | route | skips on = nothing skipped | only as a source = always | the route moves part A |
|---|---|---|---|---|
| unheard (filter B `--`, no oscillators) | Amp Env B → Pitch A, +0.1 | identical | identical | yes |
| unheard | Amp Env B → Mix A, −5 | identical | identical | yes |
| heard, no oscillators (filter B at rest, skipped) | Amp Env B → Pitch A | identical | identical | yes |
| heard, no oscillators | Amp Env B → Mix A | identical | identical | yes |
| filter A on AB, filter B `--` (B crosses, no oscillators) | Amp Env B → Pitch A | identical | identical | yes |
| filter A on AB, filter B `--` | Amp Env B → Mix A | identical | identical | yes |

- **`dsp::Voice` already runs both amp envelopes in every chunk**, whatever the skip plan does, so an amp envelope used as a source is always there. The plan stays exact (6 of 6), and the source moves part A in every case.
- **Running an unheard part's envelope only while it is a source saves nothing measurable**: filter B on `--`, 125.9 ns per voice always against 128.0 only as a source.

### Question 6: each source's bound

The test runs random scenarios:

- notes with velocities 1–127, Slide glides, bends to either end and anywhere, the wheel, pressure, Step Velocity, legato and stolen notes, events 1 sample to 50 ms apart;
- 13 routes reading every source, LFO 1 a triangle with its Rate from Amp Env A, LFO 2 an S&H with its Gain from LFO 1;
- 2 s at each of 44.1, 48 and 96 kHz, at host blocks of 1, 37 and 4096 (9 renders).

Each source's bound is taken at each grid point and at the start of each `process()` call (where the skip test may re-test), for the rest of the segment. Every sample's value is checked against it.

| Source | bound (task 2's kinds) | samples | outside | mean width |
|---|---|---:|---:|---:|
| Velocity | its two ends | 1,678,022 | 0 | 0.0005 |
| Mod Wheel | its two ends | 1,678,022 | 0 | 0.0009 |
| KeyTrack | its parts' ends | 1,678,022 | 0 | 0.0003 |
| Amp Env A | by stage | 1,678,022 | 0 | 0.0187 |
| Amp Env B | by stage | 1,678,022 | 0 | 0.0290 |
| Mod Env 1 | by stage | 1,678,022 | 0 | 0.0922 |
| Mod Env 2 | by stage | 1,678,022 | 0 | 0.1711 |
| LFO 1 | the segment rendered ahead | 1,678,022 | 0 | 0.0125 |
| LFO 2 | the segment rendered ahead | 1,678,022 | 0 | 0.0027 |
| Aftertouch | its two ends | 1,678,022 | 0 | 0.0011 |
| Step Velocity | its two ends | 1,678,022 | 0 | 0 |

**Deliberately small bounds** over the same 9 renders, samples outside (worst excess):

| Source | keytrack from its own ends | every source its value at the start | an LFO from its two ends |
|---|---:|---:|---:|
| Velocity | 0 | 118,539 (0.0212) | 0 |
| Mod Wheel | 0 | 37,668 (0.1220) | 0 |
| KeyTrack | 98 (0.0004) | 164,878 (0.0095) | 0 |
| Amp Env A | 0 | 192,394 (0.0937) | 0 |
| Amp Env B | 0 | 279,110 (0.0601) | 0 |
| Mod Env 1 | 0 | 772,264 (0.0856) | 0 |
| Mod Env 2 | 0 | 1,050,084 (0.2497) | 0 |
| LFO 1 | 0 | 990,856 (0.4478) | 8,965 (0.0218) |
| LFO 2 | 0 | 989,783 (0.2045) | 481 (0.0048) |
| Aftertouch | 0 | 44,420 (0.1211) | 0 |
| Step Velocity | 0 | 0 | 0 |

- **Every bound holds in the voice**, with its events, glides, bends, controllers and modulated LFOs.
- **Each small rule fails where it should.** Keytrack's own ends fail on opposing glides and bends. Any value at the start fails for everything that moves. An LFO's ends miss its crests and slews (task 2's finding, now in the voice).
- **Step Velocity's stand-in is constant between its events**, so its ends are exact. That stays true in M1.6 only if every step arrives as an event at its sample (below).

### Real-time terms

Every candidate gives the same output at host blocks of 1, 37 and 4096, with no allocation (the tables above), except the per-block one-pole (by design) and the reconstruction's replay, whose time grows with the note's age. `SourceVoice` adds to `dsp::Voice`:

- two `Adsr`s, two `Lfo`s and their copies at the grid point;
- the LFOs' segments (2 × 32 floats);
- a chunk buffer per source (12 × 32 floats);
- the per-sample sums of Pitch and Mix per part (4 × 32 floats);
- the compiled routes twice (the per-sample ones apart), and a gate log for the replay (32 entries).

## Recommendations

*Stage 1's recommendations. Where only the demo can settle a question (M14–M23), the recommendation is the working hypothesis, and the measurement is what makes it the safer default. Awaiting the project owner's decision.*

| # | Question | Recommendation | Why | Alternatives | Settled by |
|---|---|---|---|---|---|
| 1 | **How sources reach the per-sample destinations** | **(c) folded.** At each chunk, each source a per-sample route reads is put in a chunk buffer (an envelope's own buffer; an LFO from its segment rendered ahead; a linear source or a controller filled from its closed form), or, **if it is constant over the chunk, added as one term** with no buffer. Then each route's loop over its source adds amount × value into its target's per-sample sum, the product in a statement of its own. | Exact: the three deliveries agree to the bit at blocks 1/37/4096. The cheapest everywhere: 4 routes +11.9 ns per voice, 16 routes +33.5, against buffered's +16.6 / +48.6 and inline's +30.0 / +69.1. Moving controllers cost it 1–5 ns. | (a) buffered: simpler, 28–31% dearer. (b) inline: no buffers, 2.1–2.5× the cost. | (ours) |
| 2 | **Running only what is used** | **A mod envelope or an LFO runs only while `uses()` says a route reads it.** Its gates are still tracked. **A route that appears mid-note starts its source at the voice's next grid point, from its note-on state**: an envelope gated on there (and off, if the key is already up), a non-free LFO restarted, **a free LFO copied from the global instance** (exact). The amp envelopes, velocity, keytrack and the global sources cost nothing to keep, and are always there. | Running every source costs 2.3–4.0 ns per voice (1.8–3.1%) on every patch, routed or not, which breaks "the exact 0s cost nothing measurable"; only-used costs 0.0. A start at the grid point is bounded work and clicks less than the route's own step (−77.1 against −22.8 dB). It differs from an always-run envelope for the rest of that note only (−16.6 dB); the next note is exact. Only a player editing the patch while holding a note hears it, and hears the envelope start then. | Every source always (exact for a late route, +1.8–3.1% always). Reconstruct (exact, but a replay of 91–322 µs per voice for a 10 s note, growing with the note's age; bounded only with a closed-form jump in `Adsr` and `Lfo`, which S&H and Lorenz don't allow). The next note (the route silent until then: −6.8 to −9.5 dB). | M15 |
| 3 | **ModWheel and Aftertouch smoothing** | **A linear ramp over 5 ms from where the value stands to each new one, read at absolute samples, as `PitchBend` is**: one global object per controller in the engine, set at each event's sample with value / 127. | −84.7 dB of zipper on a slow 7-bit sweep into Pitch at +1.00 (a step: −62.0), −77.1 on Mix (−50.0); a 0 → 127 step clicks at −43.5 / −38.1 dB (a step: −19.5 / −13.3); a fixed 4.5 ms to 90%; exact in closed form, so independent of the host's blocks (3 of 3) and with an exact bound from its ends. The same law and time as the bend, so one class can serve both. | 10 ms (2.3–5.4 dB better, twice the lag). A one-pole (2 ms: more zipper, −81.7 dB, and longer to 90%, 4.65 ms; it never arrives). A smoother per host block: depends on the blocks and clicks (−4.1 dB). | M20, M21 |
| 4 | **The wheel's resolution, and aftertouch** | **CC 1 as 7-bit** (CC 33 is M1.9's question; the DSP takes a value 0–1). **Channel pressure only**, one global value; polyphonic aftertouch ignored. | The manual names CC 1 and nothing else; a 2008 user's channel aftertouch "did respond"; nothing names polyphonic aftertouch. A per-voice pressure would be a new kind of source (per voice, from MIDI), and nothing asks for it. | Poly aftertouch as a per-voice source (Phase 2). | M20, M21 |
| 5 | **KeyTrack and velocity** | **Per sample**, folded (one term while still). **Keytrack's bound from its parts**: the glide's interval plus the bend's, plus the master tune, through `keytrackSource()`. **Velocity's from its two ends.** | Held velocity on Mix adds −40.9 dB of zipper; per sample costs nothing while still (folded). Keytrack's own ends are too small in 98 of 1.68 million samples (up to 4.5 cents) when a glide and a bend oppose; its parts' ends hold (0). Velocity is one linear ramp: its ends hold (0). | Held at the grid point (zips on gains). Keytrack from its own ends (too small). | (ours) |
| 6 | **Mod envelopes** | **Two `Adsr`s per voice, the same as the amp envelopes in every way** (E9): gated at each note-on, `legatoGateOn()` at a legato note, attacking from their level at a retrigger, released at a note-off, ending at exactly 0, no velocity input, the knob laws the amp envelopes' (M1.9). They don't keep a voice alive: it ends with its amp envelopes. Their bound by stage. | The manual's "similar to the amplitude envelope"; Mono Legato retriggers no envelope. The bound by stage held in every sample (0 of 1.68 million). | Velocity-scaled envelopes (not in Sylenth1). | M14 |
| 7 | **LFOs** | **Two `Lfo`s per voice in segment order (task 2): restarted at a note-on from silence, retriggered (slewed) on a sounding voice, left alone by a legato note.** **Free: copied from the engine's global instance at a note-on from silence, moved on to the voice's own sample, and kept through a steal**; a voice's rate modulation makes the copy its own. The global instances run at their knobs' rates, never synced. Their bound from the segment rendered ahead. | "Always starts on the top peak"; "will not be retriggered" for Free. A copy that is kept is continuous, and one copied again differs (−29.1 dB) by a phase jump. Free under a late route is exact (Q2). A copy taken at the call's start rather than the voice's sample was 7 samples stale. | Copy again at every note-on. | M16, M17 |
| 8 | **AmpEnv A/B as sources** | **Each part's own amp envelope, alone (0–1)**: the voice's existing envelope buffer, read where the output stage reads it. **Both amp envelopes keep running whatever the skip plan does**, as `dsp::Voice` does today. No change to the plan. | Exact in 6 of 6 skip cases; running an unheard part's envelope only as a source saves nothing measurable (125.9 against 128.0 ns). | An unheard part's envelope run only while it is a source (no saving, one more state to track). | M22, AL9 |
| 9 | **Step Velocity until M1.6** | **A global stand-in input**, 0 by default, set by the engine at event samples and read at absolute samples; its bound its two ends. | Nothing in `libs/dsp` makes steps yet; a constant between events keeps the bound exact. The developer's "always on" makes it the arpeggiator's step sequencer running as a source of its own (requirements on M1.6, below). | (none) | M23 |
| 10 | **Each source's bound** | **Task 2's kinds, all of which hold in the voice**: an envelope by stage, an LFO from its segment rendered ahead, a linear source and a controller from their two ends, **keytrack from its parts**. Computed only for sources a route to a factor the skip test reads uses, at the voice's grid points and call starts. | 0 of 1.68 million samples outside for every source over 9 renders at three rates and three block sizes, and three small rules failing (98 to 1,050,084 samples outside). | Each source's range (never too small, never skips a modulated part). | (ours) |
| 11 | **No routes** | **Keep the init patch on `dsp::Voice`'s path**: nothing runs and nothing is summed while the compiled routes are empty, and a target with no routes keeps its unmodulated path. | Identical in 24 of 24 renders under every candidate; 128.7 against 128.7 ns per voice. | (none) | (ours) |

### What the library needs (stage 2)

- **`dsp::Voice`:**
  - one `modulation::RouteCompiler`, as task 2's requirement says;
  - two mod envelopes and two LFOs, run only while used, with the late start of recommendation 2;
  - a `SourceValues` of the sources as they stand at each grid point, for Cutoff, Reso and LFO Rate and Gain;
  - folded per-sample sums of Pitch and Mix per part, and the slots fed a note per sample on a part whose Pitch is routed;
  - Mix through `mixGain({.mix, .modulation})` per sample;
  - `mixModulationBound()` from `targetBound()` with the sources' bounds (keytrack from its parts).
- **What a voice reads that isn't its own** (`VoiceKnobs` or a context like the prototype's `SourceContext`):
  - the engine's `modulation::Patch`;
  - the mod envelopes' and LFOs' settings, or the global ramps they come from (M1.9);
  - the wheel and pressure controllers;
  - the free LFOs' global instances;
  - Step Velocity.
- **A controller ramp:** `PitchBend`'s closed form, generalised to a value 0–1 (or `PitchBend` built on it), 5 ms.
- **`Lfo`:** nothing new. A free copy is the value type copied and run forward.
- **`Adsr`:** nothing new for the recommendation. A closed-form jump ahead (`advance(n)`) only if reconstruction is chosen instead.
- **Tests, as task 2's stage 2 did:**
  - bit for bit this prototype in its decided configuration at host blocks of 1, 37 and 4096, with no allocation;
  - the init patch bit-identical to `dsp::Voice`;
  - each source's values against its definition;
  - each bound against its values over random scenarios, with small bounds failing.

### What this pushes back on

- **M1.6 (the step sequencer):** the developer's "the gate is indeed always on" makes the step sequencer a modulation source that runs whether or not the arpeggiator is on. Step Velocity is one global value, and each step's value arrives as an event at its sample, so that it is constant between events and its bound from its ends stays exact. M23 settles what clocks it (the arp's Time, from the transport?), what a "-" step and a hold step output, and whether a note restarts it. The stand-in's input is the seam.
- **M1.7 (rule 10's global copies):** a free LFO's global instance is the same object the voices copy. The global copies of per-voice sources (an envelope gated by the first key down and the last key up, the last velocity, the last keytrack) are new objects in the engine, cheap and run only while a global destination reads them, as recommendation 2 runs the voices' sources.
- **M1.8 (the engine):**
  - The engine owns the `modulation::Patch` (slot changes are events that split the block), the wheel's and pressure's controllers (each event at its sample), the free LFOs' global instances and Step Velocity.
  - It moves the free LFOs on with each piece it renders. A voice copies one moved on by its own offset into the call, since the instance stands at the call's start.
  - The mod envelopes' and LFOs' knob ramps are global, read at absolute samples, as the filter knobs are.
- **M1.9 (interop):**
  - CC 1 → the wheel (/ 127) and channel pressure → aftertouch. Whether CC 33 refines CC 1 (14-bit) is decided here (M20's hypothesis: ignored).
  - Polyphonic aftertouch is ignored.
  - The mod envelopes take the amp envelopes' knob laws, and the LFOs their L6/L7 laws, with Free and Sync.
  - **A route's amount steps when it changes** (−22.8 dB on a held note when a route appears at Mix −5): decide whether amounts ramp as knobs do (10 ms) or step as a menu choice does. That is a question for task 4 too.
- **Task 4:** reads this task's per-sample sums and sources. The Volume skip test takes `targetBound()` with these bounds.
- **The inventory:** E9, E10, L8, AL9, K3 and F20 are annotated, and M14–M23 are new.

## Tests

Stage 1's prototypes are checked in the default run under `[sources-model]` ([source_candidates_test.cpp](../../tests/dsp/modulation/source_candidates_test.cpp), 4 test cases):

- **No routes:** `SourceVoice` is `dsp::Voice` to the bit under four candidates (folded, used only; buffered and inline, every source always; an unheard amp envelope only as a source), on three patches at host blocks of 512 and 37, with no allocation.
- **The three deliveries:** under both run policies they give the busy patch's output to the bit at host blocks of 1, 37 and 4096, with no allocation; the patch differs from no routes.
- **Every bound** holds over random scenarios at 44.1, 48 and 96 kHz and host blocks of 1, 37 and 4096 (over a million samples checked). Keytrack from its own ends, every source at its start value, and an LFO from its ends each fail.
- **A source reconstructed** when its route appears mid-note, after a steal, is the always-run source to the bit, for a mod envelope, an LFO and a free LFO. Started at the next grid point instead, it is exact only for the free LFO.

The measurements are the hidden `[sources-report]`; the CPU numbers are `[!benchmark][sources]` and [baselines.md](../performance/baselines.md#2026-11-22-m15-the-sources-stage-1-prototypes).

## References

- LennarDigital, *Sylenth1 User Manual* v2.5 (2015), pp. 5, 19–22, 29, 31.
- LennarDigital, v1.1 notes ([mirror](https://www.svartling.net/2006/11/lennardigital-sylenth1-v11-pc.html), 2006); the [official changelog](https://lennardigital.com/sylenth1/whatsnew.php) (v3.008).
- Lennard (the developer) and Exhile on KVR, "Where's the gate sequencer?", [t=236247](https://www.kvraudio.com/forum/viewtopic.php?p=3367390) (16 Dec 2008): Step Velocity as an always-on gater.
- JimmiG and Lennard on KVR, "Sylenth1, Aftertouch in Live?", [t=219110](https://www.kvraudio.com/forum/viewtopic.php?p=3204446) (2008): channel aftertouch as a source, and not a parameter.
- Seventh7 on KVR, [p=4235806](https://www.kvraudio.com/forum/viewtopic.php?p=4235806) (2010): a non-free LFO restarts at a note.
- [ADSR Sounds: activating the mod wheel in Sylenth](https://www.adsrsounds.com/sylenth-tutorials/activating-mod-wheel-in-sylenth/): the wheel through the Misc slots.
- Every other source as cited in [modulation.md](modulation.md) and the inventory.
