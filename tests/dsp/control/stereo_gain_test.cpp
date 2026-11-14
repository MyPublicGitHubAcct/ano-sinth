// Tests for StereoGain: a mono signal added into a stereo block with a
// smoothed volume, pan, and polarity. Doc: docs/dsp/oscillator-controls.md

#include <anoesis/dsp/control/stereo_gain.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using anoesis::dsp::panGains;
using anoesis::dsp::PanLaw;
using anoesis::dsp::ProcessSpec;
using anoesis::dsp::StereoGain;
using anoesis::test::Buffer;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinULP;

namespace
{

constexpr float rate = 48000.0f;
constexpr ProcessSpec spec{.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2};

StereoGain prepared()
{
    StereoGain gain;
    gain.prepare(spec);
    return gain;
}

// The gains StereoGain applied, sample by sample: feed ones into silence.
struct GainTrace
{
    std::vector<float> left;
    std::vector<float> right;
};

GainTrace trace(StereoGain& gain, std::size_t numSamples)
{
    const std::vector<float> ones(numSamples, 1.0f);
    Buffer out{2, numSamples};
    gain.process(ones, out.block());
    return {
        .left = {out.channel(0).begin(), out.channel(0).end()},
        .right = {out.channel(1).begin(), out.channel(1).end()},
    };
}

std::vector<float> ramp(std::size_t numSamples)
{
    std::vector<float> x(numSamples);
    for (std::size_t i = 0; i < numSamples; ++i)
    {
        x[i] = std::sin(0.01f * static_cast<float>(i)) * 0.9f;
    }
    return x;
}

bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

} // namespace

TEST_CASE("StereoGain defaults to unity volume, centre pan (equal power), not inverted", "[dsp][control][gain]")
{
    auto gain = prepared();
    const auto g = gain.targetGains();
    const auto centre = panGains(0.0f, PanLaw::equalPower);
    CHECK_THAT(g.left, WithinULP(centre.left, 0));
    CHECK_THAT(g.right, WithinULP(centre.right, 0));
    CHECK_FALSE(gain.isSmoothing());
}

TEST_CASE("StereoGain adds volume x pan gain x input into each channel", "[dsp][control][gain]")
{
    auto gain = prepared();
    gain.setGain(0.5f);
    gain.setPan(-0.5f);
    gain.reset(); // no ramp: test the steady state

    const auto in = ramp(300);
    Buffer out{2, in.size()};
    std::ranges::fill(out.channel(0), 0.25f);
    std::ranges::fill(out.channel(1), -0.125f);
    gain.process(in, out.block());

    const auto pan = panGains(-0.5f, PanLaw::equalPower);
    const float gl = 0.5f * pan.left;
    const float gr = 0.5f * pan.right;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        CAPTURE(i);
        REQUIRE_THAT(out.channel(0)[i], WithinULP(0.25f + (gl * in[i]), 0));
        REQUIRE_THAT(out.channel(1)[i], WithinULP(-0.125f + (gr * in[i]), 0));
    }
}

TEST_CASE("StereoGain inverted is the exact negation", "[dsp][control][gain]")
{
    for (const float position : {-1.0f, -0.3f, 0.0f, 0.7f, 1.0f})
    {
        CAPTURE(position);
        auto plain = prepared();
        auto inverted = prepared();
        for (auto* g : {&plain, &inverted})
        {
            g->setGain(0.8f);
            g->setPan(position);
        }
        inverted.setInverted(true);
        plain.reset();
        inverted.reset();

        const auto in = ramp(256);
        Buffer a{2, in.size()};
        Buffer b{2, in.size()};
        plain.process(in, a.block());
        inverted.process(in, b.block());
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            for (std::size_t i = 0; i < in.size(); ++i)
            {
                CAPTURE(ch, i);
                REQUIRE_THAT(b.channel(ch)[i], WithinULP(-a.channel(ch)[i], 0)); // +0 and -0 compare equal
            }
        }
    }
}

TEST_CASE("StereoGain ramps a volume change linearly over 10 ms and lands exactly", "[dsp][control][gain]")
{
    auto gain = prepared();
    gain.setPan(-1.0f); // the left channel carries the whole volume
    gain.setGain(0.2f);
    gain.reset();
    gain.setGain(1.0f);
    CHECK(gain.isSmoothing());

    // 10 ms at 48 kHz = 480 samples; equal steps of 0.8 / 480.
    constexpr std::size_t rampLength = 480;
    const auto t = trace(gain, 1000);
    for (std::size_t i = 0; i < rampLength; ++i)
    {
        CAPTURE(i);
        // The smoother's linear ramp: start + step * k (k = 1 .. 480). 1e-6:
        // a few float roundings of values near 1.
        const auto expected = 0.2f + (0.8f * static_cast<float>(i + 1) / static_cast<float>(rampLength));
        REQUIRE_THAT(t.left[i], WithinAbs(expected, 1e-6f));
        REQUIRE_THAT(t.right[i], WithinULP(0.0f, 0));
    }
    for (std::size_t i = rampLength - 1; i < t.left.size(); ++i)
    {
        REQUIRE_THAT(t.left[i], WithinULP(1.0f, 0));
    }
    CHECK_FALSE(gain.isSmoothing());
}

TEST_CASE("StereoGain: a pan move ramps both channels together", "[dsp][control][gain]")
{
    auto gain = prepared();
    gain.setPan(-1.0f);
    gain.reset();
    gain.setPan(1.0f);
    const auto t = trace(gain, 600);

    // Both gains move in a straight line between the laws' end points, so
    // the power dips at the midpoint of a full left-to-right swing:
    // 0.5^2 + 0.5^2 = 1/2, -3 dB, for an instant.
    for (std::size_t i = 0; i < 480; ++i)
    {
        CAPTURE(i);
        const auto k = static_cast<float>(i + 1) / 480.0f;
        REQUIRE_THAT(t.left[i], WithinAbs(1.0f - k, 1e-6f));
        REQUIRE_THAT(t.right[i], WithinAbs(k, 1e-6f));
    }
    CHECK_THAT(t.left[599], WithinULP(0.0f, 0));
    CHECK_THAT(t.right[599], WithinULP(1.0f, 0));
}

TEST_CASE("StereoGain: toggling invert crossfades through zero instead of jumping", "[dsp][control][gain]")
{
    auto gain = prepared();
    gain.setPan(-1.0f);
    gain.reset();
    gain.setInverted(true);
    const auto t = trace(gain, 600);

    // From +1 to -1 in 480 equal steps: no step larger than 2 / 480
    // (plus rounding), where an instant flip would step by 2.
    float largestStep = std::abs(t.left[0] - 1.0f);
    for (std::size_t i = 1; i < t.left.size(); ++i)
    {
        largestStep = std::max(largestStep, std::abs(t.left[i] - t.left[i - 1]));
    }
    CHECK(largestStep < (2.0f / 480.0f) + 1e-6f);
    CHECK_THAT(t.left[599], WithinULP(-1.0f, 0));
}

TEST_CASE("StereoGain: reset() and a new prepare() finish any ramp", "[dsp][control][gain]")
{
    auto gain = prepared();
    gain.setGain(0.0f);
    static_cast<void>(trace(gain, 10)); // mid-ramp
    REQUIRE(gain.isSmoothing());
    gain.reset();
    CHECK_FALSE(gain.isSmoothing());
    CHECK_THAT(gain.currentGains().left, WithinULP(0.0f, 0));

    gain.setGain(1.0f);
    static_cast<void>(trace(gain, 10));
    REQUIRE(gain.isSmoothing());
    gain.prepare(spec);
    CHECK_FALSE(gain.isSmoothing());
    CHECK_THAT(gain.currentGains().left, WithinULP(gain.targetGains().left, 0));
}

TEST_CASE("StereoGain: ramp time is settable; zero means immediate", "[dsp][control][gain]")
{
    auto gain = prepared();
    gain.setRampTime(0.0f);
    gain.setGain(0.25f);
    CHECK_FALSE(gain.isSmoothing());

    gain.setRampTime(0.001f); // 48 samples
    gain.setGain(0.5f);
    static_cast<void>(trace(gain, 47));
    CHECK(gain.isSmoothing());
    static_cast<void>(trace(gain, 1));
    CHECK_FALSE(gain.isSmoothing());
}

TEST_CASE("StereoGain: the pan law is selectable", "[dsp][control][gain]")
{
    auto gain = prepared();
    gain.setPanLaw(PanLaw::linear);
    CHECK_THAT(gain.targetGains().left, WithinULP(0.5f, 0));
    gain.setPanLaw(PanLaw::balance);
    CHECK_THAT(gain.targetGains().left, WithinULP(1.0f, 0));
}

TEST_CASE("StereoGain: a NaN volume is silence; a mono block is left untouched", "[dsp][control][gain]")
{
    auto gain = prepared();
    gain.setGain(std::numeric_limits<float>::quiet_NaN());
    gain.reset();
    CHECK_THAT(gain.targetGains().left, WithinULP(0.0f, 0));

    gain.setGain(1.0f);
    gain.reset();
    const auto in = ramp(64);
    Buffer mono{1, in.size()};
    gain.process(in, mono.block());
    CHECK(std::ranges::all_of(mono.channel(0), [](float s) { return sameBits(s, 0.0f); }));
}

TEST_CASE("StereoGain: processes the shorter of the input and the block", "[dsp][control][gain]")
{
    auto gain = prepared();
    const std::vector<float> in(10, 1.0f);
    Buffer out{2, 20};
    gain.process(in, out.block());
    CHECK(out.channel(0)[9] > 0.0f);
    CHECK(sameBits(out.channel(0)[10], 0.0f));
}
