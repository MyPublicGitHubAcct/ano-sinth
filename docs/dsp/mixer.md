# The mixer: Mix A and B, Main Volume, the meter, and an unclipped output

After its filter and amp envelope, each part of a voice is scaled by its Mix knob. The voices are summed, the master effects run, and Main Volume sets the output level, which a meter shows and nothing clips. This page covers M1.4 task 4, stage 1:

- **Theory:** gain laws and tapers; a per-voice gain against a gain on a bus; zipper noise from a control grid; what peak, RMS, VU and PPM meters measure and how their ballistics are specified; floating-point output and headroom; and handing a meter's value to another thread.
- **Sylenth1:** what is known about its mixer. The manual gives the controls, the meter, and "the output of Sylenth1 is not clipped". The developer gives Mix's internal 0–15 scale, additive modulation in internal units, the v2 engine's inversion at a negative volume, and v3's mute there.
- **The candidates** prototyped here, over the real `dsp::VoiceAllocator`.
- **What they measured:** the prototype against the library; four Mix laws and three clamps against the developer's numbers; zipper and clicks for four placements; V3's two readings; Main Volume's ramp; six meters against reference signals and the standards' ballistics; absolute levels and proof that nothing clips; block-size exactness, allocation and CPU.
- **Recommendations** for the project owner, with the alternatives.
- **The decision** (every recommendation, 2026-11-22) and **as built**: `mixGain` and Mix in `dsp::Voice`, `MainVolume`, `PeakMeter` and `OutputGuard`, with the tests that pin them.

Read [voice-architecture.md](voice-architecture.md) (the voice whose output is mixed), [parameter-smoothing.md](parameter-smoothing.md) (the ramps), [levels-and-pitch.md](levels-and-pitch.md) (decibels) and [filter-control.md](filter-control.md#the-knob-ramps-one-global-ramp-read-at-each-voices-grid) (global ramps read by each voice) first.

## Theory

### Gain laws and tapers

A volume knob maps a position to a gain. The usual laws:

- **Linear amplitude:** gain = position / unity. Halving the knob is −6.02 dB. It is the simplest law, it passes through 0, and its odd extension gives a negative gain: the waveform inverted at the same amplitude. Modulation added to it moves the gain in equal steps.
- **Squared:** gain = (position / unity)². Halving is −12.04 dB, closer to an audio-taper potentiometer. It spends more of the knob's travel on quiet levels.
- **A dB taper** (a mixing-console fader): linear in decibels over most of the travel, with a jump to silence at the bottom. Equal knob steps sound like equal loudness steps. A negative value has no meaning.

The law must say where **unity** is. A knob that goes above unity gives headroom for boosting at the cost of a non-round top.

### Where a gain sits: per voice or on a bus

A part's level can be applied in two places:

- **In each voice**, after its amp envelope. Each voice can then follow its own modulation sources: velocity, its envelopes, its keytrack, its own LFO.
- **On a bus**: each voice writes each part to its own pair of channels, and one gain per part acts on the sum. This takes one multiply per sample per part instead of one per voice. But it can only follow global sources, such as the knob or a free LFO. Velocity can't reach it, because a chord's notes each have their own.

A gain is linear, so the two give the same output for any global gain, up to the order in which the sums are rounded.

### Zipper from a control grid

A voice that computes a gain only every N samples (a control rate) and **holds** it makes a staircase. Each step is a small click, repeating at fs/N, and audible as sidebands at multiples of fs/N around every partial. **Ramping** linearly between the grid points turns the staircase into a polyline. Its error is second-order in the curve's bend over one segment. It is exact for a straight-line ramp, but it trails a corner by up to one segment. **Per sample** is exact by construction. The cost of each is the law's arithmetic per point, against per sample.

### Meters

A meter reduces a signal to one number per channel, refreshed at the display's rate. What that number is depends on the meter:

- **A sample peak meter:** the largest |x|, with an instant attack, usually a hold, then a fall at a fixed rate in dB per second. It shows when a sample exceeds full scale (an *over*). It misses peaks between samples: a sine at fs/4 sampled at 45° reads −3.01 dB below its true peak. ITU-R BS.1770's true-peak meter oversamples ×4 to find them.
- **RMS:** the square root of the mean square over a window (a sliding window of T, or a one-pole with a time constant of T). A sine reads −3.01 dB below its peak and a square 0 dB. AES17 calibrates RMS so that a full-scale sine reads 0 dBFS, by adding 3.01 dB.
- **VU** (IEC 60268-17, from ANSI C16.5): the full-wave rectified signal through a needle whose step response reaches **99% of its final reading in 300 ms ± 10%, overshooting by 1 to 1.5%**. It is an average-responding meter, slow by design, that reads loudness more than level.
- **PPM** (IEC 60268-10): a quasi-peak meter with a fast attack defined by an *integration time*, the burst length that reads 2 dB below the steady tone. Type I (the German DIN scale) has 5 ms and returns 20 dB in 1.7 s ± 0.3. Type II (BBC and EBU) has 10 ms and returns 24 dB in 2.8 s.

A digital meter's ballistics are recursions over samples. Run per sample, with its hold counted in samples, a meter reaches the same state at the same sample whatever the block size. Applied once per block (the block's peak, the fall by the block's length), it depends on where the blocks fall.

### An unclipped floating-point output

A plugin's output is 32-bit float. Values above 1.0 (0 dBFS) pass to the host unchanged, and float has about 1,500 dB of range above the noise of the mantissa, so nothing in a plugin needs to clip. The host's fader, or the converter at the end, is where full scale matters. A synth that "doesn't clip" has no limiter or clamp after its last gain, so its output scales exactly with Main Volume. Two edge cases are worth knowing:

- **Non-finite values** (NaN, ±∞) can come from a bug or an unstable filter. One NaN can disable a host's meters or poison downstream effects for good.
- **Subnormal values**, from decaying recursions, are slow on x86 without flush-to-zero.

A guard that maps both to 0 costs a compare per sample.

### Handing a meter value to the GUI

The audio thread writes the meter, and the GUI thread reads it at about 60 Hz. The handover must neither lock nor tear. A `std::atomic<float>` that is always lock-free compiles to plain loads and stores on every 32-bit-or-wider target with atomic word access. A DSP library can instead only produce the value (a `reading()` call after each block), and let the plugin layer publish it.

## Sylenth1: what is known

The full source table with ratings is in the inventory: [§5 Mixer](../reference/sylenth1-features.md#5-mixer-p-18) (X1–X12), with V3 ([§1](../reference/sylenth1-features.md#voice-structure-and-routing)) and O5 ([§2](../reference/sylenth1-features.md#tuning-volume-pan-and-invert)) annotated. In short:

- **The manual.**
  - p. 18: "Mix A sets the volume of Part A; Mix B sets the volume of Part B. The Main Volume knob sets the volume of Sylenth1's audio output after the master effects have been applied."
  - p. 18: "On the right side of the Mixer panel is a VU meter which measures the output level. Please note that it is in general not a problem when the red LEDs are lighting because the output of Sylenth1 is not clipped. The host software can turn the volume down if needed to prevent clipping."
  - p. 22: Step Velocity to Mix AB, with "the Mix A and B sliders to 0, so that the output volume will be set purely by the Step Velocity".
  - p. 31: Main Volume on CC 7, Mix A on 8, Mix B on 9.
- **The developer on Mix.**
  - In 2010: "Volume AB is the volume level of the oscillator before the filter, while Mix AB is after." For velocity, he advises modulating Mix AB: "Just set the Mix A and B sliders to 0 if you do so." ([p=4282618](https://www.kvraudio.com/forum/viewtopic.php?p=4282618))
  - In 2016 ([t=461365](https://www.kvraudio.com/forum/viewtopic.php?t=461365)):
    - Mix A and B are the part volumes before the effects ("Yes and yes").
    - Mix "internally scale[s] from 0 to +15… displayed as 0 to 10": "set Mix a to 6.667 (which is a real 10 internally)".
    - Modulation is additive in those units. With −10 of keytrack (0.5 at A4), silence comes at Mix 3.333.
    - "Volume A/B are using the correct scale of 0 to 10".
    - "Or you can set Mix to 10 and modulate keytrack with +15 (in 2 mod dest slots)".
    - "A negative volume simply inverts the waveform… It has the exact same amplitude (volume) as the positive wave."
- **The v3 engine.**
  - v3.029 (January 2017): "Negative volume modulation removed (UseV2Engine option added)."
  - The developer in 2026: "In v2 the osc inverses the wave amplitude at negative volume, in v3 it will mute negative volume", and the LFO offset of −10 that "will modulate between -10 and 0 … so indeed the output will be muted" ([t=629612](https://www.kvraudio.com/forum/viewtopic.php?t=629612)).
  - So the inversion quote is v2's. v3 clamps a negative volume at 0.
- **The meter.** v3.0: "Improved responsiveness of VU meter and many other controls", in a rewrite of the user interface. Nothing gives its ballistics.
- **Users.** In 2025–2026, a Logic user reported "split second bursts of noise reaching hundreds of dB", unconfirmed and unanswered.
- **Unknown:** the Mix law and its unity, the init values of Mix and Main Volume, Main Volume's law and range and whether it is a destination, and what the meter shows.

## The candidates

In [mixer_candidates.hpp](../../tests/dsp/voice/mixer_candidates.hpp):

- **`MixVoice`** is a copy of `dsp::Voice` as built (tasks 1–3). Each filter's output, times its part's amp envelope, is multiplied by its part's Mix gain before it is added to the output. With every Mix at unity it is `dsp::Voice` to the bit.
- **`MixEngine`** runs the real `dsp::VoiceAllocator` over 16 `MixVoice`s. Each host block is split at its parameter events. The Mix A/B knob ramps are global `LinearSmoother`s (10 ms) that each voice reads with `valueAfter` at its own samples, as the filter knobs are read. After the voices come the part buses (in that placement), Main Volume, and the meters.
- **The modulation** of Mix stands in for M1.5's. It is either a free LFO, a function of absolute time and the same for every voice, or a *pluck* per note, `amount · e^(−n/τ)` from each note-on, like a mod envelope.

| Question | Candidates |
|---|---|
| **1. The Mix law** (internal m, 0–15) | linear, unity at internal 10 (6.67 shown); linear, unity at the top; squared, unity at 10; a dB fader (−60 dB just above 0, 0 dB at 10, +6 at the top) |
| **1. The clamp of knob + modulation** | [0, 15] (F24); at 0 only (v3's mute), unbounded above; none (v2: odd, a negative value inverts) |
| **2. Where Mix applies** | per voice, at each 32-sample grid point, held; the same, ramped across the segment; per voice, per sample (a constant path while the knob and modulation are still); a part bus with one global gain per part |
| **2. The knob's ramp** | 10 ms linear (the other global knobs'); none |
| **V3. A crossing part** | the filter's part's Mix only, after its envelope (the hypothesis); the crossing part's own Mix too, before the filter, with its envelope |
| **3. Main Volume** | laws: linear, squared, a dB taper; ramp: 10 ms on the gain, or none; a stereo stage at the chain's end |
| **4. The meter** | sample peak (0.5 s hold, Type I's fall of 20 dB in 1.7 s, an over light held 1 s); block peak (the same ballistics once per block, for contrast); RMS over a 300 ms window (a ring and a double sum, re-summed at each wrap); RMS through a 300 ms one-pole; VU (a two-pole needle at ζ 0.8127, for 1.25% overshoot, its corner found for 99% in 300 ms); Type I PPM (a quasi-peak charge found for −2 dB at 5 ms, falling 20 dB in 1.7 s) |
| **5. An output guard** | none; non-finite and subnormal samples to 0 |

**Patches.** *Init* is A1 one saw at Volume 10, LP24 at 4,677 Hz (Cutoff 5 + 5), k 0, gates. The zipper measurements use A1 a sine at A3 (220 Hz) with both filters bypassed: at Volume 10 the filter's saturation adds harmonics above 1 kHz (V8), which would mask the zipper (−32.9 dB through the filter, −91.4 bypassed). *Worst* is 4 × 8 detuned, spread saws at Volume 10, both parts on their own filters, Cutoff 10 + 5. *Pad* is A1, A2 and B1 unison into filter A on AB at k 3.6, with a release.

## What they measured

By the hidden `[mixer-report]` ([mixer_report.cpp](../../tests/dsp/voice/mixer_report.cpp), about 20 s) and the `[mixer]` benchmarks ([mixer_benchmark.cpp](../../tests/dsp/benchmarks/mixer_benchmark.cpp)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[mixer-report]"
tools/bench.sh "[mixer]"
```

### The prototype against the library

`MixEngine`, with every Mix at internal 10 (linear10 gives exactly 1.0), Main Volume 1 and no modulation, against `VoiceAllocator<dsp::Voice>` on the same events (Polyphony 4, two steals), at host blocks of 512 and 37:

| Patch | grid, held | grid, ramped | per sample | part bus |
|---|---|---|---|---|
| init (saw) | identical | identical | identical | identical |
| pad (AB, resonant) | identical | identical | identical | identical |
| A1 and B1 saws, own filters | identical | identical | identical | 7.2 × 10⁻⁷ at a peak of 4.5 |

A gain of exactly 1 multiplied into the envelope changes nothing, so every per-voice placement is the library to the bit. The bus sums all part A voices, then all part B voices, where the library adds each voice's two parts in turn. So with both parts sounding, it differs by float rounding.

### Question 1: the Mix law

Gain in dB at the displayed position (internal = shown × 1.5):

| Law | 0 | 1 | 2.5 | 3.333 | 5 | 6.667 | 7.5 | 10 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| linear, unity at internal 10 (6.67 shown) | silent | −16.48 | −8.52 | −6.02 | −2.50 | 0.00 | +1.02 | +3.52 |
| linear, unity at the top | silent | −20.00 | −12.04 | −9.54 | −6.02 | −3.52 | −2.50 | 0.00 |
| squared, unity at internal 10 | silent | −32.96 | −17.04 | −12.04 | −5.00 | 0.00 | +2.05 | +7.04 |
| dB fader, unity at internal 10 | silent | −51.00 | −37.50 | −30.00 | −15.00 | 0.00 | +1.50 | +6.00 |

The developer's cases (gain, the linear10 law) through each clamp:

| Case | knob + modulation | [0, 15] (F24) | at 0 only (v3) | none (v2) |
|---|---:|---:|---:|---:|
| Mix 3.333 shown, −10 × keytrack 0.5 at A4: "silence" | 0 | 0 | 0 | 0 |
| Mix 0 shown, −10 × 0.5: "a negative volume simply inverts" | −5 | 0 | 0 | −0.500 |
| Mix 10 shown, +15 "in 2 mod dest slots" | 30 | 1.500 | 3.000 | 3.000 |
| Mix 0, Step Velocity 0.5 × +10 (p. 22) | 5 | 0.500 | 0.500 | 0.500 |

- **Every law meets the silence points.** Silence is where the knob plus its modulation reaches 0, and all four laws are 0 there. The developer's numbers (3.333 at −10, 5 at −15) fix the internal units, not the law.
- **The v2 inversion needs an odd law that is symmetric in amplitude.** That is "exact same amplitude… as the positive wave", which a linear (or sign-kept squared) law gives and a dB fader can't. The developer says v3 mutes there instead, which is what [0, 15] and "at 0 only" do.
- **The top of the clamp** decides whether "+15 in 2 slots" on Mix 10 does anything: ×1.5 clamped, ×3.0 not. His suggestion makes sense only if the sum can pass 15 or the keytrack amount is negative. Nothing settles it (X3).
- **Step Velocity to Mix AB "purely by the Step Velocity"** is proportional under both linear laws (half the velocity −6.02 dB), and −12.04 squared or −30 dB through the fader.
- **Mix is after the filter.** At internal 15 against 10, the init saw is ×1.5 sample by sample to a relative 6.0 × 10⁻⁸ (one rounding). The filter's saturation doesn't see Mix, as "Mix AB is after" says.

### Question 2: where Mix applies

The sine at A3, both filters bypassed. Energy above 1 kHz in a Hann 4096, in dB re an instant unit step, averaged over the windows:

- the sweeps at 0.3, 0.5, 0.7 and 0.9 s;
- a jump at its sample (0.5 s + 7, off the grid as host events are);
- the pluck at three note-ons.

*Added* is the same measure on the difference from per sample, which is the error each placement adds.

| Case | grid, held | grid, ramped | per sample | part bus | added: held | added: ramped | added: bus |
|---|---:|---:|---:|---:|---:|---:|---:|
| Mix still (the sine's own floor) | −91.4 | −91.4 | −91.4 | −91.4 | 0 | 0 | 0 |
| automation 15 → 0 over 1 s, an event every 512 samples, ramped 10 ms | −48.6 | −70.0 | −73.3 | −73.3 | −48.7 | −66.8 | 0 |
| the same automation, unramped | −37.0 | −49.8 | −37.0 | −37.0 | −33.6 | −37.2 | 0 |
| a jump 15 → 0, ramped 10 ms | −14.1 | −34.4 | −40.1 | −40.1 | −14.2 | −32.9 | 0 |
| a jump 15 → 0, unramped | −1.9 | −13.8 | −14.9 | −14.9 | −1.2 | −12.8 | 0 |
| a free LFO, 5 Hz, ±5 around 7.5 | −29.0 | −86.2 | −89.9 | −89.9 | −29.0 | −88.7 | 0 |
| a free LFO, 40 Hz, ±5 around 7.5 | −13.3 | −46.6 | −90.1 | −90.1 | −13.3 | −46.6 | 0 |
| a pluck per note, +15 on Mix 0, τ 10 ms | −14.1 | −18.2 | −18.2 | can't | −17.0 | −58.6 | — |

(0: the difference is exactly zero.)

- **Per sample is exact, and the bus equals it** wherever a bus can follow the source. Both leave a 40 Hz LFO at the sine's own floor (−90.1).
- **Held per grid point zips audibly** on anything that moves:
  - −48.7 dB added on a smoothed automation sweep;
  - −14.2 on a ramped jump, a 15-step staircase of 32 samples each;
  - −13.3 on a 40 Hz LFO, sidebands at 1.5 kHz ± 220 Hz and their multiples.

  It also moves a host event up to 31 samples late.
- **Ramped per grid point** follows a straight-line ramp exactly between grid points, but it trails each corner by a segment:
  - −66.8 dB added on the sweep, −32.9 on a ramped jump;
  - −46.6 on a 40 Hz LFO, −58.6 on the pluck.

  It is the filter feed's scheme ([filter-control.md](filter-control.md)), and here it isn't free of error.
- **The bus can't take a per-voice source.** Velocity, a mod envelope, keytrack and a voice's own LFO are per voice, and the developer's own advice routes velocity to Mix AB, so the bus fails that case outright ("can't").
- **The knob's 10 ms ramp** takes an unramped jump's click from −14.9 to −40.1 dB, and an automation sweep's zipper from −37.0 to −73.3. An unramped jump's click depends on where in the sine's cycle it lands: the held grid's −1.9 is the same step 25 samples later, at a larger sample value.

### V3: a crossing part

Filter A on AB, filter B on `--`, B1 a sine at A3 alone, both envelopes gates. Stereo RMS, dBFS:

| Mix A, Mix B (internal) | the filter's part's Mix only (the hypothesis) | the crossing part's own Mix too, before the filter |
|---|---:|---:|
| 10, 10 | −6.08 | −6.08 |
| 10, 0 | −6.08 | silent |
| 0, 10 | silent | silent |
| 10, 15 | −6.08 | −2.62 |

The two readings differ exactly where V3's protocol looks: at Mix B 0, B1 is heard under the hypothesis and silent under the alternative. The alternative also puts Mix before a saturating filter. At Mix B 15 it drives filter A 3.5 dB harder, so Mix would change the filter's distortion, against "Mix AB is after [the filter]".

### Question 3: Main Volume

| Law | 0 | 1 | 2.5 | 5 | 7.5 | 10 |
|---|---:|---:|---:|---:|---:|---:|
| linear | silent | −20.00 | −12.04 | −6.02 | −2.50 | 0.00 |
| squared | silent | −40.00 | −24.08 | −12.04 | −5.00 | 0.00 |
| dB, −60 to 0 | silent | −54.00 | −45.00 | −30.00 | −15.00 | 0.00 |

Zipper and clicks, with the bypassed sine and the gain linear:

| Case | ramped 10 ms | unramped |
|---|---:|---:|
| automation 1 → 0 over 1 s, an event every 512 samples | −76.8 | −40.6 |
| a jump 1 → 0 | −43.7 | −18.5 |

Main Volume is one global gain, so its ramp is all that matters. A 10 ms ramp on the gain, with the block split at its events, takes the zipper to −77 dB and a jump to −44, and it is exact at any block size (below). The law only reshapes the knob, so it belongs in the parameter table (M1.9), as Portamento's does.

### Question 4: the meter

Steady readings after 2 s, in dBFS. The VU and PPM are calibrated so that a steady sine reads its peak; the RMS meters read true RMS:

| Signal | sample peak | RMS, 300 ms window | RMS, 300 ms exponential | VU | PPM Type I |
|---|---:|---:|---:|---:|---:|
| 1 kHz sine, 0 dBFS | 0.00 | −3.01 | −3.02 | −0.01 | −0.16 |
| 1 kHz sine, −20 dBFS | −20.00 | −23.01 | −23.02 | −20.01 | −20.16 |
| 1 kHz square, 0 dBFS | 0.00 | 0.00 | −0.01 | +3.92 | −0.00 |
| 12 kHz sine (fs/4) at 45°, 0 dBFS | −3.01 | −3.01 | −3.02 | +0.91 | −3.01 |
| 100 Hz sine, 0 dBFS | 0.00 | −3.01 | −3.02 | 0.00 | −0.17 |

Tone bursts from silence, the highest reading in dB re the steady tone's (the PPM on 5 kHz, as the standard specifies; the others on 1 kHz):

| Burst | sample peak | RMS window | RMS exponential | VU | PPM Type I |
|---|---:|---:|---:|---:|---:|
| 1 ms | 0.00 | −24.77 | −24.77 | −44.92 | −9.19 |
| 3 ms | 0.00 | −20.00 | −20.02 | −35.38 | −3.61 |
| 5 ms | 0.00 | −17.78 | −17.81 | −30.94 | −2.00 |
| 10 ms | 0.00 | −14.77 | −14.84 | −24.93 | −0.73 |
| 100 ms | 0.00 | −4.77 | −5.47 | −5.56 | 0.00 |
| 300 ms | 0.00 | 0.00 | −1.99 | −0.08 | 0.00 |

- **The VU meets IEC 60268-17**: 99% at 300.0 ms with 1.25% overshoot, from a needle at ζ 0.8127 with a 2.151 Hz corner.
- **The PPM meets Type I**: −2.00 dB at 5 ms by construction, −0.73 at 10 ms and −3.61 at 3 ms (commonly quoted tolerances: −1 ± 0.5 and −4 ± 1). It falls 20 dB in 1.701 s.
- **The sample peak** holds 0.5 s and falls 20 dB in a further 1.7 s. It is the only candidate that sees a single sample over full scale, which is what a red light means.
- **What each meter misses:**
  - A VU reads a square 3.9 dB above a sine of the same peak, and reads a 10 ms burst 25 dB low. It can't show an over.
  - RMS reads a short peak low by 10·log₁₀(burst / 300 ms).
  - Every sample meter misses an inter-sample peak: −3.01 dB at fs/4 and 45°.

The same reading at the same sample, whatever the split: a 2 s render (16 staggered notes of the init saw, released from 0.5 s, so every meter rises and falls) is metered in blocks of 1, 37, 512 and 4096, split also at the reading points. Each reading is compared to the bit against blocks of 4096:

| Meter | identical | largest difference |
|---|---|---:|
| sample peak, hold and fall | yes | 0 |
| block peak (once per block) | no | 0.153 dB |
| RMS, 300 ms window | yes | 0 |
| RMS, 300 ms exponential | yes | 0 |
| VU | yes | 0 |
| PPM Type I | yes | 0 |

The block peak also depends on where a peak falls in its block. A lone full-scale sample read 0.5 s later reads −5.86 to −5.74 dB, depending on its place in a 512-sample block. Every per-sample meter is exact.

**Threading.** `libs/dsp` makes no threading calls. Whether a 4-byte atomic is always lock-free, checked with clang's `__atomic_always_lock_free(4, 0)` for each target triple:

| Target | lock-free |
|---|---|
| arm64 and x86-64 macOS; x86-64 and AArch64 Linux; x86-64 and ARM64 Windows (MSVC ABI) | yes |
| Cortex-M3/M4/M7 (`thumbv7m`, `thumbv7em`), Cortex-M33 (`thumbv8m.main`) | yes |
| Cortex-M0/M0+ (`thumbv6m`) | **no** (no exclusive loads and stores) |

GCC and MSVC agree on the desktop targets: x86-64 and AArch64 have atomic aligned word access. These are clang's answers, not runs of those compilers. So an atomic would compile lock-free wherever M3 and above is the STM32 floor. But it would put a concurrency decision in the DSP library for something only the plugin needs.

### Question 5: levels, and no clipping

Mix at internal 10 (unity), Main Volume 1, the decided voice. Peak and stereo RMS (dBFS) from 0.1 s to 1 s held, then released (R 0.3 s) and rendered to 3 s; non-finite and subnormal samples are counted over the whole render:

| Case | peak | RMS | non-finite | subnormal |
|---|---:|---:|---:|---:|
| init patch, one note (A2) | −3.41 | −8.05 | 0 | 0 |
| init patch, one note, Mix at the top (15) | +0.11 | −4.53 | 0 | 0 |
| init patch, a 16-voice chord | +16.83 | +5.90 | 0 | 0 |
| worst: 4 × 8 saws, both parts, cutoff open, 16 voices, Mix 15 | +34.75 | +22.17 | 0 | 0 |
| worst, drive ×16 (F8's top) | +49.04 | +35.94 | 0 | 0 |
| worst, Resonance 10 on both knobs (k 6) | +20.09 | +6.29 | 0 | 0 |

- **Nothing clips.** At Main Volume ×10 and ×1000, the worst case is exactly ×10 and ×1000 of ×1, sample by sample, to a relative 6.0 × 10⁻⁸ (one rounding), at a peak of +94.8 dBFS. No stage limits the output.
- **The levels are hot at unity.** One init note peaks at −3.4 dBFS, but a 16-voice chord of it peaks at +16.8. Whether that is Sylenth1's level depends on X1, X2, X6 and X11: an init Mix or Main Volume below unity would be headroom. The manual's "it is in general not a problem when the red LEDs are lighting" suggests that hot output is expected.
- **Drive ×16 raises the worst case by 14 dB.** The filter's drive isn't compensated at its output (F8). That is a level for the demo to compare, not a mixer question.
- **No non-finite or subnormal sample in any render**, released tails included. The envelope ends at exactly 0 (E10), and the filter flushes its states and takes a non-finite input as 0 (F25). An output guard changed 0 samples of these renders.

### Non-finite and subnormal output

Nothing upstream produces a non-finite sample from finite parameters: the setters clamp, the filter's solve can't fail (F23), and the decided voice's arithmetic stays inside float's range by hundreds of dB. A guard at the output would only matter for a bug. It costs 0.28 ns per stereo sample, 0.1% of one voice. Its risk is that it hides the bug it catches. `ScopedFlushDenormals` already handles subnormals on every target.

### Room for tasks 5 and 6

Part solo and the skipping of unused parts (V13) are next. With Mix a per-voice gain per part:

- **Solo** is the other part's Mix gain forced to 0. Ramped over the knob's 10 ms, it doesn't click, and the voice is unchanged otherwise.
- **Skipping** can test, per voice and per grid segment, whether a part's Mix (the knob plus that voice's modulation) is 0 for the whole segment. The prototype doesn't skip: A1 and B1 cost 453 ns per sample at Mix B 10 and 460 at Mix B 0, against 347 with B1 at Voices 0. So Mix 0 is the saving task 5 can claim, about 110 ns per voice here.
- **A part bus** would make solo a global gain, but skipping would then need the same per-voice test, and the bus would still lose per-voice modulation.

Task 5's stage 1 measured these ([part-solo-and-skipping.md](part-solo-and-skipping.md)):
- Solo as the other part's gain ramped over 10 ms clicks at −52.7 dB, against −24.0 for a step and −15.0 for a routing change.
- At Mix 0 or under Solo, the saving is part B's filter and slot, 233 ns per voice (473 → 240), not the 110 estimated here, which was measured with B1 off. It comes only if the fed filter is skipped too, with a difference after the part returns (−64 to −70 dB over 50 ms, −39 to −44 resonant).
- The skip test is a bound per factor over each 32-sample segment: Mix's ramp at its larger end plus its modulation's bound.

Built in task 5's stage 2 (2026-11-22, [as built](part-solo-and-skipping.md#as-built)): Solo's gains (`PartSolo`, through `VoiceKnobs::solo`) are multiplied into each part's Mix gain per sample, and a part whose Mix bound is 0 over a segment has its output stage skipped and its fed filter frozen. So at Mix 0 the voice is no longer this page's prototype to the bit after the part returns; [mix_test.cpp](../../tests/dsp/voice/mix_test.cpp) now keeps Mix above 0, and Mix at and below 0 is tested against task 5's prototype.

### Real-time terms

A 2 s sequence: the pad at Polyphony 4 with steals, Mix A and B automated every 300 samples at odd offsets, and two Main Volume jumps, rendered at host blocks of 1, 37 and 4096:

| Placement | no modulation | LFO 40 Hz ±5 | pluck +10, τ 20 ms | allocations |
|---|---|---|---|---:|
| per voice, grid, held | identical | identical | identical | 0 |
| per voice, grid, ramped | identical | identical | identical | 0 |
| per voice, per sample | identical | identical | identical | 0 |
| part bus | identical | identical | identical | 0 |

Every placement is identical to the bit (12 of 12) with 0 allocations while processing. The knob ramps are global and read at absolute offsets, the LFO is a function of absolute time, the pluck counts from each note-on, and Main Volume steps per sample between events. The prototype engine allocates its bus scratch and the RMS window's ring in `prepare()`. A library version would fix the bus at the maximum block size, and the ring at the sample rate's worst case (14,400 doubles per channel at 48 kHz, 28,800 at 96 kHz: 115–230 KB per channel), which matters for STM32.

### CPU

`tools/bench.sh "[mixer]"`, `relwithdebinfo`, Apple M1 Ultra (virtualised), 512 samples per run at 48 kHz, the median of three runs, ns per sample:

| Benchmark | Mix still | Mix ramping | free LFO 40 Hz |
|---|---:|---:|---:|
| `dsp::Voice`, init | 349.5 | | |
| `MixVoice`, grid, held | 345.0 | 344.7 | 350.3 |
| `MixVoice`, grid, ramped | 346.2 | 346.6 | 362.9 |
| `MixVoice`, per sample | 347.5 | 348.4 | 362.7 |
| `MixVoice`, part bus (the voice only) | 363.7 | | |

| Benchmark | init | worst |
|---|---:|---:|
| 16 voices, `VoiceAllocator<dsp::Voice>` | 5,584 | 10,709 |
| 16 voices, engine, per sample | 5,621 | 10,653 |
| 16 voices, engine, part bus | 5,588 | 10,621 |

| Global stage (both channels) | ns per sample |
|---|---:|
| Main Volume ramping / settled | 0.94 / 0.10 |
| meter: sample peak / block peak | 1.61 / 2.45 |
| meter: RMS window / RMS exponential | 8.50 / 4.57 |
| meter: VU / PPM | 8.36 / 5.34 |
| output guard | 0.28 |

- **Mix per voice costs nothing measurable** while the knob is still or ramping: 347.5 and 348.4 against `dsp::Voice`'s 349.5. That is a multiply into the envelope, plus the law where the gain moves. Sixteen voices through the engine are within noise of the library (±1%).
- **With the LFO, per sample costs 3.8% more** (362.7). The prototype computes the LFO's `sin` and the law every sample for every voice; M1.5 computes its LFOs anyway, so the mixer's own share is the add and the law. Held per grid point is cheaper (350.3), but it is the candidate that zips.
- **The bus saves nothing measurable** at 16 voices (5,588 against 5,621, within noise), and its one-voice row is noisy (345–383).
- **The global stages are about 1% of one voice.** The RMS window and the VU (8.4–8.5 ns) cost five times the sample peak.
- **Machine noise:** `dsp::Voice` measured 349.5 here against 367.6 in the previous session, so compare within a table.

## Recommendations

*Stage 1's recommendations, for the project owner to choose from. Where only the demo can settle a question, the recommendation is the working hypothesis in the inventory (X1–X12), and the measurement is what makes it the safer default. Chosen by the project owner on 2026-11-22, every one ([The decision](#the-decision), [As built](#as-built)).*

| # | Question | Recommendation | Why | Alternatives | Settled by |
|---|---|---|---|---|---|
| 1 | **Mix's law** | **Linear in internal units, unity at internal 10 (6.67 shown), ×1.5 (+3.52 dB) at 10 shown.** The knob's internal value (shown × 1.5) is what modulation adds to. | The developer's v2 inversion ("the exact same amplitude") needs an odd, amplitude-symmetric law, which linear is; "Volume A/B are using the correct scale of 0 to 10" puts Mix's internal 10 where Volume's 10 (unity, O4) is; Step Velocity sets the level "purely", proportionally. | Unity at the top (−3.52 dB everywhere); squared; a dB fader. Each is one function in the parameter table. | X1 |
| 2 | **Mix's clamp** | **[0, 15], before the law, per part** (F24, as the Filter Control's laws do). | v3 "will mute negative volume" (the developer, 2026; v3.029). The top is the knob's range until X3 says otherwise. | At 0 only (if "+15 in 2 slots" passes 15 on the demo); none, for a v2-engine option. | X3 |
| 3 | **Where Mix applies** | **Per voice, per sample**, after the part's amp envelope: the gain multiplied into the envelope, a constant per chunk while the knob and the voice's modulation are still. The Mix A/B knobs are global `LinearSmoother`s over 10 ms, read by each voice with `valueAfter` (as the filter knobs, through `VoiceKnobs`). | Exact (no added zipper), the library to the bit at unity, bit-exact at any block size, no measurable cost (347.5–348.4 against 349.5 ns). The developer routes velocity to Mix AB, which only a per-voice gain can follow. | Ramped per grid point (−46.6 dB added on a 40 Hz LFO, −58.6 on a pluck; the same cost). A part bus (exact for global sources; can't take per-voice ones). Held per grid point (−13 dB: no). | X4, X5 |
| 4 | **V3, a crossing part** | **The filter's part's Mix only**, after its envelope, as hypothesised. | It is what "Mix AB is after [the filter]" and the manual's path give; the alternative silences B1 at Mix B 0 and moves the filter's drive with Mix. | The crossing part's Mix too, before the filter. | V3 |
| 5 | **Main Volume** | **A global stereo gain at the end of the chain (after the effects, M1.8), a `LinearSmoother` on the linear gain over 10 ms, the block split at its events.** The knob's law in the parameter table: placeholder linear, 0–10 → 0–1, unity at 10. A destination only if M1.5's list has it. | −76.8 dB of zipper on automation and −43.7 for a jump, against −40.6 and −18.5 unramped; exact at any block size; 0.94 ns per sample while ramping. | Unramped; a squared or dB knob law. | X6, X7, X8 |
| 6 | **The meter** | **A sample peak per channel, after Main Volume: instant attack, a 0.5 s hold, a fall of 20 dB in 1.7 s, and an over flag held 1 s after any sample above 1.0**, run per sample. `libs/dsp` produces the values (a `reading()` and `over()` per channel after each block); the plugin publishes them to the GUI (its own `std::atomic<float>`, lock-free on every desktop target). | "Red LEDs" and "not clipped" are about samples over full scale, which only a peak meter shows; it is the cheapest (1.6 ns), exact at any split, and needs no memory. Keeping atomics out of `libs/dsp` keeps it free of concurrency, and Cortex-M0 has no lock-free atomic. | A VU (IEC 60268-17, as built here: exact, 8.4 ns) or a Type I PPM (5.3 ns) if X9 finds slow ballistics; RMS (the window needs 115 KB per channel at 48 kHz). A block peak (not exact: 0.15 dB). An atomic in `libs/dsp` (fine on M3 and above). | X9 |
| 7 | **No clipping** | **Nothing after the voices limits or clips; the output is float, as loud as the patch makes it.** A test pins it: the output scales exactly with Main Volume to ×1000. | The manual; measured: one rounding at +94.8 dBFS. | A safety limiter (not parity). | X10 |
| 8 | **An output guard** | **None in the voice path.** In M1.8's engine, a non-finite check at the very end that zeroes the sample and counts it, for tests to require 0. No subnormal flush there: `ScopedFlushDenormals` covers it. | No render produced a non-finite or subnormal sample; the guard costs 0.28 ns per sample. A NaN reaching the host is worse than any cost (the 2025–2026 reports), and a counter keeps it from hiding a bug. | No guard; a guard with the subnormal flush as well. | X12 |
| 9 | **Levels** | **Record and keep the unity chain** (Volume 10, Mix internal 10, Main Volume 1). Make no level change before X1, X2, X6 and X11 are measured. | Its init note peaks at −3.41 dBFS, but a 16-voice chord at +16.8 and the worst case at +34.8. Only the demo can say whether Sylenth1's init settings leave more headroom. | A fixed output trim now (a guess). | X11, O5 |

### What the voice and the engine need

- **The voice.** `VoiceKnobs` gains the Mix A/B ramps (two `const LinearSmoother*`, read like the filter knobs at `offset`). `dsp::Voice` multiplies each filter's envelope by its part's Mix gain per sample at step 5, through the law and the [0, 15] clamp, with the modulation term 0 until M1.5. It takes a constant path per chunk while the gain is still, and is identical to the bit at unity. The law (linear10) is a `constexpr` function beside `clampToKnob`. The prototype also tracks samples since the note-on for its pluck; M1.5's envelopes will own that.
- **The engine (M1.8).** Main Volume as a stereo gain stage after the effects, split at its events. The meter after it. The non-finite guard and its counter at the very end.
- **Interop (M1.9).** The knob laws (Mix's shown × 1.5, Main Volume's placeholder), the Mix and Main Volume ramps as global `LinearSmoother`s, and the meter's hand-off to the GUI.

### What this pushes back on

- **F24 stands for Mix.** The inversion quote describes v2, and v3 mutes a negative volume, which is the clamp at 0. The clamp's top for Mix is still open (X3).
- **V3 stands.** It is measured and consistent with the developer's "after the filter".
- **O5 and V9:** the absolute chain now has numbers (init peak −3.41 dBFS at unity), and it waits on the init values (X2, X6).
- **F8:** drive ×16 raises the output by 14 dB at the worst case. Whether Sylenth1's drive compensates is a demo comparison for the filter.
- **`dsp::Voice`'s "Mix A/B … and the voice's output bus are the mixer's":** recommendation 3 keeps the voice's stereo output and adds the gain inside, so there is no bus.

## The decision

Chosen by the project owner on 2026-11-22: every recommendation (Decisions log).

- **Mix's law:** linear in internal units, unity at internal 10 (6.67 shown), ×1.5 at the top. The knob plus its modulation is clamped to [0, 15] before the law.
- **Where Mix applies:** in each voice, per sample, after the part's amp envelope. The Mix A/B knobs are global 10 ms ramps, read through `VoiceKnobs`. A part crossing into the other filter takes that filter's part's Mix only (V3).
- **Main Volume:** a global gain on every channel at the end of the chain, ramped over 10 ms on the gain, with the block split at its events. Its knob law is the parameter table's.
- **The meter:** a sample peak per channel after Main Volume, with a 0.5 s hold, a fall of 20 dB in 1.7 s and an over flag held 1 s. `libs/dsp` produces the values; the plugin publishes them.
- **No clipping, and the guard:** nothing limits the output. A counted non-finite guard sits at M1.8's output, and there is none in the voice path.
- **Levels:** the unity chain stays until the demo is measured.

## As built

### The law

[mix_law.hpp](../../libs/dsp/include/anoesis/dsp/voice/mix_law.hpp): `MixLaw` holds the law's numbers (`knobMin` 0, `knobMax` 15, `unity` 10); `mixLaw` is the decided instance. `MixTerms` holds the knob and its modulation, in internal units. `mixUnits()` adds them and clamps the sum to the knob's range, with NaN counting as the bottom. `mixGain()` is `mixUnits / unity`, a true division, so every quarter unit is exact to one rounding and unity is exactly 1. `mixInternalPerShown` (1.5) is the knob's display factor, for the parameter table. These are pure `constexpr` functions, beside the Filter Control's.

### The voice

[voice.hpp](../../libs/dsp/include/anoesis/dsp/voice/voice.hpp):

- `VoiceKnobs` gains `mix`: Mix A and Mix B's ramps in internal units, read at `offset` like the filter knobs. **A null ramp is unity**, so every caller without a mixer hears the voice as before, to the bit (tested).
- In each chunk, after the envelopes, `fillMixGains()` writes each sounding filter's part's gain for every sample: the ramp read with `valueAfter` at that sample, through `mixGain`. A linear ramp is monotonic, so when it reads the same at both ends of the chunk, the gain is computed once.
- At step 5, the gain is multiplied into the envelope (`g · e`, exactly `e` at unity), then into the filter's output. A crossing part is already inside the filter's output, so it takes that filter's part's Mix only.
- The modulation term is 0 until M1.5 supplies the voice's Mix A / Mix B / Mix AB modulation.
- One more 2 × 32-float buffer per voice (256 bytes); nothing allocates.

### The output stages

In `libs/dsp/include/anoesis/dsp/output/`, for M1.8's engine to chain after the effects:

- **[main_volume.hpp](../../libs/dsp/include/anoesis/dsp/output/main_volume.hpp), `MainVolume`:** one gain on every channel, in place. A `LinearSmoother` ramps the gain over 10 ms from each `setGain()`, and `setGainImmediately()` jumps (for a preset load). A gain that isn't finite and at least 0 counts as 0. Settled, it is one multiply per sample; ramping, one ramp step per sample shared by the channels. The caller splits its block at Main Volume events. It satisfies `BlockProcessor`.
- **[peak_meter.hpp](../../libs/dsp/include/anoesis/dsp/output/peak_meter.hpp), `PeakMeter` and `StereoPeakMeter`:** the decided ballistics in `PeakMeterSettings` (hold 0.5 s, fall 20/1.7 dB per second, over 1 s), converted to samples at `prepare()`, so they are seconds at any rate. It runs per sample, with `reading()` and `over()` per channel after each block. Below −200 dB the reading is 0, so the fall never goes subnormal. It is the stage-1 prototype's meter to the bit. Handing its values to the GUI is the plugin's job (M1.9, M1.11), through its own `std::atomic<float>`.
- **[output_guard.hpp](../../libs/dsp/include/anoesis/dsp/output/output_guard.hpp), `OutputGuard`:** a non-finite sample becomes 0 and is counted, per block (`process()`'s return) and in total (`count()`). Finite samples, subnormals included, pass unchanged. It satisfies `BlockProcessor`.

### Tests

[mix_test.cpp](../../tests/dsp/voice/mix_test.cpp) and [output_stage_test.cpp](../../tests/dsp/output/output_stage_test.cpp), tagged `[mixer]`:

- **The law:**
  - exactly `units / 10` at every quarter unit from 0 to 15;
  - unity at internal 10 (a `static_assert`) and at 6.667 shown;
  - −6.02 dB at half;
  - the clamp: the developer's silence point, a negative sum silent, the top held, NaN and −∞ silent, +∞ at the top;
  - the law's numbers as parameters.
- **The voice:**
  - **bit for bit the stage-1 prototype** (`MixEngine` in its decided configuration) through `VoiceAllocator`, with `MainVolume` after it, at host blocks of 1, 37 and 4096, with 0 allocations. The setup: two patches (a resonant pad on AB, and A1 and B1 on their own filters), steals at Polyphony 4, Mix A and B automated every 300 samples (past both ends of the range; since task 5, past the top only, as above), and Main Volume jumps;
  - a null Mix ramp is the library without a mixer, to the bit;
  - each part's Mix scales only its own filter's output, exactly (×1.5 and ×0.5 to one rounding, so after the filter);
  - V3: a crossing B1 is untouched by Mix B and silenced by Mix A;
  - the ramp is read at every sample: a jump at an off-grid sample follows the 10 ms ramp to one rounding, silent from 480 samples on.
- **`MainVolume`:**
  - the gains are a `LinearSmoother`'s over 10 ms, on three channels, landing exactly;
  - the output is the input times the gain to the bit, at ×0.25 to ×1000 (nothing clips);
  - the same output at blocks of 1, 37, 512 and 4096 with gain events;
  - NaN, negative and infinite gains count as 0;
  - the lifecycle contract, no allocation, and acting at all.
- **`PeakMeter`:**
  - an instant attack, a 0.5 s hold, and 20 dB in 1.7 s, within 174 samples. The tolerance is derived: 81,600 float multiplies, each rounded by 2⁻²⁴;
  - a peak equal to the held level restarts the hold;
  - the over flag: not at 1.0, at 1.0001, held exactly 1 s; settings of its own;
  - the same reading and flag at blocks of 1, 37, 512 and 4096, per channel, and **the stage-1 prototype's meter to the bit**;
  - hold and fall in seconds at 96 kHz; `reset()`; no allocation.
- **`OutputGuard`:** NaN and ±∞ zeroed and counted, per block and in total; huge, subnormal and signed-zero samples untouched; a clean block unchanged; `reset()`; no allocation.

**Red, then deliberate breaks.** Against inert headers (the law returning 1, the voice ignoring Mix, the stages doing nothing), 15 of 17 failed. Two passed:

- the null-ramp test, rightly: it checks that a caller without a mixer is unchanged;
- `MainVolume`'s lifecycle test, because an inert gain changes nothing. It was strengthened to require the gain to act.

Then 24 deliberate breaks:

- **the law:** no clamp at the bottom (v2's inversion), none at the top, NaN passing, ×0.1 instead of ÷10, unity at the top;
- **the voice:** the gain held from the chunk's start, the ramp read without the knobs' offset, a null ramp silent, the other part's Mix, Mix ignored, the ramp read a sample ahead, Mix applied twice;
- **`MainVolume`:** no ramp, negative gains, two channels only, the ramp skipped;
- **the meter:** a strict attack, the over at full scale, the fall at 10 dB per decade, the hold counted at 48 kHz, `reset()` keeping the level;
- **the guard:** subnormals zeroed, the count per block only, `reset()` keeping the count.

All 24 are caught. One is caught at compile time, by the `static_assert`. One survived until it led to a stronger test: a strict attack, so that a peak equal to the held level doesn't restart the hold. It is caught by the new equal-peak test.

### Results, as built

`tools/bench.sh "[mixer]"` (the as-built rows), `relwithdebinfo`, Apple M1 Ultra (virtualised), the median of three runs, ns per sample:

| Benchmark | ns per sample |
|---|---:|
| `dsp::Voice`, init, Mix still / ramping | 359.9 / 360.8 |
| 16 voices, `VoiceAllocator<dsp::Voice>` without Mix ramps / with Mix, `MainVolume`, `StereoPeakMeter`, `OutputGuard` | 5,803.8 / 5,822.5 |
| `MainVolume` (stereo), ramping / settled, each including a 0.10 ns block copy | 1.73 / 0.21 |
| `StereoPeakMeter` | 1.66 |
| `OutputGuard` (stereo) | 0.43 |

- **Mix costs nothing measurable in the voice.** A ramping Mix is 0.3% over a still one, and the whole output chain on 16 voices is 0.3% over the voices alone, within noise. This session's `dsp::Voice` read 359.9 against stage 1's 349.5 in another session. The machine's spread between sessions is larger than that (349.5 to 367.6 for the same code).
- **The global stages together cost about 2.3 ns per sample while Main Volume ramps, and 0.6 settled.** That is under 0.7% of one voice.

`dsp::Voice` grows by 256 bytes (the Mix gains); `MainVolume` is a `LinearSmoother` (about 40 bytes), `StereoPeakMeter` two sets of a few counters, and `OutputGuard` one counter. Nothing allocates, so the RMS window's memory question (stage 1) doesn't arise.

## References

- IEC 60268-17:1990, *Sound system equipment, Part 17: Standard volume indicators* (the VU meter's ballistics).
- IEC 60268-10:1991, *Sound system equipment, Part 10: Peak programme level meters* (Types I and II).
- ITU-R BS.1770-5 (2023), *Algorithms to measure audio programme loudness and true-peak audio level*, Annex 2 (true peak).
- AES17-2020, *AES standard method for digital audio engineering: Measurement of digital audio equipment* (dBFS for a full-scale sine).
- Eddy Brixen, *Audio Metering: Measurements, Standards and Practice* (3rd ed., 2020).
- Andrew Simper, "Linear Trapezoidal Integrated State Variable Filter" (Cytomic technical paper, 2013): the VU's needle.
- cppreference, `std::atomic<T>::is_always_lock_free`; ARM, *ARMv6-M Architecture Reference Manual* (no LDREX/STREX).
- Sylenth1 sources: the inventory's [§5](../reference/sylenth1-features.md#5-mixer-p-18).
