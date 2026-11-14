# Brutalis

A virtual analog synthesizer plugin (VST3, AU, Standalone) by **Anoesis Audio**,
for macOS, Linux, and Windows. Built with C++23, JUCE 9, and Catch2.

Status: **Phase 0, foundation.** The plugin builds and loads but makes no sound
yet. See [PLAN.md](PLAN.md) for the roadmap.

## Quick start

```sh
git clone --recursive <repo-url>
cd ano-sinth
cmake --workflow --preset debug     # configure, build, and run all tests
```

Requirements and per-platform setup: [docs/development/building.md](docs/development/building.md).

## Repository layout

| Path | What it is |
|---|---|
| `libs/dsp/` | Portable DSP library (MIT). No JUCE; also meant to build for embedded ARM. |
| `libs/gui/` | Reusable JUCE UI components, packaged as a JUCE module. |
| `libs/interop/` | Reusable JUCE glue (parameters, presets, MIDI learn), packaged as a JUCE module. |
| `plugin/` | The Brutalis plugin itself: the only product-specific code. |
| `tests/` | Catch2 tests, one executable per library. |
| `docs/` | Write-ups of every technique used. Start at [docs/README.md](docs/README.md). |
| `tools/` | Format, lint, identity-leak check, and git hooks. |

## Contributing

Test-driven development is required, and warnings are errors. Read
[docs/development/testing.md](docs/development/testing.md) and run
`tools/install-hooks.sh` once after cloning.

## Running the checks

Locally (macOS), before every commit:

```sh
tools/check.sh           # debug build + tests, clang-tidy, format, identity and publish checks
tools/check.sh --asan    # the same, plus the ASan/UBSan build and tests
```

Linux and Windows are not built locally. They run in the Version CI workflow on
GitHub, which a `vX.Y.0` tag starts with every job. To run only some platforms,
dispatch it by hand. Every input defaults to `true`, so pass `false` for each
job to skip:

```sh
gh workflow run version-ci.yml                                                    # everything
gh workflow run version-ci.yml -f windows=false -f macos=false -f release=false   # Linux only
gh workflow run version-ci.yml -f linux=false -f macos=false -f release=false     # Windows only
gh workflow run version-ci.yml -f linux=false -f windows=false -f release=false   # macOS only
gh workflow run version-ci.yml -f macos=false -f linux=false -f windows=false     # macOS release build only
gh workflow run version-ci.yml -f asan=false                                      # skip the ASan legs (macOS, Linux)
```

The selected jobs still run in order (Linux → Windows → macOS → release), and a
failure stops the rest. Details: [docs/development/ci.md](docs/development/ci.md).

## License

MIT; see [LICENSE](LICENSE). The JUCE and Catch2 submodules keep their own
licenses. Plugin binaries contain JUCE, so distributing them also requires
complying with JUCE's terms: the AGPLv3 for the combined work, or a commercial
JUCE license.
