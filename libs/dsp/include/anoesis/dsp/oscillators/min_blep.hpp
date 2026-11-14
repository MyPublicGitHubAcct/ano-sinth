// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/oscillators/waveform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

/// Band-limited oscillator: minBLEP / minBLAMP (Brandt, 2001), the method
/// chosen in decision D6 for its alias rejection (-91 dB below 20 kHz at
/// 44.1 kHz with 64 taps, no oversampling) and flat passband.
///
/// A residual is added at each jump: the difference between a band-limited
/// step and the naive one. The band-limited step comes from a long windowed
/// sinc made *minimum phase*, so it is causal: it
/// starts at the jump and rings only afterwards. Each jump adds `Taps`
/// residual samples into a small ring buffer; each output sample reads one.
/// Residuals come from an oversampled table (phases() points per sample),
/// linearly interpolated at the jump's sub-sample position.
///
/// Minimum phase delays the band-limited edge by the filter's group delay
/// (delay(), about 1-2 samples), while the naive straight segments are not
/// delayed. Left alone, that would offset every sloped segment by
/// -slope x delay: a pitch-dependent DC on the saw, and a stair-step on the
/// triangle. So the output subtracts slope x delay, and the BLAMP table is the
/// integrated step residual *plus* the delay, which brings it to zero at the
/// last tap. The output is then exactly the waveform filtered by the
/// minimum-phase kernel, delayed by latencySamples().
///
/// That compensation assumes the residuals of earlier edges are in the
/// buffer, which after reset() they are not. So the first sample after a
/// reset is a band-limited *onset*: silence before, the waveform after, with
/// the jump and the bend from silence added like any other edge. The output
/// starts at 0 and needs no history.
///
/// The waveform is data (a WaveShape: corners with jumps and bends, see
/// waveform.hpp), so the per-sample loop is the same for every waveform. A
/// sine part has no edges; slope x delay would only delay it to first order,
/// so it is evaluated at the phase latencySamples() ago instead, read from a
/// short phase history.
///
/// Docs: docs/dsp/band-limited-oscillators.md, docs/dsp/oscillator-waveforms.md
namespace anoesis::dsp
{

/// The residual length chosen in D6. 32 taps (-66.5 dB, half the work per
/// edge and half the memory) is the candidate for a cheaper real-time mode.
inline constexpr std::size_t minBlepTaps = 64;

/// The step and compensated ramp residual tables, laid out by phase:
/// row p (0..phases()) holds taps 0..taps()-1 at x = tap + p / phases()
/// samples after the edge. Built once at construction (allocates).
class MinBlepTable
{
public:
    /// `taps`: residual length in samples (a power of two; minBlepTaps by
    /// decision D6). The kernel is a
    /// Kaiser-windowed sinc, cut off at Nyquist, `taps` samples long; its
    /// transition band is about +-4.6% of the sample rate around Nyquist
    /// at 32 taps (beta 4.6) and the same at 64 taps with deeper rejection
    /// (beta 9.3), aimed at 20-24.1 kHz at 44.1 kHz.
    explicit MinBlepTable(std::size_t taps, std::size_t phases = 64);

    [[nodiscard]] std::size_t taps() const noexcept
    {
        return tapCount;
    }

    [[nodiscard]] std::size_t phases() const noexcept
    {
        return phaseCount;
    }

    /// The minimum-phase kernel's group delay at DC, in samples.
    [[nodiscard]] float delay() const noexcept
    {
        return groupDelay;
    }

    [[nodiscard]] float step(std::size_t phase, std::size_t tap) const noexcept
    {
        return steps[(phase * tapCount) + tap];
    }

    [[nodiscard]] float ramp(std::size_t phase, std::size_t tap) const noexcept
    {
        return ramps[(phase * tapCount) + tap];
    }

    [[nodiscard]] const float* stepRow(std::size_t phase) const noexcept
    {
        return steps.data() + (phase * tapCount);
    }

    [[nodiscard]] const float* rampRow(std::size_t phase) const noexcept
    {
        return ramps.data() + (phase * tapCount);
    }

    /// Bytes of table data.
    [[nodiscard]] std::size_t bytes() const noexcept
    {
        return (steps.size() + ramps.size()) * sizeof(float);
    }

private:
    std::size_t tapCount;
    std::size_t phaseCount;
    float groupDelay = 0.0f;
    std::vector<float> steps;
    std::vector<float> ramps;
};

template <std::size_t Taps>
class MinBlepOscillator
{
public:
    static_assert(Taps >= 2 && (Taps & (Taps - 1)) == 0, "Taps must be a power of two");

    /// `residuals.taps()` must equal Taps. The table must outlive the oscillator.
    explicit MinBlepOscillator(const MinBlepTable& residuals) noexcept
        : table(&residuals),
          delay(residuals.delay()),
          sineDelayWhole(wholeSamples(residuals.delay())),
          sineDelayFraction(residuals.delay() - static_cast<float>(wholeSamples(residuals.delay())))
    {
    }

    void prepare(const ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        reset();
    }

    /// Restarts at phase 0, from silence (a band-limited onset).
    void reset() noexcept
    {
        phase = 0;
        ring.fill(0.0f);
        position = 0;
        started = false;
        untilCorner = 0;
        historyValid = false;
    }

    /// One of the periodic waveforms. Noise is not generated here (see
    /// isPeriodic()); selecting it gives silence.
    void setWaveform(Waveform newWaveform) noexcept
    {
        setShape(waveShape(newWaveform));
    }

    /// Any shape, for waveforms beyond the table. It must outlive its use.
    void setShape(const WaveShape& newShape) noexcept
    {
        shape = &newShape;
        sineLevel = newShape.sineLevel();
        hasSine = sineLevel < 0.0f || sineLevel > 0.0f;
        untilCorner = 0; // find the segment and the next corner on the next sample
        historyValid = false;
    }

    void setFrequency(float hz) noexcept
    {
        setIncrement(incrementFor(hz, sampleRate));
    }

    void setIncrement(std::uint32_t newIncrement) noexcept
    {
        increment = newIncrement < halfCycle ? newIncrement : halfCycle;
    }

    /// The phase step per sample (2^32 per cycle), after clamping to Nyquist:
    /// the exact pitch, phaseIncrement() x sampleRate / 2^32 Hz.
    [[nodiscard]] std::uint32_t phaseIncrement() const noexcept
    {
        return increment;
    }

    /// The kernel's group delay: edges and slopes come out this late.
    [[nodiscard]] float latencySamples() const noexcept
    {
        return table->delay();
    }

    [[nodiscard]] float next() noexcept
    {
        // Two versions of the loop body: with the sine's history and sin()
        // in the body, even untaken, a sample of the saw costs about 30% more
        // (measured on Apple clang).
        return hasSine ? nextSample<true>() : nextSample<false>();
    }

private:
    static constexpr std::uint32_t historySize = 8; // a power of two, > the delay + 1

    template <bool WithSine>
    [[nodiscard]] float nextSample() noexcept
    {
        // The step that brought us here: none on the first sample, which is
        // the onset. Most samples cross no corner; one compare tells.
        const auto step = started ? arrivedBy : 0U;
        if (step >= untilCorner)
        {
            scanCorners(step);
        }
        else
        {
            untilCorner -= step;
        }

        // The straight segments, undelayed: the residuals carry the delay.
        const float naive = segmentValue + (segmentSlope * toCycles(phase - segmentPhase));
        const float slope = segmentSlope * toCycles(increment); // per sample

        // The sine has no edges. It is delayed by evaluating it at the phase
        // latencySamples() ago, on the same trajectory the edges are timed by.
        const float sine = WithSine ? delayedSine() : 0.0f;

        if (!started)
        {
            // The onset: steps from silence to the waveform's value and
            // slope. The sine's bend is left uncorrected (see the doc).
            addResidual(&MinBlepTable::stepRow, 0.0f, naive + sine);
            addResidual(&MinBlepTable::rampRow, 0.0f, slope);
            started = true;
        }
        float y = naive - (slope * delay) + sine;
        y += ring[position];
        ring[position] = 0.0f;
        position = (position + 1) & (Taps - 1);
        phase += increment;
        arrivedBy = increment;
        return y;
    }

    // The sine is read from the phase history, wholeSamples + 1 back.
    [[nodiscard]] static std::uint32_t wholeSamples(float groupDelay) noexcept
    {
        const auto whole = static_cast<std::uint32_t>(groupDelay);
        return whole < historySize - 2 ? whole : historySize - 2;
    }

    // The sine at the phase latencySamples() ago, recording this sample's
    // phase in the history first.
    [[nodiscard]] float delayedSine() noexcept
    {
        if (!historyValid)
        {
            // As if it had run at this pitch forever.
            for (std::uint32_t k = 1; k < historySize; ++k)
            {
                history[(historyIndex - k) & (historySize - 1)] = phase - (k * increment);
            }
            historyValid = true;
        }
        history[historyIndex & (historySize - 1)] = phase;
        const float sine = sineLevel * std::sin(2.0f * std::numbers::pi_v<float> * toCycles(delayedPhase()));
        ++historyIndex;
        return sine;
    }

    // The phase latencySamples() before this sample. The phase moves
    // linearly between samples, so this interpolates between the two
    // samples around it, exactly.
    [[nodiscard]] std::uint32_t delayedPhase() const noexcept
    {
        const std::uint32_t later = history[(historyIndex - sineDelayWhole) & (historySize - 1)];
        const std::uint32_t earlier = history[(historyIndex - sineDelayWhole - 1) & (historySize - 1)];
        return later - static_cast<std::uint32_t>(sineDelayFraction * static_cast<float>(later - earlier));
    }

    // The slow path, on a sample where `step` crossed a corner (or after a
    // reset or a new shape): adds the residuals of every corner crossed,
    // finds the segment the phase is now in, and how far the next corner is.
    void scanCorners(std::uint32_t step) noexcept
    {
        const float stepCycles = toCycles(step);
        std::uint64_t nearest = std::numeric_limits<std::uint64_t>::max();
        for (const auto& corner : shape->corners())
        {
            float since = 0.0f;
            if (crossed(corner.phase, step, since))
            {
                if (corner.jumps)
                {
                    addResidual(&MinBlepTable::stepRow, since, corner.jump);
                }
                if (corner.bends)
                {
                    addResidual(&MinBlepTable::rampRow, since, corner.bend * stepCycles);
                }
            }
            // A corner exactly here has just been crossed (or is part of the
            // onset): the next time it comes round is a whole cycle away.
            const std::uint32_t ahead = corner.phase - phase;
            nearest = std::min(nearest, ahead == 0 ? std::uint64_t{1} << 32U : std::uint64_t{ahead});
        }
        untilCorner = nearest;
        if (shape->corners().empty())
        {
            segmentPhase = 0;
            segmentValue = 0.0f;
            segmentSlope = 0.0f;
            return;
        }
        const auto& segment = shape->cornerAt(phase);
        segmentPhase = segment.phase;
        segmentValue = segment.value;
        segmentSlope = segment.slope;
    }

    // Adds `height` x the residual `row` for an edge `since` samples ago.
    void addResidual(const float* (MinBlepTable::*rowOf)(std::size_t) const noexcept, float since,
                     float height) noexcept
    {
        const float scaled = since * static_cast<float>(table->phases());
        auto p = static_cast<std::size_t>(scaled);
        p = p < table->phases() ? p : table->phases() - 1;
        const float w = scaled - static_cast<float>(p);
        const float* r0 = (table->*rowOf)(p);
        const float* r1 = (table->*rowOf)(p + 1);
        for (std::size_t i = 0; i < Taps; ++i)
        {
            ring[(position + i) & (Taps - 1)] += height * (r0[i] + (w * (r1[i] - r0[i])));
        }
    }

    // `stepTaken` moved the phase onto this sample; did it cross `edgePhase`?
    [[nodiscard]] bool crossed(std::uint32_t edgePhase, std::uint32_t stepTaken, float& since) const noexcept
    {
        const std::uint32_t past = phase - edgePhase;
        if (stepTaken == 0 || past >= stepTaken)
        {
            return false;
        }
        since = toCycles(past) / toCycles(stepTaken);
        return true;
    }

    const MinBlepTable* table;
    float delay; // the table's, kept here: the per-sample path reads no table
    const WaveShape* shape = &waveShape(Waveform::saw);
    // The segment the phase is in, and the distance (in phase units) from the
    // previous sample's phase to the next corner; 0 forces a scan.
    std::uint32_t segmentPhase = 0;
    float segmentValue = 0.0f;
    float segmentSlope = 0.0f;
    std::uint64_t untilCorner = 0;
    float sineLevel = 0.0f;
    bool hasSine = false;
    bool historyValid = false;
    float sampleRate = 48000.0f;
    std::uint32_t phase = 0;
    std::uint32_t increment = 0;
    std::uint32_t arrivedBy = 0;
    std::uint32_t historyIndex = 0;
    std::uint32_t sineDelayWhole = 0;
    float sineDelayFraction = 0.0f;
    std::size_t position = 0;
    bool started = false;
    std::array<std::uint32_t, historySize> history{};
    std::array<float, Taps> ring{};
};

} // namespace anoesis::dsp
