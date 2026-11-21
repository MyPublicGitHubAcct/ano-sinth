# Parameter smoothing and zipper noise

Turn a filter cutoff knob on a careless synth and you hear a fine grainy buzz on top of the sweep: **zipper noise**. This page explains where it comes from and how `LinearSmoother` and `ExponentialSmoother` remove it.

## Theory

### Where zipper noise comes from

Parameters reach the audio thread far less often than samples do. A host sends automation once per block (every 64–1024 samples). A UI knob moves at screen rate (60 Hz). A MIDI CC arrives at most every millisecond or so, and has only 128 steps. If the DSP applies each new value the moment it arrives, the parameter is a **staircase**:

- a gain staircase multiplies the signal by a step function. Every step is a discontinuity in the output's amplitude. Its spectrum is broadband, so it sounds like a click, and a sweep produces a click every block: a buzz at the block rate.
- a cutoff or pitch staircase makes a filter or oscillator jump. The jump itself is a discontinuity in the waveform's slope or phase, with the same result.

The fix is to turn the staircase into a continuous curve: interpolate from the old value to the new one over a short time, typically 5–50 ms. Short enough that the control still feels immediate, long enough that no single step is audible.

### Linear and exponential ramps

- **Linear:** equal steps of (target − start)/N for N samples. The rate of change is constant, and the largest step is the change divided by N. It suits parameters that are already *perceptually scaled*, meaning a knob in dB, in semitones, or a normalised 0–1 position, so that equal steps sound equal.
- **Exponential** (one-pole): each sample moves a fixed fraction of the *remaining* distance, `y += (1 − a)·(target − y)`. It is the discrete form of an RC low-pass filter, the way an analog synth's control voltages naturally slew. It responds fast at first and eases in at the end, which hides the moment the target is reached. A plain one-pole never actually arrives: the distance shrinks geometrically forever, and in float it eventually stalls a few ulps away.

**Multiplicative** (log-domain) ramps are a third option, equal *ratios* per sample, for frequencies or linear gains. Here they are obtained by smoothing in the log domain (semitones or dB) and converting afterwards, so they don't need a separate shape.

### Choosing the time

Zipper noise disappears once no step is perceptible. For a gain change, a 10 ms ramp turns a click into a fade. For cutoff sweeps, 20–50 ms. Longer ramps make the control feel sluggish. The right value depends on the parameter, so the smoother takes it as a setting in seconds.

## How it is applied here

[libs/dsp/include/anoesis/dsp/control/smoother.hpp](../../libs/dsp/include/anoesis/dsp/control/smoother.hpp) defines one class template, `Smoother<SmoothingShape>`, and two aliases, `LinearSmoother` and `ExponentialSmoother`. Both satisfy `Preparable` ([processor lifecycle](processor-lifecycle.md)).

**Every ramp lands exactly on its target after exactly N samples**, whatever its shape, where N = round(rampSeconds × sampleRate). This is the main design choice, and it buys three things:

- `isSmoothing()` becomes reliable, so a processor can switch to a cheaper constant-parameter path the moment the ramp ends;
- nothing stalls short of the target, and no denormal tail is left behind;
- linear and exponential ramps are interchangeable without retuning their times.

The exponential shape is defined so that the remaining distance falls to **10⁻⁴ (−80 dB) of the step** over the ramp time: decay^N = 10⁻⁴, a time constant of N / ln(10⁴) ≈ N/9.2 samples. The last sample then snaps the final 0.01% of the way. A jump of −80 dB relative to the change is inaudible.

Other details:

- **Linear values are computed, not accumulated.** Each value is `start + step·k`, not `value += step`. Over a 10-second ramp (480,000 samples) accumulation would collect 480,000 roundings; the computed form has one or two.
- **Exponential values track the remaining distance** (`distance *= decay; value = target − distance`). The distance shrinks geometrically and monotonically, so the value never overshoots.
- **Setting an unchanged target is a no-op.** Hosts often resend every parameter every block, and restarting the ramp each time would make it crawl.
- **Retargeting mid-ramp** starts a new full ramp from the current value, so the output stays continuous.
- `skip(n)` advances a whole block at once for code that reads a parameter once per block, and `fill(span)` writes a per-sample buffer of values.
- **`valueAfter(n)` (linear only) reads the ramp n samples ahead without advancing it**: exactly the value `next()` would return n calls from now, since it evaluates the same `start + step·k` at the step it would be on. So one ramp can be shared by many readers at different offsets. The Filter Control's knob ramps are global, and each voice reads them at the ends of its own 32-sample grid segments, which are not aligned across voices ([filter-control.md](filter-control.md#the-knob-ramps-one-global-ramp-read-at-each-voices-grid)). Reading n ahead and then skipping m gives the same values as reading m + n ahead, so where a host block is split changes nothing. The exponential shape has no such read: it would need a `pow` per read.
- The ramp time is stored in seconds and converted again by every `prepare()`, so a sample-rate change keeps the time in milliseconds the same.
- The ramp length is capped at 2²⁴ samples, the largest count a float holds exactly.

## Trade-offs and alternatives

- **Exact landing versus a pure one-pole.** A pure one-pole is one multiply-add per sample with no counter, and its output is smooth to every derivative. But it has no end: it needs an epsilon test to stop, and that test fails when the float update stalls. The counter costs a decrement and a branch, and the −80 dB snap is inaudible.
- **Per-sample versus per-block smoothing.** Updating coefficients once per block (or per 16 or 32 samples) is cheaper for parameters whose coefficient computation is expensive, such as filter cutoffs. `skip()` supports that pattern. Which parameters run at control rate is decided in M1.5.
- **Smoothing the parameter versus smoothing the coefficient.** Smoothing a filter's *coefficients* linearly can pass through unstable or strange intermediate filters. Smoothing the *parameter* (cutoff in semitones) and recomputing the coefficients is the safer default.
- **Slew limiting** (a maximum change per sample) gives a ramp time that grows with the size of the jump. It is useful for glide and portamento (M1.4), less so for knobs.

## Tests

[tests/dsp/control/smoother_test.cpp](../../tests/dsp/control/smoother_test.cpp):

- the ramp length is set in seconds and stays independent of the sample rate (480 samples at 48 kHz, 960 at 96 kHz); a ramp is immediate before `prepare()`; negative times count as zero;
- linear ramps take equal steps and land bit-exactly; the largest sample-to-sample jump equals change/N, which is the zipper-noise reduction itself, shown numerically;
- a 10 s linear ramp is within 1e-7 of 0.5 at its midpoint (no drift);
- a retarget mid-ramp stays continuous; an unchanged target doesn't restart the ramp;
- zero ramp time, `setCurrentAndTarget()`, and `reset()` all jump immediately;
- `skip()` matches stepping one sample at a time, and `fill()` is bit-identical to it;
- `valueAfter(n)` is bit for bit the value `next()` returns n calls later, in every state (settled, at a ramp's start, partway after `skip()` or `next()`, retargeted mid-ramp, one sample from the end, landed, with no ramp time), doesn't move the smoother, gives the target for any n past the ramp's end, and is unchanged by where a `skip()` splits the read;
- the exponential shape matches 1 − 10^(−4k/N) at every sample, reaches 63.2% after one time constant (N/9.21 samples), and its final snap is 1e-4 of the step;
- both shapes are monotonic and never overshoot, upward (20 Hz → 20 kHz) and downward.

Every tolerance has its derivation next to it in the test.

Cost ([CPU baselines](../performance/baselines.md), Apple M1): 0.80 ns per sample for a linear ramp, 0.05 ns once settled, and 3.72 ns for an exponential ramp. The exponential figure is the first Phase 3 candidate: the likely cause is aliasing between the output span and the smoother's members, not the arithmetic.

## References

- Will Pirkle, *Designing Software Synthesizer Plugins in C++* (2nd ed., 2021), "Parameter smoothing".
- Julius O. Smith III, *Introduction to Digital Filters* (online book, CCRMA): the one-pole filter and its time constant.
- JUCE `SmoothedValue` (`juce_audio_basics/utilities/juce_SmoothedValue.h`): a widely used linear and multiplicative smoother, for comparison.
