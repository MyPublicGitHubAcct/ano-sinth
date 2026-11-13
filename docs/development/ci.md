# Continuous integration

## Policy

- **Local first.** Every commit is tested locally on macOS before it is made. Run [tools/check.sh](../../tools/check.sh), which:
  1. configures, builds, and runs all tests (`debug` preset);
  2. runs clang-tidy;
  3. runs the format check;
  4. runs the identity-leak check;

  and stops at the first failure. Add `--asan` to also run the sanitizer build. The git hooks also check formatting and identity leaks on each commit.
- **GitHub Actions runs only for major and minor versions.** The trigger is a pushed tag `vX.Y.0`, or a manual dispatch. Patch versions (`vX.Y.Z` with Z > 0), ordinary pushes, and pull requests run nothing. This keeps hosted CI time for the points where a version is cut.
- **Platform order.** macOS is tested first: locally before every commit, then in CI. **Linux and Windows are tested after macOS** in every version run. They are not built locally, and release artifacts for them are still deferred (see PLAN.md).
- **Fail fast.** One failure stops the run, so no minutes are spent after something has already broken.

## The workflow: [version-ci.yml](../../.github/workflows/version-ci.yml)

The jobs run as a chain. Each needs the one before it, so a failure skips everything downstream:

| # | Job | Runner | What it does |
|---|---|---|---|
| 1 | Version, format, and identity checks | ubuntu-latest | Checks that the tag equals `v` + `project(VERSION …)` in [CMakeLists.txt](../../CMakeLists.txt). Then runs the clang-format 23.1.5 check and the identity-leak check over tracked files and every commit message, with values from secrets. These need no compiler, so they use the cheapest runner. |
| 2 | macOS tests (`debug`, `asan`) | macos-latest | `cmake --workflow --preset <preset>`, which includes pluginval at strictness 10 on the VST3 (not in `asan`; see [plugin-validation.md](plugin-validation.md)). The debug leg also runs clang-tidy 23.1.4. The matrix has `fail-fast: true`, so if one leg fails the other is cancelled. Uses the placeholder identity. |
| 3 | Linux and Windows tests | ubuntu-22.04, windows-latest | Starts only after both macOS legs pass. The `debug` legs include pluginval. Three legs in one fail-fast matrix: Linux GCC 12 `debug` and `asan` (under `xvfb-run`), and Windows MSVC `debug` (inside `vcvars64.bat`, located with `vswhere`). ASan isn't run on Windows. Uses the placeholder identity. |
| 4 | macOS release build | macos-latest | The `release` preset (universal binary) with the real identity from secrets. The preset sets `BRUTALIS_REQUIRE_REAL_IDENTITY=ON`, so a missing secret fails configure. Builds, tests (pluginval validates the release VST3), and uploads the VST3, AU, and Standalone outputs as artifacts. |

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

## Secrets

These are set in the repository's Actions secrets (done on 2026-11-13):

- `BRUTALIS_MANUFACTURER_CODE`
- `BRUTALIS_PLUGIN_CODE`
- `BRUTALIS_BUNDLE_ID`

The leak check and the release build use them. To change one, run `printf %s "<value>" | gh secret set <NAME>`. Never echo a value in a log.

## Platform notes

- **Linux:** built on `ubuntu-22.04` with GCC 12. 22.04 is the minimum supported Linux (decision D8). Building there links against its libstdc++ (from GCC 12), so the binaries run on 22.04 and newer. Tests run under `xvfb-run`, because the editor test creates JUCE components.
- **Windows:** the build runs inside `vcvars64.bat`, located with `vswhere`, so that Ninja finds `cl.exe`. Doing this ourselves avoids depending on a third-party action.
- **Because Linux and Windows are tested only at version time,** GCC- and MSVC-specific problems show up only then. Keep code standard C++23 with no extensions, and don't assume a library feature exists just because Apple Clang accepts it.

## History

The first CI run (run 43498239659, from commit `2b5476c`) ran on every push and covered all three platforms:

- **Linux, GCC 12:** passed as-is, for both `debug` and `asan`.
- **Windows, MSVC:** failed on warning C4459. A test constant was hidden by a JUCE template parameter with the same name. That has been fixed in the test, but MSVC hasn't checked the fix yet. The first version run will show whether it worked.

## Not done yet

| Item | Planned in |
|---|---|
| AU validation (`auval`; needs an installed AU) | M0.6 follow-up / M1.12 |
| Linux and Windows release artifacts | The Linux and Windows milestone (PLAN.md) |
| Code signing and notarization (macOS), installers | Before the first public release |
| Build caching (ccache) | When CI time becomes a problem |
