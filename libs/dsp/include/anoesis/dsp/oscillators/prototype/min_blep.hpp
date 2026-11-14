// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/oscillators/prototype/common.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/// D6 prototype: minBLEP / minBLAMP (Brandt, 2001).
///
/// Like PolyBLEP, a residual is added at each jump, but the band-limited step
/// comes from a long windowed sinc made *minimum phase*, so it is causal: it
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
/// Doc: docs/dsp/band-limited-oscillators.md
namespace anoesis::dsp::prototype
{

/// The step and compensated ramp residual tables, laid out by phase:
/// row p (0..phases()) holds taps 0..taps()-1 at x = tap + p / phases()
/// samples after the edge. Built once at construction (allocates).
class MinBlepTable
{
public:
    /// `taps`: residual length in samples (a power of two). The kernel is a
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

    /// `table.taps()` must equal Taps. The table must outlive the oscillator.
    explicit MinBlepOscillator(const MinBlepTable& table) noexcept : table(&table) {}

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
    }

    void setShape(Shape newShape) noexcept
    {
        shape = newShape;
    }

    void setPulseWidth(float width) noexcept
    {
        widthPhase = widthToPhase(width);
    }

    void setFrequency(float hz) noexcept
    {
        setIncrement(incrementFor(hz, sampleRate));
    }

    void setIncrement(std::uint32_t newIncrement) noexcept
    {
        increment = newIncrement < halfCycle ? newIncrement : halfCycle;
    }

    /// The kernel's group delay: edges and slopes come out this late.
    [[nodiscard]] float latencySamples() const noexcept
    {
        return table->delay();
    }

    [[nodiscard]] float next() noexcept
    {
        // Edges crossed on the way to this sample, with the increment that
        // brought us here. None on the first sample, which is the onset.
        const auto step = started ? arrivedBy : 0U;
        const float dt = toCycles(increment);
        const float t = toCycles(phase);
        float naive = 0.0f;
        float slope = 0.0f; // per sample
        switch (shape)
        {
        case Shape::saw:
            jumpIfCrossed(0, step, -2.0f);
            naive = naiveSaw(t);
            slope = 2.0f * dt;
            break;
        case Shape::pulse:
            jumpIfCrossed(0, step, 2.0f);
            jumpIfCrossed(widthPhase, step, -2.0f);
            naive = naivePulse(phase, widthPhase);
            break;
        case Shape::triangle:
        {
            const float slopeChange = 8.0f * toCycles(step);
            bendIfCrossed(0, step, slopeChange);
            bendIfCrossed(halfCycle, step, -slopeChange);
            naive = naiveTriangle(t);
            slope = t < 0.5f ? 4.0f * dt : -4.0f * dt;
            break;
        }
        }
        if (!started)
        {
            addResidual(&MinBlepTable::stepRow, 0.0f, naive);
            addResidual(&MinBlepTable::rampRow, 0.0f, slope);
        }
        float y = naive - (slope * table->delay());
        y += ring[position];
        ring[position] = 0.0f;
        position = (position + 1) & (Taps - 1);
        phase += increment;
        arrivedBy = increment;
        started = true;
        return y;
    }

    /// Per-oscillator state, in bytes.
    static constexpr std::size_t stateBytes() noexcept
    {
        return sizeof(MinBlepOscillator);
    }

private:
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

    void jumpIfCrossed(std::uint32_t edgePhase, std::uint32_t stepTaken, float height) noexcept
    {
        float since = 0.0f;
        if (crossed(edgePhase, stepTaken, since))
        {
            addResidual(&MinBlepTable::stepRow, since, height);
        }
    }

    void bendIfCrossed(std::uint32_t edgePhase, std::uint32_t stepTaken, float slopeChange) noexcept
    {
        float since = 0.0f;
        if (crossed(edgePhase, stepTaken, since))
        {
            addResidual(&MinBlepTable::rampRow, since, slopeChange);
        }
    }

    const MinBlepTable* table;
    float sampleRate = 48000.0f;
    std::uint32_t phase = 0;
    std::uint32_t increment = 0;
    std::uint32_t arrivedBy = 0;
    std::uint32_t widthPhase = halfCycle;
    std::size_t position = 0;
    bool started = false;
    Shape shape = Shape::saw;
    std::array<float, Taps> ring{};
};

} // namespace anoesis::dsp::prototype
