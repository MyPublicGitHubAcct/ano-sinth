// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

/// A global 7-bit controller as a modulation source: the mod wheel (CC 1)
/// and channel pressure (M1.5 task 3, decided 2026-11-22; docs/dsp/
/// modulation-sources.md). A value 0-1, ramped linearly over 5 ms from where
/// it stands to each new one, so a slow sweep's 128 steps don't zip (-84.7 dB
/// on Pitch at +1.00, against -62.0 for steps) and a full step doesn't click
/// (-43.5 dB, against -19.5).
///
/// It is PitchBend's closed form, generalised to a value 0-1: one object per
/// controller in the engine, given each new value at its event's absolute
/// sample (counted from prepare() or reset()), and read by every voice at its
/// own absolute samples, so all voices hear the same value at the same sample
/// whatever the host's block size. The value is exact in closed form at any
/// sample, from + (to - from) x n / N with the product rounded in a statement
/// of its own (no fused multiply-add on any compiler), it lands exactly on
/// the new value after N = round(5 ms x rate) samples, and it is monotonic
/// between events, so its two ends bound it (source_bound.hpp, linearBound()).
///
/// PitchBend isn't rebuilt on it: PitchBend's expression is one statement,
/// which Apple Clang contracts into a fused multiply-add, and its length is
/// rounded from a float 5 ms (220 samples at 44.1 kHz, against 221 here), so
/// the bend would move by an ulp and the voices with it.
///
/// No allocation; a few words of state. Doc: docs/dsp/modulation-sources.md
namespace anoesis::dsp::modulation
{

class ControllerRamp
{
public:
    /// Each new value is reached over this long.
    static constexpr double rampMilliseconds = 5.0;

    void prepare(double sampleRate) noexcept
    {
        const double n = std::max(0.0, rampMilliseconds * 1e-3 * sampleRate);
        length = static_cast<std::uint64_t>(std::llround(n));
        reset();
    }

    /// At 0, from now on.
    void reset() noexcept
    {
        from = 0.0f;
        to = 0.0f;
        start = 0;
    }

    /// A new value at the absolute sample `time` (at or after the latest
    /// one's): the ramp runs from where it stands there to `value`, clamped
    /// to [0, 1]. NaN is ignored.
    void set(std::uint64_t time, float value) noexcept
    {
        if (std::isnan(value))
        {
            return;
        }
        const float clamped = std::clamp(value, 0.0f, 1.0f);
        from = valueAt(time);
        to = clamped;
        start = time;
    }

    /// A 7-bit MIDI value (0-127; others clamped) as value / 127.
    void setMidi(std::uint64_t time, int value) noexcept
    {
        set(time, static_cast<float>(std::clamp(value, 0, 127)) / 127.0f);
    }

    /// The value at the absolute sample `time` (at or after the latest
    /// value's).
    [[nodiscard]] float valueAt(std::uint64_t time) const noexcept
    {
        if (time >= start + length)
        {
            return to;
        }
        if (time <= start)
        {
            return from;
        }
        const float t = static_cast<float>(time - start) / static_cast<float>(length);
        const float step = (to - from) * t; // its own statement: rounded before the sum
        return from + step;
    }

    /// Whether the value stays the same from `time` on (until a new one).
    [[nodiscard]] bool isSteadyFrom(std::uint64_t time) const noexcept
    {
        return time >= start + length;
    }

    /// The ramp's length in samples.
    [[nodiscard]] std::uint64_t rampSamples() const noexcept
    {
        return length;
    }

private:
    std::uint64_t length = 240;
    float from = 0.0f;
    float to = 0.0f;
    std::uint64_t start = 0;
};

} // namespace anoesis::dsp::modulation
