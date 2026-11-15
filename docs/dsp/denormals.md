# Denormals: why they hurt and how we keep them out

A synth can sit idle for a few seconds after a note ends and then use *more* CPU, sometimes ten times more, until the plugin is reset. The usual cause is **denormal numbers**. This page explains what they are and lays out the two-layer strategy `libs/dsp` uses against them.

## Theory

### What a denormal is

An IEEE 754 single-precision float stores a sign, an 8-bit exponent, and a 23-bit fraction. A *normal* float has an implicit leading 1, so it represents 1.f × 2^e. The smallest normal float is 2^-126 ≈ 1.18 × 10^-38.

Below that, IEEE 754 provides **gradual underflow**. With the exponent field at zero, the implicit 1 becomes a 0. The values 0.f × 2^-126 fill the gap down to 2^-149 ≈ 1.4 × 10^-45, losing a bit of precision at every halving. These are **denormal** (or *subnormal*) numbers. They guarantee that x − y = 0 only when x = y, which matters for numerical analysis and not at all for audio: 10^-38 is about −760 dB.

### Why they are slow

Many CPUs don't handle denormals in the fast path of the floating-point unit:

- **x86 (Intel and AMD):** an operation that produces or consumes a denormal triggers a *microcode assist* costing roughly 100+ cycles, against about 4 for a normal multiply. A filter that spends its time in denormals can run tens of times slower.
- **ARM:** Cortex-A cores, Apple Silicon, and the Cortex-M4F/M7 FPUs handle denormals in hardware, with little or no penalty in most cases. Flushing them still gives the same results on every platform.

### Where they come from in a synth

Any **recursive** structure whose input stops decays geometrically towards zero, and so passes through the denormal range. Examples:

- IIR filter states (the ladder filter, the EQ, DC blockers);
- feedback delays, comb filters, and reverb tanks;
- exponential envelope releases;
- one-pole smoothers.

The tests show something worse. A one-pole with coefficient 0.9 doesn't merely *pass through* the denormal range, it **gets stuck there**. Once the state is a few multiples of the smallest denormal, x × 0.9 rounds back to x. So it never reaches zero, and every sample after that is a denormal, indefinitely. That is how a silent plugin ends up burning CPU.

### The fixes, in general

1. **Flush-to-zero (FTZ) and denormals-are-zero (DAZ) modes.** Every mainstream FPU can be told to replace denormal results (FTZ) and inputs (DAZ) with zero. It is one register write per audio callback and costs nothing per sample. The mode is per thread, so it must be set on the audio thread, inside the callback.
2. **Flushing state explicitly.** Test recursive state against the smallest normal and zero it. It is portable and costs a compare and a select per state variable per sample.
3. **Adding a tiny offset or noise** (for example ±10^-20, alternating) to recursive state keeps it away from zero. This is common in older code. It is portable, but it adds a DC or noise floor and has to be placed carefully.
4. **`-ffast-math` and similar flags.** On some platforms these set FTZ for the whole process at startup. They also allow reassociation, contraction, and assumptions that NaN and inf never occur, all of which change results in ways tests can't pin down. Not acceptable here.

## How it is applied here

[libs/dsp/include/anoesis/dsp/core/denormals.hpp](../../libs/dsp/include/anoesis/dsp/core/denormals.hpp) provides two layers.

**Layer 1: `ScopedFlushDenormals`**, an RAII guard. Create one at the top of every audio callback. The constructor saves the FPU control register and sets the flush bits. The destructor restores the saved value, so guards nest correctly. Per target ([libs/dsp/src/denormals.cpp](../../libs/dsp/src/denormals.cpp)):

| Target | Register | Bits set |
|---|---|---|
| x86 / x86-64 (any compiler with SSE) | MXCSR via `_mm_getcsr`/`_mm_setcsr` | FTZ (bit 15) + DAZ (bit 6) |
| AArch64 (GCC, Clang: Apple Silicon, Linux ARM) | FPCR via `mrs`/`msr` | FZ (bit 24): flushes inputs and outputs |
| ARMv7 and Cortex-M4F/M7 with an FPU (GCC, Clang) | FPSCR via `vmrs`/`vmsr` | FZ (bit 24) |
| anything else (for example MSVC on ARM64) | none | `flushDenormalsSupported == false`; the guard is a no-op |

The register access is out of line, in a `.cpp`. The compiler doesn't model the FPU mode, so it could otherwise move floating-point arithmetic across an inlined mode switch. A call into another translation unit is a barrier it can't see through.

In the plugin, `PluginProcessor::processBlock` already uses JUCE's `juce::ScopedNoDenormals`, which does the same thing. `libs/dsp` can't use JUCE, so it has its own guard. The engine (M1.8) installs one at the top of its process call, so it is protected when hosted outside JUCE, in tests, and on an STM32.

**Layer 2: `flushDenormal(x)`** returns 0 when |x| is below the smallest normal float, and x unchanged otherwise (inf and NaN pass through). Recursive processors in later milestones apply it to their feedback state, once per sample or once per block. That covers targets without an FTZ control, and code that runs outside a guarded callback. Where it is cheap enough to keep in Phase 3 will be decided by measurement. Two places it was measured (M1.3): the ladder filter flushes its states and its output per sample, at no measurable cost beside the Newton solve; the `Oversampler`'s halfbands flush their outputs per sample but their states once per block, because a flush inside the serial allpass chain cost 50–57% of the round trip. A flushed output can hide subnormal states behind it, so both expose `isAtRest()` (every state exactly zero), which their tests check ([filters.md](filters.md)).

## Trade-offs and alternatives

- **FTZ alone** is the cheapest, but it depends on the caller remembering the guard and on the platform having the control. **Explicit flushing alone** is portable but costs per sample. Using both is belt and braces: the guard makes denormals impossible in the common case, and the explicit flush keeps state clean even when the guard is absent.
- **Offset or noise injection** was rejected: it adds a signal, however small, that tests then have to tolerate. Bit-exact silence after a note ends is a property worth testing.
- **FTZ changes results**, but only for values below 10^-38, where the difference is irrelevant to audio. It also makes results identical across CPUs that would otherwise handle denormals differently.

## Tests

[tests/dsp/core/denormals_test.cpp](../../tests/dsp/core/denormals_test.cpp):

- `flushDenormal` zeroes denormals of both signs and anything below the smallest normal, and leaves normals, zero, inf, and NaN unchanged;
- **the problem, demonstrated:** with no protection, a decaying one-pole produces over 1000 denormals in 2000 samples, because it gets stuck;
- with `flushDenormal` in the feedback path, it produces none;
- inside a `ScopedFlushDenormals`:
  - halving the smallest normal gives exactly 0 (FTZ);
  - a denormal input times 1 gives 0 (DAZ/FZ);
  - the decaying loop produces no denormals;
  - after the scope ends, IEEE behaviour is back;
- nested guards restore the mode they found: the inner guard restores "flushing", not "off".

The guard tests `SKIP` on targets without a flush-to-zero control. They run on macOS arm64 locally, and the x86-64 path is compiled as part of the universal release build. Linux (GCC) and Windows (MSVC) run them in Version CI. The [CPU baselines](../performance/baselines.md) measure the cost: on Apple Silicon a denormal-bound loop runs exactly as fast as a normal one (about 2.0 ns per sample either way), as the theory predicts for ARM. The x86 penalty, the main reason for the guard, is still to be measured on an x86 machine.

## References

- IEEE 754-2019, *Standard for Floating-Point Arithmetic*, §3.4: subnormal numbers.
- Intel, *64 and IA-32 Architectures Optimization Reference Manual*: "Floating-point modes and exceptions" (FTZ, DAZ, and microcode assists).
- Arm, *Arm Architecture Reference Manual for A-profile*: FPCR.FZ; *Cortex-M4 Technical Reference Manual*: FPSCR.FZ.
- Laurent de Soras, "Denormal numbers in floating point signal processing applications" (2005).
