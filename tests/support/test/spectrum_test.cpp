// Tests for test signals, windows, and the power spectrum: the normalisation
// every spectral measurement relies on.

#include <anoesis/test/signals.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

using namespace anoesis::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("sine() generates the requested frequency, amplitude, and phase", "[support][signals]")
{
    const auto x = sine(1000.0, 48000.0, 48, 0.5, 0.0);
    REQUIRE(x.size() == 48);
    CHECK_THAT(x[0], WithinAbs(0.0, 1e-9));
    CHECK_THAT(x[12], WithinAbs(0.5, 1e-7)); // a quarter period later: the peak
    const auto c = sine(1000.0, 48000.0, 4, 1.0, std::numbers::pi / 2.0);
    CHECK_THAT(c[0], WithinAbs(1.0, 1e-7)); // phase pi/2: a cosine
}

TEST_CASE("coherentFrequency() gives an odd whole number of cycles per block", "[support][signals]")
{
    const auto f = coherentFrequency(1000.0, 48000.0, 8192);
    const auto cycles = f * 8192.0 / 48000.0;
    CHECK_THAT(cycles, WithinAbs(std::round(cycles), 1e-9));
    CHECK(static_cast<long>(std::round(cycles)) % 2 == 1);
    CHECK(std::abs(f - 1000.0) < 48000.0 / 8192.0); // within two bins of the request
}

TEST_CASE("impulse() and constant()", "[support][signals]")
{
    const auto i = impulse(8, 3, 0.5f);
    CHECK_THAT(i[3], WithinAbs(0.5, 0.0));
    CHECK_THAT(i[2], WithinAbs(0.0, 0.0));
    const auto c = constant(4, -0.25f);
    CHECK_THAT(c[3], WithinAbs(-0.25, 0.0));
}

TEST_CASE("Windows have their textbook coherent gains", "[support][window]")
{
    // Coherent gain = mean of the window: Hann 1/2, 4-term Blackman-Harris
    // a0 = 0.35875, rectangular 1. Periodic windows make these exact.
    const auto mean = [](const Window& w)
    {
        double sum = 0.0;
        for (const auto v : w.samples)
        {
            sum += v;
        }
        return sum / static_cast<double>(w.samples.size());
    };
    CHECK_THAT(mean(rectangularWindow(1024)), WithinAbs(1.0, 1e-12));
    CHECK_THAT(mean(hannWindow(1024)), WithinAbs(0.5, 1e-12));
    CHECK_THAT(mean(blackmanHarrisWindow(1024)), WithinAbs(0.35875, 1e-12));
    CHECK(kaiserWindow(1024, 20.0).mainLobeBins == 7); // ceil(sqrt(1 + (20/pi)^2)) = ceil(6.44)
}

TEST_CASE("Power spectrum: a sine of amplitude A has power A^2/2", "[support][spectrum]")
{
    constexpr std::size_t n = 4096;
    constexpr double rate = 48000.0;

    SECTION("bin-centred, rectangular window: all the power in one bin")
    {
        const auto f = coherentFrequency(3000.0, rate, n);
        const auto spectrum = powerSpectrum(sine(f, rate, n, 0.5), rate, rectangularWindow(n));
        CHECK_THAT(spectrum.power[spectrum.nearestBin(f)], WithinRel(0.125, 1e-6));
        CHECK_THAT(spectrum.powerAround(f, 1), WithinRel(0.125, 1e-6));
    }

    SECTION("between bins, Blackman-Harris: the main lobe holds the power")
    {
        const auto f = 3000.0 + (0.37 * rate / n); // deliberately off-bin
        const auto spectrum = powerSpectrum(sine(f, rate, n, 0.5), rate, blackmanHarrisWindow(n));
        // Tolerance: the main lobe (+-4 bins) of a 4-term Blackman-Harris holds
        // all but ~1e-9 of the energy, but the window's DFT is not perfectly
        // flat-summing for off-bin tones; 1e-3 relative (0.004 dB) is ample.
        CHECK_THAT(spectrum.powerAround(f, spectrum.mainLobeBins), WithinRel(0.125, 1e-3));
    }
}

TEST_CASE("Power spectrum: the bins sum to the signal's mean square (Parseval)", "[support][spectrum]")
{
    constexpr std::size_t n = 2048;
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto t = static_cast<double>(i);
        x[i] = static_cast<float>(std::sin(0.37 * t) + (0.5 * std::sin(1.91 * t * t / 1000.0)));
    }
    double meanSquare = 0.0;
    for (const auto v : x)
    {
        meanSquare += static_cast<double>(v) * static_cast<double>(v);
    }
    meanSquare /= static_cast<double>(n);

    const auto spectrum = powerSpectrum(x, 48000.0, rectangularWindow(n));
    double total = 0.0;
    for (const auto p : spectrum.power)
    {
        total += p;
    }
    CHECK_THAT(total, WithinRel(meanSquare, 1e-12));
}

TEST_CASE("Spectrum bin and frequency bookkeeping", "[support][spectrum]")
{
    const auto spectrum = powerSpectrum(std::vector<float>(1024), 48000.0, hannWindow(1024));
    CHECK(spectrum.power.size() == 513);
    CHECK_THAT(spectrum.binWidth(), WithinAbs(46.875, 1e-12));
    CHECK_THAT(spectrum.frequencyOf(512), WithinAbs(24000.0, 1e-9));
    CHECK(spectrum.nearestBin(1000.0) == 21);
    CHECK(spectrum.nearestBin(99999.0) == 512); // clamped to Nyquist
}
