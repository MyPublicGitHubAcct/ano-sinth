# Mono Legato, portamento, pitch bend, and the hold pedal

These are the controls that move a note's pitch after its note-on, or that keep a note going after its key is up. This page covers M1.4 task 3:

- **The theory:**
  - mono modes and note priority;
  - legato against retrigger;
  - portamento's modes, time laws and curves, in mono and in poly;
  - the pitch bend's resolution, range and zipper noise;
  - the hold pedal;
  - where a moving pitch enters a band-limited oscillator, and how to keep all of it independent of the host's block size.
- **What is known about Sylenth1.**
  - The manual's controls. The default CCs: Portamento on CC 5, the pedal on CC 64.
  - The product description: portamento "will slide both velocity and pitch parameters in polyphonic or monophonic mode".
  - The developer, 2006: Slide is "sliding notes after pauses".
  - The changelogs: the bend's range up to 24 (v1.1), the pedal (v1.21, v3.023), Mono Legato (v3.024), MIDI learn for the bend (v3.072).
  - A 2016 user's hint about note priority, and reports of a stepped MIDI bend.
- **The candidates**, prototyped on a copy of `dsp::Voice` that adds a pitch input, driven through a copy of the decided `VoiceAllocator` with a mono mode and the pedal added.
- **What the candidates measured:**
  - note priorities;
  - the click at a legato step and at a returning key;
  - velocity at a legato note;
  - the glide's pitch against its formula, for each curve and each rate of applying the pitch;
  - glide lengths and an interrupted glide;
  - Normal and Slide, in mono and poly;
  - the zipper of 7-bit and 14-bit wheels, stepped or ramped;
  - the bend's range ends, and a released voice;
  - the filter following the glide and the bend at keytrack 10;
  - the pedal's rules;
  - block sizes and allocation;
  - CPU.
- **The recommendations** for the project owner, with the alternatives and the open question that settles each, and what the voice and the allocator would need to change.
- **The project owner's decision** (every recommendation) and **what was built**: `Glide`, `PitchBend`, the voice's pitch input, and Mono Legato, portamento, the bend and the pedal in `VoiceAllocator`, with the tests that pin them and their cost.

Read [voice-allocation.md](voice-allocation.md) (the allocator this extends), [keytracking.md](keytracking.md) (which pitch the filter follows, F18–F22), [envelopes.md](envelopes.md) (`legatoGateOn`, a retrigger from the current level), [oscillator-range.md](oscillator-range.md) (band-limited pitch steps) and [parameter-smoothing.md](parameter-smoothing.md) (zipper noise) first.

## Theory

### Mono modes and note priority

A mono synth plays one note at a time, but the player can hold several keys. It keeps a list of the held keys, and a **note priority** picks the key that sounds:

- **Last:** the most recent key still held. Releasing it goes back to the one pressed before it. This is the usual choice for leads, and trills work in both directions.
- **Low:** the lowest key held, as on many old monosynths (a Minimoog). A new key above the held one doesn't sound, so a held bass note can't be pushed aside.
- **High:** the highest key held. The melody stays on top.

When the sounding key is released and others are held, the synth **returns** to one of them. That return can be a legato note (the pitch moves, the envelopes go on) or a new note (the envelopes retrigger). Some synths don't return at all and simply release.

### Legato and retrigger

A **retrigger** starts the envelopes again for a new note. In this project the attack starts from the envelope's current level (E5), so it never jumps to 0. A **legato** note changes only the pitch. The envelopes go on (`Adsr::legatoGateOn`), so a phrase has one attack. Two more things can restart at a note change:

- **The oscillators' phase.** With Retrig on, a new note jumps every oscillator to Phase. That jump is band-limited, but it is a step in the waveform, and it clicks. A legato note can leave the phase alone.
- **The filters.** Here they always keep their state on a sounding voice (decided in M1.4 task 2).

At a legato step without portamento, the oscillators' frequency steps but their phase is continuous. A sine's slope changes at that sample (a corner), and a corner's spectrum falls as 1/f². So the step has some energy above 1 kHz, but much less than a jump in value.

### Portamento: when, how long, and what shape

**When it glides.** A *fingered* mode glides only from a key that is still held, so the player chooses each glide by playing legato. Sylenth1 calls this Normal: "it will only slide notes whenever at least one other key is pressed". An *always* mode glides to every new note from wherever the last one ended, even after a gap. Sylenth1 calls this Slide.

**How long it takes.** Two laws:

- **Constant time:** every interval takes the knob's time. A semitone and two octaves both take 100 ms.
- **Constant rate:** the knob sets a time per octave, so a semitone takes 1/12 of an octave's time. This is how an analog glide circuit that slews at a fixed rate behaves.

**The curve.** Three shapes:

- **Linear in semitones:** a straight line in pitch. It sounds even, because pitch is heard logarithmically.
- **Exponential:** an RC lag on a control voltage, as on most analog synths. The pitch moves fast at first and slows towards the target. Strictly it never arrives, so it needs a definition of "done". Here it has a time constant of T / ln 1000 and lands exactly on the target at T, where the remaining 0.1% of the interval is inaudible.
- **Linear in Hz:** a straight line in frequency. It is slow at the bottom and fast at the top of a rising glide, as heard in cents.

**An interrupted glide** is a new note played before the glide ends. The new glide can start from the pitch the old one has reached, which is continuous, or from the note it was heading for, which jumps there first.

**In poly,** each new note can glide too, but from what? Three choices:

- **From the last note played,** one global pitch. A chord played as a block all starts from one pitch, and a strummed chord cascades, each note from the note before it.
- **From its voice's own last pitch.** Chords move by voice leading, but only when voices are reused. A free voice that has never played has no last pitch, so which notes glide depends on allocation.
- **Not at all.**

### Pitch bend

MIDI sends the wheel as a 14-bit value, 0 to 16,383 with the centre at 8,192. A controller may send only the 7-bit MSB. That gives 128 values, so each step is range/64 semitones: 3.1 cents at a range of 2, and 37.5 at 24.

The wheel moves continuously but messages arrive every few milliseconds, so the bent pitch is a staircase. Each step is a pitch step: a corner in the phase at the step's sample, and a train of them at the message rate is **zipper noise** (sidebands around every partial). Ramping linearly to each new value over a few milliseconds removes the steps. The cost is a lag of about the ramp's length behind the wheel. The common scale divides by 8,192 both ways: full down is exactly −range and full up is range × 8,191/8,192. Dividing the upper half by 8,191 makes both ends exact.

A bend is usually global: every sounding voice follows the wheel, releasing ones included. The alternative is to freeze a voice's bend at its note-off, so a release keeps the pitch it had.

### The hold pedal

CC 64 is a switch by convention: 0–63 up, 64–127 down. With the pedal down, a key that is released keeps sounding, its gate held, until pedal-up releases it. Pedal-up should release only notes whose keys are up. Sylenth1's developer, in 2007: "it should only kill notes that are not being pressed". That leaves four questions:

- **A key played again under the pedal** takes a new voice (as any repeated key here, P3), or retriggers its own sustained voice, as a piano's hammer strikes the same string.
- **The steal order:** a sustained note's key is up, but it sounds at full level. It can count as held, as released, or between the two.
- **CC 64's threshold:** at ≥ 64, a pedal resting near 64 toggles with the controller's noise. Accepting any value above 0 holds a resting foot. Hysteresis avoids both.
- **In a mono mode:** the pedal can keep released keys in the held-key list, so the note doesn't change at their release. Or it can only delay the final release: the list follows the keys, and the pedal holds the last note's gate. In the second case, a key played while the pedal holds the note can be a new note or a legato one.

### Where a moving pitch enters the oscillators

`OscillatorSlot` takes a note for each call (`process(out, note)`) or for each sample (`process(out, notes)`). The oscillator's phase is continuous, so a pitch change never makes a step in the waveform. A change of `bandLimitedPitchStep` (2^18, about 2.9 Hz at 48 kHz) or more in one sample adds a ramp residual, so the corner it makes is band-limited ([oscillator-range.md](oscillator-range.md)). The choices for a moving pitch:

- **Per sample:** exact, at the cost of a note-to-frequency conversion per sample for each slot, and a frequency set per unison voice.
- **At each 32-sample grid point, held** for the segment: a staircase of small steps. Held from the segment's start, it lags by half a segment on average. Held from the segment's midpoint, it matches the average.
- **At each grid point, ramped** across the segment: per sample to the slot, so it costs nearly as much as per sample, but a glide's pitch is read only at the grid points.

Keytrack is fed the voice's pitch at each grid point, for the segment's end (F22, decided), whichever of these the oscillators use.

To stay independent of the host's block size, the glide is a closed form of the samples since it began. The bend is a ramp at absolute sample times, which every voice reads at its own samples, as the knob ramps are read with `valueAfter`.

## Sylenth1: what is known

The full source table with ratings and the open questions with their demo protocols are in the inventory: [§7 Mono Legato, portamento, pitch bend, and the hold pedal](../reference/sylenth1-features.md#mono-legato-portamento-pitch-bend-and-the-hold-pedal) (K1–K19), with P7 and §4's Mono Legato and Retrigger rows. In short:

- **The manual (p. 29).**
  - Mono Legato: "Sylenth1 will be forced into mono mode and the polyphony setting on the top bar will be discarded. It will also stop retriggering notes whenever another key is pressed. This enables you to slide notes up and down in pitch and velocity without actually restarting a new note."
  - Portamento: "The Portamento knob sets the amount of time it takes to slide from one note to another." Slide "will always slide the pitch to the next note played". Normal "will only slide notes whenever at least one other key is pressed, but it will not slide when you play a single note at a time".
  - Pitch bend: "You can specify the bending range in half notes using the BendRange control."
  - The default CCs (p. 31): Modwheel 1, **Portamento 5**, Hold Pedal 64, All Notes Off 123.
- **The product description** (the KVR listing): "2 Modes of portamento sliding, which will slide both velocity and pitch parameters in polyphonic or monophonic mode." So portamento works in poly, and "velocity" slides along with the pitch.
- **The developer, 2006** ([p=2126421](https://www.kvraudio.com/forum/viewtopic.php?p=2126421)): "SynthEdit does not support "slide-mode" portamento (sliding notes after pauses)." So Slide glides across a gap.
- **The changelogs:**
  - v1.1 (2006): "Maximum pitchbend range changed to 24 notes."
  - v1.20 (2007): "Eliminated clicking effects in Mono mode", "Fixed portamento sliding bug".
  - v1.21 (2007): "Correct implementation of Midi CC's 64 (hold pedal) and 123". Then the developer: the pedal "should only kill notes that are not being pressed".
  - v2.0 (2008): a "complete rewrite of the … voice management".
  - v3.020 (2016): "Pitch accuracy improved".
  - v3.023 (2016): "Sustain pedal issue fixed".
  - v3.024 (2016): "Fixed crash after switching Mono Legato".
  - v3.072 (2022): "Added MidiLearn for PitchBend".
  - v3.073 (2022): "Fixes pitch wheel midi link bug".
- **Users.**
  - In 2016 ([p=6349094](https://www.kvraudio.com/forum/viewtopic.php?p=6349094)), a user's favourite mono mode is Polyphony 1, and "Sometimes I do miss a combined retrigger & last note priority mono mode". By elimination, Mono Legato returns to a held key without retriggering, and Polyphony 1 retriggers without remembering held keys.
  - Several report a MIDI bend that steps while the GUI wheel is smooth (2010, 2016), not reproduced by everyone.
  - A 2022 user: in Slide "you will also hear the attack/transient portion of every note" when notes don't overlap, which reads as a glide with a retrigger.
- **Unknown:**
  - the note priority;
  - what a returning key does;
  - what "velocity" slides;
  - Retrig on a legato note;
  - the time law, the curve and the knob's range;
  - where a poly voice glides from;
  - the bend's smoothing, its effect on released voices and its default range;
  - CC 64's threshold;
  - the pedal in Mono Legato and in the steal order.

## The candidates

In [legato_candidates.hpp](../../tests/dsp/voice/legato_candidates.hpp):

- **`PitchedVoice`** is `dsp::Voice` as built, with a pitch input added and nothing else changed. `dsp::Voice` takes its pitch once, at `noteOn()`, so nothing can glide or bend it.
  - Its pitch is a `Glide` (per voice: a closed form from the samples since it began) plus the bend (a global `BendRamp` at absolute sample times, read through the voice's knobs).
  - It adds `legatoNoteOn()`, which leaves the envelopes alone (`legatoGateOn`), keeps the drift and the filters, and restarts the slots only if asked.
  - It keeps a velocity that a legato note can ramp. For measurement only, it can multiply its output by that velocity.
  - Keytrack follows the pitch at each grid point, for the segment's end.
- **`Engine`** is the decided `VoiceAllocator`: the same pool of 16, the same records, the same steal order and freeing. It adds a mono mode (voice 0, a held-key stack of up to 128 keys), a map of the keys held, the pedal, the bend, and the bookkeeping for where a glide starts.

| Topic | Candidates |
|---|---|
| **The pitch's rate** | per sample; per 32-sample grid point, held from the segment's start; held from its midpoint; ramped across it (the pitch for the rest of the segment is refreshed at a note-on mid-segment) |
| **The glide's curve and time** | linear in semitones at a constant time; linear in semitones at a constant rate (the time per octave); exponential (an RC lag landing at T); linear in Hz |
| **An interrupted glide** | from the pitch reached; from the last target |
| **When it glides** | Normal (only while another key is held); Slide (always) |
| **Poly portamento** | from the last note played (its pitch now, if it sounds); from the voice's own last pitch; no glide |
| **Note priority** | last; low; high |
| **A released key's return** | legato; retriggered (from the envelopes' level) |
| **Retrig on a legato note** | the phases kept; restarted (a band-limited jump to Phase) |
| **Velocity at a legato note** | at once; ramped over the glide; kept from the phrase's first note |
| **The bend** | stepped at each message, or ramped linearly over 2–20 ms; 14-bit or MSB only; scaled by 8,192 both ways or exactly to both ends; reaching released voices, or frozen at their note-off |
| **CC 64** | down at ≥ 64; at > 0; at ≥ 64 and up below 48 (hysteresis) |
| **A key again under the pedal** | a new voice; its sustained voice, retriggered |
| **A sustained note in the steal order** | as held; as released; between (released notes first, then sustained, then held) |
| **The pedal in Mono Legato** | the stack follows the keys and the pedal holds the last gate; released keys stay in the stack until pedal-up; the first, and a key played while the pedal holds the note is legato |

Counted as models, that is 4 + 4 + 2 + 2 + 3 + 3 + 2 + 2 + 3 + 6 + 3 + 2 + 3 + 3 = **42**.

**Patches.** The click measurements use sines, as in voice-allocation.md:

- **Init:** A1 one sine, LP24 at 4,677 Hz, k 0, the envelopes a gate (A 0, S 1, R 0), Retrig on.
- **Ringing:** one sine into LP24 at 585 Hz, k 3.96; A 5 ms, R 300 ms, Retrig on.
- **Pad at −12 dB:** A1 and A2 four detuned, spread, free-running sines each, at Volume 2.5, into LP24 at 585 Hz, k 3.6; A 200 ms, D 500 ms, S 0.8, R 1 s. At full Volume, eight sines drive the ladder so hard that its own band above 1 kHz is at −37 dB at E3, which hides any event.

The pitch is read from one sine with filter A on Bypass.

## What they measured

By the hidden `[legato-report]` ([legato_report.cpp](../../tests/dsp/voice/legato_report.cpp), about 7 s) and the `[legato]` benchmarks ([legato_benchmark.cpp](../../tests/dsp/benchmarks/legato_benchmark.cpp)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[legato-report]"
tools/bench.sh "[legato]"
```

### The prototype against the library

At a constant pitch the prototype is the library, to the bit:

- **`PitchedVoice` against `dsp::Voice`:** a note, a retrigger in place at sample 24013 (off the grid), and a note-off. Init, ringing, and the pad with drift on, at host blocks of 1, 37 and 4096, at each of the four pitch rates: identical to the bit, 36 of 36.
- **The allocator sequence** of voice_allocator_test.cpp (the pad with drift, steals, a repeated key, Polyphony 4 → 2 → 6), at blocks of 37. `VoiceAllocator<PitchedVoice>` against `VoiceAllocator<dsp::Voice>`: identical. The `Engine` with no glide, bend or pedal, against `VoiceAllocator<dsp::Voice>`: identical.

So every difference below comes from what task 3 adds.

### Note priority and a released key

Mono Legato, Portamento 0. The key sounding after each event:

| Event | last | low | high |
|---|---|---|---|
| C3 down | C3 | C3 | C3 |
| G3 down | G3 | C3 | G3 |
| E3 down | E3 | C3 | G3 |
| E3 up | G3 | C3 | G3 |
| G3 up | C3 | C3 | C3 |
| G3 down | G3 | C3 | G3 |
| C3 up | G3 | G3 | G3 |
| A3 down | A3 | G3 | A3 |
| A3 up | G3 | G3 | G3 |
| G3 up | G3, released | G3, released | G3, released |

Last makes 7 legato notes, low 1 and high 5, and none of them retriggers. Each priority behaves as its definition says. The table is what K1's protocol reads on the demo.

### The click at a legato step and at a returning key

Key 45 held from 0, then either key 52 at 24013 (a step) or key 52 pressed at 12007 and released at 24013 (a return to 45). The measure is energy above 1 kHz in a Hann 4096 centred 32 samples after the event, in dB re a unit step. The rows are Mono Legato, except the retrigger rows (Polyphony 1, not legato).

| Event | portamento | init | ringing | pad at −12 dB |
|---|---|---:|---:|---:|
| steady at key 45 (no event) | | −88.5 | −101.8 | −110.6 |
| steady at key 52 (no event) | | −56.9 | −109.3 | −113.7 |
| **legato, phases kept** | 0 | −34.9 | −60.1 | −73.4 |
| **legato, phases kept** | 30 ms | **−58.2** | **−90.9** | **−96.8** |
| legato, phases restarted (Retrig) | 0 | −21.0 | −47.9 | −73.4 |
| legato, phases restarted (Retrig) | 30 ms | −21.1 | −48.4 | −96.8 |
| retrigger (Polyphony 1) | 0 | −21.0 | −47.9 | −74.1 |
| retrigger (Polyphony 1) | 30 ms | −21.1 | −48.4 | −88.8 |
| **return: legato, phases kept** | 0 | −48.5 | −71.4 | −70.2 |
| **return: legato, phases kept** | 30 ms | **−54.6** | **−89.0** | **−97.2** |
| return: legato, phases restarted | 0 | −6.6 | −30.3 | −70.2 |
| return: legato, phases restarted | 30 ms | −7.1 | −32.2 | −97.2 |
| return: retriggered | 0 | −6.2 | −34.6 | −70.0 |
| return: retriggered | 30 ms | −7.4 | −36.6 | −92.6 |

- **Restarting the phases is the click.** With Retrig on, a legato note that restarts the oscillators clicks at −21 dB on the init patch, as a retrigger does, whatever the portamento. With the phases kept it is −34.9 dB, the frequency step's corner alone. On a return it is worse: −6.6 dB restarted against −48.5 kept. The restart's jump lands wherever the old note's phase was, so it can be a full-scale step.
- **A 30 ms glide removes the corner.** Legato with the phases kept is −58.2 dB on the init patch, which is the patch's own level at E3 (−56.9, the ladder's saturation harmonics, higher at E3 than at A2). It is −90.9 on the ringing patch and −96.8 on the pad, within 10–17 dB of steady.
- **On the free-running pad,** Retrig doesn't apply, so the rows differ only by the glide. A retrigger there still costs 8 dB against legato at 30 ms (−88.8 against −96.8): the envelopes attack again from 0.8 to 1.
- **A return clicks the same as a step** when it is legato, and with the same glide.

### Velocity at a legato note

The init patch in Mono Legato with the phases kept, the output multiplied by the velocity (a stand-in for a route through a Misc panel). Key 45 at velocity 127 is held, and key 52 at velocity 32 is played legato at 24013. The click is measured as above, and the level from 50 to 150 ms after the step is given against the same span before it.

| Velocity at a legato note | portamento | click (dB) | level after (dB) |
|---|---|---:|---:|
| taken at once | 0 | −27.0 | −12.00 |
| taken at once | 30 ms | −27.4 | −11.99 |
| ramped over the glide | 0 | −27.0 | −12.00 |
| **ramped over the glide** | 30 ms | **−60.4** | −11.99 |
| kept from the first note | 0 | −34.9 | −0.02 |
| kept from the first note | 30 ms | −58.2 | −0.02 |

- **A velocity taken at once is a 12 dB gain step**, −27 dB whatever the glide.
- **Ramped over the glide**, it goes as smoothly as the pitch (−60.4 dB at 30 ms). At Portamento 0 it is a step too, which is what "slide" can mean with no glide time.
- **Kept**, nothing moves but the pitch. The manual's "slide … in pitch and velocity" rules this out, unless it describes the note's voice and not its velocity.

### The glide's pitch

A sine with filter A on Bypass, in Mono Legato: C3 held, C4 legato at 24000, 100 ms. Each period's pitch (from zero crossings) is compared with the formula's mean over that period, from the note-on to 50 ms after the glide. Cents, largest / RMS:

| Curve | per sample | grid, held from the start | grid, held from the midpoint | grid, ramped |
|---|---|---|---|---|
| linear in semitones, constant time | 0.00 / 0.00 | 3.96 / 2.70 | 0.24 / 0.10 | 0.00 / 0.00 |
| linear in semitones, 100 ms per octave | 0.00 / 0.00 | 3.96 / 2.70 | 0.24 / 0.10 | 0.00 / 0.00 |
| exponential (RC), constant time | 0.00 / 0.00 | 16.75 / 4.13 | 0.82 / 0.17 | 0.13 / 0.04 |
| linear in Hz, constant time | 0.00 / 0.00 | 5.18 / 2.66 | 0.22 / 0.09 | 0.01 / 0.00 |

- **Per sample is exact** on every curve.
- **Held from the segment's start lags by half a segment.** That is 4 cents on a linear octave in 100 ms: 16 samples × 12 semitones / 4800 samples = 0.04 semitones, as predicted. On the exponential's steep start it is 17 cents.
- **Held from the midpoint** is within 0.24 cents on the linear curves and 0.82 on the exponential.
- **Ramped across the segment** is exact on a linear curve, and within 0.13 cents on the exponential's curvature.

The length, measured to the last period more than 1 cent from the target, per sample:

| Curve | a semitone | an octave | two octaves |
|---|---:|---:|---:|
| linear in semitones, constant time | 97.4 ms | 100.9 ms | 99.6 ms |
| linear in semitones, 100 ms per octave | 8.2 ms | 100.9 ms | 199.1 ms |
| exponential (RC), constant time | 66.5 ms | 96.9 ms | 99.1 ms |
| linear in Hz, constant time | 97.3 ms | 98.0 ms | 98.9 ms |

The two time laws separate by a factor of 24 between a semitone and two octaves, which K7's protocol reads directly. The exponential reaches within 1 cent of a semitone in two-thirds of its time, so its audible length depends on the interval as well.

**An interrupted glide:** C3 held, C5 legato at 24000 (200 ms, linear), and E4 at 28800, where the glide is at C4.

| Restarts from | largest step between periods | pitch 2 ms after |
|---|---:|---:|
| the pitch reached | 51.7 cents (the glide's own slope over one period) | 60.09 |
| the last target | 808.1 cents | 71.91 |

Restarting from the last target jumps to C5 and then glides down. That is audible as a blip, and nothing in the evidence suggests it.

### Normal and Slide, mono and poly

**Mono Legato with a 50 ms glide.** The pitch 1 ms after E3's note-on:

| Playing | Normal | Slide |
|---|---|---|
| C3 then E3, overlapping | 48.08: glides, legato | 48.08: glides, legato |
| C3 then E3, with a gap | 52.00: jumps, retriggered | 48.08: glides, retriggered |

Normal and Slide differ only after a gap. Slide glides from the last note and retriggers the envelopes, since nothing is held: the developer's "sliding notes after pauses", and the 2022 user's "attack/transient".

**Poly with a 50 ms glide.** The chord C3 E3 G3 (strummed 100 samples apart) is held to 24000, then F3 A3 C4 are played at 30000, 30100 and 30200, with R 1 s. Each new note's pitch 1 sample after its note-on is where it glides from:

| Polyphony | glides from | mode | F3 | A3 | C4 |
|---|---|---|---:|---:|---:|
| 16 | the last note | Normal | 53.00 | 53.00 | 53.17 |
| 16 | the last note | Slide | 55.00 | 54.92 | 55.01 |
| 16 | its voice's last pitch | Normal or Slide | 53.00 | 57.00 | 60.00 |
| 3 | the last note | Normal | 53.00 | 53.00 | 53.17 |
| 3 | the last note | Slide | 55.00 | 54.92 | 55.01 |
| 3 | its voice's last pitch | Normal | 53.00 | 52.00 | 55.00 |
| 3 | its voice's last pitch | Slide | 48.00 | 52.00 | 55.00 |

- **From the last note,** every new note glides from the one before it, at the pitch it has reached. In Normal the first note of the second chord doesn't glide, because nothing is held (53.00). In Slide it glides from G3 (55.00). A strum cascades. A block chord would start every note from one pitch.
- **From its voice's last pitch,** the result depends on allocation:
  - At Polyphony 16 the chord takes fresh voices that have never played, so nothing glides.
  - At Polyphony 3 the steals reuse C3's, E3's and G3's voices, so the chord moves by voice leading: C3→F3, E3→A3, G3→C4. In Normal the first note still doesn't glide.
- The same patch at a different Polyphony glides differently, which only "from the last note" avoids.

### The bend

A3 (a sine, filter A on Bypass) is held, and the wheel moves ±Bend Range at 2 Hz from 0.1 s. It sends a message every 2 ms (96 samples, 13 samples after each grid point), as 14-bit values or MSB only. The zipper is the power outside 150–320 Hz (the band A3 sweeps, with the window's lobes) over 0.68 s, in dB re the total. The pitch error is each period against the wheel's exact sine, largest / RMS cents, lag included.

| Wheel (range 2) | per sample | grid, held | grid, ramped | pitch error (per sample) |
|---|---:|---:|---:|---|
| 14-bit, stepped | −75.5 | −75.5 | −75.5 | 2.7 / 1.8 |
| 7-bit, stepped | −64.2 | −64.2 | −64.2 | 4.9 / 2.4 |
| **14-bit, 5 ms ramp** | **−113.8** | −91.9 | −113.1 | 12.5 / 8.9 |
| 7-bit, 5 ms ramp | −74.5 | −74.6 | −74.1 | 14.3 / 9.0 |
| 7-bit, 10 ms ramp | −81.4 | −81.2 | −81.1 | 26.4 / 17.7 |
| 7-bit, 20 ms ramp | −87.8 | −86.5 | −87.5 | 50.4 / 34.4 |

At range 12 on A2 (the power outside 40–300 Hz, per sample): 14-bit stepped −60.3, 7-bit stepped −53.7, 7-bit with a 10 ms ramp −63.5.

- **A 14-bit wheel ramped over 5 ms** takes the zipper from −75.5 to −113.8 dB. The cost is a lag of about the ramp: 12.5 cents at the steepest point of a fast wheel (25 semitones per second), against 2.7 stepped.
- **A 7-bit wheel steps 11 dB louder** (−64.2), and ramps help it less. Each step of 3.1 cents (37.5 at range 24) is much larger than a ramp can hide without lagging 25–50 cents. The stepping reports could be 7-bit controllers.
- **The grid's ramp can't anticipate a message.** At a grid point the segment's end is read before the message has arrived. So a step, ramped or held, lands at the next grid point unchanged: the stepped rows are identical across rates, and per grid point the pitch error grows from 2.7 to 3.7 cents. Held per grid point, a 5 ms ramp leaves a staircase of 32-sample steps at −91.9 dB. Ramped per grid point, it is −113.1, as per sample.

**The range's ends.** Dividing by 8,192 both ways leaves full up short by 0.024 / 0.146 / 0.293 cents at ranges 2 / 12 / 24. Dividing the upper half by 8,191 is exact at both ends. Neither difference can be heard.

**A released voice and the wheel.** A3 is bent to +2, released at 0.5 s with R 1 s, and the wheel goes back to 0 at 0.6 s. At 0.8 s the release plays 57.00 when it follows the wheel and 59.00 when frozen at its note-off.

### The filter following the glide and the bend

LP24 self-oscillating (Resonance knob 7.5, k 4.5) at keytrack 10, with A1 a sine at −80 dB to start it, the cutoff set so the filter sings near the note, and 3 s to settle. The filter is fed at each grid point with the voice's pitch at the segment's end (F22). Its pitch, period by period, is compared with the oscillators' (the key, the glide's formula, and the bend the voice reads at each sample), after taking out its steady detune. Cents, largest / RMS:

| Motion | largest | RMS |
|---|---:|---:|
| a legato octave glide, C3–C4, 100 ms, at a grid point | 0.14 | 0.07 |
| the same, 13 samples after a grid point | 0.24 | 0.07 |
| the wheel ±2 at 2 Hz, 14-bit every 2 ms, stepped | 2.79 | 1.25 |
| the same wheel, 5 ms ramp | 0.14 | 0.07 |

- **The filter follows a glide to 0.24 cents.** This confirms on the voice what [keytracking.md](keytracking.md) measured on a stand-in.
- **A note-on off the grid** reaches the filter at the next grid point, as decided, which costs 0.1 cents here.
- **A stepped wheel** puts the oscillators on each step at its sample and the filter at the next grid point, so they part by up to 2.8 cents. A 5 ms ramp lets the filter follow to 0.14 cents.

### The hold pedal

**A key played again under the pedal (P7).** The pedal goes down at 0. C3 is played from 1000 to 5000, then again from 10000 to 14000. E3 is held from 12000, and the pedal comes up at 20000, with R 1 s.

| C3 again | voices on C3 at 16000 | C3's gates on at 21000 | E3's gate on at 21000 |
|---|---:|---:|---|
| **a new voice** | 2 | 0 | yes |
| its sustained voice | 1 | 0 | yes |

With a new voice, the key sounds twice under the pedal, as a repeated key does without it (P3). Pedal-up releases both C3s and leaves the held E3 sounding: "only kill notes that are not being pressed".

**The steal order under the pedal.** Polyphony 8, with a bass C2 held from 0 to 4 s. The pedal is down from 0.25 to 3.7 s. Sixteenth notes over 8 keys run from 0.5 to 3.5 s, each 62.5 ms long and sustained by the pedal, with R 1 s.

| A sustained note counts as | the bass at 3.6 s | steals |
|---|---|---:|
| held | **stolen** | 41 |
| released | sounding | 41 |
| between: after released notes, before held keys | sounding | 41 |

Counted as held, every sustained note is as protected as the bass under the player's hand, so the oldest of them, the bass, goes first. Counted as released, or between, the bass survives. "Between" also lets a note already fading go before a sustained one at full level.

**CC 64's threshold.** The number of pedal toggles (down or up) for three controller signals, a value every 5 ms with ±4 of noise:

| Down at | a full press and release over 2 s | resting at 20 | resting at 62 |
|---|---:|---:|---:|
| **≥ 64** | 10 | 0 | 178 |
| > 0 | 5 | 1 | 1 |
| ≥ 64, up below 48 | 2 | 0 | 1 |

- **At ≥ 64,** a noisy pedal chatters at the threshold: 10 toggles for one press, and 178 when resting near 64.
- **At > 0,** a pedal resting at 20 holds every note.
- **Hysteresis** gives one toggle per motion. That is a controller's problem as much as the synth's, and the MIDI convention is ≥ 64.

**The pedal in Mono Legato** (Portamento 0, last note). The key sounding after each event:

| Event | the stack follows the keys; the pedal holds the last gate | released keys stay in the stack until pedal-up | the first, and a key played while the pedal holds the note is legato |
|---|---|---|---|
| pedal down; C3 down; E3 down | E3 | E3 | E3 |
| E3 up | **C3** | E3 | **C3** |
| pedal up | C3 | C3 | C3 |
| C3 up | C3, released | C3, released | C3, released |

| Event | the stack follows the keys; the pedal holds the last gate | released keys stay in the stack until pedal-up | the first, and a key played while the pedal holds the note is legato |
|---|---|---|---|
| pedal down; C3 down; C3 up | C3, held by the pedal | C3 | C3, held by the pedal |
| E3 down | E3, **retriggered** | E3, legato | E3, **legato** |
| E3 up | E3, held by the pedal | E3 | E3, held by the pedal |
| pedal up | E3, released | E3, released | E3, released |

- **When the stack follows the keys,** releasing E3 over a held C3 returns to C3, as it does without the pedal. But a key played while the pedal holds a note retriggers, because no key is down.
- **When released keys stay in the stack,** a melody under the pedal never returns to the key still held, until pedal-up.
- **The third rule** keeps the first's returns and makes a key played under a pedal-held note legato. The pedal then means "still held" for the envelopes, as Mono Legato's "stop retriggering notes whenever another key is pressed" suggests.

### Real-time terms

Two 2 s sequences, each rendered at host blocks of 1, 37 and 4096, at all four pitch rates:

- **Mono Legato:** Slide, 60 ms, last note, returns, a gap, and the pedal.
- **Poly:** Polyphony 4, Normal, 60 ms from the last note, steals, and the pedal.

Both have the wheel sending every 2 ms (5 ms ramp), the pad with drift, keytrack 10 and k 3.6.

| Sequence | per sample | grid, held from the start | grid, held from the midpoint | grid, ramped | allocations |
|---|---|---|---|---|---:|
| mono | identical | identical | identical | identical | 0 |
| poly | identical | identical | identical | identical | 0 |

Every block size is identical to 4096, to the bit (16 of 16), with 0 allocations from `prepare()` on. The glide is a closed form from its start, and the bend a ramp at absolute sample times. Each voice reads both at its own samples, and the engine renders every voice exactly up to each event. So nothing depends on the host's block size.

### CPU

`tools/bench.sh "[legato]"`, `relwithdebinfo`, Apple M1 Ultra (virtualised), 512 samples per run at 48 kHz. Each row is the median of three runs. One voice, ns per sample:

| Benchmark | init (A1 one saw) | worst case (4 × 8 saws, AB, drive ×4) |
|---|---:|---:|
| `dsp::Voice`, held | 367.6 | 746.9 |
| `PitchedVoice`, constant pitch, grid held / ramped / per sample | 366.2 / 371.2 / 374.6 | |
| `PitchedVoice`, gliding 3 octaves a second, grid held | 382.0 | 797.0 |
| `PitchedVoice`, gliding, grid ramped | 383.2 | 843.1 |
| `PitchedVoice`, gliding, per sample | 386.1 | 843.8 |
| `PitchedVoice`, the wheel every 96 samples, grid held / ramped / per sample | 367.1 / 373.3 / 375.5 | |

The engine's events, 512-sample blocks, ns per sample, with the cost per event:

| Benchmark | ns per sample | per event |
|---|---:|---:|
| Mono Legato, one key held, no events (init) | 363.5 | |
| … 64 legato note events per block | 386.8 | 186 ns |
| … 64 legato note events with a 30 ms glide | 385.1 | 172 ns |
| … 64 pedal toggles per block | 379.0 | 123 ns |
| poly, 64 note events per block on voices with no oscillators (each note-on from silence) | 158.4 | 1,267 ns |
| `VoiceAllocator<dsp::Voice>`, the same events | 153.6 | 1,229 ns |

- **The pitch input costs nothing at a constant pitch** on the held path: 366.2 against 367.6.
- **Per sample at a constant pitch costs 1.9% more:** the slot's per-sample path converts a note every sample. So a voice should use the constant path whenever its pitch isn't moving in a chunk. At a constant pitch the two paths are identical to the bit, so switching between them is exact.
- **A moving pitch costs 5% (init) to 13% (worst case) per sample,** against 4% and 7% held per grid point. Ramped per grid point costs the same as per sample: the slot's per-sample conversion and the 32 unison voices' frequency sets are the cost, not reading the glide.
- **Per event:**
  - A legato note costs about 180 ns. A pedal toggle costs about 120 ns, mostly the render split at the event.
  - A poly note-on from silence costs 1.27 µs, and the library's 1.23 µs. So the glide's and the pedal's bookkeeping add about 38 ns per event. The rest is `dsp::Voice`'s own note-on from silence (the filters fed and reset).
- **Noise:** in one run of five, one row read +130% and another +40%, and neither reproduced in the other runs. That is this virtual machine's noise, so the table uses medians.

**Memory:** `PitchedVoice` adds three 32-float buffers (pitches, notes, velocity gains) and the glide's and velocity's state to `dsp::Voice`'s 25 KB, about 0.4 KB. The engine adds a 128-key map, a 128-key stack and a bend ramp, about 0.7 KB.

## Recommendations

*Stage 1's recommendations, for the project owner to choose from. Where only the demo can settle a question, the recommendation is the working hypothesis in the inventory (K1–K19), and the measurement is what makes it the safer default. Chosen by the project owner on 2026-11-21, every one ([The decision](#the-decision), [As built](#as-built)). The inventory's questions were numbered L1–L19 in stage 1's commit, which collided with the LFOs' L1–L14; they are K1–K19 here and in the inventory.*

| # | Question | Recommendation | Why | Alternatives | Settled by |
|---|---|---|---|---|---|
| 1 | **Mono Legato's note priority** | **Last note**: the most recent key still held sounds; releasing it returns to the key held before it. | The 2016 user's "retrigger & last note priority mono mode", read by elimination; the usual choice; trills work both ways (7 legato notes in the sequence against 1 for low). | Low (a held bass can't be displaced); high. | K1 |
| 2 | **A returning key** | **A legato note**, gliding as any legato note (in Normal too, since a key is still held). | Legato returns at −48.5 dB (init) against −6.2 for a retrigger and −6.6 for a phase restart; the user's hint that Mono Legato doesn't retrigger. | A retrigger from the envelopes' level. | K2 |
| 3 | **Retrig and Phase on a legato note** | **The phases are kept**: a legato note never restarts the oscillators. | "without actually restarting a new note"; a restart clicks at −21 dB on a step and up to −6.6 dB on a return, whatever the glide. | Restart per Retrig (then legato only spares the envelopes). | K4 |
| 4 | **Velocity at a legato note** | **Ramped linearly to the new velocity over the glide's length**, at once at Portamento 0. The same wherever a note glides, in poly too (not measured there). | The product description: portamento "will slide both velocity and pitch"; −60.4 dB at 30 ms against −27.0 at once. | At once; kept from the phrase's first note (no velocity change at all). | K3 |
| 5 | **Mono Legato and Polyphony** | **One voice, whatever Polyphony, 0 included.** | "the polyphony setting on the top bar will be discarded". | Mono Legato silent at Polyphony 0. | K5 |
| 6 | **Switching Mono Legato while notes sound** | **The switch releases every sounding note**, clears the stack, and keys still down sound when next played. Not measured. | The simplest transition that can't hang a note (v3.024 fixed a crash there). | Carrying the most recent held key over into the new mode. | K6 |
| 7 | **The glide's time law** | **Constant time.** | "the amount of time it takes to slide from one note to another". | Constant rate per octave (a semitone in 8.2 ms, two octaves in 199 ms at 100 ms per octave). | K7 |
| 8 | **The glide's curve** | **Linear in semitones**, landing exactly at its time. | An even glide in pitch, exact length, no definition of "done" needed. | Exponential (an RC lag; within 1 cent of a semitone in two-thirds of its time); linear in Hz. Each is one switch in the glide. | K8 |
| 9 | **The knob, and Portamento 0** | **The DSP takes seconds; 0 is an exact jump.** The knob's law belongs to the parameter table (M1.9). Placeholder: E1's 25 s · x^4.64 (0.6 ms at knob 1, 14 ms at 2, 1 s at 5). | No source for the range. The placeholder fits a user's "above 1" before a glide is heard. | (576^x − 1)/23 (39 ms at knob 1); 10 s · x³; 2 s · x². | K9 |
| 10 | **An interrupted glide** | **From the pitch it has reached.** | Continuous: the largest step is the glide's own slope (51.7 cents a period), against an 808-cent jump from the last target. | From the last target. | K10 |
| 11 | **Normal and Slide** | **Normal glides only from a key still held. Slide always glides from the last note's pitch, and after a gap it is a new note (the envelopes retrigger from their level).** | The manual; the developer's "sliding notes after pauses"; a user's "attack/transient" without overlap. | Slide after a gap as a legato note (no retrigger). | K12 |
| 12 | **Portamento in poly** | **Poly notes glide too, from the last note played, at the pitch it has reached.** Normal glides only while another key is held. | The product description says poly; the same patch glides the same way at any Polyphony (from its voice's own last pitch, nothing glides at Polyphony 16 and the chord voice-leads at 3). | From the voice's own last pitch (voice-led chords when voices are reused); no poly glide. | K11 |
| 13 | **The rate the pitch enters the oscillators** | **Per sample while the pitch moves (a glide, a bend ramp), the slot's constant path otherwise.** Keytrack stays as decided (F22: at grid points, for the segment's end). | Exact on every curve (0.00 cents) and for a bend at its sample. The constant path keeps a held note at today's cost (366 against 375 ns), and the two are identical to the bit at a constant pitch. A moving pitch costs 5–13%. | Grid, held from the midpoint (0.24 cents on a linear glide, 0.82 exponential; the bend's ramp a −92 dB staircase; 4–7% instead of 5–13%). Grid, ramped (exact on a linear glide, the same cost as per sample). | Not the demo's: a cost/accuracy choice. |
| 14 | **The bend's resolution and smoothing** | **14-bit, with a linear ramp to each new value over 5 ms**, read by every voice at its own samples. | −113.8 dB of zipper against −75.5 stepped (7-bit: −74.5 against −64.2), for a lag of at most the ramp (12.5 cents at a fast 2 Hz wheel). | Stepped (Sylenth1 may step: the user reports); 10 ms (−81 dB on 7-bit, 25 cents of lag); a one-pole. | K14 |
| 15 | **The Bend Range** | **One control, 0–24 semitones, the same up and down; 14-bit values scaled by 8,192 both ways.** | The manual's one control "in half notes"; v1.1's 24; full up 0.29 cents short at 24, inaudible, and the same step size both ways. | Exact at both ends (/8,191 up). The default range from the demo. | K13 |
| 16 | **The bend and released voices** | **Every sounding voice follows the wheel**, releasing ones included. | A global pitch offset is simplest and what a wheel usually does. | Frozen at the note-off (the release keeps the pitch it had: 59.00 against 57.00 in the test). | K15 |
| 17 | **MIDI learn for the bend** | **An interop feature (M1.10)**: the wheel is a learnable control. Nothing in the DSP. | v3.072: "Added MidiLearn for PitchBend". | — | K16 |
| 18 | **CC 64's threshold** | **Down at 64 and above.** | The MIDI convention; v1.21's "Correct implementation of Midi CC's 64". | Hysteresis (one toggle per motion against 10 for a noisy pedal; 178 resting at 62); > 0. | K17 |
| 19 | **The pedal in poly** | **Pedal-up releases only notes whose keys are up; a key played again under the pedal takes a new voice** (P7 as hypothesised). | The developer, 2007; measured: the key sounds twice, and pedal-up leaves a held key sounding. | Retriggering the sustained voice (1 voice on the key). | P7 |
| 20 | **A sustained note in the steal order** | **Between: released notes first, then sustained ones, then held keys** (oldest first within each). | Counted as held, a pedalled melody steals the bass under the player's hand (stolen, against sounding); a sustained note's key is up, as the developer's "notes that are not being pressed". | As released (no difference from a fading note); as held. | K19 |
| 21 | **The pedal in Mono Legato** | **The held-key stack follows the keys; the pedal holds the last note's gate; a key played while the pedal holds the note is legato.** | Returns work as without the pedal, and a note held by the pedal counts as held for the envelopes (0 retriggers against 1). | The stack follows the keys and a new key retriggers; released keys stay in the stack until pedal-up. | K18 |

### What the voice needs

- **A pitch input.** `noteOn(note, knobs, glide, velocity)` jumps or glides. A new `legatoNoteOn(note, knobs, glide, velocity, rule)` leaves the envelopes to `legatoGateOn`, keeps the slots' phases, the drift and the filters, and moves the pitch. Two pieces of state:
  - a per-voice `Glide`: a closed form from the samples since it began, linear in semitones at a constant time, from a given pitch;
  - a velocity ramp, the value M1.5's velocity source reads.
- **The bend**, read through the voice's knobs. One global ramp at absolute sample times is set by the allocator at each bend event, and every voice reads it at its own samples, as the knob ramps are read with `valueAfter`. `VoiceFilterKnobs`, or a knobs struct around it, gains a pointer to it and the absolute sample of the block's start.
- **The master tune**, which task 3 also owns (keytracking.md, F19). The slots' `setReferenceHz`, and `masterTuneSemitones` added to keytrack's pitch.
- **Rendering:**
  - while the pitch moves in a chunk, the slots get a note per sample (`process(out, notes)`, drift added);
  - otherwise they use the constant path as today, which is identical to the bit;
  - at a note-on mid-segment, the new pitch applies at once.
- **Keytrack** from the voice's pitch (glide + bend + master tune) at each grid point, for the segment's end, instead of `voiceNote`.
- **`currentPitch()`** for the allocator's glide origin. `stop()` keeps the pitch, so Slide glides from the last note after a voice has been freed.

### What the allocator needs

- **A mono mode:**
  - voice 0, ignoring Polyphony;
  - a held-key stack (fixed, 128 keys) and a map of the keys down;
  - last-note priority;
  - legato note-ons while a key is held, and legato returns;
  - a new note (a retrigger in place, from the envelopes' level) when none is held;
  - a switch that releases everything.
- **The glide's bookkeeping:** the last note played and its voice, for the pitch it has reached; Normal's "another key held" from the key map; Slide always.
- **The pedal:**
  - a "sustained" flag per record;
  - note-offs under the pedal set it instead of releasing;
  - pedal-up releases the sustained voices whose keys are up;
  - the steal order's three tiers;
  - in mono, a note held by the pedal counts as held for legato.
- **Events:** `VoiceEvent` gains a velocity for note-ons, and new kinds for the pedal (CC 64's value) and the bend (its 14-bit value), handled at their samples like the others.
- **Measured cost:** about 38 ns per note event over today's allocator; a legato note about 180 ns, a pedal toggle about 120 ns.

### What this pushes back on

- **voice-allocation.md's "a pedal-sustained note counts as held"** (written for task 3, not decided): measured, it steals a held bass under a pedalled melody. Recommendation 20 puts sustained notes between released and held.
- **E5** stands for every retrigger here: a gap in Slide, Polyphony 1, and the alternative return.
- **F18–F22** stand. On the voice, the filter follows a glide to 0.24 cents and a 5 ms bend ramp to 0.14, fed at grid points.
- **`dsp::Voice`'s "the note's glide, bend and master tune are task 3's":** the changes above, for the owner to approve.

## The decision

Chosen by the project owner on 2026-11-21: every recommendation, 1–21 (Decisions log).

- **Mono Legato:**
  - last-note priority;
  - returns as legato notes, gliding as any legato note;
  - the oscillators' phases kept;
  - the velocity ramped over the glide;
  - one voice whatever Polyphony, 0 included;
  - a switch releases every note.
- **Portamento:**
  - constant time, linear in semitones, from the pitch reached;
  - Portamento 0 is a jump, and the knob's law is the parameter table's;
  - Normal glides only from a held key, and Slide always, so after a gap the next note is a new note that glides;
  - poly notes glide from the last note played.
- **The pitch:** per sample while it moves, the constant path otherwise; keytrack as decided (F22).
- **The bend:** 14-bit, ramped over 5 ms, 0–24 semitones both ways, scaled by 8192, reaching every sounding voice; MIDI learn for it is M1.10's.
- **The pedal:**
  - down at 64 and above;
  - pedal-up releases only keys that are up;
  - a key played again takes a new voice;
  - a sustained note comes between released and held notes for stealing;
  - in Mono Legato the stack follows the keys, and a key played under a pedal-held note is legato.

## As built

- **[glide.hpp](../../libs/dsp/include/anoesis/dsp/voice/glide.hpp), `Glide`:** a linear ramp in closed form from the samples since it began, from + (to − from) × n / N.
  - N = round(seconds × rate). The time is clamped to 60 s. A time of 0, NaN or a negative one, or a glide to the note it is on, is a jump.
  - `at(ahead)` reads ahead without advancing, which is how the voice feeds keytrack for a segment's end. `advance(n)` moves on, and the value lands exactly on the target after N samples.
  - `startSamples()` gives the velocity's ramp the pitch's length.
  - The arithmetic is the prototype's, so the voice is the prototype's to the bit.
- **[pitch_bend.hpp](../../libs/dsp/include/anoesis/dsp/voice/pitch_bend.hpp), `PitchBend` and `pitchBendFraction()`:**
  - `pitchBendFraction()` is (value − 8192) / 8192, clamped to 0–16383.
  - The range is 0–24 semitones (default 2; NaN ignored) and applies from the next value.
  - Each value is reached by a 5 ms linear ramp (240 samples at 48 kHz) from where the bend is, at absolute sample times.
  - `isSteadyFrom(t)` tells a voice when it can use its constant path.
- **[voice_knobs.hpp](../../libs/dsp/include/anoesis/dsp/voice/voice_knobs.hpp):**
  - `VoiceKnobs` is what was `VoiceFilterKnobs` in voice.hpp, renamed and moved here so the allocator needn't include the voice. It gains `bend` (a `PitchBend*`) and `time` (the absolute sample of the block's first sample), and `advanced(n)` moves both. The filter fields and their order are unchanged, so every caller that set them still compiles.
  - `NoteStart` says how a note-on moves the pitch and the velocity: the velocity, whether it glides, from where, over what time, and the velocity a gliding note ramps from.
- **[voice.hpp](../../libs/dsp/include/anoesis/dsp/voice/voice.hpp), `dsp::Voice`:**
  - **`noteOn(note, knobs, start = {})`** jumps or glides the pitch. The velocity is taken at once, or ramped from `start.fromVelocity` over the glide. The rest is unchanged: from silence the filters are fed and reset; on a sounding voice they keep their state.
  - **`legatoNoteOn(note, knobs, start)`** moves the pitch the same way and ramps the velocity from where it is over the glide. The envelopes get `legatoGateOn()`: a held one goes on, and a released one opens again. Nothing else restarts: the phases whatever Retrig says, the drift, the filters and the grid. On a voice that isn't sounding it is `noteOn()`.
  - **Rendering.** A chunk (at most 32 samples) is *moving* when the glide hasn't ended at its start, or the bend isn't steady from there.
    - Moving, the slots get a note per sample (glide + bend + drift) through `process(out, notes)`.
    - Otherwise they get one note, as before. At a constant pitch the two are identical to the bit, so switching between them is exact.
  - **Keytrack** follows the glide plus the bend plus `masterTuneSemitones()`, read at each grid point for the segment's end (F18, F19, F22), and at a note-on from silence for its sample.
  - **`setMasterTune(hz)`** sets the four slots' reference and keytrack's offset. Anything not finite and positive counts as 440.
  - **Readouts:** `currentPitch()`, `targetPitch()`, `hasPlayed()`, `velocity()`. `stop()` keeps the pitch, so Slide glides from the last note after its voice was freed. `reset()` puts it at A4.
- **[voice_allocator.hpp](../../libs/dsp/include/anoesis/dsp/voice/voice_allocator.hpp), `VoiceAllocator`:**
  - **Its interface:**
    - `VoiceEvent` gains a velocity (1–127, clamped) and three kinds: `pedal` (CC 64's value), `pitchBend` (14-bit) and `monoLegato` (0 or not).
    - Settings: `setMonoLegato`, `setPortamento(seconds)` (NaN or negative counts as 0), `setPortamentoMode` (`PortamentoMode::normal`, `slide`) and `setBendRange`.
    - Readouts: `monoKey()`, `isPedalDown()`, `isSustained(i)`.
    - `AllocatableVoice` now requires `VoiceKnobs`, the three-argument `noteOn`, `legatoNoteOn`, `currentPitch()`, `hasPlayed()` and `velocity()`.
  - **What it keeps:** a map of the 128 keys down, a stack of the keys held in Mono Legato, the last key played and its velocity, the voice that took it, the pedal, a `sustained` flag per record, one `PitchBend`, and a sample clock from `prepare()`/`reset()` that gives the bend's absolute times. Each block's knobs get the bend and the clock.
  - **Mono Legato:**
    - Voice 0 alone, with no Polyphony check (0 included).
    - A note-on while no key is held (and no pedal-held note) is a new note: `noteOn`, a retrigger in place if the voice sounds. Otherwise it is a legato note on the new key.
    - A note-off of the sounding key returns, legato, to the most recent key still held.
    - The last key up releases the voice, or, under the pedal, marks it sustained.
    - The glide starts from the voice's `currentPitch()`: in Normal only for a legato note; in Slide always, once the voice has played.
  - **Poly:** as before, plus:
    - Normal glides when another key is down. Slide always glides, once a note has played.
    - The glide starts from the last note played: its voice's `currentPitch()` and `velocity()` if that voice still plays it, else its key and velocity.
    - Under the pedal a note-off marks the record sustained instead of releasing. Pedal-up releases every sustained record whose key is up.
    - A steal takes the oldest of the lowest tier: released (0), sustained (1), held (2).
  - **A switch of Mono Legato** (a setter or an event) releases every busy voice whose key is down or sustained, and clears the stack and the mono key. The same value again does nothing.
  - **`reset()`** frees every voice, lets go of every key and the pedal, and puts the bend at 0 and the clock at 0. It keeps the settings: Polyphony, Mono Legato, portamento and the Bend Range.
- **Settled in the build:**
  - A gliding note-on ramps its velocity from the last note's: recommendation 4's "wherever a note glides", which the prototype left as a jump. Velocity reaches no audio until M1.5, so the output is still the prototype's to the bit.
  - The stage-1 report's `VoiceAllocator<PitchedVoice>` row is retired. The allocator's concept now requires the library's `VoiceKnobs`, and the report's `Engine` row (the prototype engine against `VoiceAllocator<dsp::Voice>`) covers the same comparison.

### Tests

[glide_test.cpp](../../tests/dsp/voice/glide_test.cpp) and [legato_test.cpp](../../tests/dsp/voice/legato_test.cpp) have 21 test cases. The allocator's stub voice moved to [stub_voice.hpp](../../tests/dsp/voice/stub_voice.hpp), which also records legato notes and the pitch each note started from.

| Test | What it pins down | Tolerance and why |
|---|---|---|
| Glide | a jump; an octave in 100 ms is 4800 samples, at(1200/2400/4800) = 51/54/60; every sample within 3.9e-6 of the line; constant time for a semitone and two octaves; advancing equals reading ahead; 0, negative, NaN and tiny times jump; clamped at 60 s; in samples; the rate | exact where the arithmetic is exact; otherwise one rounding each of the product and the sum (≤ 3.9e-6 below 64) |
| PitchBend | the fraction at 0, 4096, 8192, 16383 and clamped outside, `constexpr`; 0 until a value; the ramp's start, middle and end at 48 kHz, steady from 240 samples on; a value mid-ramp continuous; the range from the next value, clamped, NaN ignored; 96 kHz; reset | exact (1 ULP at the ramp's middle) |
| The voice against the prototype | dsp::Voice against the stage-1 `PitchedVoice` (per sample) through glides on legato notes and on a retrigger, a legato note at Portamento 0, a 2 Hz wheel every 96 samples, velocities, a note-off; three patches (the pad with drift, a crossing patch with Retrig on, a sine); host blocks 1/37/4096 against 4096 | bit for bit; 0 allocations from `prepare()` on |
| A glide on its line | after each sample of an octave glide, the oscillator's increment against the formula | 0.01 cents (the increment's quantum is 1e-4 cents, a float note's 4e-4) |
| The bend | per sample through a value's 240-sample ramp; no bend without one in the knobs; `currentPitch()` without the bend | 0.01 cents |
| A legato note leaves the envelopes and phases alone | the decay goes on (a note-on attacks); the envelopes' levels equal a voice with no legato note, bit for bit, while the pitch is the new note's; a released envelope opens again; Retrig on and Phase 180: the largest step between samples at the change within 1% of the new pitch's own slope, and a retrigger more than twice it | bit for bit; 1% (one sample's slope change at the corner) |
| Keytrack | the filter's cutoff after a glide's segment (52, then 60), with the bend at full range 12, and at 432 Hz, against the steady cutoff | 2e-5 relative (the cutoff law in float) |
| The master tune | A4 plays `incrementFor(432)`; NaN, 0, −5, inf count as 440 | exact |
| Velocity | at once at a note-on; ramped over a legato glide (0.8 → 0.5 at half → 0.2); at once at Portamento 0; a gliding note-on from `fromVelocity`, a jumping one not | 1e-6 at a ramp's middle; exact at its ends |
| The pitch through `stop()` and `reset()` | 54 half way, kept by `stop()`; a jump; 69, unplayed and velocity 1 after `reset()` | exact |
| Mono Legato's priority | on a stub: C3, G3, E3 down, released in turn, the returns legato to the key held before; a non-sounding key's release changes nothing; the last up releases; only voice 0 plays; Polyphony 0; a new phrase a retrigger | exact |
| Normal and Slide | the first note jumps; a legato one glides from the pitch the voice reached (49.5); a return too, at its velocity; a gap: Normal jumps, Slide glides from the last note at its velocity; Portamento 0 never glides | exact |
| An interrupted glide | on dsp::Voice: C5 over C3 in 200 ms reaches 60 half way; E4 there glides from 60 (62 after another half) | 1e-5 |
| Poly portamento | the first note doesn't glide; one over a held key glides from the last note's voice's pitch (48.7) and velocity; then from the next; nothing held in Normal: no glide; Slide after the last note's voice was freed: from its key | exact |
| A switch of Mono Legato | three held notes released; the same value twice changes nothing; a released key's later note-off changes nothing; the next note plays mono; an event at its sample back to poly | exact |
| The pedal in poly | 63 up, 64 down; a key released under it sustained, not released; played again, a new voice; pedal-up releases both and leaves the held key; the steal order released → sustained → held, with the sustained note older than the released one | exact |
| The pedal in Mono Legato | a release returns to the held key; the last up leaves the note sustained; a key under it legato; pedal-up with a key held changes nothing; with none, releases | exact |
| The bend through the allocator | on dsp::Voice: a held and a releasing voice both bent at the value's sample; the range from the next value; `reset()` with the pedal down and a mono note: all released, no bend after | 0.01 cents |
| Velocity | 64 → 64/127, 0 → 1/127, 200 → 1 | exact |
| The allocator against the prototype | VoiceAllocator<dsp::Voice> against the stage-1 `Engine` in its decided configuration (per sample): the mono sequence (Slide, returns, a gap, the pedal) and the poly one (Normal, steals, the pedal), both with the wheel; host blocks 1/37/4096 | bit for bit; 0 allocations from `prepare()` on |

**Red first**, against inert headers with the decided interface (`Glide` and `PitchBend` returning 0, `legatoNoteOn` doing nothing, the allocator ignoring the new events and settings): 20 of the 21 test cases failed (158 assertions).

- During the red run, three tests turned out to pass for the wrong reason, and were strengthened before going on:
  - the envelopes' and the phases' sections passed with a legato note that did nothing, so both now require the new pitch;
  - the steal order passed with the pedal ignored (every note simply released), so the sustained note is now older than the released one, which only the tiered order puts second.
- Then 30 deliberate breaks of the new code:
  - **The first pass caught 27.** They covered:
    - the glide not ramping, not advancing, or its time not clamped;
    - the bend stepping, ramping from 0, scaled by 8191, or its range unclamped;
    - a legato note restarting the slots or retriggering the envelopes;
    - the voice never per sample, or deaf to a bend's ramp;
    - keytrack without the master tune, or at the segment's start;
    - a legato velocity taken at once;
    - the master tune not reaching the slots;
    - a return to the first key held, or as a retrigger;
    - Normal gliding after a gap;
    - a poly glide from the chosen voice, or ignoring held keys in Normal;
    - pedal-up releasing held keys;
    - no legato under a pedal-held mono note;
    - CC 64 above 64;
    - a switch releasing nothing;
    - `reset()` keeping the bend;
    - the glide from the last target;
    - no bend reaching the voices.
  - **One survived:** counting a sustained note as held. In the steal-order test the sustained note was also the oldest, so "as held" stole it at the same point.
  - **One didn't compile** under `-Werror` (a bend of 0 that left its arguments unused). It was rewritten, and then caught.
  - The steal-order test was strengthened: the held key is now the oldest, the sustained note between, and the released note the newest. With one more break (a sustained note counted as released), all 30 were caught.

### Results, as built

- **Bit for bit the stage-1 prototype:**
  - the voice: 9 of 9 (three patches at three block sizes);
  - the allocator: 6 of 6 (two sequences at three block sizes);
  - 0 allocations.
- **The stage-1 report reproduces unchanged**, except its retired row.
- **The task-1 and task-2 tests pass unchanged.** Only the stub voice moved to a shared header, and the knobs type was renamed.

**CPU** (`tools/bench.sh "[legato]"`, `relwithdebinfo`, the same machine, the median of three runs; one voice; ns per sample):

| Benchmark | as built | the stage-1 prototype (this session) |
|---|---:|---:|
| `dsp::Voice`, init, constant pitch | 369.23 | 368.32 (`dsp::Voice` before task 3's rows: 367.64) |
| `dsp::Voice`, init, gliding 3 octaves a second | 383.48 | 387.34 (per sample) |
| `dsp::Voice`, init, the wheel every 96 samples | 367.21 | 375.50 (per sample) |
| `dsp::Voice`, worst case, gliding | 843.63 | 849.80 (per sample); `dsp::Voice` constant 743.65 |
| `VoiceAllocator` Mono Legato, one key held, no events | 361.85 | 359.84 |
| … 64 legato note events per block | 388.86 (216 ns per event) | 389.99 |
| … 64 with a 30 ms glide | 389.49 (221 ns per event) | 386.79 |
| … 64 pedal toggles per block | 381.00 (153 ns per event) | 377.82 |
| `VoiceAllocator`, 64 poly note events with glides on voices with no oscillators | 156.98 (1.26 µs per event) | 161.50 (`Engine`) |

- **A constant pitch costs what it did** (369.23 against 368.32 in the same runs): the constant path is the old one.
- **A moving pitch costs 4% (init) to 13% (worst case)**, as stage 1 measured for per sample.
- **An event** costs about 0.2 µs in Mono Legato and 1.26 µs for a poly note-on from silence, within noise of the prototype.
- One run read 1,252 ns for `dsp::Voice`'s worst case, against 743 in the other two: this machine's noise again.

**Memory:** `dsp::Voice` gains two 32-float buffers and two glides, about 0.3 KB on 25 KB. `VoiceAllocator` gains the key map, the stack, the bend and a few words, about 0.7 KB on its 400 KB of voices.

## References

- LennarDigital, *Sylenth1 User Manual*, v2.5, 2015, pp. 29 (Pitchbend, Mono Legato, Portamento) and 31 (the CC map). The [KVR product page](https://www.kvraudio.com/product/sylenth1-by-lennardigital) (the developer's description).
- The changelogs: [whatsnew.php](https://lennardigital.com/sylenth1/whatsnew.php) (v3.020, v3.023, v3.024, v3.072, v3.073); the old news pages [oldnews1.php](https://www.lennardigital.com/home/oldnews1.php) (v1.1, v1.11), [oldnews2.php](https://www.lennardigital.com/home/oldnews2.php) (v1.20, v1.21), [oldnews3.php](https://www.lennardigital.com/home/oldnews3.php) (v2.0–v2.2).
- The developer on KVR: [p=2126421 (2006)](https://www.kvraudio.com/forum/viewtopic.php?p=2126421) (slide mode), [p=2769347 (2007)](https://www.kvraudio.com/forum/viewtopic.php?p=2769347) (the pedal), [p=6371072 (2016)](https://www.kvraudio.com/forum/viewtopic.php?p=6371072) (the bend). Users: [p=6349094 (2016)](https://www.kvraudio.com/forum/viewtopic.php?p=6349094), [t=456194 (2016)](https://www.kvraudio.com/forum/viewtopic.php?t=456194), the [Ableton forum (2010)](https://forum.ableton.com/viewtopic.php?t=146970), the [Image-Line forum (2017)](https://forum.image-line.com/viewtopic.php?t=171160), [audiosex.pro (2022)](https://audiosex.pro/threads/sylenth1-question-glide-portamento-legato.65708/). Every source is rated in the [feature inventory](../reference/sylenth1-features.md#mono-legato-portamento-pitch-bend-and-the-hold-pedal).
- The MIDI 1.0 specification: pitch bend as a 14-bit value centred on 8,192; CC 64 off at 0–63, on at 64–127; RPN 0 as the pitch-bend sensitivity.
- [voice-allocation.md](voice-allocation.md) (the allocator), [keytracking.md](keytracking.md) (F18–F22), [envelopes.md](envelopes.md) (`legatoGateOn`, E5), [oscillator-range.md](oscillator-range.md) (band-limited pitch steps), [oscillator-phase.md](oscillator-phase.md) (Retrig's jump), [parameter-smoothing.md](parameter-smoothing.md) (zipper), [filters.md](filters.md) (the click measure).
