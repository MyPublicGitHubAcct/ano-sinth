// Tests for Lfo (libs/dsp/include/anoesis/dsp/control/lfo.hpp): the LFO
// chosen in docs/dsp/lfos.md. A 32.32 fixed-point phase; 11 waveforms as
// data; piecewise-linear rows exact per sample, curved rows (sine, smoothed
// random, Lorenz) interpolated linearly on a 32-sample grid counted from the
// last (re)start; every jump slewed linearly over 1 ms, at most 1/16 cycle;
// output = gain x (wave + offset), gain and offset ramped over 10 ms.
//
// References are computed in double from the exact phase (the 64-bit
// increment the LFO reports). Tolerances are derived next to each use from
// float rounding and, for the curved rows, the interpolation's own error.

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/core/processor.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/waveform.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

using anoesis::dsp::Lfo;
using anoesis::dsp::LfoEvent;
using anoesis::dsp::LfoKind;
using anoesis::dsp::lfoShape;
using anoesis::dsp::LfoTrigger;
using anoesis::dsp::LfoWaveform;
using Catch::Matchers::WithinAbs;
namespace dsp = anoesis::dsp;

namespace
{

constexpr std::array<float, 4> sampleRates{44100.0f, 48000.0f, 96000.0f, 192000.0f};
constexpr double u = 0x1p-24; // float's unit roundoff

constexpr std::array allWaveforms{
    LfoWaveform::sine,   LfoWaveform::saw,        LfoWaveform::ramp,   LfoWaveform::triangle,
    LfoWaveform::pulse,  LfoWaveform::hpulse,     LfoWaveform::qpulse, LfoWaveform::trisaw,
    LfoWaveform::lorenz, LfoWaveform::sampleHold, LfoWaveform::smooth,
};

constexpr std::array linearWaveforms{
    LfoWaveform::saw,    LfoWaveform::ramp,   LfoWaveform::triangle, LfoWaveform::pulse,
    LfoWaveform::hpulse, LfoWaveform::qpulse, LfoWaveform::trisaw,
};

Lfo prepared(float rate, LfoWaveform waveform, float hz, std::uint64_t seed = 1)
{
    Lfo lfo{seed};
    lfo.setWaveform(waveform);
    lfo.setFrequency(hz);
    lfo.prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 1});
    return lfo;
}

std::vector<float> render(Lfo& lfo, std::size_t n)
{
    std::vector<float> y(n);
    lfo.process(y);
    return y;
}

std::vector<float> renderNext(Lfo& lfo, std::size_t n)
{
    std::vector<float> y(n);
    for (auto& v : y)
    {
        v = lfo.next();
    }
    return y;
}

bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool bitIdentical(std::span<const float> a, std::span<const float> b)
{
    return a.size() == b.size() && std::ranges::equal(a, b, sameBits);
}

// The phase in cycles, [0, 1), of sample k after a (re)start at `start`
// (2^32 per cycle) with the 64-bit increment `increment` (2^64 per cycle).
double cyclesAt(std::uint32_t start, std::uint64_t increment, std::uint64_t k)
{
    const std::uint64_t phase = (static_cast<std::uint64_t>(start) << 32U) + (k * increment);
    return std::ldexp(static_cast<double>(phase), -64);
}

// The ideal value of a periodic row at `cycles` in [0, 1), in double, from
// its definition (not from the table under test). A corner exactly at the
// phase belongs to the segment after it.
double ideal(LfoWaveform waveform, double t)
{
    const auto pulse = [t](double width)
    {
        return t < width ? 1.0 : -1.0;
    };
    switch (waveform)
    {
    case LfoWaveform::sine:
        return std::sin(2.0 * std::numbers::pi * t);
    case LfoWaveform::saw:
        return -1.0 + (2.0 * t);
    case LfoWaveform::ramp:
        return 1.0 - (2.0 * t);
    case LfoWaveform::triangle:
        return t < 0.5 ? -1.0 + (4.0 * t) : 3.0 - (4.0 * t);
    case LfoWaveform::pulse:
        return pulse(0.5);
    case LfoWaveform::hpulse:
        return pulse(0.25);
    case LfoWaveform::qpulse:
        return pulse(0.125);
    case LfoWaveform::trisaw:
        return t < 0.5 ? -1.0 + (3.0 * t) : 1.0 - t;
    case LfoWaveform::lorenz:
    case LfoWaveform::sampleHold:
    case LfoWaveform::smooth:
        break;
    }
    return 0.0;
}

// The steepest slope of a periodic row, per cycle.
double steepest(LfoWaveform waveform)
{
    switch (waveform)
    {
    case LfoWaveform::sine:
        return 2.0 * std::numbers::pi;
    case LfoWaveform::saw:
    case LfoWaveform::ramp:
        return 2.0;
    case LfoWaveform::triangle:
        return 4.0;
    case LfoWaveform::trisaw:
        return 3.0;
    default:
        return 0.0;
    }
}

// A linear row's value is corner.value + slope x t, with t the phase's top
// 32 bits since the corner converted to float (an error up to 2^-25 of a
// cycle, relative 2^-24) and the 32 bits below dropped (2^-32): then the
// product and the sum each round once (|slope| t u and u).
double linearTolerance(LfoWaveform waveform)
{
    const double s = steepest(waveform);
    return (s * (u + std::ldexp(1.0, -32))) + (s * u) + (2.0 * u);
}

// Linear interpolation of a curve with |second derivative| <= c per cycle^2
// between points `spacing` cycles apart errs by at most c spacing^2 / 8;
// plus sin's own float error (1 ulp at most, 2u), and the interpolation's
// arithmetic (slope rounding times up to 32 samples, the product, the sum).
double interpolationTolerance(double curvature, double spacing)
{
    return (curvature * spacing * spacing / 8.0) + (2.0 * u) + (36.0 * u);
}

std::size_t expectedSlew(float rate, float hz)
{
    const double samples = std::min(0.001 * static_cast<double>(rate),
                                    hz > 0.0f ? static_cast<double>(rate) / (16.0 * static_cast<double>(hz)) : 1e30);
    return std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(samples)));
}

double meanOf(std::span<const float> x)
{
    double sum = 0.0;
    for (const float v : x)
    {
        sum += static_cast<double>(v);
    }
    return sum / static_cast<double>(x.size());
}

double correlation(std::span<const float> a, std::span<const float> b)
{
    const double ma = meanOf(a);
    const double mb = meanOf(b);
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (std::size_t n = 0; n < a.size(); ++n)
    {
        const double x = static_cast<double>(a[n]) - ma;
        const double y = static_cast<double>(b[n]) - mb;
        ab += x * y;
        aa += x * x;
        bb += y * y;
    }
    return ab / std::sqrt(aa * bb);
}

// One midpoint step of the Lorenz system with every product and sum rounded
// on its own, through a volatile store: what a compiler that never fuses a
// multiply into an add computes. The LFO's step must match it bit for bit,
// so that every target renders a Lorenz patch identically.
void unfusedLorenzStep(dsp::LorenzState& s, float dt)
{
    const auto r = [](float v)
    {
        const volatile float stored = v;
        return static_cast<float>(stored);
    };
    constexpr float sigma = 10.0f;
    constexpr float rho = 28.0f;
    constexpr float beta = 8.0f / 3.0f;
    const float h = r(0.5f * dt);
    const float dx1 = r(sigma * r(s.y - s.x));
    const float dy1 = r(r(s.x * r(rho - s.z)) - s.y);
    const float dz1 = r(r(s.x * s.y) - r(beta * s.z));
    const float xm = r(s.x + r(h * dx1));
    const float ym = r(s.y + r(h * dy1));
    const float zm = r(s.z + r(h * dz1));
    const float dx2 = r(sigma * r(ym - xm));
    const float dy2 = r(r(xm * r(rho - zm)) - ym);
    const float dz2 = r(r(xm * ym) - r(beta * zm));
    s.x = r(s.x + r(dt * dx2));
    s.y = r(s.y + r(dt * dy2));
    s.z = r(s.z + r(dt * dz2));
}

} // namespace

TEST_CASE("LFO: follows the Preparable lifecycle", "[dsp][lfo]")
{
    STATIC_CHECK(dsp::Preparable<Lfo>);
    for (const auto waveform : allWaveforms)
    {
        INFO("row " << static_cast<int>(waveform));
        const auto play = [](Lfo& lfo)
        {
            auto y = render(lfo, 3000);
            lfo.retrigger();
            const auto tail = render(lfo, 3000);
            y.insert(y.end(), tail.begin(), tail.end());
            return y;
        };
        auto fresh = prepared(48000.0f, waveform, 37.0f, 9);
        const auto expected = play(fresh);

        auto used = prepared(48000.0f, waveform, 37.0f, 9);
        static_cast<void>(render(used, 1234));
        used.retrigger();
        static_cast<void>(render(used, 77));
        used.reset();
        CHECK(bitIdentical(play(used), expected));

        auto rePrepared = prepared(48000.0f, waveform, 37.0f, 9);
        static_cast<void>(render(rePrepared, 1234));
        rePrepared.prepare({.sampleRate = 48000.0f, .maxBlockSize = 4096, .numChannels = 1});
        CHECK(bitIdentical(play(rePrepared), expected));
    }
}

TEST_CASE("LFO: defaults are a 1 Hz sine at gain 1 and offset 0, starting at its peak", "[dsp][lfo]")
{
    Lfo lfo;
    lfo.prepare({.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 1});
    CHECK(lfo.waveform() == LfoWaveform::sine);
    CHECK_THAT(lfo.frequency(), WithinAbs(1.0, 0.0));
    CHECK_THAT(lfo.gain(), WithinAbs(1.0, 0.0));
    CHECK_THAT(lfo.offset(), WithinAbs(0.0, 0.0));
    CHECK_THAT(lfo.next(), WithinAbs(1.0, 2.0 * u));
}

TEST_CASE("LFO: the table reuses the oscillator's rows, and every row starts at its start value", "[dsp][lfo]")
{
    STATIC_CHECK(dsp::lfoWaveformCount == 11);
    const auto same = [](const dsp::WaveShape& a, const dsp::WaveShape& b)
    {
        for (std::uint32_t k = 0; k < 4096; ++k)
        {
            const std::uint32_t phase = k * 1048576U;
            if (!sameBits(a.valueAt(phase), b.valueAt(phase)))
            {
                return false;
            }
        }
        return true;
    };
    using dsp::Waveform;
    using dsp::waveShape;
    CHECK(same(lfoShape(LfoWaveform::sine).wave, waveShape(Waveform::sine)));
    CHECK(same(lfoShape(LfoWaveform::saw).wave, waveShape(Waveform::saw)));
    CHECK(same(lfoShape(LfoWaveform::triangle).wave, waveShape(Waveform::triangle)));
    CHECK(same(lfoShape(LfoWaveform::pulse).wave, waveShape(Waveform::pulse)));
    CHECK(same(lfoShape(LfoWaveform::hpulse).wave, waveShape(Waveform::hpulse)));
    CHECK(same(lfoShape(LfoWaveform::qpulse).wave, waveShape(Waveform::qpulse)));
    CHECK(same(lfoShape(LfoWaveform::trisaw).wave, waveShape(Waveform::trisaw)));
    // The Ramp is the saw falling: +1 at the start to -1.
    for (std::uint32_t k = 1; k < 4096; ++k)
    {
        const std::uint32_t phase = k * 1048576U;
        CHECK_THAT(lfoShape(LfoWaveform::ramp).wave.valueAt(phase),
                   WithinAbs(-static_cast<double>(waveShape(Waveform::saw).valueAt(phase)), 4.0 * u));
    }
    CHECK(lfoShape(LfoWaveform::lorenz).kind == LfoKind::lorenz);
    CHECK(lfoShape(LfoWaveform::sampleHold).kind == LfoKind::sampleHold);
    CHECK(lfoShape(LfoWaveform::smooth).kind == LfoKind::smoothRandom);
    CHECK(lfoShape(LfoWaveform::sine).isCurved());
    CHECK(lfoShape(LfoWaveform::lorenz).isCurved());
    CHECK(lfoShape(LfoWaveform::smooth).isCurved());
    CHECK_FALSE(lfoShape(LfoWaveform::saw).isCurved());
    CHECK_FALSE(lfoShape(LfoWaveform::sampleHold).isCurved());

    // The start phases (inventory L5): the sine and triangle at their peak,
    // the ramp at its top, the saw and TriSaw at their bottom, the pulses high.
    const std::array starts{
        std::pair{LfoWaveform::sine, 1.0f},   std::pair{LfoWaveform::saw, -1.0f},
        std::pair{LfoWaveform::ramp, 1.0f},   std::pair{LfoWaveform::triangle, 1.0f},
        std::pair{LfoWaveform::pulse, 1.0f},  std::pair{LfoWaveform::hpulse, 1.0f},
        std::pair{LfoWaveform::qpulse, 1.0f}, std::pair{LfoWaveform::trisaw, -1.0f},
    };
    for (const auto& [waveform, start] : starts)
    {
        INFO("row " << static_cast<int>(waveform));
        auto lfo = prepared(48000.0f, waveform, 3.0f);
        CHECK_THAT(lfo.next(), WithinAbs(start, 2.0 * u));
        // A note from silence on a sounding LFO starts there too, at once.
        static_cast<void>(render(lfo, 5555));
        lfo.restart();
        CHECK_THAT(lfo.next(), WithinAbs(start, 2.0 * u));
    }
}

TEST_CASE("LFO: the piecewise-linear rows are exact at every sample", "[dsp][lfo]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    for (const auto waveform : linearWaveforms)
    {
        for (const float hz : {0.01f, 1.0f, 7.3f, 333.0f})
        {
            INFO("rate " << rate << ", row " << static_cast<int>(waveform) << ", " << hz << " Hz");
            auto lfo = prepared(rate, waveform, hz);
            const auto increment = lfo.phaseIncrement();
            const auto start = lfoShape(waveform).start;
            const auto y = render(lfo, 20000);
            const double tolerance = linearTolerance(waveform);
            const std::size_t slew = expectedSlew(rate, hz);
            double worst = 0.0;
            std::size_t sinceJump = slew;
            for (std::size_t k = 0; k < y.size(); ++k)
            {
                // Skip the samples a slew covers after each jump.
                const double now = cyclesAt(start, increment, k);
                const double before = k == 0 ? now : cyclesAt(start, increment, k - 1);
                const double expected = ideal(waveform, now);
                const double continued = ideal(waveform, before);
                const bool jumped =
                    k > 0 && std::abs(expected - continued) >
                                 (steepest(waveform) * std::ldexp(static_cast<double>(increment), -64)) + 1e-6;
                sinceJump = jumped ? 0 : sinceJump + 1;
                if (sinceJump >= slew)
                {
                    worst = std::max(worst, std::abs(static_cast<double>(y[k]) - expected));
                }
            }
            CHECK(worst <= tolerance);
        }
    }
}

TEST_CASE("LFO: the curved rows are interpolated on a 32-sample grid, within the interpolation's bound", "[dsp][lfo]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    for (const float hz : {0.01f, 1.0f, 5.0f, 40.0f, 400.0f})
    {
        INFO("rate " << rate << ", " << hz << " Hz");
        auto lfo = prepared(rate, LfoWaveform::sine, hz);
        const auto increment = lfo.phaseIncrement();
        const auto y = render(lfo, 40000);
        const double spacing = 32.0 * std::ldexp(static_cast<double>(increment), -64);
        const double curvature = 4.0 * std::numbers::pi * std::numbers::pi;
        const double tolerance = interpolationTolerance(curvature, spacing);
        double worst = 0.0;
        double worstAtGrid = 0.0;
        for (std::size_t k = 0; k < y.size(); ++k)
        {
            const double error = std::abs(static_cast<double>(y[k]) -
                                          ideal(LfoWaveform::sine, cyclesAt(dsp::quarterCycle, increment, k)));
            worst = std::max(worst, error);
            if (k % Lfo::gridSamples == 0)
            {
                worstAtGrid = std::max(worstAtGrid, error);
            }
        }
        CHECK(worst <= tolerance);
        // On the grid there is no interpolation: sin's own error (2u), and
        // its argument 2 pi t rounded three times (t, 2 pi, the product), each
        // at most u relative, times the slope of at most 1.
        CHECK(worstAtGrid <= (2.0 * u) + (6.0 * std::numbers::pi * u));
    }
}

TEST_CASE("LFO: the phase is exact: the frequency to double precision, and nothing accumulates", "[dsp][lfo]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    for (const float hz : {0.01f, 1.0f, 100.0f, 2000.0f})
    {
        INFO("rate " << rate << ", " << hz << " Hz");
        auto lfo = prepared(rate, LfoWaveform::saw, hz);
        // The increment is the nearest 2^-64 of a cycle to hz / rate as
        // double computes it (rounded once, 2^-53 relative).
        const double exact = static_cast<double>(hz) / static_cast<double>(rate);
        const double actual = std::ldexp(static_cast<double>(lfo.phaseIncrement()), -64);
        CHECK(std::abs(actual - exact) <= std::ldexp(1.0, -65) + (exact * 0x1p-52));

        // After any number of samples the phase is the start plus that many
        // increments, exactly (in blocks of 4096, then one at a time).
        const std::uint64_t n = std::uint64_t{4096} * 300U;
        for (std::uint64_t k = 0; k < 300; ++k)
        {
            static_cast<void>(render(lfo, 4096));
        }
        CHECK(lfo.phase() == (n * lfo.phaseIncrement()));
        static_cast<void>(lfo.next());
        CHECK(lfo.phase() == ((n + 1) * lfo.phaseIncrement()));
    }
}

TEST_CASE("LFO: a jump is slewed linearly over 1 ms, at most 1/16 of a cycle", "[dsp][lfo]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    for (const float hz : {1.0f, 20.0f, 1000.0f, 3000.0f})
    {
        INFO("rate " << rate << ", " << hz << " Hz");
        auto lfo = prepared(rate, LfoWaveform::saw, hz);
        const std::size_t slew = expectedSlew(rate, hz);
        CHECK(lfo.slewSamples() == slew);

        // The saw's first edge comes on the first sample past a whole cycle.
        const auto increment = lfo.phaseIncrement();
        const auto y =
            render(lfo, static_cast<std::size_t>(static_cast<double>(rate) / static_cast<double>(hz)) + (4 * slew) + 8);
        REQUIRE(increment > 0);
        std::size_t edge = 1;
        while (cyclesAt(0, increment, edge) > cyclesAt(0, increment, edge - 1))
        {
            ++edge;
        }
        // Each sample of the slew is the saw plus the share left of the jump
        // from the last sample before the edge to the saw at the edge.
        const double jump = static_cast<double>(y[edge - 1]) - ideal(LfoWaveform::saw, cyclesAt(0, increment, edge));
        CHECK(jump > 1.5); // about 2, less the slope of one sample
        const double tolerance = (2.0 * linearTolerance(LfoWaveform::saw)) + (8.0 * u);
        for (std::size_t k = 0; k <= slew; ++k)
        {
            const double expected = ideal(LfoWaveform::saw, cyclesAt(0, increment, edge + k)) +
                                    (jump * static_cast<double>(slew - k) / static_cast<double>(slew));
            CHECK_THAT(y[edge + k], WithinAbs(expected, tolerance));
        }
    }
}

TEST_CASE("LFO: a retrigger restarts the cycle with the jump slewed; a restart starts at once", "[dsp][lfo]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    for (const auto waveform :
         {LfoWaveform::sine, LfoWaveform::saw, LfoWaveform::triangle, LfoWaveform::pulse, LfoWaveform::trisaw})
    {
        INFO("rate " << rate << ", row " << static_cast<int>(waveform));
        const float hz = 2.0f;
        const std::size_t slew = expectedSlew(rate, hz);
        const auto at = static_cast<std::size_t>(0.3f * rate);

        auto retriggered = prepared(rate, waveform, hz);
        auto y = render(retriggered, at);
        const float before = y.back();
        retriggered.retrigger();
        const auto after = render(retriggered, 4 * slew);

        auto fresh = prepared(rate, waveform, hz);
        const auto reference = render(fresh, 4 * slew);

        // The first sample after it continues from where the LFO was (one
        // sample's motion at most), then a straight line to the restarted
        // cycle over the slew, and from then on exactly the restarted LFO.
        const double step = steepest(waveform) * static_cast<double>(hz) / static_cast<double>(rate);
        CHECK(std::abs(static_cast<double>(after[0]) - static_cast<double>(before)) <= step + (4.0 * u));
        const double jump = static_cast<double>(before) - static_cast<double>(reference[0]);
        CHECK(std::abs(jump) > 1.0); // 0.6 of a cycle from the start, every row is far from it
        for (std::size_t k = 0; k < slew; ++k)
        {
            const double share = jump * static_cast<double>(slew - k) / static_cast<double>(slew);
            CHECK_THAT(after[k], WithinAbs(static_cast<double>(reference[k]) + share, 8.0 * u));
        }
        CHECK(bitIdentical(std::span{after}.subspan(slew), std::span{reference}.subspan(slew)));

        // restart(): the restarted cycle at once, with no slew.
        auto restarted = prepared(rate, waveform, hz);
        static_cast<void>(render(restarted, at));
        restarted.restart();
        CHECK(bitIdentical(render(restarted, 4 * slew), reference));
    }
}

TEST_CASE("LFO: retriggers and restarts are sample-accurate within a block", "[dsp][lfo]")
{
    for (const auto trigger : {LfoTrigger::retrigger, LfoTrigger::restart})
    {
        for (const std::size_t offset : {std::size_t{0}, std::size_t{1}, std::size_t{200}, std::size_t{511}})
        {
            INFO("offset " << offset);
            auto blocks = prepared(48000.0f, LfoWaveform::saw, 31.0f);
            static_cast<void>(render(blocks, 1000));
            std::vector<float> y(512);
            const std::array events{LfoEvent{.offset = offset, .trigger = trigger}};
            blocks.process(y, events);

            auto samples = prepared(48000.0f, LfoWaveform::saw, 31.0f);
            static_cast<void>(render(samples, 1000));
            std::vector<float> expected(512);
            for (std::size_t n = 0; n < 512; ++n)
            {
                if (n == offset)
                {
                    samples.apply(trigger);
                }
                expected[n] = samples.next();
            }
            CHECK(bitIdentical(y, expected));
        }
    }
}

TEST_CASE("LFO: a rate change mid-cycle is phase continuous", "[dsp][lfo]")
{
    for (const auto waveform : {LfoWaveform::sine, LfoWaveform::triangle, LfoWaveform::saw, LfoWaveform::smooth})
    {
        INFO("row " << static_cast<int>(waveform));
        auto lfo = prepared(48000.0f, waveform, 2.0f);
        auto y = render(lfo, 7000);
        lfo.setFrequency(7.0f);
        const auto tail = render(lfo, 200);
        y.insert(y.end(), tail.begin(), tail.end());
        // No step larger than the faster rate's steepest slope per sample
        // (the smooth random's is 1.5 x its span of at most 2 per cycle).
        const double slope = (waveform == LfoWaveform::smooth ? 3.0 : steepest(waveform)) * 7.0 / 48000.0;
        double worst = 0.0;
        for (std::size_t n = 6900; n < 7150; ++n)
        {
            worst = std::max(worst, static_cast<double>(std::abs(y[n] - y[n - 1])));
        }
        CHECK(worst <= (slope * 1.001) + (8.0 * u));
        // ... and the periodic rows do move (the smoothed random may be
        // momentarily flat).
        if (waveform != LfoWaveform::smooth)
        {
            CHECK(worst > slope * 2.0 / 7.0 * 0.5);
        }
    }
}

TEST_CASE("LFO: output is gain x (wave + offset), and gain and offset changes ramp over 10 ms", "[dsp][lfo]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    INFO("rate " << rate);
    for (const auto waveform : {LfoWaveform::sine, LfoWaveform::saw, LfoWaveform::sampleHold})
    {
        auto unit = prepared(rate, waveform, 3.0f);
        const auto wave = render(unit, 5000);
        for (const auto& [g, o] : {
                 std::pair{0.0f, 1.0f},
                 std::pair{0.5f, 0.0f},
                 std::pair{0.333f, 1.0f},
                 std::pair{0.333f, -1.0f},
                 std::pair{1.0f, -0.25f},
             })
        {
            Lfo lfo{1};
            lfo.setWaveform(waveform);
            lfo.setFrequency(3.0f);
            lfo.setGain(g);
            lfo.setOffset(o);
            lfo.prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 1});
            const auto y = render(lfo, 5000);
            for (std::size_t n = 0; n < y.size(); ++n)
            {
                if (!sameBits(y[n], g * (wave[n] + o)))
                {
                    FAIL_CHECK("sample " << n << ": " << y[n] << " against " << g * (wave[n] + o));
                    break;
                }
            }
        }
        // Gain 0 is silence whatever the offset; offset +-1 is one-sided.
        CHECK(std::ranges::all_of(wave, [](float v) { return v >= -1.0f && v <= 1.0f; }));
    }

    // A change ramps linearly to the new value over round(10 ms x rate)
    // samples, and lands exactly.
    Lfo lfo{1};
    lfo.setWaveform(LfoWaveform::pulse);
    lfo.setFrequency(0.01f); // high throughout
    lfo.prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 1});
    static_cast<void>(render(lfo, 10));
    lfo.setGain(0.5f);
    lfo.setOffset(-0.5f);
    const auto ramp = static_cast<std::size_t>(std::lround(0.01 * static_cast<double>(rate)));
    const auto y = render(lfo, ramp + 10);
    for (std::size_t k = 0; k < ramp; ++k)
    {
        const double share = static_cast<double>(k + 1) / static_cast<double>(ramp);
        const double g = 1.0 - (0.5 * share);
        const double o = -0.5 * share;
        CHECK_THAT(y[k], WithinAbs(g * (1.0 + o), 16.0 * u));
    }
    for (std::size_t k = ramp; k < y.size(); ++k)
    {
        CHECK(sameBits(y[k], 0.5f * (1.0f + -0.5f)));
    }
}

TEST_CASE("LFO: controls clamp, and ignore NaN", "[dsp][lfo]")
{
    Lfo lfo;
    lfo.prepare({.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 1});
    lfo.setGain(2.0f);
    CHECK_THAT(lfo.gain(), WithinAbs(1.0, 0.0));
    lfo.setGain(-1.0f);
    CHECK_THAT(lfo.gain(), WithinAbs(0.0, 0.0));
    lfo.setGain(std::numeric_limits<float>::quiet_NaN());
    CHECK_THAT(lfo.gain(), WithinAbs(0.0, 0.0));
    lfo.setOffset(3.0f);
    CHECK_THAT(lfo.offset(), WithinAbs(1.0, 0.0));
    lfo.setOffset(-3.0f);
    CHECK_THAT(lfo.offset(), WithinAbs(-1.0, 0.0));
    lfo.setOffset(std::numeric_limits<float>::quiet_NaN());
    CHECK_THAT(lfo.offset(), WithinAbs(-1.0, 0.0));
    // Below 2^-24 a gain or an offset is 0, so no output can be subnormal.
    lfo.setGain(1e-30f);
    CHECK(sameBits(lfo.gain(), 0.0f));
    lfo.setOffset(-1e-30f);
    CHECK(sameBits(lfo.offset(), 0.0f));
    lfo.setFrequency(5.0f);
    lfo.setFrequency(std::numeric_limits<float>::quiet_NaN());
    CHECK_THAT(lfo.frequency(), WithinAbs(5.0, 0.0));
    lfo.setFrequency(-3.0f);
    CHECK(lfo.phaseIncrement() == 0);
    lfo.setFrequency(1e9f);
    CHECK(lfo.phaseIncrement() == (std::uint64_t{1} << 63U)); // Nyquist
    lfo.setWaveform(static_cast<LfoWaveform>(200));
    CHECK(lfo.waveform() == LfoWaveform::sine);
    for (int i = 0; i < 1000; ++i)
    {
        CHECK(std::isfinite(lfo.next()));
    }
}

TEST_CASE("LFO: sample and hold draws one uniform value per cycle, held, with its steps slewed", "[dsp][lfo]")
{
    constexpr float rate = 48000.0f;
    constexpr float hz = 100.0f;
    auto lfo = prepared(rate, LfoWaveform::sampleHold, hz, 7);
    const std::size_t cycles = 20000;
    const auto y = render(lfo, (cycles + 1) * 480);
    const std::size_t slew = expectedSlew(rate, hz);
    // Each cycle starts on the first sample whose 64-bit phase has wrapped.
    REQUIRE(lfo.phaseIncrement() > 0);
    std::vector<std::size_t> starts{0};
    std::uint64_t phase = 0;
    for (std::size_t n = 1; starts.size() <= cycles; ++n)
    {
        const std::uint64_t next = phase + lfo.phaseIncrement();
        if (next < phase)
        {
            starts.push_back(n);
        }
        phase = next;
    }
    std::vector<float> held;
    for (std::size_t c = 0; c < cycles; ++c)
    {
        // Constant from the end of the slew to the end of the cycle.
        const std::size_t from = starts[c] + (c == 0 ? 0 : slew);
        const auto value = y[from];
        bool constant = true;
        for (std::size_t n = from; n < starts[c + 1]; ++n)
        {
            constant = constant && sameBits(y[n], value);
        }
        if (!constant)
        {
            FAIL_CHECK("cycle " << c << " is not held");
            break;
        }
        held.push_back(value);
        // The step into the cycle is a straight line over the slew.
        if (c > 0)
        {
            const auto previous = static_cast<double>(held[c - 1]);
            for (std::size_t k = 0; k < slew; ++k)
            {
                const double expected =
                    static_cast<double>(value) + ((previous - static_cast<double>(value)) *
                                                  static_cast<double>(slew - k) / static_cast<double>(slew));
                if (std::abs(static_cast<double>(y[starts[c] + k]) - expected) > 8.0 * u)
                {
                    FAIL_CHECK("cycle " << c << ", slew sample " << k);
                    break;
                }
            }
        }
    }
    // Uniform on (-1, 1): mean 0 and variance 1/3, each within 5 standard
    // errors (sigma / sqrt(n); the variance's is sqrt(4/45 / n)).
    const auto n = static_cast<double>(held.size());
    const double mean = meanOf(held);
    double variance = 0.0;
    for (const float v : held)
    {
        variance += (static_cast<double>(v) - mean) * (static_cast<double>(v) - mean);
    }
    variance /= n;
    CHECK(std::abs(mean) <= 5.0 * std::sqrt(1.0 / 3.0 / n));
    CHECK(std::abs(variance - (1.0 / 3.0)) <= 5.0 * std::sqrt(4.0 / 45.0 / n));
    CHECK(std::ranges::all_of(held, [](float v) { return v > -1.0f && v < 1.0f; }));
    // Independent from cycle to cycle: |r| within 5 / sqrt(n).
    const std::span<const float> h{held};
    CHECK(std::abs(correlation(h.first(h.size() - 1), h.subspan(1))) <= 5.0 / std::sqrt(n));

    // A retrigger and a restart draw a new value.
    auto again = prepared(rate, LfoWaveform::sampleHold, hz, 7);
    static_cast<void>(render(again, 100));
    const float first = again.next();
    again.restart();
    CHECK_FALSE(sameBits(again.next(), first));
}

TEST_CASE("LFO: the smoothed random row is continuous, bounded, and moves", "[dsp][lfo]")
{
    constexpr float rate = 48000.0f;
    for (const float hz : {1.0f, 30.0f})
    {
        INFO(hz << " Hz");
        auto lfo = prepared(rate, LfoWaveform::smooth, hz, 3);
        const auto y = render(lfo, 480000);
        // Smoothstep between values at most 2 apart has a slope of at most
        // 3 per cycle, so no step between samples exceeds 3 hz / rate (plus
        // the interpolation's rounding).
        double worst = 0.0;
        for (std::size_t n = 1; n < y.size(); ++n)
        {
            worst = std::max(worst, static_cast<double>(std::abs(y[n] - y[n - 1])));
        }
        CHECK(worst <= (3.0 * static_cast<double>(hz) / static_cast<double>(rate) * 1.001) + (8.0 * u));
        CHECK(std::ranges::all_of(y, [](float v) { return v >= -1.0f && v <= 1.0f; }));
        // It moves: ten or more independent uniform draws all within a span
        // of 0.5 has a probability below 0.5^9.
        const auto [lo, hi] = std::ranges::minmax(y);
        CHECK(hi - lo > 0.5f);
    }
}

TEST_CASE("LFO: the Lorenz row steps exactly as an unfused reference, and stays in range at every rate", "[dsp][lfo]")
{
    // The step function, against every product rounded on its own.
    for (const float dt : {1e-6f, 1e-4f, 0.0037f, 0.02f})
    {
        INFO("dt " << dt);
        dsp::LorenzState a{.x = 1.3f, .y = -2.1f, .z = 24.0f};
        dsp::LorenzState b = a;
        bool same = true;
        for (int i = 0; i < 200000 && same; ++i)
        {
            dsp::lorenzStep(a, dt);
            unfusedLorenzStep(b, dt);
            same = sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.z, b.z);
        }
        CHECK(same);
    }

    // The row: within +-1, continuous, and its time scaled by the rate (about
    // the same number of zero crossings per cycle at every rate).
    std::vector<double> crossingsPerCycle;
    for (const float hz : {0.5f, 5.0f, 50.0f})
    {
        INFO(hz << " Hz");
        auto lfo = prepared(48000.0f, LfoWaveform::lorenz, hz, 11);
        const auto cycles = 300.0;
        const auto y = render(lfo, static_cast<std::size_t>(cycles * 48000.0 / static_cast<double>(hz)));
        CHECK(std::ranges::all_of(y, [](float v) { return v > -1.0f && v < 1.0f; }));
        std::size_t crossings = 0;
        double worst = 0.0;
        for (std::size_t n = 1; n < y.size(); ++n)
        {
            crossings += (y[n] < 0.0f) != (y[n - 1] < 0.0f) ? 1U : 0U;
            worst = std::max(worst, static_cast<double>(std::abs(y[n] - y[n - 1])));
        }
        crossingsPerCycle.push_back(static_cast<double>(crossings) / cycles);
        // |dx/dt| = 10 |y - x|, and |y - x| < 50 on the attractor (|x| < 20,
        // |y| < 30), so x moves at most 500 per time unit: per sample that is
        // 500 x 0.7 x hz / rate time units, over the output scale 20.
        CHECK(worst <= 500.0 * 0.7 * static_cast<double>(hz) / 48000.0 / 20.0);
    }
    // Rate only scales its time: the crossing rates agree within a factor 2
    // (the attractor's own irregularity over 300 cycles).
    const auto [lo, hi] = std::ranges::minmax(crossingsPerCycle);
    CHECK(lo > 0.1);
    CHECK(hi < 2.0 * lo);
}

TEST_CASE("LFO: the same seed gives the same output; voices with adjacent seeds are independent", "[dsp][lfo]")
{
    for (const auto waveform : {LfoWaveform::sampleHold, LfoWaveform::smooth, LfoWaveform::lorenz})
    {
        INFO("row " << static_cast<int>(waveform));
        auto a = prepared(48000.0f, waveform, 100.0f, 5);
        auto b = prepared(48000.0f, waveform, 100.0f, 5);
        auto c = prepared(48000.0f, waveform, 100.0f, 6);
        const auto ya = render(a, 960000);
        CHECK(bitIdentical(ya, render(b, 960000)));
        const auto yc = render(c, 960000);
        // 2000 cycles. Independent signals whose values stay correlated over
        // at most about 5 cycles (the Lorenz row's dwell in a lobe; one cycle
        // for the others) correlate within about 4 sqrt(5 / 2000) = 0.2.
        CHECK(std::abs(correlation(ya, yc)) <= 0.2);
    }
}

TEST_CASE("LFO: a copy continues exactly as the original (Free: a voice copies the global LFO)", "[dsp][lfo]")
{
    for (const auto waveform : allWaveforms)
    {
        INFO("row " << static_cast<int>(waveform));
        auto global = prepared(48000.0f, waveform, 3.7f, 21);
        static_cast<void>(render(global, 12345));
        Lfo voice = global;
        CHECK(bitIdentical(render(voice, 20000), render(global, 20000)));
        // The copy's own rate changes it alone.
        voice.setFrequency(5.0f);
        const auto apart = render(voice, 20000);
        CHECK_FALSE(bitIdentical(apart, render(global, 20000)));
    }
}

TEST_CASE("LFO: a waveform change is slewed, with the phase continuing", "[dsp][lfo]")
{
    auto lfo = prepared(48000.0f, LfoWaveform::saw, 2.0f);
    const auto y = render(lfo, 6000); // the saw at -0.5
    lfo.setWaveform(LfoWaveform::ramp);
    const auto after = render(lfo, 200);
    const std::size_t slew = expectedSlew(48000.0f, 2.0f);
    const double slope = 2.0 * 2.0 / 48000.0;
    CHECK(std::abs(static_cast<double>(after[0] - y.back())) <= slope + (4.0 * u));
    // The ramp is at +0.5 at that phase: the jump of 1 spread over the slew.
    for (std::size_t n = 1; n < after.size(); ++n)
    {
        CHECK(std::abs(static_cast<double>(after[n] - after[n - 1])) <=
              (1.0 / static_cast<double>(slew)) + slope + (8.0 * u));
    }
    CHECK_THAT(after[slew], WithinAbs(ideal(LfoWaveform::ramp, cyclesAt(0, lfo.phaseIncrement(), 6000 + slew)), 1e-6));
}

TEST_CASE("LFO: the output does not depend on the block size", "[dsp][lfo]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    for (const auto waveform : allWaveforms)
    {
        INFO("rate " << rate << ", row " << static_cast<int>(waveform));
        // A script: retriggers and restarts (two on adjacent samples), rate,
        // gain, offset, and waveform changes at fixed samples.
        constexpr std::size_t length = 30000;
        const auto renderIn = [&](std::size_t block)
        {
            auto lfo = prepared(rate, waveform, 3.0f, 4);
            std::vector<float> y(length);
            constexpr std::array<std::size_t, 6> changes{1000, 5003, 9000, 15555, 20000, 26001};
            constexpr std::array<std::size_t, 4> triggers{2500, 2501, 12000, 22222};
            std::size_t start = 0;
            std::size_t nextChange = 0;
            while (start < length)
            {
                std::size_t end = std::min(start + block, length);
                if (nextChange < changes.size() && changes[nextChange] < end)
                {
                    end = changes[nextChange];
                }
                std::vector<LfoEvent> events;
                for (const auto t : triggers)
                {
                    if (t >= start && t < end)
                    {
                        events.push_back(
                            {.offset = t - start, .trigger = t == 12000 ? LfoTrigger::restart : LfoTrigger::retrigger});
                    }
                }
                lfo.process(std::span{y}.subspan(start, end - start), events);
                start = end;
                if (nextChange < changes.size() && changes[nextChange] == start)
                {
                    switch (nextChange)
                    {
                    case 0:
                        lfo.setFrequency(13.7f);
                        break;
                    case 1:
                        lfo.setGain(0.4f);
                        break;
                    case 2:
                        lfo.setFrequency(0.3f);
                        break;
                    case 3:
                        lfo.setOffset(0.5f);
                        break;
                    case 4:
                        lfo.setWaveform(waveform == LfoWaveform::saw ? LfoWaveform::smooth : LfoWaveform::saw);
                        break;
                    default:
                        lfo.setFrequency(777.0f);
                        break;
                    }
                    ++nextChange;
                }
            }
            return y;
        };
        const auto one = renderIn(1);
        CHECK(bitIdentical(one, renderIn(37)));
        CHECK(bitIdentical(one, renderIn(4096)));
    }

    // next() gives what process() does.
    for (const auto waveform : allWaveforms)
    {
        auto a = prepared(48000.0f, waveform, 61.0f, 2);
        auto b = prepared(48000.0f, waveform, 61.0f, 2);
        CHECK(bitIdentical(render(a, 5000), renderNext(b, 5000)));
    }
}

TEST_CASE("LFO: no subnormal outputs, at any rate, row, gain, or offset", "[dsp][lfo]")
{
    const auto isSubnormal = [](float v)
    {
        return std::fpclassify(v) == FP_SUBNORMAL;
    };
    for (const auto waveform : allWaveforms)
    {
        for (const float hz : {0.01f, 0.5f, 300.0f})
        {
            INFO("row " << static_cast<int>(waveform) << ", " << hz << " Hz");
            // A gain of 1e-35 would make every value below 1.2e-3 subnormal:
            // it is flushed to 0, as is the offset.
            auto lfo = prepared(48000.0f, waveform, hz, 8);
            lfo.setGain(1e-35f);
            lfo.setOffset(1e-35f);
            for (int s = 0; s < 20; ++s)
            {
                const auto y = render(lfo, 12000);
                CHECK(std::ranges::none_of(y, isSubnormal));
                lfo.retrigger();
                lfo.setGain(s % 2 == 0 ? 1.0f : 1e-35f);
            }
        }
    }
}

TEST_CASE("LFO: no allocation in any control, trigger, or process call", "[dsp][lfo]")
{
    Lfo lfo{3};
    lfo.prepare({.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 1});
    std::array<float, 512> out{};
    const std::array events{
        LfoEvent{.offset = 3, .trigger = LfoTrigger::retrigger},
        LfoEvent{.offset = 300, .trigger = LfoTrigger::restart},
    };
    std::size_t count = 0;
    {
        const anoesis::test::AllocationCounter counter;
        for (const auto waveform : allWaveforms)
        {
            lfo.setWaveform(waveform);
            lfo.setFrequency(5.0f);
            lfo.setGain(0.5f);
            lfo.setOffset(0.25f);
            lfo.retrigger();
            lfo.restart();
            static_cast<void>(lfo.next());
            lfo.process(out);
            lfo.process(out, events);
            Lfo copy = lfo;
            copy.process(out);
        }
        lfo.setSeed(9);
        lfo.reset();
        count = counter.count();
    }
    CHECK(count == 0);
}

TEST_CASE("LFO: stable under random controls, triggers, and block sizes", "[dsp][lfo]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    INFO("rate " << rate);
    // One random script, rendered in blocks of 1, 37, and 4096.
    constexpr std::size_t length = 200000;
    const auto renderIn = [&](std::size_t block)
    {
        dsp::Random script{42};
        Lfo lfo{17};
        lfo.prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 1});
        std::vector<float> y(length);
        // Controls change every 997 samples; triggers at random offsets.
        std::size_t start = 0;
        while (start < length)
        {
            const std::size_t segmentEnd = std::min(start + 997, length);
            // Controls for this segment.
            const float pick = script.nextUnipolar();
            const auto waveform = static_cast<LfoWaveform>(static_cast<std::size_t>(pick * 11.0f));
            lfo.setWaveform(waveform);
            const float choice = script.nextUnipolar();
            if (choice < 0.1f)
            {
                lfo.setFrequency(0.0f);
            }
            else if (choice < 0.2f)
            {
                lfo.setFrequency(rate); // past Nyquist
            }
            else if (choice < 0.3f)
            {
                lfo.setFrequency(std::numeric_limits<float>::quiet_NaN());
            }
            else
            {
                lfo.setFrequency(0.01f * std::exp2(20.0f * script.nextUnipolar()));
            }
            lfo.setGain(script.nextUnipolar() * 1.2f);
            lfo.setOffset(script.nextBipolar() * 1.2f);
            std::vector<std::size_t> triggers;
            triggers.reserve(3);
            for (int t = 0; t < 3; ++t)
            {
                triggers.push_back(start + static_cast<std::size_t>(script.nextUnipolar() * 997.0f));
            }
            std::ranges::sort(triggers);
            for (std::size_t s = start; s < segmentEnd;)
            {
                const std::size_t e = std::min(s + block, segmentEnd);
                std::vector<LfoEvent> events;
                for (std::size_t t = 0; t < triggers.size(); ++t)
                {
                    if (triggers[t] >= s && triggers[t] < e)
                    {
                        events.push_back({
                            .offset = triggers[t] - s,
                            .trigger = t == 1 ? LfoTrigger::restart : LfoTrigger::retrigger,
                        });
                    }
                }
                lfo.process(std::span{y}.subspan(s, e - s), events);
                s = e;
            }
            start = segmentEnd;
        }
        return y;
    };
    const auto one = renderIn(1);
    // The wave passes +-1 by at most its steepest slope (4 per cycle, the
    // triangle) over a slew of at most 1/16 cycle: 0.25. With gain <= 1 and
    // |offset| <= 1, |output| <= 2.25; every value finite.
    CHECK(std::ranges::all_of(one, [](float v) { return std::isfinite(v) && std::abs(v) <= 2.25f; }));
    CHECK(std::ranges::none_of(one, [](float v) { return std::fpclassify(v) == FP_SUBNORMAL; }));
    CHECK(bitIdentical(one, renderIn(37)));
    CHECK(bitIdentical(one, renderIn(4096)));
}
