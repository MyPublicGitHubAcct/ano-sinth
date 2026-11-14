# Brutalis plan

The working plan for the project. [CLAUDE.md](CLAUDE.md) holds the rules: stack, architecture, and conventions. This file holds the **order of work and its status**. The feature target is [docs/reference/sylenth1-features.md](docs/reference/sylenth1-features.md).

## How to use this file

- Status markers: `[ ]` not started · `[~]` in progress · `[x]` done · `[-]` dropped (say why).
- Work milestones in order unless the dependencies listed say otherwise.
- Every task meets the **Definition of done** below before it is ticked.
- Record each decision under **Decisions log** with the date. Move it out of **Open decisions** once it is made.
- Keep this file current. Update it in the same commit as the work.

## Definition of done (every task)

1. A Catch2 test was written first and failed, then the code made it pass (TDD).
   Before committing, `tools/check.sh` passes locally (macOS).
2. Builds without warnings and all tests pass on macOS, Linux, and Windows. *macOS is checked locally before every commit. Linux and Windows are checked by Version CI on every major or minor version (`vX.Y.0`). A task counts as done once it passes macOS. Linux or Windows failures found in a version run must be fixed before that version ships.*
3. Code is formatted with clang-format, and clang-tidy reports nothing.
4. Audio-thread code is real-time safe. `libs/dsp` code meets the portability rules in CLAUDE.md (no JUCE, no allocation after prepare, and so on).
5. Part 4 docs are written or updated for the technique (see the doc column of each milestone).
6. PLAN.md is updated.

---

## Current status

**Phase 0: Foundation, done.** Version CI passed on the `v0.1.0` tag (run 43529431143), on macOS, Linux, and Windows. The skeleton builds, and all 7 tests pass on macOS (Apple Clang) under `debug`, `asan`, and `release` (universal binary, real identity). Format and clang-tidy are clean. The repo is public on GitHub and MIT-licensed (D1). Commits are tested locally with `tools/check.sh`. GitHub Actions runs only on `vX.Y.0` tags, as a fail-fast chain: Linux, then Windows, then macOS, then the macOS release build.

Still open:

- **Linux and Windows:** tested in Version CI, ahead of macOS. Not built locally. See the section below.
- **Distribution:** GitHub never hosts compiled builds. See **Distribution** below.

**Phase 1: M1.0 done** (2026-11-13). `libs/dsp` has the processor lifecycle, math utilities, smoothing, denormal protection, seedable noise, and 2×/4× oversampling. `tests/support` holds the measurement harness (FFT, THD, aliasing, frequency response, golden files), and the first CPU baselines are recorded. 96 tests pass on macOS (debug and ASan/UBSan).

**M1.1 (oscillators) in progress.** D6 is decided: **minBLEP, 64 taps** (2026-11-13), chosen by prototyping and measuring five candidates with the harness. The 8 waveforms are built on it as data (2026-11-13), with provisional shapes to be measured in M1.13. Tuning, volume, pan, and invert are done (2026-11-13), in an `OscillatorSlot` that the unison task will extend. Retrigger and phase are done (2026-11-14): a retrigger while sounding is a band-limited phase jump, which hard sync will reuse. Unison is done (2026-11-14): 0–8 voices per oscillator with detune and stereo spread, Noise as one source per slot, and voice-count changes that fade instead of clicking, all on working hypotheses to be measured in M1.13 (inventory O8, O13–O16). Stable from 0.01 Hz up to Nyquist is done (2026-11-14): above Nyquist a voice fades to its waveform's mean from 0.45 fs, pitch steps are band-limited, and the whole slot is tested across the range at every rate and block size, with hours of simulated time in a hidden report. **Next:** oscillator drift (v3).

---

## Open decisions

Settle these before or during Phase 0. Each one affects the work that follows.

| # | Decision | Why it matters | Needed by |
|---|---|---|---|
| D7 | **Preset and bank file format** (for example JSON or XML with a version field) | A long-term compatibility commitment. | M1.10 |
| D9 | **Visual design direction for the GUI** | Must be original (clean-room). Affects `libs/gui`. | M1.11 |

Note: AU is a macOS-only format, so Linux and Windows ship VST3 and Standalone.

---

## Phase 0: Foundation

Goal: an empty but complete skeleton. It builds on all three platforms, runs one test, enforces the conventions, and publishes its first docs.

- [x] **M0.1 Repo hygiene.** Add `.gitignore` (build dirs, IDE files, `.DS_Store`), `.gitattributes` (line endings), and `README.md`. Add licensing: `libs/dsp/LICENSE` (MIT, Anoesis Audio) and a root `LICENSE` per D1.
- [x] **M0.2 Dependencies.** Add the JUCE submodule at `external/JUCE`, checked out at tag `9.0.5` (or the latest 9.x at setup time). Add the Catch2 submodule at `external/Catch2`, at tag `v3.16.1` (or the latest v3).
- [x] **M0.3 CMake skeleton.** *Verified on macOS. Linux and Windows are checked in the first version run (see **Linux and Windows** below).*
  - Add a top-level `CMakeLists.txt`.
  - Add targets for `libs/dsp`, `libs/gui`, `libs/interop`, `plugin/`, and `tests/`. Each lib is a separate target with its own namespace.
  - Enforce the dependency direction: `dsp` has no dependencies, and `gui`/`interop` → JUCE.
  - Set the C++ standard to the highest supported (CLAUDE.md). Turn extensions off.
  - Add a JUCE plugin target for VST3, AU, and Standalone. All identity values come from the `BRUTALIS_*` variables.
  - Load `cmake/Identity.example.cmake`, then `cmake/Identity.local.cmake` if it exists (`include(... OPTIONAL)`).
  - Add a `BRUTALIS_REQUIRE_REAL_IDENTITY` option that fails configure if any placeholder value is still in use. Release presets and CI turn it on.
  - Validate both codes: 4 characters, at least one uppercase letter.
- [x] **M0.4 Presets and toolchains.** *Verified on macOS. Linux and Windows are checked in the first version run (see **Linux and Windows** below).* Add a `CMakePresets.json` with Debug, Release, and RelWithDebInfo for each platform: Apple Clang, GCC, and MSVC, all with Ninja. Turn on warnings-as-errors with the per-compiler flags.
- [x] **M0.5 Tooling.**
  - Add `.clang-format` and `.clang-tidy` (Core Guidelines checks).
  - Add an optional sanitizer preset (ASan/UBSan) for Clang and GCC.
  - Add a pre-commit format check or a script that does the same.
- [x] **M0.6 CI.** *[version-ci.yml](.github/workflows/version-ci.yml) runs only on `vX.Y.0` tags or manual dispatch, as a fail-fast chain: Linux (GCC 12) tests, then Windows (MSVC) tests, then macOS tests, then the macOS release build. Day-to-day testing is local (`tools/check.sh`). Secrets are set. Passed in full on run 43524447930, after four Linux fixes found by the first version runs (see docs/development/ci.md, History). **pluginval** runs as a CTest test (strictness 10, VST3, pinned v1.0.4 with SHA-256 checks), so it runs locally and in every CI test leg. **auval** runs as a CTest test on macOS: it installs the AU temporarily, runs `auval -strict`, then always uninstalls and restores. The `v0.1.0` tag run (43529431143) passed every job.* A 3-platform matrix that configures, builds, runs tests, and runs a format check. Later, add plugin validation (pluginval; `auval` on macOS).
  - The test jobs use the placeholder identity.
  - Release jobs read `BRUTALIS_MANUFACTURER_CODE`, `BRUTALIS_PLUGIN_CODE`, and `BRUTALIS_BUNDLE_ID` from GitHub Actions secrets and set `BRUTALIS_REQUIRE_REAL_IDENTITY=ON`.
  - Add a CI check that fails if the real values appear in tracked files. Keep the values for this check in a secret too.
- [x] **M0.7 First test.** *Verified on macOS. Linux and Windows are checked in the first version run (see **Linux and Windows** below).* A Catch2 smoke test in each lib's test target, run by `ctest`. Proves the TDD loop works end to end.
- [x] **M0.8 Docs skeleton.** Add `docs/README.md` (index and reading order) and `docs/development/building.md`. Fill in the **Commands** section of CLAUDE.md with the real commands.
- [x] **M0.9 Initial commit(s).** Merged to `main` and pushed.

**Docs:** building and toolchains; project structure and dependency rules; how TDD works in this repo.
Written: [building](docs/development/building.md), [project structure](docs/development/project-structure.md), [testing](docs/development/testing.md), [code quality](docs/development/code-quality.md), [product identity](docs/development/product-identity.md), [CI](docs/development/ci.md).

## Linux and Windows

Decided on 2026-11-13:

- Development and pre-commit testing happen **on macOS only**.
- **Every major or minor version is tested on Linux and Windows**, in Version CI ([docs/development/ci.md](docs/development/ci.md)).
- Shipping Linux and Windows builds is deferred, along with all distribution (see **Distribution**).

The D8 baselines (Windows 10, Ubuntu 22.04 with GCC 12) still stand, and code stays standard C++23 with no compiler extensions.

What the one CI run with all three platforms showed (run 43498239659, on every push, before this policy):

- **Linux, GCC 12:** `debug` and `asan` both **passed** as-is.
- **Windows, MSVC:** failed on warning C4459, "declaration of 'sampleRate' hides global declaration". A namespace-scope test constant was hidden by a JUCE template parameter with the same name. The constant has been renamed, but MSVC hasn't checked that fix yet.

The first version run (`v0.1.0`, run 43516389668):

- **macOS:** `debug` and `asan` passed.
- **Linux, GCC 12:** failed to compile JUCE's own module sources under our `-Wconversion` (added after run 43498239659). JUCE's GCC flag set leaves `-Wconversion` out, and GCC's version is stricter than Clang's. Fixed, on GCC only, by compiling JUCE's units with `-Wno-conversion` and marking JUCE's include directories `SYSTEM`. Our own code keeps the flag (see [docs/development/code-quality.md](docs/development/code-quality.md)).
- **Windows, MSVC:** cancelled by fail-fast before it finished, so the C4459 fix is still unchecked.

Still to do:

- [x] A green Version CI run with Linux and Windows. *Run 43524447930 (manual dispatch on `main` at `6cdada5`) passed every job: macOS `debug` and `asan`, Linux GCC 12 `debug` and `asan`, Windows MSVC `debug` (95/95, which confirms the C4459 fix), and the macOS release build.*
- [x] Point the `v0.1.0` tag at a commit that passes. *The owner moved it from `407e966` to `2ab7587`. Its run, 43529431143, passed every job.*

## Distribution

Decided on 2026-11-13 by the project owner: **GitHub hosts source only. No compiled build is ever downloadable from it**, on any platform. That rules out workflow artifacts, release assets, packages, and Pages, whether produced by CI or uploaded by hand. The Version CI release job builds and validates the release binaries and then discards them.

- Enforced for workflows by [tools/check-no-published-builds.sh](tools/check-no-published-builds.sh), in `tools/check.sh` and CI job 1.
- Checked on 2026-11-13: no releases, no workflow artifacts, no packages, and no binaries anywhere in git history.
- Where and how builds are distributed (and signing, notarization, installers) is a separate, open decision. Distributing binaries also brings in the JUCE licence terms (D1).

---

## Phase 1: Sylenth1 parity

Goal: match every feature in the inventory, including the v3.x additions in §15. Extension points are designed in from the start.

Each milestone lives mostly in `libs/dsp` and is tested in isolation before integration. **Extensibility rule:** waveforms, filter types, effects, and mod sources and destinations are registered behind interfaces, so Phase 2 adds entries rather than editing switches.

### M1.0 DSP foundations and test harness: done
- [x] Sample-rate and block-size handling (`prepare`/`reset`/`process` lifecycle shared by all processors). *`ProcessSpec`, `AudioBlock`, the `Preparable`/`BlockProcessor` concepts, and `processInChunks` in `libs/dsp/include/anoesis/dsp/core/`. The `anoesis_test_support` library (`tests/support/`) provides `checkLifecycleContract()`, which every stateful processor is tested with. Doc: [processor lifecycle](docs/dsp/processor-lifecycle.md).*
- [x] Parameter smoothing, linear and exponential. *`Smoother<SmoothingShape>` (`LinearSmoother`, `ExponentialSmoother`) in `libs/dsp/include/anoesis/dsp/control/smoother.hpp`. Every ramp lands exactly on its target after round(seconds × rate) samples; the exponential shape decays to −80 dB of the step by then. Doc: [parameter smoothing](docs/dsp/parameter-smoothing.md).*
- [x] Math utilities: dB/gain, note/frequency (with **master tune**, a v3 feature), fast approximations (tanh, exp2) with accuracy tests. *`libs/dsp/include/anoesis/dsp/math/`. Measured: `fastExp2` 3.1e-7 max relative error (exact at integers); `fastTanh` 1.6e-7 absolute, 7.6e-7 relative. Docs: [levels and pitch](docs/dsp/levels-and-pitch.md), [fast approximations](docs/dsp/fast-math.md).*
- [x] Denormal protection strategy. *Two layers: `ScopedFlushDenormals` (FTZ/DAZ on x86 MXCSR, FZ on AArch64 FPCR and ARMv7/Cortex-M FPSCR; a no-op elsewhere), plus `flushDenormal()` for recursive state. Tested on macOS arm64; the x86-64 path compiles (no Rosetta locally to run it), and Version CI runs it on Linux and Windows. Doc: [denormals](docs/dsp/denormals.md).*
- [x] White noise generator: deterministic and seedable for tests. *`Random` (xoshiro128+, SplitMix64 seeding, 32-bit operations only) and `WhiteNoise` (a `BlockProcessor`, sample-major so chunking doesn't change output). Bit-identical across platforms, pinned by known-answer values cross-checked against a Python port. Spectral flatness is tested once the FFT harness exists. Doc: [white noise](docs/dsp/white-noise.md).*
- [x] Oversampling building blocks (2× and 4× up/down) with measured passband and stopband. *`Oversampler` with polyphase IIR elliptic halfbands (stage 1: 8 coefficients, stage 2: 4), from `tools/design/halfband_iir.py`. Measured at a 44.1 kHz base: round-trip ripple 0–20 kHz under 1.6e-6 dB; image and alias rejection at least 104.2 dB and 106.0 dB; latency 3.18 (2×) / 4.30 (4×) samples. Doc: [oversampling](docs/dsp/oversampling.md).*
- [x] **Test harness:** *in `tests/support/` (`anoesis_test_support`, with its own self-test executable). Doc: [how we measure DSP quality](docs/dsp/measuring-dsp.md).*
  - signal generators *(`sine`, `impulse`, `constant`, `coherentFrequency`)*
  - FFT analysis (D5) *(radix-2 in double; rectangular, Hann, Blackman–Harris, and Kaiser windows; power spectra normalised so lobe sums give tone power; Welch averaging)*
  - helpers that measure THD, aliasing, and frequency response *(each checked against a known answer: a naive saw's worst alias measures −20.00 dB at 22,980 Hz, exactly its folded 10th harmonic)*
  - golden-file comparison for offline renders *(32-bit float WAV in `tests/golden/`, `ANOESIS_UPDATE_GOLDEN=1` to record, actual output saved on mismatch. First golden: white noise, bit-exact.)*
- [x] **Benchmark harness** (Catch2 `BENCHMARK`). Record CPU baselines from now on for Phase 3. *`[!benchmark]` test cases (hidden from ctest, enforced by a CTest check) and `tools/bench.sh`, which prints a Markdown table. First baseline: [docs/performance/baselines.md](docs/performance/baselines.md). Docs: [benchmarking](docs/performance/benchmarking.md).*

**Docs:** the processor lifecycle; smoothing and why zipper noise happens; denormals; oversampling theory; how we measure DSP quality.

### M1.1 Oscillators (inventory §2)
- [x] Prototype and measure the anti-aliasing approaches (D6). Choose one and record the decision. *Five prototypes (PolyBLEP 2- and 4-point, minBLEP 32 and 64 taps, octave-mipmapped wavetables), each a saw, pulse, and triangle, measured with the harness: aliasing over MIDI 0–127 at 44.1 and 48 kHz with and without 2×/4× oversampling, droop, a ±25% vibrato, 0.01 Hz to Nyquist, DC, CPU, and memory. Chosen: **minBLEP, 64 taps**, with a worst alias of −91 dB below 20 kHz at 44.1 kHz with no oversampling, a flat passband, and 2.9 ns per oscillator-sample at normal pitches. Code: `MinBlepOscillator` and `MinBlepTable` in [libs/dsp/.../oscillators/](libs/dsp/include/anoesis/dsp/oscillators/), with a fixed-point phase. PolyBLEP is kept as a test-only reference. The wavetable prototype is deleted, and can be rerun from commit `984d868`. Doc: [band-limited oscillators](docs/dsp/band-limited-oscillators.md). CPU: [baselines](docs/performance/baselines.md).*
- [x] 8 band-limited waveforms. The names are unspecified; identify them from the demo or video, then implement. *Names from third-party sources (the manual names none): Saw, Pulse, HPulse, QPulse, Triangle, Sine, Noise, TriSaw. Shapes are working hypotheses, to be measured against the demo in M1.13 (inventory §2, W1–W9): Pulse 50%, HPulse 25%, QPulse 12.5%, all ±1, and TriSaw = (triangle + saw)/2. Waveforms are data: a `WaveShape` of corners (each with a jump and a bend) plus a sine level, in a `constexpr` table, so a new waveform is a table row, not a change to the per-sample loop. minBLEP and the PolyBLEP reference both read it. The minBLEP sine is evaluated at the exact delayed phase, from a short phase history. Noise stays in `WhiteNoise`, alongside the oscillator. No pulse-width parameter. Measured at 64 taps with no oversampling: worst alias below 20 kHz −84.5 dB (QPulse) to −94.1 dB (triangle) at 44.1 kHz, sine at float rounding; no droop; each waveform at its ideal DC; peak up to 2.04 (HPulse at 10 kHz). A one-compare fast path keeps minBLEP within noise of D6's hard-coded switch at ordinary pitches; the new worst case is the TriSaw at 12 kHz, at 32.6 ns per sample. Doc: [oscillator waveforms](docs/dsp/oscillator-waveforms.md). CPU: [baselines](docs/performance/baselines.md).*
- [x] Octave, note, and fine tuning (±1 semitone). Volume, pan, invert. *Ranges and laws gathered from the manual and third-party sources, with their confidence and the demo measurement that settles each, in inventory §2 (O1–O9): Octave −3..3 and Note −7..7 in whole steps, Fine ±1 semitone, linear. Working hypotheses: Volume a linear gain (knob / 10), Pan equal-power sin/cos (−3 dB per side at the centre), Inv an exact negation. The DSP takes physical units (semitones, a linear gain, a position in [−1, 1]); the knob mappings belong to the parameter table (M1.9). New: `Tuning` (pitch as one fractional note), `panGains` (equal-power, linear, and balance laws), `StereoGain` (volume, pan, and polarity folded into one gain per channel, each ramped by a 10 ms `LinearSmoother`, so toggling Inv crossfades instead of clicking), and `OscillatorSlot` (one minBLEP oscillator with its tuning, master tune, and `StereoGain`, adding into a stereo voice bus; the unit unison will extend). Measured over every note, octave, note offset, and fine step at 44.1 and 48 kHz with three master tunings: the pitch is within 0.0013 cents of the target at 20 Hz and above, and within the fixed-point floor below (0.030 cents at 0.64 Hz); the pan law and the volume taper are exact to 5 and 7 decimals; ramps last exactly 10 ms. The two-saw PWM trick gives a DC-free pulse whose width is the phase offset (tested). CPU: the slot adds 0.2–0.3 ns per sample to the bare oscillator at ordinary pitches (3.4 against 3.1 ns for a saw), and is cheaper than the same stereo gain written inline, because its settled gain loop vectorizes. A gain ramp adds 1.3–1.7 ns for its 10 ms, and a pitch per sample about 6 ns (the `exp2`). The oscillator rows are unchanged against the parent commit, measured in the same session. The harness gained `measureFrequency` (a least-squares fit to zero crossings, 10⁻⁹ relative). Doc: [oscillator controls](docs/dsp/oscillator-controls.md). CPU: [baselines](docs/performance/baselines.md).*
- [x] Retrigger and phase (0–360°). Free-running when retrigger is off. *Phase 0 is the same point for every waveform (W6), and Phase is degrees in the slot (`phaseFromDegrees`, wrapped, exact for 360/2^k) and fixed point in the oscillator. `MinBlepOscillator` gained `restart(phase)` (the band-limited onset from silence, at any phase) and `jumpTo(phase, since)`: a hard-sync-style jump at a sub-sample time, which adds the jump in value and in slope as step and ramp residuals and keeps the residuals in flight, so a retrigger never drops a sounding oscillator to silence. The API is per oscillator, for unison to reuse unchanged; the per-sample fast path is untouched. The sine needed its own treatment: it is delayed by evaluating it late, so its history keeps the true trajectory across the jump, and its residuals are offset for the taps before the delay. A first version that treated it like a straight segment peaked at 21 under random jumps; now the worst peak of any waveform is 2.61. `OscillatorSlot` gained Phase, Retrig, `noteOn()` (Retrig on: onset or jump to Phase; off: a sounding oscillator runs on, one starting from silence takes a seeded random phase, hypothesis O10), and `stop()` for a finished voice. Measured as hard sync, 64 taps: the jump aliases 78–87 dB less than a naive phase jump for every piecewise-linear waveform, and 27–76 dB less than restarting from silence. Its worst alias below 20 kHz is −91.7 dB with the slave at 6.5 kHz at 44.1 kHz. The sine is limited by its uncorrected 2nd derivative at high slave pitches (−51 dB at 6.5 kHz), a Part 2 item for sine hard sync. Without the bend correction the triangle measures −44.5 dB instead of −122.5. Tested: the first cycle after a note-on at 0/90/180/270° for every waveform, from silence and while sounding; a jump to the current phase changes nothing; a jump is exactly a natural edge of the same size; the two-saw PWM trick through Phase 90; free-run continuity; sample-accurate note-ons; the lifecycle contract with note-ons. CPU: a jump costs 8–42 ns, about its residual adds; one per note is not measurable; the oscillator rows are within 0–3% of the parent at 440 Hz and 2–6% slower at 12 kHz. Inventory: new Phase and Retrig section, O10–O12. Doc: [oscillator phase](docs/dsp/oscillator-phase.md). CPU: [baselines](docs/performance/baselines.md).*
- [x] Unison: 0–8 voices, detune spread, stereo spread. Off at 0 voices. The oscillator slot built here also picks between the periodic oscillator and `WhiteNoise` for the Noise waveform (one noise source per slot; see [oscillator waveforms](docs/dsp/oscillator-waveforms.md)). *Evidence gathered and rated in inventory §2 (new Unison section, O13–O16; O8 settled as a hypothesis): one KVR poster's measurements of the demo (detune linear in semitones, about 3.32 · x², a maximum that falls with the voice count, the stereo orders, equal amplitude), and two users' report that in-phase voices only add level. Built as working hypotheses, chosen by the project owner: detune **linear in semitones, evenly spaced**, the DSP taking the outermost offset in semitones (the knob law 3.3219 · x² · g(N), g 1 → 0.66 from 2 to 8 voices, goes in the parameter table); stereo sides from the **reported orders**, read by pitch so each ±d pair is split left and right; position = Pan + width × side, **clipped** (O8); **equal power** per voice; **no level normalisation**; Retrig restarts every voice at Phase, and free-running voices draw independent seeded phases. Noise: one `WhiteNoise` per slot, two draws per sample mixed by the symmetric square root of the voices' covariance, so Voices and Stereo give it N voices' power and width. New: `unison.hpp` (the layouts as a `constexpr` table, `unisonPosition`, `unisonNoiseMix`); `OscillatorSlot` with fixed arrays for 8 voices (an oscillator, a `StereoGain`, a detune ratio, and an off/on/fading state each), `setVoices`, `setDetune`, `setStereo`. A voice-count change while sounding fades voices in and out over the 10 ms gain ramp, and the moment a fade ends is exact, so output stays independent of the block size. Measured: every voice within 0.00014 cents of its target; stereo gains exact to 3·10⁻⁶; in phase exactly +6.02 dB per doubling, free-running +9.15 dB at 8 voices; 8 in-phase saws panned hard peak at 11.4 (+21 dBFS; gain staging is M1.4's); the 8-voice stack adds no aliasing of its own (within the coherent sum of its voices at every bin; at most +5.7 dB over the worst single voice, where two voices' aliases share a bin); noise power and L/R correlation within 0.3% and 0.002 of N voices'. No allocation in any control, note-on, or process call, proved by a new `AllocationCounter` that replaces `operator new` in the DSP tests. CPU: 3.1–3.7 ns per voice at 440 Hz, 13–15 at 12 kHz; a 32-voice note 102–105 ns per sample (0.5% of a core); 0 voices 0.13 ns. The oscillator rows are within noise of the parent, measured in the same session; a pitch per sample is 21–26% faster; a one-voice slot is 4% slower settled and 13% while ramping (standalone probe). Phase 3 constraints for SIMD across voices are recorded in the doc. Doc: [oscillator unison](docs/dsp/oscillator-unison.md). CPU: [baselines](docs/performance/baselines.md).*
- [x] Stable from 0.01 Hz up to Nyquist (tested). *Above Nyquist, measured and decided first (commit 9fef36a): the old clamp to a Nyquist increment played a stationary fs/2 tone (exactly dc + a·(−1)ⁿ) at up to −3.9 dBFS for the square and full scale for the sine, its level set by the phase at the crossing, where a band-limited waveform has only its mean. Of five options measured, the project owner chose a **fade about the mean** from 0.45 fs (Decisions log). Built: `nyquistFadeGain()` (a raised cosine of the increment), `WaveShape::mean()`, and the fade in `MinBlepOscillator` on one predictable branch, with the mean switched on by the onset's band-limited step so an onset still starts at 0 (an existing test caught a first version that did not). Above Nyquist every waveform now plays exactly its mean, bit for bit; a sweep and a 13 Hz vibrato across Nyquist put −74 to −107 dB into 10 Hz–18 kHz; a voice above Nyquist adds nothing to a driven saturator downstream (the clamp added −25 to −65 dB). **Found and fixed:** a pitch step clicked on the sloped waveforms by slope change × 2.79 samples (minBLEP's slope × delay compensation jumped with it; 5.4 on a triangle stepping from 0.01 Hz to 0.42 fs, 0.23 for a legato 1 kHz → 2 kHz). A change of 6.1·10⁻⁵ cycles or more is now a band-limited bend, added on the next sample's segment so a jump between the same two samples cannot misplace it (a first version did, peaking at 9.0); smaller, smooth changes are left as they were (threshold chosen by the project owner for CPU). `jumpTo()` from silence now advances its phase at the first sample's increment, which `OscillatorSlot::noteOn(since)` (new: note-ons at a sub-sample time) needs. The range: the slot reaches 0.01 Hz and below through its note input (note −115.79), so no new input. Tested: the fade's law; exactly the mean above Nyquist at 4 rates; the level and mean through the band; sweeps and vibrato; the slot at 0.01 Hz with period and phase over whole cycles (within 0.04 samples at 48 kHz; pitch within the fixed-point floor, measured at 44.1–192 kHz); nothing accumulates (the output after any history equals a restart 72 samples back, bit for bit; the phase wraps exactly); the whole slot under stress at 44.1/48/96/192 kHz in blocks of 1, 37, and 4096 (every waveform with Noise, 0–8 voices changed mid-fade, sub-sample note-ons, Retrig and Inv toggled, per-sample sweeps from 0.01 Hz to past Nyquist, a vibrato across it): finite, under a derived bound of 21.6 (8 × 2.7 per voice; measured 2.33 per voice, 5.5 for the slot), no subnormals in the output or the residuals, bit-identical across block sizes; a pitch step is exactly the table's band-limited bend; either order of a step and a jump is the same. Every new test fails under a deliberate break (13 checked). Long runs (hidden `[range-long-report]`): 12.4 hours at 0.01 Hz, every cycle's DC and RMS within 2·10⁻⁷ of the first; an hour at 20 kHz, no trend. CPU: constant pitch unchanged below 0.45 fs; 25–45% faster at 5–12 kHz (the residual add now vectorizes); a pitch every sample +0.4 ns per voice; a pitch step one residual (about 15 ns). Inventory: O17. Doc: [oscillator range](docs/dsp/oscillator-range.md). CPU: [baselines](docs/performance/baselines.md).*
- [ ] Oscillator drift (v3).

**Docs:** aliasing and band-limited synthesis; the method chosen and the alternatives; unison and detune laws; stereo spreading; the oscillator's range (low-frequency precision, above Nyquist, pitch steps).

### M1.2 Envelopes and LFOs (§4, §6, §8)
- [ ] ADSR with sample-accurate gate. Curve shapes and time ranges come from measurement.
- [ ] LFO: 10 waveforms (names unspecified), rate, gain, bipolar offset, free mode, retrigger on note.
- [ ] Tempo sync: note values including triplets and dotted values, driven by host BPM and transport.

**Docs:** envelope generator design (linear vs exponential segments, retrigger behavior); LFO design; tempo sync math.

### M1.3 Filters (§3)
- [ ] 4-stage filter core with nonlinear saturation per stage (for example a zero-delay-feedback/TPT ladder-style structure).
- [ ] LP, BP, and HP outputs at 12 and 24 dB/oct. Bypass truly skips processing.
- [ ] Cutoff, resonance up to and past self-oscillation, drive.
- [ ] Warm Drive switch: higher-quality saturation versus a cheaper path.
- [ ] Keytracking (0–10; at 10 the filter tracks 1:1 and plays in tune when self-oscillating).
- [ ] Global filter control: shared cutoff, resonance, and keytrack. Decide the combining law from measurement.
- [ ] Stability and aliasing under extreme settings (use oversampling if needed; v3 adds oversampling options).

**Docs:** analog filter theory; ladder topologies; TPT and zero-delay-feedback discretization; nonlinear stages and their stability; keytracking.

### M1.4 Voice and part architecture (§1, §7)
- [ ] Voice = 2 parts × (2 oscillators → filter → amp ADSR). Filter input routing A, B, AB, or none.
- [ ] Polyphony 0–16. Voice allocation and stealing policy. Count active voices for the Voices readout.
- [ ] Mono Legato. Portamento time with Normal and Slide modes. Pitch bend with range. Hold pedal (CC 64).
- [ ] Mixer: Mix A, Mix B, main volume, metering values. No output clipping.
- [ ] Part solo. Automatically skip unused parts and oscillators to save CPU.
- [ ] AmpEnv A/B link option (v3).

**Docs:** voice management and stealing; legato and portamento; per-voice versus global processing.

### M1.5 Modulation system (§6)
- [ ] Source and destination **registries**, extensible for Phase 2.
- [ ] Per-source routing: each of 2 mod envelopes, 2 LFOs, and 4 misc slots drives 2 destinations with bipolar amounts.
- [ ] Misc sources: velocity, aftertouch, keytrack, mod wheel, amp envelopes, mod envelopes, LFOs, arpeggiator step velocity.
- [ ] Recover the full destination list from the plugin, since the manual does not give it.
- [ ] Per-voice versus global evaluation. Decide control rate versus audio rate for each destination, and record the decision.

**Docs:** modulation architecture; polyphonic versus monophonic modulation; control-rate trade-offs.

### M1.6 Arpeggiator and step sequencer (§9)
- [ ] 10 modes. Known: Up, Down, Up/Down variants, Random, Ordered, Step Seq, Step Chord. Identify the rest from the demo.
- [ ] Time (sync), gate, wrap, octave range, swing (v3).
- [ ] Step sequencer: transpose, velocity (rest), and hold per step, with pages.
- [ ] Velocity modes: Key, Hold, Step, Step+Key, Step+Hold. Step velocity output as a mod source.
- [ ] Sample-accurate timing within a block.

**Docs:** arpeggiator design; MIDI event scheduling inside an audio block; swing.

### M1.7 Master effects (§10)
Each effect is a standalone, reusable `libs/dsp` processor. Add them to the fixed chain with bypass switches.
- [ ] Distortion: Overdrive, Foldback, Clip, Decimate, Bitcrush. Amount, dry/wet. 4× oversampling.
- [ ] Phaser: 6 stages, center frequency, spread, LR offset, width, internal LFO, feedback, dry/wet. 2× oversampling.
- [ ] Chorus/Flanger: delay, rate, depth, feedback, dual mode (4 lines in quadrature), width, dry/wet. 2× oversampling.
- [ ] EQ: bass and treble shelves with frequency controls and **automatic volume normalization**.
- [ ] Reverb: size, damp, predelay, width, dry/wet.
- [ ] Delay: L/R times (sync), feedback, ping-pong with spread, width, low and high cut, **smear**, dry/wet.
- [ ] Compressor: ratio 1:1 to 100:1, threshold 0 to −30 dB, attack, release. Stereo-linked.
- [ ] Chain: Distortion → Phaser → Chorus → EQ → Reverb → Delay → Compressor. Disabled effects cost nothing.

**Docs:** one write-up per effect (waveshaping and foldback; allpass phasers; modulated delay lines; shelving EQ; reverb topologies; delay smear via diffusion; compressor detectors and gain computers).

### M1.8 Engine integration
- [ ] A `Synth` engine in `libs/dsp` that combines the voices, modulation, arpeggiator, and effects. Takes MIDI events and parameters in; produces audio out.
- [ ] Sample-accurate parameter automation (v3).
- [ ] Global oversampling options for real-time and render (v3).
- [ ] An offline render tool and **golden-render regression tests** for reference patches.
- [ ] CPU benchmarks for representative patches, recorded as Phase 3 baselines.

**Docs:** engine architecture overview, with a diagram of the signal and control flow.

### M1.9 Interop library (`libs/interop`)
- [ ] Parameter definition and binding layer that is generic (knows nothing about the synth) and fed by a parameter table in `plugin/`.
- [ ] Host tempo and transport bridge.
- [ ] MIDI learn and forget, persisted globally. Default CC map. MIDI learn for program and bank selection and for pitch bend (v3).
- [ ] Parameter and part **locking** across preset changes (v3).

**Docs:** parameter systems and host automation; MIDI learn design.

### M1.10 Presets and banks
- [ ] Our own versioned format (D7). Load and save single presets and 512-preset banks (4 × 128).
- [ ] Copy, paste, insert, delete, clear (init), randomize, reset, rename. Copy to Part A/B (v3).
- [ ] Program change selects within the first 128.
- [ ] A default init patch. Factory content must be original.

**Docs:** preset format and versioning or migration strategy.

### M1.11 GUI library (`libs/gui`) and plugin UI
- [ ] Reusable components: knob, slider, stepper or selector, toggle, LCD-style panel, VU meter, keyboard, pitch and mod wheels.
- [ ] Interaction: vertical drag, Shift for fine, Alt for rotary, double-click or Ctrl-click to reset, click shows the value without changing it.
- [ ] Resizable, high-DPI-aware layout. Original visual design (D9).
- [ ] Synth layout in `plugin/` that composes the components: part select, solo, sync, polyphony and voices display, effect and arpeggiator panels.

**Docs:** GUI component architecture; separating the UI from the DSP thread (lock-free communication).

### M1.12 Plugin target and validation
- [ ] VST3, AU, and Standalone build on every platform (AU on macOS only).
- [~] pluginval in CI, plus `auval` on macOS. Both were done in Phase 0, as CTest tests. Still to do: a manual smoke test in at least one DAW per platform.
- [ ] Steinberg's VST3 Plugin Validator, run from pluginval. Build the `validator` tool from the VST3 SDK, pinned to a release tag; JUCE bundles only part of the SDK, without the validator. Then pass it to pluginval (`--vst3validator <path>`; confirm that pluginval v1.0.4 supports this flag) so the `pluginval:VST3` CTest test runs it on every platform, locally and in CI. Record the SDK's license and how it is fetched and cached. Update docs/development/plugin-validation.md.
- [ ] State save and restore round-trips.

### M1.13 Parity verification and sign-off
- [ ] Resolve every **Unspecified** item in the inventory, using measurements of the demo. Record the method and result.
- [ ] A/B listening and measurement against the demo for each component. Rebuild the tutorial patches listed in CLAUDE.md resources.
- [ ] Every inventory item is ticked or explicitly dropped with a reason.

**Docs:** black-box measurement methods (how each unknown was characterized).

---

## Phase 2: Extensions (backlog; prioritize after M1.13)

Candidates. Choose and order them once Phase 1 is complete.

- **Waveforms:** add waveforms like those in the Korg DW8000 and Ensoniq ESQ1, and different types of noise.
- **Filters:** multimode SVF (with notch and peak), diode ladder, Sallen-Key/MS-20 style, comb, formant/vowel; **serial and parallel filter routing** (a known Sylenth1 gap).
- **Synthesis:** hard sync, ring mod, FM/PM between oscillators, true PWM, sub-oscillator, wavetable oscillator, sample oscillator, additive.
- **Modulation:** full mod matrix, more envelopes and LFOs, MSEG or drawable envelopes, MPE support.
- **Effects:** more distortion models, multiband compressor, limiter, algorithmic reverb variants (plate, shimmer), filter effects, reorderable effects chain.

**Docs:** one write-up per addition, same structure as Phase 1.

---

## Phase 3: Optimization

The baselines come from M1.0 and M1.8 benchmarks.

- [ ] Profile representative patches on each platform. Rank the hotspots.
- [ ] First measured candidate (M1.0 baseline): `ExponentialSmoother::fill` is 4.7× slower than `LinearSmoother`, likely from the output span aliasing its `float` members. Measure, fix (local copies of the state), and measure again.
- [ ] Measure the denormal penalty on x86 (none on Apple Silicon: [baselines](docs/performance/baselines.md)).
- [ ] SIMD: vectorize across unison voices first (the manual suggests Sylenth1 does this), then across polyphonic voices. Keep the scalar fallbacks.
- [ ] Memory layout (SoA), block-based processing, control-rate decimation of modulation.
- [ ] Cheaper nonlinearities and oversampling where it can't be heard (prove it with measurement tests).
- [ ] **STM32 feasibility:** cross-compile `libs/dsp` for Cortex-M (for example M7 with FPU). Measure the voice budget and identify blockers.

**Docs:** profiling methodology; SIMD techniques; before and after numbers for each optimization.

---

## Phase 4: Documentation (continuous)

Each milestone above lists its docs. In addition:

- [ ] `docs/README.md`: an index and reading order (theory first, then the codebase).
- [ ] Architecture overview and dependency diagram.
- [ ] Glossary of synth and DSP terms.
- [ ] Contributor guide: TDD workflow, adding a filter, effect, waveform, or mod source through the registries.

---

## Decisions log

| Date | Decision | Notes |
|---|---|---|
| 2026-11-13 | Build: CMake + Ninja. Compilers: Apple Clang (macOS), GCC (Linux), MSVC (Windows). | CLAUDE.md |
| 2026-11-13 | C++ standard: the highest supported by JUCE and Catch2 on all target compilers. | Confirm the exact version in M0.3. |
| 2026-11-13 | JUCE and Catch2 v3 as git submodules. TDD is required. | |
| 2026-11-13 | Formats: VST3, AU, Standalone. Platforms: macOS, Linux, Windows; STM32 possible later. | |
| 2026-11-13 | `libs/dsp` has no JUCE dependency and is portable to bare metal. `libs/gui` and `libs/interop` know nothing about this synth. | |
| 2026-11-13 | `libs/dsp` is MIT-licensed. | D1 (partial). The license for code that uses JUCE is still open. |
| 2026-11-13 | Product: **Brutalis** by Anoesis Audio. The manufacturer code, plugin code, and bundle ID are chosen and kept private. | D2. No product named Brutalis was found online. The codes were checked for clashes with installed AUs. |
| 2026-11-13 | The private identifiers are kept out of the repo and injected at build time from a gitignored `cmake/Identity.local.cmake`, or from CI secrets. The repo holds only the placeholders in `cmake/Identity.example.cmake`. | The codes still become visible in shipped binaries. This keeps them out of the repo and its history only. |
| 2026-11-13 | JUCE 9, pinned to release tag 9.0.5 (the latest 9.x on 2026-11-13). Catch2 pinned to v3.16.1. | D3 |
| 2026-11-13 | D1 (first deferred, then decided the same day): **all project code is MIT**. Root `LICENSE`; `libs/dsp` keeps its own copy. | JUCE stays AGPLv3 or commercial. Distributing plugin binaries (which contain JUCE) also requires either AGPLv3 compliance for the combined work or a commercial JUCE license. The MIT code itself is unaffected. |
| 2026-11-13 | CI policy: test locally on macOS before every commit (`tools/check.sh`). GitHub Actions runs only for major and minor versions (`vX.Y.0` tags, or manual dispatch), as a fail-fast chain: macOS, then Linux and Windows. The identity secrets are set in the repository. | Decided by the project owner to save hosted CI time. See docs/development/ci.md. |
| 2026-11-13 | D4: GitHub Actions. | The MSVC environment is set up with `vswhere` + `vcvars64.bat` rather than a third-party action. |
| 2026-11-13 | D8: macOS 12.0 (universal arm64 + x86_64 in release), Windows 10 x64, Ubuntu 22.04 (built with GCC 12). | When Linux builds return, build on 22.04 with its own GCC 12 so the binaries match that libstdc++. |
| 2026-11-13 | C++23 confirmed. JUCE 9.0.5 and Catch2 v3.16.1 build cleanly as C++23 under `-Wall -Wextra -Wpedantic -Werror` plus JUCE's recommended warnings (Apple Clang 21). | GCC 12 and MSVC still to be confirmed by CI. |
| 2026-11-13 | `libs/gui` and `libs/interop` are **JUCE modules** (`juce_add_module`), not static libraries. | The plugin target always compiles JUCE, so a static lib linking JUCE would put a second copy in the binary (duplicate symbols or ODR violations). See docs/development/project-structure.md. |
| 2026-11-13 | Namespaces: `anoesis::dsp`, `anoesis::gui`, `anoesis::interop`, `brutalis`. CMake targets: `anoesis_dsp`, `anoesis_gui`, `anoesis_interop`, `brutalis`. | The reusable libs carry the company name because they are meant for reuse beyond this product. |
| 2026-11-13 | Plugin tests compile the plugin's own sources (the `brutalis_plugin_common` INTERFACE target) into a JUCE console app, rather than linking the plugin's shared-code library. | The tests exercise the shipped code without the format wrappers. |
| 2026-11-13 | Linux and Windows: no local builds. Every major or minor version is tested on both in Version CI, after macOS passes. Release artifacts for them are deferred. | Decided by the project owner (this refines an earlier same-day deferral of Linux and Windows builds). See **Linux and Windows**. |
| 2026-11-13 | pluginval v1.0.4 (latest stable) added as a CTest test at strictness 10 on the VST3. Downloaded at configure time with pinned SHA-256 hashes and cached in `.cache/`. Skipped in sanitizer builds. | Running as a CTest test means `tools/check.sh` and every CI test leg run it with no extra wiring. Verified both ways: the plugin passes, and NaN output fails. See docs/development/plugin-validation.md. |
| 2026-11-13 | auval added as a CTest test (`auval -strict`, macOS, skipped under ASan). [tools/auval-test.sh](tools/auval-test.sh) installs the AU into `~/Library/Audio/Plug-Ins/Components` for the duration of the test. It always removes it afterwards and restores any previously installed copy. The codes go in through environment variables and are masked in the output. | auval only sees installed AUs, and installing per-user avoids `sudo`. Verified: the AU passes, wrong codes fail, a pre-existing component is restored, and the codes are absent from the output. |
| 2026-11-13 | D5: the test FFT is a **hand-written radix-2 FFT in double precision**, in `tests/support/` only. | Chosen by the project owner over vendoring pocketfft or KissFFT (both BSD-3-Clause). No new dependency, MIT throughout. Power-of-2 sizes are enough because the harness picks its own analysis lengths. Double-precision round-off (about 1e-15·log₂N) is far below float's noise floor, so the FFT never limits a measurement. It is checked against a naive DFT. |
| 2026-11-13 | The processor lifecycle is expressed as C++20 **concepts** (`Preparable`, `BlockProcessor`) over a non-owning `AudioBlock` view, with a float sample rate in `ProcessSpec`. | Zero-cost and inlinable in voice loops. Run-time polymorphism for Phase 2 registries will wrap conforming processors in a thin virtual interface, dispatched once per block. See docs/dsp/processor-lifecycle.md. |
| 2026-11-13 | Clang and GCC builds of project code add `-Wfloat-equal -Wconversion -Wsign-conversion -Wshadow`, and `libs/dsp` also gets `-Wdouble-promotion`. | JUCE targets already had these through JUCE's recommended flags. The JUCE-free `libs/dsp` and its tests did not, although CLAUDE.md relies on `-Wfloat-equal` being on. |
| 2026-11-13 | Master tune is a reference frequency for A4 (`noteToFrequency(note, referenceHz)`), with `referenceFromCents()` for a cents-based control. | Sylenth1's control units are unspecified; both forms reduce to a reference in Hz. The UI law is settled in M1.13 by measuring the demo. |
| 2026-11-13 | `fastExp2` and `fastTanh` share one degree-5 minimax polynomial for 2^f − 1 with q(0) = 0 and q(1) = 1 built in (exact at integers, continuous). tanh is computed as m/(m + 2) with m = e^{2x} − 1, so it has no cancellation near zero; it saturates to exactly ±1 at \|x\| ≥ 8.6. | Designed by `tools/design/exp2_polynomial.py` (Remez, pure Python). See docs/dsp/fast-math.md. |
| 2026-11-13 | Smoothers land exactly on the target after a fixed ramp length, for both shapes. The exponential shape decays to 10⁻⁴ (−80 dB) of the step over the ramp time, then snaps. Linear values are computed as start + step·k, not accumulated. | `isSmoothing()` becomes exact, so processors can switch to constant-parameter paths. No float stall, no denormal tail. See docs/dsp/parameter-smoothing.md. |
| 2026-11-13 | Denormal strategy: an FTZ/DAZ RAII guard (`ScopedFlushDenormals`) at the top of every audio callback, plus `flushDenormal()` on recursive state. No offset or noise injection, no `-ffast-math`. | The guard costs nothing per sample. The explicit flush covers targets without FTZ control and code outside a guard. Injection would add signal that tests must tolerate. See docs/dsp/denormals.md. |
| 2026-11-13 | Random numbers: xoshiro128+ seeded through SplitMix64, with floats from the top 24 bits mapped to exact, symmetric, zero-mean values. `<random>` is not used. | Bit-identical on every platform (`<random>`'s float distributions are implementation-defined). 32-bit only, so cheap on Cortex-M. See docs/dsp/white-noise.md. |
| 2026-11-13 | Golden files are 32-bit float WAVs in `tests/golden/`, compared per sample with an explicit tolerance (0 for integer-exact output). They are recorded with `ANOESIS_UPDATE_GOLDEN=1`, and failed renders go to `build/<preset>/test-output/`. | Inspectable in any audio tool. The tolerance absorbs FMA-contraction differences between compilers. See docs/dsp/measuring-dsp.md. |
| 2026-11-13 | Oversampling uses **polyphase IIR elliptic halfbands**, not linear-phase FIR. Spec at a 44.1 kHz base: pass 0–20 kHz, reject ≥ 100 dB from 24.1 kHz. 4× is two cascaded 2× stages. | About 12 first-order sections instead of 70+ FIR taps. Latency is 3–4 samples rather than about 35, so the reported latency barely changes when oversampling is toggled. Ripple is about 1e-10 dB. The phase is non-linear only near 20 kHz, and dry/wet mixing happens inside the oversampled domain. See docs/dsp/oversampling.md. |
| 2026-11-13 | Benchmarks are Catch2 `BENCHMARK`s in the test executables, tagged `[!benchmark]` (hidden from ctest, enforced by a CTest check), run with `tools/bench.sh` in `relwithdebinfo`. Results are recorded by hand in `docs/performance/baselines.md` with the machine and commit. Not run in CI. | No new dependency. Shared CI runners are too noisy for CPU numbers. See docs/performance/benchmarking.md. |
| 2026-11-13 | D6: oscillators are band-limited with **minBLEP/minBLAMP, 64 taps** (a Kaiser-windowed sinc, β 9.3, cut off at Nyquist, made minimum-phase by the real cepstrum, tabulated at 64 phases per sample). Phase is a 32-bit fixed-point accumulator. | Chosen by the project owner from measurements of five prototypes. At 44.1 kHz with no oversampling: worst alias below 20 kHz −91.0 dB over MIDI 0–127 (−102.5 dB below MIDI 104), against −14.1/−22.3 dB (PolyBLEP 2-/4-point) and −68.6 dB (wavetables, at bass notes); a passband flat to 20 kHz; −124 dB under a ±25% vibrato. Cost: 2.9 ns per oscillator-sample at normal pitches (as cheap as PolyBLEP there), rising to 22 ns for a 12 kHz square; 304 B per oscillator plus 33 KB of shared tables. Taps stay a table parameter (32 taps: −66.5 dB, 176 B, 16.6 KB), so a cheaper real-time mode remains possible. PolyBLEP is kept as a test-only reference. The wavetable prototype is removed in the commit after the one that measured it (see the doc). See docs/dsp/band-limited-oscillators.md. |
| 2026-11-13 | The 8 oscillator waveforms are Saw, Pulse, HPulse, QPulse, Triangle, Sine, Noise, and TriSaw (from third-party sources; the manual names none). They are built on working hypotheses: Pulse 50%, HPulse 25%, QPulse 12.5%, all ±1; TriSaw = (triangle + saw)/2; equal peak level; each starting at phase 0 as in inventory W6. | Chosen by the project owner, to be measured against the demo in M1.13 (inventory §2, W1–W9). Each shape is one row of data in `waveShapes`, so a measurement that disagrees changes a row and its expected test values. |
| 2026-11-13 | Waveforms are data: a `WaveShape` (corners with their jumps and bends, plus a sine level) in a `constexpr` table indexed by `Waveform`. The minBLEP loop and the PolyBLEP reference iterate over the corners. No pulse-width parameter: `setPulseWidth` and `widthToPhase` are removed. | Adding a waveform is a table row, not a change to the per-sample loop. Part 2 can pass shapes built at run time through `setShape()` (true PWM, for example). See docs/dsp/oscillator-waveforms.md. |
| 2026-11-13 | Noise lives alongside the oscillator, in `WhiteNoise`, not inside it. `MinBlepOscillator` treats `Waveform::noise` as silence, and `isPeriodic()` is false for it. The oscillator slot built with unison chooses between them. | Noise has no phase, pitch, edges, delay, or onset. Putting it in the oscillator would add generator state and a per-sample branch to every unison voice for one waveform, and a slot can run one noise source instead of 8 detuned copies. |
| 2026-11-13 | The minBLEP sine is evaluated at the exact delayed phase, interpolated from an 8-entry phase history, rather than delay-compensated by slope. Its onset after a reset band-limits the step only. | Slope × delay compensation is exact only for straight lines. On a sine it would scale the amplitude by √(1 + (ωτ)²): ×2 at 5 kHz. The history keeps the sine on the edges' phase trajectory under per-sample pitch changes. |
| 2026-11-13 | Oscillator control ranges and laws: Octave −3..3 and Note −7..7 in whole steps, Fine ±1 semitone linear in cents (from MusicRadar and the manual, medium confidence). Working hypotheses, to be measured against the demo in M1.13: Volume a **linear** gain (knob / 10, so 10 is unity); Pan **equal-power sin/cos**, −3 dB per side at the centre, unity at the hard sides (knob / 5 − 1); Inv an exact negation (manual, high confidence). | Chosen by the project owner from the recommendations. No source gives the volume taper or the pan law. The only hint for the taper, v1.20's "Knob behavior changed to linear", is ambiguous. Sources, confidence, and the measurement that settles each are in inventory §2 (O1–O9). |
| 2026-11-13 | The controls live in an **`OscillatorSlot`**, not in `MinBlepOscillator` (which stays a mono, single-voice source) and not in the voice. The DSP takes physical units (semitones, a linear gain, a position in [−1, 1], a bool); Sylenth1's ranges and knob laws go in the parameter table (M1.9). Pitch is one fractional note (key + 12 · octave + note + fine, and later bend, glide, modulation, detune, drift), converted once by `noteToFrequency` at the master tune, per block or per sample. | Unison will turn the slot into N oscillators plus a `WhiteNoise`, each voice with its own stereo gains; gain and pan inside the oscillator would repeat that work 8 times. FM is a per-sample input of the same shape as the notes, and hard sync a list of sync events, so neither is boxed in. A measured open question (O1–O6) changes a mapping, not the library. See docs/dsp/oscillator-controls.md. |
| 2026-11-13 | Smoothing: `StereoGain` folds volume, pan, and polarity into one gain per channel and ramps each with a **`LinearSmoother`, 10 ms**. Invert is the gain's sign, so toggling it crossfades through zero; settled, the output is the exact negation. Pitch is not smoothed. | Linear lands exactly (so the settled loop can be used, and it vectorizes) and costs 0.80 ns per sample against the exponential's 3.72. Smoothing the gains rather than the pan position keeps `sin` out of the per-sample path; the cost is a chord of the pan curve mid-ramp (at worst −3 dB at the midpoint of a full left-to-right swing). The oscillator's phase is continuous, so a pitch step does not click, and a MIDI-CC step of Fine (1.6 cents) is below the 5-cent threshold. |
| 2026-11-13 | The test harness measures frequency with `measureFrequency`: upward zero crossings, interpolated linearly, with the period taken as the least-squares slope of crossing time against count. | Averaging every crossing's interpolation bias resolves 10⁻⁹ relative over 2 s, which an FFT peak would need long interpolation to reach. Used to check that the oscillator plays its increment's frequency. See docs/dsp/measuring-dsp.md. |
| 2026-11-13 | **No downloadable builds on GitHub.** The release job's `actions/upload-artifact` step was removed, and `tools/check-no-published-builds.sh` fails on any workflow step that would publish files. | Decided by the project owner. The repo is public; binaries contain JUCE and the real identity, and distribution is decided separately. See **Distribution**. |
| 2026-11-13 | The white-noise known-answer values are reproduced by an independent Python port, [tools/reference/xoshiro128plus.py](tools/reference/xoshiro128plus.py), written from the published reference C code rather than from `random.hpp`. | Expected values copied from the implementation's own output would pin its bugs too. See docs/dsp/white-noise.md. |
| 2026-11-13 | clang-format and clang-tidy pinned to major version 23. CI installs `clang-format==23.1.5` and `clang-tidy==23.1.4` from PyPI. | Different majors format the same code differently. |
| 2026-11-13 | On GCC, JUCE's own compilation units (`external/JUCE/modules/*/*.cpp` and `*.c`) are compiled with `-Wno-conversion`, and JUCE's include directories are `SYSTEM` (so GCC skips warnings located in JUCE headers). Every other warning still applies to JUCE's units, and project code keeps `-Wconversion` on every compiler. Apple Clang still holds JUCE's units and headers to the full set. | The first version run showed that JUCE 9.0.5 isn't `-Wconversion`-clean on GCC 12. JUCE's GCC flag set omits it, unlike its Clang set, so the earlier row's "JUCE targets already had these" held for Clang only. The exemption is kept to JUCE's units, as code-quality.md requires. |
| 2026-11-13 | Version CI runs as one chain, macOS → Linux → Windows → macOS release, and a manual dispatch can switch each job (and the ASan legs) on or off; a tag push runs everything. Sanitizer builds leave the plugin's shared code and format wrappers out of the default build (327 → 249 compiles on macOS). | Requested by the project owner. Debug and asan share no object files, so the saving comes from not building what ASan can't use: no host loads an ASan plugin, and `brutalis_plugin_tests` already compiles the plugin's sources under the sanitizers. Both Linux legs are kept: debug runs pluginval on the GCC build, asan adds LeakSanitizer and GCC's sanitizers. See docs/development/ci.md. |
| 2026-11-13 | Version CI's chain order is now Linux → Windows → macOS → macOS release (it was macOS → Linux → Windows → release). | Requested by the project owner. macOS already passes locally before every commit, so the platforms that are never built locally now run first, and a GCC or MSVC failure ends the run before any macOS minutes are spent. See docs/development/ci.md. |
| 2026-11-14 | Phase and Retrig: with Retrig on, a note-on restarts at Phase, as the band-limited onset from silence or, if the oscillator is sounding, as a band-limited phase jump (minBLEP/BLAMP at the note-on's time, keeping the residuals in flight). With Retrig off, a sounding oscillator runs on, and one starting from silence takes a **seeded random phase** from the slot's `Random` (working hypothesis O10). Phase 0 is the same point for every waveform (W6). | A reset to silence cuts edges short and steps to 0: 27–76 dB more aliasing than the jump, measured as hard sync. Continuing from 0 when free-running would start detuned unison voices in step, the opposite of free-running; a random phase is what a never-stopping oscillator looks like at a new note, and the seed keeps renders reproducible. To be checked against the demo in M1.13 (inventory O10–O12). |
| 2026-11-14 | Unison laws (working hypotheses, inventory O8, O13–O16): detune **linear in semitones, evenly spaced**, the DSP taking the outermost voice's offset in semitones; the Detune knob 3.3219 · x² · g(N) semitones in the parameter table, g from 1 at 2 voices to 0.66 at 8, linear in N. Stereo sides from the **reported orders** (C, LR, LCR, LRLR, LRCLR, LRRLLR, LRLCRLR, LRRLRLLR), read from the lowest voice to the highest; position = Pan + width × side with width ∈ [0, 1], **clipped** at the sides (O8: an offset); the **equal-power** law per voice. **No level normalisation**: every voice at full level. Retrig restarts every voice at Phase; free-running voices from silence draw independent seeded phases in voice order. | Chosen by the project owner from the recommendations. The evidence is one KVR poster's measurements of the demo (BlitBit, thread 518035, 2019: linear in semitones, about 3.32 · x², a voice-count-dependent maximum read as about 2.2 semitones at 8 voices, the orders, equal amplitude) and two users' report that in-phase voices only add level (thread 452081, 2015). Each law is one table or function in `unison.hpp`, so a measurement that disagrees changes that, not the slot. Headroom: 8 in-phase voices panned hard peak at 11.4 for a saw (+21 dBFS) and 13.1 for HPulse at C9; gain staging is settled with the mixer (M1.4, O5). See docs/dsp/oscillator-unison.md. |
| 2026-11-14 | Noise with unison: **one `WhiteNoise` per slot**, two draws per sample mixed by the symmetric square root of the voices' 2×2 covariance, so Voices and Stereo give it the power and left/right correlation of N noise voices at the voices' gains. Detune, Phase, and Retrig do not apply; Inv negates exactly. | Chosen by the project owner. Exact in power and correlation for any width and pan, at 2 draws per sample instead of 8. Only the amplitude distribution differs from 8 real sources (trapezoidal against nearly Gaussian), which W7's histogram would show. |
| 2026-11-14 | Unison data layout: fixed arrays for 8 voices in `OscillatorSlot` (a `MinBlepOscillator`, a `StereoGain`, a detune ratio, and an off/on/fading state per voice), voice by voice per 64-sample chunk. Detune is a frequency ratio computed when the detune or count changes. A voice-count change while sounding fades voices in and out over the 10 ms gain ramp. No summed-mono fast path at Stereo 0. | Chosen by the project owner. `restart()` and `jumpTo()` are reused unchanged, nothing is allocated, and a slot is about 5 KB at 64 taps. The mono fast path changed float rounding at a point that depends on the block split, which would break block-size independence. Phase 3 constraints (a shared ring position needs a per-lane restart; per-lane corner masks; lane forms of `untilCorner` and the sine history) are recorded in docs/dsp/oscillator-unison.md. |
| 2026-11-14 | Real-time tests count heap allocations: `AllocationCounter` ([tests/dsp/support/](tests/dsp/support/)) replaces the global `operator new` in `anoesis_dsp_tests` and counts allocations on a thread while a counter is alive. | Structural arguments ("fixed arrays, so it cannot allocate") miss allocations in called code. Compiled into the executable rather than linked from `anoesis_test_support`, so the replacement is always the one used. See docs/dsp/processor-lifecycle.md. |
| 2026-11-14 | Above Nyquist: **fade about the waveform's mean**. A voice's output is mean + g·(y − mean), g a raised cosine of its increment from 1 at 0.45 fs to 0 at Nyquist, and 0 above it, so a voice above Nyquist plays its waveform's mean alone: the band-limited ideal. Replaces the clamp to a Nyquist increment. | Chosen by the project owner from five measured options ([range-report], docs/dsp/oscillator-range.md). The clamp played a stationary fs/2 tone at up to full scale (sine) or −3.9 dBFS (square), at a level set by the phase at the crossing, which a driven saturator at 4× oversampling turned into intermodulation up to 53 dB above the note's own. Muting at once clicked (−44 to −76 dB in band); muting with a 10 ms ramp, or fading the whole output, stepped or wobbled the pulses' DC (−10 to −62 dB in band). The fade about the mean measured −94 to −125 dB in band on sweeps and a vibrato across Nyquist, is a pure function of the pitch (no state), and keeps the pulses' DC at every pitch. 0.45 fs is where the minBLEP kernel's own roll-off begins. |
| 2026-11-14 | Pitch steps are **band-limited**: a change of increment of `bandLimitedPitchStep` (2¹⁸ units, 6.1·10⁻⁵ cycles per sample) or more adds the slope's change as a ramp residual (BLAMP), on the segment of the next sample; smaller changes are left as they are. | Found by the range stress test: a pitch step clicked on the sloped waveforms by slope change × 2.79 samples, because minBLEP's slope × delay compensation jumped with the step (5.4 on a triangle from 0.01 Hz to 0.42 fs). Fixing it now, and the threshold, were chosen by the project owner. A threshold 8 times lower (error ≤ 8.5·10⁻⁵ per unbandlimited change) made a fast per-sample vibrato (±1 semitone at 384 Hz) cost a residual every sample, 4.2 → 17 ns; 2¹⁸ keeps it at 4.3 ns and still makes steps exact, leaving smooth modulation's slow error (≤ 6.8·10⁻⁴ per change; −74 dB below 18 kHz at worst, measured). With it, `jumpTo()` from silence advances by `since` at the first sample's increment, not the increment at the call. See docs/dsp/oscillator-range.md. |
