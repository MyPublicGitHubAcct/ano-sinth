// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/math/pan.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <string_view>

/// The unison laws: where each of an oscillator's unison voices sits in pitch
/// and in the stereo field, and how the oscillator's Pan moves them. Data and
/// pure functions; OscillatorSlot applies them.
///
/// Every law here is a working hypothesis, to be measured against the demo in
/// M1.13 (inventory §2, O8 and O13-O16). Each is one table or one function,
/// so a measurement that disagrees changes that, not the slot. The knob laws
/// (Sylenth1's Detune knob to semitones, which also depends on the voice
/// count, and the Stereo knob to a width) belong to the parameter table.
///
/// Doc: docs/dsp/oscillator-unison.md
namespace anoesis::dsp
{

/// Sylenth1's Voices control: 0 (off) to 8 unison voices per oscillator.
inline constexpr std::size_t maxUnisonVoices = 8;

/// Where the voices of an N-voice unison sit, listed from the lowest pitch to
/// the highest; entries from N on are 0.
struct UnisonLayout
{
    /// Each voice's detune as a fraction of the spread, in [-1, 1]: evenly
    /// spaced in semitones, the outermost voices at -1 and +1, a centre voice
    /// (odd N) at exactly 0. Antisymmetric: voice k is -(voice N-1-k).
    std::array<float, maxUnisonVoices> detune{};
    /// Each voice's side at full stereo width: -1 left, 0 centre, +1 right.
    std::array<float, maxUnisonVoices> side{};
};

namespace unison_detail
{

// The orders reported at Stereo 10 (BlitBit, KVR thread 518035, 2019),
// read from the lowest voice to the highest. Each pair of voices at -d and +d
// is split left and right, and the lowest voice is always left.
inline constexpr std::array<std::string_view, maxUnisonVoices + 1> sideOrders{
    "", "C", "LR", "LCR", "LRLR", "LRCLR", "LRRLLR", "LRLCRLR", "LRRLRLLR",
};

constexpr float sideOf(char side) noexcept
{
    if (side == 'L')
    {
        return -1.0f;
    }
    return side == 'R' ? 1.0f : 0.0f;
}

constexpr UnisonLayout makeLayout(std::size_t count) noexcept
{
    UnisonLayout layout;
    for (std::size_t k = 0; k < count; ++k)
    {
        // (2k - (N - 1)) / (N - 1): the numerators of k and N-1-k are exact
        // negatives, so the quotients are too.
        if (count > 1)
        {
            const auto numerator = static_cast<float>((2 * static_cast<int>(k)) - static_cast<int>(count - 1));
            layout.detune[k] = numerator / static_cast<float>(count - 1);
        }
        layout.side[k] = sideOf(sideOrders[count][k]);
    }
    return layout;
}

constexpr std::array<UnisonLayout, maxUnisonVoices + 1> makeLayouts() noexcept
{
    std::array<UnisonLayout, maxUnisonVoices + 1> layouts{};
    for (std::size_t count = 0; count <= maxUnisonVoices; ++count)
    {
        layouts[count] = makeLayout(count);
    }
    return layouts;
}

} // namespace unison_detail

/// The layouts for 0 to 8 voices.
inline constexpr std::array<UnisonLayout, maxUnisonVoices + 1> unisonLayouts = unison_detail::makeLayouts();

/// The layout for `count` voices (clamped to 8).
[[nodiscard]] constexpr const UnisonLayout& unisonLayout(std::size_t count) noexcept
{
    return unisonLayouts[std::min(count, maxUnisonVoices)];
}

/// A voice's pan position: the oscillator's Pan plus the voice's side scaled
/// by the stereo width, clipped to [-1, 1] (decision O8: an offset). The
/// width is clamped to [0, 1]; NaN counts as 0, and a NaN pan as the centre.
[[nodiscard]] inline float unisonPosition(float pan, float width, float side) noexcept
{
    const float centre = pan > 0.0f || pan < 0.0f ? pan : 0.0f;
    const float spread = width > 0.0f ? std::min(width, 1.0f) : 0.0f;
    return std::clamp(centre + (spread * side), -1.0f, 1.0f);
}

/// How two independent, equally loud noise sources n1 and n2 are mixed into
/// a stereo pair:
///
///   left  = this.left  x n1 + this.cross x n2
///   right = this.cross x n1 + this.right x n2
struct NoiseMix
{
    float left = 0.0f;
    float cross = 0.0f;
    float right = 0.0f;
};

/// The mix that gives two noise sources the power and the left/right
/// correlation of independent noise voices, one per entry of `voices`, each
/// added into the bus with that entry's gains. The voices' covariance is
///
///   C = [ sum(l^2)  sum(l r) ]
///       [ sum(l r)  sum(r^2) ]
///
/// and the mix is its symmetric square root S (S^2 = C), so both channels are
/// mixes of both sources. For a 2x2 matrix, S = (C + sqrt(det C) I) /
/// sqrt(trace C + 2 sqrt(det C)). `inverted` negates the mix (Inv).
///
/// Call it when a control changes, not per sample (it takes square roots).
[[nodiscard]] inline NoiseMix unisonNoiseMix(std::span<const StereoGains> voices, bool inverted) noexcept
{
    float left = 0.0f;
    float right = 0.0f;
    float cross = 0.0f;
    for (const auto& gains : voices)
    {
        left += gains.left * gains.left;
        right += gains.right * gains.right;
        cross += gains.left * gains.right;
    }
    // det C >= 0 in exact arithmetic (Cauchy-Schwarz); rounding can take it
    // just below. Only s^2 = det enters S^2, so an error in s cancels.
    const float s = std::sqrt(std::max(0.0f, (left * right) - (cross * cross)));
    const float t = std::sqrt(left + right + (2.0f * s));
    if (!(t > 0.0f))
    {
        return {};
    }
    const float sign = inverted ? -1.0f : 1.0f;
    return {.left = sign * (left + s) / t, .cross = sign * cross / t, .right = sign * (right + s) / t};
}

} // namespace anoesis::dsp
