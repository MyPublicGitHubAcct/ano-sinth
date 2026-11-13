# Project structure and dependency rules

## Layout

```
CMakeLists.txt         top-level build: options, standard, identity, subdirectories
CMakePresets.json      debug / relwithdebinfo / release / asan
cmake/                 build helpers (compiler policy, JUCE settings, identity)
external/              JUCE and Catch2 submodules (never edited)
libs/dsp/              anoesis::dsp: portable DSP, MIT, no JUCE
libs/gui/              anoesis_gui JUCE module: reusable UI components
libs/interop/          anoesis_interop JUCE module: reusable plugin glue
plugin/                the Brutalis plugin: the only product-specific code
tests/<lib>/           one Catch2 executable per library, mirroring the tree
tools/                 format, tidy, identity-leak check, git hooks
docs/                  these write-ups
```

## Dependency graph

```
                 ┌──────────────┐
                 │  plugin/     │  brutalis (VST3, AU, Standalone)
                 └──┬────┬────┬─┘
                    │    │    │
       ┌────────────┘    │    └────────────┐
       ▼                 ▼                 ▼
 ┌───────────┐   ┌───────────────┐   ┌──────────┐
 │ libs/gui  │   │ libs/interop  │   │ libs/dsp │  ◄── no dependencies at all
 └─────┬─────┘   └──────┬────────┘   └──────────┘
       │                │                 ▲
       ▼                ▼                 │ (allowed: gui/interop → dsp)
   juce_gui_basics  juce_audio_processors
```

The rules (from [CLAUDE.md](../../CLAUDE.md)):

- **`libs/dsp` depends on nothing but the C++ standard library.** It is added to the build *before* JUCE, so it cannot pick up JUCE includes by accident. It must stay portable to bare-metal ARM (STM32):
  - no heap allocation after `prepare()`;
  - no exceptions or RTTI required;
  - no OS, thread, file, or log calls;
  - `float` samples.
- **`libs/gui` and `libs/interop` may depend on JUCE and on `libs/dsp`**, but must know nothing about Brutalis's parameter set. They get what they need through configuration or interfaces.
- **`plugin/` is the only product-specific code.** It wires the libraries together.
- Dependencies point toward `dsp`, never back up, and never in a cycle.

## Namespaces and target names

| Library | C++ namespace | CMake target (alias) | Include as |
|---|---|---|---|
| dsp | `anoesis::dsp` | `anoesis_dsp` (`anoesis::dsp`) | `#include <anoesis/dsp/...>` |
| gui | `anoesis::gui` | `anoesis_gui` (`anoesis::anoesis_gui`) | `#include <anoesis_gui/anoesis_gui.hpp>` |
| interop | `anoesis::interop` | `anoesis_interop` (`anoesis::anoesis_interop`) | `#include <anoesis_interop/anoesis_interop.hpp>` |
| plugin | `brutalis` | `brutalis` (+ `brutalis_VST3`, `_AU`, `_Standalone`) | — |

The reusable libraries carry the company name (`anoesis`), not the product name, because they are meant to outlive this product and be reused in other projects.

## Why `gui` and `interop` are JUCE modules, not static libraries

**The problem.** In JUCE's CMake API, each JUCE module is an `INTERFACE` library *with sources*. Every final binary compiles the JUCE code it uses into itself. The plugin's shared-code target always compiles `juce_audio_plugin_client`, and through it most of JUCE.

If `libs/gui` were an ordinary static library linking `juce_gui_basics`, it would compile its own copy of JUCE. The plugin would then contain two copies. That causes duplicate-symbol link errors at best, and silent One Definition Rule (ODR) violations at worst. JUCE's own docs (`external/JUCE/docs/CMake API.md`, `juce_add_module`) warn about exactly this.

**The solution.** Package each JUCE-dependent library as a **JUCE module** with `juce_add_module(...)`:

- The module is a folder with a main header that holds a `BEGIN_JUCE_MODULE_DECLARATION` block, plus one root `.cpp` that `#include`s the implementation files from its subfolders. See [libs/gui/anoesis_gui/](../../libs/gui/anoesis_gui/).
- Its sources compile into whichever final binary links it, together with the JUCE modules it depends on. Each binary therefore holds exactly one copy of everything, built with one consistent set of JUCE config flags.
- Always link modules `PRIVATE`. JUCE's docs explain that `PUBLIC` causes duplicate compilation.
- Bonus: the module format is JUCE's native unit of reuse, so these libraries also drop into Projucer-based projects.

**Alternatives considered:**

- One shared static library holding all of JUCE, linked by everything. This doesn't work, because the plugin target compiles JUCE anyway (above).
- Exposing JUCE headers to static libs without sources. This is fragile: the config macros (`JUCE_WEB_BROWSER` and the like) change class layouts, so the static lib and the plugin could silently disagree, which is an ODR violation.

## How the plugin and its tests share code

[plugin/CMakeLists.txt](../../plugin/CMakeLists.txt) defines `brutalis_plugin_common`, an `INTERFACE` target that carries the plugin's sources, include paths, and library links. Two final binaries link it:

1. `brutalis`, the JUCE plugin shared-code target. The format wrappers (VST3, AU, Standalone) link this.
2. `brutalis_plugin_tests`, a JUCE console app with Catch2.

The tests therefore compile and run exactly the code the plugin ships, without loading a plugin binary or depending on the format wrappers.

JUCE config flags shared by every JUCE-compiling target are set in one place: `anoesis_configure_juce_target()` in [cmake/AnoesisJuce.cmake](../../cmake/AnoesisJuce.cmake).

## Public product info in C++

[plugin/brutalis_info.hpp.in](../../plugin/brutalis_info.hpp.in) is turned into `build/.../plugin/generated/brutalis_info.hpp` at configure time. It holds the product name, company name, and version, which are public. It must **never** hold the private codes or bundle ID. Those go only to `juce_add_plugin`, which writes them into build outputs (Info.plist, the VST3 class ID), never into tracked files.
