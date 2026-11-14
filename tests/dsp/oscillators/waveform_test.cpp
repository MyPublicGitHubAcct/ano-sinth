// Tests for the waveform table: each of the 8 waveforms as data (a list of
// corners, each with a jump and a bend, plus an optional sine), checked
// against the ideal shapes written out independently in oscillator_support.hpp.
// Doc: docs/dsp/oscillator-waveforms.md

#include "oscillator_support.hpp"

#include <anoesis/dsp/oscillators/waveform.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

using namespace anoesis::test::d6;
using anoesis::dsp::WaveSegment;
using anoesis::dsp::WaveShape;
using Catch::Matchers::WithinAbs;

namespace
{

double cyclesOf(std::uint32_t phase)
{
    return std::ldexp(static_cast<double>(phase), -32);
}

std::uint32_t phaseOf(double cycles)
{
    return static_cast<std::uint32_t>(std::ldexp(cycles, 32));
}

// The ideal slope (per cycle) just after phase t, by a one-sided difference.
double idealSlopeAfter(Waveform shape, double t)
{
    constexpr double h = 1e-7;
    return (idealWave(shape, t + (2.0 * h)) - idealWave(shape, t + h)) / h;
}

} // namespace

TEST_CASE("Every periodic waveform's shape follows its ideal form", "[dsp][oscillator][waveform]")
{
    for (const auto shape : periodicShapes)
    {
        CAPTURE(shapeName(shape));
        const auto& wave = anoesis::dsp::waveShape(shape);
        for (std::uint32_t i = 0; i < 4096; ++i)
        {
            const std::uint32_t phase = (i << 20U) + 12345U; // off the corners, which sit on multiples of 1/8
            CAPTURE(cyclesOf(phase));
            // float: one cycle in 2^-23 steps is 1.2e-7; a few roundings.
            REQUIRE_THAT(wave.valueAt(phase), WithinAbs(idealWave(shape, cyclesOf(phase)), 1e-6));
        }
    }
}

TEST_CASE("Every periodic waveform's corners are its jumps and bends", "[dsp][oscillator][waveform]")
{
    for (const auto shape : periodicShapes)
    {
        CAPTURE(shapeName(shape));
        const auto& wave = anoesis::dsp::waveShape(shape);
        const auto expected = idealCorners(shape);
        REQUIRE(wave.corners().size() == expected.size());
        double rise = 0.0; // jumps plus the segments' climbs
        double bends = 0.0;
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            const auto& corner = wave.corners()[i];
            const auto t = expected[i];
            CAPTURE(t);
            CHECK(corner.phase == phaseOf(t));
            const double before = idealWave(shape, t > 0.0 ? t - 1e-9 : 1.0 - 1e-9);
            const double after = idealWave(shape, t);
            CHECK_THAT(corner.value, WithinAbs(after, 1e-6));
            CHECK_THAT(corner.jump, WithinAbs(after - before, 1e-6));
            const double slopeAfter = idealSlopeAfter(shape, t);
            const double slopeBefore = idealSlopeAfter(shape, t > 0.0 ? t - 1e-5 : 1.0 - 1e-5);
            CHECK_THAT(corner.slope, WithinAbs(slopeAfter, 1e-4));
            CHECK_THAT(corner.bend, WithinAbs(slopeAfter - slopeBefore, 1e-4));
            // The flags let the oscillators skip a zero residual.
            CHECK(corner.jumps == (std::abs(after - before) > 1e-6));
            CHECK(corner.bends == (std::abs(slopeAfter - slopeBefore) > 1e-4));
            const double end = i + 1 < expected.size() ? expected[i + 1] : 1.0;
            rise += corner.jump + (corner.slope * (end - t));
            bends += corner.bend;
        }
        // Over one cycle a periodic waveform returns to its starting value
        // and slope: the jumps undo the segments' net climb, and the bends
        // cancel.
        CHECK_THAT(rise, WithinAbs(0.0, 1e-6));
        CHECK_THAT(bends, WithinAbs(0.0, 1e-5));
        CHECK_THAT(wave.sineLevel(), WithinAbs(shape == Waveform::sine ? 1.0 : 0.0, 0.0));
    }
}

TEST_CASE("A waveform is built from straight segments", "[dsp][oscillator][waveform]")
{
    // A skewed triangle: up over the first 3/4 of the cycle, down over the
    // last 1/4. No jumps; the slope changes by +-(8/3 + 8) at the corners.
    constexpr auto skewed = WaveShape::piecewiseLinear(std::array{
        WaveSegment{.start = 0.0, .from = -1.0, .to = 1.0},
        WaveSegment{.start = 0.75, .from = 1.0, .to = -1.0},
    });
    REQUIRE(skewed.corners().size() == 2);
    const auto& rise = skewed.corners()[0];
    const auto& fall = skewed.corners()[1];
    CHECK(rise.phase == 0U);
    CHECK(fall.phase == 0xC0000000U);
    CHECK_THAT(rise.slope, WithinAbs(8.0 / 3.0, 1e-6));
    CHECK_THAT(fall.slope, WithinAbs(-8.0, 1e-6));
    CHECK_THAT(rise.jump, WithinAbs(0.0, 0.0));
    CHECK_THAT(fall.jump, WithinAbs(0.0, 0.0));
    CHECK_FALSE(rise.jumps);
    CHECK_THAT(rise.bend, WithinAbs((8.0 / 3.0) + 8.0, 1e-5));
    CHECK_THAT(fall.bend, WithinAbs(-8.0 - (8.0 / 3.0), 1e-5));
    CHECK(rise.bends);
    CHECK_THAT(skewed.valueAt(0x60000000U), WithinAbs(0.0, 1e-6)); // halfway up, at t = 3/8
    CHECK_THAT(skewed.valueAt(0xE0000000U), WithinAbs(0.0, 1e-6)); // halfway down, at t = 7/8

    SECTION("a segment's value is reached exactly at its own corner")
    {
        CHECK(&skewed.cornerAt(0U) == &rise);
        CHECK(&skewed.cornerAt(0xBFFFFFFFU) == &rise);
        CHECK(&skewed.cornerAt(0xC0000000U) == &fall);
        CHECK(&skewed.cornerAt(0xFFFFFFFFU) == &fall);
    }
}

TEST_CASE("Noise is not one of the periodic shapes", "[dsp][oscillator][waveform]")
{
    // Noise has no phase or edges: it comes from WhiteNoise, alongside the
    // oscillator. In the oscillator's table it is silence.
    CHECK_FALSE(anoesis::dsp::isPeriodic(Waveform::noise));
    for (const auto shape : periodicShapes)
    {
        CHECK(anoesis::dsp::isPeriodic(shape));
    }
    const auto& silence = anoesis::dsp::waveShape(Waveform::noise);
    CHECK(silence.corners().empty());
    CHECK_THAT(silence.sineLevel(), WithinAbs(0.0, 0.0));
    CHECK_THAT(silence.valueAt(0x40000000U), WithinAbs(0.0, 0.0));
}

TEST_CASE("An out-of-range waveform value falls back to the saw", "[dsp][oscillator][waveform]")
{
    // A corrupt parameter value must not index past the table.
    const auto bogus = static_cast<Waveform>(200);
    CHECK(&anoesis::dsp::waveShape(bogus) == &anoesis::dsp::waveShape(Waveform::saw));
}
