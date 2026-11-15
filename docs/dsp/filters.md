# Filters

Each part of a Brutalis voice has a filter: four saturating stages in a resonant loop, with lowpass, bandpass, and highpass responses at 12 and 24 dB/oct, a true bypass, Cutoff, Resonance that goes well past self-oscillation, Drive, and a Warm Drive switch that trades CPU for a richer saturator (inventory [§3](../reference/sylenth1-features.md#3-filters-pp-5-1618)). This page covers M1.3 tasks 1–4, the core and those controls, as one structure:

- what is known about Sylenth1's filter;
- the candidate models prototyped here, and why these;
- what each measured: the linear response against the analog prototype in every mode, self-oscillation (pitch, threshold, level), passband loss, harmonics against drive, aliasing at 1×, 2×, and 4×, the distance from a converged solution, stability, block sizes, clicks and zipper noise, and CPU;
- the decision, and how it is built (`LadderFilter`), measured and tested as built.

Keytracking, the Filter Control's combining law, and the v3 oversampling options are later rounds. Their inputs are measured here anyway: self-oscillation tuning decides how well keytrack 10 can play in tune, and aliasing decides how much oversampling the filter needs.

The theory (ladders, the tap mixes, transistor-ladder saturation, the bilinear transform and prewarping, TPT and zero-delay feedback, Newton and linearised solvers, Huovilainen's explicit ladder and its exact tuning, stability, aliasing) is in [filter-theory.md](filter-theory.md). Read it first.

## Sylenth1: what is known

The full source table, with confidence ratings and the open questions F1–F16, is in the inventory ([§3](../reference/sylenth1-features.md#3-filters-pp-5-1618)). In short:

- **Official:** "4 filter stages with nonlinear saturation incorporated" (manual p. 5); "The filter contains four filter stages and each stage incorporates a non-linear saturation function" (p. 16); Bypass, Lowpass, Bandpass, Highpass at 12 or 24 dB/oct; resonance "way beyond self-oscillation"; Drive overdrives "the internal filter stages"; Warm Drive "makes the filter stages use a higher quality of saturation which introduces more higher harmonics" and off "makes the filters use less CPU power" (p. 18); bypass stops processing (p. 30). The specification: "4-stage stereo filters per note" and drive "in each filter stage". The KVR feature list: "Each filter contains 5 independent non-linear saturation functions." The changelog: v1.1 "more accurate filter cutoff modulation", v1.11 "Eliminated clicking effects caused by the filters", v1.20 "Improved filter cutoff frequency conversion" and "Improved bandpass/highpass filters", v3.040 an option to show the cutoff in Hz, v3.050 "Improved filter response and sound quality" and the oversampling options.
- **The developer (KVR):** in 2007, "a bit of high frequency aliasing when you use high resonance or high drive levels", the cutoff's "frequency conversion" has an accuracy setting, and parameters are interpolated. In 2008, a lowpass at its maximum cutoff still differs from bypass: "At the cutoff frequency it will always have some attenuation", "the maximum filter cutoff frequency is half the samplerate"; bypass also bypasses drive and resonance. In 2016, the Filter Control's cutoff and each filter's "are simply added". In 2019, "Sylenth1 already oversamples several parts internally by 2 or 4x", and the setting (1–16) multiplies that. In 2020, the v2 engine "will sound slightly different on different sample rates".
- **Unknown:** the cutoff range and law, where self-oscillation starts on the knob, the drive's gain, Warm Drive's saturator, whether the passband loss with resonance is made up, and whether the filter is oversampled at the default setting.

So the evidence describes a **transistor-ladder-style loop**: four stages, a saturator in each plus one at the loop's input (five), attenuation at the cutoff as a cascade of identical poles has, and saturators that aliased at high drive and resonance when the filter ran at the host rate.

## The candidates

Seven models, all in `tests/dsp/filters/filter_candidates.hpp` at commit `a0e9899`. The six ladders share one loop (four one-poles, feedback k from the last) and the same six responses from tap mixes (`tapTable`, filter-theory.md), so the comparison is about discretisation and saturation, not about the modes.

| Model | Discretisation | Saturators | Solver | Why it is here |
|---|---|---|---|---|
| **Linear ZDF** | TPT (trapezoidal, prewarped), zero-delay loop | none | closed form | The linear reference: what every ZDF model is at small signals. |
| **Newton ZDF** | TPT, zero-delay loop | tanh at the input and in each stage (5) | Newton–Raphson to 10⁻⁵, at most 8 iterations, from the linearised solve; also a "1 step" variant | The transistor-ladder model solved properly: the candidate that matches the manual and the five saturators. |
| **Linearised ZDF** | TPT, zero-delay loop | the same 5, each replaced by its secant gain at the state (Voipio) | closed form | The cheap nonlinear ZDF: no iteration. |
| **Feedback-only ZDF** | TPT, zero-delay loop | one, at the loop input | scalar Newton | The other cheap nonlinear ZDF: linear stages. |
| **Huovilainen** | explicit stages, unit-delay feedback with a half-sample correction, run in the `Oversampler` at 2× or 4× | the same 5 | none | The standard oversampled nonlinear ladder. Two tunings: naive, and exact (derived in filter-theory.md). |
| **Unit-delay ladder** | trapezoidal, prewarped stages; the loop closed through one sample of delay | one, at the loop input | none | The non-ZDF baseline: what the delay costs. |
| **Linear SVF** | TPT state-variable filter | none | closed form | The reference for the SVF question (12 dB only). |

The saturator is a separate axis, for Warm Drive: tanh (`dsp::fastTanh`), a cubic soft clip x − (4/27)x³ up to |x| = 1.5, the algebraic sigmoid x/√(1 + x²), and an asymmetric tanh (even harmonics). The Newton ladder runs with each. Every model takes the cutoff in Hz, the loop gain k, and a drive gain on the input with no make-up. Recursive state is flushed with `flushDenormal`, nothing allocates after `prepare()`, and the oversampled models use the library's `Oversampler` (`Oversampled<Model>`).

Two things were left out on purpose. A model of the actual Moog circuit with its own tuning and level quirks (D'Angelo and Välimäki, 2013) is a refinement of the Newton ladder, not a different structure. And an SVF with saturators doesn't have four stages, so it can't be the parity filter; the linear one answers the cost and response questions (see the options).

## What they measured

Measured at commit `a0e9899` by the hidden `[filter-report]` and the `[filter]` benchmarks, on the test-only prototypes. The report and the benchmarks now measure the built `LadderFilter` ([Results, as built](#results-as-built)). To rerun the candidates, build that commit:

```sh
git checkout a0e9899
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[filter-report]"   # about 30 s
tools/bench.sh "[filter]"
```

### The linear response

At −80 dBFS every saturator is linear, so this is each model's small-signal response, from its impulse response, against the analog prototype H(s) = Σ cᵢ(1 + s)⁴⁻ⁱ / ((1 + s)⁴ + k). Oversampled models are divided by the `Oversampler`'s own response (its delay and the halfbands' phase). LP24 at 44.1 kHz, k = 0; each cell is the gain error at the cutoff (dB), the phase error at the cutoff (degrees), the largest magnitude error from 20 Hz to the cutoff, and the largest from the cutoff to 20 kHz where the prototype is within 30 dB of its peak:

| Model | 1 kHz | 10 kHz | 19 kHz |
|---|---|---|---|
| Every ZDF model (linear, Newton, linearised, feedback-only), and the unit-delay ladder at k = 0 | 0.00 / 0.0° / 0.01 / 0.2 | 0.00 / 0.0° / 1.12 / 44.0 | 0.00 / 0.0° / 5.82 / 7.5 |
| Newton ZDF, 2× | 0.00 / 0.0° / 0.00 / 0.0 | 0.00 / 0.0° / 0.26 / 4.3 | 0.00 / 0.0° / 1.00 / 0.4 |
| Huovilainen, naive, 2× | 0.01 / 8.1° / 0.01 / 0.0 | 0.72 / 71.9° / 0.72 / 2.9 | 2.45 / 120.2° / 2.45 / 2.7 |
| Huovilainen, exact, 2× | −0.29 / 6.1° / 0.29 / 0.5 | −1.14 / 61.2° / 1.14 / 1.1 | 1.64 / 116.3° / 1.64 / 1.9 |
| Huovilainen, exact, 4× | −0.15 / 3.1° / 0.15 / 0.2 | −1.06 / 30.6° / 1.06 / 1.3 | −1.17 / 58.2° / 1.17 / 1.2 |
| Linear SVF (LP12, its own prototype) | 0.00 / 0.0° / 0.01 / 0.9 | 0.00 / 0.0° / 0.56 / 22.0 | 0.00 / 0.0° / 2.91 / 3.8 |

- **The prewarped trapezoidal models are exact at the cutoff** (gain and phase, at every cutoff and k) and below it to a fraction of a dB up to 5 kHz. Above the cutoff they cramp towards Nyquist (filter-theory.md): up to 44 dB below the prototype by 20 kHz with a 10 kHz cutoff at 44.1 kHz. At 2× the cramping moves up an octave (4.3 dB).
- **Huovilainen's explicit stages** are not exact anywhere: about 1 dB off at the cutoff above 5 kHz even with the exact tuning, and 30–120° of extra phase (its stages and its half-sample delay). The phase matters for how resonance and drive interact, not for the magnitude.
- **At k = 3 the unit-delay ladder is unstable above about 3.5 kHz** (its threshold falls below 3, next section), so its row there is meaningless; at k = 0 there is no loop and it equals the others.

**The tap mixes are exact.** For every response, at k = 0 and k = 3, the linear ZDF's error against the tap-mix prototype is the cramping alone: exact at the cutoff, departing only towards Nyquist. Over 20 Hz–20 kHz where the prototype is within 30 dB of its peak, with a 1 kHz cutoff: 0.03–0.06 dB for the highpasses, 0.2–0.9 dB for the lowpasses, and 4.2 dB (BP24) and 13.5 dB (BP12), whose wide skirts reach 20 kHz. Huovilainen at 2× has 0.2–1.2 dB of its own at 1 kHz.

**Where a resonant peak lands** (LP24, k = 3.5, 44.1 kHz), the digital peak against the prototype's, in cents: every ZDF model 0.2 at 1 kHz, 4.7 at 5 kHz, 17.7 at 10 kHz, 53 at 20 kHz (the cramping moves a peak below the threshold; at the threshold itself the mapping is exact, next section); Newton at 2× 1.2 at 5 kHz, 17.7 at 20 kHz; Huovilainen exact at 2× within 5.2 cents everywhere; naive at 2× up to 75.5 cents; the unit-delay ladder −58 cents at 1 kHz and −391 at 5 kHz.

### Self-oscillation

Started by one impulse of 0.5 and left alone with no input; the pitch from the last 0.5 s of a 2.5 s render (`measureFrequency`). At k = 4.4 (1.1× the threshold), cents from the cutoff, 44.1 kHz:

| Model | 55 Hz | 440 Hz | 1760 Hz | 3520 Hz | 7040 Hz | 14080 Hz |
|---|---:|---:|---:|---:|---:|---:|
| Newton ZDF | −35.78 | −35.76 | −35.42 | −34.40 | −30.98 | −16.73 |
| Newton ZDF (cubic) | −32.35 | −32.33 | −32.02 | −31.09 | −27.97 | −15.04 |
| Linearised ZDF | −34.99 | −29.80 | −16.21 | −5.51 | −3.01 | −24.00 |
| Feedback-only ZDF | −0.13 | −0.10 | −0.08 | −0.04 | 0.00 | −0.07 |
| Unit-delay ladder | −6.90 | −52.85 | −193.35 | −346.02 | −563.45 | −727.45 |
| Huovilainen, naive, 2× | −33.50 | −17.59 | +35.81 | no oscillation | no oscillation | no oscillation |
| Huovilainen, exact, 2× | −35.83 | −36.15 | −37.29 | −38.84 | −42.03 | −47.81 |
| Huovilainen, exact, 4× | −35.80 | −35.97 | −36.53 | −37.29 | −38.84 | −42.03 |
| Newton ZDF, 2× | −35.78 | −35.77 | −35.69 | −35.42 | −34.40 | −30.98 |

At 192 kHz the Newton ladder is −35.78 to −35.48 cents from 55 Hz to 7 kHz; the full tables for 44.1, 48, 96, and 192 kHz are in the report.

- **A ladder that saturates in every stage goes flat when it self-oscillates**, by an amount set by k, not by the cutoff (filter-theory.md: the saturators lower the stages' effective cutoff as the oscillation grows). The Newton ladder holds −35.8 cents within 1.4 cents from 55 Hz to 3.5 kHz at 44.1 kHz, and within 0.3 cents to 7 kHz at 192 kHz. So with keytrack 10, **notes keep their intervals** to about a cent over the musical range; only the whole scale is flat by an amount that depends on Resonance. Near Nyquist the cramping changes the oscillation's amplitude and so its detune (−16.7 cents at 14 kHz, 44.1 kHz); at 2× that moves an octave up.
- **Saturating only the loop's input keeps the pitch**: the feedback-only ladder is within 0.2 cents everywhere (its stages stay linear).
- **The linearised solve's detune depends on the cutoff** (−35 to −3 cents): its secant gains come from the state, which lags the signal more at high cutoffs. That breaks the intervals that keytrack needs.
- **The unit delay detunes by hundreds of cents** at high cutoffs, and Huovilainen's naive tuning stops oscillating above 1.8 kHz at k = 4.4 (its threshold rises). The exact tuning fixes the threshold but its explicit stages drift by up to 12 cents (2×) over the range.

**The threshold** (the smallest k at which a 10⁻¹⁵ impulse grows, by bisection): **exactly 4.000** at 110 Hz to 7 kHz, at 44.1 and 96 kHz, for every ZDF model and Huovilainen with the exact tuning (4.005–4.010 at 14 kHz at 44.1 kHz, the measurement's resolution at three samples per cycle). Huovilainen naive at 2×: 4.016 at 110 Hz, 4.79 at 7 kHz. The unit-delay ladder: 3.94 at 110 Hz, 2.18 at 7 kHz, 2.0 at 14 kHz (44.1 kHz).

**Level and pitch against k** (48 kHz, 440 Hz; peak-equivalent dBFS from the RMS / cents):

| Model | k 4.05 | 4.2 | 4.4 | 5 | 6 | 8 |
|---|---|---|---|---|---|---|
| Newton ZDF | −24.9 / −5.3 | −19.2 / −19.6 | −16.5 / −35.8 | −13.5 / −70.1 | −11.9 / −101.2 | −10.9 / −125.7 |
| Newton ZDF (cubic) | −21.4 / −5.2 | −15.9 / −18.6 | −13.6 / −32.3 | −11.4 / −54.6 | −10.7 / −64.3 | −10.3 / −70.1 |
| Linearised ZDF | −25.1 / −4.5 | −19.4 / −16.7 | −16.8 / −30.3 | −13.8 / −58.4 | −12.2 / −81.5 | −11.2 / −94.1 |
| Feedback-only ZDF | −25.1 / −0.0 | −19.4 / −0.0 | −16.7 / −0.1 | −13.6 / −0.5 | −11.9 / −1.5 | −10.7 / −3.8 |
| Huovilainen, exact, 2× | −25.0 / −5.3 | −19.3 / −19.8 | −16.6 / −36.1 | −13.7 / −70.8 | −12.0 / −102.0 | −11.0 / −126.6 |

At 7 kHz the Newton ladder's figures are within 8 cents of these. The oscillation is −11 to −25 dBFS at the LP24 output; how loud Sylenth1's is, and how flat, is F7. With no input, every model stays exactly silent even at k = 8: something has to start the oscillation.

### Passband loss against resonance

LP24 and LP12 at a 2 kHz cutoff, the component at a 100 Hz probe (which reads the passband even while the filter self-oscillates at 2 kHz), 48 kHz, dB:

| Model | k 0 | 1 | 2 | 3 | 4 | 5 | 6 | 8 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Linear ZDF, LP24 (theory 1/(1 + k)) | 0.0 | −6.0 | −9.5 | −12.0 | −14.0 | unstable | unstable | unstable |
| Newton ZDF, LP24, −40 dBFS | 0.0 | −6.0 | −9.5 | −12.0 | −14.0 | −16.3 | −18.1 | −21.0 |
| Newton ZDF, LP12, −40 dBFS | 0.0 | −6.0 | −9.5 | −12.0 | −13.9 | −15.7 | −17.4 | −20.1 |
| Huovilainen exact 2×, LP24 | 0.0 | −6.3 | −9.9 | −12.4 | −14.4 | −16.7 | −18.5 | −21.4 |
| Unit-delay ladder, LP24 | 0.0 | −6.0 | −9.5 | −12.0 | −14.8 | −17.2 | −19.1 | −21.9 |

Every model follows 1/(1 + k) to 0.1 dB up to the threshold; above it the self-oscillation compresses the saturators and the passband loses up to 1.9 dB more than 1/(1 + k) (Newton LP24: 0.7 dB at k 5, 1.9 dB at k 8). At −6 dBFS the figures are the same within 0.1 dB except at k = 0, where the feedback-only and unit-delay ladders lose 0.5–0.6 dB to their input saturator. Huovilainen's exact tuning scales k up slightly at 2 kHz, so it loses 0.3–0.4 dB more. Whether Sylenth1 makes this up is F6.

### Harmonics against drive (Warm Drive)

LP24 at 10 kHz, k = 0, a 220 Hz sine at −6 dBFS, 48 kHz, drive a gain on the input with no make-up; THD over harmonics 2–10 and the harmonics in dB re the fundamental:

| Model | Drive | Out (dBFS) | THD | H2 | H3 | H5 | H7 | H9 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Newton ZDF (tanh) | 1 | −6.0 | 0.50% | — | −46.1 | −78.4 | −108.2 | −136.6 |
| | 2 | 0.0 | 2.37% | — | −32.5 | −52.7 | −69.5 | −85.6 |
| | 4 | 5.7 | 14.4% | — | −17.5 | −26.5 | −32.0 | −36.7 |
| | 8 | 8.2 | 29.7% | — | −12.1 | −18.0 | −21.8 | −24.6 |
| | 16 | 8.7 | 34.3% | — | −11.1 | −16.3 | −19.8 | −22.4 |
| Newton ZDF (cubic) | 1 | −6.0 | 0.23% | — | −52.9 | −83.5 | −113.7 | −143.2 |
| | 2 | 0.0 | 1.26% | — | −38.1 | −54.8 | −71.0 | −86.4 |
| | 4 | 4.3 | 15.3% | — | −16.8 | −28.3 | −30.8 | −35.1 |
| | 8 | 5.0 | 29.6% | — | −11.5 | −18.8 | −25.2 | −30.3 |
| | 16 | 5.2 | 36.3% | — | −10.4 | −15.9 | −19.9 | −23.3 |
| Newton ZDF (algebraic) | 2 | 0.0 | 3.24% | — | −29.8 | −49.9 | −65.5 | −80.4 |
| | 8 | 9.2 | 30.6% | — | −12.0 | −17.4 | −21.0 | −23.7 |
| Newton ZDF (asymmetric tanh) | 1 | −6.0 | 1.13% | −40.0 | −45.6 | −77.4 | −105.5 | −131.5 |
| | 4 | 5.6 | 15.8% | −22.7 | −18.2 | −28.6 | −35.9 | −41.3 |
| Linearised ZDF (tanh) | 4 | 5.6 | 12.1% | — | −18.9 | −29.0 | −35.3 | −40.8 |
| Feedback-only ZDF (tanh) | 1 | −6.5 | 1.95% | — | −34.2 | −66.8 | −99.3 | −131.2 |
| | 4 | 1.0 | 17.2% | — | −15.5 | −28.4 | −41.2 | −53.9 |
| Huovilainen exact 2× | 4 | 5.3 | 19.5% | — | −15.2 | −22.8 | −27.8 | −31.8 |

(— : below −150 dB, the symmetric saturators' even harmonics. The report has every drive for every row.)

- **The per-stage saturators limit slew, not level**: the output keeps rising with drive (to +8.7 dBFS at drive 16 for tanh), because a 220 Hz sine is slow for a 10 kHz stage. The input-only saturator flattens it at +2 dBFS.
- **tanh against cubic** is the closest pair to "a higher quality of saturation which introduces more higher harmonics", but the difference in harmonics is modest. tanh's upper harmonics are 5–7 dB stronger at drive 1 (H5 −78.4 against −83.5, H9 −136.6 against −143.2) and 3–6 dB stronger from H7 up at drive 8 (H9 −24.6 against −30.3). At drive 2, 4, and 16 the two are within 2 dB of each other, either way round. The cubic's output peaks 3.5 dB lower at high drive, and its third harmonic is weaker below its knee. The algebraic sigmoid has more harmonics than tanh at low drive and about the same at high drive. The asymmetric tanh adds a second harmonic at −40 to −23 dB, the "tube" alternative for F9.

### Aliasing against drive and oversampling

A 2.5 kHz sine at −6 dBFS, 44.1 kHz, LP24 at 15 kHz, k = 0; the strongest single alias below 20 kHz, dB re the fundamental:

| Model | Drive 1 | Drive 2 | Drive 4 | Drive 16 |
|---|---:|---:|---:|---:|
| Newton ZDF (tanh), 1× | −135.7 | −79.6 | −47.4 | −33.2 |
| Newton ZDF (tanh), 2× | −154.5 | −147.5 | −102.0 | −65.7 |
| Newton ZDF (tanh), 4× | −168.6 | −164.2 | −149.8 | −120.5 |
| Newton ZDF (cubic), 1× | −170.7 | −104.6 | −59.4 | −31.8 |
| Newton ZDF (cubic), 2× | −175.2 | −172.5 | −87.0 | −53.6 |
| Newton ZDF (cubic), 4× | −172.2 | −171.6 | −101.3 | −75.4 |
| Linearised ZDF, 1× / 2× / 4× | −127.2 / −158.4 / −165.3 | −77.9 / −149.0 / −162.2 | −49.7 / −92.9 / −152.7 | −32.0 / −53.3 / −107.2 |
| Feedback-only ZDF, 1× / 2× / 4× | −143.8 / −165.0 / −169.3 | −127.2 / −151.0 / −168.7 | −83.6 / −157.1 / −157.0 | −34.7 / −65.7 / −121.3 |
| Huovilainen exact, 2× / 4× | −156.8 / −169.1 | −149.3 / −166.6 | −99.1 / −153.3 | −65.1 / −120.6 |

With resonance (LP24 at 5 kHz, k = 3) the stages filter the harmonics before they return, and 1× is clean up to drive 4 (Newton −95.2 dB) but not at 16 (−32.0; 2×: −57.7; 4×: −112.4).

- **At the host rate, drive aliases audibly from about +6 dB**: −80 dB at drive 2, −47 dB at drive 4, −33 dB at drive 16, the developer's 2007 "bit of high frequency aliasing when you use high resonance or high drive levels".
- **2× keeps the strongest alias under −100 dB up to drive 4** (+12 dB) for tanh; **drive 16 needs 4×** (−120 dB).
- **The cubic aliases less at 1×** (its harmonics fall off faster below its knee), **more at 2× and 4× under heavy drive** (its kink at |x| = 1.5 makes harmonics that never fall away: −87 against −102 dB at 2×, drive 4).
- At k = 0 the feedback-only and unit-delay ladders are the same filter (no loop); their stages filter what the single saturator makes, so they alias less at moderate drive.

### Distance from the converged solution

How far each cheaper solver lands from the Newton ladder solved to 10⁻⁵, at the same oversampling factor (so aliasing and the `Oversampler`'s delay are common): a 110 Hz saw at −6 dBFS, the cutoff swept 100 Hz → 8 kHz → 100 Hz over 2 s, 44.1 kHz; the error's power below 20 kHz against the signal's, dB. Hard: k 3.8, drive 4. Gentle: k 2, drive 2.

| Model | 1× hard / gentle | 2× hard / gentle | 4× hard / gentle |
|---|---|---|---|
| Newton ZDF, 1 step | −45.8 / −73.2 | −69.9 / −89.3 | −94.1 / −112.4 |
| Linearised ZDF | −17.0 / −31.2 | −22.6 / −37.3 | −27.5 / −43.5 |
| Huovilainen, exact | — | −23.0 / −24.3 | −27.8 / −29.4 |
| Huovilainen, naive | — | −19.2 / −26.1 | −22.9 / −31.6 |
| Feedback-only ZDF (a different model) | −6.8 / −16.8 | −6.8 / −16.8 | −6.8 / −16.8 |
| Unit-delay ladder | −3.3 / −16.9 | −5.3 / −17.1 | −6.1 / −17.0 |

- **One Newton step from the linearised guess** is within −46 dB on hard material at 1× and −70 dB at 2×; the linearised solve alone is only 17–28 dB away. Huovilainen solves the same equations differently, and it stays 23–29 dB from the trapezoidal solution even when gentle (its phase differs).
- **Newton's iterations** (tolerance 10⁻⁵, from the linearised guess): a mean of 1.18 (a saw, k 0, drive 1), 1.99 (k 3.8, drive 4), 1.96 (the hard sweep), 2.21 (the sweep at k 8, drive 32); at most 5; never unconverged.

### Stability, subnormals, block sizes, allocation

48 kHz, 2 s each, no FTZ. A: k 8, drive 32, a full-scale saw, the cutoff swept 20 Hz–20 kHz at 500 Hz per sample. B: the same sweep at k 3.9, drive 4. C: DC steps 0 → 1 → 10 → −10 at k 4.5. D: an impulse, then 20 s of silence at k 3.

| Model | A peak | B peak | C peak | D: subnormal outputs | D: exactly 0 after |
|---|---:|---:|---:|---:|---:|
| Linear ZDF | NaN/inf | 2.44 | NaN/inf | 617 | 1.79 s |
| Newton ZDF | 2.45 | 1.04 | 1.96 | 607 | 1.78 s |
| Newton ZDF (cubic) | 1.48 | 1.05 | 1.50 | 608 | 1.79 s |
| Linearised ZDF | 2.37 | 1.20 | 1.94 | 617 | 1.79 s |
| Feedback-only ZDF | 1.26 | 0.96 | 1.00 | 606 | 1.78 s |
| Unit-delay ladder | 1.26 | 1.24 | 1.00 | 606 | 1.86 s |
| Huovilainen (naive, exact), 2× and 4× | 2.04–2.15 | 0.82–0.92 | 1.91–1.96 | 875,342–876,439 | never (> 20 s) |
| Newton ZDF, 2× | 2.31 | 0.94 | 1.96 | 875,608 | never (> 20 s) |

- **Every saturating model is bounded** at k 8 and drive 32 under an audio-rate sweep, and with DC of 10. The linear ladder blows up above k = 4, as it must.
- **Subnormals.** The models flush their states, but for about 600 samples (13 ms) the output, a product of tiny states, is subnormal before the states reach 0. The audio thread's FTZ guard covers that. **The oversampled models never reach 0 and put out subnormals for the whole 20 s**: the library's `Oversampler` doesn't flush its halfbands' allpass states, so without FTZ they decay into subnormals and stay there. Production code runs under `ScopedFlushDenormals`, but the second layer the denormal strategy promises (`flushDenormal` on recursive state, [denormals.md](denormals.md)) is missing in `halfband.hpp`. To fix test-first with the build.
- **Block sizes:** every model, oversampled or not, is bit-identical in blocks of 1 and 37 against 4096 under the per-sample sweep (B), and none allocated while processing (`AllocationCounter`).

### Clicks and zipper noise

**A stepped cutoff** (48 kHz, LP24, k 3.6, a 150 Hz sine at −12 dBFS, the cutoff 500 Hz → 4 kHz in one sample): the energy above 1 kHz in a Hann window of 4096 centred on the step, dB re an instant unit step. Every ZDF model −27.3 to −27.4 dB stepped, **−46.4 to −46.7 dB with the cutoff's log₂ ramped over 10 ms** (`LinearSmoother`), −113 dB steady; Huovilainen the same within 2 dB. The step excites the resonance: a 10 ms ramp takes 19 dB off, which is what v1.11's "Eliminated clicking effects caused by the filters" and v3.008's "Clicks and pops during automation" suggest Sylenth1 does.

**A staircase cutoff** (the hard sweep, 44.1 kHz): the error against the per-sample sweep, dB re the signal.

| Model | Held 32 samples | Held 64 | log₂ interpolated every 32 | g interpolated every 32 |
|---|---:|---:|---:|---:|
| Linear ZDF | −23.6 | −17.5 | −76.9 | −76.7 |
| Newton ZDF | −38.2 | −32.0 | −80.9 | −80.9 |
| Linearised ZDF | −40.0 | −33.9 | −85.8 | −85.8 |
| Huovilainen exact, 2× | −38.0 | −31.8 | −82.3 | −82.3 |

Holding the cutoff for a block zippers audibly (−17 to −40 dB). **Interpolating the integrator gain g = tan(π·fc/fs) linearly between points every 32 samples is as good as computing it per sample** (−77 to −86 dB), and it removes the `tan` (6 ns per call) from every sample but one in 32.

### CPU

Nanoseconds per sample for one mono filter channel, 48 kHz, LP24 at 1 kHz on a saw, `relwithdebinfo`, Apple M1 Ultra (`tools/bench.sh "[filter]"`). Serial: one channel at a time, so each sample waits for the last (latency-bound). Interleaved: four independent channels per loop, as a voice loop would run them, so the core overlaps their dependency chains. 16 voices × 2 filters × stereo is **64 filter channels**; the last column is their share of one core at 48 kHz (20.8 µs per sample).

| Model | Serial, k 2, drive 2 | Serial, k 6, drive 16 | Interleaved ×4 | 64 channels, serial (interleaved) |
|---|---:|---:|---:|---:|
| Linear ZDF | 22.6 | 23.1 | 7.4 | 6.9% (2.3%) |
| Linear SVF | 14.4 | 14.4 | | 4.4% |
| Unit-delay ladder | 26.0 | 25.6 | | 8.0% |
| Linearised ZDF (tanh) | 47.9 | 49.1 | 26.1 | 14.7% (8.0%) |
| Linearised ZDF (cubic) | 36.8 | 38.1 | | 11.3% |
| Feedback-only ZDF | 66.7 | 79.6 | | 20.5% |
| Newton ZDF, 1 step (tanh) | 94.8 | 95.4 | 65.7 | 29.1% (20.2%) |
| **Newton ZDF (tanh)** | **113.0** | **139.3** | **86.9** | **34.7% (26.7%)** |
| **Newton ZDF (cubic)** | **78.4** | **102.9** | **47.1** | **24.1% (14.5%)** |
| Newton ZDF (algebraic) | 89.7 | 111.8 | | 27.5% |
| Newton ZDF (tanh), 2× | 228.6 | 284.4 | | 70.2% |
| Linearised ZDF (tanh), 2× / 4× | 113.1 / 220.2 | 111.3 / 222.2 | | 34.7% / 67.7% |
| Huovilainen exact, 2× / 4× | 193.6 / 387.0 | 194.3 / 383.3 | | 59.5% / 119% |

A cutoff set every sample (a `tan` per sample) costs nothing measurable in these serial runs (linear ZDF 22.0 against 22.6 ns; Newton 107.8 against 113.0): the `tan` is off the loop's dependency chain, so the core hides it behind the latency. `prewarpedGain()` alone is 6.0 ns, and Huovilainen's exact tuning 17.4 ns per call. Once channels are interleaved or vectorised, throughput is the limit and the 6 ns counts (it is most of the interleaved linear ZDF's 7.4).

- **These are unoptimised scalar prototypes**, each sample a long dependency chain with several divisions (the ladder's solve, the Newton diagonals, `fastTanh`'s quotient). Interleaving four channels already gives 1.3–3×; SIMD across channels (four or eight lanes) is the Phase 3 step, with its scalar fallback.
- **Newton with tanh is the costliest ZDF**: 113–139 ns serial, a third of a core for a full 16-voice, two-filter, stereo patch, before SIMD. The cubic saturator cuts that by 26–46% (31% serial at k 2, 26% at k 6 and drive 16, 46% interleaved: 47 against 87 ns), more than its harmonics would suggest, because tanh's quotient sits in every iteration.
- **Oversampling multiplies the filter's cost by the factor and adds the round trip** (17 ns at 2×, 27 ns at 4× per channel, [baselines](../performance/baselines.md)). Huovilainen, though cheap per inner sample, is not cheap per output sample: 194 ns at 2× against Newton's 113 at 1×.

## The decision

*Decided by the project owner on 2026-11-14: every recommendation below, as written (PLAN.md, Decisions log). The build follows it, with the details in [How it is applied here](#how-it-is-applied-here).*

| Question | Decision | Why |
|---|---|---|
| **Core model** | **The Newton ZDF ladder**: four trapezoidal one-poles with the cutoff prewarped, a zero-delay loop, a saturator at the loop input and in each stage (five), solved by Newton–Raphson from the linearised guess to 10⁻⁵, at most 8 iterations (a cap for the worst case; 5 was the most measured). | It is the structure the manual and the feature list describe (four stages, each saturating; five saturators). Prewarped and delay-free, it is exact where the parity questions are asked: the cutoff (0.00 dB, 0.0°), the threshold (4.000 at every cutoff and rate), the passband (1/(1 + k) to 0.1 dB), and self-oscillation that keeps its intervals across the keyboard to about a cent. It is bounded under every stress, bit-identical across block sizes, and allocation-free. Its cost (113 ns serial, 87 interleaved) is the price, and SIMD is the plan for it. The linearised solve (half the cost) is out because its detune changes with the cutoff, which breaks keytrack 10; Huovilainen because it needs 2× just to be in tune, is not exact even then, and costs more than Newton at 1×; the unit delay because it detunes by hundreds of cents. |
| **Solver for Warm off** (or a Phase 3 economy) | Keep the same solver; vary the saturator (next row). "One Newton step" is the fallback if CPU needs it. | One step is −46 dB from converged on hard material (−70 at 2×) and saves 16–24%, but it changes nothing audible, so it can't be what Warm Drive switches. |
| **Warm Drive pair** | **On: tanh. Off: the cubic soft clip x − (4/27)x³ to \|x\| = 1.5,** in the same Newton ladder. | The manual's two sides: "a higher quality of saturation which introduces more higher harmonics" against "less CPU power". tanh's upper harmonics are 5–7 dB stronger at drive 1 and 3–6 dB at drive 8 (within 2 dB elsewhere), and the cubic is 26–46% cheaper. Same structure, so the cutoff, the threshold, the modes, and keytracking don't move when the switch is toggled. Its costs: the harmonic difference is modest; the cubic self-oscillates less flat at high k (−70 against −126 cents at k 8) and peaks lower; and with oversampling it aliases more under heavy drive (−87 against −102 dB at 2×, drive 4). The asymmetric tanh (even harmonics, Futuremusic's "tube nuance") is the alternative if F9 finds even harmonics with Warm on. |
| **Oversampling** | **The filter is factor-agnostic** (prepared at R·fs). **Default 1×**, as Sylenth1's default setting appears to be, with 2× and 4× as the v3 Realtime and Render options over the whole voice (M1.8), not around the filter alone. Recommended render setting: 4×. | 1× is clean (under −100 dB) at drive ≤ 1 and with resonance up to drive 4, and aliases from drive 2 (−80 dB) to drive 16 (−33 dB), which is what the developer described in 2007. 2× fixes drive up to +12 dB; drive 16 needs 4×. At 2× the filter alone costs 70% of a core for 64 channels before SIMD, so 2× as the default would be a CPU decision, not a parity one. Oversampling the whole voice shares one round trip between the oscillators and the filter, and the oscillator already has its own band-limiting. F10 settles whether Sylenth1's filter is oversampled at 1. |
| **12 dB and the modes** | **Tap mixes of the one four-pole loop** (`tapTable`): LP12 = y2, BP12 = 2(y1 − y2), BP24 = 4(y2 − 2y3 + y4), HP12 = u − 2y1 + y2, HP24 = u − 4y1 + 6y2 − 4y3 + y4, the bandpasses at unit peak. **Bypass** skips the filter entirely: output = input, no drive, no cost; leaving it starts from reset state (F11). | Four stages with saturation in every mode (the manual says it of the filter, not of LP24). Every mode resonates at the cutoff with one threshold, the highpass keeps its passband, and a new response is a row of five weights, which is the extensibility rule. Measured exact against the tap-mix prototypes. An SVF for the 12 dB modes would have no stages to saturate, a different resonance, and its own threshold; it is not worth a slot in the parity build, and a multimode SVF is already a Phase 2 item. |
| **Resonance compensation** | **None in the core**: the ladder's 1/(1 + k). A compensation, if F6 finds one, is an **output gain law g(k)** (for example (1 + k)^c), one function in the DSP, defaulting to 1. | No source says Sylenth1 makes it up, and an analog ladder doesn't. Compensating at the output keeps drive's behaviour unchanged; a gain at the input would also change how hard the stages are driven. |
| **Self-oscillation detune** | **Leave it**: no correction of the pitch against k in the core. If F7 shows Sylenth1 in tune at high resonance, add a cutoff scale c(k) (a table in cents), which corrects every note equally because the detune is flat across the cutoff. | It is how a saturating ladder behaves, and keytrack 10 keeps intervals regardless. A correction is easy to add later because it depends on k alone. |
| **Cutoff modulation and smoothing** | The cutoff (after keytrack and modulation, in log₂ units) is turned into g at the 32-sample grid and **g is interpolated linearly between grid points**; **steps from controls (MIDI, automation) are ramped over 10 ms in log₂**; modulation from audio-rate sources may set g per sample. | −77 to −86 dB from per-sample g; the held staircase zippers at −17 to −40 dB; a 10 ms ramp takes 19 dB off a resonant step. One `tan` per 32 samples (6 ns) instead of per sample. |
| **Knob ranges (hypotheses for M1.9)** | **Cutoff:** exponential, 10 Hz × 2^(11·x), x = knob / 10 (10 Hz to 20.5 kHz), clamped below 0.495·fs (F3). **Resonance:** k = 6·x, self-oscillating from 6.7 (F5). **Drive:** a gain of 0 to +24 dB on the input, dB-linear in the knob, no make-up (F8). **Keytrack** (later round): 0–10 → 0–100%, linear (F13). | Placeholders shaped by the evidence (cutoff up to half the sample rate; resonance "way beyond" the threshold; drive that "adds higher harmonics"), each one function in the parameter table, so a measurement changes a law, not the DSP. At k = 6 the Newton ladder self-oscillates at −11.9 dBFS and −101 cents; at +24 dB drive its THD is 34%. |
| **Where it lives** | `libs/dsp`: `filters/` with the ladder (a `LadderFilter` taking the cutoff in Hz, k, a drive gain, a `TapMix` row, and a saturator type), `tapTable` as data, the saturators as small types, and the g grid. A `flushDenormal` fix in `resampling/halfband.hpp`. The plugin: the knob laws and Filter Control (M1.9 and later rounds). | The design constraints: plain units in `libs/dsp`, no JUCE; filter types as registered data (Phase 2 adds rows and models); the oscillator, filter, and envelope all run per voice at the voice's rate. |

## How it is applied here

### The filter

`LadderFilter` ([ladder_filter.hpp](../../libs/dsp/include/anoesis/dsp/filters/ladder_filter.hpp)) holds its controls, four `LinearSmoother`s (log₂ of the cutoff, k, drive, and a 0 → 1 fade for response changes), the grid's two gains, the current and previous tap mixes, and per channel (up to two: a stereo voice) four integrator states and the last loop input. It is 344 bytes, and a `BlockProcessor`.

- **The loop** is the decided Newton ZDF ladder. Each sample, `solve<Sat>()` first linearises the five saturators at the state (each t(v) as v · t(v₀)/v₀, v₀ the stage's integrator state or the previous loop input) and solves that linear loop in closed form, then runs Newton–Raphson on the residuals Fᵢ = yᵢ − sᵢ − g·(t(yᵢ₋₁) − t(yᵢ)), y₀ = x − k·y4, until no stage moves by 10⁻⁵, at most 8 times. The Jacobian solve is the bidiagonal-plus-corner sweep of filter-theory.md: each Δᵢ as pᵢ + qᵢ·Δ4, then Δ4 = p4/(1 − q4), where 1 − q4 ≥ 1 because every slope is non-negative. The integrators then take sᵢ ← 2yᵢ − sᵢ.
- **Warm Drive** picks the saturator once per run of samples, as a template argument: `TanhSaturator` (`fastTanh`, slope 1 − t², secant t/x) or `CubicSaturator` (x − (4/27)x³ to |x| = 1.5, then ±1; slope 1 − (4/9)x²; secant 1 − (4/27)x² inside the knee and 1/|x| outside, no division in the curve itself). Toggling it changes nothing else.
- **Responses are data.** `filterTapMixes` holds the five weights of u, y1 … y4 for each `FilterResponse`; the output is their mix, with u taken after its saturator (the signal the first stage sees). `setResponse()` crossfades: the old mix (or, mid-fade, the blend so far) and the new one are interpolated over 10 ms, sample by sample, from the same state. This detail was not in the prototypes: switching the weights at once would step the output (a lowpass's y4 to a highpass's u), and the fade costs only the five-weight blend while it lasts.
- **The cutoff** is a `LinearSmoother` on log₂(Hz), so a `setCutoff()` from a knob, a MIDI controller, or automation ramps over 10 ms in octaves. `setCutoffModulation()` adds octaves without a ramp. Both are read at grid points every 32 samples, counted from `reset()`: there the smoother is advanced 32 samples, the cutoff (control plus modulation) is clamped to [1 Hz, 0.495 fs], and g = tan(π·fc/fs) is computed once; across the next 32 samples g is interpolated linearly from the last grid point's value. A change therefore applies from the next grid point, at most 31 samples late, which is what makes the output independent of the block size. `currentCutoff()` reports the cutoff at the end of the current segment (for tests, and for a display).
- **Resonance and drive** ramp linearly over 10 ms, sample by sample. k is clamped to [0, 64] and drive to [0, 1000]; NaN gives 0. The cutoff maps NaN and +∞ to 1 MHz (then the clamp below Nyquist) and −∞ to 1 Hz; a NaN modulation counts as 0.
- **Bypass** returns before touching anything: the audio is bit for bit the input, and no smoother or grid advances. Leaving bypass calls `reset()`, so the filter starts from rest with every ramp finished, as a new note would.
- **Channels** share the controls, the grid, and the ramps (computed once per run) and keep their own loop states. A channel beyond the prepared count is left untouched.
- **Denormals.** The integrator states, the last loop input, and the output are flushed below FLT_MIN with `flushDenormal`, so silence reaches exact zero without FTZ (the slowest pole sets when: 1.8 s at 100 Hz, k 3) and no output is subnormal. The candidate measurements showed about 600 subnormal outputs before the states reached zero; flushing the output removes them for one compare per sample. `isAtRest()` is true once every state is exactly zero: the filter holds no energy, so silence in gives silence out (a voice may use it; the tests use it, because a flushed output alone can't show whether the states behind it are subnormal).
- **Real time.** Nothing allocates; there are no tables. `prepare()` and `reset()` finish every ramp and clear the states, so a voice that starts a note calls `reset()` after setting its controls.

### The Oversampler's halfbands

The candidate measurements found the library's `Oversampler` never reaching silence without FTZ (875,000 subnormal outputs in 20 s). Two changes fix it. Every output of a halfband (each upsampler path's sample, the downsampler's) is flushed below FLT_MIN per sample, outside the recursion, so no sample is ever subnormal. And every allpass state is flushed once per block (`flushStates()`, called by `upsample()` and `downsample()`), so the states reach exact zero within a few thousand samples of the input stopping (`Oversampler::isAtRest()`). A first version flushed each section's output inside the chain, per sample: correct, but the compare sat in the chain's serial dependency and made the round trip 50–57% slower (2×: 15.8 → 24.0 ns per sample; 4×: 27.9 → 43.9). Per block, a state that crosses FLT_MIN can run subnormal at most until the block ends, once per decay; the round trip measures 15.7 and 26.3 ns, within noise of the 16.7 and 26.8 recorded before (baselines).

### How a voice uses it

```cpp
// Per part (M1.4): the filter after the oscillators, before the amp envelope.
filter.setResponse(responseFromTypeAndSlope);   // the parameter table maps Type x Slope
filter.setBypassed(type == FilterType::bypass); // bypass costs nothing
filter.setWarm(warmDrive);                      // Filter Control's switch
filter.setCutoff(cutoffHz);                     // knob law and Filter Control: M1.9 and a later round
filter.setResonance(k);                         // knob law: k = 6 x knob / 10 (F5)
filter.setDrive(driveGain);                     // knob law: 0 to +24 dB (F8)
// Modulation (M1.5): envelopes, LFOs, keytrack in octaves, read on the grid.
filter.setCutoffModulation(modulationOctaves);
// At a note-on from silence: filter.reset();
filter.process(partBlock);                      // stereo, in place
```

The voice decides the rate the filter runs at (M1.8): it is prepared at whatever rate it is given, so the v3 Realtime and Render oversampling wrap the whole voice.

## Results, as built

From the `[filter-report]` ([filter_report.cpp](../../tests/dsp/filters/filter_report.cpp)), which now measures `LadderFilter` with Warm on and off, at 1× and inside the `Oversampler` at 2× and 4×, and the `[filter]` benchmarks ([filter_benchmark.cpp](../../tests/dsp/benchmarks/filter_benchmark.cpp)):

```sh
cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[filter-report]"   # about 10 s
tools/bench.sh "[filter]"
```

Every figure matches the chosen prototype's (the Newton ZDF rows above, tanh and cubic) to the printed digit: the built filter computes the same equations, and a settled cutoff gives the same g.

| Measurement | Warm on (tanh) | Warm off (cubic) |
|---|---|---|
| At the cutoff, 100 Hz–19 kHz, 44.1 kHz | 0.00 dB, 0.0° | the same |
| Cramping above a 10 kHz cutoff, LP24: 1× / 2× / 4× | 44.0 / 4.3 / 0.9 dB | the same |
| Threshold, 110 Hz–14 kHz at 44.1 and 96 kHz | 3.998–4.003 | the same |
| Self-oscillation at k 4.4, 55 Hz–3.5 kHz, 44.1 kHz | −35.78 to −34.40 cents | −32.35 to −31.09 cents |
| The same at 2×, 44.1 kHz | −35.78 to −35.42 cents | −32.35 to −32.02 cents |
| Level / pitch at k 4.05, 6, 8 (440 Hz) | −24.9 dBFS / −5.3; −11.9 / −101.2; −10.9 / −125.7 | −21.4 / −5.2; −10.7 / −64.3; −10.3 / −70.1 |
| Passband, LP24 at k 4 / 8 | −14.0 / −21.0 dB | −14.0 / −21.3 dB |
| THD at drive 1 / 4 / 16 | 0.50 / 14.4 / 34.3% | 0.23 / 15.3 / 36.3% |
| Strongest alias, drive 4: 1× / 2× / 4× | −47.4 / −102.0 / −150.0 dB | −59.4 / −87.0 / −101.3 dB |
| Strongest alias, drive 16: 1× / 2× / 4× | −33.2 / −65.7 / −120.6 dB | −31.8 / −53.6 / −75.4 dB |
| Peak at k 8, drive 32, under an audio-rate sweep | 2.35 | 1.47 |
| Silence after an impulse, 100 Hz, k 3, no FTZ: subnormal outputs, exactly 0 after | 0, 1.77 s (2× and 4× the same: 0, 1.76 and 1.74 s) | the same |
| Block sizes 1 / 37 / 4096 (controls changed every 1000 samples), allocations | identical, 0 | identical, 0 |
| A resonant cutoff step, re a unit step: `setCutoff()` / modulation / steady | −46.5 / −29.5 / −114.7 dB | −46.4 / −29.5 / −114.7 dB |

At 2× and 4× the oversampled rows now also reach exact silence with no subnormal output (the prototypes' 875,000 subnormals are gone).

**CPU** (ns per sample per channel, 48 kHz, LP24 at 1 kHz on a saw, `relwithdebinfo`, Apple M1 Ultra):

| | Warm on (tanh) | Warm off (cubic) |
|---|---:|---:|
| k 2, drive 2 | 98.3 | 63.5 |
| k 6, drive 16 | 126.1 | 81.8 |
| Stereo, per sample (two channels) | 191.6 | 126.3 |
| A cutoff ramp every block | 95.4 | 64.3 |
| Modulation every 32 samples | 92.7 | 64.2 |
| 2× with the `Oversampler` | 201.5 | 139.4 |
| 4× with the `Oversampler` | 389.1 | 266.2 |
| Bypassed | 0.06 | |
| **16 voices × 2 stereo filters** (64 channels), per sample | **6,047 (29.0% of a core)** | |

The built filter is 13% (tanh) and 19% (cubic) faster than its prototype (113 and 78 ns; the prototype also kept solver statistics and divided by each Jacobian diagonal twice). The ramps and the grid cost nothing measurable (a ramp in progress, or modulation every grid point, is within noise of settled). Warm off saves 35% at typical settings. The full 16-voice, two-filter, stereo case is 29% of a core, before SIMD across channels (Phase 3); a part's filter is per voice, so a patch with one part and four voices costs an eighth of that.

## Tests

[ladder_filter_test.cpp](../../tests/dsp/filters/ladder_filter_test.cpp), 24 tests, each tolerance derived in the test from the model or from the decay of the slowest pole:

| Test | What it pins down | Tolerance and why |
|---|---|---|
| The tap table | `filterTapMixes` against the six mixes written out independently | exact |
| The small-signal response, every response, Warm on and off | the impulse response at −80 dBFS against the bilinear map of the prototype, at 100 Hz–19 kHz and k 0 and 3 | 4·10⁻⁵ absolute: the saturators are linear to 3·10⁻⁹ there, and float rounding over the response's decay is about 10⁻⁵ of a peak of at most 1 (measured: 8.7·10⁻⁶) |
| At the cutoff, LP24 is 1/\|4 − k\| | the response at the cutoff, placed on an FFT bin, at 44.1/96/192 kHz and k 0, 2, 3.5 | 10⁻⁴: 65,536 samples are 34 time constants of the slowest case, so only rounding remains (measured: 2.7·10⁻⁵) |
| Self-oscillation starts at k = 4 | a 10⁻¹⁵ impulse decays at k 3.99 and grows at 4.01, at 110 Hz–7 kHz and 44.1–192 kHz, Warm on and off | the bilinear map keeps the threshold exactly; the margin is the growth over 25 cycles (e^0.07) |
| Self-oscillation is flat across the cutoff and falls with resonance | the pitch at k 4.4 within 1.5 cents from 55 Hz to 3.5 kHz; −35.8 (tanh) and −32.3 (cubic) cents at 440 Hz; flatter and louder as k rises; under 0 dBFS at k 8 | the report's figures (spread 1.17 and 1.07 cents; pitch within 0.04 cents of the pinned values, checked to 0.2) |
| Silence stays silent | no input at k 8 gives exact zeros | exact |
| The passband against resonance | LP24 at 20 Hz is the prototype and 1/(1 + k) within 0.01 dB; HP24 at 10 kHz stays within 0.1 dB of 0 dB | the prototype's own distance from those values (0.002 and 0.04 dB) |
| Drive is a gain on the input | 4× drive at −80 dBFS gives 4× the output | 10⁻⁵ of the peak (the saturators: 5·10⁻⁸) |
| Warm Drive's harmonics | tanh's H5, H7, H9 at least 3 dB above the cubic's at drive 1 | the report: 5.1, 5.5, 6.7 dB |
| Bounded at maximum resonance and drive | k 8, drive 32, a full-scale saw, the cutoff swept 20 Hz–20 kHz at 500 Hz through the modulation input; DC steps to ±10 at k 4.5 | drive·max\|x\|/(1 + k) + 1, the loop's DC gain plus a self-oscillation under 0 dBFS (measured 2.35 against 4.6) |
| Bypass | bit for bit the input; leaving bypass equals a fresh filter | exact |
| A response change crossfades | during the fade the output is (1 − b)·LP24 + b·HP24 of the same state, b linear over 480 samples at 48 kHz | 10⁻⁶ (measured 6.6·10⁻⁸) |
| A cutoff step ramps in log₂ over 10 ms on the grid | `currentCutoff()` after each sample: unchanged until the next grid point, then log₂ linear to the target over round(0.01 fs) samples | 10⁻⁵ relative |
| A cutoff step at resonance clicks far less than a jump | the 10 ms ramp against a modulation step (one 32-sample ramp of g) | at least 10 dB less energy above 1 kHz (measured 17.0 dB) |
| Block sizes | blocks of 1, 37, and 4096, with every control changed at fixed samples | bit for bit |
| Stereo | two equal channels give equal outputs, equal to the mono filter; a silent channel stays silent | bit for bit |
| The lifecycle contract | `checkLifecycleContract` in stereo | bit for bit |
| Allocation | processing and every control call | none |
| Silence reaches exactly 0 | an impulse at 100 Hz, k 3, without FTZ: no subnormal output; at rest (`isAtRest()`, every state exactly 0) within 2.0 s, and exactly 0 out from then on | the slowest pole: 87.3 time constants of 22.9 ms is 2.0 s from a peak of 1 (the output measured silent from 1.77 s) |
| The loop is solved | against the same equations solved in double with `std::tanh` to 10⁻¹³: a 110 Hz saw at −6 dBFS, drive 4, k 3.8, 1 s | under −100 dB of the signal: per-sample errors of a few 10⁻⁷ (`fastTanh`, the quadratic Newton step) times a resonance gain of 5 (measured −119.7 dB; one Newton step: −81.2 dB) |
| Between grid points g is interpolated linearly | a modulation step read at the next grid point: the output against a double-precision linear ladder fed g linear across the 32 samples, at −80 dBFS | 10⁻⁴ of the peak (float tan and the loop's rounding) |
| reset() restarts the grid | a filter reset mid-segment, then a cutoff step, against a fresh filter | bit for bit |
| Controls clamp, and NaN is safe | NaN, ±∞, −1, 0, 10⁹ for every control | finite output, cutoff in [1 Hz, fs/2] |
| A randomized stress test | random controls at random samples, every response, Warm and bypass toggled, 44.1–192 kHz | finite, under drive·max\|x\|·16 + 1, bit for bit across blocks of 1, 37, 4096 |

And for the halfbands, in [oversampler_test.cpp](../../tests/dsp/resampling/oversampler_test.cpp): after a signal, a round trip at 2× and 4× without FTZ is at rest (`isAtRest()`) within 4096 samples, its output exactly zero from then on, and no sample inside or out is ever subnormal (the slowest allpass section, a = 0.944, falls below FLT_MIN within 1515 samples).

Red first: against an inert stub (a pass-through), 13 of the first 21 failed; the other 8 are properties a pass-through also has (silence, block sizes, stereo, allocation, the lifecycle). The halfband test failed against the unflushed sections. Then 26 deliberate breaks, one at a time: no prewarp; feedback from y3; no Newton iterations; one Newton iteration; linear tanh stages; a cubic knee of 1/3; a wrong HP12 weight; no response crossfade; bypass keeping its state; bypass still processing; g held per segment; no cutoff ramp; drive after the filter; no state flush; no output flush; the grid not reset; channels sharing a state; the modulation ignored; resonance capped at 4; the state update s = y; Warm inverted; and in the halfbands, the upsampler's or downsampler's states unflushed, and their outputs unflushed. The first pass caught all but four (one Newton iteration, no state flush, g held per segment, the grid not reset), which led to three tests (the double-precision reference, g's interpolation, the grid after `reset()`), `isAtRest()`, and the tighter 2.0 s bound; then all 25 were caught. The 26th, a Newton tolerance of 10⁻³ instead of 10⁻⁵, is not: Newton converges quadratically, so stopping when a step is under 10⁻³ leaves an error near 10⁻⁶, below anything audible or tested. The tolerance is a cost choice (Phase 3 can measure a looser one), not a correctness one.

## References

- LennarDigital, *Sylenth1 User Manual*, v2.5, 2015, pp. 5, 8, 16–18, 30–31; [specifications](https://www.lennardigital.com/sylenth1/specs.php); [version history](https://lennardigital.com/sylenth1/whatsnew.php) and the [v1.x news archive](https://lennardigital.com/home/oldnews1.php). The developer's and users' posts on KVR are listed in the [feature inventory](../reference/sylenth1-features.md#3-filters-pp-5-1618), with confidence ratings.
- The theory's references are in [filter-theory.md](filter-theory.md#references): Moog (1969), Stilson and Smith (1996), Huovilainen (2004), Välimäki and Huovilainen (2006), Zavalishin (2018), Voipio (2012), D'Angelo and Välimäki (2013), Parker et al. (2016).
