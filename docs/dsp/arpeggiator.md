# The arpeggiator and the step sequencer

An arpeggiator turns held keys into a sequence of notes: it plays them one after another at a steady rate, in an order set by its mode, over a range of octaves. A step sequencer gives each step of that sequence its own transpose, velocity and tie. This page covers M1.6:

- **Theory:** where an arpeggiator sits; the held-note set and the modes that order it; octaves and wrap; what happens when the held keys change mid-sequence; the clock (steps at exact samples, gate, swing, ties); velocity; a step sequencer as a modulation source (a gater); and the arpeggiator's notes going into the voices.
- **Sylenth1** (task 1): what is known about its arpeggiator, and what that means for the build (tasks 2–5).

Read [tempo-sync.md](tempo-sync.md) (exact note values, and why a step's sample must be computed and not summed), [voice-allocation.md](voice-allocation.md) and [legato-portamento.md](legato-portamento.md) (where the arpeggiator's notes go), and [modulation-sources.md](modulation-sources.md) (Step Velocity, a global source with a stand-in until now) first.

## Theory

### Where an arpeggiator sits

An arpeggiator works on notes, not on audio. It sits between the keyboard and the voice allocator. It reads the key events (and perhaps the hold pedal), and it writes note events of its own. The voices see only its notes, so polyphony, mono legato, portamento, velocity and keytrack all act on the arpeggiator's notes as if they had been played. With the arpeggiator off, the keys go straight to the allocator.

So an arpeggiator has three parts:

1. **The held-note set.** The keys that are down now, each with its velocity and the order it was pressed in.
2. **The pattern.** Given the set and the step number, it gives the note (or notes) to play at that step. This is the mode, with Octave and Wrap.
3. **The clock.** It gives the sample at which each step starts and ends (Time, Gate, Swing), and the step number.

The pattern is a pure function of the set and the step number. The clock knows nothing about notes. Keeping the two apart makes each one easy to test, and it lets an external clock drive the same pattern (a VCV Rack module's clock and reset inputs, R1).

### The modes

Most modes sort the held keys and walk through them. With C3, E3 and G3 held and an Octave range of 2, the expanded set is C3 E3 G3 C4 E4 G4 (n = 6):

| Mode | One cycle | Length |
|---|---|---|
| Up | C3 E3 G3 C4 E4 G4 | n |
| Down | G4 E4 C4 G3 E3 C3 | n |
| Up/Down, ends not repeated | C3 E3 G3 C4 E4 **G4** E4 C4 G3 E3 | 2n − 2 |
| Up/Down, ends repeated | C3 E3 G3 C4 E4 **G4 G4** E4 C4 G3 E3 **C3** | 2n |
| Down/Up (either variant) | the same, starting from the top | 2n − 2 or 2n |
| Ordered | the keys in the order they were pressed, octave by octave | n |
| Random | a note drawn from the set at each step | none |

The two Up/Down variants differ only at the turns. Without the repeat, the top and bottom notes are played once per cycle, so they come up half as often as the others, and the rhythm of a cycle is 2n − 2 steps. With the repeat, every note comes up twice per cycle, and the cycle is 2n steps, which divides evenly into bars when n is a power of 2. Synths that offer both usually name the repeating one "2" (Novation's SL MkIII and Steinberg's HALion arpeggiators do), but the numbering isn't a standard.

Two orders are possible for the octaves: **note-major** (all held notes in the lowest octave, then all of them an octave up, as above) and **octave-major** (each note in every octave before the next note: C3 C4 E3 E4 G3 G4). Note-major is the common one, and it is the one the word "range" suggests.

**Random** has three common forms. In the first, each step is an independent draw, so a note can repeat and over n steps some notes may not sound at all. In the second, a shuffle, each cycle of n steps plays every note once in a random order. In the third, a random walk, each step moves one place up or down. Their statistics tell them apart: with 4 notes, an independent draw repeats the last note at 25% of steps, and a shuffle repeats only across a cycle's boundary, at about 1 step in 16. Ours must be **reproducible**: the draws come from the seedable generator ([white-noise.md](white-noise.md)), so the same MIDI renders the same audio.

**Step modes** don't sort anything. A step sequencer holds a pattern of offsets (here, a transpose in semitones per step), and the mode adds the step's offset to a base note. One key held plays the pattern from that key. A new key transposes it. A chord mode adds the offset to every held key at once, so each step is a chord.

### Octave and Wrap

**Octave** sets how many octaves the set is spread over: 1 is the held keys alone. **Wrap** restarts the pattern after N steps, whatever the mode's own length. Wrap is how a pattern is made to fit a bar: 3 held keys over 2 octaves in Up is a 6-step cycle, and Wrap 4 turns it into C3 E3 G3 C4, repeated. Wrap also cuts a mode short. With n notes and Wrap ≤ n, Up/Down never reaches its way down, so it sounds the same as Up. In a step mode, Wrap is the pattern's length.

Three choices follow:

- what Wrap counts (notes as they are played, so each octave's notes count, or the held keys);
- what Wrap 0 means (no wrap is the obvious reading);
- whether a step mode's own steps count rests.

### When the held keys change

Keys go down and up while a pattern plays. There are three reasonable rules for what the pattern does next:

- **Continue by index.** The step counter goes on, and the next step reads the new set at the counter's position. It is simple and keeps the rhythm, but a key added below the current one makes a note repeat or be skipped (in Up, at E3 of C3 E3 G3, adding D3 makes the next index read E3 again).
- **Continue by pitch.** The next note is the one after the last note played, in the mode's direction (after E3 in Up, G3). It sounds smoother, but it needs the last note's pitch and a search, and it doesn't make sense for Ordered or Random.
- **Restart.** Any change restarts the pattern from step 1. It is easy to hear, but it makes the rhythm stumble whenever a chord changes.

All three keep the clock running while any key is held. All three must also decide what happens when the last key goes up (the sequence stops, but does the sounding note end at once or at its gate?) and when a key follows silence (a fresh start from step 1, at the key press or at the next step on a grid). A key pressed between two steps joins at the next step. The arpeggiator follows its own clock, not the keys' timing, so a chord whose keys arrive a few milliseconds apart starts with whichever key came first.

**Latch and the hold pedal.** A latch keeps the set after the keys go up, until new keys replace it. The hold pedal can act on the input or the output. On the input, a released key stays in the set until pedal-up, which works like a latch. On the output, the pedal sustains the arpeggiator's own notes, so they pile up into a wash. A third option is for the arpeggiator to ignore the pedal.

### The clock

**Steps at exact samples.** The step length L is a note value at the host's tempo, or a time in ms. At 120 BPM and 48 kHz, a 1/16 step is exactly 6000 samples. At 93 BPM and 44.1 kHz, a 1/8T step is 294000/31 samples, which is not a whole number. [tempo-sync.md](tempo-sync.md) measured what adding a rounded length does over three hours (the steps drift by whole samples) and decided that step k starts at the first sample at or after t₀ + k·L, computed exactly (⌈k · num / den⌉ in integers, or a 64-bit phase). The same rule keeps the output independent of the host's block size: a step's sample depends on the sequence's start and k, never on where a block begins. A step that lands inside a block splits the block there. The fraction of a sample left over (`since`) can go to the oscillators' `noteOn(since)` ([oscillator-range.md](oscillator-range.md)) for an onset finer than a sample. A tempo change during a sequence keeps the phase within the step and changes the rate from then on, as an LFO's does.

**Where the sequence starts.** Either at the first key press (the sequence's own grid), or at the next point of the host's grid (bar-locked, which needs the song position). They sound the same only when the keys are played on the grid.

**Gate** is the note's length as a fraction of the step. At 100% the note-off and the next note-on fall on the same sample, and their order matters. With the off first, the new note retriggers (in a mono voice, a new attack). With the on first, the notes overlap, which a mono-legato voice plays as a legato change (a glide, no new attack). Most arpeggiators send the off first and leave legato to an explicit **tie**. A tied step sends no note-off at its gate. Its note runs into the next step's note, which then overlaps it by design. In a mono-legato voice that is the 303's slide. Gate 0 needs a floor, either a minimal note or no note.

**Swing** delays every second step, the off-beat of each pair of steps, by a fraction of a step. It is usually given as the pair's ratio: 50% is straight, 66.7% is a triplet feel, and 75% is a dotted feel. The gate is then a fraction of each step's own (swung) length, or of the straight length. The first keeps the notes' duty, and the second keeps their lengths equal. Both are exact under the same integer rule, because the swung onset is t₀ + (k + s)·L for odd k.

### Velocity

The arpeggiator's notes need a velocity. The candidates are:

- each key's own velocity ("key");
- the last key's velocity for every note ("hold");
- the step's velocity ("step");
- a mix of a key's velocity and the step's.

The mix can be a product (the step scales the key, so 127 is the key unchanged), a mean, or a minimum. A step velocity of 0 (or a "rest" value) plays no note but still takes its step's time.

### A step sequencer as a modulation source

A step sequencer's velocity row is also a signal: a value per step, held between steps. Routed to a gain, it is a **gater** (a trance gate). Such a signal can be shaped in three ways.

- **Steps or a gate.** The value can hold for the whole step, or only for the step's gate and then drop to 0. Only the second rhythmically gates a sustained pad on its own. With a tie, the value runs into the next step without the gap.
- **Edges.** A hard step in a gain clicks: an instant gain change multiplies the audio by a step function. A short ramp (1–5 ms) removes the click and softens the gate. M1.5 decided that each step's value arrives as an event at its sample, so the value is constant between events and its bound from its two ends is exact ([modulation-sources.md](modulation-sources.md#the-decision)). A linear ramp keeps that bound exact.
- **What clocks it.** It can be clocked by the arpeggiator's clock, even with the arpeggiator off (a gater with no arpeggio), or by the transport. And it may run only while a key is held, or always.

## Sylenth1: what is known

The full source table with ratings is in the inventory: [§9, The arpeggiator: what is known](../reference/sylenth1-features.md#the-arpeggiator-what-is-known) (AR1–AR20), with S1 and M23 annotated. The manual gives the arpeggiator a page and a half (pp. 21–22). The control list, the mode list and the ranges come from the third-party template for v2.x preset files that M1.5 used ([modulation.md](modulation.md#sylenth1-what-is-known)). We take only names and the ranges a user sees from it. Our IDs and preset format are our own (D7), and they don't follow its numbering. The rest is the developer on KVR (2008, 2009, 2017, 2018), the changelogs, tutorials, and the project owner's screenshots of the demo's popups and panel (2026-11-23), which are kept outside the repo and recorded as text in the inventory. In short:

- **The 10 modes.** The manual names "Up", "Down", "the Up/Down modes", "Random", "Ordered", "Step Seq" and "Step Chord" (p. 21) and counts "10 different melodic modes" (p. 5). The template lists all 10: Up, Down, Up_Down, Up_Down2, Down_Up, Down_Up2, Random, Ordered, Step, Chord. Their host values are evenly spaced at k/9 in exactly that order (one entry, Chord, is written with its 16-bit halves swapped, a slip in the template). That order is probably the menu's (AR1). **The demo's Mode popup confirms it** (AR1, settled 2026-11-23): one column with nothing greyed out, labelled Up, Down, Up/Down, Up/Down2, Down/Up, Down/Up2, Random, Ordered, Step, Chord. The labels are the template's, not the manual's "Step Seq" and "Step Chord". That host value k/9 selects the k-th entry is left for the demo to measure. Vintage Synth Explorer gives the same list without Random (9 names). Nothing says how Up/Down differs from Up/Down 2 (AR2). The developer in 2018, on a user who couldn't hear the difference: "If you wrap at 4 and hold 4 notes, it'll wrap before it can even reach the down part of the sequence, so those will all sound as Up."
- **The controls** (manual p. 21, the template's ranges):
  - Mode;
  - **Time**, "the duration between two successive notes": synced, 1/1 to 1/64 (template); with Sync off, ms (users; S4, §8);
  - **Gate**, "halfway… half of the time between two successive notes": 0–100, shown as % in tutorials (30%, 18%). The demo's LCD reads "50.00 %" with the knob at its centre;
  - **Wrap**, "When Wrap is set to 8… restarted every 8 notes": 0–16. It is a stepped field in the demo, and its bottom value reads "-", not "0", which supports Wrap 0 as off (AR4);
  - **Octave**, "over how many octaves": 1–4, a stepped field;
  - **Velocity**, the mode: Key, Hold, Step, Step+Key, Step+Hold, at k/4 in that order. **The demo's popup confirms the order** (AR13's menu, settled 2026-11-23), labelled "Step + Key" and "Step + Hold";
  - an **on/off** checkbox beside the panel's name ("ARPEG"), as every effect has;
  - **Swing**, added in v3.050 (2019) after users asked from 2008 on. Nothing written says its range or what it moves (AR15). The demo's knob is marked **0, 100 (top) and 200**, while Time's and Gate's are marked 0–5–10. Both presets shown sit at 0. So the range is probably 0–200, with 0 straight. The LCD reading and the law are still open.
- **The step sequencer: 16 steps** (the template; Vintage Synth Explorer's "16 Note Internal Step Sequencer"). Each step has:
  - a **Transpose**, −24 to +24 semitones (the template, and a user in 2009), "used only when the arpeggiator Mode is set to Step Seq or Step Chord";
  - a **Velocity**, 0–127, where "-" means "the note will not be played at all". The template has no separate rest value, so "-" is probably 0 (AR12);
  - a **Hold**, on/off, which "extended" a note's length, "to create portamento sliding effects". A tutorial turns on Mono Legato and Portamento to hear its slides.

  The developer: "The arp does not need the step values (it will only use the step sequencer if the Arp Mode is set to step and/or the Arp Velocity is set to Step (+ Key/Hold)." Pages: the inventory says so, but the manual's text doesn't, and the template has no page field (AR14). **The demo's panel settles it** (AR14, 2026-11-23): the steps are shown **8 to a page** under "PAGE 1" with a ▼, so 16 steps are two pages. A held step shows an arrow, and a transpose is signed. There is no step-length control, so Wrap is the only length. The panel has **no C and P buttons**, against the manual (p. 20, v2.5): a ▼ menu at its top right holds Copy, Paste, Init and Lock. What the page ▼ offers (AR19) and what each menu entry covers (AR20) are new questions, for the UI and presets, not the DSP.
- **Step modes.** A tutorial: "Playing a single note delivers a repeating sequence. Playing a series of single notes transposes the pattern." "Sylenth's Step Chord mode even allows you to transpose chords." A 2009 user asks for a "C" option in the transpose row "to mix chord-mode with other modes (e.g. Up)", so the modes don't combine. Nothing says which key Step Seq plays when several are held (AR7).
- **The sequence.** The developer, 2009:
  - "Yes, the arp will keep on playing its sequence as long as one or more notes are held. If you delete the lower note then the arp will stop at the end of the upper note and it will restart its sequence at the start of the next note."
  - "The arp plays its own sequence, not the midi sequence. That means any note changes that occur in between its own notes will be discarded and the notes will be aligned to the arp's own sequence."
  - In 2017: it starts "at the moment you press a key", locked to the host's tempo but not to its song position (S7, §8).

  So the clock runs while any key is held, a key that follows silence restarts the sequence at the key press, and key changes take effect at the arpeggiator's next step. Whether the pattern continues by index or by pitch is open (AR8).
- **No dry signal.** Asked for a wet/dry knob, the developer pointed to Step Velocity instead. The held keys don't sound on their own while the arpeggiator is on (AR16).
- **Step Velocity** (M23, §6): "outputs its velocity and hold values as a signal", the manual's gater (Step Vlcty to Mix AB, the Mix sliders at 0), and the developer's "the gate is indeed always on", with or without the arpeggiator. In 2009 he named the factory GAT presets and Volume or Mix as destinations, and in 2008 said "You can use the step velocity and the hold buttons on the step sequencer to design a gating pattern". That only makes sense if the signal drops between steps unless a step is held. So it follows the Gate, and a "-" step is 0, not the previous value that M23's hypothesis assumed (AR18).
- **What changed, by version.** v1.20 (Feb 2007) added the arpeggiator, its step sequencer and Step Velocity. v1.21 "Fixed arpeggiator note limit" (no number given, AR17). v3.040 (2018) "Fixes Arpeggiator host tempo sync problem". v3.050 (2019) "Added Arpeggiator Swing knob". The default CC map (pp. 31–32) has no arpeggiator control.
- **What is open.** Mode's host values (the rest of AR1; the menu is settled); the Up/Down variants (AR2); Octave in each mode (AR3); what Wrap counts (AR4); Random's draw (AR5); Ordered's order (AR6); the step modes' base note (AR7); changes to the held set and the end of a sequence (AR8, AR9); Gate's ends (AR10); a tie (AR11); a rest (AR12); the Step+Key and Step+Hold mixes (AR13; its menu is settled); Swing's LCD reading and law (AR15); the switch (AR16); the voicing, the pedal and the note limit (AR17); Step Velocity's shape (AR18); the page menu (AR19); and the panel menu's Copy, Paste, Init and Lock (AR20). The panel (AR14) is settled. The synced Time values are S1's question, and Sync off is S4's.

### What this means for the build

- **The pattern (task 2)** is a pure function: the held set (fixed capacity, no allocation), Mode, Octave, Wrap and the step number give the notes. The 10 modes are data or small functions behind one interface, so Phase 2 can add modes without touching the clock (CLAUDE.md's registry rule). The Up/Down variants, the octave order, Wrap's count and the rule for a changing set are candidates to measure. Their audible differences are cheap to render. The demo's menus (2026-11-23) add no mode and confirm the order, so the mode registry has the 10 in the menu's order. Wrap is the step modes' only length control, and its bottom reads "-", so Wrap 0 is the candidate "off" (each mode's natural length, 16 steps in the step modes). The pages are a view and don't enter the pattern.
- **The clock (task 3)** reuses M1.2's exact note values (`NoteValue`, ⌈k · num / den⌉). It emits events (step on, gate off) at samples counted from the sequence's start, independent of the host's block size, including under swing and tempo changes. It can be driven by an external clock instead (R1). The template's Time range (1/1 to 1/64) differs from the LFO's (8/1D to 1/256T) and the delay's (1/64 to 1/2), which is new evidence for S1 and M1.9's parameter table. **Swing** takes a value from 0 to 200 (the demo's knob marks), with 0 straight, so each candidate law is a mapping from 0–200 to the off-beat's delay. Candidates: 200 as a 75% pair; 100 as a triplet feel and 200 as an 83% pair; or 100 meaning something else. The LCD readings at 0, 100 and 200 (AR15) narrow them down.
- **The step sequencer and velocity (task 4)** add the 16 steps, the rests and ties, the five velocity modes (the demo's menu, in the template's order), and Step Velocity, which replaces M1.5's stand-in (`modulation::SourceContext::stepVelocity`). It runs with the arpeggiator off, and each change is an event at its sample.
- **Into the voices (task 5)**: the arpeggiator's notes go through `VoiceAllocator` as played notes. A tie becomes a legato overlap, which Mono Legato already turns into a glide with no retrigger ([legato-portamento.md](legato-portamento.md)). Keytrack already follows "the arpeggiator's note" (F18, [keytracking.md](keytracking.md)). The hold pedal's place (on the input keys, on the output notes, or ignored) is a choice to make there. The hand-off to M1.11 covers the steps in two pages of 8, and the panel menu (copy, paste, init, lock) in place of the manual's C and P. The hand-off to M1.10 covers what copy, paste and init carry (AR19, AR20).
- **Arp off, Step Velocity unread:** the engine must render exactly as it does today, to the bit and at no measurable cost.

## References

- *Sylenth1 User Manual* v2.5 (LennarDigital, 2015), pp. 5, 20–22 (the arpeggiator), 31–32 (the default CCs).
- Per Ivar Nerseth, [FXPSylenth1Preset.bt](https://github.com/perivar/AudioVSTToolbox/blob/master/PresetConverter/Assorted/FXPSylenth1Preset.bt), revision 1.3, an 010 Editor template for Sylenth1's v2.x preset files: the arpeggiator's fields, the ARPMODE and ARPVELO lists, the ranges. Names and ranges only (clean-room note in the inventory).
- Lennard (the developer) on KVR:
  - "Chord Arpeggiator issue", [t=251340](https://www.kvraudio.com/forum/viewtopic.php?t=251340), [p=3585430](https://www.kvraudio.com/forum/viewtopic.php?p=3585430) and [p=3586259](https://www.kvraudio.com/forum/viewtopic.php?p=3586259) (15–16 May 2009): the sequence while keys are held, its restart, and alignment to the arp's own steps;
  - "arpeggiator swing?", [t=236039](https://www.kvraudio.com/forum/viewtopic.php?p=3366686) (16 Dec 2008): no swing yet; a user's transpose range, [p=3453679](https://www.kvraudio.com/forum/viewtopic.php?p=3453679) (10 Feb 2009);
  - "Arpeggiator adding v low note in 'wrap' sequence", [t=503304](https://www.kvraudio.com/forum/viewtopic.php?t=503304), [p=7056699](https://www.kvraudio.com/forum/viewtopic.php?p=7056699) (16 Apr 2018): Wrap 4 with 4 notes never reaches the down part;
  - "Where's the gate sequencer?", [t=236247](https://www.kvraudio.com/forum/viewtopic.php?p=3367390) (16 Dec 2008): Step Velocity always on (M23);
  - KVR thread 491811, [p=6884485](https://www.kvraudio.com/forum/viewtopic.php?p=6884485) (30 Sep 2017): tempo but not song position (S7).
- Release notes: [v1.20](https://www.kvraudio.com/news/lennardigital_releases_sylenth1_v1_20_6809) (23 Feb 2007), [v1.21](https://www.kvraudio.com/news/lennardigital_updates_sylenth1_to_v1_21_7065) (10 Apr 2007), and the [official changelog](https://lennardigital.com/sylenth1/whatsnew.php) (v3.040, v3.050).
- Tutorials: Attack Magazine, [Vintage Arpeggio in Sylenth](https://www.attackmagazine.com/technique/synth-secrets/vintage-arpeggio-in-sylenth/) (17 Jun 2015), [303-Style Arpeggiator in Sylenth](https://www.attackmagazine.com/technique/synth-secrets/tb-303-acid-arpeggiator-sylenth/) (12 Mar 2015), and Bruce Aisher, [Complex Arps](https://attackmagazine.com/technique/tutorials/complex-arps-arpeggiator/3) (4 Jun 2014); [Vintage Synth Explorer](https://www.vintagesynth.com/lennar-digital/sylenth1) (reviewed March 2012). Ask.Audio's arpeggiator tips refuse automated fetches (403) and were not read.
- Other arpeggiators' Up/Down 2, for the convention only: Novation's SL MkIII [arpeggiator guide](https://userguides.novationmusic.com/hc/en-gb/articles/25626813121298-Using-the-SL-MkIII-s-Arpeggiators-Arp) and Steinberg's [Padshop arp page](https://steinberg.help/padshop/v2/en/halion/topics/padshop/padshop_arp_page_main_section_r.html).
