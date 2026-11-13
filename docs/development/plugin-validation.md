# Plugin validation (pluginval and auval)

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

## auval (Audio Units, macOS)

`auval` is Apple's own AU validator. Logic Pro and GarageBand run it before they load an AU, and refuse AUs that fail. That makes it the gate that matters for AU users. It checks:

- the component's description and properties;
- its channel configurations;
- render behavior across formats and sample rates;
- parameter and preset handling.

With `-strict`, it adds checks for behaviors Apple treats as required.

**The catch: auval only sees installed AUs.** macOS discovers Audio Units through a system registry (the `AudioComponentRegistrar` daemon), which scans `Components` folders. A component sitting in the build tree is invisible to it.

How it is wired in:

- [tests/auval/CMakeLists.txt](../../tests/auval/CMakeLists.txt) registers the CTest test `auval:AU`, labeled `auval`. It exists on macOS only and is skipped in sanitizer builds. Its AU type is `aumu`, because Brutalis is an instrument.
- The test runs [tools/auval-test.sh](../../tools/auval-test.sh), which:
  1. moves any already-installed `Brutalis.component` in `~/Library/Audio/Plug-Ins/Components` aside;
  2. copies the freshly built component there;
  3. refreshes the registry: it kills `AudioComponentRegistrar` (the daemon restarts on demand), then runs `auval -a`;
  4. runs `auval -strict -v aumu <subtype> <manufacturer>`;
  5. **always** removes the test copy, puts back what was there before, and refreshes the registry again. A `trap` handles this, so it happens on failure and Ctrl-C too.

  Only the per-user folder is touched; no `sudo` is needed. The test is `RUN_SERIAL` with a resource lock, so it never races another install.
- **The identity codes stay out of logs.**
  - CTest passes them to the script through environment variables, not on the command line.
  - The script masks them in auval's output as `<manufacturer>` and `<subtype>`. It matches whole words, so "Brutalis" stays readable.
  - In the CI release job, GitHub's secret masking applies as well.

```sh
ctest --preset debug -L auval                 # only auval
tools/auval-test.sh <Plugin.component> aumu   # by hand (needs BRUTALIS_AU_MANUFACTURER / _SUBTYPE)
```

| Option | Default | Effect |
|---|---|---|
| `BRUTALIS_AUVAL` | ON | Registers the auval test (macOS only). |

## What is not covered (yet)

- **Sanitizer builds** (`asan`) skip pluginval and auval. An ASan-instrumented plugin can't be loaded into a host that wasn't built with ASan: the ASan runtime has to be present from process start.
- **AU through pluginval.** pluginval validates the VST3 only. auval covers the AU, and the processing code behind the two formats is the same.
- **Steinberg's VST3 validator.** pluginval can call it (`--vst3validator`), but the VST3 SDK's validator isn't built here. Consider adding it with M1.12.
- **Random seed.** pluginval uses a different random seed each run and prints it in the log. To reproduce a failure, rerun with `--random-seed <seed>`.

## Evidence that it works (2026-11-13)

pluginval:

- **Pass.** The Phase 0 plugin passes every strictness-10 test on macOS (Apple Clang, debug) in about 16 s. The tests cover scanning, cold and warm open, info, programs, editor, editor-while-processing, audio processing, non-releasing processing, state, state restoration, automation, parameters, background-thread state, parameter thread safety, and buses.
- **Fail.** To prove it catches real bugs, `processBlock` was temporarily changed to output NaNs. The test then failed with `NaNs found in buffer`, and the change was reverted.

auval:

- **Pass.** The debug AU passes `auval -strict` (`AU VALIDATION SUCCEEDED`, about 12 s), and the output contains neither identity code.
- **Fail.** Wrong codes make the test fail, because auval can't find the component.
- **Restore.** A dummy `Brutalis.component` that was installed beforehand was put back intact after the test.

## Bumping the pluginval version

1. Update `ANOESIS_PLUGINVAL_VERSION` in `cmake/AnoesisPluginval.cmake`.
2. Download the three release zips and replace the three SHA-256 values: `shasum -a 256 pluginval_*.zip`.
3. Run `tools/check.sh`. Old cached versions in `.cache/pluginval/` can be deleted.

## References

- pluginval README and the [testing-plugins guide](https://github.com/Tracktion/pluginval/blob/develop/docs/Testing%20plugins%20with%20pluginval.md).
- Apple: `man auval`, and "Audio Unit Programming Guide" (validation section).
- JUCE `AudioProcessor` documentation on thread safety and state (`external/JUCE/modules/juce_audio_processors_headless`).
