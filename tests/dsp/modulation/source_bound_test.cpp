// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Tests for each source's bound against the source's own values (libs/dsp
// modulation/source_bound.hpp; docs/dsp/modulation.md, as built), over
// random segments at 44.1, 48 and 96 kHz, with the host's blocks splitting
// them:
//
// - an envelope by its stage, over 36,000 stretches of 1-32 samples (12,000
//   at each rate) with random gates and sustain changes between them;
// - an LFO exactly, over 79,200 grid segments (26,400 at each rate), in
//   segment order as the voice will run it: rendered ahead at each grid
//   point, the gain ramped from the previous point's value (the prototype
//   voice's ramp), the offset held, retriggers mid-segment rendering the rest
//   again from a copy taken at the grid point; the bound taken for each
//   chunk the host's blocks of 1, 37 or 4096 make;
// - and a bound that is too small fails the same checks: an envelope's level
//   alone, an LFO's two ends, the exact LFO bound one ulp narrower.

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/modulation/source_bound.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace dsp = anoesis::dsp;
namespace mod = anoesis::dsp::modulation;

namespace
{

/// Exactly equal, written so -Wfloat-equal accepts it.
bool sameValue(float a, float b)
{
    return a >= b && a <= b;
}

constexpr std::array sampleRates{44100.0f, 48000.0f, 96000.0f};
constexpr std::size_t grid = dsp::Lfo::gridSamples;

/// Which bound the checks take.
enum class Policy : std::uint8_t
{
    asBuilt,    ///< the library's
    tooSmall,   ///< a bound expected to be too small
    oneUlpLess, ///< the library's, one ulp narrower at each end (LFO only)
};

struct Tally
{
    std::size_t stretches = 0;
    std::size_t samples = 0;
    std::size_t violations = 0;
    /// Held-gain chunks whose bound equals the values' minimum and maximum.
    std::size_t exact = 0;
    std::size_t heldChunks = 0;
    /// LFO samples whose value isn't 0, so the checks aren't vacuous.
    std::size_t nonZero = 0;
};

Tally envelopeTrials(Policy policy)
{
    Tally tally;
    std::uint64_t seed = 11;
    for (const float fs : sampleRates)
    {
        dsp::Random random{seed++};
        const dsp::ProcessSpec spec{.sampleRate = fs, .maxBlockSize = 4096, .numChannels = 1};
        for (int trial = 0; trial < 60; ++trial)
        {
            dsp::Adsr e;
            e.prepare(spec);
            e.setAttack(0.0005f + (0.05f * random.nextUnipolar()));
            e.setDecay(0.0005f + (0.05f * random.nextUnipolar()));
            e.setSustain(trial % 4 == 0 ? 0.0f : random.nextUnipolar());
            e.setRelease(0.0005f + (0.05f * random.nextUnipolar()));
            e.gateOn();
            // The host's blocks split each stretch.
            const std::size_t block = std::array<std::size_t, 3>{1, 7, 32}[static_cast<std::size_t>(trial) % 3];
            std::array<float, grid> y{};
            for (int seg = 0; seg < 200; ++seg)
            {
                // Events between stretches only: a gate, or a sustain change.
                const auto r = random.nextUint32() % 40;
                if (r == 0)
                {
                    e.gateOff();
                }
                else if (r == 1)
                {
                    e.gateOn();
                }
                else if (r == 2)
                {
                    e.setSustain(random.nextUnipolar());
                }
                const auto n = 1 + (random.nextUint32() % grid);
                const auto state = mod::envelopeState(e);
                const auto bound = policy == Policy::asBuilt ? mod::envelopeBound(state)
                                                             : mod::Interval{.lo = state.level, .hi = state.level};
                for (std::size_t done = 0; done < n; done += block)
                {
                    e.process(std::span{y}.subspan(done, std::min(block, n - done)));
                }
                for (std::size_t i = 0; i < n; ++i)
                {
                    tally.violations += bound.contains(y[i]) ? 0U : 1U;
                }
                tally.samples += n;
                ++tally.stretches;
            }
        }
    }
    return tally;
}

Tally lfoTrials(Policy policy)
{
    Tally tally;
    std::uint64_t seed = 13;
    for (const float fs : sampleRates)
    {
        dsp::Random random{seed++};
        const dsp::ProcessSpec spec{.sampleRate = fs, .maxBlockSize = 4096, .numChannels = 1};
        for (int trial = 0; trial < 88; ++trial)
        {
            dsp::Lfo lfo{static_cast<std::uint64_t>(trial)};
            lfo.prepare(spec);
            lfo.setWaveform(static_cast<dsp::LfoWaveform>(static_cast<std::size_t>(trial) % dsp::lfoWaveformCount));
            lfo.setFrequency(0.1f * std::exp2(13.0f * random.nextUnipolar()));
            // A twin run in the host's chunks, retriggered at the same samples:
            // rendering ahead gives the wave the host's blocks would.
            dsp::Lfo twin = lfo;
            const std::size_t block = std::array<std::size_t, 3>{1, 37, 4096}[static_cast<std::size_t>(trial) % 3];
            const bool ramped = trial % 2 == 0;
            float gainNow = random.nextUnipolar();
            const float offset = random.nextBipolar();
            std::uint64_t time = 0;
            std::array<float, grid> wave{};
            std::array<float, grid> twinWave{};
            for (int seg = 0; seg < 300; ++seg)
            {
                // The grid point: the gain from the sources as they stand,
                // ramped from the previous point's value, the wave rendered ahead.
                const float gainBefore = gainNow;
                if (random.nextUint32() % 3 != 0)
                {
                    gainNow = random.nextUnipolar();
                }
                const dsp::Lfo atGridPoint = lfo;
                lfo.process(wave);
                // A retrigger somewhere in a fiftieth of the segments.
                const std::size_t retriggerAt = random.nextUint32() % 50 == 0 ? random.nextUint32() % grid : grid;
                std::size_t pos = 0;
                while (pos < grid)
                {
                    if (pos == retriggerAt)
                    {
                        lfo = atGridPoint;
                        std::array<float, grid> scratch{};
                        lfo.process(std::span{scratch}.first(pos));
                        lfo.retrigger();
                        lfo.process(std::span{wave}.subspan(pos));
                        twin.retrigger();
                    }
                    const std::size_t toBlockEnd = block - static_cast<std::size_t>(time % block);
                    const std::size_t toEvent = retriggerAt > pos ? retriggerAt - pos : grid - pos;
                    const std::size_t len = std::min({toBlockEnd, toEvent, grid - pos});
                    const auto chunk = std::span<const float>{wave}.subspan(pos, len);
                    twin.process(std::span{twinWave}.subspan(pos, len));
                    const mod::Interval gain =
                        ramped ? mod::linearBound(gainBefore, gainNow) : mod::Interval{.lo = gainNow, .hi = gainNow};
                    mod::Interval bound{};
                    switch (policy)
                    {
                    case Policy::asBuilt:
                        bound = mod::lfoBound(mod::waveBound(chunk), gain, {.lo = offset, .hi = offset});
                        break;
                    case Policy::tooSmall:
                        bound = mod::lfoBound(mod::linearBound(chunk.front(), chunk.back()), gain,
                                              {.lo = offset, .hi = offset});
                        break;
                    case Policy::oneUlpLess:
                    {
                        const auto b = mod::lfoBound(mod::waveBound(chunk), gain, {.lo = offset, .hi = offset});
                        bound = {
                            .lo = std::nextafter(b.lo, std::numeric_limits<float>::infinity()),
                            .hi = std::nextafter(b.hi, -std::numeric_limits<float>::infinity()),
                        };
                        break;
                    }
                    }
                    float lo = std::numeric_limits<float>::infinity();
                    float hi = -lo;
                    for (std::size_t i = 0; i < len; ++i)
                    {
                        // The prototype voice's ramp (modulation_candidates.hpp).
                        const float t = static_cast<float>(pos + i) / static_cast<float>(grid);
                        const float g = ramped ? gainBefore + ((gainNow - gainBefore) * t) : gainNow;
                        // The source's value as the prototype voice computes it
                        // (lfoSourceValue() is checked against it in routing_test.cpp).
                        const float v = 0.5f * g * (chunk[i] + offset);
                        tally.nonZero += v < 0.0f || v > 0.0f ? 1U : 0U;
                        tally.violations += bound.contains(v) ? 0U : 1U;
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                        // Every value lies in the LFO's range as a source.
                        tally.violations += v >= -mod::lfoSourceLimit && v <= mod::lfoSourceLimit ? 0U : 1U;
                    }
                    if (!ramped)
                    {
                        ++tally.heldChunks;
                        const bool same = sameValue(lo, bound.lo) && sameValue(hi, bound.hi);
                        tally.exact += same ? 1U : 0U;
                    }
                    tally.samples += len;
                    ++tally.stretches;
                    pos += len;
                    time += len;
                }
                // The twin, run in the host's chunks, made the same wave.
                for (std::size_t i = 0; i < grid; ++i)
                {
                    tally.violations += sameValue(twinWave[i], wave[i]) ? 0U : 1U;
                }
            }
        }
    }
    return tally;
}

} // namespace

TEST_CASE("Modulation bounds: an envelope's bound by stage holds over 36,000 random stretches", "[modulation]")
{
    const auto t = envelopeTrials(Policy::asBuilt);
    CHECK(t.stretches == 36000);
    CHECK(t.samples > 36000 * 15);
    CHECK(t.violations == 0);
}

TEST_CASE("Modulation bounds: an LFO's bound from its segment rendered ahead is exact over 79,200 segments",
          "[modulation]")
{
    const auto t = lfoTrials(Policy::asBuilt);
    CHECK(t.samples == 79200 * grid);
    CHECK(t.stretches >= 79200);
    CHECK(t.violations == 0);
    CHECK(t.nonZero > t.samples / 2);
    // Exact: with the gain held, the bound is the values' minimum and maximum.
    CHECK(t.heldChunks > 10000);
    CHECK(t.exact == t.heldChunks);
}

TEST_CASE("Modulation bounds: a bound that is too small fails the same checks", "[modulation]")
{
    SECTION("an envelope's level alone")
    {
        const auto t = envelopeTrials(Policy::tooSmall);
        CHECK(t.violations > 1000);
    }
    SECTION("an LFO's two ends, missing its crests and slews")
    {
        const auto t = lfoTrials(Policy::tooSmall);
        CHECK(t.violations > 1000);
    }
    SECTION("the exact LFO bound, one ulp narrower")
    {
        const auto t = lfoTrials(Policy::oneUlpLess);
        CHECK(t.violations > 1000);
    }
}

TEST_CASE("Modulation bounds: an envelope's bound by each stage", "[modulation]")
{
    using S = dsp::AdsrStage;
    const auto b = [](S stage, float level, float sustain)
    {
        return mod::envelopeBound({.stage = stage, .level = level, .sustain = sustain});
    };
    const auto is = [](mod::Interval x, float lo, float hi)
    {
        return sameValue(x.lo, lo) && sameValue(x.hi, hi);
    };
    CHECK(is(b(S::idle, 0.0f, 0.5f), 0.0f, 0.0f));
    // The attack heads for 1, then decays towards the sustain.
    CHECK(is(b(S::attack, 0.2f, 0.5f), 0.2f, 1.0f));
    CHECK(is(b(S::attack, 0.7f, 0.5f), 0.5f, 1.0f));
    // The decay and the sustain's ramp head for the sustain.
    CHECK(is(b(S::decay, 0.9f, 0.3f), 0.3f, 0.9f));
    CHECK(is(b(S::sustain, 0.3f, 0.6f), 0.3f, 0.6f));
    // The release for 0.
    CHECK(is(b(S::release, 0.4f, 0.6f), 0.0f, 0.4f));
}

TEST_CASE("Modulation bounds: the LFO's bound takes gain and offset as intervals", "[modulation]")
{
    const auto is = [](mod::Interval x, float lo, float hi)
    {
        return sameValue(x.lo, lo) && sameValue(x.hi, hi);
    };
    // A sine at Gain 10 is +-0.5 (M6).
    CHECK(is(mod::lfoBound({.lo = -1.0f, .hi = 1.0f}, {.lo = 1.0f, .hi = 1.0f}, {}), -0.5f, 0.5f));
    // Offset +10: 0 to +1.
    CHECK(is(mod::lfoBound({.lo = -1.0f, .hi = 1.0f}, {.lo = 1.0f, .hi = 1.0f}, {.lo = 1.0f, .hi = 1.0f}), 0.0f, 1.0f));
    // A gain ramping from 0 to 1 over a wave at 0.5 to 1: 0 to 0.5.
    CHECK(is(mod::lfoBound({.lo = 0.5f, .hi = 1.0f}, {.lo = 0.0f, .hi = 1.0f}, {}), 0.0f, 0.5f));
    // An offset anywhere in [-1, 1] under a wave at 0.25: [-0.375, 0.625].
    CHECK(is(mod::lfoBound({.lo = 0.25f, .hi = 0.25f}, {.lo = 1.0f, .hi = 1.0f}, {.lo = -1.0f, .hi = 1.0f}), -0.375f,
             0.625f));
    // The widest: the LFO's range as a source, +-1.125.
    CHECK(is(mod::lfoBound({.lo = -mod::lfoWaveLimit, .hi = mod::lfoWaveLimit}, {.lo = 0.0f, .hi = 1.0f},
                           {.lo = -1.0f, .hi = 1.0f}),
             -mod::lfoSourceLimit, mod::lfoSourceLimit));
}
