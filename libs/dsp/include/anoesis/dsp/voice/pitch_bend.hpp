// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

/// The pitch bend (inventory §7, L13-L15; decided 2026-11-21,
/// docs/dsp/legato-portamento.md): a 14-bit MIDI value (0-16383, centre
/// 8192) scaled by 8192 both ways, times the Bend Range (0-24 semitones, the
/// same up and down), ramped linearly from where it is to each new value
/// over 5 ms, and read by every sounding voice, releasing ones included.
///
/// Global, one per instance: the allocator sets it at each bend event, at
/// the event's absolute sample (counted from prepare() or reset()), and each
/// voice reads it at its own absolute samples through VoiceKnobs::bend, so
/// all voices hear the same bend at the same sample whatever the host's
/// block size. A Bend Range change applies from the next value.
///
/// No allocation; a few words of state. Doc: docs/dsp/legato-portamento.md
namespace anoesis::dsp
{

/// A 14-bit pitch-bend value as a fraction of the range: (value - 8192) /
/// 8192, from -1 (0) to 8191/8192 (16383). Values outside are clamped.
[[nodiscard]] constexpr float pitchBendFraction(int value) noexcept
{
    return static_cast<float>(std::clamp(value, 0, 16383) - 8192) / 8192.0f;
}

class PitchBend
{
public:
    static constexpr int centre = 8192;
    static constexpr float maxRange = 24.0f;
    static constexpr float defaultRange = 2.0f;
    /// Each new value is reached over this long.
    static constexpr float rampSeconds = 0.005f;

    void prepare(double sampleRate) noexcept
    {
        rampLength = static_cast<std::uint64_t>(std::llround(static_cast<double>(rampSeconds) * sampleRate));
        reset();
    }

    /// The bend at 0, from now on.
    void reset() noexcept
    {
        from = 0.0f;
        to = 0.0f;
        start = 0;
        length = 0;
    }

    /// Semitones, clamped to [0, maxRange]; NaN is ignored. From the next
    /// value.
    void setRange(float semitones) noexcept
    {
        if (!std::isnan(semitones))
        {
            rangeSemitones = std::clamp(semitones, 0.0f, maxRange);
        }
    }
    [[nodiscard]] float range() const noexcept
    {
        return rangeSemitones;
    }

    /// A new 14-bit value at the absolute sample `time`: the bend ramps from
    /// where it is there to range x pitchBendFraction(value).
    void setValue(std::uint64_t time, int value) noexcept
    {
        from = semitonesAt(time);
        to = rangeSemitones * pitchBendFraction(value);
        start = time;
        length = rampLength;
    }

    /// The bend in semitones at the absolute sample `time` (at or after the
    /// latest value's).
    [[nodiscard]] float semitonesAt(std::uint64_t time) const noexcept
    {
        if (time >= start + length)
        {
            return to;
        }
        if (time <= start)
        {
            return from;
        }
        return from + ((to - from) * (static_cast<float>(time - start) / static_cast<float>(length)));
    }

    /// Whether the bend stays the same from `time` on (until a new value).
    [[nodiscard]] bool isSteadyFrom(std::uint64_t time) const noexcept
    {
        return time >= start + length;
    }

private:
    float rangeSemitones = defaultRange;
    std::uint64_t rampLength = 240;
    float from = 0.0f;
    float to = 0.0f;
    std::uint64_t start = 0;
    std::uint64_t length = 0;
};

} // namespace anoesis::dsp
