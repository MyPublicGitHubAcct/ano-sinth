# Test-driven development in this repo

## The rule

No production code goes in without a test that covers it, and the test comes first:

1. **Red.** Write a Catch2 test for the behavior you want. Build it and watch it **fail for the right reason**. A failed assertion is the best evidence: it proves the test can detect the missing behavior. A compile error is weaker, but acceptable when the API doesn't exist yet.
2. **Green.** Write the least code that makes the test pass.
3. **Refactor.** Clean up the code and the test with the test still passing. Format, then run clang-tidy.

Why it matters here: DSP bugs are often *quiet*. A filter that is 3 dB off, an envelope with a 1-sample glitch, or aliasing at −60 dB all still make sound. Tests that measure behavior numerically are the only reliable way to keep such bugs out. They also become the regression net for Phase 3, when code is rewritten for speed.

## Where tests live

`tests/` mirrors the source tree, with one executable per library:

| Executable | Tests | Links |
|---|---|---|
| `anoesis_dsp_tests` | `libs/dsp` | Catch2 only (no JUCE, like the library) |
| `anoesis_gui_tests` | `libs/gui` | JUCE console app + the `anoesis_gui` module |
| `anoesis_interop_tests` | `libs/interop` | JUCE console app + the `anoesis_interop` module |
| `brutalis_plugin_tests` | `plugin/` | JUCE console app + the plugin's own sources (see [project-structure.md](project-structure.md#how-the-plugin-and-its-tests-share-code)) |

Name test files `<unit>_test.cpp`. Tag test cases with the library (`[dsp]`) plus a category, such as `[smoke]`, and later `[spectral]`, `[timing]`, or `[!benchmark]`.

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

## Planned (Phase 1)

- Numerical DSP checks: frequency and phase response, aliasing level, envelope timing, stability at extreme settings, denormals. FFT helpers come with decision D5 in M1.0.
- Offline-render **golden-file** regression tests for whole patches (M1.8).
- Catch2 `BENCHMARK`s that record CPU baselines for Phase 3.

## References

- Kent Beck, *Test-Driven Development: By Example* (2002).
- Catch2 docs: `external/Catch2/docs/` (start with `tutorial.md`, `comparing-floating-point-numbers.md`, `cmake-integration.md`).
