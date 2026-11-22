# Part solo, and skipping what can't be heard

Solo plays only the part the interface shows. Separately, the voice should not compute what nobody can hear: Sylenth1 "will also automatically turn off any parts that are not used, to save extra CPU time" (manual p. 6). This page covers M1.4 task 5, stage 1 and the build (stage 2):

- **Theory:** solo as a gain or as a routing change; skipping as proving silence with an upper bound; what has to stay exact through a skip (a filter's state, an oscillator's phase, the ramps); when the proof is made, and why that decides whether the output depends on the host's block size; the mono path; and a bound that modulation can join later.
- **Sylenth1:** what is known about Solo and the skipping. The manual gives Solo in two sentences, a tutorial says Solo follows Part select, another says Solo taps the filter's output, and the developer says a part with no oscillators has its filter "off".
- **The candidates:** prototyped on copies of the real filter, slot and voice, over the real `dsp::VoiceAllocator`.
- **What they measured:** the prototype against the library; Solo's two readings and its clicks; every V13 case under each skip candidate, exact or by how much; an oscillator's phase through a skip; when the test is made; bounds on modulation; the mono path; the real-time terms; CPU.
- **Recommendations** for the project owner, one per question, with the alternatives, and **the decision** (every recommendation, 2026-11-22).
- **As built** (stage 2): the library's API, the tests and their tolerances, red and the deliberate breaks, three flaws the build found in the prototype, and the results.

Read [voice-architecture.md](voice-architecture.md) (the voice, its routing, its 32-sample grid, and question 4's mono path), [mixer.md](mixer.md) (Mix per voice per sample, and "Room for tasks 5 and 6"), [voice-allocation.md](voice-allocation.md) (`isActive()`, E11, a retrigger in place keeping the filters) and [filters.md](filters.md) (the filter's grid and ramps) first.

## Theory

### Solo: a gain or a routing change

Solo makes one part inaudible. A voice can do that in two places:

- **As a gain.** Multiply the other part's output by 0, after its filter and amp envelope, where Mix already acts (mixer.md). A gain can ramp, so switching it need not click. The part keeps running underneath, so it comes back exactly as it would have been.
- **As a routing change.** Treat the other part as if no filter took it (its Input Select `--`). Nothing runs, so nothing costs, but the change is a step: the part stops at once, and when it comes back its filter starts from rest, at the voice's next grid point (voice-architecture.md).

A gain at 0 costs as much as a gain at 1 unless the voice also notices that the output is 0 and skips the work. That is the second half of this task, so the two are measured together.

### Skipping is proving silence

A voice may skip a computation only if it can prove the result won't be heard. For a part, the output is a product: filter output × amp envelope × Mix gain × solo gain. If an **upper bound** on any factor's magnitude over a stretch of samples is 0, the product is exactly 0 there, and skipping the multiply-add changes nothing: adding ±0 to a sample leaves it as it is. The bounds are cheap where the factors are simple:

- a linear ramp (a knob, Solo) is largest at one of its ends;
- an amp envelope can only rise from 0 at a gate, which is an event: idle, or held at a sustain of exactly 0, it stays 0 until then;
- a modulation source's bound is its range, or tighter, its largest value over the stretch.

The proof must be conservative: a bound that is too large only skips less, while one that is too small skips audio.

### What has to survive a skip

Skipping the *output* of a part is exact. Skipping its *computation* is exact only if, when the part comes back, everything it carries from the past is what it would have been:

- **A filter's state** depends on everything it has seen. A filter at rest (every state exactly 0) fed exact zeros stays at rest and outputs zeros, so skipping it changes nothing. A filter that is fed a sounding oscillator while its output is muted keeps changing. Skipped, its state is either kept as it was (**frozen**) or cleared to rest when it comes back; either way it differs from the state it would have had, until the difference decays. A resonant filter decays slowly.
- **The filter's ramps and grid.** The filter interpolates its cutoff and resonance across 32-sample segments and ramps drive and response per sample. A skipped filter has to advance them, or it comes back out of step with the voice's feeds.
- **An oscillator's phase.** A free-running oscillator has been running unheard, so its phase when it returns is where the skipped samples took it. A fixed-point phase accumulator makes that exact: n samples of an increment Δ move the phase by n·Δ modulo 2³². What can't be recovered without rendering is the band-limiting residual of the edges in the last 64 samples before the return. A band-limited jump to the advanced phase confines the difference to those 64 samples.
- **Ramps that move.** A gain ramp in a slot, a Mix ramp, or Solo's ramp must be at the same place after the skip as without it. A global ramp read at absolute offsets (`valueAfter`) always is. A per-voice ramp has to be advanced, or the voice doesn't skip while it moves.

### When the proof is made

A proof about "the next N samples" must hold for all N of them. It can be made:

- **At every chunk**, where the voice's 32-sample grid or a `process()` call splits the block. A call boundary comes from an event, or simply from where the host's block ends. A skip that isn't exact (a frozen filter) then starts at a sample that depends on the host's block size, and so does the output.
- **At the voice's grid points only**, over the whole coming segment. Between grid points a new call can only *end* a skip, if its re-test over the rest of the segment fails. Without an event the bound over part of a segment can't exceed the bound over all of it, so only an event ends a skip mid-segment. Events sit at the same samples whatever the block size, so the output is independent of the block size even for a skip that isn't exact.
- **Once, at the note-on.** This is cheap, but wrong for anything that changes during the note.

### The mono path

The slot's output is stereo. If every oscillator voice sits exactly at the centre, left and right are the same to the bit, and so is everything downstream, so the voice can filter one channel and copy it (voice-architecture.md, question 4). To leave mid-note, the filter's channel-1 state is set from channel 0's at that sample. This is exact because they were equal until then.

## Sylenth1: what is known

The full source table with ratings is in the inventory: [Part solo and skipping unused parts](../reference/sylenth1-features.md#part-solo-and-skipping-unused-parts) (PS1–PS5, SK1–SK7), with §1's Solo bullet and "Unused parts" row, V12 and V13 annotated. In short:

- **The manual.**
  - pp. 9–10 (3.2, "Working with parts"): "You can switch between the two parts using the Part Select buttons shown on the top bar on user interface. It is easy to see which part is currently shown on the interface…" Then: "Right next to the part select buttons you'll find a button named Solo. Whenever this button is activated, you will hear only the sound of the currently selected part. This is very useful if you are working on complex sounds."
  - p. 6: it "will also automatically turn off any parts that are not used, to save extra CPU time."
  - p. 30: "If you don't use the filters, switch them to bypass mode. This will force them to stop processing."
  - pp. 31–32: no Solo or Part select in the default CC table.
- **Tutorials.**
  - MusicRadar (2015): "Each Part can be auditioned in isolation using the Solo button at the top". The tutorial adds that it stays engaged when the Part selection changes.
  - Syntorial (2015), video transcript: "Now, when you use this solo button where you're actually soloing is the filters output". With filter A on AB, soloing part A, "we're only hearing what comes out of filter A which in this case is the oscillators from both part A and part B".
- **The developer.**
  - 2010, on presets whose Osc 3 and 4 are at Voices 0: "In that case filter B is off, so there's nothing to modulate" ([p=4288321](https://www.kvraudio.com/forum/viewtopic.php?p=4288321)).
  - 2010: "Sylenth1 fully supports both parameter automation and midi automation on all its controls" ([p=4289093](https://www.kvraudio.com/forum/viewtopic.php?p=4289093)).
- **The changelog** never mentions Solo. v3.050 "Improved host automation for buttons and option controls"; v3.053 "Added locking options for all parameters and parts"; v3.000 and v3.050 optimised the CPU, with no detail.
- **Unknown:**
  - whether Solo is saved with a preset or automatable;
  - how it switches (a ramp or a step);
  - whether it acts on release tails;
  - what a part sounds like when it comes back from Mix 0;
  - which unused cases, besides a part with no oscillators, Sylenth1 skips.

So, rated in the inventory: **Solo plays the selected part only** (high). **It follows Part select while on** (medium: one tutorial). **It silences the other part's filter output**, so a part crossing into the selected filter still sounds (medium-low: one tutorial, by ear). **A filter whose part has no oscillators isn't run** (medium-high: the developer). **Whether Solo is a parameter** (low).

## The candidates

In [solo_candidates.hpp](../../tests/dsp/voice/solo_candidates.hpp), on two copies of the library's classes with what skipping needs added:

- [skip_ladder_filter.hpp](../../tests/dsp/voice/skip_ladder_filter.hpp): `LadderFilter` as built, plus:
  - `skip(n)`: n samples without solving, with the grid and the ramps advanced exactly as `process()` would;
  - `clearState()`;
  - `copyChannel(from, to)`.
- [skip_oscillator_slot.hpp](../../tests/dsp/voice/skip_oscillator_slot.hpp): `OscillatorSlot` as built, plus:
  - `isSilent()`: every rendered gain is settled at exactly 0;
  - `isCentred()`: each voice's gains, current and target, are equal left and right, and so is Noise's mix;
  - `canSkip()`: no gain ramp is moving;
  - `skip(n, note)`: the phase steps added up in `uint32`; at the next `process()`, each oscillator jumps to the phase it would have had (`MinBlepOscillator::jumpTo`), or, for contrast, carries on from where it stopped.

**`SkipVoice`** is a copy of `dsp::Voice` (tasks 1–4) on those two. With nothing switched on it is `dsp::Voice` to the bit (measured below). **`SoloEngine`** runs the real `dsp::VoiceAllocator` over 16 of them. It holds the Mix A/B ramps and a solo gain ramp per part (global `LinearSmoother`s read with `valueAfter`, as the Mix knobs are), and events for Mix, Solo, Part select, and a slot's Volume and Pan, splitting each host block at them.

| Question | Candidates |
|---|---|
| **Solo: how it switches** | a gain per part (0 or 1) ramped over 10 ms, multiplied into the part's Mix gain per sample; the same gain instant; a routing change (the soloed-out filter as `--` at the voice's next grid point, from rest when it returns) |
| **Solo: what it silences** | the other part's filter output (Syntorial's reading); that, and its oscillators where they cross into the selected part's filter (AB) |
| **What a part with a silent output skips** (`FilterSkip`) | nothing (as built); the filter only while its input is exact zeros and it is at rest (**atRest**, exact by construction); also the filter whenever the part's output gain is 0, its state **frozen**, or **cleared** to rest when it comes back |
| **Oscillators** | rendered as built; skipped when no running filter takes them or their gain is settled at 0, the phase **advanced** (a band-limited jump at the return) or **frozen** |
| **When the test is made** | per chunk; **per segment** (a skip starts only at a grid point; mid-segment it can only end); per segment with hysteresis (skipped for its gain only after 128 samples of 0); once at the note-on |
| **The modulation's bound** | a stand-in for M1.5 on Mix B: a free LFO or a per-note pluck (`amount · e^(−n/τ)`); bounded **conservatively** (the LFO's ±amount, the pluck's value at the chunk's start) or **tightly** (the LFO's largest value over the chunk: its ends, or a crest inside) |
| **The mono path** | decided at a note-on from silence when every sounding slot is centred; left at the first chunk where one isn't, with each filter's channel 1 set from channel 0; never re-entered within the note |

**The skip test**, per part and per segment, is that the part's output gain can't be above 0. It holds when any one of these does:
- its amp envelope is idle, or held in sustain at exactly 0 with a sustain target of 0;
- Mix's ramp at its larger end, plus the modulation's bound, is 0 or less;
- Solo's ramp at its larger end is 0.

What follows from the test:
- **The output stage.** When the test holds, the part's filter output isn't added.
- **The filter.** It is skipped while it is at rest and its input can't be non-zero (no sounding, non-silent slot in its part, and nothing crossing). Under freeze and clear it is also skipped whenever the test holds.
- **The slots.** They are rendered only for a filter that runs (or a bypassed one that is heard). Otherwise they are skipped, if allowed and if no ramp moves.
- **A crossing part.** It isn't added into the other filter while its envelope (or, in the second reading, its Solo) is 0.

A skipped filter is still fed at its grid points and its ramps advance, and the envelopes always run.

**Patches.**
- *Init*: A1 one saw, both filters on their own parts at the Filter Control's defaults; filter B runs on part B's silence in `dsp::Voice`.
- *Two parts*: A1 and B1 saws, B1 a fifth up, own filters; *resonant*, the same at k 3.6.
- *Crossing*: filter A on AB, filter B on B.
- *Worst*: 4 × 8 unison saws, spread or centred.
- *Pad*: A1, A2 and B1 unison into filter A on AB, resonant, with a release.
- *Noise*: A1 Noise.

Drift is on at 1.5 cents.

## What they measured

By the hidden `[solo-report]` ([solo_report.cpp](../../tests/dsp/voice/solo_report.cpp), about 25 s) and the `[solo]` benchmarks ([solo_benchmark.cpp](../../tests/dsp/benchmarks/solo_benchmark.cpp)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[solo-report]"
tools/bench.sh "[solo]"
```

### The prototype against the library

`SkipVoice` with nothing switched on, through `VoiceAllocator`, against `dsp::Voice`. The sequence: Polyphony 4, two steals, 60,000 samples. The patches: init, init with filter B on `--`, two parts, crossing, pad, and worst spread. Host blocks of 512 and 37: **12 of 12 identical to the bit.** The copies of the filter and slot are the library's to the bit wherever they don't skip.

### Solo: its two readings

Crossing patch (filter A on AB), Solo on from the start, settled. The output's RMS over 0.1–0.9 s, and B1's share of it (the output less the same render with B1 at Voices 0), both in dB re the output without Solo:

| Reading | Solo | Output | B1 in it |
|---|---|---:|---:|
| (no Solo) | off | 0.0 | −1.1 |
| **filter output** (Syntorial) | A | −4.0 | **−7.1** |
| | B | −7.0 | −7.0 |
| part sources | A | −6.9 | **exact 0** |
| | B | −7.0 | −7.0 |

The readings separate exactly where the tutorial says they do: under the filter-output reading, Solo A still plays B1 through filter A, at −7.1 dB. PS2's protocol is a single listen. With both parts on their own filters, Solo A is **identical to the bit** to part B having no oscillators at all.

### Solo: clicks

A1 and B1 sines at A3 and E4, Volume 0.25 (near-linear through LP24 at 4,677 Hz, so no energy above 1 kHz of their own), own filters, release 0.5 s. The events:
- a note held from 0;
- Solo on (A) at 0.5 s;
- Part select to B at 1.0 s;
- Solo off at 1.5 s;
- the note-off at 2.0 s;
- Solo on at 2.2 s, in the release.

Each event sits a few samples off the grid. The click is filters.md's measure: energy above 1 kHz in a Hann 4096 around the event, in dB re a unit step:

| Solo | on | Part select | off | on, in a release |
|---|---:|---:|---:|---:|
| **gain, ramped 10 ms** | **−52.7** | **−47.8** | **−53.9** | **−77.6** |
| gain, instant | −24.0 | −9.2 | −18.2 | −48.6 |
| routing (at the grid point, from rest) | −15.0 | −14.1 | −19.5 | −52.4 |
| no Solo (the floor) | −69.9 | −68.4 | −64.1 | −90.1 |

- **A 10 ms ramp keeps Solo within 10–21 dB of the floor**, against 42–59 dB above it for a step. Part select under Solo is the loudest event for a step (−9.2 dB): one part stops and the other starts in the same sample.
- **Routing clicks like a step** (−14 to −20 dB), and its return also restarts the filter from rest.
- **On a release tail** everything is quieter, but the order is the same.

### Skipping: each case

A three-note chord held 1 s, host blocks of 512. The reference is the prototype with nothing skipped (the library, to the bit). The columns:
- *Filters* and *slots*: the share of samples skipped.
- *After the return*: where the part comes back, the difference's RMS over the next 50 ms, dB re the reference's.
- *Click*: the difference's energy above 1 kHz around the return, beside the reference's own there. On a saw that is the signal's own high end, so it measures masking, not a click.
- *Until*: the last differing sample after the event.

| Case | Candidate | Filters | Slots | Identical | After the return | Click (reference) | Until |
|---|---|---:|---:|---|---:|---:|---:|
| init (B at Voices 0, filter B on B) | every candidate | 50.0% | 0% | **yes** | | | |
| filter B `--` | every candidate | 0% (as built) | 0% | yes | | | |
| every oscillator at Voices 0 | every candidate | 100% | | yes | | | |
| crossing, env B held at sustain 0 | atRest | 0% | 0% | yes | | | |
| | freeze or clear + slots | 47.5% | 47.5% | **yes** | | | |
| B1 at Volume 0, then 1 at 0.5 s | atRest (slots rendered) | 25.0% | 0% | yes | | | |
| | any + slots, phase advanced | 25.0% | 25.0% | no | −82.4 | −56.1 (+12.3) | 122 |
| | phase frozen | 25.0% | 25.0% | no | **+0.4** | +11.6 | the end |
| Mix B 0, then 10 at 0.5 s | atRest (+ slots) | 0% | 0% | yes | | | |
| | freeze or clear + slots | 25.0% | 25.0% | no | −68.4 | −43.4 (+12.2) | 208 |
| the same, resonant (k 3.6) | freeze or clear + slots | 25.0% | 25.0% | no | −41.0 | −24.2 (+7.8) | 1,622 |
| Mix B 10, to 0 at 0.25 s, back at 0.5 s | freeze + slots | 12.0% | 12.0% | no | −64.0 | −38.5 (+12.2) | 208 |
| | clear + slots | 12.0% | 12.0% | no | −70.4 | −44.8 (+12.2) | 208 |
| the same, resonant | freeze + slots | 12.0% | 12.0% | no | −39.4 | −22.5 (+7.8) | 1,622 |
| | clear + slots | 12.0% | 12.0% | no | −43.8 | −26.9 (+7.8) | 1,857 |
| Solo A, off at 0.5 s | freeze or clear + slots | 25.0% | 25.0% | no | −68.4 | −43.4 (+12.2) | 207 |
| env B held at sustain 0, retriggered in place at 0.5 s (Polyphony 1) | freeze + slots | 45.0% | 45.0% | no | −36.6 | −17.3 (+5.2) | 76 |
| | clear + slots | 45.0% | 45.0% | no | −30.3 | −9.4 (+5.2) | 76 |

(atRest, with or without slots, is identical to the bit in every case; the phase-frozen rows of the Mix and Solo cases are +0.4 to −0.9 dB, to the end.)

- **The exact skips are large where they apply.** The init patch's filter B (half its filter work) runs on exact zeros and is skipped to the bit, as the developer's "filter B is off" describes. The same proof covers every oscillator at Voices 0, and a crossing part whose envelope holds at 0.
- **A part whose gain is 0 but whose filter is fed** (Mix 0, Solo, an envelope held at 0) can only be skipped with a difference after it returns. While it is skipped the output is identical. After the return, a plain saw through LP24 differs by −64 to −70 dB over 50 ms, and is exact after 208 samples (4.3 ms). The resonant filter differs by −39 to −44 dB for 1,622–1,857 samples (34–39 ms, its ring's decay). An in-place retrigger after an envelope held at 0 (attack 0, so the note's own onset is a step) differs by −30 to −37 dB for 76 samples. Every difference is 15 to 68 dB below the signal's own energy above 1 kHz in the same window (22 to 68 dB when frozen).
- **Frozen or cleared**: clearing is 4–6 dB closer after a Mix or Solo dip (the Mix ramp from 0 meets a filter warming up from rest), while freezing is 6 dB closer, with an 8 dB smaller click, at an in-place retrigger. A part muted from its note-on is the same either way.
- **The oscillator's phase must advance.** Frozen, a free-running part comes back at the wrong phase for good (+0.4 to +0.8 dB of difference to the end).

### An oscillator through a skip

Both filters bypassed, so only the slot differs. B1 sits at Volume 0 from the note-on (its slot skipped while its gain is settled at 0), then goes to 1 at 0.5 s + 7, its gain ramping over 10 ms:

| B1 | Phase through the skip | Peak difference | RMS over 50 ms, re the reference | Differs until |
|---|---|---:|---:|---:|
| free-running | **advanced** (a band-limited jump at the return) | 0.0031 | **−74.7 dB** | 63 samples |
| | frozen | 1.29 | +0.9 dB | the end |
| Retrig on | advanced | 0.0016 | −80.7 dB | 60 samples |
| | frozen | 1.29 | +0.4 dB | the end |

With the phase advanced, the only difference is the band-limiting residual of the edges inside the last 64 skipped samples, confined to the 64 samples after the return, and scaled down by the gain ramp rising from 0. Retrig on is no different: the note-on that restarted the phase was at 0 s, and the free run since then is what is skipped.

### When the test is made

A1 and B1 saws, a chord held 2 s. (1) Mix B at 0, raised to 10 at 0.5 s. (2) Mix B at 0 with a free 2 Hz LFO of ±5 on it, tightly bounded. The difference is over the whole render, dB re the reference:

| Sequence | Candidate | Timing | Filters skipped | Returns | Identical | Difference |
|---|---|---|---:|---:|---|---:|
| (1) | atRest + slots | per chunk, per segment, hysteresis | 0% | 0 | yes | |
| | | **at the note-on** | 0% | 0 | **no** | **−3.4** |
| | freeze + slots | per chunk / **per segment** | 12.5% | 3 | no | −83.2 |
| | | hysteresis 128 | 12.4% | 3 | no | −77.1 |
| | | at the note-on | 50.0% | 0 | no | −3.4 |
| | clear + slots | per chunk / per segment | 12.5% | 3 | no | −83.2 |
| | | hysteresis 128 | 12.4% | 3 | no | −79.2 |
| (2) | atRest + slots | per chunk, per segment, hysteresis | 0% | 0 | yes | |
| | | at the note-on | 0% | 0 | no | −12.6 |
| | freeze + slots | per chunk / per segment | 24.9% | 9 | no | −99.7 |
| | | hysteresis 128 | 24.7% | 9 | no | −99.6 |
| | clear + slots | per chunk / per segment | 24.9% | 9 | no | −101.6 |
| | | hysteresis 128 | 24.7% | 9 | no | −101.5 |

- **Deciding at the note-on is wrong**: a part muted at its note-on stays silent when Mix comes back (−3.4 dB: part B is missing), and an LFO's open half is lost (−12.6 dB).
- **Hysteresis buys nothing**: the same number of returns (3, 9), the skip shortened by 0.1–0.2%, and a slightly larger difference, because a skip that starts later carries more state.
- **Per chunk and per segment give the same here** (host blocks of 512 against 2 s of slow events). They differ on the host's block size, below.

### Bounds on modulation (M1.5)

A1 and B1 saws, Mix B at 0 with modulation on it. atRest + slots skips only the output stage on the gain's test, which is exact if the bound holds. Freeze + slots also skips the filter.

| Modulation of Mix B | Bound | atRest: outputs skipped | Identical | freeze: filters skipped | Identical |
|---|---|---:|---|---:|---|
| free LFO 2 Hz, ±5 | conservative (±5) | 0% | yes | 0% | yes |
| | **tight** (its largest over the segment) | **24.9%** | **yes** | 24.9% | no (−111.6 dB) |
| the same, Mix B at 5 | either | 0% | yes | 0% | yes |
| pluck +10, τ 20 ms, a note every 0.25 s | either | 0.3% | yes | 0.3% | yes |

- **The tight bound is exact** (identical with every output it proved silent skipped) and finds the LFO's closed half, which the conservative bound never does. For a sine it is the segment's ends, or ±amount if a crest falls inside.
- **An exponential never proves 0**: the pluck's tail is above 0 for as long as anyone waits (the 0.3% is the gaps between notes). M1.5's mod envelopes end at exactly 0, as the amp envelope does (E10), so a bound on them can.

### The mono path

Mono on against mono off (the same skipping), the off render at host blocks of 4096 the reference, the on renders at 1, 37 and 4096. Two sequences: a four-note chord held 0.5 s, and the identity sequence's notes (Polyphony 4, two steals) with A1's Pan moved to 0.3 at 0.2 s + 5. Each with no skipping and with atRest + slots:

| Patch | Mono path taken | Left at the Pan move | Identical |
|---|---|---|---:|
| init | yes | 3 notes | 12 of 12 |
| worst, centred | yes | 3 notes | 12 of 12 |
| pad, centred (AB, resonant, attack and release) | yes | 3 notes | 12 of 12 |
| worst, spread / pad, spread | never | | 24 of 24 |
| A1 Noise, centred | never | | 12 of 12 |

**72 of 72 renders are identical to the bit** to the stereo reference, and left equals right to the bit wherever the path is taken. Two things were settled on the way:

- **The output stage must use the stereo path's expression** on each channel (`left[i] += ge * y0[i]`), not a product rounded once and added to both. Apple Clang contracts the stereo expression into a fused multiply-add (`-ffp-contract=on` is its default), so a separately rounded product differs by an ulp wherever the envelope isn't 1. The pad found it (0 of 12 until fixed); the init patch, at gates, can't.
- **Noise is never centred**: the slot mixes two independent draws per sample into left and right. A centred Noise oscillator keeps its voice stereo.

### Real-time terms

16 voices through the engine for 1.3 s at Polyphony 16 (the identity sequence's notes and a chord of 8 held from 0), with these events, all off the grid:
- Mix B to 0 at 0.3 s, back to 10 at 0.6 s;
- Solo A at 0.7 s, Part select B at 0.8 s, Solo off at 0.9 s;
- B1's Volume to 0 at 1.0 s and back at 1.1 s;
- A1's Pan to 0.2 at 1.2 s.

Host blocks of 1 and 37 against 4096, Solo as a gain and as routing:

| Candidate (with mono) | Identical at 1 and 37 | Allocations |
|---|---:|---:|
| nothing skipped | 4 of 4 | 0 |
| atRest + slots, per chunk / per segment | 4 of 4 / 4 of 4 | 0 |
| freeze + slots, **per chunk** | **0 of 4** | 0 |
| freeze + slots, **per segment** | **4 of 4** | 0 |
| clear + slots, per chunk / per segment / hysteresis | 0 of 4 / 4 of 4 / 4 of 4 | 0 |

**A skip that isn't exact must start only at the voice's grid points.** Per chunk, a frozen or cleared filter starts its skip where the host's block happens to end, so the output depends on the block size. Per segment it doesn't (20 of 20 for the per-segment candidates). Nothing allocates.

### CPU

`tools/bench.sh "[solo]"`, `relwithdebinfo`, Apple M1 Ultra (virtualised), 16 voices held (keys 40–55) through `VoiceAllocator`, 512 samples per run at 48 kHz, the median of three runs (spread 0.3–4.8%, one row 8.5%), ns per sample for all 16 and per voice:

| Case | As built (`dsp::Voice`) | Candidate | ns per sample (16 voices) | per voice | vs as built |
|---|---:|---|---:|---:|---:|
| init | 5,762.3 | prototype, nothing skipped | 5,796.4 | 362.3 | +0.6% |
| | | **atRest** | **3,831.6** | **239.5** | **−33.5%** |
| | | mono only | 3,025.2 | 189.1 | −47.5% |
| | | atRest + slots + mono | 2,012.6 | 125.8 | −65.1% |
| filter B `--` | 3,807.2 | atRest + slots + mono | 1,988.1 | 124.3 | −47.8% |
| Mix B 0 (two parts) | 7,574.8 | nothing skipped | 7,533.0 | 470.8 | −0.6% |
| | | atRest + slots | 7,527.2 | 470.4 | −0.6% |
| | | **freeze + slots** | **3,840.0** | **240.0** | **−49.3%** |
| | | freeze + slots + mono | 2,007.5 | 125.5 | −73.5% |
| B soloed out (two parts) | | gain, nothing skipped | 7,516.0 | 469.8 | |
| | | gain, freeze + slots | 3,827.4 | 239.2 | −49.1% vs the gain alone |
| | | routing | 3,793.5 | 237.1 | −49.5% vs the gain alone |
| two parts sounding, centred | 7,569.0 | freeze + slots + mono | 3,941.9 | 246.4 | −47.9% (the mono path) |
| every oscillator at Voices 0 | 0.1 | atRest | 0.1 | 0 | (voices freed at once) |
| worst, centred | 10,259.1 | mono only | 6,088.8 | 380.5 | −40.6% |
| | | freeze + slots + mono | 6,065.7 | 379.1 | −40.9% |
| worst, spread (nothing to skip) | 10,139.9 | nothing skipped / freeze + slots + mono | 10,134.2 / 10,105.2 | 633.4 / 631.6 | within noise |

- **The exact skip of a filter on silence takes a third off the init patch** (360 → 240 ns per voice). That is task 1's 126 ns for a filter on silence.
- **Mix B at 0 or Solo saves nothing unless the fed filter is skipped too**: atRest is within noise of nothing skipped, because the filter still runs. Freeze (or clear) halves the voice: the 233 ns per voice of part B's filter and oscillator, more than the mixer's estimate of 110 ns, which was measured with B1 off rather than at Mix 0. Solo as a gain plus freeze costs the same as Solo as routing (3,827 against 3,794, within noise), so routing's CPU is no reason to choose it.
- **The mono path saves 47.5% on the init patch and 40.6% on the worst case centred**, the share of the filters' second channel, as task 1 measured (49% and 42%). With the exact skip, the init patch costs 126 ns per voice, a third of today's.
- **The test costs nothing measurable**: where nothing can be skipped (worst, spread), the prototype is within noise of `dsp::Voice` (−0.3%), and the init patch with nothing switched on is +0.6%.
- **V12's "every oscillator at Voices 0" costs nothing in ours already**: `isActive()` is false from the note-on, so the allocator frees each voice at its first check.

## Recommendations

*Stage 1's recommendations, as written. Where only the demo can settle a question, the recommendation is the working hypothesis in the inventory (PS1–PS5, SK1–SK7), and the measurement is what makes it the safer default. Chosen by the project owner on 2026-11-22, every one ([The decision](#the-decision)).*

| # | Question | Recommendation | Why | Alternatives | Settled by |
|---|---|---|---|---|---|
| 1 | **What Solo silences** | **The other part's filter output.** A part crossing into the selected part's filter (AB) is still heard through it. | Syntorial's transcript ("where you're actually soloing is the filters output", and both parts heard through filter A); the manual's "the sound of the currently selected part" is that part's output. Measured: the readings separate to exact 0 (B1 at −7.1 dB against silence), so one listen on the demo settles it. | Also silence its oscillators where they cross (B1 exact 0 under Solo A). | PS2 |
| 2 | **How Solo switches** | **A gain per part, 0 or 1, ramped linearly over 10 ms** (the Mix knobs' ramp), **multiplied into the part's Mix gain per sample**, on held notes and release tails alike. | −52.7 / −53.9 dB of click on and off, against −24.0 / −18.2 for a step and −15.0 / −19.5 for routing. The part underneath runs on (or is skipped, row 6), so it comes back as it would have been. It is Mix's mechanism, with no new path through the voice. | Instant; routing (a step, and a filter restarted from rest at its return). | PS3 |
| 3 | **Solo and Part select** | **Solo follows Part select while it is on**: changing the selection ramps both gains over 10 ms. | MusicRadar (medium). A step there is the worst click measured (−9.2 dB); the crossfade −47.8. | Solo latches the part it was turned on with. | PS1 |
| 4 | **Is Solo a parameter** | **Interface state, not a preset parameter**: not saved in presets, not host-automated, not MIDI-mapped, kept with the host's session. `libs/dsp` takes Solo as two gains (or Solo and the selected part), so either answer to PS4 changes nothing in the DSP. M1.9 decides where it is stored. | The manual puts it with Part select (which part the interface shows) as an editing aid ("very useful if you are working on complex sounds"); no CC; the changelog never mentions it. A preset that loads soloed would be a trap. | An automatable parameter (the developer's "all its controls", 2010). | PS4 |
| 5 | **What is skipped exactly** (always) | **(a) A filter at rest whose input can't be non-zero** (no sounding, non-silent slot in its part, nothing crossing in): not solved, its grid and ramps advanced, still fed at its grid points; it runs again the moment an input can sound. **(b) A part's output stage when its gain bound is 0.** **(c) A crossing part whose envelope (or Solo, under 1's alternative) is 0** isn't added into the other filter. Plus what is skipped today: `--` filters and parts no filter reads. | Exact by construction, and measured: identical to the bit in every case and at every block size. The init patch's filter B is a third of the voice (360 → 240 ns): the developer's "filter B is off". | Nothing beyond today's (−33.5% left on the init patch). | SK1, SK5 |
| 6 | **A part whose gain is 0 but whose filter is fed** (Mix with its modulation's bound at 0, soloed out, its amp envelope idle or held at sustain 0) | **Skip its filter and its oscillators while the gain's bound is 0 over the segment; keep the filter's state (frozen) for its return.** | Halves such a voice (473 → 240 ns per voice), as Mix 0 and Solo are meant to be used. While skipped, the output is identical. After the return the difference is −64 to −70 dB over 50 ms for a plain saw (gone in 4.3 ms), −39 to −44 dB on a resonant filter (34–39 ms), and −30 to −37 dB at an in-place retrigger. Frozen, each is 22 to 68 dB below the signal's own high end there. Frozen rather than cleared: the worse of the two cases, an in-place retrigger, is 6 dB closer (click −17.3 against −9.4); it needs no code at the return; and it matches task 2's choice to keep the filters at a retrigger. | **Exact only** (row 5): no saving at Mix 0 or under Solo (7,527 against 7,575). **Cleared** at the return: 4–6 dB closer after a Mix or Solo dip, 6 dB further at a retrigger. | SK2, SK3, SK6 |
| 7 | **An oscillator through a skip** | **Its phase advances**: the phase steps of the skipped samples are added up (exact in `uint32`), and at the return each oscillator jumps there with `MinBlepOscillator::jumpTo`. A slot is skipped when no running filter takes it or its gain is settled at exactly 0 (Volume 0), and only while no gain ramp in it moves. | A free-running phase carries on unheard (O10's premise). The difference is the band-limiting residual of the last 64 skipped samples, at −74.7 dB (−80.7 with Retrig) and 63 samples, scaled down by the gain ramp. Frozen is wrong for good (+0.9 dB). | Render a slot at Volume 0 (exact; its saving is a few ns per unison voice). Frozen phase (no). | SK4 |
| 8 | **When the test is made** | **Per segment**: at each of the voice's 32-sample grid points, for the whole coming segment. A `process()` call mid-segment re-tests only a part being skipped, over the rest of the segment, and can only end its skip. **No hysteresis.** | Row 6's skip isn't exact, so it must start at a sample that doesn't depend on the host: per chunk, 0 of 4 renders matched at blocks 1 and 37; per segment, 20 of 20. Without an event the rest of a segment can't have a larger bound, so only an event ends a skip mid-segment. Hysteresis changed no return count (3, 9) and cost 0.1–0.2% of the skip. At the note-on is wrong (−3.4 dB: a muted part stays muted). | Per chunk (exact skips only); hysteresis; at the note-on. | (ours) |
| 9 | **The bound (M1.5)** | **The test takes an upper bound per factor over the segment**: the amp envelope (0 when idle or held at 0), the Mix ramp's larger end plus the sum over the voice's modulation routes to that Mix of the bound of amount × source, and Solo's ramp. **Each M1.5 source gives its bound over a segment**: an LFO its largest value (its ends, or ±1 if a crest falls inside), a mod envelope its level when idle or held at 0 (exact, as E10's amp envelope), anything else its range. | The tight LFO bound is exact and finds the closed half (24.9% of outputs skipped, against 0% for ±amount). Adding a modulation term is one more interval in the sum, with no change to the test. An exponential tail never proves 0, so M1.5's envelopes must end at exactly 0 to be skippable. | Every source bounded by its range (exact, skips less). | (ours) |
| 10 | **The mono path** | **As decided in task 1**: at a note-on from silence, if every sounding slot is centred (its gains, current and target, equal left and right to the bit; Noise never), the filters' inputs, the filters and the output stage run on channel 0, written to both channels **with the stereo path's own expression**. It is left at the first chunk where a slot isn't centred, each filter's channel 1 set from its channel 0 (`LadderFilter::copyChannel`), and never re-entered within the note. | 72 of 72 renders identical to the bit, at host blocks of 1, 37 and 4096, with notes leaving it mid-note. It saves 47.5% on the init patch and 40.6% on a centred 4 × 8 unison. With row 5, the init patch costs 126 ns per voice, a third of today's. | Leave it to Phase 3 (the measurement kept). | SK7 |

### What the library needs (stage 2)

- **`LadderFilter`**: `skip(n)` (the grid and the ramps advanced, no solve) and `copyChannel(from, to)`. `clearState()` only if row 6's alternative is chosen.
- **`OscillatorSlot`**: `isSilent()`, `isCentred()`, `canSkip()`, and `skip(n, note)` / `skip(notes)`, with the phase steps added up and the jump at the next `process()`, `noteOn()` or `setVoices()`.
- **`dsp::Voice`**:
  - the per-segment plan (rows 5–8);
  - Solo's gains, read like Mix's: `VoiceKnobs` gains `solo`, two `const LinearSmoother*`, null being 1, so every caller today is unchanged to the bit;
  - the mono path (row 10);
  - the bound as one function of the segment, so that M1.5's terms join it.
- **The engine (M1.8)** holds Solo's two 10 ms ramps, their targets set from Solo and Part select, and splits its blocks at their events.
- **Found on the way, for stage 2:**
  - `keytrack.hpp`'s parameter `referenceNote` shadows `dsp::referenceNote` (`-Wshadow`, an error here) in any file that includes `math/pitch.hpp` before it. The library's own include order hides it; the prototype had to include `keytrack.hpp` first. Renaming the parameter fixes it.
  - The voice's output stage (`left[i] += ge * y0[i]`) is fused into a multiply-add by Apple Clang's default `-ffp-contract=on`. GCC in ISO mode (`-std=c++23`, no extensions) doesn't contract, and MSVC's `/fp:precise` may not, so renders can differ by an ulp between compilers. It doesn't matter for sound. It matters for M1.13's golden files if they are compared to the bit across platforms: either set the contraction explicitly on every compiler, or compare with a tolerance.

### What this pushes back on

- **mixer.md's "about 110 ns per voice" at Mix 0** was measured with B1 off, not at Mix 0 with B1 sounding. The saving at Mix 0 or under Solo is part B's filter and slot, 233 ns per voice, and only with row 6. Row 5 alone saves nothing there.
- **voice-architecture.md, question 4**: the mono path is still exact on the current voice (72 of 72), provided the output stage uses the stereo expression. The saving is 40.6–47.5%, as before.
- **E11** (a sustain-0 hold keeps its voice busy) stands for the allocator. Row 6 stops such a part being rendered.
- **V12**: 16 notes at Voices 0 cost nothing in ours already (the allocator frees them), so the demo's reading of that case tells us only whether Sylenth1 does the same.
- **Task 2's in-place retrigger** keeps the filters. Under row 6 a skipped part's frozen state is what it keeps, 30–37 dB from the state it would have had.
- **AmpEnv A/B link (task 6)** is noted only. With the link on, the envelope that applies to a part is the linked one, so row 6's envelope test (and `isActive()`) must read that envelope. Nothing else here depends on it.

## The decision

Chosen by the project owner on 2026-11-22: every recommendation (Decisions log). Stage 2 builds them in `libs/dsp`.

- **Solo** silences the other part's filter output. A part crossing into the selected part's filter is still heard through it.
- **How Solo switches:** a gain per part, 0 or 1, ramped linearly over 10 ms and multiplied into the part's Mix gain per sample, on held notes and release tails alike. It follows Part select while it is on, both gains crossing over 10 ms.
- **Solo and Part select are interface state:** not in presets, not host-automated, not MIDI-mapped, kept with the host's session. `libs/dsp` takes Solo as two gains; where the state is stored is M1.9's.
- **Exact skips, always:**
  - a filter at rest whose input can't be non-zero isn't solved, its grid and ramps advanced, still fed at its grid points;
  - a part's output stage isn't run while its gain's bound is 0;
  - a crossing part whose envelope is 0 isn't added into the other filter.
- **A part whose gain is 0 while its filter is fed** (Mix with its modulation's bound at 0, soloed out, its amp envelope idle or held at sustain 0) has its filter and oscillators skipped, the filter's state kept (frozen) for its return.
- **Oscillators:** the phase advances through a skip (the phase steps added up, a band-limited jump at the return). A slot is skipped when no running filter takes it or its gain is settled at 0, and only while no gain ramp in it moves.
- **When:** per segment, at each of the voice's 32-sample grid points for the coming segment; a call mid-segment can only end a skip. No hysteresis.
- **The bound:** an upper bound per factor over the segment (the amp envelope, Mix's ramp at its larger end plus its modulation routes' bounds, Solo's ramp). Each M1.5 source supplies its bound over a segment.
- **The mono path** as decided in task 1, its output stage writing the stereo path's expression on each channel.

## As built

Stage 2 (2026-11-22) built the decision in `libs/dsp`, test-first, against the stage-1 prototype in its decided configuration.

- **[ladder_filter.hpp](../../libs/dsp/include/anoesis/dsp/filters/ladder_filter.hpp), `LadderFilter`:**
  - `skip(n)`: n samples with no solve and nothing written. The grid advances, calling `beginSegment()` at each grid point, so the cutoff and resonance modulations the voice fed are taken exactly where `process()` would take them. The per-sample ramps (resonance, drive, the response's crossfade) skip n. The state is left as it was. On a filter at rest fed zeros, that is `process()` to the bit. Nothing while bypassed, as `process()`.
  - `copyChannel(from, to)`: one channel's state set from another's (indices past the last are the last), to leave the mono path.
  - Not `clearState()`: only the rejected alternative (cleared at the return) needed it.
- **[oscillator_slot.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/oscillator_slot.hpp), `OscillatorSlot`:**
  - `isSilent()`: every voice it renders has its gains settled at exactly 0 (Volume 0, or nothing sounding), or, for Noise, the noise mix has.
  - `isCentred()`: every voice it renders has its gains, now and at their target, equal left and right to the bit. Never for Noise.
  - `canSkip()`: no ramp it renders with moves: the voices' gains, a voice fading out included, or for Noise the noise mix's.
  - `skip(n, note)` and `skip(notes)`: each running oscillator's phase steps added up in `uint32` (2³² per cycle, so the sum is exact), a voice that has faded out dropped as `process()` drops it, and the noise's draws made.
  - At the next `process()`, `noteOn()` or `setVoices()`, each oscillator jumps to the phase it would have had with `MinBlepOscillator::jumpTo()`, band-limited. `stop()` forgets a pending skip.
- **[part_solo.hpp](../../libs/dsp/include/anoesis/dsp/voice/part_solo.hpp), `PartSolo`:** Solo's two gains, for M1.8's engine to hold.
  - Two `LinearSmoother`s over 10 ms. Their targets come from Solo and the selected part: 1 for every part with Solo off, else 1 for the selected part and 0 for the other.
  - `setSolo(on)` and `selectPart(p)` ramp. Part select under Solo crosses both gains; with Solo off it changes no gain and is remembered. A part past the last counts as the last.
  - `setImmediately(on, part)` jumps, for a session restored soloed. `prepare()` finishes the ramps.
  - `gains()` is `VoiceKnobs::solo`. `skip(n)` moves both ramps past what the caller rendered, as the Mix knobs are moved. The caller splits its block at Solo and Part select events.
- **[voice_knobs.hpp](../../libs/dsp/include/anoesis/dsp/voice/voice_knobs.hpp):** `VoiceKnobs::solo`, two `const LinearSmoother*` read at `offset` like Mix. **A null one is 1**, so every caller without Solo is unchanged to the bit (tested).
- **[voice.hpp](../../libs/dsp/include/anoesis/dsp/voice/voice.hpp), `dsp::Voice`:**
  - **The plan, per chunk** (`planChunk()`). At a grid point, each part's output gain is tested over the whole coming segment (`gainSilent()`), as an upper bound per factor:
    - the amp envelope: idle, or held in sustain at exactly 0 with a sustain of 0 (`envelopeSilent()`);
    - Mix: the ramp's larger end plus `mixModulationBound()` at or below the law's 0 (`mixSilent()`);
    - Solo: the ramp's larger end at 0 (`soloSilent()`).
    
    A chunk that starts mid-segment (a `process()` call starting there) re-tests only a part whose gain is 0, over the rest of the segment, so it can only end that skip.
  - **What the plan skips:**
    - a filter whose part's gain is 0 (frozen), or at rest with no possible input: no sounding non-silent slot in its part, and nothing crossing in (exact);
    - the output stage of a filter whose gain is 0 (exact);
    - a crossing part whose envelope can't be above 0, not added into the other filter (exact).
    
    A skipped filter is still fed at its grid points, and both envelopes always run.
  - **The slots.** A part's slots are rendered for a filter that takes them: its own, or the other filter when the part crosses into it, where the filter runs, or is bypassed and heard. Otherwise, or when a slot is silent, a slot is skipped if `canSkip()`. Like a filter's, a slot's skip starts only at a grid point and can only end mid-segment: where it starts decides what its phase's jump leaves in flight at the return. One that isn't skipped while no filter takes it is rendered into its part's buffer, which nothing reads that chunk.
  - **Solo's gains** are multiplied into each part's Mix gain per sample, after the law. A constant Solo of exactly 1 is skipped; that multiply would change nothing.
  - **The mono path.** At a note-on from silence, `mono` is set if every sounding slot `isCentred()`. On it, the filters' inputs, the filters and the output stage run on channel 0. The output stage writes both channels with the stereo path's own expression, with `y1` the same span as `y0`. At the first chunk where a slot isn't centred, `copyChannel(0, 1)` on both filters, and the voice is stereo until its next note-on from silence. `isMono()` reports it, for measurement.
  - **The seam for M1.5:** `mixModulationBound(part, knobs, done, length)` returns an upper bound on the modulation of that part's Mix over those samples. It returns 0 until M1.5. Each modulation route to Mix X is one more term in it. The test that uses it doesn't change.
  - **Unchanged:** `isActive()`, `activeOscillatorVoices()`, the allocator, and the filter input level (the mono path writes `(ge · y0) · outputLevel` on both channels, as the stereo path does).
- **[keytrack.hpp](../../libs/dsp/include/anoesis/dsp/filters/keytrack.hpp):** `keytrackOctaves()`'s third parameter is `reference`. As `referenceNote`, it shadowed `dsp::referenceNote` (`-Wshadow`, an error here) in any file that included `math/pitch.hpp` first. [keytrack_include_order_test.cpp](../../tests/dsp/filters/keytrack_include_order_test.cpp) includes them in that order. It failed to compile before the rename. The prototype's include-order workaround is gone.

### Found in the build

The build found three flaws in the stage-1 prototype. All three are fixed in the prototype, so it is still the reference. The stage-1 report then reproduces to the printed digit, except one cell of a rejected candidate (the third item):

- **A part on AB was dropped when its own filter didn't take it.** The prototype rendered part p's slots for the other filter when the *other* part crossed into it (`crossing[other]`), where it meant part p crossing (`crossing[p]`). Two cases were affected:
  - filter B on `--` with part B on filter A's AB (the pad);
  - part B soloed out while it crosses into filter A.
  
  In both, B's slots were skipped and filter A got zeros for them. That contradicts the decision: Solo A still plays B1 through filter A. Stage 1 never compared that configuration against a render without skipping. Its mono table rendered the pad with the same skipping on both sides, so it never showed. The test "Solo silences the other part's filter output; a part crossing into the selected filter still sounds" fails on it (break 23 below).
- **`canSkip()` waited on the noise mix's ramps on every slot.** A Volume or Pan change retargets those ramps, but only a Noise slot advances them. So a saw slot changed while it sounded could never be skipped again before the next `stop()`. That only lost skips, never changed the output. `canSkip()` also ignored a voice fading out, whose gain ramp `skip()` wouldn't advance. Both are fixed: a slot checks the ramps it renders with. The slot test found the first, and break 12 below the second.
- **A slot's skip could start anywhere in a segment.** The prototype decided each slot's skip per drift run inside a chunk, so a slot whose gain settled at 0 mid-segment started its skip where the next chunk began, which follows the host's blocks. The skip is exact while it lasts, but the band-limiting residual the jump leaves at the return depends on where the skip began. Stage 1's flawed `canSkip()` hid this: a saw changed while sounding was never skipped. Once that was fixed, the report's real-time table fell to 4 of 32 at host blocks 1 and 37. Now a slot's skip starts only at a grid point, under every timing, and can only end mid-segment. The table is back to stage 1's figures, except "clear + slots + mono, per chunk": 0 of 4 then, 2 of 4 now. Per chunk still depends on the host's blocks, through the filter's skip. The per-segment test caught it once B1's Volume went to 0 and back in it (break 37).

A related reading, from the "as it was" test: the allocator counts its check points per record and resets them at a steal, so after a steal they no longer fall on the voice's own grid. A voice whose envelopes have just gone idle can reach its own grid point, and be planned as silent with its slots skipped, before the allocator frees it. The noise generator carries on across `stop()`. So a `skip()` that didn't make the noise's draws would change that voice's next note (break 7).

Task 4's stage-2 test ([mix_test.cpp](../../tests/dsp/voice/mix_test.cpp)) drove Mix to −2…0 in its busy sequence. At Mix 0 the voice now freezes the part's fed filter, as decided, which task 4's prototype doesn't, so that sequence now stays at 0.5 and above (still past the top). Mix at and below 0 is covered here, against task 5's prototype: the busy sequence below takes Mix B to −3.

### Tests

[solo_test.cpp](../../tests/dsp/voice/solo_test.cpp), tagged `[part-solo]`, has 19 test cases, and [keytrack_include_order_test.cpp](../../tests/dsp/filters/keytrack_include_order_test.cpp) has one more. They use [solo_library_engine.hpp](../../tests/dsp/voice/solo_library_engine.hpp), which holds `LibrarySoloEngine` (the prototype's `SoloEngine` with `dsp::Voice` and `PartSolo` in place of the prototype) and the prototype's decided switches.

| Test | What it pins down | Tolerance and why |
|---|---|---|
| The voice against the prototype | `VoiceAllocator<dsp::Voice>` with `PartSolo` against `SoloEngine` as decided, at host blocks of 1, 37 and 4096, over 11 scenarios. A busy sequence (steals at Polyphony 4, Mix B to −3 and back, Solo on, Part select, Solo off, B1's Volume to 0 and back, a Pan move that leaves the mono path) on the init patch, two parts, crossing, pad, Noise, and the worst case centred and spread. Mix B 0 from the note-on and back on the resonant patch. Solo A from the note-on, Part select, Solo off on the crossing patch. An envelope held at sustain 0, then a retrigger in place, on own filters and crossing | bit for bit; 0 allocations from `prepare()` on |
| As it was | with `VoiceKnobs::solo` null, against the prototype with nothing switched on (`dsp::Voice` before stage 2) on six patches with Mix moving, at host blocks of 37 and 4096 | bit for bit: only exact skips and the mono path apply |
| A null Solo ramp is 1 | Solo's ramps at 1 against none, three patches | bit for bit |
| Each exact skip | against the prototype with nothing skipped: the init patch, B1 at Volume 0, every oscillator at Voices 0, a crossing part held at sustain 0, Mix B 0 throughout, B soloed out throughout, A soloed out on the crossing patch | bit for bit |
| The frozen skip | identical up to the return. After it, the difference's RMS over 50 ms and its last differing sample against stage 1's: Mix B dipping to 0 (−64.0 dB, 208 samples), resonant (−39.4, 1,622), Solo off (−68.4, 207), a retrigger after a sustain-0 hold (−36.6, 76), B1 back from Volume 0 (−82.4, 122). And one measured here: both filters bypassed, B soloed out, then Solo off (−78.9 dB, 63 samples: only the slot's phase jump, for the kernel's length) | ±1 dB and ±25% of the samples. The figures are deterministic on one compiler; another's contraction moves the rounding by an ulp, which moves these by hundredths of a dB and the last sub-ulp sample by a few. Two-sided, so a skip that didn't happen (far below) fails as well as a state that came back wrong (far above) |
| `LadderFilter::skip()` at rest | at splits of 1, 5, 31, 32, 33, 100 and 700 samples, with every ramp moving (cutoff, its modulation, resonance and its modulation, drive, the response), against `process()` on zeros; then both filter the same signal | bit for bit, both at rest |
| `skip()` keeps the state | skipped by whole segments with the ramps settled, against not skipped at all; and against a filter that solved the zeros | bit for bit; differs |
| `copyChannel()` | after it, equal inputs give equal outputs; an index past the last clamps | bit for bit |
| The slot's phase | after `skip(n)` of 1, 63, 64, 1,000 and 480,000 samples on 8 unison voices, `nextPhase()` against rendering n. Then the outputs from sample 64 on (the kernel's length) | exact; bit for bit after 64 samples |
| A note per sample | `skip(notes)` through a glide, against rendering it; the jump at `setVoices()` too | exact |
| Noise | the draws after `skip()` at Volume 1 and 0, against rendering | bit for bit |
| `isSilent()`, `isCentred()`, `canSkip()` | silent only when settled at 0 (a 10 ms ramp: not at 479 samples, at 480); Voices 0 faded out; Noise at Volume 0; centred at Pan 0 and Stereo 0, inverted, one voice at Stereo 1; not while the gains move back to the centre; never for Noise; `canSkip()` false while a gain or a fading voice's gain moves | exact |
| `PartSolo` | each gain against a 10 ms `LinearSmoother` at every sample: Solo on; Part select mid-ramp, both crossing from where they are; Solo off. Part select with Solo off. The clamp. `setImmediately()`. `prepare()` | exact (0 ULP) |
| Solo in the voice | bypassed sines; Solo on, Part select before B's gain reaches 0, Solo off before A's does. The output against each part's own render times its ramp, at every sample | 3 roundings of a sum under 0.4 (7.2e-8): one per product and one for the sum |
| What Solo silences | crossing patch: Solo A is Mix B at 0, to the bit, and still has B1 in it (it differs from B1 at Voices 0, and is louder). Solo B is Mix A at 0 | bit for bit |
| Per segment | Mix B's ramp landing mid-segment, Solo on, Part select, Solo off, B1's Volume to 0 and back, on three patches. Host blocks of 1, 2, 3, 7, 31, 33, 37 and 64 against 4096. Sensitivity check: the prototype deciding per chunk differs at 37 | bit for bit |
| A call mid-segment ends a skip | Solo off 7 samples into a segment: B is heard from the next sample, not at the next grid point | the first difference within 3 samples |
| An envelope held at 0 | on `dsp::Voice` directly: env B held at sustain 0, then Sustain raised to 0.7 mid-segment. B is heard from there (the level ramps up from 0 in the sustain stage) | the first difference within 3 samples; louder by 1.5× after |
| The mono path | the init patch, the worst case centred and a pad on filter A, at filter input levels 1 and 2.5, host blocks of 37 and 4096, with a Pan move mid-note. The reference never takes the mono path: B1 panned at 0.3 on a part nothing hears. Also a note-on on the sounding voice (Polyphony 1) after the Pan has come back to the centre, where the filters' two channels no longer hold the same state. Taken on centred patches with left equal to right; not on spread or Noise; left at a Pan move and not taken again when the Pan comes back | bit for bit |
| `keytrack.hpp` after `pitch.hpp` | it compiles; the third argument is the reference | exact |

**Red first**, against inert headers with the decided interface (`skip()` and `copyChannel()` doing nothing, the slot never silent, centred or skippable, `PartSolo` ramps that never move, the voice ignoring `VoiceKnobs::solo`): 14 of the 18 test cases written then failed, and the include-order file didn't compile. (The envelope test, the 19th, came later, from the breaks.) Four passed:

- "as it was" and "a null Solo ramp is 1", rightly: both are non-regression tests, true of the old voice by design;
- "per segment", rightly: a voice that skips nothing doesn't depend on the host's blocks either. Its sensitivity check (the per-chunk prototype differs) is what keeps it from being vacuous;
- "`skip()` keeps the state", because an inert `skip()` does keep it. It was strengthened to require that the continuation differs from a filter that solved the skipped zeros, so a `skip()` that solves (or clears) fails it.

Then 38 deliberate breaks of the new code, each a one-line change, with every test case in `[part-solo]` run against it ([solo_test.cpp](../../tests/dsp/voice/solo_test.cpp)):

- **The filter:** `skip()` not advancing the ramps, not taking its grid points, or clearing the state; `copyChannel()` the wrong way.
- **The slot:** the phase frozen through a skip; `skip(notes)` holding the first note's pitch; the noise's draws not made; no jump at `noteOn()`; a jump to the skipped steps alone; `isSilent()` true on a ramp to 0; `isCentred()` reading only the targets; Noise centred; `canSkip()` ignoring a fading voice (stage 1's).
- **The voice:** the test per chunk; a skip never ended mid-segment; a slot's skip starting mid-segment; an envelope silent at level 0 in any stage, or in sustain whatever its sustain; Mix's bound from the ramp's first end; Solo's from its last; Solo not multiplied in, read a sample ahead, or a constant one applied only when it is 1; a part's slots rendered for the other part's crossing (stage 1's); a crossing part added at envelope 0, or dropped while it sounds; the mono path re-entered at a note-on on a sounding voice, left without `copyChannel()`, or its output a product rounded once; no freeze; no exact skip of a filter at rest; the output stage run at gain 0; slots never skipped; a silent slot rendered.
- **`PartSolo`:** Part select under Solo ignored; `setImmediately()` ramping; 20 ms ramps; the selected part unclamped.

The first pass ran the other 37 and caught 32. (Break 37, a slot's skip starting mid-segment, was added after the third flaw above.) Two didn't compile under `-Werror` (an unused argument): they were rewritten, and then caught. Three survived:

- **An envelope at level 0 in sustain counted as silent whatever its sustain.** No test raised Sustain while a part was held at 0, and with this break that part never comes back. The new envelope test catches it.
- **The mono path re-entered at a note-on on a sounding voice.** No test stole a voice after its Pan came back to the centre. The mono test's steal sequence catches it.
- **The output stage run at gain 0.** Alone that is exact. But it also counts a bypassed, soloed-out filter as heard, so its slots are rendered rather than skipped, and the phase jumps at the return disappear. No case had a bypassed part come back. The bypassed case in the frozen-skip test catches it: the difference falls to nothing.

With those, 37 of the 38 are caught by the tests. The one left is **no exact skip of a filter at rest**. It must survive any output test: the skip is the same to the bit as running the filter. The `[solo]` benchmark catches it instead: with it the init patch costs 3,036.7 ns per sample for 16 voices, against 1,958.1 as built (+55%), filter B solving silence again.

### Results, as built

- **Bit for bit the stage-1 prototype as decided:** 33 of 33 in the tests (11 scenarios at three block sizes), and 69 of 69 in the report's new "As built" section. That section replays every sequence of the report that the library can play: the identity sequence, the 11 skip cases, the phase through a skip, the timing sequence, Solo's readings and clicks, the mono sequences at blocks 1/37/4096, and the real-time sequence. 0 allocations.
- **The stage-1 report reproduces to the printed digit**, apart from the real-time cell above (clear + slots + mono per chunk: 2 of 4 against 0 of 4). That includes the prototype against the library with nothing switched on (12 of 12: the library's exact skips and mono path change nothing there) and every skip-case figure (the frozen returns at −64.0, −39.4, −68.4 and −36.6 dB).
- **The earlier tests pass**, apart from task 4's busy sequence, which now keeps Mix above 0 (above).

**CPU** (`tools/bench.sh "[solo]"`, `relwithdebinfo`, Apple M1 Ultra (virtualised), the median of three runs, spread 0.4–5.2%; one prototype row 26.9% from one slow run). 16 voices held (keys 40–55), ns per sample for all 16:

| Case | `dsp::Voice` before stage 2 (stage 1's entry) | as built | per voice | change | the prototype as decided, this session |
|---|---:|---:|---:|---:|---:|
| init | 5,762.3 | **1,958.1** | 122.4 | −66.0% | 1,989.9 (atRest + slots + mono) |
| filter B `--` | 3,807.2 | **1,948.4** | 121.8 | −48.8% | 1,946.7 |
| Mix B 0 (two parts) | 7,574.8 | **1,960.7** | 122.5 | −74.1% | 2,002.0 |
| B soloed out (two parts) | 7,375.0 (Solo as a gain alone, this session) | **1,973.0** | 123.3 | −73.2% | 3,820.4 (no mono) |
| two parts sounding, centred | 7,569.0 | **3,831.6** | 239.5 | −49.4% | 3,882.7 |
| worst, centred | 10,259.1 | **6,139.0** | 383.7 | −40.2% | 6,021.4 |
| worst, spread (nothing to skip) | 10,139.9; the prototype with nothing skipped 10,037.0 this session | **9,906.7** | 619.2 | within noise | 10,085.6 |
| every oscillator at Voices 0 | 0.1 | 0.1 | | (freed at once) | 0.1 |

- **As built is the decided prototype's cost**, within noise in every row.
- **The init patch costs a third of what it did** (360 → 122 ns per voice): filter B's exact skip and the mono path. A part at Mix 0, or soloed out, now costs nothing beyond the init patch (the voice is centred, so the mono path applies too).
- **Where nothing can be skipped, the plan costs nothing measurable** (worst, spread: as built 9,906.7 against 10,037.0 for the prototype with nothing skipped).

**Memory:** `dsp::Voice` gains a few flags (the mono path, a gain test and a skip flag per part and per slot). Each `OscillatorSlot` gains 8 pending phase sums (32 bytes). `PartSolo` is two smoothers.

## References

- LennarDigital, *Sylenth1 User Manual*, v2.5, 2015, pp. 6, 9–10, 30–32.
- The developer on KVR: [p=4288321](https://www.kvraudio.com/forum/viewtopic.php?p=4288321) and [p=4289093](https://www.kvraudio.com/forum/viewtopic.php?p=4289093) (October 2010). [The official changelog](https://lennardigital.com/sylenth1/whatsnew.php).
- MusicRadar, [*How to master Sylenth1's parts and oscillators*](https://www.musicradar.com/tuition/tech/how-to-master-sylenth1s-parts-and-oscillators-630710) (2015). Syntorial, [*Synth Tutorial: Layering with Sylenth1*](https://www.syntorial.com/tutorials/synth-tutorial-layering-with-sylenth1/) (2015, updated 2025). Every source is rated in the [feature inventory](../reference/sylenth1-features.md#part-solo-and-skipping-unused-parts).
- E. Brandt, "Hard Sync Without Aliasing", *Proc. ICMC* 2001 (band-limited steps, and why a jump confines a phase change's error to the kernel's length).
- GCC manual, `-ffp-contract`; Clang manual, `-ffp-contract`; Microsoft, `/fp (Specify floating-point behavior)`.
- [voice-architecture.md](voice-architecture.md) (the voice, question 4), [mixer.md](mixer.md) (Mix per voice), [voice-allocation.md](voice-allocation.md) (`isActive()`, E11), [filters.md](filters.md) (the grid and ramps), [oscillator-phase.md](oscillator-phase.md) (`jumpTo`), [parameter-smoothing.md](parameter-smoothing.md).
