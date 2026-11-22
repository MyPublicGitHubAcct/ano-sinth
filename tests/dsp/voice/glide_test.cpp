// Tests for portamento's glide (libs/dsp/.../voice/glide.hpp) and the pitch
// bend (pitch_bend.hpp), M1.4 task 3 (docs/dsp/legato-portamento.md):
//
// - Glide: linear in semitones, constant time, a closed form of the samples
//   since it began, landing exactly on its target; read ahead without
//   advancing; a time of 0, NaN or negative, or no interval, is a jump.
// - PitchBend: 14-bit values scaled by 8192 both ways, times the Bend Range
//   (0-24 semitones), ramped linearly over 5 ms at absolute sample times; a
//   range change applies from the next value.

#include <anoesis/dsp/voice/glide.hpp>
#include <anoesis/dsp/voice/pitch_bend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace dsp = anoesis::dsp;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinULP;

TEST_CASE("Glide: linear in semitones over a constant time, landing exactly", "[glide]")
{
    dsp::Glide g;
    g.prepare(48000.0);

    SECTION("a jump stands at its note")
    {
        g.jump(48.0f);
        CHECK_THAT(g.at(0), WithinULP(48.0f, 0));
        CHECK_THAT(g.at(100000), WithinULP(48.0f, 0));
        CHECK_FALSE(g.isGliding());
        CHECK(g.lengthSamples() == 0);
        CHECK_THAT(g.target(), WithinULP(48.0f, 0));
    }

    SECTION("an octave in 100 ms: 4800 samples, straight in semitones")
    {
        g.start(48.0f, 60.0f, 0.1f);
        CHECK(g.lengthSamples() == 4800);
        CHECK(g.isGliding());
        CHECK_THAT(g.target(), WithinULP(60.0f, 0));
        CHECK_THAT(g.at(0), WithinULP(48.0f, 0));
        CHECK_THAT(g.at(1200), WithinULP(51.0f, 0));
        CHECK_THAT(g.at(2400), WithinULP(54.0f, 0));
        CHECK_THAT(g.at(4800), WithinULP(60.0f, 0));
        CHECK_THAT(g.at(1000000), WithinULP(60.0f, 0));
        CHECK(g.at(4799) < 60.0f);
        // Every sample on the line: one rounding of the product and one of
        // the sum, each at most half an ULP at < 64 (3.8e-6).
        for (std::size_t k = 0; k <= 4800; ++k)
        {
            const double expected = 48.0 + (12.0 * static_cast<double>(k) / 4800.0);
            REQUIRE_THAT(static_cast<double>(g.at(k)), WithinAbs(expected, 3.9e-6));
        }
    }

    SECTION("constant time: a semitone and two octaves take the same time")
    {
        g.start(48.0f, 49.0f, 0.1f);
        CHECK(g.lengthSamples() == 4800);
        g.start(48.0f, 72.0f, 0.1f);
        CHECK(g.lengthSamples() == 4800);
        g.start(60.0f, 48.0f, 0.1f); // downwards
        CHECK_THAT(g.at(2400), WithinULP(54.0f, 0));
    }

    SECTION("advancing moves now on, bit for bit what reading ahead gave")
    {
        dsp::Glide ahead;
        ahead.prepare(48000.0);
        ahead.start(48.0f, 60.0f, 0.1f);
        g.start(48.0f, 60.0f, 0.1f);
        g.advance(1000);
        for (const std::size_t k : {0U, 1U, 777U, 3800U, 5000U})
        {
            CHECK_THAT(g.at(k), WithinULP(ahead.at(1000 + k), 0));
        }
        g.advance(3799);
        CHECK(g.isGliding());
        g.advance(1);
        CHECK_FALSE(g.isGliding());
        CHECK_THAT(g.at(0), WithinULP(60.0f, 0));
        g.advance(std::numeric_limits<std::size_t>::max() / 2);
        CHECK_THAT(g.at(0), WithinULP(60.0f, 0));
    }

    SECTION("no time, a bad time, or no interval is a jump")
    {
        for (const float seconds : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), 1e-6f})
        {
            g.start(48.0f, 60.0f, seconds);
            CHECK_FALSE(g.isGliding());
            CHECK_THAT(g.at(0), WithinULP(60.0f, 0));
        }
        g.start(60.0f, 60.0f, 0.1f);
        CHECK_FALSE(g.isGliding());
        CHECK_THAT(g.at(0), WithinULP(60.0f, 0));
    }

    SECTION("the time is clamped to maxSeconds")
    {
        g.start(48.0f, 60.0f, std::numeric_limits<float>::infinity());
        CHECK(g.lengthSamples() == 60 * 48000);
        g.start(48.0f, 60.0f, 1000.0f);
        CHECK(g.lengthSamples() == 60 * 48000);
    }

    SECTION("in samples, for a velocity")
    {
        g.startSamples(0.5f, 1.0f, 100);
        CHECK_THAT(g.at(50), WithinULP(0.75f, 0));
        CHECK_THAT(g.at(100), WithinULP(1.0f, 0));
        g.startSamples(0.5f, 1.0f, 0);
        CHECK_FALSE(g.isGliding());
        CHECK_THAT(g.at(0), WithinULP(1.0f, 0));
    }

    SECTION("the time follows the sample rate")
    {
        g.prepare(96000.0);
        g.start(48.0f, 60.0f, 0.1f);
        CHECK(g.lengthSamples() == 9600);
    }
}

TEST_CASE("PitchBend: 14-bit values times the range, ramped over 5 ms at absolute times", "[pitch-bend]")
{
    SECTION("the 14-bit value as a fraction, by 8192 both ways")
    {
        CHECK_THAT(dsp::pitchBendFraction(0), WithinULP(-1.0f, 0));
        CHECK_THAT(dsp::pitchBendFraction(4096), WithinULP(-0.5f, 0));
        CHECK_THAT(dsp::pitchBendFraction(8192), WithinULP(0.0f, 0));
        CHECK_THAT(dsp::pitchBendFraction(16383), WithinULP(8191.0f / 8192.0f, 0));
        CHECK_THAT(dsp::pitchBendFraction(-5), WithinULP(-1.0f, 0));
        CHECK_THAT(dsp::pitchBendFraction(20000), WithinULP(8191.0f / 8192.0f, 0));
        constexpr float atCompileTime = dsp::pitchBendFraction(12288); // constexpr
        CHECK_THAT(atCompileTime, WithinULP(0.5f, 0));
    }

    dsp::PitchBend b;
    b.prepare(48000.0);

    SECTION("0 until a value; the default range is 2")
    {
        CHECK_THAT(b.semitonesAt(0), WithinULP(0.0f, 0));
        CHECK_THAT(b.semitonesAt(123456), WithinULP(0.0f, 0));
        CHECK(b.isSteadyFrom(0));
        CHECK_THAT(b.range(), WithinULP(2.0f, 0));
    }

    SECTION("a value ramps linearly from where the bend is, over 240 samples at 48 kHz")
    {
        b.setValue(1000, 16383);
        const float full = 2.0f * (8191.0f / 8192.0f);
        CHECK_THAT(b.semitonesAt(1000), WithinULP(0.0f, 0));
        CHECK_THAT(b.semitonesAt(1120), WithinULP(0.5f * full, 1));
        CHECK_THAT(b.semitonesAt(1240), WithinULP(full, 0));
        CHECK_THAT(b.semitonesAt(5000), WithinULP(full, 0));
        CHECK_FALSE(b.isSteadyFrom(1000));
        CHECK_FALSE(b.isSteadyFrom(1239));
        CHECK(b.isSteadyFrom(1240));

        // A new value mid-ramp starts from where the bend is: continuous.
        const float there = b.semitonesAt(1100);
        b.setValue(1100, 8192);
        CHECK_THAT(b.semitonesAt(1100), WithinULP(there, 0));
        CHECK_THAT(b.semitonesAt(1340), WithinULP(0.0f, 0));
        CHECK(b.isSteadyFrom(1340));
    }

    SECTION("the range applies from the next value, clamped to 0-24")
    {
        b.setValue(0, 16383);
        b.setRange(12.0f);
        CHECK_THAT(b.semitonesAt(1000), WithinULP(2.0f * (8191.0f / 8192.0f), 0));
        b.setValue(2000, 0);
        CHECK_THAT(b.semitonesAt(2240), WithinULP(-12.0f, 0));
        b.setRange(30.0f);
        CHECK_THAT(b.range(), WithinULP(24.0f, 0));
        b.setRange(-1.0f);
        CHECK_THAT(b.range(), WithinULP(0.0f, 0));
        b.setRange(7.0f);
        b.setRange(std::numeric_limits<float>::quiet_NaN());
        CHECK_THAT(b.range(), WithinULP(7.0f, 0));
    }

    SECTION("the ramp follows the sample rate; reset() returns to 0")
    {
        b.prepare(96000.0);
        b.setValue(0, 0);
        CHECK_THAT(b.semitonesAt(240), WithinULP(-1.0f, 0));
        CHECK_THAT(b.semitonesAt(480), WithinULP(-2.0f, 0));
        b.reset();
        CHECK_THAT(b.semitonesAt(480), WithinULP(0.0f, 0));
        CHECK(b.isSteadyFrom(0));
    }
}
