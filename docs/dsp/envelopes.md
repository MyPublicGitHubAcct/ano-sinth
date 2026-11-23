# Envelopes: the ADSR

An envelope generator gives a note its shape over time. It rises when a key goes down, settles while the key is held, and falls away when the key comes up. Sylenth1 has four per voice: an amp envelope for each part, and two mod envelopes for the modulation section (inventory [§4, Envelopes](../reference/sylenth1-features.md#envelopes)). This page covers:

- what an ADSR is, and the ways to draw its segments;
- what is known about Sylenth1's envelopes;
- the segment models prototyped here, and what each one measured: shape, timing at every rate, discontinuities and clicks, stages of 0, control changes mid-stage, block sizes, denormals, and CPU;
- the decision, how it is built, and the tests that pin it down.

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

Measured at commit `c8c1ab1` by the hidden `[envelope-report]` and the `[envelope]` benchmarks, on test-only prototypes (`tests/dsp/control/envelope_candidates.hpp` at that commit). The prototypes were removed once the choice was built. The report and the benchmarks now measure `Adsr` (**Results, as built** below), and checking out that commit reruns every figure in this section.

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

### The decision

Chosen by the project owner on 2026-11-14, each the recommended option (PLAN.md, Decisions log):

| Question | Decision | Why |
|---|---|---|
| **Model** | **Exponential towards an overshoot target, attack ratio 0.3, decay and release ratio 0.001, counted in float** (closed form through `fastExp2`, stages ended by sample count) | Exponential, as the one Sylenth1 test reports. Its release is a near-straight line in dB that stays audible for 90% of its time, so a long release lasts about as long as it is set. Exact timing at every rate and length in float only (fit for STM32). Ends at exactly 0, so the end of a note is exact. Never goes subnormal, so it needs no flush. 2.8 ns per sample, and it can vectorize. |
| **Retrigger while sounding** | **From the current level** (the attack re-entered where its curve passes the current level) | A restart from 0 clicks in proportion to the level it cuts (−12 dB from a quarter). The one user report says Sylenth1 doesn't click (E5). |
| **A stage entered part-way, and time changes mid-stage** | **Constant rate**: the stage's full-scale curve, entered at the current level | Analog behavior. The level stays continuous under every event and control change. With a near-exponential release it differs little from constant time (90% of the time from half level, E7). |
| **Sustain changed mid-sustain** | **A 10 ms linear ramp** to the new level (during the decay, the decay re-aims) | −50 dB against −12 dB for a jump, independent of the decay time. v3.008 fixed "clicks and pops during automation" (E8). |
| **Legato** | A gate-on with no retrigger leaves a held envelope alone, and starts one that is released or idle | Mono Legato (manual p. 29). M1.4 decides when to use it. |
| **End of note** | `isActive()` is false exactly when the release reaches 0 (or after an R of 0). No threshold. | The model ends at 0 in finite time. E10 asks what the demo does; E11 (freeing a held note at sustain 0) is the voice's decision in M1.4. |
| **Knob law (parameter table, M1.9)** | A working hypothesis to be measured (E1): **t = 25 s · x^4.64** for knob x ∈ [0, 1], the same for A, D, and R | It passes through the two reported points (1 s at half, 25 s at the top) and through 0 at 0 (the manual's instant 0). It gives fine control of short times, which fits the "snappy" reputation: 0.6 ms at knob 1, 14 ms at 2, 92 ms at 3. The offset exponential (576^x − 1)/23 fits the same points but puts 39 ms at knob 1. Sustain: linear, knob / 10 (E4). |

## How it is applied here

### The envelope

`Adsr` ([adsr.hpp](../../libs/dsp/include/anoesis/dsp/control/adsr.hpp)) is the whole of it: four settings, a stage, the current segment's four floats and two counts, and a `LinearSmoother` for sustain changes. It is about 80 bytes, with no tables and no allocation.

- **A segment** is set up whenever a stage is entered, in double (a few logs, at events only): the overshoot target T = e + r(e − s), the starting distance d₀ = level − T, the exponent per sample log₂c = log₂(r / (1 + r)) / N, and the samples left on the full-scale curve from the current level, round(log₂(−r(e − s) / d₀) / log₂c). That is N from the start of the curve, and fewer part-way. These are the constant-rate rule and the exact end, in one formula.
- **Each sample** is level = T + d₀ · `fastExp2`(log₂c · k), with k counted from the stage's start, and the stage's last sample is its end level exactly. Nothing accumulates. The count is exact in float up to 2²⁴. Above that (a stage longer than 87 s at 192 kHz, or 22 s at 768 kHz), rounding the count changes the exponent by at most 2⁻²⁴ · |log₂ q|, the same as the product's own rounding. So stages up to `maxStageSeconds` (60 s) are as exact at 768 kHz as at 48 kHz (tested at 25 s and 768 kHz, 19.2 million samples).
- **Stages of 0 are skipped at once.** An attack of 0 makes the gate's own sample the decay's first. A decay of 0 puts the sustain level on the sample after the peak (no ramp). A release of 0 outputs 0 on the note-off's sample, and the envelope is idle.
- **The gate.** `gateOn()` attacks from the current level (from idle, that is 0). `legatoGateOn()` does nothing while the gate is on (attack, decay, sustain) and is `gateOn()` otherwise. `gateOff()` releases from the current level, and does nothing in the release or when idle. `isActive()` is false from the sample the release reaches 0. `isGateOn()` tells the voice whether the key is down.
- **Controls** are seconds (clamped to [0, 60]) and a linear sustain level (clamped to [0, 1], a subnormal value is 0). NaN is ignored. A time changed mid-stage re-enters the stage at once, at the new rate. A sustain change while sustaining starts a 10 ms linear ramp, which lands exactly. During the decay, the decay re-aims at the new level, or ramps to it if the level is already past it.
- **Two ways to render, the same output.** `next()` gives one sample. `process(out)` writes a block, a run of a stage at a time: the run's samples in a loop with no recursion, which clang vectorizes, and then the stage's last sample through the same step as `next()`. `process(out, events)` applies gate events at their sample offsets. Both evaluate the same expression from the same count, so they are bit-identical however a render is split.
- **Never subnormal.** Every segment ends at its end level in finite time, so the level never approaches 0 asymptotically, and the sustain level is flushed. No `flushDenormal()` is needed per sample.

### How a voice uses it

```cpp
// The voice's block: gates at their sample offsets, from the MIDI events.
std::array<AdsrEvent, 2> gates{{{.offset = 17, .gate = AdsrGate::off},
                                {.offset = 300, .gate = legato ? AdsrGate::legatoOn : AdsrGate::on}}};
ampEnvelope.process(levels, gates);  // levels[n] multiplies the part's audio
if (!ampEnvelopeA.isActive() && !ampEnvelopeB.isActive())
    freeVoice();                     // M1.4
```

In `dsp::Voice` (M1.5 task 3, built 2026-11-23, [modulation-sources.md](modulation-sources.md#as-built)) the two mod envelopes are `Adsr`s gated exactly as the amp envelopes: `gateOn()` at each note-on, `legatoGateOn()` at a legato note, `gateOff()` at a note-off. They run only while a route reads them, their gates tracked either way, and they don't keep a voice alive (`isActive()` reads the amp envelopes only). They take their settings from the engine's context at each note-on, and a change reaches a sounding voice at its next grid point. Both amp envelopes always run, so either can be a source (AmpEnv A/B) whatever the skip plan does.

Control changes (sample-accurate automation, M1.8) split the block at their sample: process up to it, set the control, process on. The knob laws live in the parameter table (M1.9): times t = 25 s · x^4.64 for knob x ∈ [0, 1] and sustain = knob / 10, both working hypotheses (E1, E4).

## Results, as built

From the `[envelope-report]` ([envelope_report.cpp](../../tests/dsp/control/envelope_report.cpp)), which now measures `Adsr` through the candidates' interface, and the `[envelope]` benchmarks ([envelope_benchmark.cpp](../../tests/dsp/benchmarks/envelope_benchmark.cpp)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[envelope-report]"   # under a second
tools/bench.sh "[envelope]"
```

Every figure matches the chosen prototype's: attack 0.399 / 0.676 / 0.867 at its quarters, decay and release −15.0 / −30.3 / −46.7 dB, 90% of a release above −60 dB. Every stage is exact to the sample at 44.1–192 kHz from 1 ms to 25 s, with the attack within 4.2·10⁻⁷ of its ideal curve. A release from 0.5 takes 0.900 of its time. The click of a retrigger from the current level is −34.7 dB. Stages of 0 are as described. Output is bit-identical in blocks of 1, 37, and 4096, and there are no subnormals. One figure changed, as decided: **a sustain change from 0.5 to 0.25 now measures −50.3 dB** (a 10 ms ramp), where the prototype jumped (−12.0 dB).

CPU, nanoseconds per sample, 48 kHz ([baselines](../performance/baselines.md)):

| Stage | `process()` | `next()` per sample |
|---|---:|---:|
| Attack | 0.52 | 2.65 |
| Decay | 0.49 | 2.69 |
| Sustain | 0.08 | 1.27 |
| Release | 0.52 | 2.75 |
| **64 envelopes (16 voices × 4), a quarter in each stage** | **25.3** | |

`process()` is 5× faster than `next()` because each sample of a stage depends only on its count, so a run vectorizes. A 16-voice synth's 64 envelopes cost 0.12% of a core.

## Tests

[adsr_test.cpp](../../tests/dsp/control/adsr_test.cpp) (`[adsr]`). The reference is the model's closed form in double. Each segment's tolerance is derived in `segmentTolerance()` from `fastExp2`'s documented bound (5·10⁻⁷ relative) and float rounding: three roundings of the exponent at 2⁻²⁴ · |log₂ q| each, and three of the level at 2⁻²⁴ · (1 + r). That is 1.2·10⁻⁶ for the attack and 1.9·10⁻⁶ for decay and release.

- **Lifecycle and defaults.** `Preparable`. `reset()` and a second `prepare()` reproduce a fresh envelope bit for bit. By default it is a plain gate (0, 0, 1, 0), idle and silent until a gate.
- **Timing.** At 44.1, 48, 96, and 192 kHz, for 1 ms, 100 ms, and 1 s, each stage ends exactly on sample round(time × rate), at exactly its end level, and not before. Long stages: 25 s at 44.1 and 192 kHz and at 768 kHz (19.2 million samples, past 2²⁴) stay within the tolerance and end exactly.
- **Shape.** Every sample of attack, decay, and release against the model, at four rates and two sets of times, and the attack at half its time against 1.3(1 − √(0.3/1.3)).
- **Constant rate.** A release from 0.9, 0.5, 0.1, and 0.001 follows the full-scale curve from that level, and ends after the samples the model counts. From 0.5 that is 0.900 of the time.
- **Retrigger and release from every stage.** A gate-on in the attack, decay, sustain, release, and idle continues from the current level along the attack curve. A note-off in the attack, decay, and sustain releases from the current level. A legato gate-on in a held stage changes nothing, bit for bit. In the release or idle it is a gate-on. A note-off in the release or idle changes nothing.
- **Stages of 0** as described above, and a time under half a sample is 0.
- **Sample-accurate gates.** A gate-on at offset 0, 1, 200, or 511 of a block is the same note delayed, bit for bit, with zeros before it. A note-off at an offset takes effect on that sample.
- **Block sizes.** A script of gates, legato gates, and control changes rendered in blocks of 1, 37, and 4096 through `process(out, events)` equals `next()` per sample, bit for bit, at four rates.
- **Control changes mid-stage.** An attack, decay, or release time changed mid-stage continues from the level at the new rate. Half-way up a 100 ms attack, 10 ms leaves exactly 240 samples. A time of 0 ends the stage at once. A sustain change while sustaining is a straight line over exactly round(10 ms × rate) samples, landing exactly. In the decay it re-aims (or ramps up if the level is already below the new level). In the attack the decay goes to the new level.
- **Controls** clamp, ignore NaN, and flush a subnormal sustain to 0.
- **No subnormals** at four rates and sustain levels 0, 10⁻³⁰, 10⁻⁶, 0.5, and 1, held and released. **No allocation** in any control, gate, or process call.
- **Stress.** At each rate, 400,000 samples of random gates, legato gates, note-offs, and control changes (times of 0, one sample, NaN, and up to 0.5 s; sustain levels including 0): finite, within [0, 1], free of subnormals, and bit-identical in blocks of 1, 37, and 4096 against `next()` per sample.

**Every test was checked to fail first.** Against an inert stub with the same interface, 12 of the 18 tests failed. The other 6 pin properties that silence also has (the lifecycle, legato as a no-op, no allocation, no subnormals, stability, block sizes), and the breaks below check them. Then 22 deliberate breaks in `Adsr`, each caught and each restored. Every test fails under at least one break.

| Break | Tests that failed |
|---|---|
| a retrigger restarts from 0 | retrigger |
| constant time instead of constant rate | 7, including part-way release, note-off, retrigger, time changes, curves, stress |
| a sustain change jumps instead of ramping | sustain change |
| each stage one sample too long | 10, including timing, long stages, curves, stages of 0, stress |
| the coefficient quantized to float (as a float recursion would be) | 7, including timing, long stages, curves |
| legato retriggers | legato |
| a note-off in the attack is ignored | note-off; stages of 0 |
| events applied one sample late | sample-accurate gates; block sizes; stress |
| `process()` computes a run differently from `next()` (a product of two powers) | sample-accurate gates; block sizes; stress |
| a decay of 0 ramps to the sustain level | stages of 0; sustain change; time changes; part-way release |
| the attack ratio 0.5 instead of 0.3 | curves; long stages; retrigger; time changes |
| the decay and release ratio 0.01 instead of 0.001 | 7, including curves, long stages, part-way release |
| NaN times not ignored; no upper clamp on times; a subnormal sustain kept | controls (each) |
| a sustain change during the decay does not re-aim it | sustain change |
| a time change mid-stage waits for the next stage | time changes; sample-accurate gates |
| an allocation at a gate-on | no allocation |
| the stage's last sample returns the next stage's level | stages of 0 |
| `reset()` keeps the level | lifecycle |
| a pure-exponential tail (ratio 10⁻³⁹, passing through subnormals) | 10, including no subnormals |
| the default sustain 0 instead of 1 | defaults |

The first build had one bug that these tests caught. With a decay of 0, the attack's last sample returned the sustain level instead of 1, because the instant decay overwrote the level before it was returned ("stages of 0").

## References

- LennarDigital, *Sylenth1 User Manual*, v2.5, 2015, pp. 17, 19, 29, 30; [version history](https://lennardigital.com/sylenth1/whatsnew.php), v3.008 and v3.053. Users' reports: KVR thread [444457](https://www.kvraudio.com/forum/viewtopic.php?t=444457) ("Synths with the snappiest envelopes", 2015) and the [Xfer forum](https://xferrecords.com/forums/general/mono-legato-retrigger-problem). Sources and open questions: the [feature inventory](../reference/sylenth1-features.md#envelopes).
- N. Redmond, ["Envelope generators: ADSR code"](https://www.earlevel.com/main/2013/06/03/envelope-generators-adsr-code/), EarLevel Engineering, 2013: the overshoot-target exponential segment and its ratios.
- C. Roads, *The Computer Music Tutorial*, MIT Press, 1996: envelopes and their stages.
- Curtis Electromusic Specialties, *CEM3310 Voltage Controlled Envelope Generator* data sheet: an analog ADSR in one chip, with exponential segments.
- D. Goldberg, "What every computer scientist should know about floating-point arithmetic", *ACM Computing Surveys* 23(1), 1991: rounding error in accumulated sums and products.
- B. C. J. Moore, *An Introduction to the Psychology of Hearing*, 6th ed., 2012, ch. 4: loudness, and why a linear fade sounds uneven.
