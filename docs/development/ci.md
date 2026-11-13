# Continuous integration

CI runs on GitHub Actions (decision D4). The workflows are in [.github/workflows/](../../.github/workflows/).

> Status (2026-11-13): the workflows are written but have **not run yet**. The repository has no GitHub remote. Expect a round of fixes on the first push.

## `ci.yml`: every push to `main` and every pull request

| Job | Runner | What it does |
|---|---|---|
| Format check | ubuntu-latest | `pip install clang-format==23.1.5`, then `tools/format.sh --check` |
| Identity leak check | ubuntu-latest | `tools/check-identity-leak.sh` over tracked files and the full commit history, with values from secrets. If the secrets are absent (as on forks), it skips. |
| macOS | macos-latest | Apple Clang: `cmake --workflow --preset debug` and `asan`. Then clang-tidy 23.1.4 on the debug build. |
| Linux | ubuntu-22.04 | GCC 12: `debug` and `asan`, run under `xvfb-run` because the editor test creates JUCE components |
| Windows | windows-latest | MSVC: `debug`. It runs inside `vcvars64.bat` so that Ninja finds `cl.exe`. |

All builds use the **placeholder** identity.

Why Linux uses `ubuntu-22.04` with GCC 12: 22.04 is the minimum supported Linux (decision D8). Building there links against its libstdc++ (from GCC 12), so the binaries run on 22.04 and newer.

Why the Windows step locates Visual Studio with `vswhere` itself: it avoids depending on a third-party action to set up the MSVC environment.

## `release.yml`: version tags (`v*`) and manual runs

1. Runs the identity leak check first.
2. Configures with the `release` preset, passing the real identity from secrets as `-D` values. The preset sets `BRUTALIS_REQUIRE_REAL_IDENTITY=ON`, so a missing secret fails the build.
3. Builds and tests on all three platforms. macOS builds a universal binary.
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
