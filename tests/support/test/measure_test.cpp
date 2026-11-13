// Tests for the measurement helpers: THD, aliasing, and frequency response.
// Each is checked on a signal whose answer is known exactly.

#include <anoesis/test/measure.hpp>
#include <anoesis/test/signals.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

using namespace anoesis::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{

constexpr double rate = 48000.0;
constexpr std::size_t n = 8192;

// A sum of harmonics of f0 with the given amplitudes (index 0 = fundamental).
std::vector<float> harmonicSignal(double f0, const std::vector<double>& amplitudes)
{
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        double v = 0.0;
        for (std::size_t h = 0; h < amplitudes.size(); ++h)
        {
            v += amplitudes[h] *
                 std::sin(2.0 * std::numbers::pi * f0 * static_cast<double>(h + 1) * static_cast<double>(i) / rate);
        }
        x[i] = static_cast<float>(v);
    }
    return x;
}

// A naive (non-band-limited) sawtooth: every harmonic above Nyquist folds back.
std::vector<float> naiveSaw(double f0)
{
    std::vector<float> x(n);
    double phase = 0.0;
    for (auto& v : x)
    {
        v = static_cast<float>((2.0 * phase) - 1.0);
        phase += f0 / rate;
        phase -= std::floor(phase);
    }
    return x;
}

// The same sawtooth built additively from harmonics below Nyquist only.
std::vector<float> bandLimitedSaw(double f0)
{
    std::vector<double> amplitudes;
    for (std::size_t h = 1; static_cast<double>(h) * f0 < rate / 2.0; ++h)
    {
        amplitudes.push_back(
            -2.0 / (std::numbers::pi * static_cast<double>(h))); // 2 frac(t) - 1 = -(2/pi) sum sin(2 pi k t) / k
    }
    return harmonicSignal(f0, amplitudes);
}

} // namespace

TEST_CASE("THD of a pure float sine is at the float noise floor", "[support][measure]")
{
    const auto f0 = coherentFrequency(1000.0, rate, n);
    const auto thd = measureThd(sine(f0, rate, n, 0.5), f0, rate, rectangularWindow(n));
    CAPTURE(toDb(thd));
    CHECK(toDb(thd) < -130.0); // 24-bit float mantissa: ~-140 dB of quantisation spread over all bins
}

TEST_CASE("THD measures known harmonic content exactly", "[support][measure]")
{
    // 3rd harmonic at 1% and 5th at 0.1% of the fundamental:
    // THD = sqrt(0.01^2 + 0.001^2) = 0.0100499 (-39.957 dB).
    const auto expected = std::sqrt((0.01 * 0.01) + (0.001 * 0.001));

    SECTION("coherent sampling, rectangular window")
    {
        const auto f0 = coherentFrequency(440.0, rate, n);
        const auto x = harmonicSignal(f0, {0.5, 0.0, 0.005, 0.0, 0.0005});
        // Tolerance: float quantisation of x adds ~1e-7 relative noise per bin.
        CHECK_THAT(measureThd(x, f0, rate, rectangularWindow(n)), WithinRel(expected, 1e-5));
    }

    SECTION("arbitrary frequency, Blackman-Harris window")
    {
        const double f0 = 437.3;
        const auto x = harmonicSignal(f0, {0.5, 0.0, 0.005, 0.0, 0.0005});
        // Tolerance: lobe sums capture each tone's power to ~1e-3 (see the
        // spectrum tests), so the ratio is good to ~2e-3: 0.02 dB.
        CHECK_THAT(measureThd(x, f0, rate, blackmanHarrisWindow(n)), WithinRel(expected, 2e-3));
    }
}

TEST_CASE("Aliasing: a band-limited sawtooth is clean, a naive one is not", "[support][measure]")
{
    const auto f0 = coherentFrequency(2500.0, rate, n);
    const auto clean = measureAliasing(bandLimitedSaw(f0), f0, rate, rectangularWindow(n));
    const auto naive = measureAliasing(naiveSaw(f0), f0, rate, rectangularWindow(n));
    CAPTURE(clean.aliasToSignalDb, clean.worstAliasDb, naive.aliasToSignalDb, naive.worstAliasDb, naive.worstAliasHz);

    CHECK(clean.worstAliasDb < -120.0); // only float rounding left
    // A naive saw's strongest alias is roughly the first harmonic above
    // Nyquist folded back: about 1/10 of the fundamental here, around -20 dB.
    CHECK(naive.worstAliasDb > -30.0);
    CHECK(naive.aliasToSignalDb > -30.0);
}

TEST_CASE("Aliasing: a known non-harmonic tone is measured at its level and frequency", "[support][measure]")
{
    const auto f0 = coherentFrequency(1000.0, rate, n);
    auto x = harmonicSignal(f0, {0.5, 0.1, 0.05});
    const auto alias = sine(coherentFrequency(7777.0, rate, n), rate, n, 0.5e-4); // -80 dB re the fundamental
    for (std::size_t i = 0; i < n; ++i)
    {
        x[i] += alias[i];
    }
    const auto result = measureAliasing(x, f0, rate, rectangularWindow(n));
    CHECK_THAT(result.worstAliasDb, WithinAbs(-80.0, 0.01));
    CHECK_THAT(result.worstAliasHz, WithinAbs(coherentFrequency(7777.0, rate, n), 1e-6));

    SECTION("a maximum frequency excludes aliases above it (for example above 20 kHz)")
    {
        const auto limited = measureAliasing(x, f0, rate, rectangularWindow(n), 5000.0);
        CHECK(limited.worstAliasDb < -120.0);
    }
}

TEST_CASE("Frequency response by sine measurement matches the analytic response", "[support][measure]")
{
    // y[n] = (x[n] + x[n-1]) / 2 has H(f) = cos(pi f / fs) * e^(-i pi f / fs).
    const MonoProcess average = [](std::span<const float> in)
    {
        std::vector<float> out(in.size());
        float previous = 0.0f;
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            out[i] = 0.5f * (in[i] + previous);
            previous = in[i];
        }
        return out;
    };

    for (const double f : {100.0, 1000.0, 5000.0, 12000.0, 20000.0})
    {
        const auto measured = measureResponseAt(average, f, rate);
        const auto omega = std::numbers::pi * f / rate;
        CAPTURE(f);
        // Tolerance: the Kaiser(20) window's leakage is ~-190 dB; what is left
        // is float rounding of the processed signal, ~1e-7 relative.
        CHECK_THAT(measured.magnitude, WithinRel(std::cos(omega), 1e-6));
        CHECK_THAT(measured.phaseRadians, WithinAbs(-omega, 1e-6));
    }
}

TEST_CASE("Frequency response from the impulse response", "[support][measure]")
{
    const MonoProcess average = [](std::span<const float> in)
    {
        std::vector<float> out(in.size());
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            out[i] = 0.5f * (in[i] + (i > 0 ? in[i - 1] : 0.0f));
        }
        return out;
    };
    const auto response = impulseResponseSpectrum(average, 1024);
    REQUIRE(response.size() == 513);
    for (std::size_t k = 0; k <= 512; ++k)
    {
        const auto omega = std::numbers::pi * static_cast<double>(k) / 1024.0;
        CAPTURE(k);
        CHECK_THAT(std::abs(response[k]), WithinAbs(std::cos(omega), 1e-7));
    }
}

TEST_CASE("Decibel conversions", "[support][measure]")
{
    CHECK_THAT(toDb(1.0), WithinAbs(0.0, 1e-12));
    CHECK_THAT(toDb(0.1), WithinAbs(-20.0, 1e-12));
    CHECK_THAT(powerToDb(0.01), WithinAbs(-20.0, 1e-12));
    CHECK(std::isinf(toDb(0.0)));
}
