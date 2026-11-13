# Plugin validation (pluginval)

## Why validate in a host

Unit tests call the processor's C++ API directly. Hosts do much more:

- they scan the binary and open it cold and warm;
- they call into it from several threads;
- they change the sample rate and block size between runs;
- they save and restore state, automate parameters, and open the editor while audio is running.

Many real-world plugin bugs live only in that interaction: thread-safety problems, state that doesn't round-trip, NaNs, crashes on an unusual block size. A validator acts as a demanding host and runs all of these scenarios automatically.

## The tool

[pluginval](https://github.com/Tracktion/pluginval), by Tracktion, is the de facto open-source validator for VST3 and AU, on macOS, Linux, and Windows. Its strictness levels range from 1 to 10. Higher levels add tests (parameter thread safety, state restoration, editor automation) and more iterations.

We pin **v1.0.4**, the latest stable release. The 2.0 betas were skipped. pluginval is GPLv3, but we only run it. We never link it or ship it, so its license places no obligations on Brutalis.

## How it is wired in

- **Download.** [cmake/AnoesisPluginval.cmake](../../cmake/AnoesisPluginval.cmake) downloads the release zip for the current platform at configure time. It verifies the zip's **SHA-256** against hashes pinned in that file, then caches the binary in `.cache/pluginval/<version>/<platform>/` (gitignored). After the first configure, no further network access is needed. The macOS build is universal and notarized (Developer ID).
- **Test.** [tests/pluginval/CMakeLists.txt](../../tests/pluginval/CMakeLists.txt) registers a CTest test, `pluginval:VST3`. It runs pluginval at **strictness 10** on the built `Brutalis.vst3`, labeled `pluginval`. JUCE stores the bundle path as a generator expression, so the test wraps it in `$<GENEX_EVAL:…>`. Without that wrapper, pluginval was handed the literal, unexpanded text.
- **Where it runs.** Every route that runs tests runs pluginval, because it is an ordinary CTest test:
  - `ctest`
  - `cmake --workflow --preset debug` / `relwithdebinfo` / `release`
  - `tools/check.sh`
  - every Version CI test leg
- **Logs** are written to `build/<preset>/tests/pluginval/logs/`.

```sh
ctest --preset debug -L pluginval          # only pluginval
ctest --preset debug -LE pluginval         # everything except pluginval
```

| Option | Default | Effect |
|---|---|---|
| `BRUTALIS_PLUGINVAL` | ON | Registers the pluginval test. |
| `BRUTALIS_PLUGINVAL_EXECUTABLE` | empty | Uses this binary instead of downloading one (for offline builds or a newer pluginval). |
| `BRUTALIS_PLUGINVAL_STRICTNESS` | 10 | Strictness level, from 1 to 10. |

## What is not covered (yet)

- **Sanitizer builds** (`asan`) skip pluginval. An ASan-instrumented plugin can't be loaded into a host that wasn't built with ASan: the ASan runtime has to be present from process start.
- **AU.** macOS finds Audio Units only when they are installed in a `Components` folder, and the build installs nothing. AU validation (`auval`, or pluginval on an installed AU) is a separate step, still open under M0.6 and M1.12.
- **Steinberg's VST3 validator.** pluginval can call it (`--vst3validator`), but the VST3 SDK's validator isn't built here. Consider adding it with M1.12.
- **Random seed.** pluginval uses a different random seed each run and prints it in the log. To reproduce a failure, rerun with `--random-seed <seed>`.

## Evidence that it works (2026-11-13)

- **Pass.** The Phase 0 plugin passes every strictness-10 test on macOS (Apple Clang, debug) in about 16 s. The tests cover scanning, cold and warm open, info, programs, editor, editor-while-processing, audio processing, non-releasing processing, state, state restoration, automation, parameters, background-thread state, parameter thread safety, and buses.
- **Fail.** To prove it catches real bugs, `processBlock` was temporarily changed to output NaNs. The test then failed with `NaNs found in buffer`, and the change was reverted.

## Bumping the pluginval version

1. Update `ANOESIS_PLUGINVAL_VERSION` in `cmake/AnoesisPluginval.cmake`.
2. Download the three release zips and replace the three SHA-256 values: `shasum -a 256 pluginval_*.zip`.
3. Run `tools/check.sh`. Old cached versions in `.cache/pluginval/` can be deleted.

## References

- pluginval README and the [testing-plugins guide](https://github.com/Tracktion/pluginval/blob/develop/docs/Testing%20plugins%20with%20pluginval.md).
- JUCE `AudioProcessor` documentation on thread safety and state (`external/JUCE/modules/juce_audio_processors_headless`).
