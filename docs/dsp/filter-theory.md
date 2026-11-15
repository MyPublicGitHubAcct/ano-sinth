# Filter theory: ladders, zero-delay feedback, and saturating stages

This page is the theory behind the filter round (M1.3). [filters.md](filters.md) holds the evidence about Sylenth1, the candidate models, what each one measured, and the decision. This page covers:

- what a filter's poles, slope, cutoff, and resonance are;
- the four-pole ladder: why its feedback makes a resonant peak, why it self-oscillates at a loop gain of exactly 4, and why its passband drops as resonance rises;
- how one ladder gives six responses (lowpass, bandpass, and highpass at 12 and 24 dB/oct) by mixing its stage outputs;
- the transistor ladder's saturating stages, what drive does to them, and why a saturating ladder's self-oscillation goes flat as resonance rises;
- discretisation: why a delay in the feedback loop detunes the filter, the bilinear transform and prewarping, the topology-preserving transform (TPT) and zero-delay feedback (ZDF), and three ways to solve a nonlinear ZDF loop;
- Huovilainen's explicit oversampled ladder, and how to tune it exactly;
- stability, denormals, and why saturators alias.

Read [oversampling.md](oversampling.md) and [fast-math.md](fast-math.md) first. [measuring-dsp.md](measuring-dsp.md) explains the measurements quoted here.

## Poles, slope, cutoff, resonance

A linear filter's transfer function H(s) is a ratio of polynomials in the complex frequency s. Its response to a sine at angular frequency ω is H(jω): the magnitude |H| scales the sine and the argument shifts its phase. The roots of the denominator are the **poles**. A filter is stable when every pole has a negative real part, so every transient decays.

The simplest lowpass is the **one-pole**, an RC circuit:

```text
H(s) = 1 / (1 + s/ωc)
```

At the cutoff ωc its gain is 1/√2 (−3 dB) and its phase −45°. Far above the cutoff the gain falls by half for each doubling of frequency, **6 dB per octave**. Each pole adds 6 dB/oct, so four poles give 24 dB/oct and two give 12. From here on, s is normalised to the cutoff (s = jf/fc).

**Resonance** is a peak in the response near the cutoff. It comes from poles close to the jω axis, which ring for a long time. A filter's poles can be pushed there by its feedback, as in the ladder, or by its damping, as in the state-variable filter. When they reach the axis the filter **self-oscillates**: it rings forever and makes a tone with no input. Beyond that, a linear filter's ringing grows without limit, so a filter that goes "way beyond self-oscillation" needs something nonlinear to bound it.

## The four-pole ladder

Robert Moog's transistor ladder (US patent 3,475,623, 1969) is four identical one-pole lowpasses in series, with the last stage's output fed back, inverted, to the input:

```text
            +---------------------------- k ------------------------------+
            |                                                              |
x ---> (+) -+-> u -> [ 1/(1+s) ] -> y1 -> [ ] -> y2 -> [ ] -> y3 -> [ ] -> y4
         -
```

With u = x − k·y4 and y4 = u/(1 + s)⁴:

```text
LP24(s) = y4 / x = 1 / ((1 + s)⁴ + k)
```

Three properties follow, and each one shows up in Sylenth1's evidence:

- **Self-oscillation at k = 4, at the cutoff.** The poles satisfy (1 + s)⁴ = −k, so 1 + s = k^¼ · e^(±jπ/4) (and the other two roots). They reach the jω axis when k^¼ · cos 45° = 1, that is at k = 4, and then s = ±j: exactly the cutoff. So the loop gain sets the resonance, and the threshold is the same at every cutoff.
- **The peak's height is 1/|4 − k|.** At s = j, (1 + j)⁴ = −4, so LP24(j) = 1/(k − 4). With no feedback it is 1/4 (−12 dB, two −3 dB poles twice over), so the response at the cutoff is attenuated: Sylenth1's developer: "At the cutoff frequency it will always have some attenuation" (inventory §3). At k = 3.9 it is 10 (+20 dB), and at k = 3.99, 100: "High levels of resonance means the signal is amplified hundreds of times."
- **The passband drops with resonance.** At s = 0, LP24 = 1/(1 + k): −6 dB at k = 1, −14 dB at k = 4. The feedback subtracts the output from the input, and at low frequencies the output is the input. A design can make this up (a gain of 1 + k somewhere), or keep it, as the analog circuit does.

### Six responses from one loop

The four stage outputs, and the loop input u, are all available. Writing p = 1 + s and D = p⁴ + k:

```text
u = x · p⁴ / D      y1 = x · p³ / D      y2 = x · p² / D      y3 = x · p / D      y4 = x / D
```

Any fixed mix Σ cᵢ yᵢ (y0 = u) has the transfer function (Σ cᵢ p⁴⁻ⁱ) / D: the numerator is whatever polynomial in p the weights spell out, and the denominator, with its resonance, is shared. The Oberheim Xpander and Matrix-12 got their filter modes this way. With s = p − 1:

| Response | Mix | Numerator | At k = 0 |
|---|---|---|---|
| LP24 | y4 | 1 | 1/(1+s)⁴ |
| LP12 | y2 | p² | 1/(1+s)², 12 dB/oct |
| BP24 | 4(y2 − 2y3 + y4) | 4(p − 1)² = 4s² | 4s²/(1+s)⁴, peak 1 at the cutoff, 12 dB/oct each side |
| BP12 | 2(y1 − y2) | 2p(p − 1) | 2s/(1+s)², peak 1, 6 dB/oct each side |
| HP24 | u − 4y1 + 6y2 − 4y3 + y4 | (p − 1)⁴ = s⁴ | s⁴/(1+s)⁴ |
| HP12 | u − 2y1 + y2 | p²(p − 1)² | s²/(1+s)² |

Every response shares the loop, so every one resonates at the cutoff and self-oscillates at k = 4. The highpasses' passband doesn't drop with resonance: at high frequencies their numerator and the denominator both tend to s⁴. The 12 dB modes are still four-pole filters with 12 dB/oct skirts: LP12 with feedback is p²/(p⁴ + k), not a two-pole resonator. A two-pole loop of identical poles can't self-oscillate at all (its poles −1 ± j√k never reach the axis), which is one reason to keep the 12 dB modes inside the four-pole loop rather than build a separate two-pole filter.

The weights are data: a new response is a row of five numbers ([filter_candidates.hpp](../../tests/dsp/filters/filter_candidates.hpp), `tapTable`).

### The transistor ladder's saturation

In Moog's circuit each stage is a differential pair of transistors, and the current a pair can steer is a hyperbolic tangent of the voltage across its bases. Huovilainen (2004) derived the large-signal model:

```text
dy_i/dt = ωc · ( tanh(y_(i−1)) − tanh(y_i) ),     y_0 = x − k·y4
```

(voltages scaled by twice the thermal voltage). Each stage charges its capacitor with a current set by the difference of two tanh terms, so there are **five distinct saturators per sample**: tanh(u) and tanh(y1) … tanh(y4). Sylenth1's feature list says "Each filter contains 5 independent non-linear saturation functions" (inventory §3).

What the saturators do:

- **They limit the slew, not the level.** At DC a stage settles where tanh(y_i) = tanh(y_(i−1)), so y_i = y_(i−1): a slow signal passes at full level whatever its amplitude. A fast, loud one can't charge the capacitor fast enough, so its harmonics are generated and its level is compressed.
- **Drive** is the level into the saturators: a gain on x. Doubling it pushes the stages further into their curves. With no make-up gain the output grows until the stages limit it.
- **They bound self-oscillation.** Above k = 4 the linear loop grows; the saturators compress the loop gain as the amplitude grows, until it settles where the compressed gain is exactly enough. The oscillation's level is then a property of the saturators and k, not of the input.
- **They make it go flat.** A saturator driven by a large sine of amplitude A passes the fundamental with an effective gain N(A) < 1 (its *describing function*). Each stage's own term ωc·tanh(y_i) then behaves like ωc·N(A)·y_i, a one-pole whose cutoff is lowered to about ωc·N(A). So the louder the oscillation, the lower its pitch, and the further k is past 4, the louder it is. In [filters.md](filters.md) a ladder with a saturator in every stage measures −5 cents at k = 4.05, −36 cents at 4.4, and −126 cents at 8, the same at every cutoff. A ladder that saturates only at the loop's input (linear stages, tanh(x − k·y4)) keeps its poles where they are and measures within 4 cents at any k: the input saturator limits the loop gain without moving the cutoff.

The saturator's shape sets the harmonics. tanh is odd and smooth, so it makes every odd harmonic, falling steadily. A cubic soft clip, x − (4/27)x³ up to |x| = 1.5, makes only the third harmonic until it reaches its knee (and the chain of stages then mixes it into higher ones). x/√(1 + x²) is like tanh with a slower knee. Moving tanh off its centre (tanh(x + b) − tanh(b)) breaks the symmetry and adds even harmonics, the "tube" sound.

## Discretisation

A plugin runs at a sample rate fs. The continuous system has to become a recursion on samples, and the choice of how matters more for a resonant filter than for almost anything else, because resonance depends on the exact phase around the loop.

### A delay in the loop

The obvious method keeps each stage's own discretisation and closes the loop with the previous sample's output, u[n] = x[n] − k·y4[n − 1]. That unit delay adds a phase lag of ω/fs radians around the loop. At the cutoff the four stages contribute −180° and the delay pushes the total past −180° earlier, so the loop reaches −180° below the cutoff (the resonance is flat) and with more gain (the threshold is lower than 4). Both errors grow with fc/fs. Measured on a prototype with exact stages and a delayed loop (filters.md): −53 cents at 440 Hz and −563 cents at 7 kHz at 44.1 kHz, and a threshold of 3.94 at 110 Hz falling to 2.18 at 7 kHz, so at k = 3 the filter already oscillates above 3.5 kHz. Older designs fix this with tables of corrections for the cutoff and the resonance (Stilson and Smith, 1996). The other way is to remove the delay.

### The bilinear transform and prewarping

The **bilinear transform** replaces each integrator 1/s with the trapezoidal rule:

```text
s  →  (2/T) · (z − 1)/(z + 1)
```

It maps the whole jω axis onto the unit circle, so a stable analog filter stays stable and its response is kept exactly, but with frequency warped: the digital response at f equals the analog one at

```text
f_analog = (fs/π) · tan(π f / fs)
```

which is close to f at low frequencies and reaches infinity at Nyquist. Warping is fixed at one frequency by **prewarping**: design the analog filter with its cutoff at (fs/π)·tan(π·fc/fs), so the digital filter has its cutoff, and anything defined there, exactly at fc. For the ladder that includes the self-oscillation frequency and the threshold: a prewarped trapezoidal ladder self-oscillates at exactly fc with k exactly 4, at every cutoff and sample rate (measured: 4.000).

The price is **cramping** near Nyquist. The analog response from fc to infinity is squeezed into fc to fs/2, and a lowpass gets zeros at Nyquist (four of them for LP24). With fc at 10 kHz and fs at 44.1 kHz, LP24 falls up to 44 dB below the analog prototype by 20 kHz. Sylenth1's developer described the same: a lowpass at its maximum cutoff (half the sample rate) still cuts some highs; "use a higher samplerate". Oversampling by 2 pushes the cramping up an octave (4.3 dB at 10 kHz instead of 44).

### The topology-preserving transform and zero-delay feedback

Zavalishin's **topology-preserving transform** (TPT) applies the trapezoidal rule to each integrator of the block diagram rather than to the overall transfer function, so the digital structure has the same states and feedback paths as the analog one. A trapezoidal integrator with gain g = tan(π·fc/fs), in transposed direct form II, keeps one state s:

```text
v = g·(x − y)        (the one-pole's integrator input)
y = s + v            ⇒   y = (g·x + s) / (1 + g) = G·x + s/(1 + g),   G = g / (1 + g)
s ← y + v = 2y − s
```

The output depends on the current input **instantaneously** (through G), so a loop of these stages has no delay in it: a **zero-delay feedback** (ZDF) loop. It can't be computed in order, because u needs y4 and y4 needs u, but for a linear loop it can be solved. Each stage is y_i = G·y_(i−1) + s_i/(1 + g), so

```text
y4 = G⁴·u + S,      S = G³·s1/(1+g) + G²·s2/(1+g) + G·s3/(1+g) + s4/(1+g)
u  = x − k·y4   ⇒   u = (x − k·S) / (1 + k·G⁴)
```

Then the stages run in order from u. Because the states are the analog integrators' (capacitor charges), changing g between samples changes the cutoff the way turning a knob on the circuit would: the state is kept and nothing jumps. That is why TPT filters are well behaved under fast modulation.

### Solving a nonlinear ZDF loop

With saturators the loop equations are implicit and nonlinear. For the trapezoidal transistor ladder, with tᵢ = tanh(yᵢ) and t₀ = tanh(x − k·y4):

```text
F_i(y) = y_i − s_i − g·( t(y_(i−1)) − t(y_i) ) = 0,     i = 1..4
```

Three ways to solve it each sample, all prototyped:

- **Newton–Raphson.** Iterate y ← y − J⁻¹F. The Jacobian is lower bidiagonal (stage i depends on stage i − 1) plus one corner (stage 1 depends on y4 through the feedback), so writing each Δᵢ as pᵢ + qᵢ·Δ4 and solving the last row for Δ4 costs a handful of operations: no matrix. Each iteration costs five saturators and their slopes. It converges quadratically near the solution; from a good first guess one or two iterations reach 10⁻⁵ (filters.md: a mean of 1.2–2.2 iterations, at most 5, on hard material). Stopping at a tolerance is deterministic (the same input and state always take the same path), so the output stays bit-identical across block sizes.
- **Linearisation at the state** (Voipio, "cheap non-linear zero-delay filters", 2012). Replace each t(v) by v·t(v̂)/v̂, its secant gain at an estimate v̂ taken from the state before the sample. Each stage becomes linear with a gain of its own, and the loop is solved in closed form as above. Five saturators, a few divisions, no iteration. It is exact when the estimate is, and its error grows with how fast the signal moves within a sample. It is also Newton's natural first guess.
- **Saturation at the loop input only.** Keep the stages linear and put one saturator at u = t(x − k·y4). Then y4 = G⁴·t(x − k·y4) + S is one scalar equation, monotonic in y4, so a scalar Newton iteration converges from the linear solution. It is a different filter, not an approximation: its stages don't saturate, so it keeps its pitch when self-oscillating and passes loud slow signals unclipped only up to the input saturator.

### Huovilainen's explicit ladder

Huovilainen's design (DAFx 2004; Välimäki and Huovilainen, 2006) keeps the nonlinear model and avoids solving anything. Each stage is a forward step, y_i += g·(t(y_(i−1)) − t(y_i)), with t(y_i) remembered from the previous sample, and the feedback is the average of the last two outputs, u = x − k·(y4[n−1] + y4[n−2])/2, a half-sample correction to the unit delay. It is cheap per sample, but it needs oversampling (2× in the paper) to keep the loop's phase errors small, and its tuning drifts with fc/fs.

The drift can be cancelled exactly for small signals. Linearised, each stage is H1(z) = g/(1 − a·z⁻¹) with a = 1 − g, and the loop is k·z⁻¹·(1 + z⁻¹)/2·H1⁴. At ω = 2π·fc/fs it oscillates when its phase is −π:

```text
−ω − ω/2 − 4·atan( a·sin ω / (1 − a·cos ω) ) = −π
⇒ atan( a·sin ω / (1 − a·cos ω) ) = π/4 − 3ω/8 =: φ
⇒ a = tan φ / (sin ω + tan φ · cos ω),   g = 1 − a          (valid for fc < fs/3)
```

and its gain is then 1 when k = 1 / (cos(ω/2) · |H1(e^jω)|⁴). Scaling the resonance by that k over 4 puts the threshold at 4 at every cutoff (measured: 4.000). The paper fits polynomials to the same two corrections. With the naive mapping (g = 1 − e^(−ω), k as given) the threshold measures 4.02 at 110 Hz and 4.79 at 7 kHz (2×, 44.1 kHz).

## Stability

- **Linear.** The trapezoidal rule maps the left half-plane inside the unit circle, so a TPT filter is stable exactly when its analog prototype is: the linear ladder for k < 4. Above that it grows without bound (the linear prototype measures NaN under the stress tests).
- **Nonlinear.** tanh is bounded by 1, so in the transistor model each stage's rate of change is bounded by 2·ωc, and the stages can't run away. Self-oscillation settles at a level set by k and the saturator, and a bounded input gives a bounded output. A cubic soft clip with a flat top (slope 0 beyond its knee) bounds the same way. The Newton solver has a monotonic last row (1 − q4 > 0 whenever the slopes are non-negative), so it never divides by zero.
- **Denormals.** A filter's states decay exponentially after its input stops, and in float they pass through the subnormal range on the way to 0, which is slow on x86. The library's two layers apply ([denormals.md](denormals.md)): flush-to-zero on the audio thread, and `flushDenormal()` on recursive state. The output of a flushed state can still be subnormal for a few hundred samples (a product of two tiny normals), which the FTZ guard handles.

## Why saturators alias

A static nonlinearity turns a sine into its harmonics, and tanh makes infinitely many. Those above fs/2 fold back into the band as **aliases**, at frequencies with no harmonic relation to the note. The harder the drive, the stronger the high harmonics, so aliasing grows quickly with drive. In a ladder the saturators sit inside the loop, before the stages' lowpass, so the stages can't remove what has already folded. Sylenth1's developer described the result in 2007: "a bit of high frequency aliasing when you use high resonance or high drive levels."

The usual cure is **oversampling**: run the nonlinear part at R·fs, so harmonics fold only above R·fs/2, then lowpass and decimate (the library's `Oversampler`: elliptic halfbands, [oversampling.md](oversampling.md)). Each doubling buys roughly the difference between a harmonic's level an octave lower and where it was. On the prototypes at drive 4: −47 dB (1×), −102 dB (2×), −150 dB (4×) for the strongest alias of a 2.5 kHz sine at 44.1 kHz. The cost is the filter's own cost times R, plus the round trip. Antiderivative antialiasing (Parker et al., 2016) is the main alternative for a static saturator, but it doesn't extend simply to saturators inside a ZDF loop; it is a Phase 3 option, not a candidate here.

## The state-variable filter

The trapezoidal **state-variable filter** (SVF) is the other standard resonant filter: two integrators and a damping feedback R, with lowpass, bandpass, and highpass outputs all at once:

```text
LP = 1/(s² + 2Rs + 1)     BP = s/(s² + 2Rs + 1)     HP = s²/(s² + 2Rs + 1)
```

Resonance is low damping, and R = 0 is self-oscillation. It is cheaper than a ladder (two states) and its 12 dB modes are true two-pole responses with a passband that doesn't drop with resonance. It has no four-stage structure, though, so it can't be "four filter stages, each with a saturator", and its 24 dB modes would need two in series with two resonances. The prototype (`LinearSvf`) is a reference: cutoff exact to 0.01 dB, 14 ns per sample. A multimode SVF is in the Phase 2 list.

## References

- R. A. Moog, "Electronic high-pass and low-pass filters employing the base to emitter diode resistance of bipolar transistors", US Patent 3,475,623, 1969.
- T. Stilson and J. O. Smith, "Analyzing the Moog VCF with considerations for digital implementation", *Proc. ICMC*, 1996.
- A. Huovilainen, "Non-linear digital implementation of the Moog ladder filter", *Proc. DAFx*, 2004.
- V. Välimäki and A. Huovilainen, "Oscillator and filter algorithms for virtual analog synthesis", *Computer Music Journal* 30(2), 2006.
- V. Zavalishin, *The Art of VA Filter Design*, rev. 2.1.2, Native Instruments, 2018: the TPT, zero-delay feedback, the ladder and its modes, the SVF, and nonlinear ZDF solutions.
- T. Voipio ("mystran"), "Cheap non-linear zero-delay filters", KVR Audio DSP forum, 2012: linearising each saturator at the state.
- S. D'Angelo and V. Välimäki, "An improved virtual analog model of the Moog ladder filter", *Proc. ICASSP*, 2013.
- A. Gelb and W. E. Vander Velde, *Multiple-Input Describing Functions and Nonlinear System Design*, McGraw-Hill, 1968: the describing function.
- J. Parker, V. Zavalishin, and E. Le Bivic, "Reducing the aliasing of nonlinear waveshaping using continuous-time convolution", *Proc. DAFx*, 2016.
- J. Schattschneider and U. Zölzer, "Discrete-time models for nonlinear audio systems", *Proc. DAFx*, 1999: aliasing from nonlinearities and oversampling.
