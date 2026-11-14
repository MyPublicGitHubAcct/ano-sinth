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

### 3. Performance

1. [Benchmarking: measuring CPU cost](performance/benchmarking.md): the harness, its pitfalls, and the real-time budget.
2. [CPU baselines](performance/baselines.md): recorded results, newest first. Phase 3 starts from these.

### 4. Reference material

- [Sylenth1 feature inventory](reference/sylenth1-features.md): the Phase 1 feature target.
