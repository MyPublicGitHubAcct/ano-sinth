# Test-driven development in this repo

## The rule

No production code goes in without a test that covers it, and the test comes first:

1. **Red.** Write a Catch2 test for the behavior you want. Build it and watch it **fail for the right reason**. A failed assertion is the best evidence: it proves the test can detect the missing behavior. A compile error is weaker, but acceptable when the API doesn't exist yet.
2. **Green.** Write the least code that makes the test pass.
3. **Refactor.** Clean up the code and the test with the test still passing. Then run `tools/check.sh`, which builds, runs every test, tidies, and checks formatting and identity leaks. It must pass before you commit. Hosted CI runs only for version tags ([ci.md](ci.md)).

Why it matters here: DSP bugs are often *quiet*. A filter that is 3 dB off, an envelope with a 1-sample glitch, or aliasing at −60 dB all still make sound. Tests that measure behavior numerically are the only reliable way to keep such bugs out. They also become the regression net for Phase 3, when code is rewritten for speed.

## Where tests live

`tests/` mirrors the source tree, with one executable per library:

| Executable | Tests | Links |
|---|---|---|
| `anoesis_dsp_tests` | `libs/dsp` | Catch2 and `anoesis_test_support` (no JUCE, like the library) |
| `anoesis_test_support_tests` | `tests/support` (the measurement code itself) | Catch2 and `anoesis_test_support` |
| `anoesis_gui_tests` | `libs/gui` | JUCE console app + the `anoesis_gui` module |
| `anoesis_interop_tests` | `libs/interop` | JUCE console app + the `anoesis_interop` module |
| `brutalis_plugin_tests` | `plugin/` | JUCE console app + the plugin's own sources (see [project-structure.md](project-structure.md#how-the-plugin-and-its-tests-share-code)) |
| `pluginval:VST3` (CTest only) | the built VST3, in a host | pluginval at strictness 10 ([plugin-validation.md](plugin-validation.md)). Skipped in `asan`. |
| `auval:AU` (CTest only, macOS) | the built AU, installed temporarily | `auval -strict` ([plugin-validation.md](plugin-validation.md#auval-audio-units-macos)). Skipped in `asan`. |
| `brutalis_plugin_tests:plugin targets are built unless sanitizing` (CTest only) | the build itself | The plugin's shared code and format wrappers are in the default build, except in `asan`, which leaves them out ([tests/cmake/check_plugin_targets_in_all.cmake](../../tests/cmake/check_plugin_targets_in_all.cmake)). |

Name test files `<unit>_test.cpp`. Tag test cases with the library (`[dsp]`) plus a category, such as `[smoke]`, and later `[spectral]`, `[timing]`, or `[!benchmark]`.

## The test support library

[tests/support/](../../tests/support/) builds `anoesis_test_support`: the measurement code and fixtures the DSP tests share. It lives under `tests/` and never in `libs/dsp`, because it may allocate, use `double` freely, and touch files. Like `libs/dsp`, it has no JUCE dependency. Include it as `#include <anoesis/test/...>`, namespace `anoesis::test`.

| Header | What it gives you |
|---|---|
| `buffer.hpp` | `Buffer`, an owning multichannel buffer that hands out an `AudioBlock`; `bitIdentical()` for determinism checks |
| `lifecycle.hpp` | `checkLifecycleContract()`: proves `reset()` and a repeated `prepare()` restore the freshly prepared state ([processor lifecycle](../dsp/processor-lifecycle.md#tests)) |
| `signals.hpp` | test signals, and `coherentFrequency()` for leakage-free spectral tests |
| `fft.hpp`, `window.hpp`, `spectrum.hpp` | radix-2 FFT (D5), analysis windows, normalised power spectra |
| `measure.hpp` | THD, aliasing, and frequency response (by sine or impulse) |
| `golden.hpp` | float WAV I/O and golden-file comparison |

How the measurements work and how to use them: [How we measure DSP quality](../dsp/measuring-dsp.md).

A wrong measurement makes every test built on it meaningless, so the support code has its own tests in `tests/support/test/`.

## Adding a test

1. Create `tests/<lib>/<unit>_test.cpp` and add it to that folder's `CMakeLists.txt`.
2. Write the test case. `anoesis_register_tests()` in [tests/CMakeLists.txt](../../tests/CMakeLists.txt) registers every `TEST_CASE` with CTest automatically, each as its own CTest test named `<executable>:<test case name>`. Discovery happens when the tests run (`DISCOVERY_MODE PRE_TEST`), so universal binaries never have to run during the build.
3. Run `cmake --workflow --preset debug`, or for a faster loop, rebuild and run the one executable directly:

```sh
cmake --build --preset debug --target anoesis_dsp_tests && ./build/debug/tests/dsp/anoesis_dsp_tests "[smoke]"
```

## Comparing floating-point values

JUCE's recommended warnings include `-Wfloat-equal`, so `CHECK(x == 0.0f)` does not compile. That is deliberate: say what you mean.

- **Exact** (for example "the output is silent"): `CHECK_THAT(x, Catch::Matchers::WithinULP(0.0f, 0))`.
- **Approximate**: `WithinAbs(expected, tolerance)` or `WithinRel(expected, fraction)`. Every tolerance needs a reason, written in a comment or derived from the theory.

## Worked example: the Phase 0 smoke tests

The first tests in the repo were written red-first, and they show the loop end to end.

- **`libs/dsp`.** [tests/dsp/library_test.cpp](../../tests/dsp/library_test.cpp) expects `anoesis::dsp::libraryName() == "anoesis_dsp"`. The first implementation returned an empty string. The test failed with `"" == "anoesis_dsp"`, and only then was the real value filled in. The test is trivial on purpose: it proves the build, link, and CTest wiring work.
- **The plugin.** [tests/plugin/plugin_processor_test.cpp](../../tests/plugin/plugin_processor_test.cpp) fills a buffer with 0.5 (simulating garbage a host may pass in), calls `processBlock`, and requires exact silence. The first `processBlock` did nothing. The test failed (`0.5f is within 0 ULPs of 0.0f`), and adding `buffer.clear()` made it pass. This is a real requirement: an instrument must write every output sample.

## Golden files

Golden files are committed renders in [tests/golden/](../../tests/golden/). A test compares its output with one using `compareWithGolden()`. To create or update one, run the test with `ANOESIS_UPDATE_GOLDEN=1`, listen to the result, and commit it with an explanation of why the output changed. On a mismatch the actual render is written to `build/<preset>/test-output/`. Details: [How we measure DSP quality](../dsp/measuring-dsp.md#golden-files).

## Benchmarks

Benchmarks are Catch2 `BENCHMARK`s in `tests/<lib>/benchmarks/`, in `TEST_CASE`s tagged `[!benchmark]`. That tag hides them from the default run and from `ctest`, and a CTest check fails if one leaks in. Run them in an optimised build with `tools/bench.sh`, and record results in [docs/performance/baselines.md](../performance/baselines.md). Methodology: [benchmarking.md](../performance/benchmarking.md).

## Planned (Phase 1)

- Envelope timing and stability checks at extreme settings, as the components arrive.
- Golden-file regression tests for whole patches (M1.8).

## References

- Kent Beck, *Test-Driven Development: By Example* (2002).
- Catch2 docs: `external/Catch2/docs/` (start with `tutorial.md`, `comparing-floating-point-numbers.md`, `cmake-integration.md`).
