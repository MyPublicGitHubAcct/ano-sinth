// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/ladder_solver.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

/// The filter: Sylenth1's per-part filter (inventory §3): four stages, each
/// with a saturator, in a resonant loop, with six responses, a true bypass,
/// Cutoff, Resonance past self-oscillation, Drive, and Warm Drive. The model
/// was chosen from measured candidates (docs/dsp/filters.md; theory in
/// docs/dsp/filter-theory.md):
///
/// - A transistor ladder: four one-pole lowpasses in a loop with negative
///   feedback k from the last, a saturator t() at the loop's input and in
///   each stage (five), dy_i/dt = wc (t(y_(i-1)) - t(y_i)), y_0 = x - k y4.
///   Self-oscillation starts at k = 4.
/// - Discretised by the trapezoidal rule with the cutoff prewarped
///   (g = tan(pi fc / fs)) and no delay in the loop (zero-delay feedback),
///   so the cutoff, the response there, and the threshold are exact at every
///   cutoff and sample rate. The implicit equations are solved each sample
///   by LadderSolver (ladder_solver.hpp): Newton-Raphson from the loop
///   linearised at the state, to 1e-5, at most 8 iterations; where Newton
///   does not converge (near Nyquist, at high drive and resonance), a nested
///   bracketing solve that cannot fail (docs/dsp/filter-stability.md). A
///   non-finite input sample counts as 0.
/// - The six responses are fixed mixes of the loop input u and the stage
///   outputs y1..y4 (filterTapMixes, data), so every response resonates at
///   the cutoff. A response change crossfades the mixes over 10 ms.
/// - Warm Drive chooses the saturator: tanh (dsp::fastTanh) when on, a cubic
///   soft clip x - (4/27) x^3 up to |x| = 1.5 when off (fewer upper
///   harmonics, 26-46% cheaper). The structure is the same either way.
/// - The cutoff is a control ramped linearly in log2 over 10 ms, plus a
///   modulation in octaves; both become g at a grid point every 32 samples
///   (counted from reset()), and g is interpolated linearly in between, so
///   tan runs once per 32 samples. A control or modulation change applies
///   from the next grid point. Resonance and drive ramp linearly over 10 ms;
///   k also has a modulation input, read at the grid and interpolated like g
///   (the Filter Control's voice feeds the whole k through it,
///   docs/dsp/filter-control.md).
/// - Drive is a gain on the input, with no make-up. The passband of the
///   lowpasses falls as 1/(1 + k), as an analog ladder's does.
/// - Bypass returns the input untouched at no cost; leaving bypass starts
///   the filter from rest (reset()).
/// - For a voice that skips what can't be heard (M1.4 task 5, docs/dsp/
///   part-solo-and-skipping.md): skip() advances the grid and the ramps
///   without solving, which on a filter at rest fed zeros is process() to
///   the bit; copyChannel() sets one channel's state from the other's, to
///   leave a mono path.
///
/// Up to two channels (a stereo voice), filtered independently with shared
/// controls. The DSP takes physical units (Hz, the loop gain k, a linear
/// drive gain); the knob laws are the parameter table's (M1.9). Satisfies
/// BlockProcessor; nothing allocates, and every state is flushed below
/// FLT_MIN, as is the output. prepare() and reset() finish every ramp.
/// Doc: docs/dsp/filters.md, docs/dsp/filter-stability.md
namespace anoesis::dsp
{

/// The responses. Bypass is not a response: see LadderFilter::setBypassed.
enum class FilterResponse : std::uint8_t
{
    lp24, ///< 1/(1+s)^4
    lp12, ///< 1/(1+s)^2, in the four-pole loop
    bp24, ///< 4 s^2/(1+s)^4, unit peak at k = 0
    bp12, ///< 2 s/(1+s)^2, unit peak at k = 0
    hp24, ///< s^4/(1+s)^4
    hp12, ///< s^2/(1+s)^2
};

inline constexpr std::size_t filterResponseCount = 6;

/// The weights of u, y1, y2, y3, y4 for one response.
using FilterTapMix = std::array<float, 5>;

/// Each response as a mix of the loop input and the stage outputs, indexed
/// by FilterResponse. With p = 1 + s and D = p^4 + k, u = x p^4 / D and
/// y_i = x p^(4-i) / D, so a mix is (sum c_i p^(4-i)) / D: the numerators
/// below over the shared, resonant denominator.
inline constexpr std::array<FilterTapMix, filterResponseCount> filterTapMixes{
    FilterTapMix{0.0f, 0.0f, 0.0f, 0.0f, 1.0f},   // LP24: 1
    FilterTapMix{0.0f, 0.0f, 1.0f, 0.0f, 0.0f},   // LP12: p^2
    FilterTapMix{0.0f, 0.0f, 4.0f, -8.0f, 4.0f},  // BP24: 4 (p - 1)^2
    FilterTapMix{0.0f, 2.0f, -2.0f, 0.0f, 0.0f},  // BP12: 2 p (p - 1)
    FilterTapMix{1.0f, -4.0f, 6.0f, -4.0f, 1.0f}, // HP24: (p - 1)^4
    FilterTapMix{1.0f, -2.0f, 1.0f, 0.0f, 0.0f},  // HP12: p^2 (p - 1)^2
};

class LadderFilter
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

    LadderFilter() noexcept
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

    /// `numSamples` samples without solving (docs/dsp/part-solo-and-
    /// skipping.md): the grid advances, taking the controls and modulations
    /// at its grid points as process() would, and the per-sample ramps
    /// (resonance, drive, the response's crossfade) move on, but the state is
    /// left as it was and nothing is written. On a filter at rest
    /// (isAtRest()) whose input would be zeros, that is process() to the bit:
    /// silence in, silence out, the state still at rest. Otherwise the state
    /// is frozen, for a voice that skips a filter it can't hear. Nothing
    /// while bypassed, as process().
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

    /// Channel `to`'s state set from channel `from`'s (indices past the last
    /// channel are the last). A voice leaving its mono path, where only
    /// channel 0 has run, sets channel 1 from it: the two were equal until
    /// then (docs/dsp/voice-architecture.md, question 4).
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

} // namespace anoesis::dsp
