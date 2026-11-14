// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <cmath>
#include <cstdint>

/// Fixed-point oscillator phase.
///
/// One cycle is 2^32 units of a std::uint32_t, so the phase wraps by itself
/// and has the same resolution everywhere in the cycle. A float phase cannot
/// do that: at 0.01 Hz and 48 kHz the increment, 2e-7 cycles, is only 3 or 4
/// float steps near t = 1, a pitch error of tens of percent. Fixed point
/// resolves 1.1e-5 Hz at 48 kHz and needs no double precision (none on a
/// Cortex-M4F). The distance from any phase to an edge at phase p is one
/// wrapping subtraction.
///
/// Doc: docs/dsp/band-limited-oscillators.md
namespace anoesis::dsp
{

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

/// A phase or an increment in cycles, [0, 1).
[[nodiscard]] constexpr float toCycles(std::uint32_t phase) noexcept
{
    return static_cast<float>(phase) * cyclesPerUnit;
}

/// The phase `degrees` into the cycle (360 per cycle), wrapped into one
/// cycle: 90 is a quarter cycle, -90 three quarters, 450 a quarter. For the
/// Phase control: degrees belong to the parameter, cycles to the oscillator.
/// Exact for every multiple of 360 / 2^k; otherwise to float precision.
/// NaN and the infinities give 0, and so does a negative angle too small to
/// tell from a whole cycle.
[[nodiscard]] inline std::uint32_t phaseFromDegrees(float degrees) noexcept
{
    float cycles = degrees / 360.0f;
    cycles -= std::floor(cycles);
    // NaN (from NaN or an infinity), or a tiny negative angle that rounded
    // up to a whole cycle: either would overflow the conversion below.
    if (std::isnan(cycles) || cycles >= 1.0f)
    {
        return 0;
    }
    return static_cast<std::uint32_t>(cycles * 4294967296.0f);
}

} // namespace anoesis::dsp
