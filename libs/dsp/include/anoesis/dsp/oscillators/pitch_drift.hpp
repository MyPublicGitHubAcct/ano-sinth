// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/random.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <type_traits>

namespace anoesis::dsp
{

/// Analog oscillator drift: a slow, random pitch deviation, added to a
/// voice's note. One generator per polyphonic voice; the voice adds offset()
/// to the note it gives every oscillator slot, so a voice's oscillators and
/// their unison voices drift together and voices drift apart (Sylenth1's
/// developer: "voice specific ... the oscs should stay in sync").
///
/// The model is two-pole noise: uniform white noise through two one-pole
/// lowpasses in cascade, both at the corner rateHz(). It is Gaussian-like,
/// stationary (it never wanders off), and smooth: its spectrum is flat below
/// the corner and falls 40 dB per decade above it, so half its variance lies
/// below 0.442 x the corner and 99% below 3.37 x. The state is kept at unit
/// standard deviation; depthCents() scales it, so the depth is the standard
/// deviation of the pitch in cents.
///
/// It is updated every updateInterval samples and held between updates: a
/// note per 64 samples costs a whole note about 2%, against 50% for a note
/// per sample (docs/dsp/oscillator-drift.md). The grid is counted from the
/// last noteOn() from silence (or reset()), never from a block boundary, so
/// the output does not depend on how a render is split into blocks.
///
/// - noteOn() from silence draws a fresh state from the stationary
///   distribution, as a drift that has been running unheard would be; while
///   the voice sounds (a legato note, a retrigger), the drift continues.
/// - Depth 0 gives offsets of exactly 0, so drift off leaves every note as
///   it was, bit for bit.
/// - Reproducible: the same seed gives the same offsets on every platform.
///   Give each voice its own seed (for example the instance's seed plus the
///   voice's index).
///
/// No allocation; nothing but a Random and a few floats. Doc:
/// docs/dsp/oscillator-drift.md
class PitchDrift
{
public:
    static constexpr std::size_t updateInterval = 64;
    static constexpr float defaultDepthCents = 1.5f;
    static constexpr float defaultRateHz = 0.5f;
    /// Depths below this are 0 (inaudible, and it keeps offsets normal).
    static constexpr float minDepthCents = 1e-3f;
    static constexpr float maxDepthCents = 1200.0f;
    static constexpr float minRateHz = 1e-3f;
    static constexpr float maxRateHz = 50.0f;
    /// The generator's seed is XORed with this (the fractional part of
    /// sqrt(3)), so a voice's drift does not repeat a slot's sequences when
    /// both are given the same seed.
    static constexpr std::uint64_t seedSalt = 0xBB67AE8584CAA73BU;

    explicit PitchDrift(std::uint64_t seed = 0) noexcept : random(seed ^ seedSalt)
    {
        updateCoefficients();
    }

    void prepare(const ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        updateCoefficients();
        reset();
    }

    /// Restarts the random sequence from the seed, puts the drift at 0 (in
    /// tune), and restarts the grid. The next noteOn() is from silence.
    void reset() noexcept
    {
        random.reset();
        y1 = 0.0f;
        y2 = 0.0f;
        position = 0;
        active = false;
    }

    /// For a voice that has finished: the next noteOn() is from silence and
    /// draws a fresh state. The random sequence carries on.
    void stop() noexcept
    {
        active = false;
    }

    /// From silence (after prepare(), reset(), or stop()): a fresh state from
    /// the stationary distribution, and the grid restarts here. While
    /// sounding: nothing changes.
    void noteOn() noexcept
    {
        if (active)
        {
            return;
        }
        active = true;
        const float u = gaussian();
        const float v = gaussian();
        y1 = start11 * u;
        y2 = (start21 * u) + (start22 * v);
        position = 0;
    }

    /// Restarts the random sequence from a new seed.
    void setSeed(std::uint64_t seed) noexcept
    {
        random.setSeed(seed ^ seedSalt);
    }

    /// The standard deviation of the pitch, in cents. 0 is off. NaN, negative
    /// values, and depths below minDepthCents are 0; clamped to
    /// maxDepthCents. Takes effect at once (a global setting, rarely changed).
    void setDepthCents(float cents) noexcept
    {
        depth = (cents >= minDepthCents) ? std::min(cents, maxDepthCents) : 0.0f;
        scale = depth * 0.01f;
    }

    [[nodiscard]] float depthCents() const noexcept
    {
        return depth;
    }

    /// The corner of both one-poles, in Hz; clamped to [minRateHz,
    /// maxRateHz], NaN is the default.
    void setRateHz(float hz) noexcept
    {
        rate = std::isnan(hz) ? defaultRateHz : std::clamp(hz, minRateHz, maxRateHz);
        updateCoefficients();
    }

    [[nodiscard]] float rateHz() const noexcept
    {
        return rate;
    }

    /// The current offset, in semitones: add it to the note.
    [[nodiscard]] float offset() const noexcept
    {
        return isOff() ? 0.0f : scale * y2;
    }

    /// Samples until the offset next changes, 1 to updateInterval.
    [[nodiscard]] std::size_t samplesToUpdate() const noexcept
    {
        return updateInterval - position;
    }

    /// Covers the next numSamples samples in runs of constant offset: calls
    /// render(start, length, offset) for each, in order, where offset is in
    /// semitones. A voice renders its slots for each run at note + offset.
    /// With drift off it is one run of the whole length, at exactly 0.
    /// noexcept when render is.
    template <typename Render>
    void process(std::size_t numSamples,
                 Render render) noexcept(std::is_nothrow_invocable_v<Render&, std::size_t, std::size_t, float>)
    {
        if (isOff())
        {
            if (numSamples > 0)
            {
                render(std::size_t{0}, numSamples, 0.0f);
            }
            advance(numSamples);
            return;
        }
        std::size_t start = 0;
        while (start < numSamples)
        {
            const auto length = std::min(samplesToUpdate(), numSamples - start);
            render(start, length, offset());
            advance(length);
            start += length;
        }
    }

    /// Adds the offset to each of a note per sample, for a voice that passes
    /// one anyway (glide, pitch modulation), and moves on by notes.size().
    void addTo(std::span<float> notes) noexcept
    {
        process(notes.size(),
                [&notes](std::size_t start, std::size_t length, float semitones) noexcept
                {
                    for (auto& note : notes.subspan(start, length))
                    {
                        note += semitones;
                    }
                });
    }

    /// Moves on by numSamples without producing offsets.
    void advance(std::size_t numSamples) noexcept
    {
        while (numSamples > 0)
        {
            const auto step = std::min(samplesToUpdate(), numSamples);
            position += step;
            numSamples -= step;
            if (position == updateInterval)
            {
                position = 0;
                update();
            }
        }
    }

private:
    [[nodiscard]] bool isOff() const noexcept
    {
        return !(depth > 0.0f);
    }

    // One step of the two-pole: unit-variance white noise w,
    // y1 = a y1 + g w, y2 = a y2 + (1 - a) y1, with g chosen so that y2 has
    // unit variance.
    void update() noexcept
    {
        y1 = flushDenormal((pole * y1) + (inputGain * random.nextBipolar() * std::numbers::sqrt3_v<float>));
        y2 = flushDenormal((pole * y2) + (outputGain * y1));
    }

    // Approximately Gaussian, unit variance: the sum of four uniform draws
    // (Irwin-Hall), each of variance 1/3, scaled by sqrt(3/4).
    [[nodiscard]] float gaussian() noexcept
    {
        const float sum = random.nextBipolar() + random.nextBipolar() + random.nextBipolar() + random.nextBipolar();
        return sum * (std::numbers::sqrt3_v<float> * 0.5f);
    }

    void updateCoefficients() noexcept
    {
        const double updateHz = static_cast<double>(sampleRate) / static_cast<double>(updateInterval);
        const double a = std::exp(-2.0 * std::numbers::pi * static_cast<double>(rate) / updateHz);
        const double b = 1.0 - a;
        // Stationary moments for unit white input (the fixed point of the
        // update): m11 = var y1, m12 = cov(y1, y2), m22 = var y2.
        const double d = 1.0 - (a * a);
        const double m11 = b * b / d;
        const double m12 = b * m11 / d;
        const double m22 = ((2.0 * a * a * b * m12) + (b * b * m11)) / d;
        const double norm = 1.0 / std::sqrt(m22);
        pole = static_cast<float>(a);
        inputGain = static_cast<float>(b * norm);
        outputGain = static_cast<float>(b);
        // The Cholesky factor of the normalised stationary covariance, for
        // a fresh draw at noteOn().
        const double c11 = std::sqrt(m11) * norm;
        const double c21 = m12 * norm * norm / c11;
        start11 = static_cast<float>(c11);
        start21 = static_cast<float>(c21);
        start22 = static_cast<float>(std::sqrt(std::max(1.0 - (c21 * c21), 0.0)));
    }

    Random random;
    float sampleRate = 48000.0f;
    float depth = 0.0f;
    float scale = 0.0f;
    float rate = defaultRateHz;
    float pole = 0.0f;
    float inputGain = 0.0f;
    float outputGain = 0.0f;
    float start11 = 0.0f;
    float start21 = 0.0f;
    float start22 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
    std::size_t position = 0;
    bool active = false;
};

} // namespace anoesis::dsp
