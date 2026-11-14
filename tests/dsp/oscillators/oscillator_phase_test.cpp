// Tests for the oscillator's phase controls: restarting from silence at any
// start phase, and the band-limited phase jump (hard-sync style) that a
// retrigger makes while the oscillator is sounding. Also the degrees-to-
// fixed-point conversion the slot uses. Measurements:
// oscillator_phase_report.cpp.
// Doc: docs/dsp/oscillator-phase.md

#include "oscillator_support.hpp"

#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>

#include <anoesis/test/measure.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;
using anoesis::dsp::phaseFromDegrees;
using Catch::Matchers::WithinAbs;

namespace
{

constexpr double rate = 48000.0;
constexpr std::uint32_t increment = 1U << 22U; // 2^-10 cycles per sample: 1024 samples per cycle
constexpr std::size_t cycle = 1024;
constexpr double dt = 1.0 / static_cast<double>(cycle);

template <typename Osc>
Osc prepared(Waveform shape, double sampleRate = rate)
{
    auto osc = Candidate<Osc>::make();
    osc.prepare({.sampleRate = static_cast<float>(sampleRate), .maxBlockSize = 512, .numChannels = 1});
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

template <typename Osc>
void append(std::vector<float>& out, Osc& osc, std::size_t numSamples)
{
    for (std::size_t i = 0; i < numSamples; ++i)
    {
        out.push_back(osc.next());
    }
}

std::uint32_t toPhase(double cycles)
{
    return static_cast<std::uint32_t>(std::ldexp(cycles - std::floor(cycles), 32));
}

double cyclesOf(std::uint32_t phase)
{
    return std::ldexp(static_cast<double>(phase), -32);
}

// compareWithIdeal() for this file's pitch and the candidate's guard.
template <typename Osc>
IdealMatch matchIdeal(const std::vector<float>& out, Waveform shape, double start, double origin, std::size_t from,
                      double delay)
{
    return compareWithIdeal(out, shape, start, origin, from, delay, dt,
                            static_cast<double>(Candidate<Osc>::guardSamples));
}

} // namespace

TEST_CASE("Degrees convert to the fixed-point phase, wrapped to one cycle", "[dsp][oscillator][phase]")
{
    CHECK(phaseFromDegrees(0.0f) == 0U);
    CHECK(phaseFromDegrees(90.0f) == (1U << 30U));
    CHECK(phaseFromDegrees(180.0f) == (1U << 31U));
    CHECK(phaseFromDegrees(270.0f) == (3U << 30U));
    CHECK(phaseFromDegrees(45.0f) == (1U << 29U));
    CHECK(phaseFromDegrees(360.0f) == 0U);
    CHECK(phaseFromDegrees(450.0f) == (1U << 30U));
    CHECK(phaseFromDegrees(-90.0f) == (3U << 30U));
    CHECK(phaseFromDegrees(-720.0f) == 0U);
    // One degree is 2^32 / 360 units, to float precision (24 bits).
    CHECK_THAT(cyclesOf(phaseFromDegrees(1.0f)), WithinAbs(1.0 / 360.0, 1e-9));
    CHECK_THAT(cyclesOf(phaseFromDegrees(359.9f)), WithinAbs(359.9 / 360.0, 1e-7));
    // Just below 0 wraps to just below a whole cycle, never past it, or to 0
    // when float cannot tell it from a whole cycle.
    CHECK(phaseFromDegrees(-0.001f) > (3U << 30U));
    CHECK(phaseFromDegrees(-1e-6f) == 0U);
    CHECK(phaseFromDegrees(-1e-30f) == 0U);
    // Not a number, or infinite: phase 0.
    CHECK(phaseFromDegrees(std::numeric_limits<float>::quiet_NaN()) == 0U);
    CHECK(phaseFromDegrees(std::numeric_limits<float>::infinity()) == 0U);
    CHECK(phaseFromDegrees(-std::numeric_limits<float>::infinity()) == 0U);
}

TEMPLATE_TEST_CASE("minBLEP restarts from silence at any start phase", "[dsp][oscillator][phase]", MinBlep32, MinBlep64)
{
    // A band-limited onset at the start phase: the first sample is exactly 0,
    // and from then on the output is the waveform from that phase, delayed by
    // latencySamples(). Starts on a corner land just after it (the square at
    // 1/2 starts low); the corners behind the start are not emitted as edges;
    // a start just before a corner crosses it on the next sample.
    for (const double start : {0.0, 0.1, 0.25, 0.5, 0.618034, 0.75, 0.9999})
    {
        for (const auto shape : periodicShapes)
        {
            CAPTURE(Candidate<TestType>::name, start, shapeName(shape));
            auto osc = prepared<TestType>(shape);
            osc.setIncrement(increment);
            const auto phase = toPhase(start);
            osc.restart(phase);
            const auto out = renderPlain(osc, 3 * cycle);
            CHECK_THAT(out[0], WithinAbs(0.0, 1e-6));
            const auto match =
                matchIdeal<TestType>(out, shape, cyclesOf(phase), 0.0, 0, static_cast<double>(osc.latencySamples()));
            CAPTURE(match.worstAt);
            CHECK(match.worstError < Candidate<TestType>::tolerance);
            // The QPulse leaves the fewest samples outside the guards.
            CHECK(match.checked > out.size() / 2);
        }
    }
}

TEMPLATE_TEST_CASE("minBLEP restart(0) is reset()", "[dsp][oscillator][phase]", MinBlep32, MinBlep64)
{
    for (const auto shape : periodicShapes)
    {
        CAPTURE(shapeName(shape));
        auto osc = prepared<TestType>(shape);
        osc.setFrequency(3456.0f);
        static_cast<void>(renderPlain(osc, 300));
        osc.restart(0);
        const auto restarted = renderPlain(osc, 500);
        osc.reset();
        const auto reset = renderPlain(osc, 500);
        CHECK(restarted == reset);
    }
}

TEMPLATE_TEST_CASE("minBLEP jumps to a new phase mid-note and carries on from there", "[dsp][oscillator][phase]",
                   MinBlep32, MinBlep64)
{
    // The jump happens `since` samples before the sample that follows it.
    // After its residual, the output is the waveform from the new phase:
    // phase `target` at time n0 - since.
    constexpr std::size_t n0 = 1500; // phase 0.46 when the jump comes
    for (const double since : {0.0, 0.375, 0.9})
    {
        for (const double target : {0.0, 0.25, 0.5, 0.75, 0.3})
        {
            for (const auto shape : periodicShapes)
            {
                CAPTURE(Candidate<TestType>::name, since, target, shapeName(shape));
                auto osc = prepared<TestType>(shape);
                osc.setIncrement(increment);
                auto out = renderPlain(osc, n0);
                osc.jumpTo(toPhase(target), static_cast<float>(since));
                append(out, osc, 2 * cycle);
                const auto delay = static_cast<double>(osc.latencySamples());
                const auto match = matchIdeal<TestType>(out, shape, target, static_cast<double>(n0) - since, n0, delay);
                CAPTURE(match.worstAt);
                CHECK(match.worstError < Candidate<TestType>::tolerance);
                CHECK(match.checked > cycle);
                CHECK(std::ranges::all_of(out, [](float x) { return std::abs(x) < Candidate<TestType>::peak; }));
            }
        }
    }
}

TEMPLATE_TEST_CASE("minBLEP: a jump to where the phase already is changes nothing", "[dsp][oscillator][phase]",
                   MinBlep32, MinBlep64)
{
    // The jump keeps the residuals already in the ring and adds only the
    // difference between the old and new waveform, which here is none. So a
    // retrigger never drops a sounding oscillator to silence.
    // (No SECTIONs here: inside loops, Catch2 would run them only on the
    // first iteration.)
    const auto twins = [](Waveform shape, float hz)
    {
        auto plain = prepared<TestType>(shape);
        auto jumped = prepared<TestType>(shape);
        plain.setFrequency(hz);
        jumped.setFrequency(hz);
        const auto a = renderPlain(plain, 777);
        const auto b = renderPlain(jumped, 777);
        REQUIRE(a == b);
        return std::pair{plain, jumped};
    };
    // From 1 kHz up, the waveforms with corners have one at least every 48
    // samples, so the ring holds residuals in flight when the jump comes.
    for (const float hz : {55.0f, 1000.0f, 3000.0f, 12000.0f})
    {
        for (const auto shape : periodicShapes)
        {
            CAPTURE(Candidate<TestType>::name, hz, shapeName(shape));
            {
                // At a sample.
                auto [plain, jumped] = twins(shape, hz);
                jumped.jumpTo(jumped.nextPhase());
                CHECK(renderPlain(jumped, 1000) == renderPlain(plain, 1000));
            }
            {
                // Between samples: the phase the trajectory had 0.4 samples
                // before the next sample. Equal to rounding: the crossings in
                // the step are split at the jump, so their sub-sample times
                // are sums.
                auto [plain, jumped] = twins(shape, hz);
                const auto back = static_cast<std::uint32_t>(0.4 * static_cast<double>(jumped.phaseIncrement()));
                jumped.jumpTo(jumped.nextPhase() - back, 0.4f);
                const auto a = renderPlain(jumped, 1000);
                const auto b = renderPlain(plain, 1000);
                double worst = 0.0;
                for (std::size_t i = 0; i < a.size(); ++i)
                {
                    worst = std::max(worst, static_cast<double>(std::abs(a[i] - b[i])));
                }
                CHECK(worst < 1e-5);
            }
        }
    }
}

TEMPLATE_TEST_CASE("minBLEP: a jump in value or slope is exactly a natural edge of the same size",
                   "[dsp][oscillator][phase]", MinBlep32, MinBlep64)
{
    // Oscillator A is at phase 1/4 when it jumps to 3/4. Oscillator B, with
    // no jump, crosses its corner at 1/2 at the same instant. For the square
    // both are a step of -2 there; for the triangle both are a bend of -8,
    // with A sitting 1 below B (it jumps from 0 to 0 instead of passing 1).
    // Every other edge is more than 64 taps away in the window compared, so
    // the outputs must agree there to rounding.
    constexpr std::size_t n0 = 2048;
    for (const double since : {0.0, 0.375})
    {
        for (const auto shape : {Waveform::pulse, Waveform::triangle})
        {
            CAPTURE(Candidate<TestType>::name, since, shapeName(shape));
            const auto at = [&](double phaseAtJump)
            {
                // The phase at sample 0 that puts the oscillator at
                // `phaseAtJump` at time n0 - since.
                return toPhase(phaseAtJump - ((static_cast<double>(n0) - since) * dt));
            };
            auto a = prepared<TestType>(shape);
            auto b = prepared<TestType>(shape);
            a.setIncrement(increment);
            b.setIncrement(increment);
            a.restart(at(0.25));
            b.restart(at(0.5));
            auto outA = renderPlain(a, n0);
            a.jumpTo(toPhase(0.75), static_cast<float>(since));
            append(outA, a, 300);
            const auto outB = renderPlain(b, n0 + 300);
            const float offset = shape == Waveform::triangle ? 1.0f : 0.0f;
            for (std::size_t n = n0 - 150; n < n0 + 250; ++n)
            {
                CAPTURE(n);
                REQUIRE_THAT(outA[n] + offset, WithinAbs(outB[n], 1e-6));
            }
        }
    }
}

TEMPLATE_TEST_CASE("minBLEP: jumping a silent oscillator starts it, like restart()", "[dsp][oscillator][phase]",
                   MinBlep32, MinBlep64)
{
    for (const auto shape : periodicShapes)
    {
        CAPTURE(shapeName(shape));
        auto jumped = prepared<TestType>(shape);
        auto restarted = prepared<TestType>(shape);
        jumped.setFrequency(777.0f);
        restarted.setFrequency(777.0f);
        CHECK_FALSE(jumped.isRunning());
        jumped.jumpTo(toPhase(0.3));
        restarted.restart(toPhase(0.3));
        CHECK(renderPlain(jumped, 500) == renderPlain(restarted, 500));
        CHECK(jumped.isRunning());

        // Between samples, from silence: the onset is at the next sample, at
        // the phase the trajectory reaches there.
        jumped.reset();
        restarted.reset();
        jumped.jumpTo(toPhase(0.3), 0.5f);
        restarted.restart(toPhase(0.3) + (jumped.phaseIncrement() / 2));
        CHECK(renderPlain(jumped, 500) == renderPlain(restarted, 500));
    }
}

TEMPLATE_TEST_CASE("minBLEP: reset() after jumps restarts identically", "[dsp][oscillator][phase]", MinBlep32,
                   MinBlep64)
{
    for (const auto shape : periodicShapes)
    {
        CAPTURE(shapeName(shape));
        auto osc = prepared<TestType>(shape);
        osc.setFrequency(2345.0f);
        const auto fresh = renderPlain(osc, 1000);
        osc.jumpTo(toPhase(0.6), 0.25f);
        static_cast<void>(renderPlain(osc, 10));
        osc.jumpTo(toPhase(0.1));
        osc.reset();
        CHECK(renderPlain(osc, 1000) == fresh);
    }
}

TEMPLATE_TEST_CASE("minBLEP stays finite and bounded under jumps at any time, from 0.01 Hz to Nyquist",
                   "[dsp][oscillator][phase]", MinBlep32, MinBlep64)
{
    // Jumps to random phases at random sub-sample times, every 1 to 16
    // samples: far denser than any retrigger or sync, so residuals pile up.
    anoesis::dsp::Random random{42};
    for (const double hz : {0.0, 0.01, 20.0, 1000.0, 12000.0, 21000.0, rate / 2.0})
    {
        for (const auto shape : periodicShapes)
        {
            CAPTURE(Candidate<TestType>::name, hz, shapeName(shape));
            auto osc = prepared<TestType>(shape);
            osc.setFrequency(static_cast<float>(hz));
            std::vector<float> out;
            for (int jumps = 0; jumps < 300; ++jumps)
            {
                osc.jumpTo(random.nextUint32(), random.nextUnipolar());
                append(out, osc, 1 + (random.nextUint32() % 16U));
            }
            // Measured at 64 taps: at most 2.61 (the square at 21 kHz),
            // the sine 1.92. A first version, which corrected the sine's
            // bend as if it were a straight segment, reached 21 here.
            CHECK(std::ranges::all_of(out, [](float x) { return std::isfinite(x) && std::abs(x) < 3.0f; }));
            CHECK(std::ranges::none_of(out, [](float x) { return std::fpclassify(x) == FP_SUBNORMAL; }));
        }
    }
}

TEMPLATE_TEST_CASE("minBLEP band-limits a retrigger: hard sync aliases far less than a naive phase jump",
                   "[dsp][oscillator][phase]", MinBlep32, MinBlep64)
{
    // A note-on every master period is hard sync. The jumps fall between
    // samples (the period is N / c samples), so the result repeats every N
    // samples and the aliasing of each jump lands between the harmonics of
    // c fs / N, where measureAliasing finds it. The naive jump sets the phase
    // and evaluates the ideal waveform, with no residual.
    constexpr std::size_t n = 65536;
    constexpr std::uint64_t masterCycles = 601; // 440.1 Hz at 48 kHz
    const double f0 = static_cast<double>(masterCycles) * rate / static_cast<double>(n);
    // 1628 Hz, 3.7 cycles per master period, so each jump is from phase 0.7
    // back to 0: a jump in value for the saw and the square, and a bend of
    // +8 (from falling to rising) for the triangle.
    const auto slaveIncrement = static_cast<std::uint32_t>(std::ldexp(f0 * 3.7 / rate, 32));
    const auto target = toPhase(0.0);
    for (const auto shape : {Waveform::saw, Waveform::pulse, Waveform::triangle})
    {
        CAPTURE(Candidate<TestType>::name, shapeName(shape));
        auto osc = prepared<TestType>(shape);
        osc.setIncrement(slaveIncrement);
        const auto banded = renderSynced(osc, target, n, masterCycles, 8192);
        const auto naive = renderSyncedNaive(shape, slaveIncrement, target, n, masterCycles, 8192);
        const auto window = rectangularWindow(n);
        const auto worst = [&](const std::vector<float>& out)
        {
            const auto alias = measureAliasing(out, f0, rate, window, 20000.0);
            return worstAliasReSawFundamental(alias.worstAliasDb, powerSpectrum(out, rate, window).powerAround(f0, 0));
        };
        const auto bandedDb = worst(banded);
        const auto naiveDb = worst(naive);
        CAPTURE(bandedDb, naiveDb);
        CHECK(bandedDb < -80.0);
        CHECK(naiveDb > bandedDb + 30.0);
    }
}
