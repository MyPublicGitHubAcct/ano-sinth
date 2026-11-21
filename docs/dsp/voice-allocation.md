# Voice allocation: polyphony, stealing, freeing, and the Voices readout

A polyphonic synth has a fixed number of voices. Each note-on needs one, and when none is free, it must take one that is sounding. This page covers M1.4 task 2:

- the theory: how an allocator works, the usual stealing policies, a key played again ("voice recycling"), the ways a stolen note can end and why each clicks or doesn't, when a voice counts as finished, what a voice readout can count, and how to keep an allocator's decisions independent of the host's block size;
- what is known about Sylenth1's allocation. The manual gives the Polyphony control and the Voices readout. The developer, in 2016: Polyphony 1 "will simply make each new key steal the single voice", higher settings give "overlapping release times, as it should", and a key played again doesn't reuse its own voice. A changelog fix concerns Polyphony 0;
- the candidate allocators prototyped here, driving `dsp::Voice`;
- what they measured: the click at a steal for each way of ending the stolen note, a key played again, freeing rules, busy voices over time, the readout against the manual's screenshot, the policies under a 32-note strum and a held bass under a melody, Polyphony changes, block sizes and allocation, and CPU;
- the recommendations for the project owner, with the alternatives;
- the project owner's decision on the three changes to the voice (all approved), and `dsp::Voice` as changed, with the tests that pin it. The allocator's own recommendations (1a–5, 7) await a decision.

Read [voice-architecture.md](voice-architecture.md) (the voice that is allocated), [envelopes.md](envelopes.md) (when an envelope ends, and a retrigger from the current level), and [oscillator-phase.md](oscillator-phase.md) (what Retrig does to a sounding oscillator) first.

## Theory

### What an allocator does

The allocator sits between the note events and the voices. For every note-on it picks a voice and starts the note there. For every note-off it finds the voice playing that key and releases it. It frees a voice when the voice has finished sounding, so a later note can use it. It also knows the polyphony limit, and it reports how many voices are busy.

A voice's lifetime is: **free → sounding with its key down → released (key up, the amp envelope releasing) → free.** A steal cuts this short. The voice goes from sounding straight to a new note. How the old note ends is a design choice, and it is audible.

### Which voice to steal

When every voice is busy, the usual policies are:

- **Oldest:** the earliest note-on. It is simple, and it's fair over time. Its weakness is a note held under faster playing, such as a bass or a pedal tone: that note is the oldest, so it is stolen first.
- **Quietest:** the lowest amp envelope level. It takes what is least audible. Its weakness is a note in a slow attack, which is quiet because it has only just started, and is then stolen first.
- **Released first, then oldest:** a note whose key is up is already on its way out. If there is none, take the oldest. Held notes survive as long as anything is releasing.
- **Same key first:** a voice already on the new note's key, if one is busy. This is the stealing form of voice recycling (below).

Refinements combine these: for example, the quietest among the released notes, or the oldest but never the lowest held note (a "bass protection" rule).

### A key played again

When a key returns while its last note is still releasing, there are two choices:

- **A new voice.** The old note's release goes on underneath. A natural instrument behaves this way: a piano string damped late, a guitar string rung again. Two voices on one key at a fixed time offset sum with a phase set by that offset. The level and the tone then differ from one repeat to the next, a static comb filter for as long as the release lasts.
- **Its own voice, retriggered** ("voice recycling", common on hardware such as Roland's JX series): one voice per key, which saves polyphony and keeps the sound identical on every repeat. The cost is the old release, cut short by the new note, and with it any click the retrigger makes.

### How a stolen note ends

- **Cut:** the voice is stopped and the new note starts from silence. A step from the old note's sample value to the new note's onset, so a broadband click, as loud as the old note was.
- **A fade, then the new note on that voice:** the old note fades over N ms, then the new note starts. No step, but the new note starts N ms late, and only when a steal happens, so the timing jitters.
- **A fade on a spare voice:** the old note fades over N ms on its own while the new note starts at once on a spare voice. No latency, but the pool needs spare voices, as many as steals that overlap within N ms.
- **Retrigger in place:** the new note starts on the sounding voice. Each part decides whether there is a step:
  - The **envelopes** attack from their current level, so they are continuous (E5).
  - The **oscillators** with Retrig on make a band-limited jump to Phase, a step in the waveform. With Retrig off they run on, so there is no step.
  - The **filters** either keep their state, so the output is continuous, or are reset to rest. A reset steps the output to zero at once, and on a resonant filter it also stops the ring, which is a large step at the resonant frequency.

A linear fade of length T multiplies by a ramp. Its spectrum falls as 1/f² above about 1/T, so 1 ms leaves energy up to a few kHz, while 10 ms keeps it below a few hundred Hz. That is why each doubling of a short fade matters so much more than going from 10 to 20 ms.

### When a voice is finished

The decided `Adsr` ends its release at exactly 0, and its `isActive()` turns false there (E10, decided 2026-11-14). The alternatives:

- **A threshold** (−60, −80, −100 dB) frees a voice a little earlier, at the cost of a step of that size.
- **A silent hold:** a key held with every envelope sustaining at exactly 0 is silent, so it could be freed early (E11). The voice is then gone if the sustain rises again later, through automation or, from M1.5, modulation.
- **What counts as busy:** each part's amp envelope, or only the envelopes the output can hear. A part with no filter routed to it, or with no oscillators, is silent whatever its envelope does.

### What a readout can count

A "voices" readout counts notes (voices in the allocator's sense), or oscillator voices: unison voices across all oscillators. Those can be counted per note (every oscillator of every busy note) or per part (only the parts whose amp envelope is still active). The maximum is the per-note count times the polyphony.

### Keeping decisions independent of the block size

An allocator's decisions depend on state: which voice has finished, which is quietest. If it checks that state only at host block boundaries, the result depends on the host's block size. A voice that finished mid-block is still "busy" at a note-on later in the same block, but free with smaller blocks. Worse, a finished voice rendered on past its end keeps drawing from its random sequences (drift's noise, free-running phases), so even its next note changes. Two rules keep everything exact at any block size:

- **Render every voice exactly up to each event**, splitting the block at events.
- **Check for finished voices only at block-independent times:** at each event, and at points counted from each voice's own note-on.

## Sylenth1: what is known

The full source table with ratings is in the inventory: [§7 Voice allocation](../reference/sylenth1-features.md#voice-allocation) (P1–P7), [§4](../reference/sylenth1-features.md#envelopes) (E5, E10, E11). In short:

- **The manual.**
  - p. 11: Polyphony sets "the maximum number of simultaneous notes", from 0 to 16.
  - p. 11: "The Voices indicator displays the number of active voices and the maximum total number of voices. Each oscillator can generate several voices. The maximum number of voices is equal to the sum of voices generated by all oscillators, multiplied by the maximum number of polyphonic notes." The example: 2 oscillators of 3 voices each make 6 voices per note, so 24 at Polyphony 4.
  - The screenshot beside it reads **Polyphony 12, Voices 204/336**. 336 is 12 × 28, so its patch has 28 oscillator voices per note, but 204 isn't a multiple of 28 (7.29 notes). So the active count is not whole notes times every oscillator's voices.
  - p. 29: Mono Legato means "the polyphony setting on the top bar will be discarded".
  - p. 30: "For every polyphonic note, almost a whole copy of the audio path is made"; "Sounds with a smaller release time will usually also use less notes of polyphony".
  - p. 31: the default CC map has Hold Pedal on CC 64.
- **The developer (KVR, LennarDigital forum, "Voice stealing", January 2016, [t=454264](https://www.kvraudio.com/forum/viewtopic.php?t=454264)).** A user had found that repeating a key on a one-saw patch with a long release changes the sound "in an ugly way":
  - Lennard: "This simply depends on the Polyphony setting for the preset in Sylenth1's top bar. Setting it to 1 will simply make each new key steal the single voice. Setting it higher and you'll get overlapping release times, as it should." ([p=6349067](https://www.kvraudio.com/forum/viewtopic.php?p=6349067))
  - Answering "a complete yet clickless restart of the voice when playing the same poly note again, just like UA2": ".. which is exactly what Sylenth1 does", in a post defending 1-voice polyphony: "There's nothing fake about 1 voice polyphony." ([p=6349459](https://www.kvraudio.com/forum/viewtopic.php?p=6349459))
  - After trying the user's other synth: "If (and only if) you play a note with the same pitch, it'll just stay in the same voice, cutting of the previous released one. If you play a note with different pitch, it'll play in a new voice. This does sound pretty unnatural to me. Every natural instrument (piano, guitar, you name it..) will just play the entire release of 2 consecutive notes of the same key." ([p=6350466](https://www.kvraudio.com/forum/viewtopic.php?p=6350466))
    - The post names no synth. It answers the user's report on the other synth (UA2) and follows "Ok I tried it", and Lennard's own earlier post says Sylenth1 overlaps releases. So it describes UA2, and Sylenth1 gives a repeated key a new voice.
  - Then: "I have no intentions of changing the voice algorithm in Sylenth1 as in my eyes it does exactly what it should do." ([p=6350841](https://www.kvraudio.com/forum/viewtopic.php?p=6350841))
- **The same user, a month later** ([t=454396, p=6391176](https://www.kvraudio.com/forum/viewtopic.php?p=6391176)): "funny that two free synths do it right, while Sylenth1 sucks in this respect. But according to the developer he wanted it to sound the way it does". Also: switching phase retrigger on and off "has zero impact on the effect", which is "the overlapping of two or more voices on the same note" ([p=6391185](https://www.kvraudio.com/forum/viewtopic.php?p=6391185)).
- **The changelog.** v3.031 (May 2017): "Hanging note when switching to polyphony 0 fixed." Also v3.010 and v3.023 (2016): "Hanging notes bug fixed", "Sustain pedal issue fixed".
- **The developer on the pedal** (2007, v1.21, [p=2769347](https://www.kvraudio.com/forum/viewtopic.php?p=2769347)): "better behavior of the sustain pedal (it should only kill notes that are not being pressed)". This is for task 3.
- **Users.**
  - Polyphony counts notes, not unison voices: "Voice -> Poly = Polyphony in Sylenth1… Unison and Num(ber) = Voices in the osc section" (chk071, 2014, [p=5970715](https://www.kvraudio.com/forum/viewtopic.php?p=5970715)). The product page: "16 notes of polyphony", "512 simultaneous oscillator voices" (16 × 4 × 8).
  - A user (Xfer forum, 2026): Sylenth1 retriggers without clicks (E5's source).
- **General practice** (not Sylenth1). A synth developer explains voice recycling ([t=454396, p=6391077, p=6391140](https://www.kvraudio.com/forum/viewtopic.php?p=6391140)): "Recycling means the same voice is used for the same note"; "There must be 'carryover' or it will introduce clicks. For example the envelope starts from the position it was already at"; without it, "the old voice is often forced to fade out quickly, although not always".
- **Unknown.** Which voice a steal takes at Polyphony 2–16. Whether a stolen note is cut, faded or retriggered. Whether the filters keep their state at a retrigger. The freeing threshold (E10). Sustain-0 holds (E11). Whether an unheard part's envelope keeps a voice busy. What Polyphony 0 and a lowered Polyphony do to sounding notes. The readout's exact count.

So, rated in the inventory:

- **Polyphony counts notes**, 0–16, and unison voices don't use it up. High: the manual and its formula.
- **At Polyphony 1 a new key steals the single voice, and it restarts without a click** (the developer, high for the steal, medium for "clickless": it answers a quoted sentence about repeated notes).
- **Above 1, releases overlap** (the developer: high).
- **A key played again takes a new voice, and the same key can sound twice.** Medium-high: the developer's description of the other synth, his defence of overlap, and the user's later complaint.
- **The readout's active count isn't whole notes × every oscillator's voices** (low-medium: one screenshot, which may be composed).
- **Switching to Polyphony 0 with notes sounding is a case the engine must handle** (the v3.031 fix): medium that notes play on, or did, past such a switch.

## The candidates

In [allocation_candidates.hpp](../../tests/dsp/voice/allocation_candidates.hpp), one `Allocator<V, Pool>` over a fixed pool of voices, constructed in place (16 for notes, up to 16 spares). Events (note-on, note-off, a Polyphony change) arrive at their samples, and the host block is split at each one. Every voice is rendered exactly up to each event. Finished voices are freed at each event and at each voice's own 32-sample points from its latest note-on, never at host block boundaries. A freed voice is stopped: its slots and drift are silenced and its envelopes made idle, so its next note starts from silence. `V` is `dsp::Voice` (through `BuiltVoice`), or the stage-1 prototype voice for one variant, or a stub for the bookkeeping's cost.

| Policy | Candidates |
|---|---|
| **Steal** | oldest; quietest (the larger amp envelope level, then the oldest); released first, then oldest; same key first, then oldest |
| **A key played again** | a new voice (the developer); its own voice, retriggered (recycling) |
| **The stolen note's end** | cut; a fade of 1, 5 or 10 ms, then the new note on that voice; a fade of 1, 5 or 10 ms on its own while the new note starts on a spare; retrigger in place, with the filters reset (`dsp::Voice::noteOn` today) or with the filters and the voice's grid kept (a new `Engine::keepFiltersOnRetrigger` on the prototype voice, identical to `dsp::Voice` otherwise) |
| **Freeing** | the release at exactly 0 (E10, decided); below −60, −80 or −100 dB once every gate is off; also when every envelope sustains at exactly 0 with the key held (E11) |
| **What keeps a voice busy** | either amp envelope (`dsp::Voice::isActive()`); only envelopes that can be heard (a part whose filter runs with something sounding in its input, or whose oscillators cross into the other filter) |
| **Polyphony lowered** | leave sounding notes (only new notes obey); release the oldest over the limit; cut them |
| **The readout** | notes; R1: busy notes × every oscillator's unison voices; R2: per busy note, the voices of the parts whose amp envelope is active; the maximum as the manual gives it |

Note-offs reach every voice on that key, whatever the limit. Each voice's seed comes from the instance's and its index.

**Patches.** The click measurements use sines, so the energy above 1 kHz around an event is the event's own (a saw's partials would fill the band whatever happens):

- **Init:** A1 one sine, LP24 at 4,677 Hz, k 0, both envelopes a gate (A 0, S 1, R 0), Retrig on.
- **Resonant pad:** A1 and A2 four detuned, spread, free-running sines each, LP24 at 585 Hz, k 3.6; A 200 ms, D 500 ms, S 0.8, R 1 s.
- **Ringing:** one sine into LP24 at 585 Hz at k 3.96, just below self-oscillation, so the filter rings long and its output stays clean; A 5 ms, R 300 ms, Retrig on.
- **Worst:** 4 × 8 sines, Retrig on, both filters on AB at drive ×4, k 4.8 (self-oscillating).

## What they measured

By the hidden `[voice-allocation-report]` ([allocation_report.cpp](../../tests/dsp/voice/allocation_report.cpp), about 15 s) and the `[voice-allocation]` benchmarks ([voice_allocation_benchmark.cpp](../../tests/dsp/benchmarks/voice_allocation_benchmark.cpp)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[voice-allocation-report]"
tools/bench.sh "[voice-allocation]"
```

### Questions 1 and 6: the end of a stolen note

Polyphony 1, key 45 at 0, key 52 at sample 24013 (off the voice's grid), the first key held (*held*) or released 100 ms before (*releasing*). The click is filters.md's: the energy above 1 kHz in a Hann window of 4096 centred 32 samples after the new note-on, both channels, in dB re an instant unit step. *Steady* is the same window 170 ms earlier, which is what the patch has above 1 kHz anyway. The reference is Polyphony 2: no steal, both notes sounding.

| End | latency | init, held | init, rel. | pad, held | pad, rel. | ringing, held | ringing, rel. | worst, held | worst, rel. |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| steady | | −93.1 | −93.1 | −67.8 | −67.8 | −107.2 | −107.2 | −8.3 | −8.3 |
| **reference: a free voice, no steal** | 0 | −25.9 | −25.9 | −65.0 | −66.4 | −55.4 | −55.4 | −6.7 | −7.3 |
| cut | 0 | −20.8 | free | −10.0 | −15.9 | −40.8 | −55.8 | −5.1 | −7.3 |
| fade 1 ms, then the note | 1 ms | −23.8 | free | −29.5 | −35.3 | −41.1 | −53.5 | −7.0 | −7.6 |
| fade 5 ms, then the note | 5 ms | −26.3 | free | −41.2 | −46.1 | −55.3 | −56.4 | −8.3 | −9.0 |
| fade 10 ms, then the note | 10 ms | −26.9 | free | −47.7 | −51.5 | −55.5 | −57.7 | −10.1 | −11.2 |
| fade 1 ms on a spare | 0 | −22.5 | free | −29.5 | −35.5 | −41.6 | −54.3 | −6.7 | −7.3 |
| fade 5 ms on a spare | 0 | −26.2 | free | −41.7 | −47.8 | −53.6 | −55.4 | −6.8 | −7.3 |
| fade 10 ms on a spare | 0 | −26.0 | free | −48.8 | −54.9 | −55.0 | −55.4 | −6.8 | −7.3 |
| **retrigger, filters reset (`dsp::Voice` today)** | 0 | −22.9 | free | **−9.8** | −15.7 | −40.2 | −53.8 | −5.0 | −7.3 |
| **retrigger, filters and grid kept** | 0 | −21.0 | free | **−39.2** | −44.0 | −47.9 | −50.9 | −6.7 | −7.3 |

"free" means the init patch's R 0 had already freed the voice, so there was no steal.

- **Resetting the filters of a sounding voice clicks as hard as a cut**: −9.8 dB on the resonant pad, against −10.0 for a cut and −65.0 for the reference. Keeping the filters and the grid takes 29 dB off (−39.2). This is question 6. The reset was decided for a voice starting from silence, where the filter is at rest anyway. On a sounding voice it throws away the ring.
- **What a kept-filter retrigger still has:**
  - On the pad (free-running, so no oscillator step), −39 dB is the note change itself: eight sines change frequency at once into a resonant filter, a slope corner that rings the filter. A legato step without glide does the same.
  - On the ringing patch (Retrig on), −47.9 dB: the oscillator's band-limited phase jump to Phase.
  - On the init patch, −21.0 dB: the same jump, at 4,677 Hz with nothing to hide it.
- **A fade removes the step at its source.** On the pad, 1 ms gives −29.5 dB, 5 ms −41 to −42, and 10 ms −48 to −49, within 7 dB of keeping the filters (or better). On the ringing patch, 5–10 ms reaches the reference. The spare and the delayed fade measure the same within 1–2 dB, but the delayed fade starts the new note 5–10 ms late.
- **The worst patch masks everything**: its own self-oscillation at drive ×4 fills the band at −8 dB, so no end is distinguishable there. Patches that saturate hard hide steals; clean resonant ones expose them.
- With the stolen note releasing, every end is 4–6 dB quieter on the pad: the release has already brought its level down.
- The prototype voice with its filters reset is `dsp::Voice` to the bit on every patch (4 of 4), so the kept-filter row differs only by keeping the filters and the grid.

How many spares a fade on a spare needs (the pad at Polyphony 16, 16 keys held, then 16 more):

| Fade | 16 steals 2 ms apart: most fading at once; cut for lack of a spare, at a pool of 32 / 20 / 17 | 16 steals at one sample: the same |
|---|---|---|
| 5 ms | 3, 0 / 3, 0 / 1, 10 | 16, 0 / 4, 12 / 1, 15 |
| 10 ms | 5, 0 / 4, 3 / 1, 12 | 16, 0 / 4, 12 / 1, 15 |

A strum needs a few spares. A block chord of steals needs one per steal, or the excess is cut.

### Question 2: a key played again

The init patch with R 1 s (110 Hz, Retrig on), the key on at 0 and off at 100 ms, then on again. The level over 100 ms from 10 ms after the second note-on is given against one fresh note's level over the same span.

| Second note-on | first note's phase | new voice: level, busy, click | own voice: level, busy, click |
|---:|---|---|---|
| 12000 | opposite | −2.36 dB, 2, −29.5 dB | 0.00 dB, 1, −24.2 dB |
| 12109 | quadrature | +0.23 dB, 2, −29.5 dB | 0.00 dB, 1, −11.2 dB |
| 12218 | in phase | +1.82 dB, 2, −29.5 dB | 0.00 dB, 1, −28.3 dB |
| 12000, Retrig off | random | +1.79 dB, 2, −8.3 dB | 0.00 dB, 1, −8.2 dB |

- **A new voice** adds the old release to the new note at a phase set by the timing: −2.4 to +1.8 dB here, steady for as long as the release lasts. This is the user's "the sound character changes a lot", and, as he found, Retrig makes no difference to it. Its click is the new note's own onset (−29.5 dB, the same as from silence).
- **Its own voice** is identical on every repeat, but the retrigger's oscillator jump clicks by up to −11.2 dB at the worst phase. (Retrig off: −8 dB, which is the init patch's A 0 attacking from the release's level, by design.)
- Busy voices for 16ths on one key (125 ms apart, each held 62.5 ms, R 1 s): **8.2 on average with a new voice, 1 with its own** (below). With an 8-key melody both are about 8.

### Question 3: freeing

**A release at knob 8** (8.877 s under E1's law) passes each level at these times after the note-off:

| Freed at | after the note-off | share of the release | the step left (click, dB re a unit step) |
|---|---:|---:|---:|
| −60 dB | 7.987 s | 90.0% | −83.4 |
| −80 dB | 8.755 s | 98.6% | −91.1 |
| −100 dB | 8.864 s | 99.9% | −108.6 |
| **exactly 0 (E10, as decided)** | 8.877 s | 100.0% | −157.9 (nothing) |

A threshold buys little. The decided release is an exponential towards an overshoot target, so it is a near-straight line in dB that steepens at the end and reaches 0 in its own time. −80 dB frees the voice 1.4% earlier. Over the 16ths above the busy count goes from 8.20 to 8.08, and the maximum stays 9.

**A held note sustaining at 0** (E11; S 0, D 300 ms, R 500 ms, key held 2 s):

| Rule | freed at | output | Sustain raised to 0.5 at 1 s, key held: RMS 1.1–1.9 s |
|---|---:|---|---:|
| release end | 2.001 s | — | 0.2496 (the note comes back) |
| also sustain-0 holds | 0.300 s | identical to the bit | 0.0000 (the note is gone) |

Freeing a silent hold is exact while nothing changes, and it saves 1.7 s of a voice here. A later sustain change finds the note gone. M1.5's modulation of the sustain, or anything acting on a held note, would find it gone too.

**An envelope on a part nothing hears** (A1 a sine, envelope A R 100 ms, envelope B R 5 s, key held 0.5 s):

| Part B | either envelope keeps it busy | only audible envelopes |
|---|---:|---:|
| filter B on `--`, no oscillators | 5.000 s | 0.100 s |
| filter B on B, no oscillators | 5.000 s | 0.100 s |
| filter B on `--`, B1 a sine (unheard) | 5.000 s | 0.100 s |
| filter B on B, B1 a sine (heard) | 5.000 s | 5.000 s |

`dsp::Voice::isActive()` keeps a silent voice busy for the length of a release nobody hears. In the init patch part B has no oscillators, so a long amp envelope B release holds every voice for that release.

### Question 5: busy voices and the Voices readout

**Busy voices** for 16ths at 120 BPM (each held 62.5 ms), R 1 s, Polyphony 16, read every 10 ms from 1 to 4 s:

| Rule | one key: mean, max | 8-key melody: mean, max |
|---|---:|---:|
| new voice, freed at the release's end | 8.20, 9 | 8.20, 9 |
| new voice, freed at −80 dB | 8.08, 9 | 8.08, 9 |
| own voice for a repeated key | 1.00, 1 | 8.00, 8 |

**The readout against the manual's screenshot.** The screenshot reads Polyphony 12, Voices 204/336. The patch here: A1 8 voices, A2 4, B1 8, B2 8 (28 per note), envelope A R 300 ms, envelope B R 2 s. Nine notes held, four released at 1 s, read at 1.5 s:

| Count | reads |
|---|---:|
| notes | 9 |
| R1: notes × every oscillator's voices | 252 |
| **R2: per note, the voices of the parts whose amp envelope is active** | **204** |
| the maximum (the manual's formula) | 336 |

R2 reproduces the screenshot's numbers exactly. R1 can only read multiples of 28. This doesn't prove Sylenth1 counts per part, since other counts could give 204 too (one that counts oscillator voices as they fade out, say), but it rules R1 out, if the screenshot is one real moment.

### Question 1: the policies

**A 32-note strum** (keys 36–67 rising 2 ms apart, held) at Polyphony 16, with a retrigger in place. The keys sounding at 1 s:

| Policy | resonant pad (A 200 ms) | init (A 0) |
|---|---|---|
| oldest | 52–67 | 52–67 |
| quietest | **36–50, 67** | 52–67 |
| released first, then oldest | 52–67 | 52–67 |
| same key first, then oldest | 52–67 | 52–67 |

Under quietest, with a slow attack, each new note takes the voice of the note just before it, which is still quiet in its attack. The newest 15 notes steal from one another and the oldest 15 survive. The pianist hears the end of the strum vanish.

**A held bass under a melody** (key 36 held from 0 to 4 s; 16ths over 8 keys from 0.5 to 3.5 s, each held 62.5 ms; R 1 s; Polyphony 8):

| Policy | the bass at 3.9 s | steals | of a held key |
|---|---|---:|---:|
| oldest | stolen | 17 | 1 |
| quietest | sounding | 17 | 0 |
| **released first, then oldest** | **sounding** | 17 | **0** |
| same key first, then oldest | stolen | 17 | 1 |

Oldest takes the held bass as soon as the melody fills the voices. Same key first does the same: the first time round no voice is on the new key, so it falls back to oldest.

### Question 4: Polyphony lowered, and Polyphony 0

The resonant pad, keys 48–55 held from 0 to 3 s. Polyphony 8 → 4 at 1 s, → 0 at 2 s, key 60 played at 2.5 s, Polyphony back to 8 at 4 s. The table gives busy voices at each time:

| Rule | 0.9 s | 1.5 s | 2.2 s | 2.6 s | 3.5 s | 5.5 s | the chord at 0.8 s | click at 1 s | note-ons ignored |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **leave them** (only new notes obey) | 8 | 8 | 8 | 8 | 8 | 0 | −15.3 | −18.2 | 1 |
| release the oldest over the limit | 8 | 8 | 4 | 4 | 0 | 0 | −15.3 | −18.4 | 1 |
| cut the oldest over the limit | 8 | 4 | 0 | 0 | 0 | 0 | −15.3 | **−3.5** | 1 |

- **Leaving the notes and releasing them** are both silent at the change: −18 dB, under the chord's own −15 dB.
- **Cutting** clicks at −3.5 dB.
- **Every rule ends with 0 busy voices:** the note-offs reach voices beyond the new limit. A note-off that looked only within the limit would leave them hanging, which may be the v3.031 bug.
- **At Polyphony 0 the new note is ignored.**

### Real-time terms

A sequence with steals, releases and a Polyphony change: the resonant pad free-running, with drift on at 1.5 cents, Polyphony 4 (2 from 1 s), a held key and 14 melody notes at offsets off every grid. It runs for 2 s at host blocks of 1, 37 and 4096:

| Policy | identical to the bit (against 4096) | allocations from `prepare()` on | steals |
|---|---:|---:|---:|
| retrigger, released first | 3 of 3 | 0 | 11 |
| cut, oldest | 3 of 3 | 0 | 11 |
| fade 5 ms then start, quietest | 3 of 3 | 0 | 11 |
| fade 5 ms on a spare, same key first | 3 of 3 | 0 | 11 |

- **Seeds from the index:** two voices (index 0 and 1) on the same free-running note differ. The same index twice is identical.
- **A voice reused after it finished.** The test plays key 52 on a voice that has finished a note, and compares it with key 52 on a fresh voice. Stopped first, it is identical. Left as it stood, it differs by up to 0.0148 within 200 ms. A finished `dsp::Voice` isn't processed, but its slots still count as sounding and its drift as active. So its next note makes a band-limited jump from where the oscillators froze, not an onset (Retrig on), or continues the frozen phase (Retrig off), and the drift doesn't draw a fresh state. **`dsp::Voice` needs a call that silences a finished voice without restarting its random sequences**; the prototype does it through the slots, drift and envelopes.

### CPU

`tools/bench.sh "[voice-allocation]"`, `relwithdebinfo`, Apple M1 Ultra (virtualised), 512 samples per run at 48 kHz:

| Benchmark | ns per sample | share of a core |
|---|---:|---:|
| 16 voices held through the allocator, init patch | 5,775 (361 per voice) | 27.7% |
| 16 voices held, worst case | 11,216 (701 per voice) | 53.8% |
| 64 note events per block (32 steals) on silent voices | 21.6 (11.0 µs per block) | |
| no events, silent voices | 1.95 (1.0 µs per block) | |
| a steal per block, init: cut / retrigger / fade 5 ms on a spare / fade 5 ms then the note | 6,185 / 6,202 / 6,365 / 6,168 | 29.7% / 29.8% / 30.6% / 29.6% |

- **The allocator costs nothing measurable next to the voices.** Sixteen voices through it cost 16 times one voice to within noise (361 and 701 ns per voice, against 361 and 700 for a voice alone in the same session).
- **Its bookkeeping is about 157 ns per note event,** a scan of the pool. That is 0.3 samples' worth at 48 kHz.
- **A fade on a spare costs 2.9% more than a cut** at a steal in every block, which is extreme. That is the spare voice rendering for 5 ms of every 10.7 ms block. The delayed fade costs nothing extra.

**Memory:** `dsp::Voice` is about 25 KB, so 16 voices are 400 KB, and 16 spares would add 400 KB. The allocator's own state is about 2 KB: a record per voice and its log.

## Recommendations

*Stage 1's recommendations, as written. The project owner approved the three changes to the voice the same day (6, `Voice::stop()` from 7, and 3c, refined in the build so a crossing part's envelope counts only through the filter's part's): [The decision](#the-decision-the-voices-part), [As built](#as-built-the-voices-part). The rest await a decision.*

| # | Question | Recommendation | Why | Alternatives |
|---|---|---|---|---|
| 1a | **Which voice a steal takes** | **The oldest released note (key up), else the oldest held note.** Ties go to note-on order. | It keeps held notes as long as anything is releasing: the bass under the melody survives (0 held notes stolen, against 1 for oldest). Unlike quietest, it never takes a note in its attack (the strum). It needs no envelope reads. Sylenth1's order is unknown (P1). | **Oldest:** simplest; Sylenth1 may do it; it steals held notes. **Quietest:** takes notes in a slow attack. **The quietest released note, else the oldest:** a refinement, if P1 finds Sylenth1 does it. |
| 1b | **How a stolen note ends** | **Retrigger in place** (as at Polyphony 1): the slots as Retrig says, the envelopes attacking from their level (E5), **the filters and the voice's grid kept** (6). | The developer: Polyphony 1 steals "the single voice", a "clickless restart". Kept filters measure −39 dB on the resonant pad, against −10 for a cut or the reset. No latency, no spare voices, and no extra cost. | **A fade on a spare, 5–10 ms:** cleaner by 2–10 dB (−42 to −49 on the pad), at +1 voice per overlapping steal (up to 16 for a block chord, +400 KB) and +2.9% CPU at one steal per block. Keep it as an option if P2 finds Sylenth1 overlaps. **A delayed fade:** the same click, but 5–10 ms late on steals only. **Cut:** −10 dB. |
| 2 | **A key played again** | **A new voice; the old release overlaps.** The same key can sound twice. | The developer, explicitly. Measured: up to ±2 dB of static comb on repeats, which he accepts ("Was that ever supposed to sound great though?"). | **Its own voice (recycling):** 1 voice per key, identical repeats, but the release is cut and the retrigger clicks (up to −11 dB with Retrig on). A Phase 2 option, not parity. |
| 3a | **When a voice is free** | **Keep E10: the release at exactly 0**, no threshold. | −80 dB frees a voice 1.4% earlier and moves the average busy count from 8.20 to 8.08. Exact 0 leaves no step and needs no constant. | A threshold of −80 or −100 dB, if E10's protocol finds Sylenth1's readout dropping early. |
| 3b | **A held note sustaining at 0** (E11) | **Busy until released**, as E11's hypothesis. Task 5 may skip rendering it while every envelope sits at exactly 0 (its output is exactly 0), keeping the voice. | Freeing it is exact until anything raises the sustain: automation, or M1.5's modulation. Then the note is gone (0 RMS against 0.25). Skipping saves the CPU without losing the note. | Free it (saves polyphony; the note is lost on a later change). E11 settles it on the demo. |
| 3c | **Envelopes no one hears** | **Only envelopes that can be heard keep a voice busy**: a part whose filter runs with something sounding in its input, or whose oscillators cross into the other filter (`envelopeAudible` in the prototype). | Exact, since what can't be heard outputs nothing. Measured: a 5 s release on an unheard part B holds the voice 5 s, against 0.1 s. | `isActive()` as it is (simplest; costs polyphony and CPU on patches like the init's). P4 asks the demo. |
| 4 | **Polyphony lowered, and 0** | **Sounding notes play on; only new notes obey. Polyphony 0 ignores new notes. Note-offs reach every voice.** | No click (−18 dB, under the chord), the simplest rule, and the v3.031 fix reads like a note that went on sounding past a switch to 0. | Release the oldest over the limit (also silent; frees sooner). Cut (−3.5 dB). P5 settles it. |
| 5 | **The Voices readout** | **Active: per busy note, the unison voices of the parts whose amp envelope is active and audible (R2). Maximum: every oscillator's voices × Polyphony (the manual), 0 at Polyphony 0 or Voices 0.** Computed by the allocator after each block, read by the UI at its own rate (M1.9, M1.11). | R2 reproduces the manual's 204/336, which R1 can't. The maximum is the manual's own formula. | R1 (notes × voices), or notes only. P6 settles it. |
| 6 | **`noteOn` on a sounding voice** | **Change `dsp::Voice::noteOn` so that a sounding voice keeps its filters' state and its grid**, counting the grid from the note-on from silence. The new note's keytrack reaches the filters at the next grid point, interpolated over that segment as any feed is. From silence nothing changes. | Measured: the reset is the steal's click on a resonant patch (−9.8 against −39.2 dB). The reset was decided for a voice from silence, where the filters are at rest anyway. | Keep the reset and fade on a spare instead (1b's alternative). |
| 7 | **The allocator's structure** | **In `libs/dsp`: a fixed pool (`std::array`, 16 voices), records of a few words per voice, no allocation. Each block split at its events, every voice rendered exactly up to each. Freeing checked at events and at each voice's own 32-sample points, never at block boundaries.** Seeds from the instance's and the voice's index. **A `Voice::stop()`** that silences a finished voice (slots, drift, envelopes) without restarting its random sequences. | Identical to the bit at blocks of 1, 37 and 4096, with 0 allocations (12 of 12 renders). Without `stop()` a reused voice doesn't start from silence (a 0.0148 difference measured). | Freeing at block boundaries (it depends on the block size, through drift's draws). |

### What task 3 needs from the allocator

- **Mono Legato** discards Polyphony (p. 29). The allocator needs a mono mode with one voice and a stack of held keys: a key released while others are held returns to one of them (which one is task 3's question). It also needs `Adsr::legatoGateOn` instead of a retrigger, and the previous pitch for the glide.
- **Portamento Normal** slides "only … whenever at least one other key is pressed" (p. 29). At each note-on the allocator must say whether another key is held. Slide mode always glides, so the voice needs its last pitch even after a release.
- **Polyphony 1 without Mono Legato** is a steal of the single voice at every note (1b). It is not legato: the envelopes retrigger.
- **The hold pedal** (CC 64). A record needs "sustained by the pedal" alongside "key down". Pedal up releases only voices whose keys are up (the developer, 2007). A key pressed again while sustained takes a new voice, as 2 (P7). For 1a, a pedal-sustained note counts as held.
- **Pitch bend** is global and reaches every voice. The allocator plays no part in it.

### What this pushes back on

- **E10** stands: measured against thresholds, exact 0 costs 1.4% of a release at −80 dB, and nothing else.
- **E11** stands as "not freed", with task 5's skip as the CPU saving.
- **V12/V13:** sixteen voices cost exactly sixteen times one through the allocator, so polyphony's cost is the voice's. Task 5's skipping and 3c both reduce it.
- **The as-built `dsp::Voice`:**
  - `noteOn` on a sounding voice should keep the filters (6);
  - it needs a `stop()` (7);
  - `isActive()` should count only audible envelopes (3c).

  These are the stage-2 changes to the voice, for the owner to approve. *Approved and built on 2026-11-21, [below](#as-built-the-voices-part).*

## The decision: the voice's part

Chosen by the project owner on 2026-11-21: the three changes to `dsp::Voice` that the recommendations need (Decisions log). The allocator's own recommendations (1a, 1b's choice of a retrigger, 2, 3a–3b, 4, 5 and 7's pool) are not yet decided.

- **A note-on on a sounding voice keeps the filters' state and the voice's grid** (recommendation 6). The grid counts from the note-on from silence. The new note's keytrack reaches the filters at the next grid point. Routing changes pending at that note-on apply there too, as during any note.
- **`Voice::stop()`** silences a finished voice (its slots and drift stop, its envelopes go idle) without restarting its random sequences, so its next note starts from silence (recommendation 7).
- **`isActive()` counts only what can be heard** (recommendation 3c).

## As built: the voice's part

[voice.hpp](../../libs/dsp/include/anoesis/dsp/voice/voice.hpp):

- **`noteOn(note, knobs)`** checks `isActive()` first:
  - From silence it does what it did: pending routing applies, and each filter is fed for this sample and reset, so the grid starts there.
  - On a sounding voice it starts the slots (a band-limited jump with Retrig on, free-running with it off), the drift (which continues) and the envelopes (an attack from their level), and stops there. The filters ring on, the grid runs on, and the next grid point feeds the new note's keytrack and applies any pending routing.
- **`stop()`** calls `stop()` on the four slots and the drift and `reset()` on both envelopes. It doesn't reset the filters or the grid: the next note-on, from silence, does that, and nothing between them can be heard.
- **`isActive()`** is true while some filter has an input (not `--`), has oscillators in that input (its own part's, or the other part's on AB, counting a slot's voices fading out), and its part's amp envelope hasn't released to 0.
  - It is a refinement of stage 1's rule. A part crossing into the other filter is heard only through that filter's part's envelope, which multiplies after the filter. So the crossing part's own envelope never keeps a voice busy on its own: once the envelope after the filter has closed, nothing is heard whatever the one before it does. Stage 1's `envelopeAudible` counted it; the prototype now follows the voice.
  - It follows the patch: a held note whose part loses its last oscillator (Voices to 0), or whose filter goes to `--`, stops counting once the oscillator has faded out. An allocator then frees the voice.
  - Before the first note-on it is false, as before.

### Tests

Three new test cases in [voice_test.cpp](../../tests/dsp/voice/voice_test.cpp):

| Test | What it pins down | Tolerance and why |
|---|---|---|
| A note-on on a sounding voice keeps the filters and the grid | A resonant patch (k 3.6 at 585 Hz, the worst oscillators, A on AB, drive ×2), key 52 at 2005 (off the grid) on a sounding key 45, at host blocks of 37: equal to the prototype voice with `keepFiltersOnRetrigger`, and unequal to the one that resets; equal up to the note-on. A routing change made at that note-on is the same as one made 5 samples later, before the next grid point | bit for bit: the same classes and arithmetic |
| `stop()` silences a finished voice | A note played to its end, then `stop()`, then key 52: equal to key 52 on a fresh voice (Retrig on, drift off), unequal without `stop()`. With drift on, `stop()` draws a fresh drift state (unequal to a voice stopped by hand except for its drift). A sounding voice stopped is silent at once and inactive. Free-running, the next note after `stop()` draws new phases, and after `reset()` repeats the first | bit for bit; exact zeros |
| `isActive()` counts only what can be heard | 50 ms after the note-off, with envelope A at R 20 ms and B at R 1 s (or swapped): part B without oscillators on filter B, or B1 on but filter B on `--`, inactive; B1 on filter B active; B1 crossing into filter A with envelope A closed inactive, with envelope B closed active. A held note whose only oscillator is set to Voices 0 is active while it fades and inactive after | exact (booleans) |

Red first, against an empty `stop()`: 10 assertions failed across the three. Then 12 deliberate breaks, each caught (4 after rewriting to compile under `-Werror`):

- **`noteOn`:** keeping nothing; resetting the grid on a sounding voice; applying pending routing there; treating every note-on as sounding.
- **`stop()`:** leaving the slots, the drift, or the envelopes running.
- **`isActive()`:** dropping the input check, the route check, or the crossing input; ignoring fading voices; counting either envelope.

### Results, as built

The `[voice-allocation-report]` now drives the voice as changed. Every stage-1 table reproduces to the printed digit, with one exception:

- **The end of a steal:** the kept row is now `dsp::Voice` itself, equal to the prototype that keeps its filters on all four patches. The reset row is the prototype with stage 1's `noteOn`.
- **The exception, question 2's own-voice column**, which retriggers through `dsp::Voice`. On the init patch (Retrig on, A 0) it clicks louder with the filters kept: −23.2 / −2.3 / −22.7 dB at the opposite, quadrature and in-phase timings, against −24.2 / −11.2 / −28.3 with the reset (Retrig off: −8.9 against −8.2).
  - The attack of 0 jumps the envelope from the release's level (about 0.35) to 1 at once. With the filters kept, that jump lifts the waveform the filter carries over to full level; with the reset, the filter's output started from 0.
  - Stage 1's steal table shows the same, smaller: kept is 1.9 dB worse on the init patch held and 2.9 dB worse on the ringing patch releasing, against 29–30 dB better on the resonant pad and 7.7 dB on the ringing patch held.
  - So keeping the filters removes the click wherever a filter rings, and lets through the one an instant attack makes on a carried-over signal. The manual calls an attack of 0 a click on purpose. That column is the own-voice alternative (2), which isn't the recommendation.

**CPU** (the same session, `relwithdebinfo`): `dsp::Voice` alone 359.52 / 235.37 / 685.31 ns per sample (init, filter B at `--`, worst case); 16 voices through the allocator 5,808.52 (init) and 11,245.88 (worst case); a steal per block 6,252.36 (cut) and 6,215.78 (retrigger). All are within 1% of stage 1's: `isActive()`'s extra checks are 4 slot reads per filter, every 32 samples per voice.

## References

- LennarDigital, *Sylenth1 User Manual*, v2.5, 2015, pp. 11–12 (Polyphony, Voices, the screenshot), 29 (Mono Legato, portamento), 30 (CPU tips), 31 (the CC map).
- The developer on KVR: [t=454264 (2016)](https://www.kvraudio.com/forum/viewtopic.php?t=454264) (voice stealing, repeated keys), [t=189597 (2007)](https://www.kvraudio.com/forum/viewtopic.php?t=189597) (the sustain pedal). Users: [t=454396 (2016)](https://www.kvraudio.com/forum/viewtopic.php?t=454396), [t=427171 (2014)](https://www.kvraudio.com/forum/viewtopic.php?t=427171). The [changelog](https://lennardigital.com/sylenth1/whatsnew.php) (v3.010, v3.023, v3.031). Every source is rated in the [feature inventory](../reference/sylenth1-features.md#voice-allocation).
- [voice-architecture.md](voice-architecture.md) (the voice), [envelopes.md](envelopes.md) (E5, E10, E11), [oscillator-phase.md](oscillator-phase.md) (Retrig's jump), [oscillator-drift.md](oscillator-drift.md) (drift per voice), [filters.md](filters.md) (the click measure).
