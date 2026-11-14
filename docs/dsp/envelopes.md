# Envelopes: the ADSR

An envelope generator gives a note its shape over time. It rises when a key goes down, settles while the key is held, and falls away when the key comes up. Sylenth1 has four per voice: an amp envelope for each part, and two mod envelopes for the modulation section (inventory [§4, Envelopes](../reference/sylenth1-features.md#envelopes)). This page covers:

- what an ADSR is, and the ways to draw its segments;
- what is known about Sylenth1's envelopes;
- the segment models prototyped here, and what each one measured: shape, timing at every rate, discontinuities and clicks, stages of 0, control changes mid-stage, block sizes, denormals, and CPU;
- the decision, and how it is built.

Read [parameter-smoothing.md](parameter-smoothing.md) (the one-pole approach and landing exactly), [denormals.md](denormals.md), and [processor-lifecycle.md](processor-lifecycle.md) first.

## Theory

### The four stages

An ADSR takes a **gate**: on while the key is held, off when it is released. Its output is a level, usually in [0, 1].

- **Attack:** on gate-on, the level rises to the peak (1) in the attack time.
- **Decay:** from the peak it falls to the sustain level in the decay time.
- **Sustain:** a *level*, not a time. The output holds there while the gate stays on.
- **Release:** on gate-off, from wherever the level is, it falls to 0 in the release time. Then the envelope is idle, and the voice can be freed.

An amp envelope multiplies the voice's audio. A mod envelope is added, scaled, to a destination such as the cutoff (a "filter envelope") or the pitch.

Three questions are not answered by "ADSR" alone, and every synth answers them its own way:

1. **The shape of each segment.** Straight lines, or curves, and which curves.
2. **What a stage means when it starts part-way.** A release that starts from the sustain level 0.3 rather than from 1, or a retrigger that starts an attack from the level a release has reached.
3. **What happens to a sounding envelope** when the gate comes on again (a retrigger), or when a control changes mid-stage.

### Segment shapes

**Linear.** The level changes at a constant rate. It is simple and exactly timed, but it sounds wrong for decays and releases. Loudness is roughly logarithmic in amplitude, so a linear fall sounds like a long hold that drops away at the end: −6 dB at half the time, −12 dB at three quarters, then the last quarter is spent falling through 50 dB of quiet. A linear attack sounds slow to start.

**Exponential (the analog RC).** A capacitor charged through a resistor approaches its target along y(t) = target + (y₀ − target)·e^(−t/τ). It never arrives, so an analog envelope's time is a convention, for example the time to fall to −60 dB. A pure exponential decay is a straight line in dB: it sounds like an even fade. The classic analog ADSR chips charge the attack towards a voltage *above* the peak and switch to the decay when a comparator sees the peak. The attack is then the first part of an exponential: concave, fast at first and slowing into the peak.

**Exponential towards an overshoot target.** This is the digital form of that comparator trick, used for every segment. Each segment approaches a target **beyond** its end level, so it crosses the end level in finite time, and the crossing ends the stage. With a segment from s to e and a ratio r, the target is e + r(e − s). The curve is

  y(k) = T + (s − T)·c^k, with c = (r / (1 + r))^(1/N),

which lands exactly on e after N samples. The ratio sets the curvature: large r is nearly linear, and small r is nearly a pure exponential that ends a little early. This is the form described by Nigel Redmond (EarLevel Engineering), who suggests a ratio of about 0.3 for the attack and 0.0001 for decay and release as an analog-like default. It keeps exact timing and a clean end at 0.

### A stage entered part-way: constant rate or constant time

A release from the sustain level 0.3 can take the full release time (**constant time**). It can also follow the full-scale release curve from where that curve passes 0.3 (**constant rate**). Then it takes less time, and the release knob sets the speed of the fall, not its duration. Analog envelopes are constant rate: a capacitor discharging through a fixed resistor falls at the same rate, whatever its starting voltage. Constant rate also makes a retrigger from the current level and a time change mid-stage simple: re-enter the full-scale curve at the current level.

### Discontinuities and clicks

The envelope multiplies the audio, so any fast change in the envelope spreads energy across the spectrum: a click. A **step in level** (an envelope that jumps) clicks like a hard gate. A **corner in slope** (a stage change at a different rate) is a much smaller event, and every ADSR has one at the attack's peak. The worst cases in practice:

- **A retrigger that restarts from 0** while the note still sounds: a step down from the current level, then the attack.
- **A sustain change that jumps** while held: a step of the change's size.
- **Stages of 0:** a step, on purpose. Sylenth1's manual says attack and release of 0 click and are useful for punch.

### Arithmetic

A segment can be computed recursively (y ← y + step, or d ← c·d for the distance d to the target) or in closed form from a sample count. In float, a recursion close to 1 runs out of precision on long stages. A 25 s stage at 192 kHz is 4.8 million samples:

- A linear step of 1/4.8·10⁶ added to a level near 1, where a float's spacing is 6·10⁻⁸, is rounded by up to 30% at every addition.
- An exponential coefficient c = 1 − 10⁻⁶ is stored to about 3·10⁻⁸, which is a 3% error in (1 − c) and so in the stage's time.

The fixes are double precision, or counting samples: the level from the count (linear: y₀ + step·k; exponential: d₀·2^(k·log₂c)), with the stage ended by the count, so no error accumulates.

### Denormals

A pure exponential never reaches its target. Its distance to it shrinks without end: below 10⁻³⁸ it is a subnormal float, which is slow on x86 ([denormals.md](denormals.md)). An analog-style decay held at its sustain level does exactly this, a few hundred milliseconds into the note. The overshoot-target model ends each segment at a finite time, so it never gets there.

## Sylenth1: what is known

The full source table, with confidence ratings and the open questions, is in the inventory ([§4, Envelopes](../reference/sylenth1-features.md#envelopes)). In short:

- **Official (manual):** the amp and mod envelopes are "common ADSR envelopes", the mod envelopes "similar to the amplitude envelope" (pp. 17, 19). An attack or release of 0 "creates a very sharp on/off switching effect, which might result in a clicking sound", on purpose for punch (p. 17). Mono Legato does not retrigger (p. 29). Short decays and releases save polyphony (p. 30). Automation is sample-accurate and was fixed not to click (v3.008).
- **Users (KVR thread 444457, 2015):** one VU-meter test reads the attack and release as exponential. One ear test puts the longest release at about 25 s and half the knob at about 1 s. One user (Xfer forum, 2026) says Sylenth1 retriggers without clicks.
- **Unknown:** the curves' exact form, the knob law between those two points, sustain's law, what a retrigger does to the level, what a control change does mid-stage, and when a voice is freed. Open questions E1–E12, each with the demo measurement that settles it in M1.13.

## The candidates and what they measured

Measured by the hidden `[envelope-report]` ([envelope_report.cpp](../../tests/dsp/control/envelope_report.cpp)) and the `[envelope]` benchmarks ([envelope_benchmark.cpp](../../tests/dsp/benchmarks/envelope_benchmark.cpp)), on test-only prototypes ([envelope_candidates.hpp](../../tests/dsp/control/envelope_candidates.hpp)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[envelope-report]"   # about 4 s
tools/bench.sh "[envelope]"
```

Every prototype shares one stage machine and one set of rules, so the tables compare curves and arithmetic and nothing else:

- **Constant rate.** Each stage is a piece of a full-scale curve: attack 0 → 1, decay 1 → sustain, release 1 → 0, each in its time. A stage entered part-way enters its curve at the current level.
- **A stage of N samples ends on its N-th sample**, which outputs the end level exactly. A stage of 0 is skipped at once.
- **Gates are sample-accurate.** A gate applies before the next sample, and the caller splits its blocks at event offsets.

| Candidate | Curve | Arithmetic |
|---|---|---|
| Linear, accumulated | straight | float, y ← y + step, ended by crossing the end level |
| Linear, counted | straight | float, y = y₀ + step·k, ended by count |
| Exponential r 0.3 / 0.3, 0.3 / 0.01, 0.3 / 0.001 | overshoot target; attack ratio / decay-and-release ratio | float, distance d ← c·d, ended by crossing, the state flushed |
| Exponential r 0.3 / 0.001, double | as above | double, d ← c·d |
| Exponential r 0.3 / 0.001, counted | as above | float closed form d = d₀·`fastExp2`(k·log₂c), ended by count |
| Analog RC | attack towards 1.5, switched at 1 (r 0.5); decay and release pure exponentials, timed to −80 dB of the span; the decay never ends, the release ends at −80 dB | float, d ← c·d, not flushed (and flushed, for comparison) |

### Shape

48 kHz, every stage 1 s.

| Model | Attack at 25 / 50 / 75% of its time | Decay to 0 and release from 1, at 25 / 50 / 75% (dB) | Release above −60 dB, fraction of its time |
|---|---|---|---:|
| Linear | 0.250 / 0.500 / 0.750 | −2.5 / −6.0 / −12.0 | 0.999 |
| Exponential r 0.3 / 0.3 | 0.399 / 0.676 / 0.868 | −4.4 / −9.8 / −17.6 | 0.997 |
| Exponential r 0.3 / 0.01 | 0.399 / 0.676 / 0.868 | −10.2 / −20.9 / −33.3 | 0.979 |
| **Exponential r 0.3 / 0.001** | **0.399 / 0.676 / 0.868** | **−15.0 / −30.3 / −46.7** | **0.900** |
| Analog RC | 0.360 / 0.634 / 0.842 | −20.0 / −40.0 / −60.0 | 0.750 |

- **Linear** is the shape users say Sylenth1 does not have ("most definitely exponential", inventory). On a dB meter its release barely moves for half the time and then collapses.
- **The overshoot ratio** moves the release from nearly linear (r 0.3) to nearly a straight line in dB (r 0.001: −15, −30, −47 dB at the quarters, against a pure exponential's −15, −30, −45 if it reached −60 dB at the end). At r 0.001 the release is audible (above −60 dB) for 90% of its time, so a "25 s" release lasts about 25 s by ear, as the user describes.
- **The analog RC** is a straight line in dB to its −80 dB end. Only 75% of its time is above −60 dB, so its release *sounds* shorter than its setting.

### Timing at every rate

Each stage's length in samples against round(time × rate), at 44.1, 48, 96, and 192 kHz: the worst error over the four rates (attack / release from 1). Last column: the worst deviation of the attack from its ideal curve, computed in double.

| Model | 1 ms | 100 ms | 1 s | 25 s | Attack curve error, 1 s / 25 s |
|---|---|---|---|---|---|
| Linear, accumulated | 1 / 1 | −2 / −2 | 325 / 325 | **393,012 / 393,012** | 1.7·10⁻³ / **7.0·10⁻²** |
| Linear, counted | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 4.8·10⁻⁸ / 5.1·10⁻⁸ |
| Exponential r 0.3 / 0.3 (float) | 1 / 1 | 4 / 4 | 200 / 200 | **73,973 / 73,973** | 5.0·10⁻⁴ / **1.5·10⁻²** |
| Exponential r 0.3 / 0.01 (float) | 1 / 1 | 4 / 2 | 200 / 129 | 73,973 / 45,721 | 5.0·10⁻⁴ / 1.5·10⁻² |
| Exponential r 0.3 / 0.001 (float) | 1 / 1 | 4 / 1 | 200 / −99 | 73,973 / 32,400 | 5.0·10⁻⁴ / 1.5·10⁻² |
| Exponential r 0.3 / 0.001, double | 0 / 1 | 0 / 1 | 0 / 1 | 0 / 1 | 3.0·10⁻⁸ / 3.0·10⁻⁸ |
| **Exponential r 0.3 / 0.001, counted** | **0 / 0** | **0 / 0** | **0 / 0** | **0 / 0** | **4.6·10⁻⁷ / 4.7·10⁻⁷** |
| Analog RC (float) | 1 / 1 | 1 / 1 | −5 / −49 | −81,898 / 30,534 | 7.0·10⁻⁵ / 2.8·10⁻² |

- **Float recursion fails on long stages**, as the theory says. At 25 s and 192 kHz the accumulated linear attack ends 8% late with its curve 7% off, and the float exponentials end 1.5% late. At 1 s they are already 200–325 samples off.
- **Counting fixes it in float.** The counted linear and the counted exponential end on exactly the right sample at every rate and length. The counted exponential's curve stays within 4.7·10⁻⁷ of ideal, the `fastExp2` error. Double recursion is as accurate but ends the release one sample late (the exact landing rounds the wrong way), and double arithmetic is slow on a Cortex-M4F, which has a single-precision FPU only.
- The count must stay exact in float for the closed form: k < 2²⁴ samples, 87 s at 192 kHz, beyond the longest stage.

### Stages entered part-way

48 kHz, every time 1 s. Constant rate: the release from a lower level is shorter, by how much depending on the curve.

| Model | Release from 0.5 | Release from 0.1 | Attack retriggered from 0.5 |
|---|---:|---:|---:|
| Linear | 0.500 | 0.100 | 0.500 |
| Exponential r 0.3 / 0.3 | 0.668 | 0.196 | 0.668 |
| Exponential r 0.3 / 0.01 | 0.852 | 0.520 | 0.668 |
| Exponential r 0.3 / 0.001 | 0.900 | 0.668 | 0.668 |
| Analog RC | 0.925 | 0.750 | 0.631 |

With a near-exponential release, constant rate and constant time differ little: from half level the release takes 90% of its time. This is what E7 measures in the demo.

### Discontinuities and clicks

48 kHz, A 10 ms, D 100 ms, S 0.5, R 100 ms. The click is the envelope's energy above 1 kHz in a 4096-sample Hann window centred on the event, in dB relative to an instant step from 0 to 1 (a hard gate is 0 dB). The window includes the 10 ms attack, so the stage changes below are dominated by the attack's own speed rather than by the corner.

| Model | Attack → decay corner: slope before → after (per ms), click | Gate-off at 5 ms into the attack | Gate-off 50 ms into the decay |
|---|---|---|---|
| Linear | 0.100 → −0.005, −38.0 dB | 0.100 → −0.010, −37.5 dB | −0.005 → −0.010, −66.6 dB |
| Exponential r 0.3 / 0.001 | 0.044 → −0.035, −35.4 dB | 0.092 → −0.047, −33.5 dB | −0.001 → −0.036, −49.8 dB |
| Analog RC | 0.055 → −0.046, −35.9 dB | 0.095 → −0.058, −33.9 dB | −0.000 → −0.046, −47.3 dB |

No stage change in any model steps in level. A gate-off mid-stage continues from the current level. The corners are inherent in an ADSR, and they are 33–38 dB below a hard gate.

**Retrigger and sustain changes** are where the choices matter. The retrigger comes 25 ms into the release from 0.5.

| Model | Level at the retrigger | Retrigger from 0: step, click | From the current level: step, click | Sustain 0.5 → 0.25 held: step, click |
|---|---:|---|---|---|
| Linear | 0.250 | −0.248, **−12.1 dB** | +0.002, −37.1 dB | −0.250, **−12.0 dB** (jump) |
| Exponential r 0.3 / 0.01 | 0.151 | −0.147, −16.5 dB | +0.004, −35.2 dB | −0.250, −12.0 dB (jump) |
| Exponential r 0.3 / 0.001 | 0.088 | −0.084, **−21.1 dB** | +0.004, **−34.7 dB** | −0.250, −12.0 dB (jump) |
| Analog RC | 0.050 | −0.047, −25.9 dB | +0.003, −35.3 dB | −0.0005, **−53.4 dB** (runs on at the decay rate) |

For comparison, the same sustain change **ramped linearly over 10 ms** (the `LinearSmoother` the oscillator gains use) measures **−50.3 dB**.

- **A retrigger from 0 clicks** in proportion to the level it cuts: −12 dB from a quarter, −21 dB from 0.09. **From the current level it is as clean as an ordinary attack**, in every model.
- **A sustain change that jumps clicks** like a gate of its size. The RC's decay runs on to the new level (−53 dB), and a 10 ms ramp does almost as well (−50 dB) in any model.

### Stages of 0

Every model handles a stage of 0 the same way, by construction. **A 0** outputs the decay's first value on the gate's own sample (0.999 at D 100 ms), a full step on purpose. **D 0** outputs exactly the sustain level on the sample after the peak. **R 0** outputs exactly 0 on the gate-off sample, and the envelope is idle.

### Control changes mid-stage

Under constant rate, a time change re-enters the running stage's new full-scale curve at the current level. In every model the level stays continuous: the change shows only as a corner in slope, with no step. An attack changed from 100 ms to 10 ms halfway finishes 5.0 ms later. A release changed from 1 s to 100 ms after 100 ms finishes 90 ms later. Both are as constant rate predicts.

### Block sizes

One script of gates and control changes at fixed samples, rendered in blocks of 1, 37, and 4096: **bit-identical for every model.** Each prototype keeps all its state per sample and does nothing per block. The built envelope must keep that rule. A block loop that computes a stage in closed form from the block's start (y = T + d·c^k with k counted from the block) would round differently from one counted from the stage's start, so the count runs from the start of the stage.

### Denormals

48 kHz, one minute, with no flush-to-zero mode set: subnormal outputs / subnormal internal states.

| Model | Decay to 0, held | Decay to 0.5, held | Release |
|---|---|---|---|
| Linear, exponential (all variants) | 0 / 0 | 0 / 0 | 0 / 0 |
| **Analog RC** | **2,875,449 / 2,875,449 (from 95 ms)** | **0 / 2,875,485 (from 94 ms)** | 0 / 0 |
| Analog RC, state flushed | 0 / 0 | 0 / 0 | 0 / 0 |

The analog RC's held decay goes subnormal 94 ms into a note with a 10 ms decay, and stays subnormal for the rest of the note. Its output does too when the sustain is 0. With `flushDenormal()` on the state it is clean. The overshoot model reaches its end level in finite time and holds it exactly, so it never comes near.

### CPU

Nanoseconds per sample, one 512-sample block in each stage, 48 kHz, `relwithdebinfo` (`tools/bench.sh "[envelope]"`). The prototypes run one sample at a time through a switch on the stage.

| Model | Attack | Decay | Sustain | Release |
|---|---:|---:|---:|---:|
| Linear, accumulated | 3.55 | 3.71 | 0.68 | 3.46 |
| Linear, counted | 2.20 | 2.33 | 0.64 | 2.20 |
| Exponential, float, flushed | 6.32 | 5.93 | 0.64 | 5.93 |
| Exponential, double | 3.67 | 3.51 | 0.64 | 3.64 |
| **Exponential, counted** | **2.76** | **2.91** | **1.28** | **2.84** |
| Analog RC | 3.48 | 3.43 | 0.67 | 3.75 |
| Analog RC, flushed | 6.21 | 6.00 | 0.65 | 5.93 |

- **Every model is cheap.** A voice has four envelopes, and 16 voices × 4 × 3 ns is about 190 ns per sample: 0.9% of a core, against about 100 ns for one note's 32 oscillator voices.
- **The flush costs about 2.5 ns per sample** in a moving stage (a compare and a branch the loop cannot hide). The overshoot model does not need it, since its segments end in finite time.
- **The counted exponential is as cheap as double recursion** and nearly as cheap as linear, though it calls `fastExp2` every sample. The multiply chain of a recursion is serial, while each counted sample is independent, so the built version can compute a block of a stage in a vectorized loop.

## The recommendation

*To be decided by the project owner. The decision and its date go here and in PLAN.md's Decisions log.*

| Question | Recommendation | Why |
|---|---|---|
| **Model** | **Exponential towards an overshoot target, attack ratio 0.3, decay and release ratio 0.001, counted in float** (closed form through `fastExp2`, stages ended by sample count) | Exponential, as the one Sylenth1 test reports. Its release is a near-straight line in dB that stays audible for 90% of its time, so a long release lasts about as long as it is set. Exact timing at every rate and length in float only (fit for STM32). Ends at exactly 0, so the end of a note is exact. Never goes subnormal, so it needs no flush. 2.8 ns per sample, and it can vectorize. |
| **Retrigger while sounding** | **From the current level** (the attack re-entered where its curve passes the current level) | A restart from 0 clicks in proportion to the level it cuts (−12 dB from a quarter). The one user report says Sylenth1 doesn't click (E5). |
| **A stage entered part-way, and time changes mid-stage** | **Constant rate**: the stage's full-scale curve, entered at the current level | Analog behavior. The level stays continuous under every event and control change. With a near-exponential release it differs little from constant time (90% of the time from half level, E7). |
| **Sustain changed mid-sustain** | **A 10 ms linear ramp** to the new level (during the decay, the decay re-aims) | −50 dB against −12 dB for a jump, independent of the decay time. v3.008 fixed "clicks and pops during automation" (E8). |
| **Legato** | A gate-on with no retrigger leaves a held envelope alone, and starts one that is released or idle | Mono Legato (manual p. 29). M1.4 decides when to use it. |
| **End of note** | `isActive()` is false exactly when the release reaches 0 (or after an R of 0). No threshold. | The model ends at 0 in finite time. E10 asks what the demo does; E11 (freeing a held note at sustain 0) is the voice's decision in M1.4. |
| **Knob law (parameter table, M1.9)** | A working hypothesis to be measured (E1): **t = 25 s · x^4.64** for knob x ∈ [0, 1], the same for A, D, and R | It passes through the two reported points (1 s at half, 25 s at the top) and through 0 at 0 (the manual's instant 0). It gives fine control of short times, which fits the "snappy" reputation: 0.6 ms at knob 1, 14 ms at 2, 92 ms at 3. The offset exponential (576^x − 1)/23 fits the same points but puts 39 ms at knob 1. Sustain: linear, knob / 10 (E4). |

## How it is applied here

*Written once the model is built (M1.2).*

## Tests

*Written once the model is built (M1.2).* The report's measurements become tests with derived tolerances: the timing of each stage, the shape against the chosen curve, retrigger and release from every stage, stages of 0, sample-accurate gates, block-size invariance, control changes mid-stage, denormals, allocation, and a randomized stress test.

## References

- LennarDigital, *Sylenth1 User Manual*, v2.5, 2015, pp. 17, 19, 29, 30; [version history](https://lennardigital.com/sylenth1/whatsnew.php), v3.008 and v3.053. Users' reports: KVR thread [444457](https://www.kvraudio.com/forum/viewtopic.php?t=444457) ("Synths with the snappiest envelopes", 2015) and the [Xfer forum](https://xferrecords.com/forums/general/mono-legato-retrigger-problem). Sources and open questions: the [feature inventory](../reference/sylenth1-features.md#envelopes).
- N. Redmond, ["Envelope generators: ADSR code"](https://www.earlevel.com/main/2013/06/03/envelope-generators-adsr-code/), EarLevel Engineering, 2013: the overshoot-target exponential segment and its ratios.
- C. Roads, *The Computer Music Tutorial*, MIT Press, 1996: envelopes and their stages.
- Curtis Electromusic Specialties, *CEM3310 Voltage Controlled Envelope Generator* data sheet: an analog ADSR in one chip, with exponential segments.
- D. Goldberg, "What every computer scientist should know about floating-point arithmetic", *ACM Computing Surveys* 23(1), 1991: rounding error in accumulated sums and products.
- B. C. J. Moore, *An Introduction to the Psychology of Hearing*, 6th ed., 2012, ch. 4: loudness, and why a linear fade sounds uneven.
