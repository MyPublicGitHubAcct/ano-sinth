# Continuous integration

## Policy

- **Local first.** Every commit is tested locally on macOS before it is made. Run [tools/check.sh](../../tools/check.sh), which:
  1. configures, builds, and runs all tests (`debug` preset);
  2. runs clang-tidy;
  3. runs the format check;
  4. runs the identity-leak check;
  5. runs the no-published-builds check;

  and stops at the first failure. Add `--asan` to also run the sanitizer build. The git hooks also check formatting and identity leaks on each commit.
- **GitHub Actions runs only for major and minor versions.** The trigger is a pushed tag `vX.Y.0`, or a manual dispatch. Patch versions (`vX.Y.Z` with Z > 0), ordinary pushes, and pull requests run nothing. This keeps hosted CI time for the points where a version is cut.
- **Platform order.** macOS is tested first: locally before every commit, then in CI. **Then Linux, then Windows**, one after the other, in every version run. They are not built locally.
- **Fail fast.** One failure stops the run, so no minutes are spent after something has already broken.
- **No downloadable builds on GitHub.** CI compiles and tests, and nothing more. No workflow uploads artifacts, creates releases or release assets, pushes packages, or deploys Pages, and no one uploads binaries by hand. The repository is public, so any of these would make a compiled plugin downloadable by anyone. Binaries contain JUCE (AGPLv3 or commercial) and the real plugin identity, so distributing them is a separate decision (see PLAN.md, **Distribution**). [tools/check-no-published-builds.sh](../../tools/check-no-published-builds.sh) enforces this for workflows: it fails on `actions/upload-artifact`, release actions, `gh release`, Pages deploys, package pushes, and `contents: write` or `packages: write` permissions. It runs in `tools/check.sh` and as the last step of CI job 1.

## The workflow: [version-ci.yml](../../.github/workflows/version-ci.yml)

The jobs run as a chain. Each needs the one before it, so a failure skips everything downstream:

| # | Job | Runner | What it does |
|---|---|---|---|
| 1 | Version, format, and identity checks | ubuntu-latest | Checks that the tag equals `v` + `project(VERSION …)` in [CMakeLists.txt](../../CMakeLists.txt). Then runs the clang-format 23.1.5 check, the identity-leak check over tracked files and every commit message (with values from secrets), and the no-published-builds check. These need no compiler, so they use the cheapest runner. Always runs. |
| 2 | macOS tests (`debug`, `asan`) | macos-latest | `cmake --workflow --preset <preset>`. The `debug` leg includes pluginval at strictness 10 on the VST3 and `auval -strict` on the AU, and runs clang-tidy 23.1.4. The `asan` leg runs the tests under ASan/UBSan (see [Why debug and asan are separate builds](#why-debug-and-asan-are-separate-builds)). Uses the placeholder identity. |
| 3 | Linux tests (GCC 12, `debug`, `asan`) | ubuntu-22.04 | The same two presets, under `xvfb-run`. The `debug` leg includes pluginval. See [Why Linux has two legs](#why-linux-has-two-legs). Uses the placeholder identity. |
| 4 | Windows tests (MSVC, `debug`) | windows-latest | `debug` inside `vcvars64.bat` (located with `vswhere`), including pluginval. The `asan` preset doesn't exist on Windows. Uses the placeholder identity. |
| 5 | macOS release build | macos-latest | The `release` preset (universal binary) with the real identity from secrets. The preset sets `BRUTALIS_REQUIRE_REAL_IDENTITY=ON`, so a missing secret fails configure. Builds and tests (pluginval and auval validate the release VST3 and AU). **Uploads nothing**: the outputs are discarded with the runner. |

The two-leg jobs are fail-fast matrices: if one leg fails, the other is cancelled. Jobs 3–5 run only if nothing before them failed or was cancelled.

Within each job, steps stop at the first failure (the GitHub default). `concurrency` cancels an older run of the same ref if a newer one starts.

### Cutting a major or minor version

```sh
# 1. Bump project(VERSION X.Y.0) in CMakeLists.txt.
tools/check.sh --asan             # 2. Local gate.
git commit -am "Version X.Y.0"    # 3. Commit.
git tag vX.Y.0                    # 4. Tag.
git push origin main vX.Y.0       # 5. Push the commit and the tag. The tag starts CI.
```

If the tag doesn't match the version in CMakeLists.txt, job 1 fails and nothing else runs.

### Running selected jobs

A tag push always runs every job. A manual dispatch (Actions → Version CI → Run workflow, or `gh workflow run`) has a checkbox for each of jobs 2–5 and one for the `asan` legs. All are on by default, so a plain dispatch matches a tag run.

```sh
gh workflow run version-ci.yml                                      # everything
gh workflow run version-ci.yml -f macos=false -f windows=false -f release=false   # Linux only
gh workflow run version-ci.yml -f asan=false -f release=false       # debug legs on all three platforms
```

| Input | Job it controls |
|---|---|
| `macos` | 2, macOS tests |
| `linux` | 3, Linux tests |
| `windows` | 4, Windows tests |
| `asan` | the `asan` leg of jobs 2 and 3 (off: `debug` only) |
| `release` | 5, macOS release build |

Job 1 always runs. The chain still holds: each selected job waits for the selected jobs before it, and is skipped if one of them failed. A job that is switched off is *skipped*, not failed, so the chain carries on past it. The `if:` conditions use `!cancelled() && !failure()` rather than the default `success()`, because `success()` treats a skipped job before it as a reason to skip. `failure()` also sees failures further back, through a skipped job, so turning off Linux doesn't let Windows run after a macOS failure.

Inputs are empty on a tag push, and GitHub's loose `==` treats an empty input as equal to `false`. So every condition checks `github.event_name != 'workflow_dispatch'` first, and never compares an input with `false`.

### Why debug and asan are separate builds

ASan and UBSan instrument every object file. They add checks around memory accesses and arithmetic and change the code layout, so a debug object and an asan object compiled from the same source are different. The two presets can't share objects, and a compiler cache wouldn't help either, because the flags are part of the cache key. They run as parallel matrix legs so that the wall time is that of the slower leg, not their sum.

What *can* be removed is work the asan build has no use for. No host can load an ASan-instrumented plugin, so pluginval and auval are skipped there. The plugin's shared code and its format wrappers (VST3, AU, Standalone) were still being built: about 80 compiles, about half of them JUCE's heaviest module units, under the slowest flags. [plugin/CMakeLists.txt](../../plugin/CMakeLists.txt) now marks those targets `EXCLUDE_FROM_ALL` when `BRUTALIS_SANITIZERS` is on. Nothing is lost: `brutalis_plugin_tests` compiles the same plugin sources (through `brutalis_plugin_common`) and runs them under the sanitizers, and the `debug` leg builds and validates the wrappers. On macOS this cut the asan build from 327 to 249 compiles. A CTest check ([tests/cmake/check_plugin_targets_in_all.cmake](../../tests/cmake/check_plugin_targets_in_all.cmake)) pins it: the targets must be out of the default build in sanitizer builds and in it otherwise.

Each build still compiles JUCE once per JUCE binary: the plugin, `brutalis_plugin_tests`, `anoesis_gui_tests`, and `anoesis_interop_tests`. That follows from the rule that each final binary compiles JUCE once ([project-structure.md](project-structure.md)). Cutting it further would mean merging test runners or linking the tests against the plugin's shared code, which is a structural change rather than a CI one.

### Why Linux has two legs

Each leg checks something the other can't:

- **`debug`** builds the plugin itself with GCC and runs pluginval on the Linux VST3. That is the only check of the plugin in a host on Linux, and it can't run in `asan`.
- **`asan`** runs the tests under GCC's sanitizers against libstdc++. macOS runs Apple Clang's sanitizers against libc++, so the instrumentation, the standard library, and the code generation all differ. And on Linux, ASan includes **LeakSanitizer** by default, which Apple Clang doesn't support on macOS: Linux is the only place leaks are checked.

To skip the asan legs on a dispatch, turn off `asan`.

## Secrets

These are set in the repository's Actions secrets (done on 2026-11-13):

- `BRUTALIS_MANUFACTURER_CODE`
- `BRUTALIS_PLUGIN_CODE`
- `BRUTALIS_BUNDLE_ID`

The leak check and the release build use them. To change one, run `printf %s "<value>" | gh secret set <NAME>`. Never echo a value in a log.

## Platform notes

- **Linux:** built on `ubuntu-22.04` with GCC 12. 22.04 is the minimum supported Linux (decision D8). Building there links against its libstdc++ (from GCC 12), so the binaries run on 22.04 and newer. Tests run under `xvfb-run`, because the editor test creates JUCE components.
- **Windows:** the build runs inside `vcvars64.bat`, located with `vswhere`, so that Ninja finds `cl.exe`. Doing this ourselves avoids depending on a third-party action.
- **CMake versions differ.** Development uses CMake 4.x, but the runners may have 3.x (the floor is 3.25). Every script run with `cmake -P` must start with `cmake_minimum_required(VERSION 3.25)`, because script mode sets no policies by default.
- **Because Linux and Windows are tested only at version time,** GCC- and MSVC-specific problems show up only then. Keep code standard C++23 with no extensions, and don't assume a library feature exists just because Apple Clang accepts it.

## History

The first CI run (run 43498239659, from commit `2b5476c`) ran on every push and covered all three platforms. It failed and stored no artifacts. On 2026-11-13 the repository had no releases, no workflow artifacts, and no packages:

- **Linux, GCC 12:** passed as-is, for both `debug` and `asan`.
- **Windows, MSVC:** failed on warning C4459. A test constant was hidden by a JUCE template parameter with the same name. That has been fixed in the test, but MSVC hasn't checked the fix yet. The first version run will show whether it worked.

The first version run (run 43516389668, tag `v0.1.0`) passed the checks and both macOS legs. Linux then failed: since run 43498239659, project targets had added `-Wconversion`, and JUCE's own module sources, which compile inside our targets, are not clean under GCC's `-Wconversion`. Fail-fast cancelled Windows. The fix exempts JUCE's units from that one flag on GCC ([code-quality.md](code-quality.md)). The first version of the fix covered only the `.cpp` units. A manually dispatched run on `main` (43518674645) then failed on the bundled C libraries (`juce_graphics_libjpg_1.c`), so the `.c` units were added. A third run (43520356376) compiled all of JUCE but failed in our own `plugin_processor.cpp` and `anoesis_interop.cpp`, on a JUCE header template (`juce_UMPUtils.h`) instantiated there. So JUCE's include directories are now `SYSTEM` on GCC as well. The fourth run (43522223838) built everything on Linux and passed 94 of 95 tests. The failure was [tests/cmake/check_benchmarks_hidden.cmake](../../tests/cmake/check_benchmarks_hidden.cmake): it runs in CMake script mode without `cmake_minimum_required`, so the runner's CMake 3.x rejected `IN_LIST` (policy CMP0057). Local CMake 4.x enables those policies unconditionally, which hid the bug. The fifth run (43524447930, `main` at `6cdada5`) passed every job, including Windows MSVC, which confirmed the C4459 fix. The owner then moved the `v0.1.0` tag from `407e966` to `2ab7587`, and the tag run (43529431143) passed every job. That is the first green version run. The lesson: a warning flag added to JUCE-compiling targets is checked against GCC and MSVC only at the next version run, so check whether JUCE's own flag set for each compiler already includes it.

## Not done yet

| Item | Planned in |
|---|---|
| Distributing builds (where, signing and notarization, installers, all platforms) | A later decision, never via GitHub as things stand (PLAN.md, **Distribution**) |
| Build caching (ccache) | When CI time becomes a problem |
