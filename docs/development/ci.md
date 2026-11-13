# Continuous integration

CI runs on GitHub Actions (decision D4). The workflows are in [.github/workflows/](../../.github/workflows/).

> Status (2026-11-13): CI runs on GitHub. It is **macOS-only** for now, because Linux and Windows builds are deferred to a later phase (see PLAN.md). Draft jobs for both platforms are in commit `2b5476c`, and the reasoning behind them is kept below so they can be restored.

## `ci.yml`: every push to `main` and every pull request

| Job | Runner | What it does |
|---|---|---|
| Format check | ubuntu-latest | `pip install clang-format==23.1.5`, then `tools/format.sh --check`. This needs no compiler, so it runs on Linux even while Linux builds are deferred. |
| Identity leak check | ubuntu-latest | `tools/check-identity-leak.sh` over tracked files and the full commit history, with values from secrets. If the secrets are absent (as on forks, or before they are set), it skips. |
| macOS | macos-latest | Apple Clang: `cmake --workflow --preset debug` and `asan`. Then clang-tidy 23.1.4 on the debug build. |

All builds use the **placeholder** identity.

### When Linux and Windows return

On the one run that included them (before the deferral), Linux passed. Windows failed on MSVC warning C4459. A test constant shadowed a JUCE template parameter. That has been fixed in the test, but MSVC hasn't verified the fix yet.

- **Linux:** run on `ubuntu-22.04` with GCC 12. 22.04 is the minimum supported Linux (decision D8). Building there links against its libstdc++ (from GCC 12), so the binaries run on 22.04 and newer. Run the tests under `xvfb-run`, because the editor test creates JUCE components.
- **Windows:** run inside `vcvars64.bat`, located with `vswhere`, so that Ninja finds `cl.exe`. Doing this ourselves avoids depending on a third-party action.

## `release.yml`: version tags (`v*`) and manual runs

1. Runs the identity leak check first.
2. Configures with the `release` preset, passing the real identity from secrets as `-D` values. The preset sets `BRUTALIS_REQUIRE_REAL_IDENTITY=ON`, so a missing secret fails the build.
3. Builds and tests on macOS as a universal binary. Linux and Windows are deferred.
4. Uploads the VST3, AU, and Standalone outputs as workflow artifacts.

These secrets must be set in the repository settings before the first release:

- `BRUTALIS_MANUFACTURER_CODE`
- `BRUTALIS_PLUGIN_CODE`
- `BRUTALIS_BUNDLE_ID`

The ordinary CI leak check uses the same three secrets.

## Not done yet

| Item | Planned in |
|---|---|
| Plugin validation with pluginval, plus `auval` on macOS | M0.6 follow-up / M1.12 |
| Code signing and notarization (macOS), signing (Windows), installers | Before the first public release |
| Build caching (ccache / sccache) | When CI time becomes a problem |
