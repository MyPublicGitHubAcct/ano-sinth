// Behavioural tests for the band-limited oscillators: the minBLEP oscillator
// (decision D6, at 32 and 64 taps) and the PolyBLEP reference, all held to:
// waveform shape, aliasing, stability from 0.01 Hz to Nyquist, DC, per-sample
// pitch changes, and reset, for each of the 7 periodic waveforms. The
// measurements are in antialiasing_report.cpp.
// Docs: docs/dsp/band-limited-oscillators.md, docs/dsp/oscillator-waveforms.md

#include "oscillator_support.hpp"

#include <anoesis/dsp/math/random.hpp>

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
#include <numbers>
#include <numeric>
#include <type_traits>
#include <utility>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinULP;

namespace
{

constexpr double rate = 48000.0;

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

} // namespace

TEMPLATE_TEST_CASE("Band-limited oscillators follow the ideal waveform away from its edges", "[dsp][oscillator][d6]",
                   PolyBlep2, PolyBlep4, MinBlep32, MinBlep64)
{
    // Sample n is the waveform at phase n dt, delayed by latencySamples().
    // The onset after reset is skipped like an edge (minBLEP starts from
    // silence; see the onset test).
    constexpr std::uint32_t increment = 1U << 22U; // 2^-10 cycles per sample: 46.9 Hz at 48 kHz
    const double dt = std::ldexp(1.0, -10);
    constexpr std::size_t length = 8192;

    for (const auto shape : periodicShapes)
    {
        CAPTURE(Candidate<TestType>::name, shapeName(shape));
        auto osc = prepared<TestType>(shape);
        osc.setIncrement(increment);
        const auto out = renderPlain(osc, length);
        const auto delay = static_cast<double>(osc.latencySamples());
        const auto guard = static_cast<double>(Candidate<TestType>::guardSamples);

        std::size_t checked = 0;
        for (std::size_t n = 0; n < out.size(); ++n)
        {
            const auto position = (static_cast<double>(n) - delay) * dt;
            const auto t = position - std::floor(position);
            if (static_cast<double>(n) <= guard + delay || distanceToEdge(shape, t, dt) <= guard)
            {
                continue;
            }
            CAPTURE(n, t);
            REQUIRE_THAT(out[n], WithinAbs(idealWave(shape, t), Candidate<TestType>::tolerance));
            ++checked;
        }
        // The QPulse, two edges 128 samples apart, leaves the fewest samples
        // outside the guards: about 70% of them at 64 taps.
        CHECK(checked > length * 6 / 10);
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
            for (const auto shape : periodicShapes)
            {
                CAPTURE(Candidate<TestType>::name, sampleRate, hz, shapeName(shape));
                auto osc = prepared<TestType>(shape, sampleRate);
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
        for (const auto shape : periodicShapes)
        {
            CAPTURE(Candidate<TestType>::name, target, shapeName(shape));
            auto osc = prepared<TestType>(shape);
            osc.setIncrement(coherentIncrement(cycles, n));
            const auto out = render(osc, rate, 1, 4096, n);
            const auto mean = std::accumulate(out.begin(), out.end(), 0.0) / static_cast<double>(n);
            // A pulse of width w has the ideal mean 2 w - 1 (the HPulse -0.5,
            // the QPulse -0.75); the others' is 0. 1e-3 is -60 dB: far below
            // anything audible as an offset.
            CHECK_THAT(mean, WithinAbs(idealMean(shape), 1e-3));
        }
    }
}

TEMPLATE_TEST_CASE("Band-limited oscillators accept a new increment every sample", "[dsp][oscillator][d6]", PolyBlep2,
                   PolyBlep4, MinBlep32, MinBlep64)
{
    SECTION("re-setting the same increment changes nothing")
    {
        for (const auto shape : periodicShapes)
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
    for (const auto shape : periodicShapes)
    {
        CAPTURE(shapeName(shape));
        auto osc = prepared<TestType>(shape);
        osc.setFrequency(4321.0f);
        const auto first = renderPlain(osc, 1000);
        osc.reset();
        const auto second = renderPlain(osc, 1000);
        CHECK(first == second);
    }
}

TEMPLATE_TEST_CASE("The sine is a pure tone", "[dsp][oscillator][waveform]", PolyBlep2, PolyBlep4, MinBlep32, MinBlep64)
{
    // The sine has no edges, so neither method adds anything to it: every
    // harmonic above the first is float rounding.
    constexpr std::size_t n = 16384;
    for (const double target : {100.0, 3000.0, 15000.0})
    {
        const auto cycles = static_cast<std::uint64_t>(std::lround(coherentFrequency(target, rate, n) * n / rate));
        auto osc = prepared<TestType>(Waveform::sine);
        osc.setIncrement(coherentIncrement(cycles, n));
        const auto out = render(osc, rate, 1, 4096, n);
        const auto f0 = static_cast<double>(cycles) * rate / static_cast<double>(n);
        const auto harmonics = measureHarmonics(out, f0, rate, rectangularWindow(n), rate / 2.0);
        CAPTURE(Candidate<TestType>::name, target);
        REQUIRE_FALSE(harmonics.empty());
        CHECK_THAT(harmonics[0], WithinAbs(1.0, 1e-5));
        for (std::size_t k = 2; k <= harmonics.size(); ++k)
        {
            CAPTURE(k);
            CHECK(harmonics[k - 1] < 1e-5); // -100 dB
        }
    }
}

TEMPLATE_TEST_CASE("The minBLEP sine is delayed exactly like the other waveforms, even under per-sample pitch changes",
                   "[dsp][oscillator][waveform]", MinBlep32, MinBlep64)
{
    // The edges of every waveform are timed by the phase trajectory: the
    // phase moves linearly, by the increment that arrived, between one
    // sample and the next. Their band-limited output arrives latencySamples()
    // late. The sine must be the sine of that same trajectory, latencySamples()
    // late, so that a sine and a saw stay in phase through any modulation.
    // Here a fast, deep vibrato; the reference trajectory is accumulated in
    // 64-bit arithmetic.
    auto osc = prepared<TestType>(Waveform::sine);
    const auto delay = static_cast<double>(osc.latencySamples());
    constexpr std::size_t length = 4096;
    std::vector<std::uint64_t> trajectory(length + 1, 0); // phase in 2^-32 cycles, unwrapped
    std::vector<std::uint32_t> increments(length);
    for (std::size_t i = 0; i < length; ++i)
    {
        const auto cycles = 0.05 * (1.0 + (0.5 * std::sin(static_cast<double>(i) * 0.03)));
        increments[i] = static_cast<std::uint32_t>(std::ldexp(cycles, 32));
        trajectory[i + 1] = trajectory[i] + increments[i];
    }
    for (std::size_t i = 0; i < length; ++i)
    {
        osc.setIncrement(increments[i]);
        const float y = osc.next();
        // Sample i is the phase at time i - delay. Between samples j and
        // j + 1 the phase moves by the increment set before sample j.
        const double time = static_cast<double>(i) - delay;
        if (time < 1.0 || i < Candidate<TestType>::guardSamples)
        {
            continue; // the onset
        }
        const auto j = static_cast<std::size_t>(std::floor(time));
        const double fraction = time - static_cast<double>(j);
        const double phase = static_cast<double>(trajectory[j]) + (fraction * static_cast<double>(increments[j]));
        const double expected = std::sin(2.0 * std::numbers::pi * std::ldexp(std::fmod(phase, 4294967296.0), -32));
        CAPTURE(i);
        REQUIRE_THAT(y, WithinAbs(expected, 1e-4));
    }
}

TEMPLATE_TEST_CASE("minBLEP starts every waveform from silence after reset", "[dsp][oscillator][waveform]", MinBlep32,
                   MinBlep64)
{
    // The first sample is a band-limited onset: the step (and, for the
    // straight segments, the bend) from silence to the waveform, so the
    // output starts at exactly 0 whatever the shape and pitch.
    for (const double hz : {50.0, 1000.0, 5000.0, 12000.0, 23000.0})
    {
        for (const auto shape : periodicShapes)
        {
            CAPTURE(Candidate<TestType>::name, hz, shapeName(shape));
            auto osc = prepared<TestType>(shape);
            osc.setFrequency(static_cast<float>(hz));
            CHECK_THAT(osc.next(), WithinAbs(0.0, 1e-6));
            const auto rest = renderPlain(osc, 256);
            CHECK(std::ranges::all_of(rest, [](float x) { return std::abs(x) < Candidate<TestType>::peak; }));
        }
    }
}

TEMPLATE_TEST_CASE("Band-limited oscillators switch waveform mid-note without losing the phase",
                   "[dsp][oscillator][waveform]", PolyBlep2, PolyBlep4, MinBlep32, MinBlep64)
{
    // A waveform change takes effect at the next sample, on the same phase.
    // Once the switch has passed through the residuals, the output is the
    // new waveform, delayed as usual. (The switch itself is a jump that is
    // not band-limited; the waveform selector is not meant to be modulated.)
    constexpr std::uint32_t increment = 1U << 22U; // 2^-10 cycles per sample
    const double dt = std::ldexp(1.0, -10);
    constexpr std::size_t before = 1000;
    constexpr std::size_t after = 4096;
    for (const auto from : periodicShapes)
    {
        for (const auto to : periodicShapes)
        {
            CAPTURE(Candidate<TestType>::name, shapeName(from), shapeName(to));
            auto osc = prepared<TestType>(from);
            osc.setIncrement(increment);
            static_cast<void>(renderPlain(osc, before));
            osc.setWaveform(to);
            const auto out = renderPlain(osc, after);
            const auto delay = static_cast<double>(osc.latencySamples());
            const auto guard = static_cast<double>(Candidate<TestType>::guardSamples);
            std::size_t checked = 0;
            for (std::size_t i = 0; i < out.size(); ++i)
            {
                const auto position = (static_cast<double>(before + i) - delay) * dt;
                const auto t = position - std::floor(position);
                if (static_cast<double>(i) <= guard + delay || distanceToEdge(to, t, dt) <= guard)
                {
                    continue;
                }
                CAPTURE(i, t);
                REQUIRE_THAT(out[i], WithinAbs(idealWave(to, t), Candidate<TestType>::tolerance));
                ++checked;
            }
            CHECK(checked > after / 2);
        }
    }
}

TEMPLATE_TEST_CASE("minBLEP band-limits a pitch step: exactly a band-limited bend, so no click",
                   "[dsp][oscillator][range]", MinBlep32, MinBlep64)
{
    // A pitch step at a sample changes the waveform's slope per sample there
    // from s c0 to s c1 (s per cycle): a bend in time of b = s (c1 - c0).
    // Band-limited, the output's deviation from the ideal waveform at the
    // delayed phase is b (R(n) + min(n - delay, 0)) at the n-th sample from
    // the step, R being the table's ramp residual for an edge at a sample:
    // the minimum-phase bend, with the delay compensation in it. Without
    // it, the output is off by b x delay at once (b x 2.79 at 64 taps).
    // The sine is evaluated on its delayed phase trajectory, so it follows
    // the step exactly: no deviation at all. Steps between 1e-4 and 2e-3
    // cycles per sample, mid-segment, so no corner is within the residuals'
    // reach (the pulses, flat between their corners, have nothing to bend).
    const auto& table = std::is_same_v<TestType, MinBlep32> ? minBlepTable32() : minBlepTable64();
    const double delay = table.delay();
    constexpr double unitsPerCycle = 4294967296.0;
    for (const auto shape : {Waveform::saw, Waveform::triangle, Waveform::trisaw, Waveform::sine})
    {
        for (const auto& [c0, c1] : {std::pair{1e-4, 2e-3}, std::pair{2e-3, 1e-4}, std::pair{5e-4, 1.5e-3}})
        {
            CAPTURE(Candidate<TestType>::name, shapeName(shape), c0, c1);
            auto osc = prepared<TestType>(shape);
            osc.restart(static_cast<std::uint32_t>(0.1 * unitsPerCycle));
            const auto i0 = static_cast<std::uint32_t>(c0 * unitsPerCycle);
            const auto i1 = static_cast<std::uint32_t>(c1 * unitsPerCycle);
            osc.setIncrement(i0);
            for (int n = 0; n < 100; ++n)
            {
                static_cast<void>(osc.next());
            }
            osc.setIncrement(i1);
            const auto stepPhase = static_cast<double>(osc.nextPhase());
            // The slope per cycle of the segment the step is in.
            const double s = shape == Waveform::sine ? 0.0 : (idealWave(shape, 0.15) - idealWave(shape, 0.12)) / 0.03;
            const double b = s * (static_cast<double>(i1) - static_cast<double>(i0)) / unitsPerCycle;
            for (std::size_t n = 0; n < Candidate<TestType>::guardSamples; ++n)
            {
                const double x = static_cast<double>(n) - delay; // the delayed time, from the step
                const double phase = stepPhase + ((x < 0.0 ? static_cast<double>(i0) : static_cast<double>(i1)) * x);
                const double ideal = idealWave(shape, phase / unitsPerCycle);
                const double expected = b * (static_cast<double>(table.ramp(0, n)) + std::min(x, 0.0));
                CAPTURE(n);
                REQUIRE_THAT(static_cast<double>(osc.next()) - ideal, WithinAbs(expected, 2e-6));
            }
        }
    }
    // A pitch step and a jump between the same two samples, in either order:
    // the bend is at the next sample, on the segment the jump lands in, so
    // the order makes no difference, bit for bit. (Bending the segment
    // before the jump peaked at 9.0 on the triangle.)
    anoesis::dsp::Random random{5};
    for (const auto shape : periodicShapes)
    {
        auto a = prepared<TestType>(shape);
        auto b = prepared<TestType>(shape);
        for (int event = 0; event < 200; ++event)
        {
            const auto to = static_cast<std::uint32_t>(random.nextUnipolar() * 0.45f * 4294967296.0f);
            const std::uint32_t target = random.nextUint32();
            const float since = random.nextUnipolar();
            a.setIncrement(to);
            a.jumpTo(target, since);
            b.jumpTo(target, since);
            b.setIncrement(to);
            for (std::uint32_t n = 0; n < 1 + (random.nextUint32() % 8U); ++n)
            {
                CAPTURE(Candidate<TestType>::name, shapeName(shape), event, n);
                REQUIRE_THAT(a.next(), WithinULP(b.next(), 0));
            }
        }
    }
    // A step from 0.01 Hz to 0.42 fs, the largest, from any phase: no
    // overshoot beyond the oscillator's own. It measured 5.4 (the
    // triangle) when a step was not band-limited.
    for (const auto shape : periodicShapes)
    {
        for (std::uint32_t k = 0; k < 16; ++k)
        {
            CAPTURE(Candidate<TestType>::name, shapeName(shape), k);
            auto osc = prepared<TestType>(shape);
            osc.restart(k << 28U);
            osc.setFrequency(0.01f);
            for (int n = 0; n < 100; ++n)
            {
                static_cast<void>(osc.next());
            }
            osc.setIncrement(static_cast<std::uint32_t>(0.42 * unitsPerCycle));
            for (int n = 0; n < 200; ++n)
            {
                REQUIRE(std::abs(osc.next()) < Candidate<TestType>::peak);
            }
        }
    }
}
