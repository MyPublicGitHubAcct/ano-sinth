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
/// restart() starts that onset at any phase. A retrigger while the oscillator
/// sounds, or hard sync, is jumpTo(): the jump between the old and the new
/// phase is one more edge, at its sub-sample time, and what is in flight
/// stays.
///
/// The waveform is data (a WaveShape: corners with jumps and bends, see
/// waveform.hpp), so the per-sample loop is the same for every waveform. A
/// sine part has no edges; slope x delay would only delay it to first order,
/// so it is evaluated at the phase latencySamples() ago instead, read from a
/// short phase history.
///
/// Docs: docs/dsp/band-limited-oscillators.md, docs/dsp/oscillator-waveforms.md,
/// docs/dsp/oscillator-phase.md
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
        restart(0);
    }

    /// Restarts from silence at `startPhase` (2^32 per cycle): the next
    /// sample is a band-limited onset at that phase, and starts at exactly 0.
    /// Corners behind the start are not edges; a corner exactly at it is
    /// behind it (the square restarted at 1/2 starts low). Residuals in
    /// flight are dropped, so this is for an oscillator that is silent (a new
    /// voice). A note-on while it sounds is jumpTo().
    void restart(std::uint32_t startPhase) noexcept
    {
        phase = startPhase;
        ring.fill(0.0f);
        position = 0;
        started = false;
        untilCorner = 0;
        historyValid = false;
        jump = {};
    }

    /// Moves the phase to `newPhase` with a band-limited jump: a retrigger
    /// while the oscillator sounds, or hard sync. The jump happens `since`
    /// samples, in [0, 1), before the next sample, so the next sample is
    /// `newPhase` plus that much of a step. The jump in value and in slope
    /// between the old and the new phase are added as a step and a ramp
    /// residual at that sub-sample time, exactly like a corner's, and the
    /// residuals already in flight are kept: nothing is cut short, and the
    /// output never drops to silence. Corners the old phase crosses before
    /// the jump are edges as usual; corners at or behind `newPhase` are not.
    ///
    /// Call it between samples, at most once per sample. Before the first
    /// sample (see isRunning()) there is nothing to jump from, and it is
    /// restart() at the phase the next sample would have.
    ///
    /// The sine's part is band-limited in value and slope too. It is delayed
    /// by evaluating it latencySamples() late, so its naive jump comes out
    /// that much later than the straight segments' (see the doc).
    void jumpTo(std::uint32_t newPhase, float since = 0.0f) noexcept
    {
        constexpr float almostOne = 0x1.fffffep-1f;
        since = since > 0.0f ? std::min(since, almostOne) : 0.0f; // and NaN is 0
        if (!started)
        {
            restart(newPhase + partOf(increment, since));
            return;
        }

        // The phase moves by `step` between the last sample and the next.
        // The jump splits that step: the old phase moves on to `jumpedFrom`,
        // then the new one from `newPhase` to the next sample's phase.
        const std::uint32_t step = arrivedBy;
        const float stepCycles = toCycles(step);
        const std::uint32_t after = partOf(step, since);
        const std::uint32_t jumpedFrom = phase - after;
        addCrossings(jumpedFrom, step - after, step, since);

        const Point lineBefore = linearAt(jumpedFrom);
        const Point lineAfter = linearAt(newPhase);
        addEdge(since, lineAfter.value - lineBefore.value, (lineAfter.slope - lineBefore.slope) * stepCycles, false);
        if (hasSine)
        {
            const Point sineBefore = sineAt(jumpedFrom);
            const Point sineAfter = sineAt(newPhase);
            addEdge(since, sineAfter.value - sineBefore.value, (sineAfter.slope - sineBefore.slope) * stepCycles, true);
            // The history keeps the old trajectory up to the jump, so the
            // delayed sine switches when the jump comes out of the delay.
            jump = {.pending = true, .since = since, .oldPhase = phase, .step = step};
        }

        phase = newPhase + after;
        addCrossings(phase, after, step, 0.0f);

        // The next sample has nothing left to cross in this step. It finds
        // its segment and the next corner from the new phase.
        arrivedBy = 0;
        untilCorner = 0;
    }

    /// False from a reset() or restart() until the first sample: the
    /// oscillator is silent and has nothing in flight.
    [[nodiscard]] bool isRunning() const noexcept
    {
        return started;
    }

    /// The phase of the next sample, before the kernel's delay.
    [[nodiscard]] std::uint32_t nextPhase() const noexcept
    {
        return phase;
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
        jump = {};
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
                const std::uint32_t then = phase - (k * increment);
                history[(historyIndex - k) & (historySize - 1)] = {.phase = then, .oldPhase = then, .step = increment};
            }
            historyValid = true;
        }
        auto& now = history[historyIndex & (historySize - 1)];
        if (jump.pending)
        {
            // The step into this sample held a jump.
            now = {.phase = phase, .oldPhase = jump.oldPhase, .step = jump.step, .jumpedAt = jump.since};
            jump = {};
        }
        else
        {
            const std::uint32_t previous = history[(historyIndex - 1) & (historySize - 1)].phase;
            now = {.phase = phase, .oldPhase = phase, .step = phase - previous};
        }
        const float sine = sineLevel * std::sin(2.0f * std::numbers::pi_v<float> * toCycles(delayedPhase()));
        ++historyIndex;
        return sine;
    }

    // The phase latencySamples() before this sample. The phase moves
    // linearly between samples, so this goes back from the sample after that
    // time, exactly. If a jump came in between, a time before it is on the
    // old trajectory: back from where the old phase would have been.
    [[nodiscard]] std::uint32_t delayedPhase() const noexcept
    {
        const Past& later = history[(historyIndex - sineDelayWhole) & (historySize - 1)];
        const std::uint32_t from = sineDelayFraction <= later.jumpedAt ? later.phase : later.oldPhase;
        return from - static_cast<std::uint32_t>(sineDelayFraction * static_cast<float>(later.step));
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
            addCrossing(corner, phase, step, stepCycles, 0.0f);
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

    // If the last `span` units of phase before `end` crossed `corner`, adds
    // its residuals. The phase moves `stepCycles` per sample, and reached
    // `end` `offset` samples before the next sample: offset is 0 on the
    // per-sample path, and the jump's time when a jump splits the step.
    void addCrossing(const WaveCorner& corner, std::uint32_t end, std::uint32_t span, float stepCycles,
                     float offset) noexcept
    {
        const std::uint32_t past = end - corner.phase;
        if (past >= span)
        {
            return; // includes span == 0: no movement crosses nothing
        }
        const float since = offset + (toCycles(past) / stepCycles);
        if (corner.jumps)
        {
            addResidual(&MinBlepTable::stepRow, since, corner.jump);
        }
        if (corner.bends)
        {
            addResidual(&MinBlepTable::rampRow, since, corner.bend * stepCycles);
        }
    }

    // addCrossing() for every corner.
    void addCrossings(std::uint32_t end, std::uint32_t span, std::uint32_t step, float offset) noexcept
    {
        const float stepCycles = toCycles(step);
        for (const auto& corner : shape->corners())
        {
            addCrossing(corner, end, span, stepCycles, offset);
        }
    }

    // The part of `step` covered in the last `since` samples of it, rounded
    // down. Integer arithmetic: since x 2^32 is exact for since >= 2^-8 (a
    // float has 24 bits), and the product fits in 64 bits.
    [[nodiscard]] static std::uint32_t partOf(std::uint32_t step, float since) noexcept
    {
        const auto fraction = static_cast<std::uint64_t>(since * 4294967296.0f);
        return static_cast<std::uint32_t>((fraction * step) >> 32U);
    }

    // A point on the naive waveform, undelayed: its value, and its slope per
    // cycle, just after any corner there.
    struct Point
    {
        float value = 0.0f;
        float slope = 0.0f;
    };

    // The straight segments' part.
    [[nodiscard]] Point linearAt(std::uint32_t at) const noexcept
    {
        if (shape->corners().empty())
        {
            return {};
        }
        const auto& corner = shape->cornerAt(at);
        return {.value = corner.value + (corner.slope * toCycles(at - corner.phase)), .slope = corner.slope};
    }

    // The sine's part.
    [[nodiscard]] Point sineAt(std::uint32_t at) const noexcept
    {
        constexpr float twoPi = 2.0f * std::numbers::pi_v<float>;
        const float angle = twoPi * toCycles(at);
        return {.value = sineLevel * std::sin(angle), .slope = twoPi * sineLevel * std::cos(angle)};
    }

    // An edge `since` samples before this sample, with a jump in value and
    // a bend (the jump in slope, per sample). The straight segments jump at
    // once, which the table's residuals assume. The sine is evaluated
    // latencySamples() late, so its naive jump and bend come out that late:
    // until then each residual tap is 1 (the step) or x - delay (the ramp)
    // higher, x being the tap's time since the edge.
    void addEdge(float since, float jumpBy, float bendBy, bool delayedNaive) noexcept
    {
        const bool jumps = jumpBy < 0.0f || jumpBy > 0.0f;
        const bool bends = bendBy < 0.0f || bendBy > 0.0f;
        if (jumps)
        {
            addResidual(&MinBlepTable::stepRow, since, jumpBy);
        }
        if (bends)
        {
            addResidual(&MinBlepTable::rampRow, since, bendBy);
        }
        if (!delayedNaive || !(jumps || bends))
        {
            return;
        }
        for (std::size_t i = 0; i < Taps; ++i)
        {
            const float x = since + static_cast<float>(i);
            if (x >= delay)
            {
                break;
            }
            ring[(position + i) & (Taps - 1)] += jumpBy + (bendBy * (x - delay));
        }
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
    // A sample's phase in the history, with the step that led to it. When a
    // jump came `jumpedAt` samples before it, `oldPhase` is where the old
    // trajectory would have been; otherwise it is `phase`.
    struct Past
    {
        std::uint32_t phase = 0;
        std::uint32_t oldPhase = 0;
        std::uint32_t step = 0;
        float jumpedAt = 0.0f;
    };
    // A jump waiting to be recorded in the history by the next sample.
    struct PendingJump
    {
        bool pending = false;
        float since = 0.0f;
        std::uint32_t oldPhase = 0;
        std::uint32_t step = 0;
    };
    PendingJump jump;
    std::array<Past, historySize> history{};
    std::array<float, Taps> ring{};
};

} // namespace anoesis::dsp
