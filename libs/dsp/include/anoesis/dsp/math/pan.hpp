// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

/// Pan laws: how a mono signal's level is split between left and right as
/// it moves across the stereo field.
///
/// A position runs from -1 (hard left) through 0 (centre) to +1 (hard
/// right). The laws differ in what they hold constant, which shows at the
/// centre:
///
///   equalPower  sin/cos: L^2 + R^2 = 1 everywhere, so a source keeps its
///               loudness in stereo as it moves. -3 dB per side at the
///               centre. Summed to mono, the centre is +3 dB louder than
///               the sides.
///   linear      L + R = 1: constant in mono, but -6 dB per side at the
///               centre, so a centred source dips in stereo.
///   balance     unity at the centre; moving away only fades the far side,
///               as a stereo balance control does.
///
/// Doc: docs/dsp/oscillator-controls.md
namespace anoesis::dsp
{

enum class PanLaw : std::uint8_t
{
    equalPower, ///< sin/cos, -3 dB at the centre (the default)
    linear,     ///< -6 dB at the centre
    balance,    ///< 0 dB at the centre
};

/// A pair of channel gains.
struct StereoGains
{
    float left = 0.0f;
    float right = 0.0f;
};

/// The left and right gains for `position` in [-1, 1] under `law`. Positions
/// outside the range are clamped, and NaN counts as the centre. The hard
/// sides are exact (1 and 0), and the result mirrors exactly: the left gain
/// at p is the right gain at -p.
///
/// The equal-power law calls std::sin, so compute gains when a position
/// changes, not per sample.
[[nodiscard]] inline StereoGains panGains(float position, PanLaw law = PanLaw::equalPower) noexcept
{
    const float p = position > 0.0f || position < 0.0f ? std::clamp(position, -1.0f, 1.0f) : 0.0f; // NaN -> 0
    // Distances from each hard side, in [0, 2]: 1 - p and 1 + p are exactly
    // mirror images, so the two channels are computed the same way.
    const float towardLeft = 1.0f - p;
    const float towardRight = 1.0f + p;
    switch (law)
    {
    case PanLaw::linear:
        return {.left = towardLeft * 0.5f, .right = towardRight * 0.5f};
    case PanLaw::balance:
        return {.left = std::min(1.0f, towardLeft), .right = std::min(1.0f, towardRight)};
    case PanLaw::equalPower:
        break;
    }
    // Equal power (and any out-of-range law value). cos((p + 1) pi / 4) =
    // sin((1 - p) pi / 4): written as sines of the distances, a hard side
    // gives sin(0) = 0 exactly (cos(pi / 2) in float is -4.4e-8, not 0).
    constexpr float quarterPi = std::numbers::pi_v<float> / 4.0f;
    return {.left = std::sin(towardLeft * quarterPi), .right = std::sin(towardRight * quarterPi)};
}

} // namespace anoesis::dsp
