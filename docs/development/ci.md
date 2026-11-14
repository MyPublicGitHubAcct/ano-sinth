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
- **Platform order.** macOS is tested locally before every commit. In every version run, CI tests **Linux first, then Windows, then macOS**, one after the other, and builds the macOS release last. Linux and Windows are not built locally, so they go first: they are the platforms most likely to find something new.
- **Fail fast.** One failure stops the run, so no minutes are spent after something has already broken.
- **No downloadable builds on GitHub.** CI compiles and tests, and nothing more. No workflow uploads artifacts, creates releases or release assets, pushes packages, or deploys Pages, and no one uploads binaries by hand. The repository is public, so any of these would make a compiled plugin downloadable by anyone. Binaries contain JUCE (AGPLv3 or commercial) and the real plugin identity, so distributing them is a separate decision (see PLAN.md, **Distribution**). [tools/check-no-published-builds.sh](../../tools/check-no-published-builds.sh) enforces this for workflows: it fails on `actions/upload-artifact`, release actions, `gh release`, Pages deploys, package pushes, and `contents: write` or `packages: write` permissions. It runs in `tools/check.sh` and as the last step of CI job 1.

## The workflow: [version-ci.yml](../../.github/workflows/version-ci.yml)

The jobs run as a chain. Each needs the one before it, so a failure skips everything downstream:

| # | Job | Runner | What it does |
|---|---|---|---|
| 1 | Version, format, and identity checks | ubuntu-latest | Checks that the tag equals `v` + `project(VERSION …)` in [CMakeLists.txt](../../CMakeLists.txt). Then runs the clang-format 23.1.5 check, the identity-leak check over tracked files and every commit message (with values from secrets), and the no-published-builds check. These need no compiler, so they use the cheapest runner. Always runs. |
| 2 | Linux GCC 12 tests (`debug`, `asan`) | ubuntu-22.04 | `cmake --workflow --preset <preset>` under `xvfb-run`. The `debug` leg includes pluginval. The `asan` leg runs the tests under GCC's sanitizers (see [Why debug and asan are separate builds](#why-debug-and-asan-are-separate-builds) and [Why Linux has two legs](#why-linux-has-two-legs)). Uses the placeholder identity. |
| 3 | Windows MSVC tests (`debug`) | windows-latest | `debug` inside `vcvars64.bat` (located with `vswhere`), including pluginval. The `asan` preset doesn't exist on Windows. Uses the placeholder identity. |
| 4 | macOS tests (`debug`, `asan`) | macos-latest | The same two presets as Linux. The `debug` leg includes pluginval at strictness 10 on the VST3 and `auval -strict` on the AU, and runs clang-tidy 23.1.4. The `asan` leg runs the tests under ASan/UBSan. Uses the placeholder identity. |
| 5 | macOS release build | macos-latest | The `release` preset (universal binary) with the real identity from secrets. The preset sets `BRUTALIS_REQUIRE_REAL_IDENTITY=ON`, so a missing secret fails configure. Builds and tests (pluginval and auval validate the release VST3 and AU). **Uploads nothing**: the outputs are discarded with the runner. |

The two-leg jobs are fail-fast matrices: if one leg fails, the other is cancelled. Jobs 3–5 run only if nothing before them failed or was cancelled.

The run page shows the legs as `macOS tests (debug)`, `Linux GCC 12 tests (asan)`, and so on. The workflow's `name:` fields contain no `${{ matrix.preset }}`: GitHub appends the matrix values to each leg's name by itself. A job that its `if:` skips is never expanded, so an expression in its name is shown literally (run 43536705167 listed `macOS tests (${{ matrix.preset }})`). Without one, a skipped matrix job reads plainly as `macOS tests`.

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
gh workflow run version-ci.yml -f windows=false -f macos=false -f release=false   # Linux only
gh workflow run version-ci.yml -f asan=false -f release=false       # debug legs on all three platforms
```

| Input | Job it controls |
|---|---|
| `linux` | 2, Linux tests |
| `windows` | 3, Windows tests |
| `macos` | 4, macOS tests |
| `asan` | the `asan` leg of jobs 2 and 4 (off: `debug` only) |
| `release` | 5, macOS release build |

Job 1 always runs. The chain still holds: each selected job waits for the selected jobs before it, and is skipped if one of them failed. A job that is switched off is *skipped*, not failed, so the chain carries on past it. The `if:` conditions use `!cancelled() && !failure()` rather than the default `success()`, because `success()` treats a skipped job before it as a reason to skip. `failure()` also sees failures further back, through a skipped job, so turning off Windows doesn't let macOS run after a Linux failure.

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

Commit `92a62d4` made the jobs a selectable chain and took the plugin's shared code and format wrappers out of the default sanitizer build. A Linux-only dispatch on `main` at `352ab66` (run 43536705167, `-f macos=false -f windows=false -f release=false`) checked both changes. The checks job and both Linux legs ran and passed. macOS, Windows, and the release build were skipped, and Linux still ran after the skipped macOS job, as intended. The new CTest check `plugin targets are built unless sanitizing` passed in both legs (96 tests in `debug`, 95 in `asan`, which skips pluginval). Compared with the Linux legs of the `v0.1.0` tag run (43529431143):

| Linux leg | Build steps (Ninja) | Build phase | Whole job |
|---|---|---|---|
| `asan`, before | 324 | 13m15s | 14m40s |
| `asan`, after | 259 | 6m59s | 8m20s |
| `debug`, before | 324 | 4m59s | 6m17s |
| `debug`, after | 324 | 6m33s | 8m09s |

The `asan` leg lost 20% of its build steps but 47% of its build time, because the removed units are JUCE's heaviest, compiled under the slowest flags. (macOS: 327 → 249.) The `debug` leg builds exactly what it built before, so its slower time is runner variance (configure alone went from 34s to 48s). The two legs now take about the same time, so the matrix's wall time is no longer set by `asan`. The skipped macOS job's name exposed the unexpanded-matrix-name problem described under [The workflow](#the-workflow-version-ciyml), and the job names were fixed afterwards.

To pull a job's log, use its ID: `gh api repos/<owner>/<repo>/actions/jobs/<id>/logs --allow-escape-sequences`. For the tag run, `gh run view --job=<id> --log` returned the `asan` leg's log for both Linux jobs. They were then named `Linux tests (GCC 12, …)`, and `gh` cut those names off at the comma.

## Not done yet

| Item | Planned in |
|---|---|
| Distributing builds (where, signing and notarization, installers, all platforms) | A later decision, never via GitHub as things stand (PLAN.md, **Distribution**) |
| Build caching (ccache) | When CI time becomes a problem |
