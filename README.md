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

## License

MIT; see [LICENSE](LICENSE). The JUCE and Catch2 submodules keep their own
licenses. Plugin binaries contain JUCE, so distributing them also requires
complying with JUCE's terms: the AGPLv3 for the combined work, or a commercial
JUCE license.
