// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/pan.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace anoesis::dsp
{

/// Adds a mono signal into a stereo block with a volume, a pan position, and
/// a polarity, all smoothed so that changes make no zipper noise.
///
/// The three controls fold into one gain per channel,
///
///   left  = sign x volume x panGains(position).left
///   right = sign x volume x panGains(position).right,
///
/// and each channel gain is ramped by a LinearSmoother (10 ms by default).
/// So the pan law's sin/cos runs only when a control changes, a settled
/// gain costs one multiply-add per channel per sample, and an inverted gain
/// gives the exact negation (float negation is exact). Toggling the polarity
/// mid-note ramps the gain through zero instead of flipping it: a 10 ms
/// crossfade rather than a click.
///
/// Gains are interpolated linearly between the pan law's end points, so
/// mid-ramp the law is a chord of its curve: a full left-to-right swing dips
/// by 3 dB at its midpoint for an instant.
///
/// Satisfies Preparable. prepare() and reset() finish any ramp (the gains
/// jump to their targets), which is what a new note wants.
///
/// Doc: docs/dsp/oscillator-controls.md
class StereoGain
{
public:
    /// The default ramp: 10 ms turns a gain step's click into a short fade
    /// (docs/dsp/parameter-smoothing.md).
    static constexpr float defaultRampSeconds = 0.01f;

    StereoGain() noexcept
    {
        setRampTime(defaultRampSeconds);
        updateTargets();
    }

    void prepare(const ProcessSpec& spec) noexcept
    {
        left.prepare(spec);
        right.prepare(spec);
    }

    void reset() noexcept
    {
        left.reset();
        right.reset();
    }

    /// Applies from the next control change. Zero means no smoothing.
    void setRampTime(float seconds) noexcept
    {
        left.setRampTime(seconds);
        right.setRampTime(seconds);
    }

    /// The volume as a linear gain. NaN counts as 0.
    void setGain(float newGain) noexcept
    {
        volume = std::isnan(newGain) ? 0.0f : newGain;
        updateTargets();
    }

    /// -1 (hard left) to +1 (hard right); see panGains().
    void setPan(float position) noexcept
    {
        pan = position;
        updateTargets();
    }

    void setInverted(bool shouldInvert) noexcept
    {
        inverted = shouldInvert;
        updateTargets();
    }

    void setPanLaw(PanLaw newLaw) noexcept
    {
        law = newLaw;
        updateTargets();
    }

    /// The gains the ramps are heading for.
    [[nodiscard]] StereoGains targetGains() const noexcept
    {
        return {.left = left.targetValue(), .right = right.targetValue()};
    }

    /// The gains most recently applied.
    [[nodiscard]] StereoGains currentGains() const noexcept
    {
        return {.left = left.currentValue(), .right = right.currentValue()};
    }

    [[nodiscard]] bool isSmoothing() const noexcept
    {
        return left.isSmoothing() || right.isSmoothing();
    }

    /// Adds left gain x `in` to channel 0 of `out` and right gain x `in` to
    /// channel 1, for the shorter of `in` and the block. A block with fewer
    /// than 2 channels is left untouched; channels after the second are
    /// ignored.
    void process(std::span<const float> in, const AudioBlock& out) noexcept
    {
        if (out.numChannels() < 2)
        {
            return;
        }
        const auto numSamples = std::min(in.size(), out.numSamples());
        float* const leftOut = out.channel(0).data();
        float* const rightOut = out.channel(1).data();
        const float* const source = in.data();
        if (!isSmoothing())
        {
            // Settled: constant gains, held in locals so the compiler need
            // not reload them through the output pointers.
            const float gainLeft = left.currentValue();
            const float gainRight = right.currentValue();
            for (std::size_t i = 0; i < numSamples; ++i)
            {
                leftOut[i] += gainLeft * source[i];
                rightOut[i] += gainRight * source[i];
            }
            return;
        }
        for (std::size_t i = 0; i < numSamples; ++i)
        {
            leftOut[i] += left.next() * source[i];
            rightOut[i] += right.next() * source[i];
        }
    }

private:
    void updateTargets() noexcept
    {
        const auto gains = panGains(pan, law);
        const float signedVolume = inverted ? -volume : volume;
        left.setTarget(signedVolume * gains.left);
        right.setTarget(signedVolume * gains.right);
    }

    LinearSmoother left;
    LinearSmoother right;
    float volume = 1.0f;
    float pan = 0.0f;
    bool inverted = false;
    PanLaw law = PanLaw::equalPower;
};

} // namespace anoesis::dsp
