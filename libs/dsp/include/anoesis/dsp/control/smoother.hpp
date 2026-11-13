// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/process_spec.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace anoesis::dsp
{

/// How a Smoother moves from its current value to a new target.
enum class SmoothingShape : std::uint8_t
{
    /// Equal steps: the value changes at a constant rate. Suits parameters
    /// that are already perceptually scaled (dB, semitones, normalised knobs).
    linear,
    /// A one-pole approach: fast at first, then easing in, like an analog RC
    /// circuit. The remaining distance falls to 1e-4 (-80 dB) of the step
    /// over the ramp time; the last sample then lands exactly.
    exponential,
};

/// Smooths a parameter towards a target so that changes don't produce zipper
/// noise (an audible step discontinuity at every control update).
///
/// Every ramp, whatever its shape, lasts exactly rampLengthSamples() and ends
/// exactly on the target, so isSmoothing() reliably tells a processor when
/// it can switch to a cheaper constant-parameter path. Satisfies Preparable.
///
///   prepare(spec)         converts the ramp time to samples at spec.sampleRate
///   reset()               finishes any ramp: current value = target
///   setRampTime(seconds)  applies from the next setTarget()
///   setTarget(value)      starts a new ramp from the current value
///   next()                advances one sample and returns the new value
///
/// All members except prepare() are real-time safe. Doc:
/// docs/dsp/parameter-smoothing.md
template <SmoothingShape Shape>
class Smoother
{
public:
    /// The longest ramp, in samples: 2^24, the largest count a float holds
    /// exactly (about 350 s at 48 kHz). Longer ramp times are clamped.
    static constexpr std::size_t maxRampLength = std::size_t{1} << 24U;

    /// The remaining fraction of the step at which an exponential ramp lands.
    static constexpr float exponentialResidual = 1e-4f;

    void prepare(const ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        updateRampLength();
        reset();
    }

    void reset() noexcept
    {
        current = target;
        remainingSteps = 0;
    }

    /// Sets how long a ramp takes. Negative times count as zero (no smoothing).
    /// A ramp already in progress keeps its length.
    void setRampTime(float seconds) noexcept
    {
        rampSeconds = std::max(0.0f, seconds);
        updateRampLength();
    }

    /// Starts a ramp from the current value to `value`. Setting the target it
    /// already has does nothing, so a host resending an unchanged parameter
    /// every block does not restart the ramp.
    void setTarget(float value) noexcept
    {
        if (value <= target && value >= target) // equal without -Wfloat-equal; false for NaN
        {
            return;
        }
        target = value;
        if (rampLength == 0)
        {
            current = target;
            remainingSteps = 0;
            return;
        }

        start = current;
        activeLength = rampLength;
        remainingSteps = rampLength;
        if constexpr (Shape == SmoothingShape::linear)
        {
            step = (target - start) / static_cast<float>(activeLength);
        }
        else
        {
            distance = target - start;
        }
    }

    /// Jumps to `value` immediately, with no ramp.
    void setCurrentAndTarget(float value) noexcept
    {
        target = value;
        reset();
    }

    /// Advances one sample and returns the new value.
    [[nodiscard]] float next() noexcept
    {
        if (remainingSteps == 0)
        {
            return current;
        }
        --remainingSteps;
        if (remainingSteps == 0)
        {
            current = target;
        }
        else if constexpr (Shape == SmoothingShape::linear)
        {
            // start + step * k rather than current += step: no rounding
            // error accumulates, however long the ramp.
            current = start + (step * static_cast<float>(activeLength - remainingSteps));
        }
        else
        {
            distance *= decay;
            current = target - distance;
        }
        return current;
    }

    /// Advances `numSamples` samples at once (for a block whose parameter is
    /// read only once). Equivalent to calling next() that many times.
    void skip(std::size_t numSamples) noexcept
    {
        if (numSamples >= remainingSteps)
        {
            reset();
            return;
        }
        remainingSteps -= numSamples;
        if constexpr (Shape == SmoothingShape::linear)
        {
            current = start + (step * static_cast<float>(activeLength - remainingSteps));
        }
        else
        {
            distance *= std::pow(decay, static_cast<float>(numSamples));
            current = target - distance;
        }
    }

    /// Writes the next `out.size()` values.
    void fill(std::span<float> out) noexcept
    {
        for (auto& sample : out)
        {
            sample = next();
        }
    }

    /// The value most recently returned by next() (or set directly).
    [[nodiscard]] float currentValue() const noexcept
    {
        return current;
    }

    [[nodiscard]] float targetValue() const noexcept
    {
        return target;
    }

    /// True while a ramp is in progress.
    [[nodiscard]] bool isSmoothing() const noexcept
    {
        return remainingSteps > 0;
    }

    /// The length of the next ramp in samples, from the ramp time and the
    /// prepared sample rate. 0 before prepare().
    [[nodiscard]] std::size_t rampLengthSamples() const noexcept
    {
        return rampLength;
    }

private:
    void updateRampLength() noexcept
    {
        const auto samples = std::min(rampSeconds * sampleRate, static_cast<float>(maxRampLength));
        rampLength = static_cast<std::size_t>(std::lround(samples));
        if constexpr (Shape == SmoothingShape::exponential)
        {
            // decay^rampLength = residual: the distance left after the ramp.
            decay = rampLength > 0 ? std::pow(exponentialResidual, 1.0f / static_cast<float>(rampLength)) : 0.0f;
        }
    }

    // Configuration.
    float sampleRate = 0.0f;
    float rampSeconds = 0.0f;
    std::size_t rampLength = 0;
    float decay = 0.0f; // exponential only: per-sample factor on the remaining distance

    // Ramp state.
    float current = 0.0f;
    float target = 0.0f;
    float start = 0.0f;
    float step = 0.0f;     // linear only
    float distance = 0.0f; // exponential only: target - current
    std::size_t activeLength = 0;
    std::size_t remainingSteps = 0;
};

/// Constant-rate smoothing. See SmoothingShape::linear.
using LinearSmoother = Smoother<SmoothingShape::linear>;

/// One-pole smoothing that lands exactly. See SmoothingShape::exponential.
using ExponentialSmoother = Smoother<SmoothingShape::exponential>;

} // namespace anoesis::dsp
