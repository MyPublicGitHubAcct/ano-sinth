// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Each source's bound over a stretch of samples (M1.5 task 2, stage 1;
// docs/dsp/modulation.md, deliverable 6). The skip test
// (docs/dsp/part-solo-and-skipping.md) takes, for each route to a factor it
// reads, amount x the source's interval over the coming segment. Too large
// only costs CPU; too small skips audio. Candidates, by kind:
//
// - linear (velocity over a glide, keytrack, a global ramp, a constant):
//   its two ends. Exact while nothing jumps (an event re-tests).
// - envelope (an Adsr): its range [0, 1]; PLAN's rule (0 when idle or held
//   at exactly 0, else its range); or by stage (between its level and where
//   the stage heads: 1 in the attack, the sustain in the decay and sustain,
//   0 in the release).
// - LFO: its range (+-1.125: half of gain x (wave + offset), with a slew
//   0.25 past +-1); the wave's ends and crests over the phases the segment
//   covers (PLAN's "its ends, or its crest", ignoring slews, the candidate
//   expected to be too small); or exact, the segment rendered ahead at the
//   grid point and its minimum and maximum taken.
//
// Prototypes for measurement, not library code.

#pragma once

#include "modulation_model.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/oscillators/waveform.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace anoesis::test::modulation
{

namespace dsp = anoesis::dsp;

/// What an envelope's bound reads: its stage, its level now, and its
/// sustain setting (where a decay or the sustain's ramp heads).
struct EnvelopeState
{
    dsp::AdsrStage stage = dsp::AdsrStage::idle;
    float level = 0.0f;
    float sustain = 0.0f;
};

[[nodiscard]] inline EnvelopeState envelopeState(const dsp::Adsr& e) noexcept
{
    return {.stage = e.stage(), .level = e.level(), .sustain = e.sustainLevel()};
}

/// The range: [0, 1].
[[nodiscard]] constexpr Interval envelopeRangeBound(const EnvelopeState& /*state*/) noexcept
{
    return {.lo = 0.0f, .hi = 1.0f};
}

/// PLAN's rule (M1.4 task 5): 0 when idle or held in sustain at exactly 0
/// with a sustain of 0, else the range.
[[nodiscard]] constexpr Interval envelopePlanBound(const EnvelopeState& s) noexcept
{
    const bool zeroHold = s.stage == dsp::AdsrStage::sustain && s.level <= 0.0f && s.sustain <= 0.0f;
    if (s.stage == dsp::AdsrStage::idle || zeroHold)
    {
        return {};
    }
    return {.lo = 0.0f, .hi = 1.0f};
}

/// By stage, until the next event: between the level and where the stage
/// heads. The attack heads for 1 and then decays towards the sustain, the
/// decay and the sustain's ramp head for the sustain, the release for 0.
[[nodiscard]] constexpr Interval envelopeStageBound(const EnvelopeState& s) noexcept
{
    const auto between = [](float a, float b)
    {
        return Interval{.lo = a < b ? a : b, .hi = a > b ? a : b};
    };
    switch (s.stage)
    {
    case dsp::AdsrStage::idle:
        return {};
    case dsp::AdsrStage::attack:
        return {.lo = s.level < s.sustain ? s.level : s.sustain, .hi = 1.0f};
    case dsp::AdsrStage::decay:
    case dsp::AdsrStage::sustain:
        return between(s.level, s.sustain);
    case dsp::AdsrStage::release:
        return between(s.level, 0.0f);
    }
    return {.lo = 0.0f, .hi = 1.0f};
}

/// A source that is linear between events: its ends.
[[nodiscard]] constexpr Interval linearBound(float first, float last) noexcept
{
    return {.lo = first < last ? first : last, .hi = first > last ? first : last};
}

/// The LFO source (M6): 0.5 x gain x (wave + offset), for gain in `gain`
/// (>= 0), the wave in `wave` and the offset in `offset`.
[[nodiscard]] constexpr Interval lfoSourceBound(Interval wave, Interval gain, Interval offset) noexcept
{
    const Interval sum = wave + offset;
    // gain >= 0: the product's ends are among the four corner products.
    const float a = gain.lo * sum.lo;
    const float b = gain.lo * sum.hi;
    const float c = gain.hi * sum.lo;
    const float d = gain.hi * sum.hi;
    const float lo = std::min({a, b, c, d});
    const float hi = std::max({a, b, c, d});
    return {.lo = 0.5f * lo, .hi = 0.5f * hi};
}

/// The wave's range with a slew: +-1.25 (lfo.hpp).
inline constexpr Interval lfoWaveRange{.lo = -1.25f, .hi = 1.25f};

/// The wave's minimum and maximum over `n` rendered samples: exact.
[[nodiscard]] inline Interval renderedBound(std::span<const float> values) noexcept
{
    if (values.empty())
    {
        return {};
    }
    const auto [lo, hi] = std::ranges::minmax(values);
    return {.lo = lo, .hi = hi};
}

/// The wave's ends and crests over the phases the next `n` samples cover,
/// read from the waveform's row with no slew (PLAN's "its ends, or its crest
/// if one falls inside"). A curved row is interpolated from grid points up
/// to one grid segment ahead, so the span is extended by that much. Random
/// and Lorenz rows: the wave's range. Expected to be too small across a
/// slewed jump.
[[nodiscard]] inline Interval lfoEndsAndCrestBound(const dsp::Lfo& lfo, std::size_t n) noexcept
{
    const auto& row = dsp::lfoShape(lfo.waveform());
    if (row.kind != dsp::LfoKind::periodic)
    {
        return lfoWaveRange;
    }
    const std::uint64_t inc = lfo.phaseIncrement();
    const std::uint64_t ahead = row.isCurved() ? dsp::Lfo::gridSamples : 0;
    const std::uint64_t span = inc * (static_cast<std::uint64_t>(n) + ahead);
    const auto top = [](std::uint64_t p)
    {
        return static_cast<std::uint32_t>(p >> 32U);
    };
    const std::uint64_t start = lfo.phase();
    float lo = row.wave.valueAt(top(start));
    float hi = lo;
    const auto take = [&](float v)
    {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    };
    take(row.wave.valueAt(top(start + span)));
    if (span >= (std::uint64_t{1} << 63U))
    {
        // Half a cycle or more: assume every value.
        return {.lo = std::min(lo, -1.0f), .hi = std::max(hi, 1.0f)};
    }
    // Corners (and the sine's crests) inside the span.
    for (const auto& corner : row.wave.corners())
    {
        const std::uint64_t at = static_cast<std::uint64_t>(corner.phase) << 32U;
        if (at - start <= span)
        {
            take(row.wave.valueAt(corner.phase));
            take(row.wave.valueAt(corner.phase - 1U)); // the value just before a jump
        }
    }
    if (row.wave.sineLevel() > 0.0f || row.wave.sineLevel() < 0.0f)
    {
        for (const std::uint32_t crest : {dsp::quarterCycle, 3U * dsp::quarterCycle})
        {
            if ((static_cast<std::uint64_t>(crest) << 32U) - start <= span)
            {
                take(row.wave.valueAt(crest));
            }
        }
    }
    return {.lo = lo, .hi = hi};
}

} // namespace anoesis::test::modulation
