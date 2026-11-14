// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <cstdint>

/// The oscillator waveforms and their naive (aliasing) forms, which a
/// band-limiting method corrects at their jumps and bends.
///
/// Three so far: a jump (saw), two jumps whose spacing varies (pulse), and
/// two bends (triangle). The next M1.1 task extends this to the 8 Sylenth1
/// waveforms. Doc: docs/dsp/band-limited-oscillators.md
namespace anoesis::dsp
{

enum class Waveform : std::uint8_t
{
    saw,      ///< rising: 2 t - 1
    pulse,    ///< +1 for t < width, -1 after
    triangle, ///< -1 at t = 0, +1 at t = 1/2
};

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

} // namespace anoesis::dsp
