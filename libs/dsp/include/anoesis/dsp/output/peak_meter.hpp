// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace anoesis::dsp
{

/// The meter's ballistics.
struct PeakMeterSettings
{
    float holdSeconds = 0.5f;                   ///< the peak held before it falls
    float fallDecibelsPerSecond = 20.0f / 1.7f; ///< IEC 60268-10 Type I's return: 20 dB in 1.7 s
    float overSeconds = 1.0f;                   ///< the over light held after the last sample above 1.0
};

/// The output meter for one channel (inventory §5, X9), chosen by the
/// project owner from measured candidates (docs/dsp/mixer.md): a sample peak
/// with an instant attack, a hold, then a fall at a fixed rate in dB, and an
/// over flag held for a time after any sample above full scale (1.0).
///
/// Run per sample, with its hold counted in samples, so it reads the same at
/// the same sample whatever the block split. It only produces values:
/// reading() and over() after each block; handing them to a GUI thread is the
/// plugin's (M1.9, M1.11). Nothing allocates.
/// Doc: docs/dsp/mixer.md
class PeakMeter
{
public:
    explicit PeakMeter(const PeakMeterSettings& s = {}) noexcept : settings(s) {}

    /// Converts the settings to samples at the spec's rate; then as reset().
    void prepare(const ProcessSpec& spec) noexcept
    {
        const auto sampleRate = static_cast<double>(spec.sampleRate);
        holdSamples = samplesFor(settings.holdSeconds, sampleRate);
        overSamples = samplesFor(settings.overSeconds, sampleRate);
        const double fallDb = static_cast<double>(std::max(settings.fallDecibelsPerSecond, 0.0f));
        fall = static_cast<float>(std::pow(10.0, -fallDb / 20.0 / sampleRate));
        reset();
    }
    /// Silence: the reading 0, no hold, no over.
    void reset() noexcept
    {
        level = 0.0f;
        hold = 0;
        overHold = 0;
    }

    void process(std::span<const float> x) noexcept
    {
        for (const float v : x)
        {
            const float a = std::abs(v);
            if (a >= level)
            {
                level = a;
                hold = holdSamples;
            }
            else if (hold > 0)
            {
                --hold;
            }
            else
            {
                level = level * fall;
                if (level < silence)
                {
                    level = 0.0f;
                }
            }
            if (a > 1.0f)
            {
                overHold = overSamples;
            }
            else if (overHold > 0)
            {
                --overHold;
            }
        }
    }

    /// The level now, as a linear amplitude (1.0 is full scale).
    [[nodiscard]] float reading() const noexcept
    {
        return level;
    }
    /// Whether a sample above 1.0 came within the over time.
    [[nodiscard]] bool over() const noexcept
    {
        return overHold > 0;
    }

private:
    /// Below this (-200 dB) the reading is 0, so the fall never goes subnormal.
    static constexpr float silence = 1e-10f;

    [[nodiscard]] static std::size_t samplesFor(float seconds, double sampleRate) noexcept
    {
        const double n = static_cast<double>(std::max(seconds, 0.0f)) * sampleRate;
        return static_cast<std::size_t>(std::lround(n));
    }

    PeakMeterSettings settings;
    std::size_t holdSamples = 0;
    std::size_t overSamples = 0;
    float fall = 1.0f;
    float level = 0.0f;
    std::size_t hold = 0;
    std::size_t overHold = 0;
};

/// A PeakMeter per channel of a stereo output.
class StereoPeakMeter
{
public:
    explicit StereoPeakMeter(const PeakMeterSettings& s = {}) noexcept : meters{PeakMeter{s}, PeakMeter{s}} {}
    void prepare(const ProcessSpec& spec) noexcept
    {
        for (auto& m : meters)
        {
            m.prepare(spec);
        }
    }
    void reset() noexcept
    {
        for (auto& m : meters)
        {
            m.reset();
        }
    }
    /// Meters channels 0 and 1 of `block` (a channel it lacks is skipped).
    void process(const AudioBlock& block) noexcept
    {
        for (std::size_t c = 0; c < meters.size() && c < block.numChannels(); ++c)
        {
            meters[c].process(block.channel(c));
        }
    }
    [[nodiscard]] const PeakMeter& channel(std::size_t c) const noexcept
    {
        return meters[c];
    }

private:
    std::array<PeakMeter, 2> meters;
};

} // namespace anoesis::dsp
