# Filter keytracking

Keytracking makes a filter's cutoff follow the note being played. Sylenth1 has one Keytrack knob, 0 to 10, on the shared Filter Control panel. At 10 "the cutoff frequency will change with the pitch of the note played", so a self-oscillating filter "can be played like a piano" (manual p. 18). Keytrack is also one of the misc modulation sources (p. 20). This page covers M1.3 task 5:

- the theory: what keytracking computes, the law and the reference note, which pitch it follows, and what "in tune" can mean for a filter that goes flat when it self-oscillates;
- what is known about Sylenth1's keytracking;
- the candidate models prototyped here, and why these;
- what each measured: the law against the reference, intervals at keytrack 10 from C1 to C7, the cutoff clamp near Nyquist, the master tune, a portamento glide and a bend sweep, the zipper of each feeding rate, a legato step at high resonance, block sizes, subnormals, allocation, and CPU;
- the decision, and how it is built (`keytrackOctaves`, `keytrackSource`, `masterTuneSemitones`), measured and tested as built.

Read [filters.md](filters.md) and [filter-theory.md](filter-theory.md) first. The self-oscillation detune measured there is what decides how far keytrack 10 can play in tune. [levels-and-pitch.md](levels-and-pitch.md) covers notes, cents, and the master tune.

## Theory

### The law

A filter's character is set by where its cutoff sits relative to the note's harmonics. A lowpass at 2 kHz is bright on a 100 Hz note (20 harmonics pass) and dull on a 1 kHz note (two pass). Keytracking moves the cutoff with the note so that the ratio, and with it the timbre, stays the same.

Pitch is exponential in the note number, f(n) = 440 · 2^((n − 69)/12), so tracking that keeps the ratio is linear in octaves:

```text
fc(n) = fc0 · 2^(a · (n − r) / 12)        a = keytrack / 10,  r = the reference note
```

- At **a = 1** the cutoff moves an octave per octave of the keyboard: fc/f is constant, so the timbre is too. A self-oscillating filter then plays intervals exactly ("1:1").
- At **a = 0.5** it moves half an octave per octave. A 0.5 brightens high notes less than 1:1 would.
- At **a = 0** the cutoff is fc0 whatever the note.
- **The reference note r** is the note at which keytrack changes nothing: fc(r) = fc0 at every a. Below it the cutoff falls, above it the cutoff rises. Synths differ: C3, C4, E3, and A4 are common (a KVR thread lists E3 for Hive, C3 for Synth1, E2, G1, F2 for others), and some let the user set it.

In an analog synth this is a CV sum. The filter's cutoff input is exponential (1 V/oct, like the oscillators), the keyboard voltage is linear in the note, and a keytrack pot scales the keyboard voltage into the sum. In a ZDF filter it is the same: an offset in octaves added to log₂ of the cutoff before g = tan(π·fc/fs). `LadderFilter` already has that input, `setCutoffModulation(octaves)`.

The alternative, **linear in Hz** (fc = fc0 + a·f(n)), keeps the ratio only when fc0 = 0. Otherwise fc/f = fc0/f + a falls with the note, so low notes are much brighter than high ones. It is here as a contrast, the way an additive mod source in Hz would behave.

### Which pitch

"The pitch of the note played" can mean several things in a voice:

| Quantity | What it includes | What a self-oscillating filter at keytrack 10 does |
|---|---|---|
| The key (note number) | the MIDI key, or the arpeggiator's note | Jumps at a legato note while the oscillators glide; ignores the bend wheel |
| The voice's pitch | the key, the portamento glide, the pitch bend | Follows the oscillators through glides and bends |
| …and the master tune | the A4 reference | Stays with the oscillators when the instrument is retuned |
| …and drift | the per-voice drift (O18–O25) | Drifts with the oscillators (σ 1.5 cents) |
| …and each oscillator's Octave, Note, Fine | per oscillator | Can't: two oscillators feed one filter |

The oscillators' own offsets are out by construction. Each of the others is a term the voice adds or leaves out, so the model doesn't change when the evidence does.

### What "in tune" can mean

A ladder that saturates in every stage goes flat when it self-oscillates, by an amount set by k and not by the cutoff (filter-theory.md: −5.3 cents at k 4.05, −101 at 6, −126 at 8 with tanh; filters.md). So at keytrack 10:

- **Intervals** hold, to within the flatness of that detune across the keyboard. A C5 is an octave above a C4 whatever k is.
- **Absolute pitch** is off by the detune, and by however far the cutoff knob is from the reference note's frequency. A player tunes it with the cutoff knob, the way an analog synth's self-oscillating filter is tuned.

A correction c(k), a table of the detune against k applied as a cutoff scale, would make the filter play the note's own pitch when the cutoff knob is set to it. filters.md's decision kept that out of the core (F7) and noted that it would be easy to add. This round measures whether keytracking is a reason to add it.

### Feeding a control-rate value into the filter's grid

`LadderFilter` turns its cutoff into g once every 32 samples (a grid point) and interpolates g linearly in between. A modulation set before grid point j is read there as the target for the **end** of the coming segment, so g reaches it at grid point j + 1 (filters.md, "How it is applied here"). That has three consequences for keytracking:

- **Fed once per host block**, the cutoff is a staircase with one step per block, and the output depends on the block size.
- **Fed at every grid point with the pitch there**, g follows the pitch one segment (32 samples) late. A glide of S semitones over T seconds then lags by S · (32/fs) / T semitones: 8 cents for an octave in 100 ms at 48 kHz.
- **Fed at every grid point with the pitch at the segment's end** ("aligned"), g arrives at each grid point with the pitch it should have there. A voice can do this because it knows its own pitch over the block: the glide is deterministic, and bend events arrive with the block.

Only the two grid-point feeds are independent of the block size. The voice has to stop at the filter's grid points anyway, which is what the LFO's 32-sample grid and drift's 64-sample hold already ask of it.

## Sylenth1: what is known

The full source table, with confidence ratings and the open questions F13 and F17–F22, is in the inventory ([§3](../reference/sylenth1-features.md#3-filters-pp-5-1618)). In short:

- **Official.** The manual, p. 18: "It is possible to link the cutoff frequencies of the filters to the pitch of the note played on the keyboard… When this knob is set to 0, the cutoff frequency will not depend on the note played. However, when this knob is set to 10, the cutoff frequency will change with the pitch of the note played." "If the filter is put into self-oscillation, setting the Keytrack knob to 10 makes it possible to play the filter like a piano." It is on the Filter Control panel, which "controls the filters A and B". P. 20 lists keytrack among the misc sources. P. 31: Filter Ctrl Keytrack is CC 75. No changelog line from v1.1 to v3.075 mentions keytracking.
- **The developer (KVR).**
  - 2016: "Keytrack ranges from 0 at A-1 to +10 at A9", "so +5 at A4", and "all modulation in Sylenth1 is additive". With Volume A at 5 and a keytrack amount of −10, the volume is "−5 at A-1, 0 at A4 and +5 at A9". Also: "The hinge point is central note A4."
  - Earlier in 2016, hedged: "If keytrack is set to 10, then A0 = 0, A3 = 5 and A7 = 10 (if I'm not mistaken)." And on playing the filter: "Just set the keytrack to 10 and you can play the cutoff with the keyboard. **You'll have to tune the tone via the cutoff yourself.**"
- **Users (KVR).** Chris-S (2017): "Base note on Sylenth seems to be A-1 (midi note #9) which is unusual"; at keytrack 10 "the resonance tone follows the keyboard one-to-one". fluffy_little_something: "on S1 it is very far down the keyboard". Two tutorials: tune the self-oscillating filter against a tuner or a spectrogram. chk071: at least one oscillator must be on for the filter to self-oscillate, though it can be muted.
- **Unknown.** Whether the keytrack follows the glide, the bend, the master tune, or drift; whether the filter's Keytrack knob uses exactly the misc source's quantity; and what happens below A-1.

So the source is **linear in notes, one unit per octave, zero at A-1 (MIDI 9)**, and keytrack 10 is **in tune only in its intervals**. The reference note is the open point. Read literally, "hinge point A4" says A4. But it came in the same breath as the range, as the answer to a question about a Mix diagram, and A4 is where the source is +5, the midpoint. Two users who measured the filter knob found the unaffected note "very far down", A-1. So the working hypothesis (F17) is that the Keytrack knob adds Keytrack/10 × the source, with its zero at A-1. The 2016 "A0 = 0, A3 = 5, A7 = 10" fits no linear law (5 units over 3 octaves, then 5 over 4) and contradicts the twice-stated range, so it is rated low.

One consequence is for later rounds. If the keytrack adds to the cutoff in knob units, as the developer's "additive" suggests, then 1:1 at keytrack 10 needs the cutoff knob to span exactly one octave per unit (10 octaves). F3's placeholder spans 11 octaves (10 Hz × 2^(11·x)), so the two would give 1.1:1, 120 cents per octave. The DSP works in octaves, so this is a knob-law question for task 6 and M1.9 (F12, F21).

## The candidates

Five models, all in `tests/dsp/filters/keytrack_candidates.hpp` at commit `3948d99`. Each turns a `VoicePitch` (key, glide, bend, master tune) and the amount a into an offset in octaves for `setCutoffModulation`, except (e), which goes through `setCutoff`. The reference note and the amount are inputs, so F17 changes a value, not a model.

| Model | Offset in octaves | Why it is here |
|---|---|---|
| **(a) Note number** | a · (key − r) / 12 | The literal reading of "the note played on the keyboard". |
| **(b) Sounding pitch** | a · (key + glide + bend + 12·log₂(tune/440) − r) / 12 | What the oscillators play, before their own offsets: a self-oscillating filter stays with them. |
| **(c) Linear in Hz** | log₂(1 + a · f(note) / fc0) | The contrast: tracking in Hz, not octaves. |
| **(d) Sounding pitch + c(k)** | (b) − detune(k) / 1200 | Keytrack 10 playing the note's own pitch. detune(k) is a 15-point table measured at 48 kHz and 880 Hz, per saturator, interpolated linearly. |
| **(e) Note number, steps ramped** | (a), sent to `setCutoff()` so each step ramps in log₂ over 10 ms | A smoother legato step without portamento (v1.11: "Eliminated clicking effects caused by the filters"). |

The second axis is **feeding**: once per host block of 64, 128, or 512 samples; at each 32-sample grid point with the pitch there ("its start"); or at each grid point with the pitch at the segment's end ("aligned").

Drift is left out of (b). The inventory's hypothesis (O25) is that drift moves the oscillators only. It is σ 1.5 cents, and adding it would be one more term.

## What they measured

Measured at commit `3948d99` by the hidden `[keytrack-report]` and the `[keytrack]` benchmarks, on the test-only prototypes and `LadderFilter` as built. The report and the benchmarks now measure the built keytracking ([Results, as built](#results-as-built)). To rerun the candidates, build that commit:

```sh
git checkout 3948d99
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[keytrack-report]"   # about 25 s
tools/bench.sh "[keytrack]"
```

Self-oscillation is started by one impulse of 0.5. Each render runs until the oscillation has grown and settled: about ten of the linear loop's e-folding times, or at least 2.5 s. Near the threshold that takes a while: 51/f seconds per e-fold at k 4.05, so 16 s at C1. A first run with 4 s caught low notes still growing in the linear mode, 5.4 cents *sharp*: (1 + s)⁴ = −k puts the growing pole at k^¼·cos 45° = 1.0031 times the cutoff. The pitch is `measureFrequency` over the last 0.5 s, or the spectral peak near Nyquist.

### The law and the reference

(a), 48 kHz, k 4.4, Warm on. The self-oscillation pitch in Hz. The reference is A-1 with the cutoff at 13.75 Hz (A-1's frequency) or A4 with the cutoff at 440 Hz, so in both cases keytrack 10 puts the cutoff at the note's frequency:

| Reference | Keytrack | A-1 | C1 | C3 | A4 | C5 | C7 |
|---|---|---|---|---|---|---|---|
| A-1 | 0 | 13.47 | 13.47 | 13.47 | 13.47 | 13.47 | 13.47 |
| A-1 | 5 | 13.47 | 20.77 | 41.54 | 76.19 | 83.09 | 166.2 |
| A-1 | 10 | 13.47 | 32.03 | 128.1 | 431.0 | 512.6 | 2050.7 |
| A4 | 0 | 431.0 | 431.0 | 431.0 | 431.0 | 431.0 | 431.0 |
| A4 | 5 | 76.19 | 117.5 | 235.0 | 431.0 | 470.0 | 940.1 |
| A4 | 10 | 13.47 | 32.03 | 128.1 | 431.0 | 512.6 | 2050.7 |

Every cell is the law exactly, 35.8 cents flat (the k 4.4 detune), within 0.5 cents from 13.5 Hz to 2 kHz. The reference note is the column that doesn't change with keytrack. At keytrack 10 the two references give the same filter, because the cutoff knob was set to the reference's frequency in each. At keytrack 5 they differ by half the distance to the reference: with A-1 as the reference, A4 at keytrack 5 sits 2.5 octaves above 13.75 Hz. **So the reference decides where the cutoff knob has to be.** With A-1, a patch at keytrack 10 needs the cutoff knob near 13.75 Hz to play in tune, which only works if the knob's range reaches that low (F3: 10 Hz). With A4 it needs 440 Hz.

### Intervals at keytrack 10, C1 to C7

The self-oscillation pitch in cents from each note's 12-TET frequency, the cutoff at the note (reference A4, cutoff 440 Hz). (a) and (b) are the same filter on a held note with no bend, glide, or master tune. Spread is the largest minus the smallest value, which is the worst error in any interval from C1 to C7:

| Model | Saturator | Rate | k | C1 | C2 | C3 | C4 | C5 | C6 | C7 | Spread |
|---|---|---|---|---|---|---|---|---|---|---|---|
| (a), (b) | tanh | 44.1 | 4.05 | −5.3 | −5.3 | −5.3 | −5.3 | −5.3 | −5.3 | −5.2 | 0.08 |
| | | 44.1 | 6 | −101.2 | −101.2 | −101.2 | −101.2 | −101.1 | −100.9 | −99.9 | 1.33 |
| | | 44.1 | 8 | −125.7 | −125.7 | −125.7 | −125.7 | −125.6 | −125.3 | −124.0 | 1.69 |
| | | 192 | 4.05 | −5.3 | −5.3 | −5.3 | −5.3 | −5.3 | −5.3 | −5.3 | 0.00 |
| | | 192 | 6 | −101.2 | −101.2 | −101.2 | −101.2 | −101.2 | −101.2 | −101.1 | 0.07 |
| | | 192 | 8 | −125.7 | −125.7 | −125.7 | −125.7 | −125.7 | −125.7 | −125.6 | 0.09 |
| (a), (b) | cubic | 44.1 | 4.05 | −5.2 | −5.2 | −5.2 | −5.2 | −5.2 | −5.2 | −5.1 | 0.08 |
| | | 44.1 | 6 | −64.3 | −64.3 | −64.3 | −64.3 | −64.2 | −64.0 | −63.1 | 1.20 |
| | | 44.1 | 8 | −70.2 | −70.2 | −70.2 | −70.2 | −70.1 | −69.7 | −68.4 | 1.75 |
| | | 192 | 6 | −64.3 | −64.3 | −64.3 | −64.3 | −64.3 | −64.3 | −64.2 | 0.07 |
| | | 192 | 8 | −70.2 | −70.2 | −70.2 | −70.2 | −70.2 | −70.2 | −70.1 | 0.10 |
| (d) | tanh | 44.1 | 4.05 | −0.0 | −0.0 | −0.0 | −0.0 | −0.0 | 0.0 | 0.1 | 0.08 |
| | | 44.1 | 6 | −0.2 | −0.2 | −0.2 | −0.2 | −0.1 | 0.2 | 1.3 | 1.48 |
| | | 44.1 | 8 | −0.3 | −0.3 | −0.3 | −0.2 | −0.1 | 0.2 | 1.7 | 1.94 |
| | | 192 | 6 | −0.2 | −0.2 | −0.2 | −0.2 | −0.2 | −0.2 | −0.1 | 0.08 |
| | | 192 | 8 | −0.3 | −0.3 | −0.3 | −0.3 | −0.3 | −0.2 | −0.2 | 0.11 |
| (d) | cubic | 44.1 | 6 / 8 | −0.2 / −0.3 | | | | | | 1.1 / 1.6 | 1.28 / 1.89 |
| | | 192 | 6 / 8 | −0.2 / −0.3 | | | | | | −0.1 / −0.2 | 0.07 / 0.11 |
| (c), keytrack 10, on 10 Hz | tanh | 44.1 | 6 | +360.7 | +145.1 | +26.3 | −36.3 | −68.4 | −84.4 | −91.6 | 452.3 |
| (c), keytrack 5, on 10 Hz | tanh | 44.1 | 6 | −475.1 | −839.3 | −1054.9 | −1173.7 | −1236.3 | −1268.4 | −1284.4 | 809.3 |

(d)'s table, the self-oscillation detune in cents at 48 kHz and 880 Hz (k 4.0 set to 0):

| k | 4.05 | 4.1 | 4.2 | 4.3 | 4.4 | 4.6 | 4.8 | 5 | 5.5 | 6 | 6.5 | 7 | 7.5 | 8 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| tanh | −5.3 | −10.3 | −19.6 | −28.0 | −35.7 | −49.1 | −60.4 | −70.0 | −88.3 | −101.0 | −110.1 | −116.7 | −121.7 | −125.5 |
| cubic | −5.2 | −10.0 | −18.6 | −26.0 | −32.3 | −42.3 | −49.4 | −54.5 | −61.0 | −64.1 | −66.2 | −67.8 | −69.0 | −69.9 |

- **Keytrack 10 keeps every interval from C1 to C7 to within 0.08–1.75 cents at 44.1 kHz, and 0.10 cents at 192 kHz**, Warm on or off, at the threshold or at k 8. The spread at 44.1 kHz is all at the top (C7, 2 kHz): there the cramping starts to reduce the detune, as filters.md measured. That is well under the 5 cents a listener can resolve.
- **(d) makes the filter play the note's own pitch** (within 0.3 cents below C6) but doesn't improve the intervals: 1.48 and 1.94 cents at 44.1 kHz against (b)'s 1.33 and 1.69. Its table is measured at 880 Hz, and near Nyquist the real detune shrinks.
- **(c) breaks intervals by 4.5 to 8 semitones** across six octaves. On a 10 Hz cutoff, linear-in-Hz tracking at 10 puts C1's cutoff 4.6 semitones above the note and C7's within 0.1 (the detune aside); at keytrack 5 the top notes sit nearly an octave below the note. It is out.

### Near the clamp

Keytrack can push the cutoff past the filter's clamp at 0.495 fs: a high note with the cutoff knob above the note, or a misc source routed to Cutoff on top. 44.1 kHz, k 6, Warm on, keytrack 10, the cutoff two octaves above the note (the header gives 4 × the note's frequency). Cents from that, and the measured pitch (the spectral peak):

| Model | C5 (2,093 Hz) | C6 (4,186) | C7 (8,372) | C8 (16,744) | C9 (33,488) | G9 (50,175) |
|---|---|---|---|---|---|---|
| (b) | −99.9 (1,976 Hz) | −96.6 (3,959) | −91.8 (7,940) | −28.1 (16,475) | **locked, peak 239** | **locked, peak 239** |
| (d) | +1.3 (2,095) | +4.8 (4,198) | +11.5 (8,428) | +79.7 (17,533) | **locked, peak 239** | **locked, peak 239** |

Up to 0.38 fs the cramping only shrinks the detune (−101 → −28 cents), so (d)'s fixed table overcorrects by up to 80 cents. Above about 0.45 fs, **the filter as then built failed**. It neither oscillates nor decays: the output locks at a large level (239 from an impulse of 0.5). The next table shows where. **Fixed in task 7** ([filter-stability.md](filter-stability.md#as-built)): with the nested fallback, (b) at C9 and G9 self-oscillates at the clamp, 21,819 Hz (0.9 cents under 0.495 fs, as the double-precision reference does there), and the keytrack report's scan finds no lock-up anywhere up to the clamp.

### Where the solver fails near Nyquist (a finding for task 7)

The lowest cutoff, as a fraction of fs, at which an impulse of 0.5 leaves `LadderFilter`'s output stuck above 2, scanned from 0.30 to 0.495 fs in steps of 0.0025 (− : bounded up to the clamp):

| Saturator | Rate | k 3.5 | 4.05 | 4.4 | 5 | 6 | 8 |
|---|---|---|---|---|---|---|---|
| tanh | 44.1 / 96 kHz | − | − | 0.4800 | 0.4625 | 0.4500 | 0.4400 |
| cubic | 44.1 / 96 kHz | − | 0.4925 | 0.4575 | 0.4325 | 0.4225 | 0.4175 |

It depends only on fc/fs and k, not on the rate, and it starts lower with the cubic. A diagnostic build found the cause: **Newton–Raphson stops converging**. Its steps cycle at 30–65 in y and hit the 8-iteration cap on every sample, and with 64 iterations it fails the same way. At these cutoffs g = tan(π·fc/fs) is 4 to 64, and an undamped Newton step on a bounded saturator overshoots into a cycle. This is not a keytracking problem. The cutoff knob alone reaches it: at 44.1 kHz, F3's placeholder law tops out at 20.5 kHz (0.465 fs), and Resonance 10 is k 6. The filter round's stress test swept through the top too fast to stay there. It belongs to M1.3's stability task (task 7), which needs a convergent solve there, for example a damped or step-limited Newton, a better first guess, or a bracketing fallback, and a test that holds the cutoff near the clamp at high k.

**Fixed in task 7 (2026-11-15).** Newton as before, and where it has not converged after its 8 iterations, a nested bracketing solve that cannot fail ([filter-stability.md](filter-stability.md#as-built)). The scan above now finds no lock-up at any k, either saturator, 44.1 or 96 kHz, and the filter matches the double-precision reference's level and pitch with the cutoff held at 0.44–0.495 fs.

### The master tune

48 kHz, k 4.4, Warm on, keytrack 10, reference A4, cutoff 440 Hz. The self-oscillation pitch minus the oscillators' pitch (the note at the master tune), in cents:

| Model | Master tune | A2 | A4 | A6 |
|---|---|---|---|---|
| (a) | 440 Hz | −35.8 | −35.8 | −35.5 |
| (a) | 432 Hz | −4.0 | −4.0 | −3.7 |
| (a) | 444 Hz | −51.4 | −51.4 | −51.1 |
| (b) | 432 or 444 Hz | −35.8 | −35.8 | −35.5 |

(a) ignores the master tune, so retuning to 432 Hz moves the oscillators 31.8 cents away from the filter. (b) moves the filter with them.

### A portamento glide and a bend sweep

48 kHz, k 4.4, Warm on, keytrack 10. Glide: a legato C3 → C4 with a 100 ms portamento, linear in semitones. Bend: A3 held, the wheel ±2 semitones at 2 Hz for 1 s. The pitch is read period by period (zero crossings) and compared with the oscillators' pitch over the same period, after taking out the steady detune. Largest / RMS, cents:

| Model | Fed | Glide | Bend |
|---|---|---|---|
| (a) | per grid point (aligned) | 1144.7 / 553.7 | 200.0 / 141.6 |
| (e) | per grid point (aligned) | 1060.6 / 517.0 | 200.0 / 141.6 |
| (b) | per block of 512 | 90.0 / 51.7 | 21.2 / 10.4 |
| (b) | per block of 128 | 21.4 / 15.0 | 4.4 / 2.9 |
| (b) | per block of 64 | 12.2 / 9.0 | 2.5 / 1.7 |
| (b) | per grid point (its start) | 7.9 / 6.0 | 1.7 / 1.2 |
| (b) | **per grid point (aligned)** | **3.2 / 0.6** | **0.1 / 0.0** |

**Zipper.** The same glide and bend through LP24 at k 3.6 on a 110 Hz saw at −6 dBFS, keys an octave up so the cutoff sits above the saw's fundamental. The error's power against the aligned feed, dB. The aligned feed is itself within −77 to −86 dB of a cutoff set every sample (filters.md, the staircase table):

| Model | Fed | Glide | Bend |
|---|---|---|---|
| (b) | per block of 512 | −8.4 | −22.1 |
| (b) | per block of 128 | −18.5 | −32.8 |
| (b) | per block of 64 | −23.0 | −37.3 |
| (b) | per grid point (its start) | −26.5 | −40.8 |

- **(a) and (e) don't follow a glide or a bend at all.** At a legato note the self-oscillating filter jumps a whole octave while the oscillators glide (1,145 cents apart), and it ignores the wheel (200 cents). Through a non-oscillating filter they are a different sound, not a zipper: −1 and −3 dB from (b).
- **Per block, the cutoff is a staircase.** At 512 samples it is 90 cents behind a glide and audibly stepped (−8 dB); even at 64 it is −23 dB.
- **At the grid's start the lag is one segment**, exactly as the theory predicts: 7.9 cents on the glide (8.0 predicted), 1.7 on the bend (1.7 predicted: 25 semitones per second × 0.67 ms).
- **Aligned, the filter follows the oscillators to 3.2 cents at worst and 0.6 RMS** through an octave glide in 100 ms (what is left is the oscillation's own response to a moving cutoff), and to 0.1 cents through the bend.

### A legato note step at high resonance

48 kHz, LP24, k 3.6, a 150 Hz sine at −12 dBFS, keytrack 10, the cutoff at the note. The key steps up from A5 (880 Hz) at a grid point and host-block boundary, by a fifth or an octave. The input is held, so only the filter's step is measured. The measure is filters.md's: energy above 1 kHz in a Hann window of 4096 centred 32 samples after the step, dB re an instant unit step:

| Model | How | A5 → E6 | A5 → A6 |
|---|---|---|---|
| (a), (b) with no portamento | a step, one 32-sample ramp of g | −49.0 | −47.8 |
| (e) | ramped over 10 ms through `setCutoff()` | −77.5 | −71.1 |
| (b) | a 30 ms portamento | −97.9 | −87.7 |
| (b) | a 100 ms portamento | −110.3 | −106.8 |
| | steady: the cutoff at the new note throughout | −115.1 | −115.2 |

A legato step without portamento rings the resonance at −48 dB re a unit step: 18 dB quieter than filters.md's three-octave modulation step (−29.5 dB), and 23–29 dB louder than a 10 ms ramp. In a patch the oscillators step at the same moment, a far bigger change, so the filter's ring is under their step. Ramping the filter alone would make a self-oscillating filter slur for 10 ms behind oscillators that jump. With portamento, (b) glides and the step is gone (−88 to −110 dB).

### Block sizes, subnormals, allocation

48 kHz, k 3.9, drive 2, a 110 Hz saw then silence, a legato key every 4,000 samples with a 60 ms portamento, the wheel moving, master tune 438 Hz, keytrack 10. Blocks of 1 and 37 against 4096, bit for bit:

| Model | Fed | 1 | 37 | Subnormal outputs | Allocations |
|---|---|---|---|---|---|
| (a), (c), (d), (e) | per grid point (aligned) | identical | identical | 0 | 0 |
| (b) | per grid point (its start, aligned) | identical | identical | 0 | 0 |
| (b) | per host block | **differs** | **differs** | 0 | 0 |

Every model is a pure function of the voice's pitch at a grid point, so feeding at grid points is independent of the block size. Feeding per host block is not, by construction.

### CPU

`tools/bench.sh "[keytrack]"`, `relwithdebinfo`, Apple M1 Ultra. Each model evaluated at the 16 grid points of a 512-sample block, as a voice would feed it, and the filter with that feed:

| Benchmark | ns per block | ns per sample |
|---|---:|---:|
| (a) note number, 16 evaluations | 6 | 0.01 |
| (b) sounding pitch, 16 evaluations (with a `log2` for the master tune) | 66 | 0.13 |
| (c) linear in Hz, 16 evaluations | 106 | 0.21 |
| (d) sounding pitch + c(k), 16 evaluations | 118 | 0.23 |
| `LadderFilter`, Warm on, k 2, modulation once per block | 46,633 | 91.1 |
| `LadderFilter`, Warm on, k 2, modulation at each grid point | 47,211 | 92.2 |

Keytracking costs nothing that matters: at most 0.23 ns per sample per voice to evaluate, and feeding at every grid point is within the filter's run-to-run noise (+1.2%, while the filter alone measures 91–97 ns across runs). (b)'s `log2` of the master tune is the same for every voice, so a build would compute it once per block.

## The decision

*Decided by the project owner on 2026-11-15: every recommendation below, as written (PLAN.md, Decisions log). The build follows it, with the details in [How it is applied here](#how-it-is-applied-here).*

| Question | Decision | Why |
|---|---|---|
| **The law** | **Linear in octaves**: offset = a · (pitch − r) / 12 octaves, a = Keytrack / 10, added to the cutoff's log₂ before g (the existing modulation input). | The developer's source is linear in notes, one unit per octave; keytrack 10 is 1:1 (manual, Chris-S); intervals then hold to 0.08–1.75 cents at 44.1 kHz and 0.10 at 192 kHz. Linear in Hz (c) breaks intervals by up to 8 semitones. |
| **The reference note** | **A-1 (MIDI 9)**, as one named constant, F17 to confirm. A4 is the alternative. | The source is 0 at A-1 ("all modulation is additive"), and two users measured the filter's unaffected note "very far down", one at A-1. "Hinge point A4" fits the source's midpoint (+5), said about a Mix example. The cost of A-1: at keytrack 10 the cutoff knob must sit near 13.75 Hz to play in tune, and a patch at keytrack 5 is 2.5 octaves brighter at A4 than its knob reads. If the demo shows A4, it is the constant that changes. |
| **Which pitch** | **(b) the voice's sounding pitch: key (or the arpeggiator's note) + portamento glide + pitch bend, with the reference moved by the master tune.** Not drift, not the oscillators' own offsets. The parity-strict alternative is key + glide only. | The manual's purpose is a self-oscillating filter played "like a piano"; (b) stays with the oscillators to 3.2 cents through an octave glide and 0.1 through a bend, and with the master tune, where (a) is 1,145, 200, and 31.8 cents off. No source says Sylenth1 does or doesn't; the manual's Mono Legato slides notes "in pitch and velocity", which suggests the note itself glides. Each term is one addend in the voice, so F18–F19 remove a term rather than change the model. Drift stays out (O25: "oscillator drift"). |
| **Absolute pitch, c(k)** | **No correction.** F7 stands: keytracking doesn't reopen it. | The developer: "You'll have to tune the tone via the cutoff yourself", so Sylenth1 is in tune only in its intervals. (d) doesn't improve intervals (1.48 against 1.33 cents at k 6), overcorrects near Nyquist (+80 cents at 0.38 fs), and its table depends on the saturator and on the oscillation's amplitude, which any input changes. Easy to add later if F21 finds otherwise. |
| **How the voice feeds it** | **At each 32-sample grid point, with the pitch at the segment's end** (aligned), the voice processing in chunks that stop at the filter's grid points. | The only feed that follows a glide to 3 cents and is independent of the block size. Per-block feeding is a staircase (−8 dB zipper at 512 samples) and changes with the host's block size; feeding the pitch at the grid's start lags 32 samples (8 cents on a fast glide). It costs nothing measurable (+1%). |
| **Legato steps without portamento** | **No extra ramp**: the step goes through the modulation input like any modulation (one 32-sample ramp of g). | Its ring (−48 dB re a unit step) sits under the oscillators' own step at the same moment; a 10 ms ramp would make a self-oscillating filter lag oscillators that jump. With portamento the filter glides (−88 to −110 dB). If F22 finds Sylenth1 smoothing it, (e) is measured: −71 to −78 dB. |
| **The misc source (M1.5)** | **The same quantity**: source = (pitch − 9) / 120, 0 at A-1, 0.5 at A4, 1 at A9 (MIDI 129), linear and unclamped, from the same pitch as the filter's. | The developer's range, twice, and his example of a source value of 0.5 times an amount of −10 adding −5. One function serves both, so the filter knob and the source can't disagree. Below A-1 the source goes negative; F20 settles whether Sylenth1 clamps it. |
| **Per voice, both filters** | Computed **once per voice** and fed to **both** filters through the Filter Control's Keytrack. A per-filter amount comes only through the misc source to Cutoff A or B (M1.5). | The manual: the Keytrack knob sits on the panel that "controls the filters A and B". |
| **Where it lives** | `libs/dsp`: a header with the pure functions (the offset in octaves from a pitch, the amount, and a reference; the source value; the reference constant), `constexpr`, no state. The voice (M1.4) composes the pitch and feeds the filter; the mod system (M1.5) reads the source. | A pure function is the smallest thing both users can share. It allocates nothing and is trivially real-time safe. The filter is unchanged: keytrack is one more octave offset into `setCutoffModulation`. |
| **Knob law (hypothesis for M1.9)** | **a = Keytrack / 10, linear**; reference A-1. How it combines with the cutoff knobs is task 6 (F12): **if the cutoffs add in knob units, F3's law must span 10 octaves for keytrack 10 to be 1:1** (F3's 11-octave placeholder would give 1.1:1). | The developer's "additive" in knob units, and Chris-S's one-to-one at 10. The DSP takes octaves, so this is a mapping in the parameter table, settled by F21's slope measurement. |
| **The solver failure near Nyquist** | **Fix it in M1.3 task 7 (stability), and do that task before task 6**, test-first, with a test that holds the cutoff near 0.495 fs at k up to 8, Warm on and off. | It is a defect in the built filter, reachable with the cutoff knob alone at 44.1 kHz and made easier to reach by keytrack. Keytrack's own build doesn't depend on it, since it adds octaves to an input that already exists. |

### How task 6 combined it

This round left the Filter Control's combining law (F12) open: keytrack produces one number per voice, an offset in octaves already scaled by its amount, and task 6 decided how it combines with Filter A's or B's cutoff and the Filter Control's. Task 6 decided on 2026-11-21 ([filter-control.md](filter-control.md)): all three add in knob units, keytrack after the two clamped knobs and unclamped, through one law at **1 octave per unit**. There keytrack's knob units (the developer's Keytrack × (pitch − 9)/120) and `keytrackOctaves(pitch, Keytrack / 10)` are the same number, bit for bit, so the voice passes `keytrackOctaves`'s value as the `keytrack` term of `filterCutoffOctaves`, and this round's function is unchanged. If the demo finds another slope (F3), the term scales with it, and keytrack 10 would no longer be 1:1 (F20, F21).

## How it is applied here

### The functions

[keytrack.hpp](../../libs/dsp/include/anoesis/dsp/filters/keytrack.hpp) holds three `constexpr` pieces, with no state:

- `keytrackReferenceNote` = 9 (A-1, 13.75 Hz at A4 = 440 Hz): the note where keytrack changes nothing.
- `keytrackOctaves(pitch, amount, reference = A-1)` = amount · ((pitch − reference) / 12): the cutoff offset in octaves. amount is Keytrack / 10. The reference is a parameter, so F17 changes a default, not the function.
- `keytrackSource(pitch)` = (pitch − 9) / 120: the misc modulation source for M1.5, 0 at A-1, 0.5 at A4, 1 at A9 (MIDI 129), negative below A-1, past 1 above A9. `keytrackOctaves(p, 1)` is 10 × `keytrackSource(p)`, so the knob and the source can't disagree.

[pitch.hpp](../../libs/dsp/include/anoesis/dsp/math/pitch.hpp) gains `masterTuneSemitones(referenceHz)` = 12·log₂(referenceHz / 440): the master tune as a note offset, so `noteToFrequency(n + offset)` is `noteToFrequency(n, referenceHz)`. The voice computes it once per block, not per voice per grid point (it is the one `log2` in keytracking).

A NaN pitch gives a NaN offset, which `LadderFilter::setCutoffModulation` already treats as 0, so a bad pitch silently means no keytrack. Nothing allocates and nothing is stored: `keytrack.hpp` includes no other header, and `masterTuneSemitones` is one `log2`. Both are real-time safe and portable.

### How a voice uses it (M1.4)

```cpp
// Once per block: the master tune as a note offset (pitch.hpp).
const float tune = masterTuneSemitones(masterTuneHz);
// The voice processes in chunks that stop at the filter's 32-sample grid
// points (it already must for the LFOs and drift). At each grid point, the
// pitch at the END of the coming segment: the filter reads a modulation there
// as the value g reaches at the next grid point.
const float pitch = key + glide.at(gridEnd) + bendSemitones(gridEnd) + tune; // not drift, not osc offsets
const float keytrack = keytrackOctaves(pitch, keytrackKnob / 10.0f);
// Task 6's law adds it, in knob units, after each filter's clamped knobs
// (filter-control.md has the whole feed):
filterA.setCutoffModulation(filterCutoffOctaves({/* the knobs and Cutoff A / AB modulation */ .keytrack = keytrack}));
filterB.setCutoffModulation(filterCutoffOctaves({/* the knobs and Cutoff B / AB modulation */ .keytrack = keytrack}));
// The misc source (M1.5): keytrackSource(pitch).
```

`key` is the MIDI key, or the arpeggiator's note when it plays (its octaves and the step sequencer's Transpose are notes). A legato note with no portamento steps the pitch, and the step goes through the modulation input like any other: one 32-sample ramp of g. The tests' stand-in for the voice, [keytrack_support.hpp](../../tests/dsp/filters/keytrack_support.hpp), feeds the filter this way, and the report measures the other feeds against it. `dsp::Voice` composes this pitch since M1.4 task 3 (2026-11-21): its glide plus the pitch bend, plus `masterTuneSemitones()` for the master tune it gives its slots, fed for the segment's end at each grid point; measured on the voice, the filter follows an octave glide to 0.24 cents and a 5 ms bend ramp to 0.14 ([legato-portamento.md](legato-portamento.md#the-filter-following-the-glide-and-the-bend)).

## Results, as built

From the `[keytrack-report]` ([keytrack_report.cpp](../../tests/dsp/filters/keytrack_report.cpp)) and the `[keytrack]` benchmarks ([keytrack_benchmark.cpp](../../tests/dsp/benchmarks/keytrack_benchmark.cpp)), which now measure the built functions fed into `LadderFilter` as the voice will feed them, with the other feeds and the key alone as contrasts:

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[keytrack-report]"   # about 20 s
tools/bench.sh "[keytrack]"
```

Every figure matches the chosen prototype's, model (b) with the aligned feed, to the printed digit. The built function computes the same offset (the prototype took the master tune in Hz and folded it in itself; the built one takes it from `masterTuneSemitones()`, with the same result):

| Measurement | As built |
|---|---|
| The law, keytrack 0 / 5 / 10, A-1 to C7 (48 kHz, k 4.4) | 13.47 Hz everywhere / A4 at 76.19 Hz / A4 at 431.0 Hz: the law, 35.8 cents flat, within 0.5 cents |
| Intervals at keytrack 10, C1–C7, 44.1 kHz, k 4.05 / 6 / 8 | spread 0.08 / 1.33 / 1.69 cents (tanh), 0.08 / 1.20 / 1.75 (cubic) |
| The same at 192 kHz | at most 0.10 cents |
| Near the clamp (44.1 kHz, k 6, the cutoff two octaves above the note) | −99.9 (C5) to −28.1 cents (C8, 0.38 fs); from C9 at the clamp, 21,819 Hz (locked before task 7's fix) |
| The master tune at 432 / 444 Hz | the filter stays with the oscillators: −35.8 cents (the detune) as at 440; without the term −4.0 / −51.4 |
| A 100 ms octave glide / a ±2 semitone bend, largest / RMS | 3.2 / 0.6 cents; 0.1 / 0.0 cents |
| The key alone, the same glide and bend | 1,144.7 / 553.7; 200.0 / 141.6 cents |
| Zipper against the aligned feed: grid start / per block of 64 / 512 | −26.5 / −23.0 / −8.4 dB (glide) |
| A legato step at k 3.6, a fifth / an octave: none / 30 ms / 100 ms portamento | −49.0 / −97.9 / −110.3; −47.8 / −87.7 / −106.8 dB re a unit step (steady −115) |
| Block sizes 1 / 37 / 4096, aligned or grid-start feed; subnormals; allocations | identical; 0; 0 (fed per host block: differs) |
| The solver failure (tanh, k 4.4 / 6 / 8) | from 0.4800 / 0.4500 / 0.4400 fs, at 44.1 and 96 kHz, before task 7; since its fix, none up to the clamp |

**CPU** (`tools/bench.sh "[keytrack]"`, `relwithdebinfo`, Apple M1 Ultra):

| Benchmark | ns per block of 512 | ns per sample |
|---|---:|---:|
| `keytrackOctaves`, 16 grid points | 5 | 0.01 |
| `keytrackSource`, 16 grid points | 5 | 0.01 |
| `masterTuneSemitones`, once per block | 4 | 0.01 |
| `LadderFilter`, Warm on, k 2, fed once per block | 47,074 | 91.9 |
| `LadderFilter`, Warm on, k 2, fed at each grid point | 46,310 | 90.5 |

The built offset costs 0.3 ns per grid point, against the prototype (b)'s 4 ns, because the master tune's `log2` moved out to once per block. Feeding the filter at every grid point is within noise of feeding it once per block.

## Tests

[keytrack_test.cpp](../../tests/dsp/filters/keytrack_test.cpp), 8 tests, and one in [pitch_test.cpp](../../tests/dsp/math/pitch_test.cpp). Each tolerance is derived in the test:

| Test | What it pins down | Tolerance and why |
|---|---|---|
| An octave per octave at 10, nothing at the reference | 0 at A-1 at every amount, evaluated at compile time; 1 octave at A0, 5 at A4, 10 at A9, −0.75 at note 0; keytrack 5 is half, 0 is none; another reference as a parameter | exact |
| Linear in octaves over and beyond the keyboard | every quarter tone from note −24 to 150 at five amounts against the formula in double | 2⁻²² relative: two roundings (the division, the product) and half an ulp for the reference |
| The source: 0 at A-1, 0.5 at A4, 1 at A9, unclamped, the filter's quantity | the end points; −0.075 at note 0 and 1.1 at note 141 (unclamped); the sweep; keytrack 10 equals 10 × the source | exact at the end points; 2⁻²² (the sweep), twice that for the identity |
| Keytrack 10 plays intervals | a self-oscillating filter, the cutoff knob at 13.75 Hz, notes C2–C6 and A4, Warm on and off: each note at its own frequency, flat by the k 4.4 detune | 0.5 cents per note against the detune pinned in filters.md (0.2 for that pin, 0.3 for the flatness the report measures); the spread under 0.5 cents |
| Keytrack 5 tracks half as many octaves | A4 at keytrack 5: 2.5 octaves above 13.75 Hz | 0.5 cents, as above |
| Follows the voice's pitch | a 100 ms octave glide, a ±2 semitone bend at 2 Hz, and the master tune at 432 and 444 Hz | glide under 5 cents at worst and 1.5 RMS (report: 3.2 / 0.6; the one-segment lag of feeding at the grid point is 8 / 6, which fails); bend under 0.5 (report 0.1; the lag 1.7); the master tune within 0.1 cents of 440's offset (without it: 31.8 and 15.7) |
| Bit-identical across block sizes, no allocation | legato glides, a moving wheel, a master tune of 438 Hz, fed at the grid, in blocks of 1 and 37 against 4096 | bit for bit; 0 allocations |
| A NaN pitch is no modulation | NaN propagates through both functions; a filter fed a NaN pitch is bit for bit the filter with no keytrack | exact |
| `masterTuneSemitones` | 0 at 440, ±12 at 880 and 220, −0.31767 at 432; a note plus the offset is the note at that master tune, for six tunings and five notes | exact at octaves; 1.2·10⁻⁶ relative (noteToFrequency's 10⁻⁶ plus `log2`'s rounding carried through `exp2`) |

Red first: against an inert stub (every function returning 0), 8 of the 9 failed. The ninth, block-size identity, is a property a stub also has. Then 12 deliberate breaks, one at a time, each caught by at least one test: the reference at A4; 1.1:1 tracking (an 11-octave knob added in knob units); the amount ignored; the amount squared; whole notes only (glide and bend lost); the source over 100; the source clamped to [0, 1]; the source centred on A4; the master tune's sign flipped; the master tune with ln instead of log₂; the master tune in cents; a NaN pitch mapped to 0. Every test but the block-size test caught at least one. That test pins a property of the feed (each grid point's value is a pure function of the pitch there), which no fault in the functions can change; it is there to keep the voice's feeding honest when M1.4 builds it.

## References

- LennarDigital, *Sylenth1 User Manual*, v2.5, 2015, pp. 18, 20, 29, 31; [version history](https://lennardigital.com/sylenth1/whatsnew.php), [v1.x news](https://lennardigital.com/home/oldnews1.php), [oldnews2](https://www.lennardigital.com/home/oldnews2.php).
- The developer's and users' posts on KVR are listed with dates and links in the [feature inventory](../reference/sylenth1-features.md#3-filters-pp-5-1618), with confidence ratings.
- R. A. Moog, "Voltage-controlled electronic music modules", *JAES* 13(3), 1965: the exponential (1 V/oct) control law that keytracking in octaves inherits.
- [filter-theory.md](filter-theory.md): the ladder, its self-oscillation threshold, and why a saturating ladder goes flat; [filters.md](filters.md): the detune measured flat across the cutoff, and the 32-sample grid.
