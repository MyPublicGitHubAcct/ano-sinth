# Brutalis documentation

Each project task and technique gets a write-up here (Part 4 of [PLAN.md](../PLAN.md)). Each write-up covers:

- the general theory;
- how it is applied in this codebase, with file links;
- the trade-offs and the alternatives considered;
- the tests that pin the behavior down;
- references.

Docs change in the same commit as the code they describe.

## Suggested reading order

### 1. Working on the project

Read these first, in this order.

1. [Building and toolchains](development/building.md): requirements, presets, the C++ standard, and platform baselines.
2. [Project structure and dependency rules](development/project-structure.md): the libraries, why `gui` and `interop` are JUCE modules, and what may depend on what.
3. [Test-driven development in this repo](development/testing.md): the red-green-refactor loop, with Catch2 and CTest.
4. [Code quality: warnings, formatting, static analysis](development/code-quality.md)
5. [Product identity](development/product-identity.md): how the private plugin codes stay out of the repository.
6. [Plugin validation with pluginval](development/plugin-validation.md)
7. [Continuous integration](development/ci.md)

### 2. DSP theory and its implementation

This section fills up during Phase 1 (oscillators, filters, envelopes, effects, and so on). Read the foundations in this order; later topics build on them.

**Foundations (M1.0)**

1. [The processor lifecycle: prepare, reset, process](dsp/processor-lifecycle.md): real-time rules, `ProcessSpec`, `AudioBlock`, and the contract every processor is tested against.
2. [Levels and pitch: decibels, notes, and master tuning](dsp/levels-and-pitch.md)
3. [Fast approximations: exp2 and tanh](dsp/fast-math.md): range reduction, minimax polynomials, avoiding cancellation, and measured error.
4. [Parameter smoothing and zipper noise](dsp/parameter-smoothing.md)
5. [Denormals: why they hurt and how we keep them out](dsp/denormals.md)
6. [White noise and reproducible randomness](dsp/white-noise.md)
7. [How we measure DSP quality](dsp/measuring-dsp.md): FFT, windows, coherent sampling, THD, aliasing, frequency response, and golden files. Read this before writing any spectral test.
8. [Oversampling: 2× and 4× with polyphase IIR halfbands](dsp/oversampling.md)

**Oscillators (M1.1)**

9. [Band-limited oscillators: choosing the anti-aliasing method (D6)](dsp/band-limited-oscillators.md): why naive waveforms alias; PolyBLEP, minBLEP, and mipmapped wavetables measured side by side; why minBLEP with 64 taps.
10. [Oscillator waveforms: the 8 shapes as data](dsp/oscillator-waveforms.md): any piecewise-linear waveform as jumps and bends; Sylenth1's 8 waveforms as a table of corners; why noise lives beside the oscillator; every waveform measured.
11. [Oscillator controls: tuning, volume, pan, and invert](dsp/oscillator-controls.md): pitch as one fractional note and how exact it is; volume tapers; pan laws (equal power, linear, balance) and what each does in mono; smoothing gains; the two-saw PWM trick; the oscillator slot.
12. [Oscillator phase: start phase, retrigger, and free-running](dsp/oscillator-phase.md): a phase reset as an edge; band-limited jumps at a sub-sample time (the hard-sync technique); why the delayed sine needs its own residuals; free-running and where a free note starts; aliasing measured against a naive jump.
13. [Oscillator unison: detune, stereo spread, and the voice count](dsp/oscillator-unison.md): why detuned copies sound big; spacing and knob laws; placing voices in stereo and how Pan combines with the spread; how a stack's level grows; noise as one source mixed to sound like many; what is known about Sylenth1's laws; changing the voice count without clicks; what SIMD across voices will need.
14. [Oscillator range: from 0.01 Hz to beyond Nyquist](dsp/oscillator-range.md): what "stable" means at each end; float phase, accumulated residuals, and denormals at the bottom; what a band-limited waveform is once its fundamental passes Nyquist, the options measured, and the fade about the mean chosen; why a pitch step clicks in a minBLEP oscillator and how it is band-limited; period and phase at 0.01 Hz over whole cycles; hours of simulated time; the slot under stress at every rate and block size.
15. [Oscillator drift](dsp/oscillator-drift.md): where analog drift comes from and why it is heard as beating between voices; random walks, one- and two-pole noise, sample and hold, slow sines, and 1/f noise compared by spectrum, distribution, independence, and cost; what is known about Sylenth1's drift; the two-pole chosen, held per 64 samples, one per polyphonic voice.

**Envelopes and LFOs (M1.2)**

16. [Envelopes: the ADSR](dsp/envelopes.md): the four stages; linear, analog RC, and overshoot-target exponential segments; constant rate against constant time; why float recursion fails on long stages and counting fixes it; clicks at retriggers and sustain changes; denormals in analog-style tails; what is known about Sylenth1's envelopes; six models measured, and the counted overshoot-target exponential chosen and built.
17. [LFOs](dsp/lfos.md): how an LFO differs from an audio oscillator; why an LFO's steps click, and why band-limiting doesn't stop the click but a short slew does; phase accuracy over hours in float, double, and fixed point; control rate against audio rate, held or interpolated; per-voice against global, and what Free means; sample and hold, smoothed random, and the Lorenz attractor (and why it renders differently on each compiler); what is known about Sylenth1's LFOs; eight models measured; a 64-bit phase, exact rows in runs, curved rows interpolated every 32 samples, and a 1 ms slew on every jump chosen and built.
18. [Tempo sync](dsp/tempo-sync.md): note values as exact fractions, and why a float table puts an arpeggiator on the wrong sample; how hosts report tempo, position (PPQ), and transport, and what they leave out; free-running at the synced rate against locking to the transport, and hybrids that nudge the phase; what is known about Sylenth1's Sync; five models measured for drift over three hours, tempo steps and ramps, loops, locates, transport start and stop, block sizes, and CPU; rate only from the note-on with an exact 64-bit increment (`NoteValue`, `notePhaseIncrement`, `HostTempo`, `Lfo::setPhaseIncrement`) chosen and built.

**Filters (M1.3)**

19. [Filter theory: ladders, zero-delay feedback, and saturating stages](dsp/filter-theory.md): poles, slope, and resonance; the four-pole ladder, why it self-oscillates at a loop gain of 4 and loses passband as resonance rises; six responses from one loop by mixing its stages; the transistor ladder's five saturators, drive, and why a saturating ladder goes flat when it self-oscillates; the bilinear transform, prewarping, and cramping; the topology-preserving transform and zero-delay feedback; Newton, linearised, and input-only nonlinear solvers; Huovilainen's explicit ladder and its exact tuning; stability, denormals, and why saturators alias.
20. [Filters](dsp/filters.md): what is known about Sylenth1's filter; seven models measured (linear, Newton, linearised, and input-only ZDF ladders, Huovilainen's at 2× and 4×, a unit-delay ladder, and an SVF) for the response against the analog prototype in every mode, self-oscillation pitch, threshold, and level at 44.1–192 kHz, passband loss, harmonics against drive for four saturators (the Warm Drive pair), aliasing at 1×, 2×, and 4×, solver error, stability, subnormals, block sizes, clicks and zipper noise, and CPU; a Newton-solved ZDF transistor ladder with tanh and cubic saturators (Warm Drive), tap-mixed responses, and a 32-sample coefficient grid chosen and built (`LadderFilter`).
21. [Keytracking](dsp/keytracking.md): keytracking as an offset in octaves, the reference note, and which pitch it follows (key, glide, bend, master tune); why a saturating ladder at keytrack 10 keeps its intervals but not its absolute pitch; feeding a control-rate value into the filter's 32-sample grid, and why the value for the segment's end is the one to feed; what is known about Sylenth1's Keytrack (the developer's source, 0 at A-1 to 10 at A9); five models measured for the law, intervals from C1 to C7, the clamp near Nyquist, the master tune, glides, bends, zipper, legato steps, block sizes, and CPU; linear in octaves from the voice's sounding pitch, reference A-1, fed at the grid with the pitch at the segment's end, chosen and built (`keytrackOctaves`, `keytrackSource`, `masterTuneSemitones`). Also found: the built filter's Newton solve fails near Nyquist above the threshold.
22. [Filter stability](dsp/filter-stability.md): why Newton fails on a trapezoidal ladder when g = tan(π·fc/fs) is large, and why the equations still have exactly one solution, in a known bracket; globalised Newton (line search, step limits, a better guess, a better variable) against a nested bracketing solve, and the rtsafe safeguard it needs; what the filter should do near Nyquist, from a double-precision reference; where the built solver fails (from 0.30 fs at high drive and resonance); nine solvers measured over a grid of cutoff, resonance, drive, rate, and Warm Drive, at the held cutoff, in the ordinary case, and for CPU; extreme settings (audio-rate modulation across the clamp, 1 Hz, DC, the ceilings, NaN and inf, block sizes, switching while self-oscillating, five minutes); aliasing at the developer's corner and of the self-oscillation at 1×, 2×, and 4×; why ADAA inside the loop detunes it; Newton with a nested bracketing fallback where it does not converge, and the input guard, chosen and built (`LadderSolver`), with the tests that pin it: the held cutoff against the reference, a per-sample error bound derived from float rounding and the step tolerance, bit-identity with the old solve wherever Newton converges, and the extremes.
23. [The Filter Control](dsp/filter-control.md): adding two cutoff knobs in knob units, in octaves, or in Hz, and why the first two are one family that differs only in octaves per unit and an anchor; where a clamp of a control plus its modulation can sit; combining resonances; feeding a combined cutoff through the filter's 10 ms ramp and 32-sample grid without smoothing twice or breaking a clamp; what is known about Sylenth1's Filter Control (the developer: the knob values add on a logarithmic scale; the old LCD's "faulty" Hz scale, 146.16 Hz at the knobs' centre default); four cutoff models, four clamps, four resonance laws, and four feeds measured on `LadderFilter` (the effective cutoff over a grid, the range and the ceiling, the knob's feel, keytrack C1–C7, full modulation, self-oscillation, a knob step, a MIDI CC sweep, a clamp during a ramp, block sizes, CPU); knob units added at an octave per unit from 4.5675 Hz, each control clamped with its own modulation, resonances added, chosen and built (`filter_control.hpp`); one global knob ramp read in closed form at each voice's own grid (`LinearSmoother::valueAfter`); why k fed through a ramped setter slews, and the resonance modulation input that replaces it; the tests that pin it. Also found: a fast, deep sweep of a self-oscillating ladder overshoots by 29–31 dB.

**The voice (M1.4)**

24. [The voice: parts, filter routing, and the level into the filter](dsp/voice-architecture.md): what a voice composes; routing as a gain matrix and three summing laws; where an amplifier sits against a saturating filter; gain staging into a saturator and how sums of oscillators grow; per-part and per-filter buffers, in-place filtering, chunked and stage-major schedules; a mono path that is exact while every voice is centred; where an oversampling boundary can sit; what is known about Sylenth1's voice (the developer: a part crossing into the other filter goes through both amp envelopes; an unrouted part is silent, even on Bypass; the Input Select's options; the Mix's internal scale; velocity in the init patch); a prototype voice on the real classes, measured for each reading, the level each kind of patch presents to the filter and where its saturation reaches 1%, the knee against Volume (and why, in a ladder, it depends on the cutoff), equality to the bit at every block size, memory, and CPU; the crossing envelope before the filter, unheard parts not run, AB a plain sum, a filter input level, and per-filter buffers on the voice's grid chosen and built (`dsp::Voice`), with the tests that pin it.
25. [Voice allocation: polyphony, stealing, freeing, and the Voices readout](dsp/voice-allocation.md): what an allocator does; stealing policies (oldest, quietest, released first, the same key) and their failure cases; a key played again, a new voice or its own ("voice recycling"); the ways a stolen note can end (a cut, a fade before the new note or on a spare voice, a retrigger in place) and why each clicks or doesn't; freeing at exactly 0, at a threshold, or on a silent hold; what a voice readout can count; keeping an allocator's decisions independent of the host's block size; what is known about Sylenth1's allocation (the developer, 2016: Polyphony 1 steals the single voice, releases overlap above it, a repeated key takes a new voice; the readout's formula and the manual's 204/336 screenshot; the Polyphony 0 fix); a prototype allocator on `dsp::Voice`, measured for the click at a steal on four patches, a repeated key's level, freeing rules, busy voices over time, the readout, a 32-note strum and a held bass under a melody, Polyphony changes, block sizes and allocation, spare voices, and CPU. Stage 1: recommendations for the project owner (released notes first, a retrigger in place that keeps the filters, a new voice for a repeated key, E10 and E11 standing, only audible envelopes keeping a voice busy, a per-part readout).

### 3. Performance

1. [Benchmarking: measuring CPU cost](performance/benchmarking.md): the harness, its pitfalls, and the real-time budget.
2. [CPU baselines](performance/baselines.md): recorded results, newest first. Phase 3 starts from these.

### 4. Reference material

- [Sylenth1 feature inventory](reference/sylenth1-features.md): the Phase 1 feature target.
