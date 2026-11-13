# Brutalis (repo: ano-sinth)

A virtual analog synthesizer plugin by **Anoesis Audio**, built with JUCE and modeled on Lennar Digital's Sylenth1.

## Product identity

- **Public:** product name **Brutalis**, company **Anoesis Audio**.
- **Private:** the manufacturer code, plugin code, and bundle ID are **not stored in this repo**.
  - The repo holds only placeholders, in [cmake/Identity.example.cmake](cmake/Identity.example.cmake).
  - Locally, the real values are in `cmake/Identity.local.cmake` (gitignored), which is loaded after the example and overrides it.
  - In CI, they come from secrets passed as `-D` options. Release builds turn on `BRUTALIS_REQUIRE_REAL_IDENTITY` so placeholders can't ship.
  - The real values are recorded in `CLAUDE.local.md` (gitignored).
- **Never write the real codes or bundle ID into any tracked file, commit message, PR, or issue.** Code and CMake refer to them only through the `BRUTALIS_*` variables.
- Once released, the real values must never change, or hosts will treat the plugin as a different product.
- AU rules: codes are 4 characters with at least one uppercase letter (all-lowercase is reserved for Apple). GarageBand wants plugin codes as one uppercase letter followed by lowercase.

## Project phases

The work has four parts. Parts 1 → 2 → 3 run in sequence; Part 4 runs alongside all of them.

1. **Sylenth1 parity.** Build a virtual analog synth that approximates *all* of Sylenth1's features (see the component map below). Design it so Part 2 can add to it: filters, effects, oscillator waveforms, and mod sources and destinations sit behind interfaces or registries rather than hard-coded switches.
2. **Extensions.** Add filter types, effects, and synthesis methods that Sylenth1 does not have.
3. **Optimization.** Profile first, then optimize: SIMD, voice and unison efficiency, denormal handling, and CPU per voice. Start capturing CPU baselines during Part 1 so later gains can be measured.
4. **Documentation.** Write up every task and technique so that a reader can (a) see what was done and why, and (b) learn how the technique works both in general and in this codebase. Update the docs in the same change as the code. Do not leave them for later.

Current phase and task breakdown: see [PLAN.md](PLAN.md).

## Sylenth1 component map (Part 1 target)

Full inventory, checked against manual v2.5 with page references and open questions: [docs/reference/sylenth1-features.md](docs/reference/sylenth1-features.md). Summary:

- **Signal path:** 2 parts (A and B), each with 2 oscillators → filter → amp ADSR. Then the mixer (Mix A, Mix B), the master effects, and Main Volume. Filter input select can route A, B, AB, or nothing into either filter.
- **Oscillators (4):** 8 waveforms, 0–8 unison voices, octave/note/fine, volume, phase, detune, stereo spread, pan, retrigger, invert. Alias-free from 0.01 Hz to Nyquist.
- **Filters (2):** bypass/LP/BP/HP at 12 or 24 dB, cutoff, resonance (can self-oscillate), and drive across 4 saturating stages. The shared Filter Control panel adds cutoff, resonance, keytrack, and a Warm Drive switch.
- **Modulation:** no general mod matrix. 2 mod ADSRs, 2 LFOs (10 waveforms, rate/gain/offset, free-run), and 4 misc source slots. Every source drives 2 destinations, each with a bipolar amount.
- **Voicing:** polyphony 0–16, mono legato, portamento (Normal or Slide), bend range, mod wheel, hold pedal, tempo sync.
- **Arpeggiator:** 10 modes, time, gate, wrap, octave, and a step sequencer (transpose, velocity, and hold per step). It has 5 velocity modes and outputs step velocity as a mod source.
- **Effects, fixed order:** distortion (5 types) → phaser → chorus/flanger → EQ → reverb → delay → compressor.
- **Presets and MIDI:** 4×128-preset banks, init/randomize/reset, default CC map, persistent MIDI learn.
- **Added in v3.x, after the manual:** oversampling options (real-time and render), master tuning, oscillator drift, arpeggiator swing, AmpEnv A/B link, parameter and part locking, Copy to Part A/B, and sample-accurate automation.

## Clean-room constraint

Reproduce Sylenth1's *behavior and feature set*, not its implementation or assets. Do not copy its UI graphics, skins, presets, factory sounds, or branding, and do not decompile or disassemble the binary. Original code only. The plugin's name, UI, and preset format are our own.

## Tech stack

- **Language:** the highest C++ standard that both JUCE and Catch2 support on every target compiler, with compiler extensions off. Currently C++23 (confirmed 2026-11-13 with JUCE 9.0.5 and Catch2 v3.16.1). When either library or any of the target compilers moves up, raise `CMAKE_CXX_STANDARD`.
- **Framework:** [JUCE](https://github.com/juce-framework/JUCE) 9, as a git submodule at `external/JUCE`, pinned to the latest 9.x release tag (9.0.5 as of 2026-11-13). Upgrade deliberately, never by tracking a branch.
- **Testing:** [Catch2](https://github.com/catchorg/Catch2) v3, as a git submodule at `external/Catch2`, pinned to a release tag (v3.16.1 as of 2026-11-13). Tests run through CTest.
- **Licensing:**
  - All project code is **MIT** (root `LICENSE`, decision D1). `libs/dsp` also carries its own copy, so it can be reused on its own. Nothing JUCE-licensed or copyleft may enter `libs/dsp`.
  - Submodules keep their own licenses: JUCE is AGPLv3 or commercial, and Catch2 is BSL-1.0.
  - Plugin *binaries* contain JUCE, so distributing them must also satisfy JUCE: either the AGPLv3 for the combined work (offer the source, add no extra restrictions) or a commercial JUCE license. The MIT code itself stays MIT.
- **Build:** CMake + Ninja on every platform. Use `CMakePresets.json` for the configurations. Development happens on macOS.
- **Compilers:** each platform's standard compiler:
  - macOS: Apple Clang (Xcode command-line tools)
  - Linux: GCC
  - Windows: MSVC (Visual Studio), invoked from a developer environment so Ninja can find `cl.exe`

  Code must build cleanly on all three. Do not rely on extensions or behavior specific to one compiler.
  **Only macOS is built locally.** Linux and Windows are tested in Version CI, after macOS, on every major or minor version. Because GCC and MSVC see the code only at version time, the portability rules matter even more.
- **Plugin formats:** VST3, AU, Standalone.
- **Target platforms:** macOS, Linux, Windows. **STM32 may be added later**, which constrains the DSP library (see below).

## Commands

Presets: `debug`, `relwithdebinfo`, `release` (needs the real identity; universal on macOS), `asan` (not Windows). Details: [docs/development/building.md](docs/development/building.md).

```sh
git submodule update --init --recursive
tools/install-hooks.sh                  # once per clone: pre-commit + commit-msg hooks
cmake --workflow --preset debug         # configure + build + run all tests
cmake --build --preset debug            # rebuild only
ctest --preset debug -R "<test name>"   # run matching tests
ctest --preset debug -L pluginval       # validate the built VST3 with pluginval (strictness 10)
ctest --preset debug -L auval           # validate the built AU with auval -strict (installs it temporarily)
tools/format.sh                         # clang-format 23 (--check to verify only)
tools/tidy.sh build/debug               # clang-tidy 23 (after a build)
tools/check-identity-leak.sh            # no private codes in tracked files/commits
tools/check-no-published-builds.sh      # no workflow step publishes build output
tools/check.sh [--asan]                 # the local gate: all of the above, run before every commit
tools/bench.sh ["[area]"]               # [!benchmark] tests in relwithdebinfo -> Markdown table (docs/performance/)
```

**Git workflow.** Commit directly to `main`. Do not create feature or work branches, in this repo or on GitHub. Run `tools/check.sh` and make sure it passes before every commit.

**Testing and CI policy.** Run `tools/check.sh` locally on macOS and make sure it passes before every commit. Only local macOS builds are used for pre-commit testing (for now). GitHub Actions runs only for major and minor versions: push a `vX.Y.0` tag that matches `project(VERSION)`. It is fail-fast: macOS tests first, then Linux (GCC 12) and Windows (MSVC) tests, then the macOS release build. Do not add per-push or per-PR CI.

**No downloadable builds on GitHub.** GitHub hosts source only. Never add workflow artifacts, releases or release assets, packages, or Pages deploys, and never upload binaries by hand. CI builds and tests, then discards the output. `tools/check-no-published-builds.sh` enforces this for workflows. Details: [docs/development/ci.md](docs/development/ci.md).

On Windows, run these from a Visual Studio developer prompt (so Ninja finds `cl.exe`).

## Architecture and modularity

Everything except the plugin target itself must be reusable in other projects. The layout below is proposed:

```
external/          JUCE, Catch2 (submodules)
libs/dsp/          anoesis::dsp: oscillators, filters, envelopes, LFOs, effects, voice management
libs/gui/          anoesis_gui JUCE module (namespace anoesis::gui): knobs, LCD panel, meters, look-and-feel
libs/interop/      anoesis_interop JUCE module (namespace anoesis::interop): parameter binding, preset/bank I/O, MIDI learn, host sync
plugin/            Thin synth target (namespace brutalis) that wires the libs together. The only product-specific code.
tests/             Catch2 tests, one executable per lib, mirroring libs/ and plugin/
tools/             format.sh, tidy.sh, check-identity-leak.sh, git hooks
docs/              Part 4 documentation
```

Full rationale: [docs/development/project-structure.md](docs/development/project-structure.md).

- **`libs/dsp` has no JUCE dependency and no dependency on the other libs.** It is pure, portable C++, so it builds for desktop and, later, STM32 (ARM Cortex-M). Concretely:
  - no heap allocation after `prepare()`/initialization;
  - nothing depends on exceptions or RTTI;
  - no OS, threading, file, or logging calls;
  - `float` is the primary sample type;
  - platform-specific SIMD sits behind an abstraction with a scalar fallback.
- **`libs/gui` and `libs/interop`** may depend on JUCE but must know nothing about this synth's parameter set. They receive what they need through configuration or interfaces.
- Each lib is its own CMake target with its own namespace, public headers, and test target. Dependencies point only toward `dsp`. Never create cycles.
- **`libs/gui` and `libs/interop` are JUCE modules** (`juce_add_module`), not static libraries. Each final binary must compile JUCE exactly once, and the plugin target always compiles it. Link modules `PRIVATE` only. Put new implementation files in a module subfolder and `#include` them from the module's root `.cpp`.
- The plugin's sources live in the `brutalis_plugin_common` INTERFACE target, which both the plugin and `brutalis_plugin_tests` link. Shared JUCE config flags go in `anoesis_configure_juce_target()` ([cmake/AnoesisJuce.cmake](cmake/AnoesisJuce.cmake)).

## Conventions

- **Standards:** idiomatic modern C++ at the project's language level, following the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/). Use clang-format and clang-tidy, both pinned to major version 23 ([.clang-format](.clang-format), [.clang-tidy](.clang-tidy)). Treat warnings as errors on every compiler:
  - Clang and GCC: `-Wall -Wextra -Wpedantic -Werror`
  - MSVC: `/W4 /WX /permissive-`
- **Naming:** types `CamelCase`; functions, variables, and members `camelBack`; namespaces `lower_case`; files `snake_case.hpp/.cpp`. Enforced by clang-tidy.
- **Float comparisons in tests:** `-Wfloat-equal` is on. Use Catch2 matchers (`WithinULP(x, 0)` for exact, `WithinAbs`/`WithinRel` with a justified tolerance).
- **Test-driven development is required.** Write a failing Catch2 test first, make it pass, then refactor. No production code goes in without a test that covers it.
  - DSP tests check behavior numerically: frequency and phase response, aliasing levels, envelope timing, stability at extreme settings, and denormals.
  - Add offline-render regression tests for whole patches once the voice path exists.
- **Real-time safety:** in the audio thread (`processBlock` and everything it calls), do not allocate, lock, do I/O, or log. Do not throw exceptions there.
- **Parameters:** define them in one place. Parameter IDs are stable once released, so never rename them.

## Documentation layout (Part 4)

- `docs/` holds the write-ups, one file per technique or component (for example `docs/oscillators/polyblep.md`). `docs/reference/` holds source material such as the Sylenth1 feature inventory.
- Each write-up covers: the general theory, how it is applied here (with file links), the trade-offs and the alternatives considered, the tests that pin the behavior down, and references.
- Keep a `docs/README.md` index with a suggested reading order.

## Sylenth1 reference resources

When sources conflict, the official ones win. Use third-party sources to fill gaps the manual leaves (marked "Unspecified" in the feature inventory), and confirm anything important by listening to or measuring the demo.

**Official (LennarDigital)**
- [Product page](https://www.lennardigital.com/sylenth1/)
- [Manual (PDF)](http://files.lennardigital.com/manual/Sylenth1Manual.pdf): v2.5 (2015). The primary source for Part 1 features.
- [Manual page](https://lennardigital.com/sylenth1/manual.php): every language edition. Check it for a manual newer than v2.5.
- [What's new (changelog)](https://lennardigital.com/sylenth1/whatsnew.php): every v3.x change. These are the features added since the manual (see §15 of the feature inventory).
- [Downloads (demo)](https://lennardigital.com/downloads): the demo, which can answer behavior questions by listening and measuring. Black-box testing only; see the clean-room constraint. The demo disables half the modulation options and plays a reminder sound about every 60 seconds.
- [Video overview](https://www.youtube.com/watch?v=y3wMVOdIZ1c)

**Third-party descriptions**
- [KVR product page](https://www.kvraudio.com/product/sylenth1-by-lennardigital): the version history and supported formats, plus user reviews.
- [MusicRadar review](https://www.musicradar.com/reviews/tech/lennardigital-sylenth1-22935): a summary of the architecture. Notes that the filters cannot run in series, which is a gap to consider for Part 2.
- [MusicRadar: parts and oscillators](https://www.musicradar.com/tuition/tech/how-to-master-sylenth1s-parts-and-oscillators-630710): covers the 8 oscillator waveforms and the invert-based PWM trick.
- [MusicRadar: filters and amp envelopes](https://www.musicradar.com/tuition/tech/how-to-master-sylenth1s-filters-and-amplitude-envelopes-630711)
- [Ask.Audio: arpeggiator tips](https://ask.audio/articles/expert-sylenth-arpeggiator-tips): arpeggiator modes and step-sequencer use.
- [ADSR Sounds: master effects tutorial](https://www.adsrsounds.com/sylenth-tutorials/sylenth1-master-effects-section/)
- [FutureMusic review](https://www.futuremusic.com/news/testdrive/lennar_sylenth1_review.html)

Patch tutorials (for example [MusicRadar's hoover](https://www.musicradar.com/tuition/tech/how-to-create-a-hoover-sound-in-sylenth1-608251) and [stacked chords](https://www.musicradar.com/tuition/tech/how-to-create-stacked-chords-in-sylenth1-608249)) make useful listening tests: build the same patch in both synths and compare the results.
