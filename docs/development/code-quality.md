# Code quality: warnings, formatting, static analysis

Three tools enforce the rules automatically. All three must be clean before a task counts as done (PLAN.md, Definition of done).

## Compiler warnings, as errors

[cmake/AnoesisCompiler.cmake](../../cmake/AnoesisCompiler.cmake) (`anoesis_set_warnings`) applies these flags:

| Compiler | Flags |
|---|---|
| Apple Clang and GCC | `-Wall -Wextra -Wpedantic -Wfloat-equal -Wconversion -Wsign-conversion -Wshadow -Werror` |
| MSVC | `/W4 /WX /permissive-` (`/W4` already covers the conversion and shadowing warnings) |
| `libs/dsp` only (Clang and GCC) | `-Wdouble-promotion` as well |

The numeric warnings (`-Wfloat-equal`, `-Wconversion`, `-Wsign-conversion`) matter most in DSP code, where a silent `double` → `float` or `float` → `int` narrowing is a real bug. `-Wdouble-promotion` applies to `libs/dsp` alone: on a Cortex-M4F, which has a single-precision FPU, every implicit `float` → `double` promotion becomes a slow software operation. Tests are exempt because Catch2's matchers take `double`. [libs/dsp/src/public_headers.cpp](../../libs/dsp/src/public_headers.cpp) includes every public header, so header-only code is compiled under the library's flags too. Add each new public header there.

Targets that compile JUCE also get `juce::juce_recommended_warning_flags`. These add, among others, `-Wshadow-all` and further checks on top of the list above.

The flags are set **per target, never globally**, so JUCE's and Catch2's own internal targets keep their own settings. JUCE module *sources* compiled into our targets do get our flags, and as of JUCE 9.0.5 they compile cleanly under them with Apple Clang. If a JUCE upgrade breaks this, report it upstream and contain the problem. Don't weaken the flags for our own code.

## Formatting: clang-format 23

- Config: [.clang-format](../../.clang-format). The style is LLVM-based with 4-space indents, Allman braces, and 120 columns.
- `tools/format.sh` formats every project file in place. `tools/format.sh --check` only reports, and exits 1 if anything needs formatting.
- Only `libs/`, `plugin/`, and `tests/` are formatted. `external/` never is.
- **The version is pinned to 23.x.** Different clang-format majors format the same code differently, so a mismatch would make CI and laptops disagree. The script refuses any other major. CI installs exactly `clang-format==23.1.5` from PyPI, and Homebrew's `llvm` gives the same version on macOS.

## Static analysis: clang-tidy 23

- Config: [.clang-tidy](../../.clang-tidy). It is based on the C++ Core Guidelines (`cppcoreguidelines-*`) plus `bugprone`, `clang-analyzer`, `modernize`, `performance`, `portability`, `readability`, and `misc`. Every finding is an error.
- Each disabled check has its reason written in the config. Two folders narrow it further:
  - [tests/.clang-tidy](../../tests/.clang-tidy) turns off two checks that fire only inside Catch2's macros.
  - `libs/{gui,interop}/.clang-tidy` allow the `#include "x.cpp"` pattern that the JUCE module format requires.
- Naming is enforced through `readability-identifier-naming`:
  - types: `CamelCase`
  - functions, variables, and members: `camelBack`
  - namespaces: `lower_case`
  - files: `snake_case.hpp/.cpp`

  This matches JUCE's naming, so plugin code reads consistently.
- Run it after a build. Some include paths are generated during the build.

  ```sh
  cmake --workflow --preset debug
  tools/tidy.sh build/debug
  ```

- To suppress a single finding, use `// NOLINT(check-name)` and give the reason on the same line or the line above. For example, JUCE's `createEditor()` must return an owning raw pointer: [plugin/source/plugin_processor.cpp](../../plugin/source/plugin_processor.cpp).

## Git hooks

Run `tools/install-hooks.sh` once per clone. It points `core.hooksPath` at [tools/hooks/](../../tools/hooks/):

- **pre-commit:** blocks staged changes that contain a private identity value, and staged C++ files that aren't formatted.
- **commit-msg:** blocks commit messages that contain a private identity value.

The hooks are fast and cover only formatting and identity leaks. Before committing, run `tools/check.sh`: it builds, tests, and tidies too. Version CI repeats the same checks on `vX.Y.0` tags.
