// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>

#include <cmath>
#include <cstddef>

namespace anoesis::dsp
{

/// Main Volume: one gain on every channel, in place, at the end of the chain
/// (after the master effects; inventory §5, X6-X8). Chosen by the project
/// owner from measured candidates (docs/dsp/mixer.md):
///
/// - The gain is linear and ramped by a LinearSmoother over 10 ms from each
///   setGain(), so automation and jumps don't click. The knob's law is the
///   parameter table's (M1.9); this takes the gain.
/// - Nothing clips: the output is the input times the gain, one rounding per
///   sample, however loud.
/// - The caller splits its block at each Main Volume event and calls
///   setGain() there, so the ramp starts at the event's sample and the
///   output doesn't depend on the host's block size.
///
/// Satisfies BlockProcessor. prepare() and reset() finish any ramp. Nothing
/// allocates.
/// Doc: docs/dsp/mixer.md
class MainVolume
{
public:
    static constexpr float defaultRampSeconds = 0.01f;

    MainVolume() noexcept
    {
        gain.setRampTime(defaultRampSeconds);
        gain.setCurrentAndTarget(1.0f);
    }

    void prepare(const ProcessSpec& spec) noexcept
    {
        gain.prepare(spec);
    }
    void reset() noexcept
    {
        gain.reset();
    }

    /// The gain to ramp to from the next sample. Anything not finite and at
    /// least 0 counts as 0.
    void setGain(float newGain) noexcept
    {
        gain.setTarget(sanitised(newGain));
    }
    /// The gain at once, with no ramp (a preset load, before audio).
    void setGainImmediately(float newGain) noexcept
    {
        gain.setCurrentAndTarget(sanitised(newGain));
    }
    [[nodiscard]] float targetGain() const noexcept
    {
        return gain.targetValue();
    }
    [[nodiscard]] float currentGain() const noexcept
    {
        return gain.currentValue();
    }
    [[nodiscard]] bool isSmoothing() const noexcept
    {
        return gain.isSmoothing();
    }

    /// Multiplies every channel of `block` by the gain, sample by sample.
    void process(const AudioBlock& block) noexcept
    {
        const auto n = block.numSamples();
        const auto channels = block.numChannels();
        if (!gain.isSmoothing())
        {
            const float g = gain.currentValue();
            for (std::size_t c = 0; c < channels; ++c)
            {
                float* const x = block.channel(c).data();
                for (std::size_t i = 0; i < n; ++i)
                {
                    x[i] *= g;
                }
            }
            return;
        }
        for (std::size_t i = 0; i < n; ++i)
        {
            const float g = gain.next();
            for (std::size_t c = 0; c < channels; ++c)
            {
                block.channel(c)[i] *= g;
            }
        }
    }

private:
    [[nodiscard]] static float sanitised(float g) noexcept
    {
        return std::isfinite(g) && g >= 0.0f ? g : 0.0f;
    }

    LinearSmoother gain;
};

} // namespace anoesis::dsp
