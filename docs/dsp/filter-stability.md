# Filter stability: the solver near Nyquist, extreme settings, and aliasing

`LadderFilter` solves its loop equations with Newton–Raphson every sample ([filters.md](filters.md)). The keytracking round found that the solve fails with the cutoff held near Nyquist above the threshold: Newton's steps cycle instead of converging, and the output locks at a large level ([keytracking.md](keytracking.md#where-the-solver-fails-near-nyquist-a-finding-for-task-7)). This page covers M1.3 task 7, stability and aliasing under extreme settings:

- the theory: why Newton fails there, why the equations still have exactly one solution, and the ways to find it;
- what the equations should do near Nyquist, from a double-precision reference that cannot fail;
- where the as-built solver fails (more widely than first found), and the candidate fixes measured against the reference: convergence over a grid, the held-cutoff case, the ordinary case, and CPU;
- extreme settings beyond the known defect: what the voice can request, and the filter under audio-rate modulation across the clamp, at 1 Hz, with DC of ±10, at the ceilings of k and drive, with NaN and inf, in blocks of 1, switching responses and bypass while self-oscillating, and over five minutes;
- aliasing at the developer's corner and while self-oscillating near the clamp, at 1×, 2×, and 4×, and antiderivative antialiasing as an option;
- the recommendations, for the project owner's decision.

Read [filter-theory.md](filter-theory.md) (the trapezoidal ladder and its Newton solve) and [filters.md](filters.md) first.

## Theory

### The equations, and why g matters

Each sample, the trapezoidal ladder solves (filter-theory.md):

```text
F_i(y) = y_i − s_i − g·( t(y_(i−1)) − t(y_i) ) = 0,     i = 1..4,     y_0 = x − k·y4
```

where t is the saturator (tanh, or the cubic with Warm Drive off), s_i the integrators' states, and g = tan(π·fc/fs) the prewarped integrator gain. g is what changes near Nyquist:

| fc/fs | 0.10 | 0.30 | 0.40 | 0.44 | 0.46 | 0.48 | 0.49 | 0.495 |
|---|---|---|---|---|---|---|---|---|
| g | 0.32 | 1.38 | 3.08 | 5.24 | 7.92 | 15.9 | 31.8 | 63.7 |

Rearranged, each stage is

```text
y_i + g·t(y_i) = s_i + g·t(y_(i−1)) =: c_i
```

a line of slope 1 plus a sigmoid scaled by g. When g is small the line dominates and the equation is almost linear: Newton converges from anywhere nearby, in one or two steps. When g is large the sigmoid dominates, and Newton on a sigmoid is the textbook case that fails. From a point beyond the knee, where t is flat, the tangent is the line of slope 1, so the step goes as far as the residual: up to 2g. That lands beyond the knee on the other side, where the tangent is flat again, and the iterate ping-pongs. The measured cycle at 0.495 fs had steps of exactly ±127 and ±255 (2g and 4g), the same with 64 iterations as with 8.

The state makes it worse. With g large each integrator moves a lot per sample (s_i ← s_i + 2g·(t(y_(i−1)) − t(y_i)), up to 4g), so even while the filter's output is a small oscillation, the states swing by ±10 or more. The first guess, the loop linearised at the state (each t(v) as v·t(v₀)/v₀ with v₀ = s_i), is then far from the solution, which is where Newton is least reliable.

The other end of the controls does the same at a lower cutoff. With a large drive, u = x − k·y4 jumps by tens in one sample (a full-scale saw at drive 32 jumps by 64), and with a large k the feedback term couples every stage to the input saturator with a gain of g·k·t'(u). Both put the first guess far from the root.

### There is exactly one solution

Newton failing is not the equations failing. Write each stage's solution as a function of its input: y_i + g·t(y_i) = c_i has slope 1 + g·t'(y_i) ≥ 1 (t' ≥ 0 for both saturators, 0 on the cubic's flat top), so for each c_i there is exactly one y_i, and it increases with c_i. Chaining the stages, the y4 that the loop produces for a given feedback value, H(y4), is

```text
y4 ↦ u = x − k·y4 ↦ t(u) ↦ y1 ↦ t(y1) ↦ y2 ↦ … ↦ y4' = H(y4)
```

Every arrow is non-decreasing except the first, which reverses with −k. So H is non-increasing, and

```text
φ(y4) = y4 − H(y4)
```

is strictly increasing with slope at least 1. It has exactly one root, and that root is within 2g of s4 (|y4 − s4| = g·|t(y3) − t(y4)| ≤ 2g). Every stage's root is likewise within g of c_i. So the trapezoidal ladder has a unique solution for every state, input, cutoff, resonance, and drive, and it lies in a known bracket. Checked numerically over 4,000 random states (s in ±50, x in ±32, g 0.1–64, k 0–64): φ changes sign exactly once and never decreases, its smallest slope 1.000.

### Ways to find it

**Globalised Newton.** The textbook remedies keep Newton's step and limit how it is taken (Dennis and Schnabel, 1996):

- *A line search*: take y + λ·Δ with λ halved until the residual ‖F‖² falls by an Armijo fraction. Every accepted step makes progress, so the iteration can't cycle. But the Newton direction can be nearly orthogonal to the descent direction when the Jacobian is badly conditioned, which it is when g·k is large, and then the accepted steps are tiny and a bounded number of them doesn't get there.
- *A step limit*: scale Δ so no stage moves by more than a fixed amount. Cheap, and it stops the wild steps, but it guarantees nothing: a limited step can cycle too, just more slowly.
- *A better first guess*: start from the previous sample's solution, which is close when the signal moves little per sample. It doesn't help when the states swing by 4g per sample.
- *A better variable*: Newton is not invariant under a nonlinear change of variable. In the saturator's output w = t(y), the stage equation is t⁻¹(w) + g·w = c, whose slope is at least g and whose curvature doesn't grow with g, so Newton behaves for large g. The same step can be taken in y and mapped (w_new = t(y) + t'(y)·Δy, y_new = t⁻¹(w_new)). But w can't represent a stage beyond the knee (tanh is 1 in float above |y| = 9; the cubic is flat beyond 1.5), the feedback still runs through y4, and t⁻¹ costs a logarithm (tanh) or an arcsine and a sine (cubic) per stage.

**Bracketing.** The structure above gives a globally convergent method directly: solve φ(y4) = 0, a monotone scalar equation with a known bracket, and for each trial y4 solve the four monotone scalar stage equations, each with its own bracket. Each scalar solve is Newton safeguarded by bisection. The safeguard needs two rules, as in rtsafe (Press et al., 1992, §9.4): bisect when Newton's step would leave the bracket, *and* when it is not at most half the step before last. The first rule alone is not enough: on a stage equation at g = 64, Newton's steps stay inside the bracket but ping-pong between its ends, each shrinking it by a sliver, and after 48 steps its residual was still −118 (the root was 3.1 away). With both rules each solve converges, in a handful of evaluations when started near the root.

Its cost is the nesting: each evaluation of φ is four scalar solves. So it is natural as a **fallback**: run Newton as built while each iteration reduces the residual, and switch to the nested solve on the first iteration that doesn't. Where Newton converges (everywhere it converges quadratically) the fallback never runs, and the output is bit-for-bit the as-built output.

**A lower clamp.** The do-nothing option: keep the cutoff below where Newton fails. It costs range at the top of the keyboard, and only works if the failure has a floor.

### NaN and inf in a recursive filter

IEEE arithmetic propagates NaN: any operation with a NaN gives NaN, and every comparison with a NaN is false. So `std::clamp`, `std::min`, and `std::max` pass it through or not depending on argument order, `flushDenormal` (a comparison) leaves it alone, and a NaN that reaches a recursive state stays there: every later output is NaN until the state is reset. An infinite input does the same one step later (∞ − ∞ in the residual). There are two places to stop it: at the input (replace a non-finite sample by 0, one compare per sample), or in the state (check once per block and reset a channel whose state isn't finite, so a NaN costs at most one block of silence). The setters already map NaN and ±∞ to safe values ([filters.md](filters.md#the-filter)).

### Antiderivative antialiasing inside a loop

First-order antiderivative antialiasing (ADAA, Parker et al., 2016; Bilbao et al., 2017) replaces a saturator t(u) by the mean of t over the segment between successive inputs, (T(u_n) − T(u_(n−1)))/(u_n − u_(n−1)) with T the antiderivative. That is the saturator's output convolved with a one-sample box: it attenuates the harmonics that would fold, by a sinc, at the price of a half-sample delay and a gentle lowpass. For small signals it is (u_n + u_(n−1))/2. On the loop's input saturator, that half-sample delay sits inside the feedback loop, which is exactly what zero-delay feedback removed (filter-theory.md): it adds −ω/2 of phase around the loop, so the resonance moves down and the threshold changes with the cutoff, and the response at the cutoff is no longer exact.

## What is known

Nothing official describes the filter at these settings. The developer gives the cutoff's top as "half the samplerate" (2008), resonance goes "way beyond self-oscillation" (manual p. 5), and in 2007 "there is a bit of high frequency aliasing when you use high resonance or high drive levels". All modulation is additive in knob units (2016), and nothing says whether a control plus its modulation clamps at the knob's range. Three open questions are new in the inventory ([§3](../reference/sylenth1-features.md#3-filters-pp-5-1618)): F23 (self-oscillation at the top of the range), F24 (how far modulation takes Resonance and Drive), and F25 (NaN, which can't be observed on Sylenth1 because its filter's input is its own oscillators).

## The candidates

Nine solvers, all in [filter_stability_candidates.hpp](../../tests/dsp/filters/filter_stability_candidates.hpp), each solving exactly the built filter's equations from the same state, with the same saturators (`TanhSaturator`, `CubicSaturator`):

| Solver | What it does | Why it is here |
|---|---|---|
| **As built** | `LadderFilter::solve`, copied operation for operation: the linearised guess, undamped Newton to 10⁻⁵, at most 8 iterations | The baseline. Checked bit for bit against `LadderFilter` itself. |
| **Damped Newton, 8 and 32** | The same step, with a backtracking line search on ‖F‖² (Armijo, λ halved down to 2⁻¹⁰); a full step accepted costs what it does as built | The textbook globalisation. |
| **Step-limited Newton** | The step scaled so no stage moves more than 1 | The cheapest globalisation. |
| **Newton from the previous solution** | As built, starting from the last sample's y | A better first guess. |
| **Newton in the saturators' outputs** | The step mapped through w = t(y) where the stage is steep (\|w_new\| < 0.999), taken in y elsewhere | A better-conditioned variable. |
| **Newton + nested fallback, from Newton's iterate** | As built while every iteration reduces ‖F‖²; on the first that doesn't (or at the cap), the nested bracketing solve, starting where Newton got to | A globally convergent fallback. |
| **Newton + nested fallback, from the guess** | The same, the nested solve starting from the linearised guess again | The same, from a saner start. |
| **Nested solve alone** | The nested bracketing solve every sample, from the linearised guess | The fallback's cost if it ran everywhere. |
| **Newton with ADAA on the input** | As built, with first-order ADAA on the loop's input saturator | An aliasing option, not a solver fix. |

The nested solve is rtsafe (Newton safeguarded by bisection, with both rules) on the last row φ(y4) over [s4 − 2g, s4 + 2g], to the Newton tolerance 10⁻⁵, and on each stage over [c − g, c + g], stopping when the residual is at float's rounding level for its terms (4 ε (|c| + g)). Each stage starts from the hard-clip model of itself: c/(1 + g) if that is within ±1, otherwise c ∓ g. Two details came out of the prototyping. Without the second rtsafe rule the stage solve ping-ponged (above). Without the rounding-level stop and the model start, the fallback cost 3,282 ns per sample at the corner instead of 1,949.

The **cutoff clamp** (the do-nothing baseline) is the as-built solver with `maxCutoffRatio` lowered to wherever it stops failing.

The reference is the same nested method in double with `std::tanh`, to 10⁻¹⁴ (stages to 10⁻¹⁵), with up to 400 evaluations per level. It cannot fail: every bracket holds the unique root.

## What they measured

From the hidden `[filter-stability-report]` ([filter_stability_report.cpp](../../tests/dsp/filters/filter_stability_report.cpp)) and the `[filter-stability]` benchmarks ([filter_stability_benchmark.cpp](../../tests/dsp/benchmarks/filter_stability_benchmark.cpp)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[filter-stability-report]"   # about 5 minutes
tools/bench.sh "[filter-stability]"
```

### What the filter should do near Nyquist

The reference with the cutoff held, an impulse of 0.5, 48 kHz; the last 0.5 s of 1.5 s, level (dBFS, peak-equivalent from the RMS) / pitch (cents from the cutoff), Warm on (tanh):

| fc/fs | g | k 4.05 | 4.4 | 5 | 6 | 8 | 16 | 64 |
|---|---|---|---|---|---|---|---|---|
| 0.300 | 1.38 | −24.9 / −2.7 | −16.5 / −18.9 | −13.6 / −39.7 | −11.8 / −65.5 | −10.2 / −116.9 | −6.7 / −315.6 | −6.7 / −315.6 |
| 0.400 | 3.08 | −24.9 / −1.2 | −16.6 / −8.4 | −13.8 / −16.2 | −12.3 / −22.0 | −11.5 / −23.1 | −11.2 / −10.5 | −10.3 / −0.0 |
| 0.440 | 5.24 | −24.9 / −0.7 | −16.6 / −4.8 | −13.8 / −9.0 | −12.4 / −11.8 | −11.7 / −10.9 | −11.6 / −0.2 | −12.2 / 17.4 |
| 0.460 | 7.92 | −24.9 / −0.5 | −16.6 / −3.1 | −13.8 / −5.8 | −12.4 / −7.5 | −11.7 / −6.7 | −11.8 / 1.0 | −12.4 / 13.1 |
| 0.480 | 15.9 | −24.9 / −0.2 | −16.6 / −1.5 | −13.8 / −2.8 | −12.4 / −3.6 | −11.8 / −3.1 | −11.8 / 0.8 | −12.6 / 6.9 |
| 0.495 | 63.7 | −24.9 / −0.1 | −16.6 / −0.4 | −13.8 / −0.7 | −12.4 / −0.9 | −11.8 / −0.8 | −11.9 / 0.2 | −12.6 / 1.7 |
| **Built, peak at 0.495** | | 0.1 | 221.7 | 135.7 | 239.2 | 81.9 | 47.5 | 246.0 |

With the cubic (Warm off) the levels are −21.5, −13.7, −11.8, −11.6, −11.6, −11.9, −12.5 dBFS at 0.495 fs and the pitch within 2.3 cents. The built filter's peak there is 198–272 at every k, including 4.05.

- **Near Nyquist the ladder self-oscillates as it does lower down**, at the same level (k 4.4: −16.6 dBFS here, −16.5 at 440 Hz; tanh) and closer to the cutoff. The cramping that bends the response near Nyquist also takes away most of the saturation's detune: −0.9 cents at 0.495 fs against −101 at 440 Hz (k 6). So the right answer at the corner is an ordinary, bounded self-oscillation, and the as-built filter's lock at 50 to 270 is purely the solver's failure.
- **At k 16 and above, low in this range, the oscillation locks to a fraction of the sample rate**: at 0.30 fs and k 16 or 64 it is exactly fs/4 (−315.6 cents from 14.4 kHz is 12 kHz), and louder (−6.7 dBFS). That is the equations' own behaviour (the reference), a strongly nonlinear oscillator locking to the sampling grid, not a solver artefact. It is out of the knobs' range (F24).
- **The solution is unique** (the theory, and 4,000 random states: one sign change of φ, never decreasing, smallest slope 1.000).

### Where the as-built solver fails

The lowest fc/fs, scanned from 0.0025 in steps of 0.0025 at 48 kHz, where Newton leaves a sample unconverged (− : nowhere up to 0.495):

| Saturator | Input | k 3.5 | 4.05 | 4.4 | 5 | 6 | 8 | 16 | 32 | 64 |
|---|---|---|---|---|---|---|---|---|---|---|
| tanh | an impulse of 0.5 | − | − | 0.4800 | 0.4625 | 0.4500 | 0.4400 | 0.4300 | 0.4250 | 0.3200 |
| tanh | a full-scale saw, drive 16 | 0.3800 | 0.3775 | 0.3750 | 0.3700 | 0.3650 | 0.3625 | 0.3750 | 0.3725 | 0.2725 |
| tanh | the same, drive 32 | 0.4050 | 0.3950 | 0.3925 | 0.3875 | 0.3850 | 0.3825 | 0.3625 | 0.3500 | 0.2600 |
| cubic | an impulse of 0.5 | − | − | 0.4575 | 0.4325 | 0.4225 | 0.4175 | 0.3825 | 0.3000 | 0.2725 |
| cubic | a full-scale saw, drive 16 | 0.4450 | 0.4150 | 0.3950 | 0.3575 | 0.3150 | 0.3150 | 0.3250 | 0.2850 | 0.2400 |
| cubic | the same, drive 32 | 0.4550 | 0.4525 | 0.4525 | 0.4125 | 0.3975 | 0.3725 | 0.3150 | 0.2850 | 0.2375 |

**It fails far more widely than the keytracking round found.** The impulse rows are that round's finding. With a loud input the failure starts much lower: from 0.315 fs (cubic, k 6, drive 16), within Resonance 10 and Drive 10 under F5 and F8, and from 0.24 fs at k 64. Over the grid below, the as-built solver leaves 23% (tanh) and 26% (cubic) of all samples unconverged. It even fails at k 0 above 0.47 fs at high drive.

**The clamp can't fix it.** Avoiding every failure up to k 64 and drive 32 needs the cutoff below 0.235 fs: 10.4 kHz at 44.1 kHz, 12.9 semitones lost. Within the knobs' ranges (k ≤ 6, drive ≤ 16) it needs 0.3125 fs: 13.8 kHz at 44.1 kHz and 15.0 kHz at 48 kHz, 8 semitones lost, and F3's top (20.5 kHz) cut by 6.9 and 5.4 semitones. The developer's own cutoff reaches half the sample rate.

### Every candidate over the grid

fc/fs 0.30, 0.35, 0.40, 0.42, 0.44–0.49 by 0.01, and 0.495. k 0, 2, 3.9, 4.05, 4.4, 5, 6, 8, 16, 32, 64; drive 1, 4, 16, 32; 44.1, 48, 96, and 192 kHz. That is 1,936 cells per saturator, each a 110 Hz saw at −6 dBFS (times the drive) for 2,048 samples from rest, 3.96 million samples in all. Columns:

- **Unconverged**: samples the solver ended without a step under 10⁻⁵.
- **Off**: cells with a sample further from the reference than 10⁻⁴ of the size of its equations' terms (1 + max|s_i| + 2g, which sets float's rounding there). The reference is solved in double from the candidate's own state, so this is the solver's error alone, not a diverged trajectory.
- **Iterations**: residual evaluations plus the nested solve's rows, mean / max.
- **Stage evaluations**: the nested solve's scalar stage evaluations, per sample.

| Solver | Saturator | Unconverged | Cells off | Iterations | Fell back / stage evaluations | Largest distance: absolute / relative |
|---|---|---|---|---|---|---|
| As built | tanh | 22.9% | 1,124 | 4.17 / 8 | — | 3.8·10³ / 31 |
| | cubic | 26.4% | 1,278 | 4.14 / 8 | — | 3.2·10⁴ / 250 |
| Damped Newton, 8 | tanh | 10.4% | 576 | 4.06 / 8 | — | 1.5·10⁴ / 78 |
| | cubic | 16.3% | 880 | 4.08 / 8 | — | 6.1·10⁴ / 440 |
| Damped Newton, 32 | tanh | 1.2% | 217 | 4.91 / 32 | — | 1.6·10⁴ / 100 |
| | cubic | 3.7% | 456 | 5.74 / 32 | — | 5.2·10⁴ / 260 |
| Step-limited Newton | tanh | 4.3% | 613 | 3.86 / 8 | — | 1.7·10³ / 3.8 |
| | cubic | 5.8% | 706 | 3.77 / 8 | — | 2.7·10³ / 5.3 |
| Newton from the previous solution | tanh | 38.9% | 1,303 | 5.22 / 8 | — | 1.6·10⁴ / 53 |
| | cubic | 50.6% | 1,501 | 5.45 / 8 | — | 4.8·10⁴ / 350 |
| Newton in the saturators' outputs | tanh | 8.4% | 567 | 3.74 / 8 | — | 1.8·10⁴ / 130 |
| | cubic | 22.2% | 1,059 | 3.93 / 8 | — | 7.1·10⁴ / 110 |
| Newton + nested fallback, from Newton's iterate | tanh | **0** | **0** | 4.23 / 33 | 23.9% / 19.4 | 3.3·10⁻³ / 8.7·10⁻⁵ |
| | cubic | **0** | **0** | 4.29 / 35 | 25.4% / 19.6 | 1.4·10⁻³ / 8.1·10⁻⁵ |
| **Newton + nested fallback, from the guess** | tanh | **0** | **0** | 3.89 / 33 | 23.9% / 15.5 | 2.6·10⁻³ / 8.6·10⁻⁵ |
| | cubic | **0** | **0** | 3.71 / 32 | 25.4% / 13.9 | 1.3·10⁻³ / 8.2·10⁻⁵ |
| Nested solve alone | tanh | **0** | **0** | 3.31 / 30 | — / 52.0 | 2.7·10⁻³ / 8.5·10⁻⁵ |
| | cubic | **0** | **0** | 3.18 / 29 | — / 43.9 | 1.7·10⁻³ / 8.3·10⁻⁵ |

- **Only the nested family converges everywhere**, to within 8.7·10⁻⁵ of each equation's scale of the reference (the largest absolute distances, 1–3·10⁻³, are at k 64 and drive 16–32, where the states are in the tens). Every globalised Newton variant fails on 1–51% of the samples.
- **The fallback is needed on a quarter of this grid's samples**, all in the region where Newton fails; the report's map (cells off by fc/fs and k) is the same region as the scan above. Restarting it from the linearised guess takes a fifth fewer stage evaluations than continuing from Newton's diverged iterate.

### The held cutoff

An impulse of 0.5 with the cutoff held at 0.495 fs, k 4.05 to 64, Warm on and off, 44.1 and 48 kHz: the number of the seven k at which a solver matches the reference's level within 0.1 dB, pitch within 1 cent, and peak within 10%. At 0.44–0.48 fs (48 kHz) the same, out of 21 per saturator.

| Solver | 0.495 fs: tanh 48 / 44.1 kHz | cubic 48 / 44.1 kHz | 0.44–0.48 fs: tanh | cubic |
|---|---|---|---|---|
| As built | 1 / 1 | 0 / 0 | 8 | 4 |
| Damped Newton, 8 / 32 | 1 / 1 | 0 / 0 | 17 / 20 | 11 / 16 |
| Step-limited Newton | 2 / 2 | 1 / 1 | 21 | 18 |
| Newton from the previous solution | 3 / 3 | 2 / 2 | 12 | 8 |
| Newton in the saturators' outputs | 1 / 1 | 0 / 0 | 15 | 5 |
| **Both nested fallbacks, the nested solve alone** | **7 / 7** | **7 / 7** | **21** | **21** |

Where the others fail at 0.495 fs they lock at +12 to +48 dBFS, or (step-limited) sit at −3 to −7 dBFS hundreds of cents off. The nested family matches the reference to the printed digit everywhere (for example k 8, tanh: −11.8 dBFS and −0.8 cents, peak 0.330 against 0.330).

### The ordinary case

On the filter report's own material: the THD table's 220 Hz sine through LP24 at 10 kHz at drive 1, 2, 4, 8, and 16, and the solver-distance sweeps (a 110 Hz saw, the cutoff 100 Hz → 8 kHz → 100 Hz over 2 s, hard: k 3.8, drive 4; gentle: k 2, drive 2):

| Solver | Against the as-built solver | Against the reference, hard / gentle (tanh) |
|---|---|---|
| As built | (itself; it is `LadderFilter` bit for bit) | −126.2 / −128.4 dB |
| Damped Newton, step-limited Newton, both nested fallbacks | **identical, bit for bit**, every case, both saturators | −126.2 / −128.4 dB |
| Newton from the previous solution | −130 to −135 dB | −126.2 / −128.5 dB |
| Newton in the saturators' outputs | −99 to −121 dB | −105.7 / −98.7 dB |
| Nested solve alone | −100 to −114 dB | −99.7 / −109.3 dB |

Where Newton converges, the fallback never runs, so **the fallback leaves every existing figure in filters.md unchanged, bit for bit**: the response, threshold, self-oscillation, passband, harmonics, aliasing, and the solver's −126 dB from the reference. (With the cubic it is −128.7 / −130.7 dB.) The nested solve alone, used everywhere, would move them by −100 dB: inaudible, but no longer bit for bit, at 5× the cost.

### CPU

ns per sample, one channel, 48 kHz, `relwithdebinfo`, Apple M1 Ultra. Normal: the filter benchmarks' settings (LP24 at 1 kHz, k 2, drive 2, a saw). Corner: the cutoff held at 0.495 fs, k 8, self-oscillating. These run in the test wrapper (`LadderFilter` itself measures 100.3 ns at the normal settings against the wrapper's 112.9 as built):

| Solver | tanh: normal | tanh: corner | cubic: normal | cubic: corner |
|---|---:|---:|---:|---:|
| As built (the corner fails) | 112.9 | 244.1 | 73.6 | 260.3 |
| Damped Newton, 8 / 32 | 105.4 / 101.6 | 252.6 / 734.5 | 75.4 / 72.9 | 195.1 / 443.5 |
| Step-limited Newton | 109.6 | 421.5 | 77.5 | 312.2 |
| Newton from the previous solution | 100.9 | 210.7 | 69.8 | 226.7 |
| Newton in the saturators' outputs | 133.8 | 174.3 | 111.6 | 232.1 |
| Newton + nested fallback, from Newton's iterate | 113.4 | 3,156.6 | 70.4 | 1,481.7 |
| **Newton + nested fallback, from the guess** | **114.1** | **1,949.5** | **79.8** | **889.7** |
| Nested solve alone | 568.2 | 1,926.7 | 309.0 | 872.6 |
| Newton with ADAA on the input | 194.4 | 76.0 | 178.0 | 414.9 |
| As built, input sanitised (NaN option 1) | 111.4 | 241.4 | 68.6 | 247.8 |
| As built, state checked per block (NaN option 2) | 113.4 | | | |

- **In the ordinary case the fallback costs nothing**: it doesn't run, and the residual compare per iteration is within noise (114.1 against 112.9 ns; the runs vary by ±5%).
- **At the corner it costs 8× (tanh) and 3.4× (cubic) what the failing solver did**: about 1.9 µs and 0.9 µs per channel-sample, where every sample falls back. Over the grid it falls back on a quarter of the samples, only where the cutoff is high and the drive or resonance large. One stereo voice with both filters parked there at Warm on is 4 × 1.95 = 7.8 µs of the 20.8 µs a sample has at 48 kHz. That is the price of being correct there, and the build (Round 2) should bring it down: the prototype's root finder is generic (two divisions per evaluation and lambdas), the first two Newton iterations at the corner are wasted, and the nested rows could start from a better y4. A worst case of about 4× the as-built corner (1 µs) is a reasonable target.
- **Both NaN guards are free** (within noise).

### Extreme settings

48 kHz. As built: `LadderFilter`. Chosen: Newton with the nested fallback from the guess, in the test wrapper.

**Cutoff modulation across the clamp**: a full-scale 110 Hz saw, the cutoff 12 kHz plus 1.5·sin(2π·300 t) octaves (up to 34 kHz, clamped at 0.495 fs), 1 s:

| Saturator | k | Drive | As built, peak | Chosen, peak | Chosen, unconverged |
|---|---|---|---|---|---|
| tanh | 0 / 4.4 / 8 / 64 | 1 | 1.65 / 0.47 / **15.0** / **296.9** | 1.28 / 0.42 / 0.72 / 1.80 | 0 |
| tanh | 0 / 4.4 / 8 / 64 | 32 | **141.6 / 321.0 / 326.1 / 351.2** | 6.11 / 5.83 / 3.85 / 2.58 | 0 |
| cubic | 0 / 4.4 / 8 / 64 | 1 | **118.3** / 0.55 / **25.6** / **215.8** | 1.27 / 0.47 / 0.90 / 1.97 | 0 |
| cubic | 0 / 4.4 / 8 / 64 | 32 | 3.36 / **252.3 / 294.3 / 311.2** | 3.45 / 3.46 / 3.45 / 2.53 | 0 |

**The cutoff at 1 Hz, DC, and the ceilings** (2 s; the bound is the loop's DC gain plus a self-oscillation under 0 dBFS, drive·max|x|/(1 + k) + 1):

| Case | As built, peak (tanh / cubic) | Chosen | Bound |
|---|---|---|---|
| 1 Hz, k 0, 4.4, 64, a full-scale saw at drive 32 | 0.003 | the same | 1.5–33 |
| 1 Hz, k 64, DC 0 → +10 → −10, drive 32 | 0.65 / 0.68 | the same | 5.92 |
| 1 Hz, k 64, an impulse: self-oscillation, the last 10 s of 20 | | −10.2 / −10.0 dBFS, peak 0.31 | |
| 1 kHz, k 64, DC ±10, drive 32 | 4.17 / 1.50 | the same | 5.92 |
| 1 kHz, k 64, saw, drive 32 / drive 1000 | 0.63 / 1.69 (tanh) | the same | 1.49 / 16.4 |
| 0.495 fs, k 64, DC ±10, drive 32 | **343.7 / 256.9** | 4.99 / 4.06 | 5.92 |
| 0.495 fs, k 64, saw, drive 1000 | **299.4 / 271.9** | 6.52 / 3.62 | 16.4 |

Away from the failing region the built filter is bounded at the ceilings, at 1 Hz (where it self-oscillates at 1 Hz with k 64), and with DC of ±10. Every unbounded case is the solver failure.

**NaN and inf** (a saw at −6 dBFS, k 4.4, 1 kHz; one bad sample at 1000, then 47,000 more):

| Bad sample | As built: non-finite outputs after it | After `reset()` | Input sanitised | State checked per block |
|---|---|---|---|---|
| NaN, +∞, −∞ | **46,999 of 46,999**: the state is poisoned for good | finite | none | 24 (to the end of that block) |

Every setter given NaN, +∞, or −∞ mid-run gives no non-finite output (the clamps of filters.md hold).

**Block sizes at the corner**: blocks of 1 and 37 against 4096 at 0.495 fs and k 8 are bit-identical, even as built while it fails. The failure is deterministic.

**Responses and bypass switched while self-oscillating** (k 8, the response changed every 2,000 samples through all six, bypass toggled every 3,100, 5 s): peak 1.21 (tanh) and 1.28 (cubic) at 440 Hz and 5 kHz, within the bound (16 × the oscillation's peak), no non-finite output. At 0.44 fs, 377 and 184: the solver failure again.

**Five minutes of self-oscillation** (an impulse, then silence): the spread of the level and pitch over the five minutes is at most 0.006 dB and 0.000 cents as built at 440 Hz (k 4.4 and 8), and 0.010 dB and 0.000 cents for the chosen solver at 0.495 fs and k 8. Nothing drifts or grows.

### What the voice can request

The DSP clamps k to [0, 64], drive to [0, 1000], the cutoff to [1 Hz, 0.495 fs], and its modulation to ±40 octaves (filters.md). What reaches it depends on M1.5 and M1.9 (F24):

- **Under the knob-law hypotheses with each control clamped to its knob's range**: k ≤ 6 (F5), drive ≤ 16 (+24 dB, F8), the cutoff up to 20.5 kHz (F3) plus keytrack (up to 9.8 octaves above the reference at MIDI 127) and modulation, so up to the 0.495 fs clamp at any rate.
- **Without that clamp**: every modulation adds in knob units, and there are up to 16 destination slots at ±10 each (2 mod envelopes, 2 LFOs, and 4 misc sources, two destinations each), plus the Filter Control's Resonance (F12). So k reaches the DSP's 64, and so does drive if it is a destination (unknown).

Either way the solver must be correct up to the DSP's ceilings, because they are the contract. The as-built solver fails inside the knobs' ranges already (F5 and F8: from 0.315 fs).

### Aliasing

**At the developer's corner** (high resonance, high drive): LP24 at 5 kHz, k 3.9, a 2.5 kHz sine at −6 dBFS, 44.1 kHz. The strongest alias below 20 kHz (dB re the fundamental), as built (the solver converges here):

| Filter | Drive 1 | 4 | 16 | 32 |
|---|---:|---:|---:|---:|
| tanh, 1× / 2× / 4× | −142.9 / −144.9 / −141.1 | −110.3 / −153.9 / −159.0 | −31.6 / −57.0 / −110.3 | −24.7 / −37.1 / −65.8 |
| cubic, 1× / 2× / 4× | −142.7 / −141.5 / −139.4 | −151.1 / −154.4 / −150.9 | −30.2 / −48.5 / −68.3 | −23.9 / −36.4 / −55.1 |

**Self-oscillating** (k 6, an impulse and no input, 44.1 kHz; the chosen solver, since the as-built one fails at F3's top): the strongest component that isn't a harmonic of the oscillation, dB re the oscillation, at its measured pitch:

| Filter | Cutoff 5 kHz | 10 kHz | 15 kHz | 20.5 kHz (F3's top, 0.465 fs) |
|---|---:|---:|---:|---:|
| tanh, 1× / 2× / 4× | −64.8 / −105.5 / −90.5 | −32.6 / −64.8 / −99.0 | −90.7* / −31.3 / −89.2 | −16.9 / −31.5 / −64.9 |
| cubic, 1× / 2× / 4× | −54.8 / −69.1 / −89.7 | −35.2 / −54.8 / −69.1 | −90.7* / −35.2 / −56.8 | −15.0 / −30.4 / −55.1 |

\* At 1× and a 15 kHz cutoff the oscillation locks to exactly fs/3 (14,700 Hz), so its harmonics fold onto itself and DC: there is nothing inharmonic to measure. At 2× it runs free (14,239 Hz) and aliases at −31 dB.

- **These are the 2007 aliasing, and F10's decision covers them.** Drive 16 at high resonance aliases at −31 dB at 1×, −57 at 2×, and −110 at 4× (tanh), as the filter round found at k 3. A self-oscillation is a saturated tone, so its harmonics fold as an oscillator's would: −33 dB at a 10 kHz cutoff and −17 dB at 20.5 kHz at 1×. At 20.5 kHz every harmonic is above Nyquist, so any saturated oscillation there aliases at 1×, whatever the solver. 4× (the recommended Render setting) brings it to −65 dB with tanh. The cubic's kink keeps it higher (−55 dB at 4×), as filters.md found for drive.
- **Nothing here demands reopening F10.** The aliasing grows where the developer said it does, 2× and 4× remove it in proportion, and the voice-level oversampling (M1.8) is the planned cure.

**ADAA on the loop's input saturator** (1×, 44.1 kHz):

| Solver | Saturator | Alias, k 0 at 15 kHz: drive 4 / 16 | k 3.9 at 5 kHz: drive 4 / 16 | Gain at a 1 / 10 kHz cutoff | Threshold at 1 / 7 kHz | Pitch at k 4.4, 440 Hz / 7 kHz |
|---|---|---|---|---|---|---|
| As built | tanh | −47.4 / −33.2 dB | −110.3 / −31.6 dB | 0.00 / 0.00 dB | 4.000 / 3.996 | −35.8 / −31.0 cents |
| ADAA | tanh | −48.1 / −40.7 | −96.6 / −70.9 | −0.02 / −2.42 | 3.747 / 3.005 | −72.2 / −419.2 |
| As built | cubic | −59.4 / −31.8 | −151.1 / −30.2 | 0.00 / 0.00 | 4.000 / 3.996 | −32.3 / −28.0 |
| ADAA | cubic | −26.7 / −22.1 | −2.0 / −47.0 | −0.02 / −2.42 | 3.747 / 3.005 | −158.6 / −426.0 |

ADAA takes up to 39 dB off the strongest alias at k 3.9 and drive 16 with tanh. But its half-sample delay inside the loop does what the theory says. The threshold falls to 3.0 at 7 kHz, so k 3.9 self-oscillates there: that is the cubic's −2.0 dB "alias", an oscillation. The self-oscillation goes 4 semitones flat at 7 kHz, which breaks keytrack 10's intervals. The response at the cutoff is no longer exact (−2.4 dB at 10 kHz). With the cubic, aliasing gets *worse* at k 0 (−27 against −59 dB at drive 4). And it costs 72% (tanh) to 142% (cubic) more in the ordinary case. It undoes the exactness the ZDF ladder was chosen for, so it is not proposed. Oversampling the voice is the measure.

## Recommendations

*For the project owner's decision. Round 2 builds what is chosen, test-first.*

| Question | Recommendation | The figure behind it |
|---|---|---|
| **The solver** | **Keep Newton as built, and fall back to the nested bracketing solve** (rtsafe on the last row over [s4 − 2g, s4 + 2g], each stage rtsafe over [c − g, c + g] from its hard-clip model, stopping at its rounding level) **on the first Newton iteration that doesn't reduce ‖F‖², restarting from the linearised guess.** Its caps (64 rows, 64 evaluations per stage) bound the worst case; on the grid the worst sample took 33 iterations in all. | The only family with no unconverged sample on the grid (3.96 million samples, k to 64, drive to 32, 0.30–0.495 fs, 44.1–192 kHz), every sample within 8.7·10⁻⁵ of its equations' scale of the double reference, and the held cutoff matched in all 28 cases at 0.495 fs and all 42 at 0.44–0.48 fs. Bit-identical to today wherever Newton converges, so every filters.md figure stands. CPU: unchanged in the ordinary case (114 against 113 ns); 1.95 µs (tanh) and 0.89 µs (cubic) per channel-sample at the corner, where the as-built solver fails at 0.24–0.26 µs. Round 2 target: about 1 µs at the corner. |
| **The rejected fixes** | No line search, step limit, previous-sample guess, change of variable, or nested-only solve; **no lower clamp**. | Damped Newton (32): 1.2–3.7% unconverged, the held cutoff 1 of 7. Step limit: 4–6%, bounded but wrong (−3 to −7 dBFS, hundreds of cents off). Previous guess: 39–51%. Output variable: 8–22%. Nested alone: correct but 5× the cost everywhere and not bit for bit (−100 dB). A clamp safe at every setting is 0.235 fs (12.9 semitones lost); within the knobs' ranges, 0.3125 fs (F3's top cut by 6.9 semitones at 44.1 kHz). |
| **The k and drive ceilings** | **Keep the DSP's ceilings (k ≤ 64, drive ≤ 1000) and require correctness up to them.** **Clamp each control plus its modulation to the knob's range in the parameter layer** (M1.5/M1.9; F24's hypothesis), so the voice asks for k ≤ 6 and drive ≤ 16 under F5 and F8. | The chosen solver is bounded and converged at k 64 with drive 32 over the grid, and at drive 1000 (peak 6.5 against a bound of 16.4). Without a parameter-layer clamp, 16 modulation slots at ±10 knob units take k past 64. Lowering the DSP ceilings would buy nothing the solver needs. Above k 16 the equations lock to fs/4 (−6.7 dBFS at 0.30 fs), which is outside the knobs' range anyway. |
| **NaN handling** | **Replace a non-finite input sample by 0 before the solve** (option 1). Keep the setters' clamps. | Today one NaN, +∞, or −∞ sample poisons every later output (46,999 of 46,999) until `reset()`. Sanitised: no non-finite output. Per-block state check: 24 non-finite samples to the block's end. Both are within noise for CPU (111.4 and 113.4 against 112.9 ns). The input guard never lets a non-finite value reach the state, so it is the simpler contract: finite in, finite out, whatever the input. |
| **Aliasing** | **No filter-local measure. F10 stands**: the filter runs at whatever rate it is prepared at; the v3 options oversample the voice (M1.8), with Render at 4×. **No ADAA in the loop.** | At the developer's corner (k 3.9, drive 16) the strongest alias is −31.6 / −57.0 / −110.3 dB at 1× / 2× / 4× (tanh). Self-oscillating at F3's top it is −16.9 / −31.5 / −64.9 dB, inherent to a saturated tone above fs/4. ADAA on the input: the threshold falls to 3.0 at 7 kHz, the pitch moves −419 cents, the gain at a 10 kHz cutoff −2.4 dB, the cubic aliases worse, and it costs 72–142% more. |
| **Test thresholds (Round 2)** | (1) **The held cutoff**: an impulse of 0.5, the cutoff at 0.44, 0.46, 0.48, and 0.495 fs, k 4.05 to 8 (and 16, 64), Warm on and off, 44.1 and 48 kHz. Level within 0.1 dB and pitch within 1 cent of the reference's, peak under 1. (2) **The grid**: no unconverged sample, and every sample within 2·10⁻⁴ of its equations' scale of the double reference. (3) **The extremes**: finite and within drive·max\|x\|/(1 + k) + 1 under audio-rate modulation across the clamp, DC ±10, drive 1000 at k 64, at 0.495 fs. (4) **NaN**: one NaN, ±∞ input sample changes nothing but that sample (the output equals the filter's with 0 there, bit for bit). (5) **Long run**: five minutes self-oscillating at 0.495 fs, k 8, level spread under 0.05 dB and pitch under 0.01 cents. (6) **Unchanged**: every existing filter test and figure, bit for bit where Newton converges. | Today's solver fails (1) at every k but 4.05 with tanh and at all of them with the cubic: that is the red. Each tolerance is to be derived in the test. For (1): the reference and the chosen solver agree to the printed 0.1 dB and 0.1 cent, and a stable limit cycle doesn't accumulate per-sample error. For (2): float rounding of terms of size 1 + max\|s\| + 2g is about 10⁻⁷ of it, and the 10⁻⁵ step tolerance, propagated by g, gives up to 10⁻⁴; measured 8.7·10⁻⁵, so Round 2 derives the bound before fixing 2·10⁻⁴. For (3), measured: 6.1 against 33, 5.0 against 5.9, 6.5 against 16.4. For (5), measured: 0.010 dB and 0.000 cents. |

## References

- W. H. Press, S. A. Teukolsky, W. T. Vetterling, and B. P. Flannery, *Numerical Recipes in C*, 2nd ed., Cambridge, 1992, §9.4: Newton–Raphson with bisection (rtsafe) and its "not decreasing fast enough" rule.
- J. E. Dennis and R. B. Schnabel, *Numerical Methods for Unconstrained Optimization and Nonlinear Equations*, SIAM, 1996, ch. 6: line searches and trust regions for Newton's method, and why they can stall.
- V. Zavalishin, *The Art of VA Filter Design*, rev. 2.1.2, 2018: the trapezoidal ladder and solving its nonlinear zero-delay loop.
- J. Parker, V. Zavalishin, and E. Le Bivic, "Reducing the aliasing of nonlinear waveshaping using continuous-time convolution", *Proc. DAFx*, 2016.
- S. Bilbao, F. Esqueda, J. D. Parker, and V. Välimäki, "Antiderivative antialiasing for memoryless nonlinearities", *IEEE Signal Processing Letters* 24(7), 2017.
- IEEE 754-2019, *Standard for Floating-Point Arithmetic*: NaN propagation and unordered comparisons.
- The theory of the ladder itself: [filter-theory.md](filter-theory.md#references).
