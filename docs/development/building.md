# Building and toolchains

## Requirements

> Only **macOS** is built locally. Linux and Windows are built and tested in Version CI, after macOS, on every major or minor version ([ci.md](ci.md)). The Linux and Windows instructions below match what CI does.

| | macOS | Linux | Windows |
|---|---|---|---|
| Compiler | Apple Clang (Xcode or its command-line tools) | GCC 12 or newer | MSVC (Visual Studio 2022 or newer) |
| Build tools | CMake ≥ 3.25, Ninja | CMake ≥ 3.25, Ninja | CMake ≥ 3.25, Ninja (both ship with Visual Studio) |
| Extra | — | JUCE's system packages (see below) | Run from a **Developer** prompt, so Ninja can find `cl.exe` |
| Optional | clang-format and clang-tidy 23 (`brew install llvm`) | `pip install clang-format==23.1.5 clang-tidy==23.1.4` | same as Linux |

Each platform builds with its standard compiler only. Configure fails if CMake picks anything else (for example Homebrew Clang on macOS). You can override this with `-DBRUTALIS_ALLOW_ANY_COMPILER=ON` for an experiment, but never for a commit or in CI. Logic: [cmake/AnoesisCompiler.cmake](../../cmake/AnoesisCompiler.cmake).

Linux packages, on Ubuntu 22.04 or 24.04 (JUCE's list is in `external/JUCE/docs/Linux Dependencies.md`):

```sh
sudo apt install g++-12 ninja-build libasound2-dev libjack-jackd2-dev \
  libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev \
  libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libxi-dev \
  libegl-dev libglu1-mesa-dev mesa-common-dev
```

## Getting the source

JUCE and Catch2 are git submodules, pinned to release tags:

```sh
git clone --recursive <repo-url>
# or, in an existing clone:
git submodule update --init --recursive
```

| Submodule | Path | Pinned tag |
|---|---|---|
| JUCE | `external/JUCE` | `9.0.5` |
| Catch2 | `external/Catch2` | `v3.16.1` |

To upgrade one, run `git -C external/JUCE fetch --tags && git -C external/JUCE checkout <tag>`. Then rebuild, run every test, read JUCE's `BREAKING_CHANGES.md`, and commit the new pin with the PLAN.md decision log updated. Never track a branch.

## Presets

All configurations live in [CMakePresets.json](../../CMakePresets.json). Every preset uses Ninja and builds into `build/<preset>/`.

| Preset | Build type | Use it for |
|---|---|---|
| `debug` | Debug | Day-to-day development and CI. |
| `relwithdebinfo` | RelWithDebInfo | Profiling and benchmarks. Placeholder identity allowed. |
| `release` | Release | Shippable builds. Universal binary (arm64 + x86_64) on macOS. **Requires the real identity** (see [product-identity.md](product-identity.md)). |
| `asan` | Debug + ASan/UBSan | Memory and undefined-behavior checking. macOS and Linux only. |

Each name works as a configure, build, test, and **workflow** preset:

```sh
cmake --workflow --preset debug   # configure + build + test in one command

# or step by step:
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

# one test executable, or one test case:
./build/debug/tests/dsp/anoesis_dsp_tests
ctest --preset debug -R "processor renders silence"
```

Build outputs:

- the plugin formats are in `build/<preset>/plugin/brutalis_artefacts/<Config>/{VST3,AU,Standalone}`;
- the tests are in `build/<preset>/tests/<lib>/`.

Plugins are not copied into the system plugin folders automatically (`COPY_PLUGIN_AFTER_BUILD FALSE`). Copy them by hand when testing in a DAW.

### CMake options

| Option | Default | Effect |
|---|---|---|
| `BRUTALIS_WARNINGS_AS_ERRORS` | ON | Turns on `-Werror` or `/WX` for project targets. |
| `BRUTALIS_SANITIZERS` | OFF | ASan + UBSan across the whole build (the `asan` preset). |
| `BRUTALIS_BUILD_TESTS` | ON | Builds the Catch2 targets. |
| `BRUTALIS_BUILD_PLUGIN` | ON | Builds JUCE, `libs/gui`, `libs/interop`, and the plugin. Turn it off to build only `libs/dsp` and its tests: fast, with no JUCE involved. |
| `BRUTALIS_REQUIRE_REAL_IDENTITY` | OFF (ON in `release`) | Fails configure if a placeholder identity value is in use. |
| `BRUTALIS_ALLOW_ANY_COMPILER` | OFF | Turns the standard-compiler check into a warning. |
| `BRUTALIS_PLUGINVAL` | ON | Validates the built VST3 with pluginval as a CTest test. The pinned binary is downloaded on the first configure; see [plugin-validation.md](plugin-validation.md). |
| `BRUTALIS_PLUGINVAL_EXECUTABLE` | empty | Uses a local pluginval binary instead of downloading one (offline builds). |

## Language standard

The project uses **C++23** with compiler extensions off (`CMAKE_CXX_STANDARD 23`, `CMAKE_CXX_EXTENSIONS OFF`). This was confirmed on 2026-11-13:

- JUCE 9.0.5 needs C++17 or newer and builds cleanly as C++23 with Apple Clang 21.
- Catch2 v3.16.1 needs C++14 or newer.
- All three target compilers accept `-std=c++23` or its MSVC equivalent.

The *language* level is C++23 everywhere. *Library* support differs by platform:

- On Linux, the Ubuntu 22.04 baseline means GCC 12's libstdc++.
- On macOS, a 12.0 deployment target means some newer libc++ features are unavailable at run time, and the compiler reports them as availability errors.

Linux and Windows are compiled only in Version CI, so "compiles on my Mac" proves nothing about GCC 12 or MSVC until then. Avoid newer library features unless you know libstdc++ 12 and MSVC support them. Raise the standard when JUCE, Catch2, and all three compilers support the next one.

## Platform baselines (decision D8)

| Platform | Minimum | Where it is set |
|---|---|---|
| macOS | 12.0 (Monterey); arm64 + x86_64 in release | `CMAKE_OSX_DEPLOYMENT_TARGET` in [CMakeLists.txt](../../CMakeLists.txt); architectures in the `release` preset |
| Windows | 10 (x64) | JUCE's default target; MSVC x64 developer environment |
| Linux | Ubuntu 22.04 (glibc 2.35, libstdc++ from GCC 12) | Version CI builds on `ubuntu-22.04` with GCC 12 |

Trade-off: macOS 12 still covers older Intel Macs that are common in studios, and it adds no availability workarounds for the language features we use. Raising the baseline later is easy. Lowering it after release is not.
