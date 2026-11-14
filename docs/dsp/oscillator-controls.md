# Oscillator controls: tuning, volume, pan, and invert

Each of Sylenth1's four oscillators has coarse and fine tuning, a volume, a pan, and an invert switch (inventory [§2](../reference/sylenth1-features.md#tuning-volume-pan-and-invert)). This page covers the layer that adds them to the band-limited oscillator: how a pitch is computed and how exact it is, the volume taper, the pan law and its alternatives, how the gains are smoothed, and how invert gives Sylenth1's two-saw PWM trick. Read [band-limited-oscillators.md](band-limited-oscillators.md) and [oscillator-waveforms.md](oscillator-waveforms.md) first.

## Theory

### Pitch as one fractional note

In 12-tone equal temperament a semitone is the ratio 2^(1/12), so adding intervals in semitones multiplies frequency ratios ([levels-and-pitch.md](levels-and-pitch.md)). An oscillator's pitch is a sum of offsets in semitones:

  n = key + 12 · octave + note offset + fine (+ bend, glide, pitch modulation, unison detune, drift)

and the frequency is one conversion of that sum: f = f_A4 · 2^((n − 69)/12), where f_A4 is the master tune. Working in the note domain has three advantages. Every pitch source adds in the same unit, whatever produced it. Whole octaves stay exact, because 2^k for whole k is exact in floating point. And there is one exponential per pitch change rather than one per source.

Sylenth1's controls (inventory O1–O3): Octave −3 to +3 and Note −7 to +7, in whole steps, and Fine ±1 semitone, continuous. Together they reach ±44 semitones.

### How exact the pitch can be

Three things limit the pitch the oscillator actually plays:

- **Float arithmetic.** `noteToFrequency` is accurate to 1·10⁻⁶ relative (its test derives this), which is 0.0017 cents. Rounding n to float adds up to half an ulp of a value up to 171: 7.6·10⁻⁶ semitones, 4.4·10⁻⁷ relative. Converting f/fs to float adds 6·10⁻⁸.
- **The fixed-point phase.** The phase increment is a 32-bit integer, 2³² per cycle, so it resolves fs/2³² Hz: 1.03·10⁻⁵ Hz at 44.1 kHz and 1.12·10⁻⁵ Hz at 48 kHz. `incrementFor` truncates, so a pitch can be up to one unit flat. In cents that is 1200 · log₂(1 + (fs/2³²)/f), which depends on the pitch:

  | Pitch | Floor at 44.1 kHz | Floor at 48 kHz |
  |---:|---:|---:|
  | 1 kHz | 1.8·10⁻⁵ cents | 1.9·10⁻⁵ cents |
  | 20 Hz | 8.9·10⁻⁴ | 9.7·10⁻⁴ |
  | 1 Hz | 0.018 | 0.019 |
  | 0.01 Hz | 1.8 | 1.9 |

  Above about 200 Hz the float arithmetic dominates; below 20 Hz the fixed point does. The unit grows with the rate: at 0.01 Hz the floor is 3.87 cents at 96 kHz and 7.72 at 192 kHz, measured over whole cycles in [oscillator-range.md](oscillator-range.md#the-bottom-of-the-range).
- **Hearing.** Listeners just distinguish about 5 cents for pure tones in the midrange, so even 0.03 cents at 0.6 Hz (a sub-audio LFO rate) is 150 times finer than needed.

### Volume taper

A knob's position has to be mapped to a gain. Common tapers:

| Taper | Gain at half the knob | Character |
|---|---:|---|
| Linear, g = x | 0.5, −6.0 dB | Half the travel covers only the top 6 dB; the bottom fifth covers everything below −14 dB. |
| Power, g = x² (or x³) | 0.25, −12.0 dB (x³: −18 dB) | Closer to equal loudness steps across the travel; still reaches true silence at 0. |
| Decibels, g = 10^(dB/20), dB linear in x | for example −24 dB on a 48 dB range | Equal steps in dB, but needs a floor (−∞ cannot be a knob position). |

No source states Sylenth1's taper (inventory O4). The working hypothesis is **linear**: the only Sylenth1-specific hint is a v1.20 changelog line, "Knob behavior changed to linear", which may refer to the mouse rather than the gain, and linear is the simplest to compare against a measurement. At knob 5, the candidates measure −6.02 dB (linear), −12.04 dB (x²), or a point on a straight line in dB.

### Pan laws

A pan control splits a mono signal between two speakers with gains L(p) and R(p), for a position p from −1 (left) to +1 (right). The laws differ in what they keep constant.

- **Equal power (sin/cos), −3 dB at the centre.** L = cos θ, R = sin θ, with θ = (p + 1)π/4. Then L² + R² = 1 at every position. Two speakers in a room add roughly in *power* (their sounds arrive at a listener with different delays, so they do not add coherently), so constant power means constant loudness as the source moves. At the centre each side is √½ = −3.01 dB. This is the usual law on mixing consoles and in DAWs.
- **Linear (constant amplitude), −6 dB at the centre.** L = (1 − p)/2, R = (1 + p)/2, so L + R = 1. Signals that add *coherently* (a mono fold-down, or two channels summed electrically) keep a constant level. In stereo, a centred source is 3 dB quieter than a hard-panned one.
- **−4.5 dB at the centre:** a compromise between the two, the geometric mean of their gains, used by some consoles.
- **Balance (0 dB at the centre).** L = min(1, 1 − p), R = min(1, 1 + p): the centre is unity and moving away only fades the far side, as a stereo balance control does. A centred source is 3 dB louder than under equal power.
- **Square-root law.** L = √((1 − p)/2), R = √((1 + p)/2) is also constant power and −3 dB at the centre, but its curve differs in between: at p = −½ it gives 0.866 / 0.500, against sin/cos's 0.924 / 0.383. That point tells the two apart in a measurement (inventory O6).

**What each law does in mono.** Summed to mono (L + R), an equal-power pan is +3 dB louder at the centre than at either side. A linear pan is flat in mono. A balance pan is +6 dB louder at the centre. So no law is constant both in stereo and in mono: equal power favours stereo listening, linear favours mono compatibility.

Sylenth1's law is unspecified (inventory O6). The working hypothesis is **equal power, sin/cos, with unity at the hard sides**: the most common choice, and the one that keeps an oscillator's loudness steady as it is panned or as the pan is modulated (Pan AB is a modulation destination).

### Smoothing gains

A gain applied as a staircase multiplies the signal by a step function, which clicks ([parameter-smoothing.md](parameter-smoothing.md)). Ramping it over about 10 ms turns each click into a short fade. Pitch is different. The oscillator's phase is continuous, so a pitch step changes only the phase's slope: the waveform itself does not jump. A MIDI-CC step of Fine (2 semitones / 127 = 1.6 cents) is below the 5-cent threshold of discrimination, and Octave and Note are meant to jump.

### Invert, and PWM from two saws

Inverting a signal negates every sample: y = −x. The manual (p. 15) uses it for PWM: add a saw and an inverted saw, and the phase between them sets a pulse width. For a rising saw s(t) = 2t − 1 (phase t in cycles) and a phase offset d:

- for t ∈ [0, 1 − d): s(t) − s(t + d) = (2t − 1) − (2t + 2d − 1) = −2d;
- for t ∈ [1 − d, 1): the second saw has wrapped, so s(t) − s(t + d) = 2 − 2d.

That is a pulse of width d (high for the last d of each cycle) with a 2-unit swing, and its mean is (2 − 2d)·d − 2d·(1 − d) = 0 for every width. A ±1 pulse of variable width, by contrast, has a mean of 2d − 1. This is the developer's point (KVR thread 301405) that the two-saw sum has no DC.

## How it is applied here

### The DSP takes physical units; the parameter table maps the knobs

| Control | DSP input | Sylenth1 knob mapping (parameter table, M1.9) | Inventory |
|---|---|---|---|
| Octave | `Tuning::octaves`, a whole number | −3 to +3 | O1 |
| Note | `Tuning::semitones`, a whole number | −7 to +7 | O2 |
| Fine | `Tuning::fine`, semitones | −1 to +1, linear (LCD value = semitones) | O3 |
| Master tune (v3) | reference Hz for A4 | units open | O9 |
| Volume | linear gain | knob 0–10 → knob / 10 | O4, O5 |
| Pan | position, −1 to +1 | knob 0–10 → knob / 5 − 1 | O6 |
| Inv | bool | switch | O7 |

The library enforces no Sylenth1 ranges, so it stays reusable, and the open questions in the inventory change a mapping in `plugin/`, not DSP code.

### The pieces

- **`Tuning`** ([pitch.hpp](../../libs/dsp/include/anoesis/dsp/math/pitch.hpp)): octaves, semitones, and fine, with `offset()` = 12 · octaves + semitones + fine. The coarse part is summed as integers, so the only rounding is adding fine.
- **`panGains(position, law)`** ([pan.hpp](../../libs/dsp/include/anoesis/dsp/math/pan.hpp)): the equal-power, linear, and balance laws. Positions are clamped, and NaN counts as the centre. The equal-power gains are written as sines of each side's distance, sin((1 − p)π/4) and sin((1 + p)π/4), rather than cos and sin of one angle. Then a hard side gives sin(0) = 0 exactly (cos(π/2) in float is −4.4·10⁻⁸), and the left gain at p is bit-for-bit the right gain at −p.
- **`StereoGain`** ([stereo_gain.hpp](../../libs/dsp/include/anoesis/dsp/control/stereo_gain.hpp)): adds a mono signal into a stereo block. Volume, pan, and polarity fold into one gain per channel, sign × volume × pan gain, and each channel gain has its own `LinearSmoother`. So the pan law's `sin` runs only when a control changes. When both ramps are settled, a sample costs one multiply-add per channel, with the gains held in locals.
- **`OscillatorSlot<Taps>`** ([oscillator_slot.hpp](../../libs/dsp/include/anoesis/dsp/oscillators/oscillator_slot.hpp)): a `MinBlepOscillator` with its `Tuning`, its master tune, and a `StereoGain`. With unison, up to 8 of each, one per voice, plus a noise source ([oscillator-unison.md](oscillator-unison.md)); with the default of 1 voice it is exactly the slot described here. `process(out, note)` converts the pitch once per block; `process(out, notes)` takes a note per sample and sets the increment every sample, exactly. The oscillator's mono output is staged on the stack in chunks of 64 samples, then panned and **added** into channels 0 and 1 of the voice's bus. Nothing is allocated, and the shared minBLEP table is the only memory outside the object.
- **`MinBlepOscillator::phaseIncrement()`**: the increment actually in use, so tests and reports can check the exact pitch without measuring a signal.

### Why a slot, and not the oscillator or the voice

- **Not in `MinBlepOscillator`.** The oscillator stays a mono, single-voice band-limited source. Putting volume and pan inside it would make every unison voice stereo and repeat the gain work 8 times per oscillator.
- **Not in the voice.** The controls belong to each of the four oscillators, and unison detune, stereo spread, and drift (later M1.1 tasks) sit between an oscillator's tuning and its pan.
- **The slot is what unison extended.** It now holds up to 8 oscillators, each with its own `StereoGain`, plus one `WhiteNoise` for the Noise waveform ([oscillator-unison.md](oscillator-unison.md), [oscillator-waveforms.md](oscillator-waveforms.md#noise-lives-alongside-the-oscillator-not-in-it)).

The design leaves the later work open:

- **Unison** (built; [oscillator-unison.md](oscillator-unison.md)). Each unison voice's pitch is the slot's pitch plus its detune (and later drift), applied as a frequency ratio computed when the detune changes. Each voice has its own pair of stereo gains, computed when a control changes. Pan combines with Stereo as an offset, clipped at the sides (inventory O8, a working hypothesis), which is one function, `unisonPosition`.
- **Per-voice pitch modulation.** The voice passes a note per sample (key, bend, portamento, Pitch A modulation); the slot adds its tuning.
- **Part 2: FM and hard sync.** FM between oscillators is a per-sample input of the same shape as the notes (an increment or phase offset). Hard sync is a list of sync events (sample index and fraction) from the master slot, applied by `MinBlepOscillator::jumpTo(phase, since)`, the band-limited phase jump that retrigger already uses ([oscillator-phase.md](oscillator-phase.md)). Block processing does not get in the way, because each event carries its position within the block.

### Smoothing: which smoother and ramp, and why

`StereoGain` ramps each channel gain with a **`LinearSmoother`, 10 ms** (441 samples at 44.1 kHz, 480 at 48 kHz, 960 at 96 kHz; it is stored in seconds, so the time holds at every rate).

- **10 ms** is the time [parameter-smoothing.md](parameter-smoothing.md) gives for turning a gain step's click into a fade, short enough that the knob still feels immediate.
- **Linear rather than exponential.** The ramp lands exactly, so `isSmoothing()` lets the slot drop to the constant-gain loop. It costs 0.80 ns per sample against the exponential's 3.72 ns (M1.0 baselines). And the exponential's fast start is the step that smoothing is meant to soften.
- **Smoothing the channel gains, not the knobs.** Smoothing the pan position and evaluating sin/cos per sample would follow the law's curve exactly mid-ramp, but would cost two `sin` calls per sample (about 8 ns) on every ramping sample. Interpolating the gains linearly instead follows a chord of the curve. Short moves stay within a fraction of a dB, and the worst case, a full swing from hard left to hard right, dips 3 dB at its midpoint for a few milliseconds.
- **Invert is the gain's sign.** Once settled, the output is the exact negation (negating a float is exact), and toggling Inv during a note crossfades through zero over 10 ms instead of clicking. On a real switch an instant flip would click on any non-zero sample.
- **A new note snaps.** `reset()` finishes any ramp, so a note starts at its gains with no fade-in. The amplitude envelope (M1.2) shapes the attack.
- **Pitch is not smoothed** (see the theory above).

## Trade-offs and alternatives

- **Tuning, volume, and pan in the oscillator.** Simplest to write, but it would make the mono source stereo and repeat the gain work in every unison voice. Rejected.
- **A per-sample pan law.** Exact mid-ramp, but two `sin` calls per ramping sample, against a chord of the curve that is within a fraction of a dB for ordinary moves. Rejected.
- **Instant invert.** Probably what Sylenth1 does, being a switch, but it clicks. The crossfade costs nothing once settled.
- **Rounding the increment rather than truncating it** would halve the fixed-point floor. Truncation keeps `incrementFor` monotonic and branch-free, and the floor is already far below hearing above 1 Hz. Left as is.
- **A knob taper in `libs/dsp`.** The DSP takes a linear gain and a position, and the knob laws stay in the parameter table, so measuring the demo (O1–O6) changes a mapping, not the library.
- **The working hypotheses** (linear volume, sin/cos pan, Fine linear in cents) are recorded with their sources and confidence in inventory §2, with the measurement that settles each one in M1.13.

## Results

From the `[controls-report]` run ([oscillator_controls_report.cpp](../../tests/dsp/oscillators/oscillator_controls_report.cpp), 17 s in `relwithdebinfo`).

### Tuning accuracy

**The increment against the target**, for every base note 0–127, every octave −3..3, every note offset −7..7, and fine in steps of 0.01 (2.7 million pitches per row), against the target computed in double precision:

| Rate | Master tune (A4) | Pitches below Nyquist | Clamped at Nyquist | Worst error, target ≥ 20 Hz | Worst error, any target |
|---|---:|---:|---:|---:|---:|
| 44.1 kHz | 432 Hz | 2,568,466 | 132,974 | 0.0012 cents (13.5 kHz) | −0.027 cents (0.65 Hz) |
| 44.1 kHz | 440 Hz | 2,565,851 | 135,589 | −0.0012 cents (20.9 Hz) | −0.027 cents (0.67 Hz) |
| 44.1 kHz | 443.5 Hz | 2,564,717 | 136,723 | −0.0012 cents (20.9 Hz) | −0.026 cents (0.65 Hz) |
| 48 kHz | 432 Hz | 2,580,007 | 121,433 | −0.0012 cents (22.0 Hz) | −0.030 cents (0.64 Hz) |
| 48 kHz | 440 Hz | 2,577,543 | 123,897 | −0.0013 cents (20.4 Hz) | −0.030 cents (0.66 Hz) |
| 48 kHz | 443.5 Hz | 2,576,465 | 124,975 | −0.0013 cents (20.9 Hz) | −0.028 cents (0.65 Hz) |

- **At 20 Hz and above, the worst error is 0.0013 cents**, about 4,000 times finer than the 5-cent threshold of hearing. At the bottom of that range it is the fixed-point floor (0.0009–0.0010 cents at 20 Hz) plus float rounding. Higher up, float rounding alone reaches 0.0012 cents.
- **Below 20 Hz, the fixed-point floor takes over**, as predicted: the worst is −0.030 cents at the lowest pitch the tuning can reach (MIDI 0, −3 octaves, −7, −1 fine, about 0.64 Hz), against a floor of 0.030 cents there. It is always flat, because the increment is truncated.
- **Pitches above Nyquist** (high notes with high tuning) clamp to a Nyquist increment, by design. There a voice plays only its waveform's mean, faded to it from 0.45 fs ([oscillator-range.md](oscillator-range.md)), not the tone at Nyquist that a clamped increment would otherwise make.

**The measured frequency of the rendered sine**, through the slot, from a least-squares fit to its upward zero crossings (`measureFrequency`, [measuring-dsp.md](measuring-dsp.md)). Fine in quarters, base notes 21, 60, and 96, targets below 5 kHz, A4 = 443.5 Hz, 0.5 s per render or 4.5 cycles if longer:

| Rate | Renders | Worst against the target | Worst against the increment's own frequency |
|---|---:|---:|---:|
| 44.1 kHz | 2,523 | −0.0080 cents (2.2 Hz) | 2.0·10⁻⁷ relative (4.9 kHz, 0.5 s) |
| 48 kHz | 2,523 | −0.0081 cents (2.3 Hz) | 2.4·10⁻⁸ relative (3.6 Hz) |

- The worst error against the target is the fixed-point floor at 2.2 Hz (0.0081 cents): the oscillator plays exactly its increment.
- The rendered signal matches the increment's frequency to 2·10⁻⁷ (3·10⁻⁴ cents). That figure is the measurement's limit, not the oscillator's: at 4.9 kHz there are only 9 samples per cycle, and over half a second the interpolation bias of the crossings does not average out. The unit test, over 2 s, holds it to 10⁻⁹.

### Pan

Measured left and right RMS of a sine through the slot, relative to an unpanned sine, against the equal-power law:

| Knob (0–10) | Position | Left | Right | L² + R² | L + R (mono sum) |
|---:|---:|---:|---:|---:|---:|
| 0 | −1 | 1.00000 | 0.00000 | 0.000 dB | 0.000 dB |
| 1.25 | −0.75 | 0.98079 | 0.19509 | 0.000 dB | +1.407 dB |
| 2.5 | −0.5 | 0.92388 | 0.38268 | 0.000 dB | +2.323 dB |
| 3.75 | −0.25 | 0.83147 | 0.55557 | 0.000 dB | +2.842 dB |
| 5 | 0 | 0.70711 | 0.70711 | 0.000 dB | +3.010 dB |
| 6.25 … 10 | | mirror images | | | |

Every measured gain equals the law to 5 decimals. The power is constant across the field, and the mono sum rises by 3.01 dB at the centre, as the theory says.

### Volume and smoothing

| Knob (0–10) | Taper gain | Measured gain | Measured |
|---:|---:|---:|---:|
| 10 | 1.00 | 1.0000000 | 0.00 dB |
| 7.5 | 0.75 | 0.7500000 | −2.50 dB |
| 5 | 0.50 | 0.5000000 | −6.02 dB |
| 2.5 | 0.25 | 0.2500000 | −12.04 dB |
| 1 | 0.10 | 0.1000000 | −20.00 dB |
| 0 | 0.00 | 0.0000000 | silence |

The gain is exact to 7 decimals. A volume, pan, or invert change takes exactly 10 ms to land: 441, 480, and 960 samples at 44.1, 48, and 96 kHz.

### CPU

Nanoseconds per sample, 64 taps, 48 kHz, `relwithdebinfo` ([baselines](../performance/baselines.md); ranges over three runs in one session):

| Case | Bare oscillator, mono | Bare + the same stereo gain inline | Oscillator slot |
|---|---:|---:|---:|
| Saw 440 Hz, settled | 3.04–3.26 | 3.53–4.01 | 3.24–3.43 |
| Saw 440 Hz, volume ramping | | | 4.65–5.14 |
| Saw 440 Hz, a pitch per sample | | 9.15–9.51 | 8.39–9.13 |
| TriSaw 12 kHz, settled | | 32.63–33.93 | 32.10–33.58 |

- **At ordinary pitches the slot adds 0.2–0.3 ns per sample to the bare oscillator,** for two channels with volume, pan, and polarity, and it is cheaper than writing the same gains inline. The oscillator's output is staged 64 samples at a time on the stack, and `StereoGain`'s settled loop then runs separately, where clang vectorizes it (width 4, interleaved twice). Inline, the gain multiply-adds sit in the oscillator's loop, which carries state from sample to sample and does not vectorize.
- **A gain ramp costs 1.3–1.7 ns per sample** for its 10 ms: two smoother steps per sample, in a loop that cannot vectorize.
- **A pitch per sample costs about 6 ns more than a pitch per block,** almost all of it `std::exp2` in `noteToFrequency`. `fastExp2` (3·10⁻⁷ relative, 5·10⁻⁴ cents) would save most of it. It is left for Phase 3, because the per-sample path would then differ slightly from the per-block one, which is currently bit-identical.
- **The oscillator itself is unchanged.** Every oscillator row is within −3% to +5% of the parent commit's build, benchmarked straight after in the same session.

## Tests

- [pitch_test.cpp](../../tests/dsp/math/pitch_test.cpp): `Tuning::offset()` sums octaves, semitones, and fine exactly (Sylenth1's extremes give ±44); whole octaves are exact frequency ratios.
- [pan_test.cpp](../../tests/dsp/math/pan_test.cpp): equal power is exact at the hard sides, −3.01 dB at the centre, matches sin/cos in double across the range, keeps L² + R² = 1, mirrors bit for bit, is monotonic, and gives 0.924 / 0.383 at p = −½; linear is −6 dB at the centre and sums to 1; balance is unity at the centre; positions clamp and NaN is the centre.
- [stereo_gain_test.cpp](../../tests/dsp/control/stereo_gain_test.cpp): it adds (not writes) volume × pan × input to each channel; inverted is the exact negation; a volume change ramps in 480 equal steps at 48 kHz and lands exactly; a pan move ramps both channels together; toggling invert never steps by more than 2/480; `reset()` and `prepare()` finish a ramp; the ramp time and the pan law are settable; NaN volume is silence; a mono block is untouched; only the shorter of input and block is processed.
- [oscillator_slot_test.cpp](../../tests/dsp/oscillators/oscillator_slot_test.cpp):
  - hard left at unity volume, the left channel is the bare oscillator bit for bit for all 7 periodic waveforms, and the right channel is exactly 0;
  - the slot plays note + tuning at the master tune;
  - over every octave, note offset, and fine in eighths, on 4 base notes at 44.1 and 48 kHz, the increment's frequency is within the derived bound of the double-precision target (1.6·10⁻⁶ plus the fixed-point unit), or clamped at Nyquist;
  - the rendered sine runs at its increment's frequency to 10⁻⁹;
  - volume halves exactly, pan follows the law exactly, invert negates exactly;
  - **the PWM trick:** two saws, both retriggered, one inverted and at Phase 90 (exactly a quarter cycle; A4 = 46.875 Hz makes the increment exactly 2²² at 48 kHz), sum to the ideal 25% pulse away from its edges (within 2·10⁻⁴), with a mean within 10⁻⁶ of 0, no 4th or 8th harmonic (below −80 dB), and a fundamental of (4/π) sin(π/4);
  - a note per sample gives the bare oscillator's output with the pitch set every sample, bit for bit, and a constant note per sample equals the per-block form;
  - the output does not depend on the block size (1, 7, 63, 64, 65, 500, 2000);
  - the lifecycle contract; `reset()` finishes a volume ramp; a mono block is untouched. (Noise through the slot, and every unison behaviour, are tested in [unison_slot_test.cpp](../../tests/dsp/oscillators/unison_slot_test.cpp).)
- [measure_test.cpp](../../tests/support/test/measure_test.cpp): `measureFrequency` reads pure sines (27.5 Hz to 5 kHz) and a 0.61 Hz sine to 10⁻⁹, a saw exactly, and throws with fewer than two crossings.
- [oscillator_controls_report.cpp](../../tests/dsp/oscillators/oscillator_controls_report.cpp) (`[controls-report]`, hidden) prints the tables above.

Every new test was checked to fail with a deliberate break in the code it covers (34 breaks, one per test or section), then the code was restored.

## References

- LennarDigital, *Sylenth1 User Manual* v2.5, 2015, pp. 10, 14–15: the controls, the centre defaults, and the two-saw PWM trick. Sources for each range and law are in the [feature inventory](../reference/sylenth1-features.md#tuning-volume-pan-and-invert).
- M. A. Gerzon, "Panpot Laws for Multispeaker Stereo", AES Convention 92, 1992: why constant-power panning keeps loudness steady.
- J. O. Smith III, *Spectral Audio Signal Processing*, online book, W3K Publishing, 2011: the Fourier series of the pulse and the saw.
- Brian C. J. Moore, *An Introduction to the Psychology of Hearing* (6th ed., 2012), ch. 6: frequency discrimination.
