// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

/// Each source's bound over a stretch of samples, and a target's bound from
/// its routes (M1.5 task 2, docs/dsp/modulation.md, recommendation 11).
///
/// The skip test (docs/dsp/part-solo-and-skipping.md) asks, for each factor
/// it reads (Mix, and the slots' Volume), how far the factor's modulation can
/// reach over the coming segment. Each route adds amount x its source's
/// interval there. A bound that is too large only costs CPU; one that is too
/// small skips audio. By the source's kind (registry.hpp):
///
/// - an envelope, by its stage until the next event: between its level and
///   where the stage heads (the attack heads for 1 and then decays towards
///   the sustain; the decay and the sustain's ramp head for the sustain; the
///   release for 0), exactly 0 when idle;
/// - an LFO, exactly: the minimum and maximum of its wave over the segment
///   rendered ahead (segment order renders it at the grid point), through
///   0.5 x gain x (wave + offset) with gain and offset taken as intervals
///   (a ramped gain lies between its two grid points' values);
/// - a linear source (velocity over a glide, keytrack, a global ramp, a
///   constant): its two ends, exact while nothing jumps (an event re-tests);
/// - any other source: its range; "None": exactly 0.
///
/// Pure functions: no state, no allocation, real-time safe.
/// Doc: docs/dsp/modulation.md
#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/modulation/routing.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace anoesis::dsp::modulation
{

/// A closed interval of values.
struct Interval
{
    float lo = 0.0f;
    float hi = 0.0f;

    [[nodiscard]] constexpr bool contains(float x) const noexcept
    {
        return x >= lo && x <= hi;
    }
};

[[nodiscard]] constexpr Interval operator+(Interval a, Interval b) noexcept
{
    return {.lo = a.lo + b.lo, .hi = a.hi + b.hi};
}

/// amount x [lo, hi].
[[nodiscard]] constexpr Interval scaled(Interval x, float amount) noexcept
{
    return amount >= 0.0f ? Interval{.lo = amount * x.lo, .hi = amount * x.hi}
                          : Interval{.lo = amount * x.hi, .hi = amount * x.lo};
}

/// What an envelope's bound reads: its stage, its level now, and its
/// sustain (where a decay or the sustain's ramp heads).
struct EnvelopeState
{
    AdsrStage stage = AdsrStage::idle;
    float level = 0.0f;
    float sustain = 0.0f;
};

[[nodiscard]] inline EnvelopeState envelopeState(const Adsr& e) noexcept
{
    return {.stage = e.stage(), .level = e.level(), .sustain = e.sustainLevel()};
}

/// An envelope's bound until its next event, by its stage.
[[nodiscard]] constexpr Interval envelopeBound(const EnvelopeState& s) noexcept
{
    const auto between = [](float a, float b)
    {
        return Interval{.lo = a < b ? a : b, .hi = a > b ? a : b};
    };
    switch (s.stage)
    {
    case AdsrStage::idle:
        return {};
    case AdsrStage::attack:
        return {.lo = s.level < s.sustain ? s.level : s.sustain, .hi = 1.0f};
    case AdsrStage::decay:
    case AdsrStage::sustain:
        return between(s.level, s.sustain);
    case AdsrStage::release:
        return between(s.level, 0.0f);
    }
    return {.lo = 0.0f, .hi = 1.0f};
}

/// A source that is linear between events: its two ends.
[[nodiscard]] constexpr Interval linearBound(float first, float last) noexcept
{
    return {.lo = first < last ? first : last, .hi = first > last ? first : last};
}

/// The minimum and maximum of a rendered wave (empty: 0).
[[nodiscard]] constexpr Interval waveBound(std::span<const float> wave) noexcept
{
    if (wave.empty())
    {
        return {};
    }
    Interval b{.lo = wave[0], .hi = wave[0]};
    for (const float w : wave.subspan(1))
    {
        b.lo = w < b.lo ? w : b.lo;
        b.hi = b.hi < w ? w : b.hi;
    }
    return b;
}

/// The LFO as a source, lfoSourceScale x gain x (wave + offset), over wave,
/// gain (>= 0) and offset each in an interval.
[[nodiscard]] constexpr Interval lfoBound(Interval wave, Interval gain, Interval offset) noexcept
{
    const Interval sum = wave + offset;
    // gain >= 0: the product's ends are among the four corner products.
    const std::array corners{gain.lo * sum.lo, gain.lo * sum.hi, gain.hi * sum.lo, gain.hi * sum.hi};
    Interval b{.lo = corners[0], .hi = corners[0]};
    for (const float c : std::span{corners}.subspan(1))
    {
        b.lo = c < b.lo ? c : b.lo;
        b.hi = b.hi < c ? c : b.hi;
    }
    return {.lo = lfoSourceScale * b.lo, .hi = lfoSourceScale * b.hi};
}

/// The LFO's value as a source: lfoSourceScale x gain x (wave + offset).
[[nodiscard]] constexpr float lfoSourceValue(float wave, float gain, float offset) noexcept
{
    return lfoSourceScale * gain * (wave + offset);
}

/// What a source's bound reads, filled by the voice at a grid point for the
/// coming stretch of samples.
struct BoundInputs
{
    Interval ends;          ///< a linear source's first and last value
    EnvelopeState envelope; ///< an envelope source's state
    Interval lfo;           ///< an LFO source's interval, already lfoBound()'s
};

/// The interval of a source with row `row`, by its kind.
[[nodiscard]] constexpr Interval boundOfRow(const SourceRow& row, const BoundInputs& in) noexcept
{
    switch (row.bound)
    {
    case BoundKind::none:
        return {};
    case BoundKind::linearEnds:
        return in.ends;
    case BoundKind::envelope:
        return envelopeBound(in.envelope);
    case BoundKind::lfo:
        return in.lfo;
    case BoundKind::range:
        return {.lo = row.min, .hi = row.max};
    }
    return {};
}

/// Source s's interval, by its kind.
[[nodiscard]] constexpr Interval sourceBound(Source s, const BoundInputs& in) noexcept
{
    return boundOfRow(sourceRow(s), in);
}

/// Each source's interval, indexed by Source.
using SourceBounds = std::array<Interval, sourceCount>;

/// The interval target t's modulation on part `part` lies in, given each
/// source's: the sum over the routes to it of amount x the source's interval.
/// Exactly 0 with no route there.
[[nodiscard]] constexpr Interval targetBound(const CompiledRoutes& routes, Target t, std::size_t part,
                                             const SourceBounds& bounds) noexcept
{
    Interval sum{};
    for (const auto& r : routes.live())
    {
        if (r.target == t && includes(r.parts, part))
        {
            sum = sum + scaled(bounds[static_cast<std::size_t>(r.source)], r.amount);
        }
    }
    return sum;
}

} // namespace anoesis::dsp::modulation
