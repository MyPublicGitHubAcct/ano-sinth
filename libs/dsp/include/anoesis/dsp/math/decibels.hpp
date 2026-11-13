// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <algorithm>
#include <cmath>

namespace anoesis::dsp
{

/// The level treated as silence by default: -100 dB, an amplitude of 1e-5,
/// is below the noise floor of any real playback chain.
inline constexpr float defaultSilenceDb = -100.0f;

/// Linear amplitude gain for a level in decibels: 10^(dB / 20).
/// Levels at or below `silenceDb` (and NaN) give exactly 0, so a fader at its
/// bottom is truly silent rather than -100 dB.
[[nodiscard]] inline float decibelsToGain(float decibels, float silenceDb = defaultSilenceDb) noexcept
{
    return decibels > silenceDb ? std::pow(10.0f, decibels * 0.05f) : 0.0f;
}

/// Level in decibels for a linear amplitude gain: 20 * log10(gain). Gains
/// that are zero, negative, or quieter than `silenceDb` give `silenceDb`, so
/// the result is always finite.
[[nodiscard]] inline float gainToDecibels(float gain, float silenceDb = defaultSilenceDb) noexcept
{
    return gain > 0.0f ? std::max(silenceDb, 20.0f * std::log10(gain)) : silenceDb;
}

} // namespace anoesis::dsp
