// Tests for decibel <-> linear gain conversion.

#include <anoesis/dsp/math/decibels.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>

using anoesis::dsp::decibelsToGain;
using anoesis::dsp::gainToDecibels;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

TEST_CASE("decibelsToGain: exact anchor points", "[dsp][math]")
{
    CHECK_THAT(decibelsToGain(0.0f), WithinULP(1.0f, 0));
    CHECK_THAT(decibelsToGain(20.0f), WithinRel(10.0f, 1e-6f));
    CHECK_THAT(decibelsToGain(-20.0f), WithinRel(0.1f, 1e-6f));
    // 20*log10(0.5) = -6.0206 dB: half amplitude.
    CHECK_THAT(decibelsToGain(-6.020600f), WithinRel(0.5f, 1e-6f));
}

TEST_CASE("decibelsToGain: at or below the floor is silence", "[dsp][math]")
{
    CHECK_THAT(decibelsToGain(-100.0f), WithinULP(0.0f, 0));
    CHECK_THAT(decibelsToGain(-1000.0f), WithinULP(0.0f, 0));
    CHECK_THAT(decibelsToGain(-std::numeric_limits<float>::infinity()), WithinULP(0.0f, 0));
    CHECK_THAT(decibelsToGain(-60.0f, -60.0f), WithinULP(0.0f, 0)); // custom floor
    CHECK(decibelsToGain(-99.0f) > 0.0f);
}

TEST_CASE("decibelsToGain: matches 10^(dB/20) across the useful range", "[dsp][math]")
{
    // Tolerance: the float argument dB/20 carries a relative rounding error of
    // 2^-24, i.e. an absolute error of up to |dB/20| * 6e-8 = 3e-7 at 100 dB.
    // The result's relative error is ln(10) times that (7e-7), plus about one
    // ulp (1.2e-7) from pow() itself. 1e-6 covers both.
    for (int i = -990; i <= 400; ++i)
    {
        const auto db = 0.1f * static_cast<float>(i);
        const auto expected = std::pow(10.0, static_cast<double>(db) / 20.0);
        CAPTURE(db);
        CHECK_THAT(decibelsToGain(db), WithinRel(static_cast<float>(expected), 1e-6f));
    }
}

TEST_CASE("gainToDecibels: inverse of decibelsToGain", "[dsp][math]")
{
    CHECK_THAT(gainToDecibels(1.0f), WithinAbs(0.0f, 1e-7));
    CHECK_THAT(gainToDecibels(10.0f), WithinAbs(20.0f, 1e-5));
    CHECK_THAT(gainToDecibels(0.5f), WithinAbs(-6.0206f, 1e-4));

    // Round trip. Tolerance: one float ulp of the dB value at 40 dB is 3.8e-6;
    // allow a few ulps for the two conversions.
    for (int i = -990; i <= 400; ++i)
    {
        const auto db = 0.1f * static_cast<float>(i);
        CAPTURE(db);
        CHECK_THAT(gainToDecibels(decibelsToGain(db)), WithinAbs(db, 2e-5));
    }
}

TEST_CASE("gainToDecibels: silence and negative gains map to the floor", "[dsp][math]")
{
    CHECK_THAT(gainToDecibels(0.0f), WithinULP(-100.0f, 0));
    CHECK_THAT(gainToDecibels(-0.5f), WithinULP(-100.0f, 0));
    CHECK_THAT(gainToDecibels(1e-9f), WithinULP(-100.0f, 0)); // -180 dB is below the floor
    CHECK_THAT(gainToDecibels(1e-4f, -60.0f), WithinULP(-60.0f, 0));
}
