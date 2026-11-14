// Behavioural tests for the band-limited oscillators: the minBLEP oscillator
// (decision D6, at 32 and 64 taps) and the PolyBLEP reference, all held to:
// waveform shape, aliasing, stability from 0.01 Hz to Nyquist, DC, per-sample
// pitch changes, and reset. The measurements that decided D6 are in
// antialiasing_report.cpp. Doc: docs/dsp/band-limited-oscillators.md

#include "oscillator_support.hpp"

#include <anoesis/test/measure.hpp>
#include <anoesis/test/signals.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;
using Catch::Matchers::WithinAbs;

namespace
{

constexpr double rate = 48000.0;
constexpr std::array allShapes{Waveform::saw, Waveform::pulse, Waveform::triangle};

anoesis::dsp::ProcessSpec specAt(double sampleRate)
{
    return {.sampleRate = static_cast<float>(sampleRate), .maxBlockSize = 512, .numChannels = 1};
}

template <typename Osc>
Osc prepared(Waveform shape, double sampleRate = rate)
{
    auto osc = Candidate<Osc>::make();
    osc.prepare(specAt(sampleRate));
    osc.setWaveform(shape);
    return osc;
}

template <typename Osc>
std::vector<float> renderPlain(Osc& osc, std::size_t numSamples)
{
    std::vector<float> out(numSamples);
    for (auto& sample : out)
    {
        sample = osc.next();
    }
    return out;
}

// Distance in samples from phase t to the nearest point where `shape` jumps
// or bends, given dt cycles per sample.
double distanceToEdge(Waveform shape, double t, double dt, double width)
{
    const auto distance = [&](double edge)
    {
        const auto d = std::abs(t - edge);
        return std::min(d, 1.0 - d) / dt;
    };
    switch (shape)
    {
    case Waveform::saw:
        return distance(0.0);
    case Waveform::pulse:
        return std::min(distance(0.0), distance(width));
    case Waveform::triangle:
        return std::min(distance(0.0), distance(0.5));
    }
    return 0.0;
}

} // namespace

TEMPLATE_TEST_CASE("Band-limited oscillators follow the ideal waveform away from its edges", "[dsp][oscillator][d6]",
                   PolyBlep2, PolyBlep4, MinBlep32, MinBlep64)
{
    // Sample n is the waveform at phase n dt, delayed by latencySamples().
    constexpr std::uint32_t increment = 1U << 22U; // 2^-10 cycles per sample: 46.9 Hz at 48 kHz
    const double dt = std::ldexp(1.0, -10);
    const double width = 0.3;

    for (const auto shape : allShapes)
    {
        CAPTURE(Candidate<TestType>::name, static_cast<int>(shape));
        auto osc = prepared<TestType>(shape);
        osc.setPulseWidth(static_cast<float>(width));
        osc.setIncrement(increment);
        const auto out = renderPlain(osc, 4096);
        const auto delay = static_cast<double>(osc.latencySamples());

        std::size_t checked = 0;
        for (std::size_t n = 0; n < out.size(); ++n)
        {
            const auto position = (static_cast<double>(n) - delay) * dt;
            const auto t = position - std::floor(position);
            if (position < 0.0 ||
                distanceToEdge(shape, t, dt, width) <= static_cast<double>(Candidate<TestType>::guardSamples))
            {
                continue;
            }
            CAPTURE(n, t);
            REQUIRE_THAT(out[n], WithinAbs(idealWave(shape, t, width), Candidate<TestType>::tolerance));
            ++checked;
        }
        CHECK(checked > 3000);
    }
}

TEMPLATE_TEST_CASE("Band-limited oscillators attenuate aliasing of a high note", "[dsp][oscillator][d6]", PolyBlep2,
                   PolyBlep4, MinBlep32, MinBlep64)
{
    // A saw near 5 kHz at 48 kHz: its 5th to 9th harmonics fold back into the
    // audible band. A naive saw's worst alias here is about -20 dB.
    constexpr std::size_t n = 65536;
    const auto cycles = static_cast<std::uint64_t>(std::lround(coherentFrequency(5000.0, rate, n) * n / rate));
    auto osc = prepared<TestType>(Waveform::saw);
    osc.setIncrement(coherentIncrement(cycles, n));
    const auto out = render(osc, rate, 1, 4096, n);
    const auto f0 = static_cast<double>(cycles) * rate / static_cast<double>(n);
    const auto alias = measureAliasing(out, f0, rate, rectangularWindow(n), 20000.0);
    CAPTURE(Candidate<TestType>::name, alias.worstAliasDb, alias.worstAliasHz);
    // The weakest candidate, the classic PolyBLEP, measures -28.3 dB: its 6th
    // harmonic (30 kHz) times its kernel's sinc^2 (-15 dB there) folds to
    // 18 kHz. Theory: 1/6 x sinc^2(0.625) = -28.7 dB.
    CHECK(alias.worstAliasDb < -27.0);
}

TEMPLATE_TEST_CASE("Band-limited oscillators stay finite and bounded from 0.01 Hz to Nyquist", "[dsp][oscillator][d6]",
                   PolyBlep2, PolyBlep4, MinBlep32, MinBlep64)
{
    for (const double sampleRate : {44100.0, 48000.0})
    {
        for (const double hz : {
                 0.0,
                 0.01,
                 1.0,
                 20.0,
                 1000.0,
                 5000.0,
                 10000.0,
                 15000.0,
                 20000.0,
                 21000.0,
                 (sampleRate / 2.0) - 1.0,
                 sampleRate / 2.0,
                 sampleRate,
             })
        {
            for (const auto shape : allShapes)
            {
                CAPTURE(Candidate<TestType>::name, sampleRate, hz, static_cast<int>(shape));
                auto osc = prepared<TestType>(shape, sampleRate);
                osc.setPulseWidth(0.1f);
                osc.setFrequency(static_cast<float>(hz));
                const auto out = renderPlain(osc, 8192);
                // Each candidate's own overshoot (see Candidate::peak); a
                // blow-up would be far beyond it.
                CHECK(std::ranges::all_of(out, [](float x)
                                          { return std::isfinite(x) && std::abs(x) < Candidate<TestType>::peak; }));
                CHECK(std::ranges::none_of(out, [](float x) { return std::fpclassify(x) == FP_SUBNORMAL; }));
            }
        }
    }
}

TEMPLATE_TEST_CASE("Band-limited oscillators add no DC offset", "[dsp][oscillator][d6]", PolyBlep2, PolyBlep4,
                   MinBlep32, MinBlep64)
{
    constexpr std::size_t n = 16384;
    for (const double target : {50.0, 1000.0, 5000.0, 12000.0, 20000.0})
    {
        const auto cycles = static_cast<std::uint64_t>(std::lround(coherentFrequency(target, rate, n) * n / rate));
        for (const auto shape : allShapes)
        {
            CAPTURE(Candidate<TestType>::name, target, static_cast<int>(shape));
            auto osc = prepared<TestType>(shape);
            osc.setPulseWidth(0.25f);
            osc.setIncrement(coherentIncrement(cycles, n));
            const auto out = render(osc, rate, 1, 4096, n);
            const auto mean = std::accumulate(out.begin(), out.end(), 0.0) / static_cast<double>(n);
            // A 25% pulse's ideal mean is 2 w - 1 = -0.5; the others' is 0.
            // 1e-3 is -60 dB: far below anything audible as an offset.
            CHECK_THAT(mean, WithinAbs(shape == Waveform::pulse ? -0.5 : 0.0, 1e-3));
        }
    }
}

TEMPLATE_TEST_CASE("Band-limited oscillators accept a new increment every sample", "[dsp][oscillator][d6]", PolyBlep2,
                   PolyBlep4, MinBlep32, MinBlep64)
{
    SECTION("re-setting the same increment changes nothing")
    {
        for (const auto shape : allShapes)
        {
            auto plain = prepared<TestType>(shape);
            auto touched = prepared<TestType>(shape);
            plain.setFrequency(3000.0f);
            touched.setFrequency(3000.0f);
            std::vector<float> a(4096);
            std::vector<float> b(4096);
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                a[i] = plain.next();
                touched.setFrequency(3000.0f);
                b[i] = touched.next();
            }
            CHECK(a == b);
        }
    }

    SECTION("a deep, fast vibrato aliases no worse than the highest pitch it reaches")
    {
        // Linear FM, f(t) = f0 (1 + 0.25 sin(2 pi g t)), with g and f0 exact
        // multiples of a coherent base frequency. The render is then periodic
        // and every true component lies on a multiple of g; aliases fall
        // between them. See antialiasing_report.cpp for the full sweep.
        constexpr std::size_t n = 65536;
        constexpr std::uint64_t vibratoCycles = 9;                   // g = 6.6 Hz
        constexpr std::uint64_t carrierCycles = vibratoCycles * 300; // f0 = 1978 Hz, peaking at 2472 Hz
        const auto g = static_cast<double>(vibratoCycles) * rate / static_cast<double>(n);
        auto osc = prepared<TestType>(Waveform::saw);
        const PerSample<TestType> vibrato = [&](TestType& o, std::size_t i)
        {
            const auto phase = 2.0 * std::numbers::pi * static_cast<double>(vibratoCycles * (i % n)) / n;
            const auto cyclesPerSample =
                static_cast<double>(carrierCycles) / static_cast<double>(n) * (1.0 + (0.25 * std::sin(phase)));
            o.setIncrement(static_cast<std::uint32_t>(std::lround(std::ldexp(cyclesPerSample, 32))));
        };
        const auto out = render(osc, rate, 1, n, n, vibrato);
        const auto window = rectangularWindow(n);
        const auto alias = measureAliasing(out, g, rate, window, 20000.0);
        const auto worst =
            worstAliasReSawFundamental(alias.worstAliasDb, powerSpectrum(out, rate, window).powerAround(g, 1));
        CAPTURE(Candidate<TestType>::name, worst, alias.worstAliasHz);
        // The FM spreads each harmonic's energy over many sidebands, so a
        // per-sample error would show as a broadband floor. -50 dB: the
        // classic PolyBLEP's static aliasing at 2.5 kHz is about -60 dB.
        CHECK(worst < -50.0);
    }
}

TEMPLATE_TEST_CASE("Band-limited oscillators restart identically after reset", "[dsp][oscillator][d6]", PolyBlep2,
                   PolyBlep4, MinBlep32, MinBlep64)
{
    for (const auto shape : allShapes)
    {
        auto osc = prepared<TestType>(shape);
        osc.setFrequency(4321.0f);
        const auto first = renderPlain(osc, 1000);
        osc.reset();
        const auto second = renderPlain(osc, 1000);
        CHECK(first == second);
    }
}
