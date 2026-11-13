// Tests for the radix-2 FFT (decision D5): checked against a naive DFT and
// against transforms known in closed form.

#include <anoesis/test/fft.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <vector>

using anoesis::test::Complex;
using Catch::Matchers::WithinAbs;

namespace
{

std::vector<Complex> naiveDft(const std::vector<Complex>& x)
{
    const auto n = x.size();
    std::vector<Complex> result(n);
    for (std::size_t k = 0; k < n; ++k)
    {
        Complex sum{};
        for (std::size_t t = 0; t < n; ++t)
        {
            const auto angle = -2.0 * std::numbers::pi * static_cast<double>(k * t % n) / static_cast<double>(n);
            sum += x[t] * std::polar(1.0, angle);
        }
        result[k] = sum;
    }
    return result;
}

// A fixed, irregular test signal (no RNG needed).
std::vector<Complex> testSignal(std::size_t n)
{
    std::vector<Complex> x(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto t = static_cast<double>(i);
        x[i] = {std::sin(0.7 * t) + (0.3 * std::cos(2.9 * t * t)), std::cos(1.3 * t) - (0.2 * std::sin(0.11 * t * t))};
    }
    return x;
}

double maxDifference(const std::vector<Complex>& a, const std::vector<Complex>& b)
{
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        worst = std::max(worst, std::abs(a[i] - b[i]));
    }
    return worst;
}

} // namespace

TEST_CASE("FFT matches a naive DFT", "[support][fft]")
{
    // Tolerance: radix-2 round-off grows as O(eps * log2 N) relative to the
    // signal's RMS times sqrt(N); the naive DFT's own error is O(eps * N).
    // With |x| ~ 1 and N <= 1024, both are far below 1e-9.
    for (const std::size_t n : {1U, 2U, 4U, 8U, 64U, 1024U})
    {
        auto x = testSignal(n);
        const auto expected = naiveDft(x);
        anoesis::test::fftInPlace(x);
        CAPTURE(n);
        CHECK(maxDifference(x, expected) < 1e-9);
    }
}

TEST_CASE("Inverse FFT undoes the forward FFT", "[support][fft]")
{
    const auto original = testSignal(4096);
    auto x = original;
    anoesis::test::fftInPlace(x);
    anoesis::test::fftInPlace(x, true);
    CHECK(maxDifference(x, original) < 1e-12); // round trip: a few eps * log2 N
}

TEST_CASE("FFT of known signals", "[support][fft]")
{
    constexpr std::size_t n = 256;

    SECTION("an impulse has a flat spectrum")
    {
        std::vector<Complex> x(n);
        x[0] = 1.0;
        anoesis::test::fftInPlace(x);
        for (const auto& bin : x)
        {
            CHECK_THAT(bin.real(), WithinAbs(1.0, 1e-15));
            CHECK_THAT(bin.imag(), WithinAbs(0.0, 1e-15));
        }
    }

    SECTION("a cosine at bin 10 puts N/2 in bins 10 and N-10, nothing elsewhere")
    {
        std::vector<Complex> x(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            x[i] = std::cos(2.0 * std::numbers::pi * 10.0 * static_cast<double>(i) / static_cast<double>(n));
        }
        anoesis::test::fftInPlace(x);
        for (std::size_t k = 0; k < n; ++k)
        {
            const auto expected = (k == 10 || k == n - 10) ? static_cast<double>(n) / 2.0 : 0.0;
            CAPTURE(k);
            CHECK_THAT(std::abs(x[k]), WithinAbs(expected, 1e-11));
        }
    }
}

TEST_CASE("Parseval: energy is the same in both domains", "[support][fft]")
{
    auto x = testSignal(2048);
    double timeEnergy = 0.0;
    for (const auto& v : x)
    {
        timeEnergy += std::norm(v);
    }
    anoesis::test::fftInPlace(x);
    double frequencyEnergy = 0.0;
    for (const auto& v : x)
    {
        frequencyEnergy += std::norm(v);
    }
    CHECK_THAT(frequencyEnergy / 2048.0, WithinAbs(timeEnergy, 1e-9 * timeEnergy));
}

TEST_CASE("realFft returns the N/2 + 1 non-negative-frequency bins", "[support][fft]")
{
    std::vector<double> signal(64);
    signal[1] = 1.0;
    const auto bins = anoesis::test::realFft(signal);
    REQUIRE(bins.size() == 33);
    // A unit impulse at t = 1 has X[k] = e^(-2 pi i k / N): magnitude 1 everywhere.
    for (const auto& bin : bins)
    {
        CHECK_THAT(std::abs(bin), WithinAbs(1.0, 1e-14));
    }
}

TEST_CASE("FFT rejects sizes that are not powers of two", "[support][fft]")
{
    std::vector<Complex> x(12);
    CHECK_THROWS_AS(anoesis::test::fftInPlace(x), std::invalid_argument);
    CHECK_FALSE(anoesis::test::isPowerOfTwo(0));
    CHECK(anoesis::test::isPowerOfTwo(1));
    CHECK(anoesis::test::isPowerOfTwo(65536));
    CHECK_FALSE(anoesis::test::isPowerOfTwo(65535));
}
