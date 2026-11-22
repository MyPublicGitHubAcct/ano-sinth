// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// A copy of dsp::LadderFilter for the part-solo and skipping candidates (M1.4
// task 5, stage 1; docs/dsp/part-solo-and-skipping.md). A prototype for
// measurement, not library code. It is LadderFilter as built, to the bit,
// with three additions a voice needs to skip a filter and to run it mono:
//
// - skip(n): n samples without solving: the grid advances (beginSegment()
//   at each grid point, so the voice's feeds are taken exactly as process()
//   would), and the per-sample ramps (resonance, drive, the response's
//   crossfade) skip n samples. The state is untouched. On a filter at rest
//   with an input of zeros, process() leaves the state at zero and writes
//   zeros, so skip() is then the same to the bit (measured by the report).
// - clearState(): the state to rest, the ramps and grid kept (reset()
//   restarts the ramps too).
// - copyChannel(from, to): one channel's state set from another's, to leave
//   a mono path (docs/dsp/voice-architecture.md, question 4).

#pragma once

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/filters/ladder_solver.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace anoesis::test::solo
{

namespace dsp = anoesis::dsp;

using dsp::AudioBlock;
using dsp::CubicSaturator;
using dsp::FilterResponse;
using dsp::FilterTapMix;
using dsp::filterTapMixes;
using dsp::flushDenormal;
using dsp::LadderSolver;
using dsp::LadderState;
using dsp::LinearSmoother;
using dsp::ProcessSpec;
using dsp::TanhSaturator;

/// dsp::LadderFilter with skip(), clearState() and copyChannel().
class SkipLadderFilter
{
public:
    static constexpr std::size_t maxChannels = 2;
    /// g is computed every gridSamples samples and interpolated in between.
    static constexpr std::size_t gridSamples = 32;
    /// The ramp for cutoff, resonance, drive, and response changes.
    static constexpr float rampSeconds = 0.01f;
    static constexpr float minCutoffHz = 1.0f;
    /// The highest cutoff, as a fraction of the sample rate (tan stays finite).
    static constexpr float maxCutoffRatio = 0.495f;
    static constexpr float newtonTolerance = LadderSolver::newtonTolerance;
    static constexpr std::uint32_t maxNewtonIterations = LadderSolver::maxNewtonIterations;

    SkipLadderFilter() noexcept
    {
        tapFade.setCurrentAndTarget(1.0f);
        drive.setCurrentAndTarget(1.0f);
        cutoffLog2.setCurrentAndTarget(std::log2(1000.0f));
    }

    void prepare(const ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        channelCount = std::min(spec.numChannels, maxChannels);
        for (auto* smoother : {&cutoffLog2, &resonance, &drive, &tapFade})
        {
            smoother->prepare(spec);
            smoother->setRampTime(rampSeconds);
        }
        reset();
    }

    void reset() noexcept
    {
        cutoffLog2.reset();
        resonance.reset();
        drive.reset();
        tapFade.reset();
        previousTaps = taps;
        channels = {};
        gridPosition = 0;
        segmentEndGain = gainAt(cutoffLog2.currentValue());
        segmentStartGain = segmentEndGain;
        segmentEndResonance = resonanceModulation;
        segmentStartResonance = segmentEndResonance;
    }

    /// The cutoff in Hz, ramped in log2 over 10 ms from the next grid point.
    /// Clamped to [1 Hz, 0.495 fs] (with the modulation) when used.
    void setCutoff(float hz) noexcept
    {
        // min/max in this order map NaN and +inf to 1 MHz, -inf to 1 Hz.
        cutoffLog2.setTarget(std::log2(std::max(minCutoffHz, std::min(1e6f, hz))));
    }

    /// A modulation in octaves added to the cutoff, unsmoothed, from the next
    /// grid point (where g is interpolated over the 32 samples to it).
    void setCutoffModulation(float octaves) noexcept
    {
        modulationOctaves = std::isnan(octaves) ? 0.0f : std::clamp(octaves, -40.0f, 40.0f);
    }

    /// The loop gain k >= 0; 4 is the self-oscillation threshold.
    void setResonance(float k) noexcept
    {
        resonance.setTarget(std::min(64.0f, std::max(0.0f, k)));
    }

    /// A modulation of k added to setResonance()'s, unsmoothed, from the next
    /// grid point, where k runs linearly over the 32 samples to it (as g
    /// does). Clamped to [0, 64], NaN to 0; the sum is capped at 64.
    void setResonanceModulation(float k) noexcept
    {
        resonanceModulation = std::isnan(k) ? 0.0f : std::clamp(k, 0.0f, 64.0f);
    }

    /// A linear gain on the input, >= 0, no make-up.
    void setDrive(float gain) noexcept
    {
        drive.setTarget(std::min(1000.0f, std::max(0.0f, gain)));
    }

    /// Crossfades to `r`'s tap mix over 10 ms.
    void setResponse(FilterResponse r) noexcept
    {
        const auto& next = filterTapMixes[static_cast<std::size_t>(r)];
        if (next == taps)
        {
            return;
        }
        previousTaps = blendedTaps(tapFade.currentValue());
        taps = next;
        tapFade.setCurrentAndTarget(0.0f);
        tapFade.setTarget(1.0f);
    }

    /// Warm Drive: tanh saturators when on, cubic when off.
    void setWarm(bool on) noexcept
    {
        warm = on;
    }

    /// While bypassed, process() leaves the audio untouched. Leaving bypass
    /// starts the filter from rest.
    void setBypassed(bool on) noexcept
    {
        if (bypassed && !on)
        {
            reset();
        }
        bypassed = on;
    }

    [[nodiscard]] bool isBypassed() const noexcept
    {
        return bypassed;
    }

    /// True when every channel's state is exactly zero: the filter holds no
    /// energy, so silence in gives silence out. Reached within the slowest
    /// pole's decay to FLT_MIN after the input stops (the states are flushed).
    [[nodiscard]] bool isAtRest() const noexcept
    {
        const auto zero = [](float v)
        {
            return std::fpclassify(v) == FP_ZERO;
        };
        return std::ranges::all_of(channels, [&](const LadderState& c)
                                   { return std::ranges::all_of(c.s, zero) && zero(c.previousU); });
    }

    /// The cutoff in Hz in use at the end of the current grid segment.
    [[nodiscard]] float currentCutoff() const noexcept
    {
        return cutoffAt(cutoffLog2.currentValue());
    }

    void process(const AudioBlock& block) noexcept
    {
        if (bypassed)
        {
            return;
        }
        const auto numChannels = std::min(block.numChannels(), channelCount);
        const auto numSamples = block.numSamples();
        std::size_t done = 0;
        while (done < numSamples)
        {
            if (gridPosition == 0)
            {
                beginSegment();
            }
            const auto length = std::min(numSamples - done, gridSamples - gridPosition);
            if (warm)
            {
                processRun<TanhSaturator>(block, numChannels, done, length);
            }
            else
            {
                processRun<CubicSaturator>(block, numChannels, done, length);
            }
            gridPosition = (gridPosition + length) % gridSamples;
            done += length;
        }
    }

    /// n samples without solving: the grid and the ramps advance as process()
    /// would move them; the state is untouched.
    void skip(std::size_t numSamples) noexcept
    {
        if (bypassed)
        {
            return;
        }
        std::size_t done = 0;
        while (done < numSamples)
        {
            if (gridPosition == 0)
            {
                beginSegment();
            }
            const auto length = std::min(numSamples - done, gridSamples - gridPosition);
            resonance.skip(length);
            drive.skip(length);
            tapFade.skip(length);
            gridPosition = (gridPosition + length) % gridSamples;
            done += length;
        }
    }

    /// The state to rest; the ramps and the grid carry on.
    void clearState() noexcept
    {
        channels = {};
    }

    /// Channel `to`'s state set from channel `from`'s.
    void copyChannel(std::size_t from, std::size_t to) noexcept
    {
        channels[std::min(to, maxChannels - 1)] = channels[std::min(from, maxChannels - 1)];
    }

private:
    [[nodiscard]] float cutoffAt(float log2Hz) const noexcept
    {
        const float hz = std::exp2(std::clamp(log2Hz + modulationOctaves, -40.0f, 40.0f));
        return std::clamp(hz, minCutoffHz, maxCutoffRatio * sampleRate);
    }

    [[nodiscard]] float gainAt(float log2Hz) const noexcept
    {
        return std::tan(std::numbers::pi_v<float> * cutoffAt(log2Hz) / sampleRate);
    }

    void beginSegment() noexcept
    {
        segmentStartGain = segmentEndGain;
        cutoffLog2.skip(gridSamples);
        segmentEndGain = gainAt(cutoffLog2.currentValue());
        segmentStartResonance = segmentEndResonance;
        segmentEndResonance = resonanceModulation;
    }

    [[nodiscard]] FilterTapMix blendedTaps(float b) const noexcept
    {
        FilterTapMix mix{};
        for (std::size_t i = 0; i < mix.size(); ++i)
        {
            mix[i] = previousTaps[i] + (b * (taps[i] - previousTaps[i]));
        }
        return mix;
    }

    template <typename Sat>
    void processRun(const AudioBlock& block, std::size_t numChannels, std::size_t offset, std::size_t length) noexcept
    {
        std::array<float, gridSamples> g{};
        std::array<float, gridSamples> k{};
        std::array<float, gridSamples> gain{};
        std::array<float, gridSamples> fade{};
        const float gainStep = (segmentEndGain - segmentStartGain) * (1.0f / static_cast<float>(gridSamples));
        const float resonanceStep =
            (segmentEndResonance - segmentStartResonance) * (1.0f / static_cast<float>(gridSamples));
        for (std::size_t i = 0; i < length; ++i)
        {
            g[i] = segmentStartGain + (gainStep * static_cast<float>(gridPosition + i));
            k[i] = std::min(64.0f, resonance.next() + (segmentStartResonance +
                                                       (resonanceStep * static_cast<float>(gridPosition + i))));
            gain[i] = drive.next();
            fade[i] = tapFade.next();
        }
        for (std::size_t ch = 0; ch < numChannels; ++ch)
        {
            auto& state = channels[ch];
            const auto io = block.channel(ch).subspan(offset, length);
            for (std::size_t i = 0; i < length; ++i)
            {
                // LadderSolver::solve(), in its two halves, so that the hot one
                // stays a leaf (ladder_solver.hpp).
                auto [solution, iterations] = LadderSolver::solveByNewton<Sat>(state, io[i] * gain[i], g[i], k[i]);
                if (iterations == 0)
                {
                    solution = LadderSolver::solveNested<Sat>(state, io[i] * gain[i], g[i], k[i]);
                }
                const auto mix = blendedTaps(fade[i]);
                const float out = (mix[0] * solution.u) + (mix[1] * solution.y[0]) + (mix[2] * solution.y[1]) +
                                  (mix[3] * solution.y[2]) + (mix[4] * solution.y[3]);
                io[i] = flushDenormal(out);
            }
        }
    }

    float sampleRate = 48000.0f;
    std::size_t channelCount = 1;
    LinearSmoother cutoffLog2;
    LinearSmoother resonance;
    LinearSmoother drive;
    LinearSmoother tapFade;
    float modulationOctaves = 0.0f;
    FilterTapMix taps = filterTapMixes[0];
    FilterTapMix previousTaps = filterTapMixes[0];
    bool warm = true;
    bool bypassed = false;
    std::array<LadderState, maxChannels> channels{};
    std::size_t gridPosition = 0;
    float segmentStartGain = 0.0f;
    float segmentEndGain = 0.0f;
    float resonanceModulation = 0.0f;
    float segmentStartResonance = 0.0f;
    float segmentEndResonance = 0.0f;
};

} // namespace anoesis::test::solo
