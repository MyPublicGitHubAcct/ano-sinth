# The Filter Control: how the shared and per-filter controls combine

Sylenth1's Filter Control panel has a Cutoff, a Resonance, a Keytrack, and the Warm Drive switch, and it acts on filters A and B together (inventory [§3](../reference/sylenth1-features.md#3-filters-pp-5-1618)). Each filter also has its own Cutoff and Resonance, and the modulation destinations include Cutoff A, B, and AB and Reso A, B, and AB. This page covers M1.3 task 6, the combining law:

- the theory: what adding in knob units, in octaves, and in Hz each mean, why the first two are one family, where a clamp can sit, how resonances can combine, and how a combined value meets the filter's 10 ms cutoff ramp and 32-sample grid;
- what is known about Sylenth1's Filter Control, including evidence the inventory didn't have: the developer's 2008 answer on how the knobs combine, the old LCD's Hz scale, and the knobs' default;
- the candidate models prototyped here, and why these;
- what each measured on `LadderFilter`: what each predicts for the settings that tutorials and users report, the effective cutoff over a grid of filter × Filter Control positions, the reachable range and where the DSP's ceiling takes over, how the knob feels, keytrack from C1 to C7, full modulation with each clamp, resonance, four ways of feeding the sum (a step, a MIDI CC sweep, a clamp during a ramp, block sizes), and CPU;
- the decision (the project owner's, 2026-11-21) and how it is built: the law in `filter_control.hpp`, a closed-form read of the global knob ramps, and a resonance modulation input on `LadderFilter`, measured and tested as built.

Read [filters.md](filters.md) (the filter and its 32-sample grid), [keytracking.md](keytracking.md) (keytrack in octaves and the "Knob law" open question this page answers), and [filter-stability.md](filter-stability.md) (the DSP's ceilings and the parameter-layer clamp, F24) first.

## Theory

### Three ways to add two cutoff knobs

A cutoff knob is a position x in [0, 10]; a law turns it into Hz. Every law in this project is exponential, f(x) = f₀ · 2^(r·x): r octaves per knob unit, from f₀ at 0. That is the 1 V/oct convention, and what keytracking in octaves assumes (keytracking.md).

Two knobs (filter X's Cutoff, x, and the Filter Control's, c) can be combined in three essentially different ways:

| Model | Cutoff | What one knob does to the other's effect |
|---|---|---|
| **Knob units added**, then one law | f₀ · 2^(r·(x + c)) | Moving c by one unit shifts the cutoff by r octaves wherever x is: the two knobs are interchangeable. |
| **Octaves added**: each knob through its own law | f_X(x) · 2^(r_C·(c − c₀)) | The same as above when both laws are exponential: log₂ f = log₂ f₀ + r·x + r_C·(c − c₀). The constant c₀ (the Filter Control's neutral position) only moves f₀. |
| **Hz added** | f(x) + f(c) | The larger term dominates: with x far below c, x does almost nothing (a dead zone), and vice versa. |

So "knob units added through one exponential law" and "each knob through its own exponential law, added in octaves" are **one family**: log₂ f is affine in x + c. What distinguishes members of the family is only two numbers, **r** (octaves per unit) and **the anchor** (f₀, or equivalently where the Filter Control is neutral). Adding in Hz is genuinely different.

Modulation and keytrack fit the same picture. A modulation source adds amount × value knob units to the knob it modulates (the developer: "all modulation in Sylenth1 is additive"); keytrack's source adds Keytrack × (pitch − 9)/120 units (keytracking.md). Under the knob-unit family every addend is r octaves per unit. That gives a constraint from keytracking: **if keytrack adds knob units, keytrack 10 tracks 1:1 only if r = 1 octave per unit** (with F3's r = 1.1 it would be 1320 cents per octave).

### Where a clamp can sit

Each knob has a range, [0, 10]. A control plus its modulation can be held to it (F24's hypothesis) in several places:

- **Nowhere** in knob space: the sum goes straight to the law, and only the DSP's own clamp, [1 Hz, 0.495 fs], applies. Sixteen destination slots at ±10 each can then ask for hundreds of octaves.
- **Per control**: x + its modulation to [0, 10], c + its modulation to [0, 10], then add. The sum lies in [0, 20]. Modulation pushing one knob past its end is lost even if the other knob has room.
- **On the sum**, to [0, 20]: modulation can borrow the other knob's room.
- **On the sum, to [0, 10]**, the literal reading of "the knob's range" applied to the sum. Then with both knobs at their default the sum is already at the top.

Keytrack is a separate question: its source is unclamped (F20), so it can be added before or after the clamp. After the clamp, keytrack can take a fully open filter higher still (into the DSP's ceiling) and a closed one lower.

### Resonance

The resonances can be added (in knob units, or in k: the same thing when the law k(x) is linear, as F5's k = 0.6·x is), multiplied (the Filter Control as a gain on each filter's resonance), or combined by a maximum. What matters for the sound is where self-oscillation starts (k = 4 exactly, filters.md) and how far the combination reaches: k = 12, three times the threshold, if two knobs at 10 add with no clamp on the sum.

### The ramp, the grid, and a clamp

`LadderFilter` has two cutoff inputs (filters.md): `setCutoff(Hz)`, ramped linearly in log₂ over 10 ms and read at each 32-sample grid point, and `setCutoffModulation(octaves)`, unramped and read at the next grid point. With an exponential law, a linear ramp of a knob in knob units is a linear ramp of log₂ of the cutoff, so a knob's ramp and the filter's ramp are the same ramp. Four ways of feeding the combined cutoff follow:

- **W1, the filter's ramp**: the knobs' sum through the law into `setCutoff`, the rest (modulation, keytrack) into the modulation input. But a per-control clamp couples a knob with its modulation: the clamped value clamp(x + m) can't be split into a ramped x and an unramped m. W1 can only clamp against the knob's *target*, so during a ramp it is wrong.
- **W2, the voice's ramp**: the voice ramps the knob positions (10 ms, linear in knob units, advanced at its grid points), computes the clamped sum at each grid point for the end of the coming segment, and feeds the whole thing into the modulation input. `setCutoff` stays at the law's anchor, so the filter's own ramp never moves. This is exactly W1's g sequence when no clamp bites, and exact when one does.
- **W3, both**: the voice's ramp into `setCutoff`, which ramps again: double smoothing.
- **W4, neither**: the knob's raw value into the modulation input at the grid.

## Sylenth1: what is known

The full source table with ratings is in the inventory ([§3](../reference/sylenth1-features.md#3-filters-pp-5-1618), the Filter Control and Cutoff display rows, F12 and F26–F30). In short:

- **Official.** The manual, p. 18: "With the Cutoff knob you can change the cutoff frequency from both filters A and B… In a similar fashion the Resonance knob controls the resonance of both filters." P. 17: the panel "does not contain an individual filter; it merely controls the filters A and B". P. 19: a modulation amount's centre "means there is no modulation". P. 10: "Some knobs and controls have a centered default setting, like the pan knob or the octave and note values", examples rather than a list, so it neither names nor excludes the cutoffs. The changelog: v3.040 (22 May 2018) "Added option in settings file to display Cutoff values in Hz"; nothing on the combining law.
- **The developer (KVR).**
  - 27 Nov 2008, asked whether the Filter Control combines with filters A and B additively, multiplicatively, or on a log scale, for cutoff and resonance: "The filter control knob values are added to the knob values of both filter A and B. The frequency scale is logarithmic though, so the cutoff values are added on a logarithmic scale." "Most modulation paths are additive too by the way." ([t=234164, p=3337069](https://www.kvraudio.com/forum/viewtopic.php?p=3337069))
  - 24 Nov 2008: "the filters are controlled by 2 knobs, so if you want to fully open the filter you may need to adjust both cutoff knobs." ([p=3333740](https://www.kvraudio.com/forum/viewtopic.php?p=3333740))
  - 17–18 Dec 2016: "The global cutoff knob controls both filter A and filter B cutoff. The values are simply added." "adjusting the global cutoff knob will just adjust Cutoff A and B from those positions." "If no modulation is used at all and keytrack is 0, the frequency still depends on both the filter A and fitler Ctrl knobs". "Of course it would be possible to just add those values and calculate the 'real' base frequency, but then the frequency could still be very different at a certain position of Cutoff A (depending on the position of the filter Ctrl knob)." ([p=6650892](https://www.kvraudio.com/forum/viewtopic.php?p=6650892), [p=6651342](https://www.kvraudio.com/forum/viewtopic.php?p=6651342))
  - 1 Mar and 22 May 2018, on the cutoff in Hz: it "depends not only on the Cutoff knob, but also on the global Cutoff knob and any modulation"; "The old version did display a value, but that was pretty useless"; v3.040 adds "an option in the Settings.xml file to display the cutoff frequencies in the same (faulty) Hz scale". ([t=500760, p=7022312](https://www.kvraudio.com/forum/viewtopic.php?p=7022312), [p=7083857](https://www.kvraudio.com/forum/viewtopic.php?p=7083857))
- **Users.**
  - titchbit, 3 Oct 2013 (a version that showed Hz): resetting a filter knob to its default, "the center", "the LCD screen will show that the center value of all 3 filters is 146.16 Hz", and "you can get frequencies well above 1,000 Hz with your filters set at 146.16 Hz". ([dubstepforum t=278794, p=3470848](https://www.dubstepforum.com/forum/viewtopic.php?t=278794))
  - fluffy_little_something, 29 Nov 2018: "The default setting for the global cutoff is 12 o'clock, i.e. value 5.0." "When you turn down the global cutoff slightly to the value of 4.5, the filter also closes completely." (Cutoff A's position unstated.) ([KVR t=509757, p=7238582](https://www.kvraudio.com/forum/viewtopic.php?p=7238582))
  - chrisw63, 2018: a two-year-old video "showed Actual Frequency values for Cutoff and Resonance"; 7up, 2017: v2.2 showed the cutoff in Hz, v3.0.2 "only goes up to 10.000", "at 10.000 the filter is fully open". LordNibbler, 2016: "the existing linear scale" (of the knob's 0–10 display).
  - Tutorials with both cutoffs, from the Hz-display era: Attack Magazine's *Detuned Pad* (2 Apr 2015): Filter A "Cutoff to 7 (around the 800Hz mark), Resonance to 3 and Drive to 7"; Filter Control "Warm Drive, set Cutoff to around 60 Hz and increase Keytrack to 3". Its *Vintage Arpeggio* (17 Jun 2015): Filter A "drive dial to 2.10, resonance to 2.00 and cutoff to 40Hz"; Filter Control "cutoff to 20 Hz and resonance to 1.00", Mod Env 1 on Cutoff A "all the way to the right". MusicRadar (2015): the panel is "used to offset the Cutoff and Resonance parameters of both filters together". ModeAudio (2016): the Filter Control's resonance "will be applied to both Filter A and Filter B".
- **Unknown.** The law's octaves per unit and anchor; whether the old LCD's Hz is each knob's own law, and with what other knob position; the Filter Control Resonance's and Keytrack's defaults; whether a control plus its modulation clamps, and where; whether Cutoff AB modulation is the Filter Control's knob moving.

So: **the knob values add**, the developer twice and explicitly, and the result goes through a logarithmic (exponential) frequency scale. "Just add those values and calculate the 'real' base frequency" says the base cutoff is a function of the sum alone. That rules out adding in Hz and puts Sylenth1 in the knob-unit family, where what's left are r, the anchor, and the clamps. The 2008 answer was to a question about cutoff *and* resonance, and he answered for the "knob values", so resonance adds in knob units too (rated medium-high). All three cutoff knobs default to the centre, 5 (two users). The old LCD showed each knob's own Hz, 146.16 at the centre for all three, which the developer calls a "faulty" scale that "did not represent the actual cutoff frequency": that fits a display of the knob's own term, f₀ · 2^(r·x), while the actual cutoff is f₀ · 2^(r·(x + c)), five octaves higher at the defaults under r = 1. The tutorials' Hz figures are LCD readings of that same scale, so they are positions, not cutoffs.

## The candidates

Four cutoff models and four clamp placements, in [filter_control_candidates.hpp](../../tests/dsp/filters/filter_control_candidates.hpp); every term in knob units, the output in octaves above 1 Hz, so it feeds `LadderFilter` as Hz or as modulation. The knobs read 146.16 Hz at 5 under (a2) and F3's 452.5 Hz under the others; a tutorial's Hz reading is turned into a position by each model's own law.

| Model | Cutoff | Keytrack | Why it is here |
|---|---|---|---|
| **(a1) Knob sum, F3's law** | 10 Hz · 2^(1.1·(x + c + mods + kt)) | in knob units: 1.1 octaves per unit | F12's working hypothesis, literally: F3's law applied to the sum. |
| **(a2) Knob sum, 1 octave per unit** | 4.5675 Hz · 2^(x + c + mods + kt) | in knob units: 1 octave per unit, so keytrack 10 is 1:1 | The family with r forced to 1 by keytrack, anchored by the LCD's 146.16 Hz at 5. |
| **(b) A centred offset** | F3(x + modX) · 2^(1.1·(c + modAB − 5)) | `keytrackOctaves`, 1:1 | "The Filter Control's cutoff as an offset centred on some knob position": at its default the filter's own knob is F3 exactly. The same family as (a), with another anchor. |
| **(c) Hz added** | (F3(x + modX) + F3(c + modAB)) · 2^(keytrack octaves) | `keytrackOctaves`, 1:1 | The developer's "simply added" read in Hz. |

kt is the keytrack source in knob units, Keytrack × (pitch − 9)/120. Clamps (for the (a) models; (b) and (c) clamp per control): none, per control, the sum to [0, 20], the sum to [0, 10].

Four resonance models, F5's k = 0.6 per knob unit: **knob units added**, each control plus its modulation to [0, 10] (k up to 12); **knob units added, the sum to [0, 10]** (k up to 6); **multiplied**, k = k_X × (Filter Control / 5) (5 neutral); **the larger** of the two. Knob units added and k added are the same model under F5's linear law.

Four feeds, as in the theory: **W1** the filter's ramp, **W2** the voice's ramp into the modulation input, **W3** both, **W4** neither.

## What they measured

Stage 1's measurements, by the hidden `[filter-control-report]` ([filter_control_report.cpp](../../tests/dsp/filters/filter_control_report.cpp), about 12 s) and the `[filter-control]` benchmarks ([filter_control_benchmark.cpp](../../tests/dsp/benchmarks/filter_control_benchmark.cpp)), on the prototypes and `LadderFilter`. Since stage 2 the report measures the library's law for the chosen model, fed as the voice will feed it; every figure in this section reproduces to the printed digit but one ([Results, as built](#results-as-built)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[filter-control-report]"
tools/bench.sh "[filter-control]"
```

### What each model predicts for the reported settings

44.1 kHz, each control clamped to [0, 10], the effective cutoff after the DSP's clamp to [1 Hz, 21,830 Hz]:

| Setting | Source | (a1) knob sum, F3 | (a2) knob sum, 1 oct/unit | (b) centred offset | (c) Hz added |
|---|---|---|---|---|---|
| A knob at 5 reads (LCD) | 146.16 Hz (titchbit, 2013) | 452.5 Hz | **146.2 Hz** | 452.5 Hz | 452.5 Hz |
| A knob at 7 reads (LCD) | "7 (around the 800Hz mark)" (Attack, 2015) | 2,079 Hz | 584.6 Hz | 2,079 Hz | 2,079 Hz |
| Knob position that reads 800 Hz | | 5.75 | 7.45 | 5.75 | 5.75 |
| Defaults: A 5, Control 5 | centre defaults (titchbit; fluffy, 2018) | 20,480 Hz | 4,677 Hz | 452.5 Hz | 905.1 Hz |
| Detuned pad: A 800 Hz, Control 60 Hz, Keytrack 3, at C3 | Attack, 2015 | 10,095 Hz | 20,657 Hz | 208.5 Hz | 1,690 Hz |
| Vintage arp: A 40 Hz, Control 20 Hz | Attack, 2015 | 80.00 Hz | 175.2 Hz | 1.77 Hz | 60.00 Hz |
| … with Mod Env 1 at +10 on Cutoff A, at its peak | Attack, 2015 | 21,830 Hz | 20,480 Hz | 905.1 Hz | 20,500 Hz |
| A 0, Control 5 / 4.5 ("closes completely" at 4.5) | fluffy, 2018 (A unstated) | 452.5 / 309.1 | 146.2 / 103.4 | 10.00 / 6.83 | 462.5 / 319.1 |
| A 10, Control 5 ("at 10.000 the filter is fully open") | 7up, 2017 | 21,830 Hz | 21,830 Hz | 20,480 Hz | 20,933 Hz |
| A 10, Control 0 ("you may need to adjust both cutoff knobs") | the developer, 2008 | 20,480 Hz | 4,677 Hz | 452.5 Hz | 20,490 Hz |
| A 0, Control 0: the bottom | | 10.00 Hz | 4.57 Hz | 1.00 Hz | 20.00 Hz |
| Keytrack 10, one octave of the keyboard | 1:1 | **1320 cents** | 1200 cents | 1200 cents | 1200 cents |

- Only (a2) reads 146.16 Hz at the centre, by construction: F3's law reads 452.5 Hz there, a factor of 3.1 off the one exact LCD reading. "7 ≈ 800 Hz" is 0.45 of a unit from (a2)'s 7.45 and 1.25 units from F3's 5.75.
- **(a1) puts the default patch at 20.5 kHz**, and half of the knob square (49.5%) at the ceiling: with both knobs at their centre the filter is all but fully open (20.5 kHz), and Cutoff A at 10 with the Filter Control at 0 gives the same, so the developer's "you may need to adjust both cutoff knobs" never applies.
- **(b) is dark everywhere**: the default is 452 Hz and the bottom 1 Hz, the vintage arp sits at 1.8 Hz, and Cutoff A at 10 needs the Filter Control above 5 to open.
- **(c)** has the developer against it ("added on a logarithmic scale"; the base frequency from the sum of the values).
- **(a2)** fits each statement: the default is 4.68 kHz, a usable open-ish filter; one knob at 10 with the other at its default is fully open (7up); one knob at 10 with the other at 0 is 4.68 kHz, so "you may need to adjust both cutoff knobs" holds; the old LCD's reading at the centre is the knob's own term, five octaves under the actual cutoff at the defaults, which is "pretty useless" indeed; and keytrack 10 is 1:1. The tutorials' patches come out plausible: a bright detuned pad and a dark arp (175 Hz) that its envelope opens to 20.5 kHz.

### The effective cutoff over filter X × Filter Control

The −12.04 dB point of LP24 at k 0 and −80 dBFS, found by bisection on the impulse response's DTFT: the prewarped ladder puts it exactly at the cutoff, so this measures the cutoff the filter actually uses. Rows: Cutoff A; columns: the Filter Control's Cutoff; 44.1 kHz, with 96 kHz in brackets where the ceiling differs. Every cell, at both rates and in every model, is the model's formula within **0.03 cents** (tolerance 0.14 cents, from the filter tests' small-signal bound of 4·10⁻⁵ at |H| = 1/4 and LP24's 12.04 dB per octave there).

**(a2), 1 octave per unit, 146.16 Hz at 5:**

| A \ Control | 0 | 2.5 | 5 | 7.5 | 10 |
|---|---|---|---|---|---|
| 0 | 4.57 | 25.84 | 146.2 | 826.8 | 4,677 |
| 2.5 | 25.84 | 146.2 | 826.8 | 4,677 | 21,830 (26,458) |
| 5 | 146.2 | 826.8 | 4,677 | 21,830 (26,458) | 21,830 (47,520) |
| 7.5 | 826.8 | 4,677 | 21,830 (26,458) | 21,830 (47,520) | 21,830 (47,520) |
| 10 | 4,677 | 21,830 (26,458) | 21,830 (47,520) | 21,830 (47,520) | 21,830 (47,520) |

**(a1), F3's law over the sum:**

| A \ Control | 0 | 2.5 | 5 | 7.5 | 10 |
|---|---|---|---|---|---|
| 0 | 10.00 | 67.27 | 452.5 | 3,044 | 20,480 |
| 2.5 | 67.27 | 452.5 | 3,044 | 20,480 | 21,830 (47,520) |
| 5 | 452.5 | 3,044 | 20,480 | 21,830 (47,520) | 21,830 (47,520) |
| 7.5 | 3,044 | 20,480 | 21,830 (47,520) | 21,830 (47,520) | 21,830 (47,520) |
| 10 | 20,480 | 21,830 (47,520) | 21,830 (47,520) | 21,830 (47,520) | 21,830 (47,520) |

**(b), the Filter Control centred on 5:**

| A \ Control | 0 | 2.5 | 5 | 7.5 | 10 |
|---|---|---|---|---|---|
| 0 | 1.00 | 1.49 | 10.00 | 67.27 | 452.5 |
| 2.5 | 1.49 | 10.00 | 67.27 | 452.5 | 3,044 |
| 5 | 10.00 | 67.27 | 452.5 | 3,044 | 20,480 |
| 7.5 | 67.27 | 452.5 | 3,044 | 20,480 | 21,830 (47,520) |
| 10 | 452.5 | 3,044 | 20,480 | 21,830 (47,520) | 21,830 (47,520) |

**(c), Hz added:**

| A \ Control | 0 | 2.5 | 5 | 7.5 | 10 |
|---|---|---|---|---|---|
| 0 | 20.00 | 77.27 | 462.5 | 3,054 | 20,490 |
| 2.5 | 77.27 | 134.5 | 519.8 | 3,112 | 20,547 |
| 5 | 462.5 | 519.8 | 905.1 | 3,497 | 20,933 |
| 7.5 | 3,054 | 3,112 | 3,497 | 6,089 | 21,830 (23,524) |
| 10 | 20,490 | 20,547 | 20,933 | 21,830 (23,524) | 21,830 (40,960) |

The (a) and (b) tables are constant along each anti-diagonal (x + c fixed), as the family must be; (c)'s aren't, and its first row and column barely move until the other knob passes it.

### The reachable range, the ceiling, and the knob's feel

Knobs only (both over [0, 10] on a 0.1 grid), after the DSP's [1 Hz, 0.495 fs]:

| Model | Rate | Lowest | Default (5, 5) | Highest | Square at the ceiling | Span of the sum | One knob |
|---|---|---|---|---|---|---|---|
| (a1) knob sum, F3's law | 44.1 | 10.00 | 20,480 | 21,830 | 49.5% | 22 oct | 11 oct |
| | 96 | 10.00 | 20,480 | 47,520 | 39.3% | | |
| (a2) knob sum, 1 oct/unit | 44.1 | 4.57 | 4,677 | 21,830 | 30.2% | 20 oct | 10 oct |
| | 96 | 4.57 | 4,677 | 47,520 | 22.3% | | |
| (b) centred offset | 44.1 | 1.00 | 452.5 | 21,830 | 12.5% | 22 oct | 11 oct |
| | 96 | 1.00 | 452.5 | 47,520 | 7.6% | | |
| (c) Hz added | 44.1 | 20.00 | 905.1 | 21,830 | 2.3% | 11 oct | 5.5 oct |
| | 96 | 20.00 | 905.1 | 40,960 | 0.0% | | |

**Where the DSP's ceiling takes over** (the sum in knob units where the cutoff reaches 0.495 fs):

| Model | 44.1 kHz | 48 kHz | 96 kHz | 192 kHz |
|---|---|---|---|---|
| (a1) | 10.08 | 10.19 | 11.10 | 12.01 |
| (a2) | 12.22 | 12.34 | 13.34 | 14.34 |

**How the knob feels**: octaves per unit of Cutoff A, at A = 0 / 2.5 / 5 / 7.5 / 10, with the Filter Control at 0 / 5 / 10 in each cell (44.1 kHz; 0.00 is a dead zone):

| Model | A 0 | A 2.5 | A 5 | A 7.5 | A 10 |
|---|---|---|---|---|---|
| (a1) | 1.10 / 1.10 / 1.10 | 1.10 / 1.10 / 0.00 | 1.10 / 1.10 / 0.00 | 1.10 / 0.00 / 0.00 | 1.10 / 0.00 / 0.00 |
| (a2) | 1.00 / 1.00 / 1.00 | 1.00 / 1.00 / 0.00 | 1.00 / 1.00 / 0.00 | 1.00 / 0.00 / 0.00 | 1.00 / 0.00 / 0.00 |
| (b) | 0.00 / 1.10 / 1.10 | 1.10 / 1.10 / 1.10 | 1.10 / 1.10 / 1.10 | 1.10 / 1.10 / 0.00 | 1.10 / 1.10 / 0.00 |
| (c) | 0.55 / 0.02 / 0.00 | 0.96 / 0.14 / 0.00 | 1.08 / 0.55 / 0.02 | 1.10 / 0.96 / 0.00 | 1.10 / 1.08 / 0.00 |

- The knob-unit family feels the same everywhere: r octaves per unit until the ceiling (or (b)'s 1 Hz floor). Its only dead zone is the ceiling, and how much of the square it takes is set by the anchor: half under (a1), 30% under (a2) at 44.1 kHz, 22% at 96 kHz.
- (c) is uneven: with the Filter Control at 5 (452 Hz), Cutoff A does almost nothing below 2.5 (0.02–0.14 octaves per unit), then 1.1 near the top.
- **F3's span.** Under the knob-unit family one knob spans 10·r octaves and the sum 20·r. With r = 1, the 10 octaves keytrack 10 needs; under (a2) the sum's 20 octaves start at 4.57 Hz and pass the ceiling at 12.2–14.3 units, so 6–8 of them lie above it. That is headroom for modulation and keytrack rather than waste: from the default, +2.2 units open the filter fully at 44.1 kHz and −10 close it to 4.57 Hz.

**With keytrack and modulation**, before the DSP's clamp (what the voice asks for): under (a2), keytrack 10 over MIDI 0–127 with the knobs anywhere reaches 2.72 Hz to 2^32 Hz; sixteen slots at ±10 with each control clamped reach 4.57 Hz to 2^22.2 Hz; unclamped, −308 to +332 octaves. The filter clamps its modulation input to ±40 octaves and then to [1 Hz, 0.495 fs], so none of this is unsafe; it is a question of what the knobs mean.

### Keytrack from C1 to C7

44.1 kHz, k 4.4, Warm on; the filter self-oscillating, Cutoff A set so that C4's cutoff is 261.6 Hz at each keytrack (the Filter Control at 0, no clamp). Slope: the least-squares fit of the self-oscillation pitch in cents per octave of the keyboard; spread: the largest distance of any note from that line.

| Model | Keytrack 0 | 5 | 10 | Spread at 10 |
|---|---|---|---|---|
| (a1), keytrack in knob units at 1.1 oct/unit | 0.0 | 660.0 | **1320.1** | 0.33 cents |
| (a2), keytrack in knob units at 1 oct/unit | 0.0 | 600.0 | 1200.1 | 0.21 cents |
| (b), `keytrackOctaves` | 0.0 | 600.0 | 1200.1 | 0.21 cents |
| (c), `keytrackOctaves` on the Hz sum | 0.0 | 600.0 | 1200.1 | 0.21 cents |

Every model tracks exactly what its law says (the spread is the detune's flatness across the cutoff, keytracking.md). Under (a1) keytrack 10 is 1.1:1, a major second too wide per octave. Under (a2) the knob-unit keytrack and the built `keytrackOctaves` are the same number, so keytracking needs no change. And under (a2) the sum that makes keytrack 10 play each note's own frequency (before the self-oscillation detune) is **1.59** (13.75 Hz, A-1's frequency, the reference): Cutoff A at 1.59 and the Filter Control at 0, or any pair summing to that.

### Full ± modulation and the clamps

An LFO at ±10 units on Cutoff A, the Filter Control at 5, over one cycle, (a2), 44.1 kHz (the (a1) table is in the report):

| Clamp | A | Lowest | Highest | Held by the knob clamp | At the ceiling |
|---|---|---|---|---|---|
| none | 0 | 1.00 | 21,830 | 0% | 24.3% |
| | 5 | 4.57 | 21,830 | 0% | 42.9% |
| | 10 | 146.2 | 21,830 | 0% | 58.9% |
| each control + its modulation | 0 | 146.2 | 21,830 | 49.9% | 24.3% |
| | 5 | 146.2 | 21,830 | 66.6% | 42.9% |
| | 10 | 146.2 | 21,830 | 49.9% | 58.9% |
| the sum to [0, 20] | 0 | 4.57 | 21,830 | 33.3% | 24.3% |
| | 5 | 4.57 | 21,830 | 0% | 42.9% |
| | 10 | 146.2 | 21,830 | 33.3% | 58.9% |
| the sum to [0, 10] | 0 | 4.57 | 4,677 | 66.6% | 0% |
| | 5 | 4.57 | 4,677 | 49.9% | 0% |
| | 10 | 146.2 | 4,677 | 66.7% | 0% |

Every slot at full depth (16 × 10 knob units) on Cutoff A or Resonance A, every knob at 10:

| Clamp | (a2) cutoff, all +10 | all −10 | k, knob units added |
|---|---|---|---|
| none | 182 octaves above 1 Hz | −138 | 108 |
| each control + its modulation | 22.2 | 12.2 | **12** |
| the sum to [0, 20] | 22.2 | 2.2 | 12 |
| the sum to [0, 10] | 12.2 | 2.2 | 6 |

- **The sum to [0, 10] contradicts the evidence**: the default (5 + 5) is then the top, so Cutoff A at 10 does nothing from the default, and the cutoff can never pass 4.68 kHz under (a2), against "at 10.000 the filter is fully open" and the developer's top at half the sample rate. Under (a1) it caps the cutoff at 20.5 kHz.
- **Per control against the sum to [0, 20]** differ only when modulation pushes one knob past its end while the other has room: per control, an LFO at full depth on Cutoff A at 0 is held at A = 0 for half its cycle (the cutoff stays at 146 Hz from the Filter Control); on the sum, it borrows the Filter Control's units and goes down to 4.57 Hz. Nothing in the evidence separates them; per control is what F24 says ("each control plus its modulation"), and it is the one a user can predict from the knob they modulate. F29 is the demo test.
- **No clamp** lets the voice ask for k 108 and hundreds of octaves; the DSP's ceilings (k 64, ±40 octaves, then [1 Hz, 0.495 fs]) keep it safe, but then a knob at 10 with modulation still adds, which F24's demo test (Resonance 10 plus an envelope) would show as louder and flatter.

**The filter under it.** A full-scale 110 Hz saw, (a2), A 5 + Control 5 with an LFO at ±10 on Cutoff A, both Resonances at 10 plus a +10 envelope on Resonance A, Warm on, fed at each grid point; the peak of `LadderFilter`, of the double-precision reference fed the same g, and of the reference with g computed every sample:

| Cutoff clamp | k | LFO | Peak | Finite | Reference | Reference, g per sample |
|---|---|---|---|---|---|---|
| none | 18 | held at either extreme | 0.31–0.37 | yes | 0.31–0.37 | 0.31–0.37 |
| | | 0.5 Hz | 0.76 | yes | 0.77 | 0.96 |
| | | 5 Hz | **12.82** | yes | 12.80 | 9.68 |
| each control | 12 | held | 0.36 | yes | 0.36 | 0.36 |
| | | 0.5 Hz | 0.80 | yes | 0.79 | 0.88 |
| | | 5 Hz | **13.10** | yes | 13.12 | 13.37 |
| each control | 6 | held | 0.34–0.36 | yes | 0.34–0.36 | 0.34–0.36 |
| | | 5 Hz | **9.83** | yes | 9.61 | 5.51 |
| the sum to [0, 10] | 6 | any | ≤ 0.42 | yes | ≤ 0.42 | ≤ 0.42 |

**Found: a fast, deep sweep of a self-oscillating ladder overshoots far past its steady level**: a 5 Hz LFO across 20 octaves (100 octaves per second) at k 6–18 peaks at 9.8–13.1, 29–31 dB above the same patch with the cutoff held (0.31–0.37). It is not the combining law's doing, nor the solver's, nor the grid's: the double-precision reference fed the same g agrees to 2% (and with g per sample it peaks at 5.5–13.4). It is the trapezoidal ladder's own response to a cutoff that falls an octave every 10 ms and rises again while resonating, and it only appears where the clamp lets the cutoff move: the sum clamped to [0, 10] pins this patch at the ceiling, which is why that row is quiet. Whether Sylenth1 does the same is F30. The voice is not clipped (Sylenth1 has "no output clipping", M1.4), so this is a level to know about in M1.5's modulation rates and M1.8's golden renders rather than a safety issue: every sample is finite and the filter matches its reference. *M1.5 task 2, stage 1 (2026-11-22; [modulation.md](modulation.md#the-fast-sweep-overshoot-filter-controlmd-recommendation-8)): the modulation's rate doesn't remove it. Through the voice, reading the LFO at the segment's end rather than as it stands at the grid point moves the peak by −46% to +46%. Between 44.1 and 48 kHz the peak moves ×2.8 (12.00 against 4.24 at 5 Hz).*

### Resonance

Where self-oscillation starts: the Resonance A position, with the Filter Control's Resonance at 0 / 5 / 10, under F5's k = 0.6 per unit (checked on `LadderFilter`: a 10⁻¹⁵ impulse decays 0.05 units below and grows 0.05 above):

| Model | Control 0 | Control 5 | Control 10 |
|---|---|---|---|
| knob units added, each clamped | 6.67 | 1.67 | 0 (already) |
| knob units added, the sum to [0, 10] | 6.67 | 1.67 | 0 |
| multiplied (Control / 5) | never | 6.67 | 3.33 |
| the larger | 6.67 | 6.67 | 0 |

k with the Filter Control's Resonance at 0 → 10, for a filter at 0, 5, 10:

| Model | Filter 0 | Filter 5 | Filter 10 |
|---|---|---|---|
| knob units added, each clamped | 0 → 6 | 3 → 9 | 6 → 12 |
| knob units added, the sum to [0, 10] | 0 → 6 | 3 → 6 | 6 → 6 |
| multiplied (Control / 5) | 0 → 0 | 0 → 6 | 0 → 12 |
| the larger | 0 → 6 | 3 → 6 | 6 → 6 |

And the self-oscillation at those k (440 Hz, 48 kHz):

| k | 4.2 | 5.4 | 6 | 7.2 | 9 | 10.8 | 12 | 18 |
|---|---|---|---|---|---|---|---|---|
| Warm on: dBFS / cents | −19.2 / −19.6 | −12.7 / −85.3 | −11.9 / −101.2 | −11.1 / −119.1 | −10.7 / −130.8 | −10.5 / −135.9 | −10.4 / −137.7 | −10.3 / −141.2 |
| Warm off | −15.9 / −18.6 | −11.0 / −60.3 | −10.7 / −64.3 | −10.4 / −68.4 | −10.3 / −71.6 | −10.2 / −73.2 | −10.1 / −73.9 | −10.1 / −75.6 |

- **Added in knob units** (the developer's "knob values are added"): the Filter Control's Resonance at 10 makes every filter self-oscillate, and a filter at 10 goes to k 12. Above k 8 the ladder changes little: 0.5 dB louder and 12 cents flatter from k 8 to 12 (Warm on; k 8 is −10.9 dBFS and −125.7 cents, filters.md). So "way beyond self-oscillation" costs nothing in stability (the DSP is correct to k 64, filter-stability.md), and gives the top of the sum a meaning: a saturated tone at −10.4 dBFS.
- **The sum clamped to [0, 10]** makes the Filter Control useless for a filter already at 10, and caps every patch at k 6.
- **Multiplied** contradicts "added", and with the Filter Control at 0 no filter can resonate, so a patch with Filter A at Resonance 10 would be silent of resonance until the panel's knob moved: no source describes that.
- **The larger** contradicts "added" too.

### Feeding the sum: the ramp, the grid, and the clamp

48 kHz, (a2). **A Filter Control step** from 3 to 6 with A at 5 (1,169 → 9,354 Hz); click: LP24 at k 3.6, a 150 Hz sine at −12 dBFS, energy above 1 kHz around the step, dB re an instant unit step (filters.md's measure):

| Feed | To 90% | From W1 | Click |
|---|---|---|---|
| W1: the knobs through `setCutoff`, the filter's ramp | 9.33 ms | 0 | −68.0 dB |
| W2: the voice's ramp, all into the modulation input | 9.33 ms | **0.000 cents** | −68.0 dB |
| W3: the voice's ramp into `setCutoff`, ramped twice | 18.00 ms | up to 2,166 cents | −80.3 dB |
| W4: no ramp | 0.67 ms | up to 3,360 cents | −49.2 dB |
| steady (Control at 6 throughout) | | | −114.9 dB |

**A 7-bit MIDI CC sweep** of the Filter Control, 0 → 10 in 2 s, events at host blocks of 512 (128 steps of 0.079 units, 94 cents each under (a2)); A at 3; the cutoff at each grid point below the ceiling:

| Feed | Largest step between grid points | Roughness (RMS second difference) | Lag behind the knob |
|---|---|---|---|
| W1 | 6.3 cents | 1.83 cents | 61.6 cents (10.3 ms) |
| W2 | 6.3 cents | 1.83 cents | 61.6 cents (10.3 ms) |
| W3 | 5.8 cents | 0.79 cents | 105.6 cents (17.6 ms) |
| W4 | **94.5 cents** | 27.44 cents | 33.7 cents (5.6 ms) |

**The clamp during a ramp**: A from 2 to 9 with +3 of modulation on Cutoff A and the Filter Control at 2; each control clamped, so the sum goes from 7 to 12, reaching it when the ramping A passes 7:

| Feed | Largest distance from the exact clamp | Click |
|---|---|---|
| W1 | **2,400 cents** | −21.2 dB |
| W2 | 0.0 cents | −44.8 dB |

**Block sizes**: the step with its event one sample after a grid point, in host blocks of 1 and 37 against 4096: W1 and W2 bit-identical.

- **W2 is W1 exactly** wherever no clamp bites (0.000 cents through the step and the sweep, the same click), because a 10 ms linear ramp of knob units read at each grid point *is* the filter's 10 ms ramp in log₂. Where a per-control clamp bites during a ramp, W1 is wrong by up to two octaves and clicks 24 dB louder: it has to clamp against the knob's target, so its modulation part steps at once while the knob part ramps.
- **W3 smooths twice**: 18 ms to 90% instead of 9.3, and 17.6 ms behind a sweep. It is smoother between grid points, but no zipper needs that: W2's largest step is already 6.3 cents against W4's 94.5.
- **W4 zippers**: a 7-bit CC is a 94-cent staircase, and a step clicks at −49 dB against −68 ramped.

### CPU

`tools/bench.sh "[filter-control]"`, `relwithdebinfo`, Apple M1 Ultra (virtualised). One voice, both filters, at the 16 grid points of a 512-sample block, every term moving:

| Benchmark | ns per block | ns per sample |
|---|---:|---:|
| (a1) knob sum, F3's law | 45 | 0.09 |
| (a2) knob sum, 1 oct/unit | 45 | 0.09 |
| (b) centred offset | 39 | 0.08 |
| (c) Hz added (two `exp2`, one `log2`) | 184 | 0.36 |
| Resonance, knob units added | 22 | 0.04 |
| `LadderFilter`, W1 | 45,735–45,832 | 89.3–89.5 |
| `LadderFilter`, W2, the same cutoffs | 44,770–48,358 | 87.4–94.5 |

The combining law costs 0.1 ns per sample per voice, against the filter's 90; feeding everything through the modulation input is within the filter's run-to-run noise of W1.

## The decision

*Decided by the project owner on 2026-11-21: every recommendation of stage 1, as written ([PLAN.md](../../PLAN.md), Decisions log). The stage-1 reasons are kept in the last column. [As built](#as-built) says how each one is built and measured.*

| # | Question | Decision | Why (stage 1) |
|---|---|---|---|
| 1 | **Cutoff: where the terms combine** | **(a2): add in knob units, then one exponential law at 1 octave per unit**: S = [Cutoff X + its modulation]₀¹⁰ + [Filter Control Cutoff + Cutoff AB modulation]₀¹⁰ + Keytrack × (pitch − 9)/120, cutoff = 4.5675 Hz × 2^S (146.16 Hz with one knob at 5 and the other at 0; 4,677 Hz at the defaults). Built: `filterCutoffUnits`, `filterCutoffOctaves` ([filter_control.hpp](../../libs/dsp/include/anoesis/dsp/filters/filter_control.hpp)). | The developer, twice: the knob values add, on a logarithmic scale, and the base frequency comes from their sum. That rules out (c). Keytrack adds knob units and keytrack 10 is 1:1, which forces 1 octave per unit, so not (a1) (1320 cents per octave). Among the 1-octave members only the anchor is free: (a2)'s is the old LCD's one exact reading, and it fits every reported setting (default open-ish, one knob at 10 fully open, "adjust both" when the other is low, the "useless" display five octaves under the cutoff at the defaults, plausible tutorial patches). (b) fits no reading and makes every reported patch dark. The anchor is one constant: F3/F27 measure it. |
| 2 | **Clamping** | **Each control plus its own modulation to [0, 10]**, **then add; keytrack after the clamps, unclamped; no clamp on the sum**; then the DSP's [1 Hz, 0.495 fs], from S = 12.22 (44.1 kHz), 12.34 (48), 13.34 (96), 14.34 (192). The same for the resonances. Built: `clampToKnob`. | F24's hypothesis, applied to each control as it says. The sum to [0, 10] contradicts "fully open at 10" and the developer's top at half the sample rate (the cutoff could never pass 4.68 kHz) and makes the Filter Control's Resonance do nothing for a filter at 10. The sum to [0, 20] behaves the same except where modulation could borrow the other knob's room; nothing separates them, per control is what F24 states, and F29 settles it. |
| 3 | **Resonance** | **Added in knob units**, each control plus its modulation clamped to [0, 10], then **k = 0.6 × (Resonance X + Filter Control Resonance), 0 to 12**. Self-oscillation from a sum of 6.67. The Filter Control's Resonance defaults to 0 until F26 is measured. Built: `filterResonanceUnits`, `filterResonanceK`. | The developer's 2008 answer covered "knob values" for a question about cutoff and resonance. Multiplied and the larger contradict "added". k 12 is bounded and well-behaved (−10.4 dBFS, −137.7 cents, Warm on; the DSP is correct to 64). 0 keeps a filter's own knob meaning what F5 says. |
| 4 | **Where it lives** | **`libs/dsp/include/anoesis/dsp/filters/filter_control.hpp`**: `constexpr` pure functions, no state, no JUCE, next to `keytrackOctaves`. The knob range, the anchor, and the octaves per unit are parameters (`FilterControlLaw`, with k per unit too), with (a2)'s values as defaults. It returns octaves (above the anchor) and k. | The clamp couples global knobs with per-voice modulation at the grid rate, which only the voice can see. A pure function is what both the voice and its tests can share, as `keytrackOctaves` is. The DSP still takes physical units. |
| 5 | **What runs where** | **The knob ramps are global**, and their value can be read at any sample (`LinearSmoother::valueAfter`, [smoother.hpp](../../libs/dsp/include/anoesis/dsp/control/smoother.hpp)). **The clamped sum is computed per voice, at each of that voice's own 32-sample grid points**, with the knob values at the end of the coming segment. | Voices' grids are not aligned (each counts from its own note-on), so a global smoother advanced on one grid would read the wrong instant; a ramp read in closed form at each voice's segment end gives every voice W1's exact sequence. |
| 6 | **The ramp** | **W2: the voice ramps the knobs once, over 10 ms, and feeds the whole cutoff into `setCutoffModulation`; `setCutoff` stays at the anchor.** For k, stage 2 measured `setResonance()` per grid point first: it slews, so k goes through a new resonance modulation input, `setResonanceModulation` ([below](#feeding-k-a-resonance-modulation-input)), with `setResonance` at 0. | W2 is W1 to 0.000 cents wherever no clamp bites, and exact where one does (W1: 2,400 cents off and 24 dB more click). Ramping twice lags 18 ms. No ramp zippers in 94-cent steps on a MIDI CC. The same CPU. |
| 7 | **The keytrack knob law** | **Stands**: amount = Keytrack / 10, linear, reference A-1; `keytrackOctaves` unchanged. Under decision 1 it is exactly the knob-unit term, so the voice passes `keytrackOctaves(pitch, Keytrack / 10)` as keytrack's knob units. | At keytrack 10, Cutoff A + Filter Control = 1.59 plays each note's own frequency (A-1's 13.75 Hz at the reference), less the self-oscillation detune. |
| 8 | **The fast-sweep overshoot** | **Recorded only; nothing changes for it in this task.** Carried to M1.5 (the modulation rates) and M1.8 (golden renders), with F30 to compare against Sylenth1. | It is the trapezoidal ladder's response (the reference agrees), independent of the combining law and of the clamp choice except that clamps which pin the cutoff hide it. |

### What it changed in the inventory

- **F3 (the cutoff knob's law).** Replaced: the placeholder 10 Hz × 2^(11·x) per knob does not survive knob-unit addition (the default patch at 20.5 kHz, half of the knob square at the ceiling, keytrack 10 at 1320 cents per octave, and it misses the LCD's 146.16 Hz at 5 and "7 ≈ 800 Hz"). The adopted hypothesis: **the law is over the sum, 4.5675 Hz × 2^S, 1 octave per unit**, so one knob spans 10 octaves (keytracking.md's condition for 1:1) and the sum 20. Its "how to settle" measures the effective cutoff with the Filter Control at 0 and at 5, not the LCD: the v3.040 Hz display is the developer's "same (faulty) Hz scale", each knob's own term (F27).
- **F5 (resonance).** The law applies to the sum: k = 0.6 per unit reaches 12 at both knobs' tops, not 6, and self-oscillation starts at a sum of 6.67 (Resonance A at 6.67 alone, or at 1.67 with the Filter Control at 5). F5's measurement holds the Filter Control's Resonance at 0, then repeats at 5 and 10 (F28).
- **F12 (the combining law)** and **F24 (the clamp)**. Adopted as above. F24's clamp of each control plus its own modulation is now built into the law for cutoff and resonance, which the voice computes (it is the only place that sees both the global knob and the voice's modulation); for Drive and every other destination it stays with M1.5/M1.9. The voice can ask for k up to 12 (two clamped controls added), not 6, and for a cutoff sum up to 20 units, 6–8 of them above the ceiling.
- **The keytrack knob law** stands, but its 1:1 depends on the cutoff law: if the demo finds r ≠ 1 octave per unit, then either Sylenth1's keytrack isn't 1:1 (F21) or keytrack enters in octaves rather than knob units (F20's second test), and the term must be scaled.

## As built

### The functions

[filter_control.hpp](../../libs/dsp/include/anoesis/dsp/filters/filter_control.hpp) holds the law as `constexpr` functions with no state and no includes, in keytrack.hpp's style:

- `FilterControlLaw` holds the law's numbers: the knob range (`knobMin` 0, `knobMax` 10), `anchorHz` (4.5675, which is 146.16 Hz five octaves down, exactly in float), `octavesPerUnit` (1), and `kPerUnit` (0.6, F5). `filterControlLaw` is the decided instance. A demo measurement (F3, F5, F27–F29) changes a default, not a function.
- `FilterCutoffTerms` and `FilterResonanceTerms` name one filter's terms in knob units: its own knob and modulation, the Filter Control's knob and the AB modulation, and, for the cutoff, keytrack.
- `clampToKnob(units)` holds one control plus its modulation to the knob's range. It is written out rather than `std::clamp`, so the header needs no include, and NaN passes through.
- `filterCutoffUnits(terms)` is S: the two clamped brackets added, then keytrack, unclamped. `filterCutoffOctaves(terms)` is octavesPerUnit × S, the octaves above the anchor that the voice feeds to `setCutoffModulation()`.
- `filterResonanceUnits(terms)` is the two clamped resonance brackets added (0 to 20), and `filterResonanceK(terms)` is kPerUnit × that (0 to 12).

Keytrack's term is in knob units: the developer's misc source times the knob, Keytrack × (pitch − 9)/120. At one octave per unit that is `keytrackOctaves(pitch, Keytrack / 10)`, so the voice passes that, computed once per voice for both filters. With other law numbers it is still knob units, scaled by `octavesPerUnit` like every other term, which is why F3's 1.1 octaves per unit would make keytrack 10 track 1.1:1.

NaN in gives NaN out, as keytrack.hpp does. Note what that now means on the filter: with the whole cutoff in the modulation input, a NaN cutoff modulation counts as 0 octaves, so the cutoff falls to the anchor, 4.57 Hz, where with keytrack alone it meant "no keytrack". A NaN k counts as 0. The voice must not feed NaN; whether it can is a question for M1.4 (recorded there in PLAN.md).

### The knob ramps: one global ramp, read at each voice's grid

The parameter layer (M1.9) ramps each knob linearly over 10 ms from its event's sample: Filter A's and B's Cutoff and Resonance, and the Filter Control's Cutoff and Resonance. Each is one `LinearSmoother`, global, shared by every voice. A voice's 32-sample grid counts from its own note-on, so voices read the ramps at different samples. `LinearSmoother` had no way to read its value at an arbitrary offset, so it gains the smallest thing that does: `valueAfter(n)`, the value `next()` would return n calls from now, computed with the same `start + step·k` expression, so it is that value bit for bit, without advancing ([parameter-smoothing.md](parameter-smoothing.md)). The parameter layer positions the ramps at the start of each piece of a host block, split at events; a voice at a grid point G reads `valueAfter(G + 32 − pieceStart)`; after the piece, the ramps `skip()` its length. Reading n ahead after skipping m is the same as reading m + n ahead, so the host's block size changes nothing.

The ramp starts at the event's own sample, wherever that falls on any voice's grid. Each voice then reads it exactly at its segment ends, except for the one segment that holds the event: that segment's end was computed at its grid point, before the event, so it keeps the old value. The filter latches a segment's end when the segment starts, so no feed can do better without changing the filter. W1, the filter's own ramp, is as late there and then lags for the whole ramp (it starts at the next grid point). Stage 1's W2 advanced per-voice smoothers on the grid, so a mid-segment event started its ramp at the next grid point too. Where events fall on the grid, as in every stage-1 measurement, the two are the same.

### Feeding k: a resonance modulation input

`setResonance()` restarts its 10 ms ramp at every call. So k fed through it at each grid point is slewed: each call moves 32/480 of the way towards a target that has already moved. Stage 2 measured that first, against the knob's own ramp, with stage 1's metrics. All figures are at 48 kHz, from the report:

| Feed | Step to 90% | Distance from the knob's ramp | Click | CC sweep: largest step / roughness / lag | LFO on Reso A, ±1.2 in k: swing and lag at 2 / 5 / 20 Hz |
|---|---|---|---|---|---|
| The filter's ramp: `setResonance()` once at the event (the knob's own ramp; no per-voice modulation possible) | 9.33 ms | 0.0000 | −57.3 dB | | |
| `setResonance()` at each grid point | **18.00 ms** | 1.0832 in k | −67.8 dB | 0.0029 / 0.00039 / **0.0526 (17.5 ms)** | **0.993× / 9.3 ms; 0.957× / 9.1 ms; 0.636× / 6.7 ms** |
| `setResonanceModulation()` at each grid point (built) | 9.33 ms | 0.0000 | −57.4 dB | 0.0031 / 0.00091 / 0.0306 (10.2 ms) | 1.000× / 0.0 ms at each |
| Steady (no step) | | | −115.3 dB | | |

The step is the Filter Control's Resonance from 1 to 4 with Resonance A at 2 (k 1.8 → 3.6), at 1,169 Hz, under a 150 Hz sine at −12 dBFS. The sweep is a 7-bit MIDI CC from 0 to 10 in 2 s (k 0 → 6) in host blocks of 512; the knob itself lags 10.2 ms behind the unquantised sweep, its 10 ms ramp.

**`setResonance()` per grid point slews**: it smooths twice, as W3 does for the cutoff. A step takes 18 ms to reach 90% where the knob's ramp takes 9.3. A sweep lags 7.3 ms more. An LFO on Reso X or AB loses swing and lags (−3.9 dB and 6.7 ms at 20 Hz). Its quieter click is only the slower ramp. So k goes through a new input:

- **`LadderFilter::setResonanceModulation(k)`**, analogous to `setCutoffModulation`: unramped, read at the next grid point, where k then runs linearly over the 32 samples to it (as g does). It is clamped to [0, 64], and NaN counts as 0. It adds to `setResonance()`'s ramped k, and the sum is capped at 64, the DSP's ceiling. `reset()` starts the grid at the modulation already set, as a note-on must.
- **The voice feeds the whole k through it** at each grid point, the k for the segment's end, with `setResonance(0)`, as the cutoff goes through `setCutoffModulation()` with `setCutoff()` at the anchor. At the grid points k is then exactly the voice's k; between them it is linear, which is exact on a linear knob ramp except at the ramp's two corners.
- With the modulation at 0, k is `setResonance()`'s exactly (k + 0 is k, under 64), so every existing filter figure and test is unchanged, bit for bit. The change is a few float operations per sample in the control loop that already computes k, not in the solve. `LadderFilter` grows from 344 to 360 bytes.

### How a voice uses it (M1.4)

```cpp
// Global, per block (the parameter layer, M1.9): each knob is a LinearSmoother
// ramping 10 ms from its event's sample; the block is split at events, and the
// ramps skip() each piece's length after the voices have read it.
//
// Per voice, at each of its filter's 32-sample grid points G (counted from its
// note-on), for the END of the coming segment (gridEnd = G + 32):
const auto ahead = gridEnd - pieceStart;
const float keytrack = keytrackOctaves(pitch(gridEnd), keytrackKnob / 10.0f); // once for both filters
filterA.setCutoffModulation(filterCutoffOctaves({
    .cutoff = cutoffA.valueAfter(ahead),
    .cutoffModulation = modCutoffA(gridEnd),
    .controlCutoff = controlCutoff.valueAfter(ahead),
    .controlModulation = modCutoffAB(gridEnd),
    .keytrack = keytrack,
}));
filterA.setResonanceModulation(filterResonanceK({
    .resonance = resonanceA.valueAfter(ahead),
    .resonanceModulation = modResoA(gridEnd),
    .controlResonance = controlResonance.valueAfter(ahead),
    .controlModulation = modResoAB(gridEnd),
}));
// ... the same for filter B with its own knobs and Cutoff B / Reso B modulation.
// At the note-on: setCutoff(filterControlLaw.anchorHz) and setResonance(0) (both
// held for good), the two modulations for the note-on's sample, then reset().
```

The tests' stand-in for the parameter layer and the voice, [filter_control_support.hpp](../../tests/dsp/filters/filter_control_support.hpp), does exactly this: global `KnobRamps`, host blocks split at events, any number of voices with their own note-ons, modulation, and pitch. The report uses it to measure the other feeds (W1, W3, W4, k through `setResonance()`) against it.

### Results, as built

The `[filter-control-report]` now measures the library's law for the chosen model, (a2) with each control clamped and the resonances added, fed through the stand-in voice, beside stage 1's other candidates, which stay in [filter_control_candidates.hpp](../../tests/dsp/filters/filter_control_candidates.hpp) for comparison. Every figure in [What they measured](#what-they-measured) reproduces to the printed digit, with one exception:

- **W2 against W1 through the Filter Control step: 0.001 cents (stage 1: 0.000).** The prototype held `setCutoff` at 2^2.1914047, a constant for log₂ 146.16 − 5; the build holds it at 4.5675 Hz in float, whose log₂ is 0.0003 cents from that constant, and adds the knob units to it in one rounding. One ulp of log₂ of the cutoff at 8–16 octaves is 2⁻²⁰ octaves, 0.0011 cents. The test bound for W2 against W1 is 0.014 cents, counted from the roundings on each path.

Two report texts changed with the build: the block-size row now says that W2's global ramp starts at the event's sample (stage 1's started at the next grid point), and the report adds the resonance-feeding section above.

**CPU** (`tools/bench.sh "[filter-control]"`, `relwithdebinfo`, Apple M1 Ultra, virtualised):

| Benchmark | ns per block of 512 | ns per sample |
|---|---:|---:|
| `filterCutoffOctaves` with keytrack, 2 filters × 16 grid points | 40 | 0.08 |
| `filterResonanceK`, 2 filters × 16 grid points | 21 | 0.04 |
| **The voice's whole feed**: 6 knob ramps read with `valueAfter`, keytrack, both filters' cutoff and k, 16 grid points | **87** | **0.17** |
| (a1), (b), (c), stage 1's other candidates, for comparison | 45, 39, 182 | 0.09, 0.08, 0.36 |
| `LadderFilter`, W1 (the knobs through `setCutoff`) | 45,332 | 88.5 |
| `LadderFilter`, W2 (the whole sum as modulation) | 48,003 | 93.8 |
| `LadderFilter`, W2 with k through `setResonanceModulation` (built) | 45,675 | 89.2 |

The whole Filter Control costs 0.17 ns per sample per voice, against the filter's 63–123 ns per channel. The resonance modulation input costs nothing measurable: against commit 9dec978 in the same session, two interleaved runs each, every `[filter]` row is within −8.3% to +5.7%, a mean of +0.9%, where the previous commit's own two runs differ by up to 6.5% ([baselines](../performance/baselines.md)). Its few operations sit in the per-segment loop that already ramps k, not in the per-channel solve.

### Tests

[filter_control_test.cpp](../../tests/dsp/filters/filter_control_test.cpp), 13 tests; two in [ladder_filter_test.cpp](../../tests/dsp/filters/ladder_filter_test.cpp) (with the resonance modulation added to its block-size and NaN tests); one in [smoother_test.cpp](../../tests/dsp/control/smoother_test.cpp). Each tolerance is derived in the test:

| Test | What it pins down | Tolerance and why |
|---|---|---|
| The law's values | 0, 5, 5, 10, 20 octaves for S = 0, one knob at 5 (either), the defaults, both tops, evaluated at compile time; the law's numbers; on the filter 4.5675, 146.16, and 4,677.12 Hz; `static_assert`s on the sums and on `noexcept` | exact; on the filter 2⁻¹⁹ + 2⁻²⁴ relative: log₂ of the anchor (2⁻²² octaves), the sum (2⁻²⁰), exp2 (2⁻²³ relative), and the anchor's own rounding |
| Each control clamped, the sum not | modulation past a control's end is lost even with room in the other (so not the sum to [0, 20]); each control's modulation is its own; modulation brings a control at its end back; all slots at ±160 give 0 and 20; the range as a parameter | exact |
| Keytrack after the clamps | a filter at its top still rises with the note (23 units), a closed one falls below 0; keytrack 10 over MIDI 0–129; the knob-unit term is `keytrackOctaves` bit for bit at every quarter tone from note −24 to 150 at six amounts, and the developer's Keytrack × (pitch − 9)/120 to rounding | exact; 5 × 2⁻²⁴ relative (three roundings against two) |
| Resonance | 0, 3, 6, 12; self-oscillation at a sum of 20/3 either way; each control clamped with its own modulation; ±160 on every slot stays in 0–12; k per unit as a parameter | one or two roundings of 0.6 × the sum (2⁻²⁴ each) |
| The parameters | F3's 1.1 octaves per unit scales the sum and keytrack alike; the units don't depend on the slope | one rounding |
| The ceiling | S* = 12.22, 12.34, 13.34, 14.34 at 44.1, 48, 96, 192 kHz; on the filter, S* − 0.01 is the law and S* + 0.01 is 0.495 fs exactly | the printed digits; 0.01 unit is 12 cents, far outside rounding |
| NaN | NaN in any term gives NaN out | exact |
| The −12 dB point | ten settings (S from −0.75 to 20, a clamp, keytrack at C7 and at note 0) at 44.1 and 96 kHz, fed as the voice feeds it | stage 1's 0.14 cents: the filter tests' small-signal bound of 4·10⁻⁵ at \|H\| = 1/4, and LP24's 12.04 dB per octave there |
| Self-oscillation from 6.67 | Resonance A 6.65 decays and 6.6833 grows; at 1.65 and 1.6833 with the Filter Control at 5; Warm on and off; through the voice's resonance input | the filter tests' pinned threshold, k 3.99 and 4.01: 1/60 of a unit either side |
| W2 is W1 where no clamp bites | a Filter Control step and a 7-bit CC sweep, every grid point; W4 misses by more than 24 cents | 12 × 2⁻²⁰ octaves (0.014 cents), the roundings of the two paths counted in the test |
| Exact where a clamp bites mid-ramp | A 2 → 9 with +3 of modulation and the Filter Control at 2: every grid point is the clamp of the ramped knob in double; W1 is over an octave off | the same 0.014 cents |
| Each voice reads the ramp at its own grid | two voices (note-ons 0 and 13), an event at sample 1000 on neither grid: each segment end is the ramp there, except the segment that holds the event (the old value); a voice's output is the same with or without the other | the same 0.014 cents; bit for bit |
| Block sizes, allocation | events at arbitrary samples, two voices, LFOs on Cutoff X and Reso AB and Reso X, keytrack 7 on a glide: blocks of 1 and 37 against 4096 | bit for bit; 0 allocations |
| The resonance modulation input (ladder_filter_test.cpp) | constant: `setResonance()` bit for bit at k 0–64; the sum capped at 64; negative, NaN, +inf; it adds to `setResonance()`'s k. A change mid-segment: read at the next grid point and linear to it, against a double linear ladder fed that k per sample; applied at once, or stepped at the grid point, differs by over ten times the bound | bit for bit; 10⁻⁴ of the peak, the g-interpolation test's bound |
| `valueAfter` (smoother_test.cpp) | bit for bit what `next()` returns, in seven states; not advancing; the target past the end; unchanged by where `skip()` splits it; no ramp time | bit for bit |

Red first: against inert stubs (every law function returning 0, `valueAfter` returning the current value, `setResonanceModulation` doing nothing), all 7 law tests, the `valueAfter` test, and both resonance-input tests failed; the behaviour tests were written after the law was green and the input built. Then 25 deliberate breaks, one at a time, each run against the tests that cover it:

- **The law** (12): the anchor at 146.16 Hz; F3's 1.1 octaves per unit; k per unit ignored; the sum clamped to [0, 20] or to [0, 10] instead of each control; keytrack inside the clamp; only the knob clamped, not knob plus modulation; Cutoff AB's modulation in filter X's bracket; resonance multiplied, the larger, or its sum clamped to [0, 10]; NaN clamped to the bottom. Six were stopped at compile time by the `static_assert`s (1.1 octaves per unit, the sum to [0, 20], keytrack inside the clamp, the knob alone clamped, the AB modulation swapped, resonance multiplied); the rest by one to seven tests each.
- **`valueAfter`** (4): one sample late, n ignored, the current value past the ramp's end, accumulated as current + step·n. Each caught by its own test and by one to four Filter Control tests on the filter.
- **The resonance input** (7): stepped at the grid point, applied at once, the sum not capped at 64, NaN not mapped to 0, negative allowed, and two in `reset()`. All caught but one: `reset()` setting the segment's start to 0 rather than its end is unobservable, because the first grid point overwrites the start with the end before any sample uses it, exactly as for g. The meaningful line, the segment's end, was broken instead (left at 0), and is caught by both tests.
- **The voice's feed** in the stand-in (2): the grid point's value instead of the segment end's (caught by three tests), and the ramps read from the host block's start instead of the piece's (caught by the block-size test). They keep the stand-in honest for M1.4.

## References

- LennarDigital, *Sylenth1 User Manual*, v2.5, 2015, pp. 10, 17–20, 31; [version history](https://lennardigital.com/sylenth1/whatsnew.php).
- The developer on KVR: [t=234164 (2008)](https://www.kvraudio.com/forum/viewtopic.php?t=234164), [t=234079 (2008)](https://www.kvraudio.com/forum/viewtopic.php?t=234079), [t=475591 (2016)](https://www.kvraudio.com/forum/viewtopic.php?t=475591), [t=500760 (2018)](https://www.kvraudio.com/forum/viewtopic.php?t=500760). Users: [dubstepforum t=278794 (2013)](https://www.dubstepforum.com/forum/viewtopic.php?t=278794), [KVR t=509757 p. 22 (2018)](https://www.kvraudio.com/forum/viewtopic.php?start=315&t=509757), [KVR t=479074 (2017)](https://www.kvraudio.com/forum/viewtopic.php?t=479074). Tutorials: Attack Magazine, [*Detuned Pad*](https://www.attackmagazine.com/?p=26440) and [*Vintage Arpeggio in Sylenth*](https://www.attackmagazine.com/technique/synth-secrets/vintage-arpeggio-in-sylenth/) (2015); [MusicRadar](https://www.musicradar.com/tuition/tech/how-to-master-sylenth1s-filters-and-amplitude-envelopes-630711) (2015); [ModeAudio](https://modeaudio.com/magazine/programming-a-pad-in-sylenth1) (2016). Every source is rated in the [feature inventory](../reference/sylenth1-features.md#3-filters-pp-5-1618).
- R. A. Moog, "Voltage-controlled electronic music modules", *JAES* 13(3), 1965: summing control voltages before an exponential converter, which is what adding knob units before one exponential law amounts to.
- [filters.md](filters.md) (the cutoff ramp and the grid), [keytracking.md](keytracking.md) (keytrack in octaves, feeding at the segment's end), [filter-stability.md](filter-stability.md) (the DSP's ceilings and F24).
