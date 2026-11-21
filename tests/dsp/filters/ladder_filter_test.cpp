// Tests for the ladder filter (LadderFilter, docs/dsp/filters.md): the tap
// table; the small-signal response against the prewarped prototype in every
// response, with Warm Drive on and off; the self-oscillation threshold and
// pitch; the passband against resonance; drive; Warm Drive's harmonics;
// bounds under stress; bypass; response crossfades; cutoff ramps on the
// grid; the resonance modulation input; block sizes; stereo; the lifecycle;
// allocation; subnormals; NaN.
//
// Tolerances are derived in each test from the model (the bilinear map of
// the analog ladder is exact, so the small-signal response is the warped
// prototype to float rounding), from the measured report figures they pin
// (docs/dsp/filters.md), or from the decay of the slowest pole.

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/core/processor.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/fft.hpp>
#include <anoesis/test/lifecycle.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/signals.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

using anoesis::dsp::FilterResponse;
using anoesis::dsp::LadderFilter;
using anoesis::test::Buffer;
using Catch::Matchers::WithinAbs;

namespace
{

namespace dsp = anoesis::dsp;
namespace test = anoesis::test;

constexpr double pi = std::numbers::pi;

constexpr std::array<FilterResponse, 6> allResponses{
    FilterResponse::lp24, FilterResponse::lp12, FilterResponse::bp24,
    FilterResponse::bp12, FilterResponse::hp24, FilterResponse::hp12,
};

// The specification of the six responses, written independently of the
// library's table: weights of u, y1, y2, y3, y4 (docs/dsp/filter-theory.md).
using Mix = std::array<float, 5>;
constexpr std::array<Mix, 6> expectedMixes{
    Mix{0.0f, 0.0f, 0.0f, 0.0f, 1.0f},  Mix{0.0f, 0.0f, 1.0f, 0.0f, 0.0f},   Mix{0.0f, 0.0f, 4.0f, -8.0f, 4.0f},
    Mix{0.0f, 2.0f, -2.0f, 0.0f, 0.0f}, Mix{1.0f, -4.0f, 6.0f, -4.0f, 1.0f}, Mix{1.0f, -2.0f, 1.0f, 0.0f, 0.0f},
};

struct Settings
{
    double cutoff = 1000.0;
    float k = 0.0f;
    float drive = 1.0f;
    FilterResponse response = FilterResponse::lp24;
    bool warm = true;
};

dsp::ProcessSpec specAt(double rate, std::size_t channels = 1)
{
    return {.sampleRate = static_cast<float>(rate), .maxBlockSize = 4096, .numChannels = channels};
}

/// A filter prepared at `rate` with `s` (set before prepare(), so no ramps).
LadderFilter makeFilter(double rate, const Settings& s, std::size_t channels = 1)
{
    LadderFilter f;
    f.setCutoff(static_cast<float>(s.cutoff));
    f.setResonance(s.k);
    f.setDrive(s.drive);
    f.setResponse(s.response);
    f.setWarm(s.warm);
    f.prepare(specAt(rate, channels));
    return f;
}

std::vector<float> render(LadderFilter& f, std::span<const float> x, std::size_t blockSize = 4096)
{
    Buffer buffer{1, x.size()};
    std::ranges::copy(x, buffer.channel(0).begin());
    dsp::processInChunks(f, buffer.block(), blockSize);
    return {buffer.channel(0).begin(), buffer.channel(0).end()};
}

/// The prewarped analog prototype at digital frequency omega (radians per
/// sample): the bilinear map s = j tan(omega / 2) / g of
/// sum c_i (1 + s)^(4 - i) / ((1 + s)^4 + k), g = tan(pi fc / fs).
std::complex<double> prototype(FilterResponse r, double k, double cutoff, double rate, double omega)
{
    const double g = std::tan(pi * cutoff / rate);
    const std::complex<double> s{0.0, std::tan(omega / 2.0) / g};
    const auto p = 1.0 + s;
    const std::array<std::complex<double>, 5> powers{p * p * p * p, p * p * p, p * p, p, 1.0};
    const auto& c = expectedMixes[static_cast<std::size_t>(r)];
    std::complex<double> numerator{};
    for (std::size_t i = 0; i < 5; ++i)
    {
        numerator += static_cast<double>(c[i]) * powers[i];
    }
    return numerator / (powers[0] + k);
}

/// The small-signal spectrum: the response to an impulse of 1e-4, scaled up.
std::vector<test::Complex> smallSignalSpectrum(double rate, const Settings& s, std::size_t fftSize)
{
    constexpr float level = 1e-4f;
    const test::MonoProcess process = [&](std::span<const float> in)
    {
        auto f = makeFilter(rate, s);
        std::vector<float> x(in.begin(), in.end());
        for (auto& v : x)
        {
            v *= level;
        }
        auto y = render(f, x);
        for (auto& v : y)
        {
            v /= level;
        }
        return y;
    };
    return test::impulseResponseSpectrum(process, fftSize);
}

/// The RMS of y over cycles [from, to) of a period in samples.
double cycleRms(std::span<const float> y, double period, double from, double to)
{
    const auto a = static_cast<std::size_t>(from * period);
    const auto b = static_cast<std::size_t>(to * period);
    double sum = 0.0;
    for (std::size_t i = a; i < b; ++i)
    {
        sum += static_cast<double>(y[i]) * static_cast<double>(y[i]);
    }
    return std::sqrt(sum / static_cast<double>(b - a));
}

/// Self-oscillation started by one impulse of 0.5, no input after; returns
/// the pitch error in cents and the peak-equivalent level in dBFS of the
/// last 0.5 s of `seconds`.
struct Oscillation
{
    double cents = 0.0;
    double levelDb = 0.0;
};

Oscillation selfOscillate(double rate, double cutoff, float k, bool warm = true, double seconds = 2.5)
{
    auto f = makeFilter(rate, {.cutoff = cutoff, .k = k, .warm = warm});
    std::vector<float> x(static_cast<std::size_t>(seconds * rate), 0.0f);
    x[0] = 0.5f;
    const auto y = render(f, x);
    const auto tail = std::span{y}.subspan(y.size() - static_cast<std::size_t>(0.5 * rate));
    double sum = 0.0;
    for (const float v : tail)
    {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    const double rms = std::sqrt(sum / static_cast<double>(tail.size()));
    return {
        .cents = 1200.0 * std::log2(test::measureFrequency(tail, rate) / cutoff),
        .levelDb = 20.0 * std::log10(rms * std::numbers::sqrt2),
    };
}

bool bitIdentical(std::span<const float> a, std::span<const float> b)
{
    return std::ranges::equal(a, b, [](float p, float q)
                              { return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q); });
}

std::vector<float> saw(double hz, double rate, std::size_t n, float amplitude = 1.0f)
{
    std::vector<float> x(n);
    double phase = 0.0;
    for (auto& v : x)
    {
        v = amplitude * static_cast<float>((2.0 * phase) - 1.0);
        phase += hz / rate;
        phase -= std::floor(phase);
    }
    return x;
}

/// Energy above 1 kHz in a Hann window of 4096 centred on `at`.
double energyAbove1k(std::span<const float> x, std::size_t at, double rate)
{
    const auto window = test::hannWindow(4096);
    const auto spectrum = test::powerSpectrum(x.subspan(at - 2048, 4096), rate, window);
    double sum = 0.0;
    for (std::size_t k = 0; k < spectrum.power.size(); ++k)
    {
        sum += spectrum.frequencyOf(k) >= 1000.0 ? spectrum.power[k] : 0.0;
    }
    return sum;
}

/// A linear trapezoidal ladder in double (the same equations, no
/// saturators), fed g and k per sample: what LadderFilter is at -80 dBFS.
std::vector<double> linearLadder(std::span<const float> x, const std::function<double(std::size_t)>& gAt,
                                 const std::function<double(std::size_t)>& kAt)
{
    std::array<double, 4> s{};
    std::vector<double> y(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double g = gAt(i);
        const double k = kAt(i);
        const double big = g / (1.0 + g);
        double sum = 0.0;
        double power = 1.0;
        for (std::size_t j = 4; j-- > 0;)
        {
            sum += power * s[j] / (1.0 + g);
            power *= big;
        }
        const double u = (static_cast<double>(x[i]) - (k * sum)) / (1.0 + (k * power));
        double in = u;
        for (std::size_t j = 0; j < 4; ++j)
        {
            const double out = (big * in) + (s[j] / (1.0 + g));
            s[j] = (2.0 * out) - s[j];
            in = out;
        }
        y[i] = in;
    }
    return y;
}

} // namespace

TEST_CASE("Ladder filter: a constant resonance modulation is setResonance() bit for bit, capped at 64 with it",
          "[dsp][filter]")
{
    // The modulation adds to setResonance()'s k. With setResonance(0) and a
    // constant modulation, k is the modulation exactly; the sum is capped at
    // the DSP's ceiling, 64; a negative or NaN modulation counts as 0, +inf
    // as 64. And reset() starts the grid at the modulation already set, as a
    // note-on must, with no ramp from 0.
    constexpr double rate = 48000.0;
    const auto x = saw(220.0, rate, 6000, 0.8f);
    const auto withK = [&](float knob, float modulation)
    {
        LadderFilter f;
        f.setCutoff(900.0f);
        f.setResonance(knob);
        f.setResonanceModulation(modulation);
        f.prepare(specAt(rate));
        return render(f, x);
    };
    for (const float k : {0.0f, 0.37f, 3.0f, 4.4f, 12.0f, 64.0f})
    {
        CAPTURE(k);
        const auto direct = withK(k, 0.0f);
        CHECK(bitIdentical(withK(0.0f, k), direct));
    }
    CHECK(bitIdentical(withK(40.0f, 40.0f), withK(64.0f, 0.0f)));
    CHECK(bitIdentical(withK(3.0f, -5.0f), withK(3.0f, 0.0f)));
    CHECK(bitIdentical(withK(3.0f, std::numeric_limits<float>::quiet_NaN()), withK(3.0f, 0.0f)));
    CHECK(bitIdentical(withK(0.0f, std::numeric_limits<float>::infinity()), withK(64.0f, 0.0f)));
    // It adds to setResonance()'s k (1 + 2.5 is 3.5 exactly).
    CHECK(bitIdentical(withK(1.0f, 2.5f), withK(3.5f, 0.0f)));
}

TEST_CASE("Ladder filter: the resonance modulation is read at the next grid point and runs linearly to it",
          "[dsp][filter]")
{
    // As the cutoff modulation does (the g interpolation test above): a
    // modulation set mid-segment is read at the next grid point G, and k then
    // runs linearly from its old value at G to the new one at G + 32. At
    // -80 dBFS the ladder is linear, so the double-precision linear ladder fed
    // that k per sample must agree to the bound of that test, 1e-4 of the
    // output's peak. Applying it at once, or stepping it at G, differs by
    // more than ten times that.
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 2048;
    constexpr std::size_t setAt = 1000;
    constexpr std::size_t gridPoint = 1024;
    constexpr double k0 = 0.5;
    constexpr double k1 = 3.5;
    const auto x = saw(310.0, rate, n, 1e-4f);
    LadderFilter f;
    f.setCutoff(700.0f);
    f.setResonanceModulation(static_cast<float>(k0));
    f.prepare(specAt(rate));
    Buffer buffer{1, n};
    std::ranges::copy(x, buffer.channel(0).begin());
    f.process(buffer.block().subBlock(0, setAt));
    f.setResonanceModulation(static_cast<float>(k1));
    f.process(buffer.block().subBlock(setAt, n - setAt));
    const auto y = buffer.channel(0);

    const double g = std::tan(pi * 700.0 / rate);
    const auto gAt = [&](std::size_t)
    {
        return g;
    };
    const auto distance = [&](const std::function<double(std::size_t)>& kAt)
    {
        const auto reference = linearLadder(x, gAt, kAt);
        double peak = 0.0;
        double worst = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            peak = std::max(peak, std::fabs(reference[i]));
            worst = std::max(worst, std::fabs(static_cast<double>(y[i]) - reference[i]));
        }
        return worst / peak;
    };
    const double interpolated = distance(
        [&](std::size_t i)
        {
            if (i < gridPoint)
            {
                return k0;
            }
            return i >= gridPoint + 32 ? k1 : k0 + ((k1 - k0) * static_cast<double>(i - gridPoint) / 32.0);
        });
    const double atOnce = distance([&](std::size_t i) { return i < setAt ? k0 : k1; });
    const double stepped = distance([&](std::size_t i) { return i < gridPoint ? k0 : k1; });
    CHECK(interpolated < 1e-4);
    CHECK(atOnce > 1e-3);
    CHECK(stepped > 1e-3);
}

TEST_CASE("Ladder filter: the tap table is the six responses' stage mixes", "[dsp][filter]")
{
    for (std::size_t r = 0; r < allResponses.size(); ++r)
    {
        for (std::size_t i = 0; i < 5; ++i)
        {
            CHECK_THAT(dsp::filterTapMixes[r][i], Catch::Matchers::WithinULP(expectedMixes[r][i], 0));
        }
    }
}

TEST_CASE("Ladder filter: the small-signal response is the prewarped prototype, every response, Warm on and off",
          "[dsp][filter]")
{
    // At -80 dBFS each saturator is linear to x^2/3 ~ 3e-9, and the
    // trapezoidal ladder is the bilinear map of the analog one, so the
    // response is the warped prototype to float rounding. Rounding: about
    // 2^-24 per operation, a few dozen per sample, accumulated over the
    // impulse response's decay (at most 2000 samples above 1e-6 here) as a
    // random walk: about 1e-5 of the peak. Checked as an absolute complex
    // error, 4e-5 against a peak of at most 1/|4 - k| = 1 (k 3).
    constexpr double rate = 44100.0;
    constexpr std::size_t fftSize = 16384;
    for (const auto response : allResponses)
    {
        for (const double cutoff : {100.0, 1000.0, 10000.0, 19000.0})
        {
            for (const float k : {0.0f, 3.0f})
            {
                for (const bool warm : {true, false})
                {
                    CAPTURE(static_cast<int>(response), cutoff, k, warm);
                    const auto spectrum = smallSignalSpectrum(
                        rate, {.cutoff = cutoff, .k = k, .response = response, .warm = warm}, fftSize);
                    double worst = 0.0;
                    for (std::size_t bin = 0; bin < spectrum.size(); ++bin)
                    {
                        const double omega = 2.0 * pi * static_cast<double>(bin) / static_cast<double>(fftSize);
                        if (omega >= pi * 0.999)
                        {
                            continue;
                        }
                        worst = std::max(worst, std::abs(spectrum[bin] - prototype(response, k, cutoff, rate, omega)));
                    }
                    CHECK(worst < 4e-5);
                }
            }
        }
    }
}

TEST_CASE("Ladder filter: at the cutoff, LP24 is 1/|4 - k|, at every rate", "[dsp][filter]")
{
    // At s = j, (1 + j)^4 = -4, so LP24 = 1/(k - 4): -12.04 dB at k 0. With
    // the cutoff prewarped the digital filter has it at fc exactly. The
    // cutoff is placed on an FFT bin. The slowest case, k 3.5 at the lowest
    // cutoff (134 Hz at 44.1 kHz), decays with a time constant of
    // 1 / ((1 - 3.5^(1/4)/sqrt 2) wc) = 43 ms, so 65536 samples are 34 time
    // constants and truncation is negligible; what is left is float rounding
    // (about 1e-5 of the peak of 2, as in the test above).
    constexpr std::size_t fftSize = 65536;
    for (const double rate : {44100.0, 96000.0, 192000.0})
    {
        for (const double bin : {200.0, 4000.0, 12000.0})
        {
            const double cutoff = bin * rate / static_cast<double>(fftSize);
            for (const float k : {0.0f, 2.0f, 3.5f})
            {
                CAPTURE(rate, cutoff, k);
                const auto spectrum = smallSignalSpectrum(rate, {.cutoff = cutoff, .k = k}, fftSize);
                const double expected = 1.0 / std::fabs(4.0 - static_cast<double>(k));
                CHECK_THAT(std::abs(spectrum[static_cast<std::size_t>(bin)]), WithinAbs(expected, 1e-4));
            }
        }
    }
}

TEST_CASE("Ladder filter: self-oscillation starts at k = 4 at every cutoff and rate", "[dsp][filter]")
{
    // The bilinear map keeps the analog loop's threshold exactly: k = 4 at
    // the cutoff (docs/dsp/filter-theory.md). From a 1e-15 impulse (linear
    // throughout), the RMS over cycles 35-40 against 10-15: k = 3.99 decays
    // and 4.01 grows (the analog growth rate at k 4.01 is 4.4e-4 wc, so
    // about e^0.07 over 25 cycles: well above float noise). Cutoffs up to
    // 0.16 fs, where 40 cycles are 250 samples or more.
    for (const double rate : {44100.0, 96000.0, 192000.0})
    {
        for (const double cutoff : {110.0, 1760.0, 7040.0})
        {
            for (const bool warm : {true, false})
            {
                CAPTURE(rate, cutoff, warm);
                const double period = rate / cutoff;
                const auto grows = [&](float k)
                {
                    auto f = makeFilter(rate, {.cutoff = cutoff, .k = k, .warm = warm});
                    std::vector<float> x(static_cast<std::size_t>(40.0 * period) + 1, 0.0f);
                    x[0] = 1e-15f;
                    const auto y = render(f, x);
                    return cycleRms(y, period, 35.0, 40.0) > cycleRms(y, period, 10.0, 15.0);
                };
                CHECK_FALSE(grows(3.99f));
                CHECK(grows(4.01f));
            }
        }
    }
}

TEST_CASE("Ladder filter: self-oscillation is flat across the cutoff and falls with resonance", "[dsp][filter]")
{
    // A saturator in every stage lowers the stages' cutoff as the oscillation
    // grows (the describing function), by an amount set by k, not by fc. The
    // report measured -35.8 cents at k 4.4 (tanh) and -32.3 (cubic), the same
    // within 1.2 cents from 55 Hz to 3.5 kHz at 48 kHz (docs/dsp/filters.md):
    // pinned here to 0.2 cents at 440 Hz, and the spread to 1.5 cents.
    constexpr double rate = 48000.0;
    for (const auto& [warm, expected] : {std::pair{true, -35.8}, std::pair{false, -32.3}})
    {
        CAPTURE(warm);
        double lowest = 1e9;
        double highest = -1e9;
        for (const double cutoff : {55.0, 220.0, 880.0, 3520.0})
        {
            const auto o = selfOscillate(rate, cutoff, 4.4f, warm);
            lowest = std::min(lowest, o.cents);
            highest = std::max(highest, o.cents);
        }
        CHECK(highest - lowest < 1.5);
        CHECK_THAT(selfOscillate(rate, 440.0, 4.4f, warm).cents, WithinAbs(expected, 0.2));
    }
    // Further past the threshold: louder and flatter.
    const auto a = selfOscillate(rate, 440.0, 4.05f);
    const auto b = selfOscillate(rate, 440.0, 4.4f);
    const auto c = selfOscillate(rate, 440.0, 6.0f);
    CHECK(a.cents < 0.0);
    CHECK(b.cents < a.cents);
    CHECK(c.cents < b.cents);
    CHECK(a.levelDb < b.levelDb);
    CHECK(b.levelDb < c.levelDb);
    // Bounded by the saturators: under 0 dBFS at the LP24 output even at k 8
    // (the report: -10.9 dBFS).
    CHECK(selfOscillate(rate, 440.0, 8.0f).levelDb < 0.0);
}

TEST_CASE("Ladder filter: silence stays silent at any resonance", "[dsp][filter]")
{
    auto f = makeFilter(48000.0, {.cutoff = 440.0, .k = 8.0f});
    const auto y = render(f, std::vector<float>(48000, 0.0f));
    CHECK(std::ranges::all_of(y, [](float v) { return v == 0.0f; }));
}

TEST_CASE("Ladder filter: the passband against resonance", "[dsp][filter]")
{
    // LP24 at DC is 1/(1 + k): a probe at 20 Hz with a 2 kHz cutoff reads the
    // prototype there, which is within 0.002 dB of 1/(1 + k) (at k 3.9,
    // |(1 + 0.01j)^4 + k| differs from 1 + k by 2e-4 relative). HP24 keeps
    // its passband: at 10 kHz with a 500 Hz cutoff it is within 0.1 dB of 1
    // for every k (the prototype's own deviation there is at most 0.04 dB, its
    // 4/s term at s = 20j). Probes at -40 dBFS, so the saturators are linear
    // to 1e-4 relative.
    constexpr double rate = 48000.0;
    for (const float k : {0.0f, 1.0f, 2.0f, 3.0f, 3.9f})
    {
        CAPTURE(k);
        const test::MonoProcess lowpass = [&](std::span<const float> in)
        {
            auto f = makeFilter(rate, {.cutoff = 2000.0, .k = k});
            return render(f, in);
        };
        const auto low = test::measureResponseAt(lowpass, 20.0, rate, 0.01);
        const double expected = std::abs(prototype(FilterResponse::lp24, k, 2000.0, rate, 2.0 * pi * 20.0 / rate));
        CHECK_THAT(low.magnitude, Catch::Matchers::WithinRel(expected, 1e-3));
        CHECK_THAT(low.magnitudeDb, WithinAbs(-20.0 * std::log10(1.0 + static_cast<double>(k)), 0.01));

        const test::MonoProcess highpass = [&](std::span<const float> in)
        {
            auto f = makeFilter(rate, {.cutoff = 500.0, .k = k, .response = FilterResponse::hp24});
            return render(f, in);
        };
        CHECK_THAT(test::measureResponseAt(highpass, 10000.0, rate, 0.01).magnitudeDb, WithinAbs(0.0, 0.1));
    }
}

TEST_CASE("Ladder filter: drive is a gain on the input", "[dsp][filter]")
{
    // At -80 dBFS (times a drive of 4: -68 dBFS) the saturators are linear
    // to (4e-4)^2 / 3 ~ 5e-8, so driving 4x scales the output 4x.
    auto plain = makeFilter(48000.0, {.cutoff = 3000.0, .k = 3.0f});
    auto driven = makeFilter(48000.0, {.cutoff = 3000.0, .k = 3.0f, .drive = 4.0f});
    const auto x = test::impulse(4096, 0, 1e-4f);
    const auto a = render(plain, x);
    const auto b = render(driven, x);
    double peak = 0.0;
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        peak = std::max(peak, static_cast<double>(std::fabs(b[i])));
        worst = std::max(worst, static_cast<double>(std::fabs(b[i] - (4.0f * a[i]))));
    }
    CHECK(worst < 1e-5 * peak);
}

TEST_CASE("Ladder filter: Warm Drive (tanh) has stronger upper harmonics than Warm off (cubic)", "[dsp][filter]")
{
    // A 220 Hz sine at -6 dBFS into LP24 at 10 kHz, k 0, drive 1. The report
    // measured tanh's H5, H7, H9 5.1, 5.5, and 6.6 dB above the cubic's
    // (docs/dsp/filters.md); each must be at least 3 dB above.
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 65536;
    constexpr std::size_t settle = 8192;
    const double f0 = test::coherentFrequency(220.0, rate, n);
    const auto harmonics = [&](bool warm)
    {
        auto f = makeFilter(rate, {.cutoff = 10000.0, .warm = warm});
        const auto y = render(f, test::sine(f0, rate, settle + n, 0.5));
        return test::measureHarmonics(std::span{y}.subspan(settle), f0, rate, test::rectangularWindow(n), 20000.0);
    };
    const auto tanh = harmonics(true);
    const auto cubic = harmonics(false);
    for (const std::size_t h : {5U, 7U, 9U})
    {
        CAPTURE(h);
        CHECK(20.0 * std::log10(tanh[h - 1] / cubic[h - 1]) > 3.0);
    }
}

TEST_CASE("Ladder filter: bounded at maximum resonance and drive, under an audio-rate sweep and DC", "[dsp][filter]")
{
    // LP24 passes at most drive * |x| / (1 + k) of a slow input (the
    // loop's DC gain), plus a self-oscillation under 0 dBFS (above): a bound
    // of drive * max|x| / (1 + k) + 1. The report measured 2.45 (sweep) and
    // 1.96 (DC) against bounds of 4.6 and 2.8.
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 96000;
    for (const bool warm : {true, false})
    {
        CAPTURE(warm);
        auto f = makeFilter(rate, {.cutoff = 1000.0, .k = 8.0f, .drive = 32.0f, .warm = warm});
        auto x = saw(110.0, rate, n);
        Buffer buffer{1, n};
        std::ranges::copy(x, buffer.channel(0).begin());
        for (std::size_t i = 0; i < n; i += 32)
        {
            // The cutoff 20 Hz - 20 kHz at 500 Hz, through the grid-rate input.
            const double t = static_cast<double>(i) / rate;
            f.setCutoffModulation(static_cast<float>((4.98 * std::sin(2.0 * pi * 500.0 * t)) + std::log2(0.6325)));
            f.process(buffer.block().subBlock(i, 32));
        }
        double peak = 0.0;
        bool finite = true;
        for (const float v : buffer.channel(0))
        {
            finite = finite && std::isfinite(v);
            peak = std::max(peak, static_cast<double>(std::fabs(v)));
        }
        CHECK(finite);
        CHECK(peak < (32.0 / 9.0) + 1.0);

        auto dc = makeFilter(rate, {.cutoff = 1000.0, .k = 4.5f, .warm = warm});
        std::vector<float> steps(n, 0.0f);
        std::fill(steps.begin() + (n / 4), steps.begin() + (n / 2), 1.0f);
        std::fill(steps.begin() + (n / 2), steps.begin() + (3 * n / 4), 10.0f);
        std::fill(steps.begin() + (3 * n / 4), steps.end(), -10.0f);
        const auto y = render(dc, steps);
        CHECK(std::ranges::all_of(y, [](float v) { return std::isfinite(v) && std::fabs(v) < (10.0f / 5.5f) + 1.0f; }));
    }
}

TEST_CASE("Ladder filter: bypass passes the input bit for bit, and leaving it starts from rest", "[dsp][filter]")
{
    constexpr double rate = 48000.0;
    const auto x = saw(220.0, rate, 4096, 0.7f);
    auto f = makeFilter(rate, {.cutoff = 800.0, .k = 3.5f, .drive = 4.0f});
    static_cast<void>(render(f, x)); // some state
    f.setBypassed(true);
    CHECK(f.isBypassed());
    const auto bypassed = render(f, x);
    CHECK(bitIdentical(bypassed, x));

    f.setBypassed(false);
    CHECK_FALSE(f.isBypassed());
    auto fresh = makeFilter(rate, {.cutoff = 800.0, .k = 3.5f, .drive = 4.0f});
    CHECK(bitIdentical(render(f, x), render(fresh, x)));
}

TEST_CASE("Ladder filter: a response change crossfades the taps over 10 ms", "[dsp][filter]")
{
    // The state does not depend on the taps, so during the fade the output is
    // exactly (1 - b) LP24 + b HP24 of the same state, b rising linearly over
    // round(0.01 fs) samples from the change. Float rounding of the blended
    // weights and the five-term mix: a few ulp of outputs under 1, so 1e-6.
    constexpr double rate = 48000.0;
    constexpr std::size_t at = 1000;
    constexpr std::size_t ramp = 480;
    const auto x = saw(150.0, rate, 4096, 0.5f);
    auto lp = makeFilter(rate, {.cutoff = 2000.0, .k = 2.0f});
    auto hp = makeFilter(rate, {.cutoff = 2000.0, .k = 2.0f, .response = FilterResponse::hp24});
    const auto yl = render(lp, x);
    const auto yh = render(hp, x);

    auto f = makeFilter(rate, {.cutoff = 2000.0, .k = 2.0f});
    Buffer buffer{1, x.size()};
    std::ranges::copy(x, buffer.channel(0).begin());
    f.process(buffer.block().subBlock(0, at));
    f.setResponse(FilterResponse::hp24);
    f.process(buffer.block().subBlock(at, x.size() - at));
    const auto y = buffer.channel(0);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        double b = 0.0;
        if (i >= at)
        {
            b = std::min(1.0, static_cast<double>(i - at + 1) / static_cast<double>(ramp));
        }
        const double expected = ((1.0 - b) * static_cast<double>(yl[i])) + (b * static_cast<double>(yh[i]));
        CAPTURE(i);
        REQUIRE_THAT(static_cast<double>(y[i]), WithinAbs(expected, 1e-6));
    }
}

TEST_CASE("Ladder filter: a cutoff step ramps in log2 over 10 ms, applied on the 32-sample grid", "[dsp][filter]")
{
    // currentCutoff() is the cutoff at the end of the current grid segment.
    // A step set between two grid points starts at the next one and lands
    // round(0.01 fs) samples later; meanwhile log2 of the cutoff rises
    // linearly (float rounding of the smoother and exp2: 1e-6 relative).
    constexpr double rate = 48000.0;
    constexpr std::size_t ramp = 480;
    auto f = makeFilter(rate, {.cutoff = 500.0});
    Buffer buffer{1, 4096};
    f.process(buffer.block().subBlock(0, 40)); // mid-segment
    f.setCutoff(4000.0f);
    std::size_t done = 40;
    for (; done < 64 + ramp + 64; ++done)
    {
        f.process(buffer.block().subBlock(done, 1));
        // After sample `done`, the segment that holds the next sample ends at
        // the next multiple of 32 above `done`; its end is 64 + ... samples
        // from where the ramp started (sample 64, the first grid point after
        // the step).
        const auto segmentEnd = ((done + 1 + 31) / 32) * 32;
        const double elapsed =
            segmentEnd <= 64 ? 0.0 : std::min(1.0, static_cast<double>(segmentEnd - 64) / static_cast<double>(ramp));
        const double expected = 500.0 * std::exp2(3.0 * elapsed);
        CAPTURE(done);
        REQUIRE_THAT(static_cast<double>(f.currentCutoff()), Catch::Matchers::WithinRel(expected, 1e-5));
    }
}

TEST_CASE("Ladder filter: a cutoff step at resonance clicks far less than a jump", "[dsp][filter]")
{
    // A resonant cutoff step 500 Hz -> 4 kHz under a 150 Hz sine excites the
    // resonance. The 10 ms log2 ramp of setCutoff() against the same step
    // through the grid-rate modulation input (one 32-sample ramp): the
    // report measured 19 dB between a ramp and an instant step; at least
    // 10 dB here.
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 48000;
    constexpr std::size_t at = 24000; // a grid point
    const auto x = test::sine(150.0, rate, n, 0.25);
    const auto run = [&](bool ramped)
    {
        auto f = makeFilter(rate, {.cutoff = 500.0, .k = 3.6f});
        Buffer buffer{1, n};
        std::ranges::copy(x, buffer.channel(0).begin());
        f.process(buffer.block().subBlock(0, at));
        if (ramped)
        {
            f.setCutoff(4000.0f);
        }
        else
        {
            f.setCutoffModulation(3.0f);
        }
        f.process(buffer.block().subBlock(at, n - at));
        return std::vector<float>(buffer.channel(0).begin(), buffer.channel(0).end());
    };
    const double jumped = energyAbove1k(run(false), at + 32, rate);
    const double ramped = energyAbove1k(run(true), at + 32, rate);
    CHECK(10.0 * std::log10(jumped / ramped) > 10.0);
}

TEST_CASE("Ladder filter: output does not depend on the block size", "[dsp][filter]")
{
    // Controls change at fixed samples (blocks are split there), so blocks of
    // 1, 37, and 4096 must give the same output bit for bit: the grid is
    // counted in samples from reset(), and the ramps are per sample.
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 20000;
    const auto x = saw(97.0, rate, n, 0.8f);
    const auto renderInBlocks = [&](std::size_t blockSize)
    {
        auto f = makeFilter(rate, {.cutoff = 300.0, .k = 3.0f, .drive = 3.0f});
        Buffer buffer{1, n};
        std::ranges::copy(x, buffer.channel(0).begin());
        const std::array<std::size_t, 6> events{1500, 3333, 7001, 9000, 12345, 16000};
        std::size_t start = 0;
        for (std::size_t e = 0; e <= events.size(); ++e)
        {
            const std::size_t end = e < events.size() ? events[e] : n;
            dsp::processInChunks(f, buffer.block().subBlock(start, end - start), blockSize);
            start = end;
            switch (e)
            {
            case 0:
                f.setCutoff(5000.0f);
                break;
            case 1:
                f.setResonance(4.6f);
                break;
            case 2:
                f.setResponse(FilterResponse::bp12);
                break;
            case 3:
                f.setWarm(false);
                break;
            case 4:
                f.setDrive(16.0f);
                f.setCutoffModulation(-1.5f);
                f.setResonanceModulation(1.5f);
                break;
            case 5:
                f.setBypassed(true);
                break;
            default:
                break;
            }
        }
        return std::vector<float>(buffer.channel(0).begin(), buffer.channel(0).end());
    };
    const auto reference = renderInBlocks(4096);
    CHECK(bitIdentical(renderInBlocks(1), reference));
    CHECK(bitIdentical(renderInBlocks(37), reference));
}

TEST_CASE("Ladder filter: stereo channels are independent and identical", "[dsp][filter]")
{
    constexpr double rate = 48000.0;
    auto f = makeFilter(rate, {.cutoff = 1200.0, .k = 3.8f, .drive = 4.0f}, 2);
    const auto x = saw(110.0, rate, 8192);
    Buffer buffer{2, x.size()};
    std::ranges::copy(x, buffer.channel(0).begin());
    std::ranges::copy(x, buffer.channel(1).begin());
    dsp::processInChunks(f, buffer.block(), 4096);
    CHECK(bitIdentical(buffer.channel(0), buffer.channel(1)));

    auto mono = makeFilter(rate, {.cutoff = 1200.0, .k = 3.8f, .drive = 4.0f});
    CHECK(bitIdentical(buffer.channel(0), render(mono, x)));

    auto half = makeFilter(rate, {.cutoff = 1200.0, .k = 3.8f, .drive = 4.0f}, 2);
    Buffer quiet{2, x.size()};
    std::ranges::copy(x, quiet.channel(0).begin());
    dsp::processInChunks(half, quiet.block(), 4096);
    CHECK(bitIdentical(quiet.channel(0), buffer.channel(0)));
    CHECK(std::ranges::all_of(quiet.channel(1), [](float v) { return v == 0.0f; }));
}

TEST_CASE("Ladder filter: the loop is solved: a double-precision reference agrees", "[dsp][filter]")
{
    // The same trapezoidal equations solved in double with std::tanh, by
    // Newton to 1e-13, on hard material: a 110 Hz saw at -6 dBFS, drive 4,
    // k 3.8, a 1 kHz cutoff (constant, so the grid plays no part), 1 s at
    // 48 kHz. The float filter stops at 1e-5 per stage and fastTanh is
    // within 2e-7 of tanh: the per-sample error is a few 1e-7 (the converged
    // Newton step ends far below its 1e-5 tolerance, being quadratic),
    // amplified at most by the loop's gain at resonance (1/(4 - k) = 5), so
    // about -120 dB of the signal; checked against -100 dB. (Measured:
    // -119.7 dB; one Newton step from the linearised guess, -81.2 dB.)
    constexpr double rate = 48000.0;
    constexpr double k = 3.8;
    const auto x = saw(110.0, rate, 48000, 0.5f);
    auto f = makeFilter(rate, {.cutoff = 1000.0, .k = static_cast<float>(k), .drive = 4.0f});
    const auto y = render(f, x);

    const double g = std::tan(pi * 1000.0 / rate);
    std::array<double, 4> s{};
    std::array<double, 4> v{};
    double errorPower = 0.0;
    double signalPower = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n)
    {
        const double input = 4.0 * static_cast<double>(x[n]);
        for (int iteration = 0; iteration < 100; ++iteration)
        {
            // Residuals and Jacobian as in the library, all in double.
            const double u = input - (k * v[3]);
            const double tu = std::tanh(u);
            std::array<double, 4> p{};
            std::array<double, 4> q{};
            double previousT = tu;
            double previousSlope = 1.0 - (tu * tu);
            for (std::size_t i = 0; i < 4; ++i)
            {
                const double t = std::tanh(v[i]);
                const double d = 1.0 - (t * t);
                const double residual = v[i] - s[i] - (g * (previousT - t));
                const double diagonal = 1.0 + (g * d);
                p[i] = i == 0 ? -residual / diagonal : (-residual + (g * previousSlope * p[i - 1])) / diagonal;
                q[i] = i == 0 ? -(g * k * previousSlope) / diagonal : g * previousSlope * q[i - 1] / diagonal;
                previousT = t;
                previousSlope = d;
            }
            const double delta4 = p[3] / (1.0 - q[3]);
            double largest = 0.0;
            for (std::size_t i = 0; i < 4; ++i)
            {
                const double delta = i == 3 ? delta4 : p[i] + (q[i] * delta4);
                v[i] += delta;
                largest = std::max(largest, std::fabs(delta));
            }
            if (largest < 1e-13)
            {
                break;
            }
        }
        for (std::size_t i = 0; i < 4; ++i)
        {
            s[i] = (2.0 * v[i]) - s[i];
        }
        if (n >= 4800) // after the first 100 ms
        {
            const double e = static_cast<double>(y[n]) - v[3];
            errorPower += e * e;
            signalPower += v[3] * v[3];
        }
    }
    CHECK(10.0 * std::log10(errorPower / signalPower) < -100.0);
}

TEST_CASE("Ladder filter: reset() restarts the grid", "[dsp][filter]")
{
    // A filter reset part-way through a grid segment must count its grid
    // from the reset, as a fresh one does: a cutoff step after it then
    // lands on the same sample in both.
    constexpr double rate = 48000.0;
    const auto x = saw(130.0, rate, 4096, 0.6f);
    const auto run = [&](LadderFilter& f)
    {
        Buffer buffer{1, x.size()};
        std::ranges::copy(x, buffer.channel(0).begin());
        f.process(buffer.block().subBlock(0, 10));
        f.setCutoff(3000.0f);
        f.process(buffer.block().subBlock(10, x.size() - 10));
        return std::vector<float>(buffer.channel(0).begin(), buffer.channel(0).end());
    };
    auto used = makeFilter(rate, {.cutoff = 400.0, .k = 3.0f});
    Buffer dirt{1, 45};
    std::ranges::copy(saw(77.0, rate, 45), dirt.channel(0).begin());
    used.process(dirt.block());
    used.reset();
    auto fresh = makeFilter(rate, {.cutoff = 400.0, .k = 3.0f});
    CHECK(bitIdentical(run(used), run(fresh)));
}

TEST_CASE("Ladder filter honours the lifecycle contract", "[dsp][filter]")
{
    const auto make = []
    {
        LadderFilter f;
        f.setCutoff(900.0f);
        f.setResonance(3.7f);
        f.setDrive(3.0f);
        f.setResponse(FilterResponse::bp24);
        return f;
    };
    const auto result = test::checkLifecycleContract(make, specAt(48000.0, 2), 8192);
    CHECK(result.resetMatchesFreshPrepare);
    CHECK(result.rePrepareMatchesFreshPrepare);
}

TEST_CASE("Ladder filter: allocates nothing while processing or changing controls", "[dsp][filter]")
{
    auto f = makeFilter(48000.0, {.cutoff = 900.0, .k = 3.0f}, 2);
    Buffer buffer{2, 4096};
    std::ranges::copy(saw(100.0, 48000.0, 4096), buffer.channel(0).begin());
    std::size_t allocations = 0;
    {
        const test::AllocationCounter counter;
        f.process(buffer.block());
        f.setCutoff(3000.0f);
        f.setCutoffModulation(1.0f);
        f.setResonance(5.0f);
        f.setDrive(10.0f);
        f.setResponse(FilterResponse::hp12);
        f.setWarm(false);
        f.process(buffer.block());
        f.setBypassed(true);
        f.process(buffer.block());
        f.setBypassed(false);
        f.reset();
        allocations = counter.count();
    }
    CHECK(allocations == 0);
}

TEST_CASE("Ladder filter: silence after a sound reaches exactly 0, with no subnormal output", "[dsp][filter]")
{
    // Without FTZ. LP24 at 100 Hz, k 3: the slowest poles decay at
    // (1 - 3^(1/4)/sqrt 2) wc = 0.0694 x 2 pi 100 per second, a time
    // constant of 22.9 ms, so from 1 to below FLT_MIN (87.3 time constants)
    // takes at most 2.0 s (less: the response's peak is under 1). The states
    // are flushed there, so the filter is at rest (isAtRest()) by then;
    // unflushed, they would run on through the subnormals. The output is
    // flushed too, so it is never subnormal.
    constexpr double rate = 48000.0;
    for (const bool warm : {true, false})
    {
        auto f = makeFilter(rate, {.cutoff = 100.0, .k = 3.0f, .warm = warm});
        CHECK(f.isAtRest());
        std::vector<float> x(static_cast<std::size_t>(3.0 * rate), 0.0f);
        x[0] = 1.0f;
        Buffer buffer{1, x.size()};
        std::ranges::copy(x, buffer.channel(0).begin());
        std::size_t restAt = x.size();
        for (std::size_t start = 0; start < x.size(); start += 480)
        {
            f.process(buffer.block().subBlock(start, 480));
            if (restAt == x.size() && f.isAtRest())
            {
                restAt = start + 480;
            }
        }
        const auto y = buffer.channel(0);
        CHECK(std::ranges::none_of(y, [](float v) { return std::fpclassify(v) == FP_SUBNORMAL; }));
        CHECK(static_cast<double>(restAt) / rate < 2.0);
        CHECK(std::all_of(y.begin() + static_cast<std::ptrdiff_t>(restAt), y.end(), [](float v) { return v == 0.0f; }));
    }
}

TEST_CASE("Ladder filter: between grid points g is interpolated linearly", "[dsp][filter]")
{
    // A modulation of +1 octave set mid-segment is read at the next grid
    // point G: g then runs linearly from its old value at G to the new one at
    // G + 32. At -80 dBFS the ladder is linear, so a double-precision linear
    // trapezoidal ladder (the same equations, no saturators) fed the same g
    // per sample must agree to float rounding: 1e-4 of the output's peak
    // (float tan of g, about 1e-7 relative, and the loop's rounding). Holding
    // g for the segment, or stepping it at G, differs by percent.
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 2048;
    constexpr std::size_t setAt = 1000; // the grid point after it: 1024
    constexpr std::size_t grid = 1024;
    constexpr double k = 3.0;
    const auto x = saw(310.0, rate, n, 1e-4f);
    auto f = makeFilter(rate, {.cutoff = 700.0, .k = static_cast<float>(k)});
    Buffer buffer{1, n};
    std::ranges::copy(x, buffer.channel(0).begin());
    f.process(buffer.block().subBlock(0, setAt));
    f.setCutoffModulation(1.0f);
    f.process(buffer.block().subBlock(setAt, n - setAt));
    const auto y = buffer.channel(0);

    const double g0 = std::tan(pi * 700.0 / rate);
    const double g1 = std::tan(pi * 1400.0 / rate);
    std::array<double, 4> s{};
    double peak = 0.0;
    double worst = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        double g = g0;
        if (i >= grid + 32)
        {
            g = g1;
        }
        else if (i >= grid)
        {
            g = g0 + ((g1 - g0) * static_cast<double>(i - grid) / 32.0);
        }
        const double big = g / (1.0 + g);
        double sum = 0.0;
        double power = 1.0;
        for (std::size_t j = 4; j-- > 0;)
        {
            sum += power * s[j] / (1.0 + g);
            power *= big;
        }
        const double u = (static_cast<double>(x[i]) - (k * sum)) / (1.0 + (k * power));
        double in = u;
        for (std::size_t j = 0; j < 4; ++j)
        {
            const double out = (big * in) + (s[j] / (1.0 + g));
            s[j] = (2.0 * out) - s[j];
            in = out;
        }
        peak = std::max(peak, std::fabs(in));
        worst = std::max(worst, std::fabs(static_cast<double>(y[i]) - in));
    }
    CHECK(worst < 1e-4 * peak);
}

TEST_CASE("Ladder filter: controls clamp, and NaN is safe", "[dsp][filter]")
{
    constexpr double rate = 48000.0;
    const auto x = saw(220.0, rate, 4096);
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    for (const float bad : {nan, inf, -inf, -1.0f, 0.0f, 1e9f})
    {
        CAPTURE(bad);
        LadderFilter f;
        f.setCutoff(bad);
        f.setCutoffModulation(bad);
        f.setResonance(bad);
        f.setResonanceModulation(bad);
        f.setDrive(bad);
        f.prepare(specAt(rate));
        const auto y = render(f, x);
        CHECK(std::ranges::all_of(y, [](float v) { return std::isfinite(v); }));
        CHECK(f.currentCutoff() >= 1.0f);
        CHECK(f.currentCutoff() <= 0.5f * static_cast<float>(rate));
    }
}

TEST_CASE("Ladder filter: a randomized stress test at every rate and block size", "[dsp][filter]")
{
    // Random controls at random samples, every response, Warm and bypass
    // toggled, 44.1-192 kHz: finite, bounded by drive * max|x| x (the largest
    // tap sum, 16) + 1, and bit-identical across blocks of 1, 37, 4096.
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        CAPTURE(rate);
        constexpr std::size_t n = 30000;
        const auto x = saw(73.0, rate, n);
        const auto renderInBlocks = [&](std::size_t blockSize)
        {
            LadderFilter f;
            f.prepare(specAt(rate));
            Buffer buffer{1, n};
            std::ranges::copy(x, buffer.channel(0).begin());
            std::uint32_t state = 12345;
            const auto next = [&]
            {
                state = (state * 1664525U) + 1013904223U;
                return static_cast<float>(state >> 8U) / 16777216.0f;
            };
            std::size_t start = 0;
            float maxDrive = 1.0f;
            while (start < n)
            {
                const auto length = std::min(n - start, 1 + static_cast<std::size_t>(next() * 700.0f));
                dsp::processInChunks(f, buffer.block().subBlock(start, length), blockSize);
                start += length;
                f.setCutoff(20.0f * std::exp2(next() * 10.0f));
                f.setCutoffModulation((next() * 4.0f) - 2.0f);
                f.setResonance(next() * 8.0f);
                const float drive = 1.0f + (next() * 31.0f);
                maxDrive = std::max(maxDrive, drive);
                f.setDrive(drive);
                f.setResponse(allResponses[static_cast<std::size_t>(next() * 5.999f)]);
                f.setWarm(next() < 0.5f);
                f.setBypassed(next() < 0.1f);
            }
            const std::vector<float> y(buffer.channel(0).begin(), buffer.channel(0).end());
            CHECK(std::ranges::all_of(y, [&](float v)
                                      { return std::isfinite(v) && std::fabs(v) < (maxDrive * 16.0f) + 1.0f; }));
            return y;
        };
        const auto reference = renderInBlocks(4096);
        CHECK(bitIdentical(renderInBlocks(1), reference));
        CHECK(bitIdentical(renderInBlocks(37), reference));
    }
}
