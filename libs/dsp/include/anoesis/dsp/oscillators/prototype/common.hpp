// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

/// Shared pieces of the anti-aliasing prototypes (decision D6, M1.1).
///
/// The prototypes live in anoesis::dsp::prototype so that they stay out of
/// the oscillator API until D6 is decided. Each one offers the same
/// interface (prepare, reset, setShape, setPulseWidth, setFrequency,
/// setIncrement, next, latencySamples) so the tests and benchmarks can
/// compare them like for like. Doc: docs/dsp/band-limited-oscillators.md
namespace anoesis::dsp::prototype
{

/// The three shapes every method must produce: a jump (saw), two jumps whose
/// spacing varies (pulse), and two bends (triangle).
enum class Shape : std::uint8_t
{
    saw,      ///< rising: 2 t - 1
    pulse,    ///< +1 for t < width, -1 after
    triangle, ///< -1 at t = 0, +1 at t = 1/2
};

/// Phase is fixed point: one cycle is 2^32 units, so it wraps by itself and
/// keeps the same resolution everywhere in the cycle. A float phase cannot:
/// at 0.01 Hz and 48 kHz the increment, 2e-7 cycles, is only 3 or 4 float
/// steps near t = 1, a pitch error of tens of percent. Fixed point resolves
/// 1.1e-5 Hz at 48 kHz, and needs no double precision (none on Cortex-M4F).
inline constexpr float cyclesPerUnit = 1.0f / 4294967296.0f;
inline constexpr std::uint32_t halfCycle = 0x80000000U;

/// The increment for `frequencyHz` at `sampleRate`, clamped to [0, Nyquist].
/// NaN gives 0.
[[nodiscard]] inline std::uint32_t incrementFor(float frequencyHz, float sampleRate) noexcept
{
    const float cycles = frequencyHz / sampleRate;
    if (!(cycles > 0.0f))
    {
        return 0;
    }
    if (cycles >= 0.5f)
    {
        return halfCycle;
    }
    return static_cast<std::uint32_t>(cycles * 4294967296.0f);
}

/// A phase or increment in cycles, [0, 1).
[[nodiscard]] inline float toCycles(std::uint32_t phase) noexcept
{
    return static_cast<float>(phase) * cyclesPerUnit;
}

/// The phase of a pulse width in (0, 1), kept one part in 2^16 away from
/// the ends so the two edges never coincide. NaN gives the narrowest width.
[[nodiscard]] inline std::uint32_t widthToPhase(float width) noexcept
{
    constexpr float lowest = 1.0f / 65536.0f;
    const float w = width > lowest ? std::min(width, 1.0f - lowest) : lowest;
    return static_cast<std::uint32_t>(w * 4294967296.0f);
}

/// The naive (aliasing) waveforms that each method corrects.
[[nodiscard]] inline float naiveSaw(float t) noexcept
{
    return (2.0f * t) - 1.0f;
}

[[nodiscard]] inline float naivePulse(std::uint32_t phase, std::uint32_t widthPhase) noexcept
{
    return phase < widthPhase ? 1.0f : -1.0f;
}

[[nodiscard]] inline float naiveTriangle(float t) noexcept
{
    return t < 0.5f ? (4.0f * t) - 1.0f : 3.0f - (4.0f * t);
}

/// ceil(log2(x)) for a finite x > 1, from the float's bits: exact at powers
/// of two, and no library call.
[[nodiscard]] inline int ceilLog2(float x) noexcept
{
    const auto bits = std::bit_cast<std::uint32_t>(x);
    return static_cast<int>((bits - 1U) >> 23U) - 126;
}

} // namespace anoesis::dsp::prototype
