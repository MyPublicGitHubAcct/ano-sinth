// Tests for parameter smoothing: LinearSmoother and ExponentialSmoother.
// Doc: docs/dsp/parameter-smoothing.md

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/processor.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

using anoesis::dsp::ExponentialSmoother;
using anoesis::dsp::LinearSmoother;
using anoesis::dsp::ProcessSpec;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinULP;

namespace
{

constexpr ProcessSpec spec48k{.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 1};
constexpr ProcessSpec spec1k{.sampleRate = 1000.0f, .maxBlockSize = 64, .numChannels = 1};

template <typename S>
S makeSmoother(const ProcessSpec& spec, float rampSeconds, float initial)
{
    S smoother;
    smoother.setRampTime(rampSeconds);
    smoother.prepare(spec);
    smoother.setCurrentAndTarget(initial);
    return smoother;
}

template <typename S>
std::vector<float> take(S& smoother, std::size_t count)
{
    std::vector<float> values(count);
    for (auto& v : values)
    {
        v = smoother.next();
    }
    return values;
}

float largestJump(float previous, const std::vector<float>& values)
{
    float jump = 0.0f;
    for (const auto v : values)
    {
        jump = std::max(jump, std::abs(v - previous));
        previous = v;
    }
    return jump;
}

} // namespace

TEST_CASE("Smoothers follow the Preparable lifecycle", "[dsp][smoothing]")
{
    STATIC_CHECK(anoesis::dsp::Preparable<LinearSmoother>);
    STATIC_CHECK(anoesis::dsp::Preparable<ExponentialSmoother>);
}

TEST_CASE("Ramp length is set in seconds and is independent of sample rate", "[dsp][smoothing]")
{
    LinearSmoother smoother;
    smoother.setRampTime(0.01f);
    CHECK(smoother.rampLengthSamples() == 0); // not prepared yet: no rate, so changes are immediate

    smoother.prepare(spec48k);
    CHECK(smoother.rampLengthSamples() == 480);
    smoother.prepare({.sampleRate = 96000.0f, .maxBlockSize = 512, .numChannels = 1});
    CHECK(smoother.rampLengthSamples() == 960); // re-prepare keeps the time, recomputes samples

    smoother.setRampTime(0.0f);
    CHECK(smoother.rampLengthSamples() == 0);
    smoother.setRampTime(-1.0f); // negative times are treated as zero
    CHECK(smoother.rampLengthSamples() == 0);
}

TEST_CASE("LinearSmoother: equal steps that land exactly on the target", "[dsp][smoothing]")
{
    auto smoother = makeSmoother<LinearSmoother>(spec1k, 0.01f, 0.0f); // 10 samples
    smoother.setTarget(1.0f);
    REQUIRE(smoother.isSmoothing());

    const auto values = take(smoother, 10);
    // Tolerance: value = start + step * k, where step = 0.1 carries one
    // rounding (6e-9 here) and the multiply-add two more: < 1e-7.
    for (std::size_t k = 0; k < 9; ++k)
    {
        CAPTURE(k);
        CHECK_THAT(values[k], WithinAbs(0.1 * static_cast<double>(k + 1), 1e-7));
    }
    CHECK_THAT(values[9], WithinULP(1.0f, 0)); // the last step is exact
    CHECK_FALSE(smoother.isSmoothing());
    CHECK_THAT(smoother.next(), WithinULP(1.0f, 0)); // and it stays there
}

TEST_CASE("LinearSmoother: smoothing turns one zipper step into many small ones", "[dsp][smoothing]")
{
    // An unsmoothed parameter change is a discontinuity of the full size of
    // the change. A linear ramp of N samples bounds every step to change / N.
    auto smoother = makeSmoother<LinearSmoother>(spec48k, 0.01f, 0.25f);
    smoother.setTarget(0.75f);
    const auto values = take(smoother, 480);
    CHECK_THAT(largestJump(0.25f, values), WithinAbs(0.5 / 480.0, 1e-7));
}

TEST_CASE("LinearSmoother: no float drift over long ramps", "[dsp][smoothing]")
{
    // 10 s at 48 kHz: 480000 steps. An accumulating `value += step` would
    // collect up to 480000 roundings; start + step * k does not accumulate.
    auto smoother = makeSmoother<LinearSmoother>(spec48k, 10.0f, 0.0f);
    smoother.setTarget(1.0f);
    smoother.skip(240000 - 1);
    // Tolerance: step = 1/480000 rounded (relative 6e-8), times k = 240000,
    // is 3e-8 absolute; plus the multiply's rounding. 1e-7 covers it.
    CHECK_THAT(smoother.next(), WithinAbs(0.5, 1e-7));
}

TEST_CASE("Retargeting mid-ramp starts a new ramp from where it is", "[dsp][smoothing]")
{
    auto smoother = makeSmoother<LinearSmoother>(spec1k, 0.01f, 0.0f);
    smoother.setTarget(1.0f);
    const auto first = take(smoother, 5);
    const auto midpoint = first.back();
    CHECK_THAT(midpoint, WithinAbs(0.5, 1e-7));

    smoother.setTarget(-1.0f); // a full new ramp: 10 steps of -0.15
    const auto second = take(smoother, 10);
    CHECK_THAT(largestJump(midpoint, second), WithinAbs(0.15, 1e-6)); // continuous: no jump at the retarget
    CHECK_THAT(second.back(), WithinULP(-1.0f, 0));
}

TEST_CASE("Setting the same target again does not restart the ramp", "[dsp][smoothing]")
{
    auto smoother = makeSmoother<LinearSmoother>(spec1k, 0.01f, 0.0f);
    smoother.setTarget(1.0f);
    static_cast<void>(take(smoother, 5));
    smoother.setTarget(1.0f); // hosts often resend unchanged values every block
    static_cast<void>(take(smoother, 5));
    CHECK_FALSE(smoother.isSmoothing());
    CHECK_THAT(smoother.currentValue(), WithinULP(1.0f, 0));
}

TEST_CASE("Zero ramp time, setCurrentAndTarget, and reset() all jump immediately", "[dsp][smoothing]")
{
    SECTION("zero ramp time")
    {
        auto smoother = makeSmoother<LinearSmoother>(spec48k, 0.0f, 0.0f);
        smoother.setTarget(0.7f);
        CHECK_FALSE(smoother.isSmoothing());
        CHECK_THAT(smoother.next(), WithinULP(0.7f, 0));
    }
    SECTION("setCurrentAndTarget")
    {
        auto smoother = makeSmoother<ExponentialSmoother>(spec48k, 0.1f, 0.0f);
        smoother.setTarget(1.0f);
        smoother.setCurrentAndTarget(0.3f);
        CHECK_FALSE(smoother.isSmoothing());
        CHECK_THAT(smoother.next(), WithinULP(0.3f, 0));
    }
    SECTION("reset() finishes any ramp at its target")
    {
        auto smoother = makeSmoother<ExponentialSmoother>(spec48k, 0.1f, 0.0f);
        smoother.setTarget(2.0f);
        static_cast<void>(take(smoother, 10));
        smoother.reset();
        CHECK_FALSE(smoother.isSmoothing());
        CHECK_THAT(smoother.currentValue(), WithinULP(2.0f, 0));
    }
}

TEST_CASE("skip() and fill() match calling next() one sample at a time", "[dsp][smoothing]")
{
    auto stepped = makeSmoother<ExponentialSmoother>(spec48k, 0.01f, 1.0f);
    auto skipped = stepped;
    auto filled = stepped;
    stepped.setTarget(0.0f);
    skipped.setTarget(0.0f);
    filled.setTarget(0.0f);

    const auto reference = take(stepped, 300);
    skipped.skip(299);
    // Tolerance: skip() raises the decay to the 299th power in one pow()
    // call, while next() multiplies 299 times. Each multiply rounds (6e-8
    // relative), so they can differ by about 300 * 6e-8 = 2e-5 of the
    // remaining distance (< 0.3 here). 1e-5 absolute is ample.
    CHECK_THAT(skipped.next(), WithinAbs(reference.back(), 1e-5));

    std::array<float, 300> block{};
    filled.fill(block);
    for (std::size_t i = 0; i < block.size(); ++i)
    {
        CAPTURE(i);
        CHECK_THAT(block[i], WithinULP(reference[i], 0)); // fill() is next() in a loop: bit-identical
    }

    SECTION("skipping past the end lands exactly on the target")
    {
        skipped.skip(100000);
        CHECK_FALSE(skipped.isSmoothing());
        CHECK_THAT(skipped.currentValue(), WithinULP(0.0f, 0));
    }
}

TEST_CASE("ExponentialSmoother: one-pole approach that settles to -80 dB, then lands", "[dsp][smoothing]")
{
    constexpr std::size_t rampLength = 480; // 10 ms at 48 kHz
    auto smoother = makeSmoother<ExponentialSmoother>(spec48k, 0.01f, 0.0f);
    smoother.setTarget(1.0f);
    const auto values = take(smoother, rampLength);

    // Shape: the remaining distance decays as 10^(-4k/N), i.e. it is 1e-4
    // (-80 dB) of the step after N samples. Tolerance: after k multiplies by a
    // rounded decay, the remaining distance carries a relative error of about
    // 2k * 6e-8, and k * decay^k peaks near 19 for N = 480, so the absolute
    // error stays below 3e-6. 1e-5 leaves room for the final subtraction.
    for (std::size_t k = 1; k < rampLength; ++k)
    {
        const auto expected = 1.0 - std::pow(10.0, -4.0 * static_cast<double>(k) / static_cast<double>(rampLength));
        CAPTURE(k);
        CHECK_THAT(values[k - 1], WithinAbs(expected, 1e-5));
    }

    // Time constant: 63.2% of the step after N / ln(10^4) = 52.1 samples.
    CHECK_THAT(values[51], WithinAbs(1.0 - std::exp(-52.0 / 52.11688), 1e-4));
    // The final snap is a jump of only 1e-4 of the step: -80 dB, inaudible.
    CHECK_THAT(1.0f - values[rampLength - 2], WithinAbs(1e-4, 2e-6));
    CHECK_THAT(values.back(), WithinULP(1.0f, 0));
    CHECK_FALSE(smoother.isSmoothing());
}

TEST_CASE("Both smoothers move monotonically and never overshoot", "[dsp][smoothing]")
{
    const auto checkMonotonic = [](auto smoother, float from, float to)
    {
        smoother.setCurrentAndTarget(from);
        smoother.setTarget(to);
        float previous = from;
        bool monotonic = true;
        bool inRange = true;
        for (std::size_t i = 0; i < 2000; ++i)
        {
            const auto v = smoother.next();
            monotonic = monotonic && (to > from ? v >= previous : v <= previous);
            inRange = inRange && v >= std::min(from, to) && v <= std::max(from, to);
            previous = v;
        }
        CHECK(monotonic);
        CHECK(inRange);
    };

    checkMonotonic(makeSmoother<LinearSmoother>(spec48k, 0.03f, 0.0f), 20.0f, 20000.0f);
    checkMonotonic(makeSmoother<LinearSmoother>(spec48k, 0.03f, 0.0f), 1.0f, -1.0f);
    checkMonotonic(makeSmoother<ExponentialSmoother>(spec48k, 0.03f, 0.0f), 20.0f, 20000.0f);
    checkMonotonic(makeSmoother<ExponentialSmoother>(spec48k, 0.03f, 0.0f), 1.0f, -1.0f);
}
