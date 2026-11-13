// Tests for the white noise generator: determinism, lifecycle, statistics.
// Doc: docs/dsp/white-noise.md

#include <anoesis/dsp/generators/white_noise.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/golden.hpp>
#include <anoesis/test/lifecycle.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <anoesis/dsp/core/processor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

using anoesis::dsp::ProcessSpec;
using anoesis::dsp::WhiteNoise;
using anoesis::test::Buffer;

namespace
{

constexpr ProcessSpec stereo{.sampleRate = 48000.0f, .maxBlockSize = 256, .numChannels = 2};

std::vector<double> generate(std::size_t count, std::uint64_t seed)
{
    WhiteNoise noise{seed};
    std::vector<double> values(count);
    for (auto& v : values)
    {
        v = noise.next();
    }
    return values;
}

} // namespace

TEST_CASE("WhiteNoise is a BlockProcessor that honours the lifecycle contract", "[dsp][noise]")
{
    STATIC_CHECK(anoesis::dsp::BlockProcessor<WhiteNoise>);
    const auto result = anoesis::test::checkLifecycleContract([] { return WhiteNoise{5}; }, stereo, 1000);
    CHECK(result.resetMatchesFreshPrepare);
    CHECK(result.rePrepareMatchesFreshPrepare);
}

TEST_CASE("WhiteNoise output does not depend on how the host splits blocks", "[dsp][noise]")
{
    // Samples are drawn sample-major (all channels of sample n, then n + 1),
    // so chunking a render differently gives bit-identical audio.
    Buffer whole{2, 1000};
    Buffer chunked{2, 1000};
    WhiteNoise a{77};
    WhiteNoise b{77};
    a.prepare({.sampleRate = 48000.0f, .maxBlockSize = 1000, .numChannels = 2});
    b.prepare(stereo);
    a.process(whole.block());
    anoesis::dsp::processInChunks(b, chunked.block(), 37); // an awkward chunk size on purpose
    CHECK(anoesis::test::bitIdentical(whole, chunked));
}

TEST_CASE("WhiteNoise: zero mean, variance 1/3, uniform, uncorrelated", "[dsp][noise]")
{
    // N = 2^20 samples of uniform noise on (-1, 1). The seed is fixed, so
    // these checks are deterministic. The bounds are 5 standard deviations of
    // each estimator, wide enough for any correct generator and seed, tight
    // enough to catch a biased or broken one.
    constexpr std::size_t n = std::size_t{1} << 20U;
    const auto x = generate(n, 2026);
    const auto count = static_cast<double>(n);

    double sum = 0.0;
    double sumSquares = 0.0;
    double lagProduct = 0.0;
    std::array<std::size_t, 64> histogram{};
    for (std::size_t i = 0; i < n; ++i)
    {
        sum += x[i];
        sumSquares += x[i] * x[i];
        lagProduct += i > 0 ? x[i] * x[i - 1] : 0.0;
        histogram[static_cast<std::size_t>((x[i] + 1.0) * 32.0)]++;
    }
    const auto mean = sum / count;
    const auto variance = (sumSquares / count) - (mean * mean);
    const auto lag1 = (lagProduct / count) / variance;

    double chiSquare = 0.0;
    const auto expected = count / 64.0;
    for (const auto observed : histogram)
    {
        const auto d = static_cast<double>(observed) - expected;
        chiSquare += d * d / expected;
    }

    CAPTURE(mean, variance, lag1, chiSquare);
    // Mean: sigma = sqrt(1/3 / N) = 5.6e-4.
    CHECK(std::abs(mean) < 2.8e-3);
    // Variance: sigma of the estimator = sqrt((E[x^4] - E[x^2]^2) / N) = sqrt((1/5 - 1/9) / N) = 2.9e-4.
    CHECK(std::abs(variance - (1.0 / 3.0)) < 1.5e-3);
    // Lag-1 autocorrelation of white noise: sigma = 1 / sqrt(N) = 9.8e-4.
    CHECK(std::abs(lag1) < 4.9e-3);
    // Chi-square over 64 equal bins, 63 degrees of freedom: mean 63, sigma
    // sqrt(126) = 11.2. 63 + 5 sigma = 119.
    CHECK(chiSquare < 119.0);
}

TEST_CASE("WhiteNoise: stereo channels are independent", "[dsp][noise]")
{
    constexpr std::size_t n = std::size_t{1} << 18U;
    Buffer buffer{2, n};
    WhiteNoise noise{3};
    noise.prepare({.sampleRate = 48000.0f, .maxBlockSize = n, .numChannels = 2});
    noise.process(buffer.block());

    double cross = 0.0;
    double left = 0.0;
    double right = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto l = static_cast<double>(buffer.channel(0)[i]);
        const auto r = static_cast<double>(buffer.channel(1)[i]);
        cross += l * r;
        left += l * l;
        right += r * r;
    }
    const auto correlation = cross / std::sqrt(left * right);
    CAPTURE(correlation);
    CHECK(std::abs(correlation) < 5.0 / std::sqrt(static_cast<double>(n))); // 5 sigma
}

TEST_CASE("WhiteNoise: the power spectrum is flat", "[dsp][noise][spectral]")
{
    // Welch average of 1024 Hann-windowed segments of 1024 samples. Uniform
    // noise of variance 1/3 has an expected one-sided power of 2 * (1/3) / N
    // per interior bin. Grouping 34 bins into a band and averaging 1024
    // segments leaves an estimation error of about 1/sqrt(34 * 1024) = 0.5%
    // (0.023 dB) per band; adjacent Hann bins are correlated, which roughly
    // doubles that. 0.25 dB is about 5 of those standard deviations.
    constexpr std::size_t segment = 1024;
    std::vector<float> x(segment * 1024);
    WhiteNoise noise{11};
    noise.fill(x);
    const auto spectrum = anoesis::test::averagedPowerSpectrum(x, 48000.0, anoesis::test::hannWindow(segment));

    const auto expectedPerBin = 2.0 * (1.0 / 3.0) / static_cast<double>(segment);
    double worstDeviationDb = 0.0;
    for (std::size_t band = 0; band < 15; ++band)
    {
        double sum = 0.0;
        for (std::size_t k = 1 + (band * 34); k < 1 + ((band + 1) * 34); ++k) // bins 1 .. 510: no DC, no Nyquist
        {
            sum += spectrum.power[k];
        }
        const auto deviation = anoesis::test::powerToDb((sum / 34.0) / expectedPerBin);
        worstDeviationDb = std::max(worstDeviationDb, std::abs(deviation));
    }
    CAPTURE(worstDeviationDb);
    CHECK(worstDeviationDb < 0.25);
}

TEST_CASE("WhiteNoise matches its golden render bit for bit", "[dsp][noise][golden]")
{
    // Integer arithmetic plus an exact float conversion: every platform must
    // produce exactly these samples, so the tolerance is zero.
    Buffer buffer{2, 2048};
    WhiteNoise noise{1};
    noise.prepare({.sampleRate = 48000.0f, .maxBlockSize = 2048, .numChannels = 2});
    noise.process(buffer.block());

    const auto result = anoesis::test::compareWithGolden("dsp/white_noise_seed1_stereo", buffer, 48000.0);
    INFO(result.message);
    CHECK(result.passed);
}
