# Brutalis documentation

Each project task and technique gets a write-up here (Part 4 of [PLAN.md](../PLAN.md)). Each write-up covers:

- the general theory;
- how it is applied in this codebase, with file links;
- the trade-offs and the alternatives considered;
- the tests that pin the behavior down;
- references.

Docs change in the same commit as the code they describe.

## Suggested reading order

### 1. Working on the project

Read these first, in this order.

1. [Building and toolchains](development/building.md): requirements, presets, the C++ standard, and platform baselines.
2. [Project structure and dependency rules](development/project-structure.md): the libraries, why `gui` and `interop` are JUCE modules, and what may depend on what.
3. [Test-driven development in this repo](development/testing.md): the red-green-refactor loop, with Catch2 and CTest.
4. [Code quality: warnings, formatting, static analysis](development/code-quality.md)
5. [Product identity](development/product-identity.md): how the private plugin codes stay out of the repository.
6. [Plugin validation with pluginval](development/plugin-validation.md)
7. [Continuous integration](development/ci.md)

### 2. DSP theory and its implementation

This section fills up during Phase 1 (oscillators, filters, envelopes, effects, and so on). Read the foundations in this order; later topics build on them.

**Foundations (M1.0)**

1. [The processor lifecycle: prepare, reset, process](dsp/processor-lifecycle.md): real-time rules, `ProcessSpec`, `AudioBlock`, and the contract every processor is tested against.

### 3. Reference material

- [Sylenth1 feature inventory](reference/sylenth1-features.md): the Phase 1 feature target.
