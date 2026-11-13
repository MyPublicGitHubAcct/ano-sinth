// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

/// Denormal protection. Two layers (docs/dsp/denormals.md):
///
///   1. ScopedFlushDenormals, at the top of each audio callback: switches the
///      FPU to flush-to-zero for the duration of the block, so no arithmetic
///      inside produces or consumes denormals. Costs nothing per sample.
///   2. flushDenormal(x), on the state of recursive structures (filter and
///      envelope memories, delay feedback): a portable fallback for targets
///      without a flush-to-zero control, and for code that might run outside
///      a guarded scope.
namespace anoesis::dsp
{

namespace detail
{

/// Which register controls flush-to-zero on this target.
enum class FpuControl : std::uint8_t
{
    none,       ///< No supported control: flushDenormal() is the only protection.
    x86Mxcsr,   ///< SSE MXCSR: FTZ (bit 15) and DAZ (bit 6).
    armFpcr64,  ///< AArch64 FPCR: FZ (bit 24) flushes inputs and outputs.
    armFpscr32, ///< ARMv7-A/R and Cortex-M with an FPU: FPSCR.FZ (bit 24).
};

// Keep these conditions in sync with src/denormals.cpp, which static_asserts
// that they agree.
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
inline constexpr FpuControl fpuControl = FpuControl::x86Mxcsr;
#elif defined(__aarch64__) && defined(__GNUC__)
inline constexpr FpuControl fpuControl = FpuControl::armFpcr64;
#elif defined(__arm__) && defined(__ARM_FP) && defined(__GNUC__)
inline constexpr FpuControl fpuControl = FpuControl::armFpscr32;
#else
inline constexpr FpuControl fpuControl = FpuControl::none;
#endif

} // namespace detail

/// True if ScopedFlushDenormals can switch this target's FPU to
/// flush-to-zero. False (for example on MSVC for ARM64), the guard does
/// nothing and flushDenormal() is the only protection.
inline constexpr bool flushDenormalsSupported = detail::fpuControl != detail::FpuControl::none;

/// While alive, puts the current thread's FPU in flush-to-zero mode: denormal
/// results become zero and denormal inputs are read as zero. Restores the
/// previous mode on destruction, so guards nest. Create one at the top of
/// every audio callback, on the audio thread. The mode is per thread.
///
/// The switch happens in an out-of-line function (src/denormals.cpp), which
/// the compiler cannot move arithmetic across.
class ScopedFlushDenormals
{
public:
    ScopedFlushDenormals() noexcept;
    ~ScopedFlushDenormals() noexcept;

    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    std::uint64_t savedState;
};

/// Returns 0 for a denormal (and for anything smaller in magnitude than the
/// smallest normal float), and x unchanged otherwise, including inf and NaN.
/// Use it on feedback state that decays towards zero.
[[nodiscard]] inline float flushDenormal(float x) noexcept
{
    return std::fabs(x) < std::numeric_limits<float>::min() ? 0.0f : x;
}

} // namespace anoesis::dsp
