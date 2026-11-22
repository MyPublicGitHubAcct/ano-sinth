// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

/// Portamento's glide: a voice's pitch moving from one note to another,
/// linear in semitones, in a constant time whatever the interval (inventory
/// §7, L7, L8; decided 2026-11-21, docs/dsp/legato-portamento.md). The same
/// ramp carries a voice's velocity across a gliding note (L3).
///
/// The value is a closed form of the samples since the glide began,
/// from + (to - from) x n / N, so it reads the same at any sample whatever
/// the block size, can be read ahead without advancing (a voice reads it at
/// its grid points for keytrack), and lands exactly on the target after N
/// samples. N = round(seconds x rate); a time of 0 (Portamento 0), NaN, a
/// negative time, or a glide to the note it is on is a jump.
///
/// No allocation; a few words of state. Doc: docs/dsp/legato-portamento.md
namespace anoesis::dsp
{

class Glide
{
public:
    /// The longest glide, in seconds; longer times are clamped.
    static constexpr float maxSeconds = 60.0f;

    void prepare(double sampleRate) noexcept
    {
        rate = sampleRate;
    }

    /// Stands at `note` from now on.
    void jump(float note) noexcept
    {
        from = note;
        to = note;
        elapsed = 0;
        length = 0;
    }

    /// Glides from `from` to `to` over `seconds`, starting now.
    void start(float fromValue, float toValue, float seconds) noexcept
    {
        const double clamped =
            std::isnan(seconds) ? 0.0 : std::clamp(static_cast<double>(seconds), 0.0, static_cast<double>(maxSeconds));
        startSamples(fromValue, toValue, static_cast<std::uint64_t>(std::llround(clamped * rate)));
    }

    /// As start(), over a number of samples (0: a jump).
    void startSamples(float fromValue, float toValue, std::uint64_t samples) noexcept
    {
        if (samples == 0 || !(std::abs(toValue - fromValue) > 0.0f))
        {
            jump(toValue);
            return;
        }
        from = fromValue;
        to = toValue;
        elapsed = 0;
        length = samples;
    }

    /// The value `ahead` samples from now, without advancing.
    [[nodiscard]] float at(std::size_t ahead) const noexcept
    {
        const std::uint64_t n = elapsed + ahead;
        if (n >= length)
        {
            return to;
        }
        return from + ((to - from) * static_cast<float>(static_cast<double>(n) / static_cast<double>(length)));
    }

    /// Moves now on by `samples`.
    void advance(std::size_t samples) noexcept
    {
        elapsed = length - elapsed > samples ? elapsed + samples : length;
    }

    [[nodiscard]] float target() const noexcept
    {
        return to;
    }
    /// Whether the value is still moving now.
    [[nodiscard]] bool isGliding() const noexcept
    {
        return elapsed < length;
    }
    /// The glide's whole length in samples (0 after a jump).
    [[nodiscard]] std::uint64_t lengthSamples() const noexcept
    {
        return length;
    }

private:
    double rate = 48000.0;
    float from = 0.0f;
    float to = 0.0f;
    std::uint64_t elapsed = 0;
    std::uint64_t length = 0;
};

} // namespace anoesis::dsp
