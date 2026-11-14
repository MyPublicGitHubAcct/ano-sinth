// Unit tests for the building blocks of each D6 prototype: the PolyBLEP and
// PolyBLAMP residual polynomials, the minBLEP tables, and the wavetable bank.
// Doc: docs/dsp/band-limited-oscillators.md

#include "prototype_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>

using namespace anoesis::test::d6;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

namespace
{

// Integrates f over [from, to] with the trapezoid rule on `steps` intervals.
template <typename F>
double trapezoid(F f, double from, double to, std::size_t steps = 20000)
{
    const auto h = (to - from) / static_cast<double>(steps);
    double sum = 0.5 * (f(from) + f(to));
    for (std::size_t i = 1; i < steps; ++i)
    {
        sum += f(from + (h * static_cast<double>(i)));
    }
    return sum * h;
}

// The same, split at 0, where the step residual jumps: across a jump the
// trapezoid rule is only first-order accurate.
template <typename F>
double integrate(F f, double from, double to)
{
    if (from < 0.0 && to > 0.0)
    {
        return trapezoid(f, from, -1e-12) + trapezoid(f, 0.0, to);
    }
    return trapezoid(f, from, to);
}

} // namespace

TEST_CASE("PolyBLEP residuals are the kernel's step and ramp, minus the naive ones", "[dsp][oscillator][d6]")
{
    const auto points = GENERATE(2, 4);
    CAPTURE(points);
    const auto support = points == 2 ? 1.0f : 2.0f;
    // Signed residuals: x < 0 before the edge, x >= 0 after it.
    const auto step = [&](double x)
    {
        const auto d = static_cast<float>(std::abs(x));
        const auto r = points == 2 ? proto::polyblep::stepResidual<2>(d) : proto::polyblep::stepResidual<4>(d);
        return static_cast<double>(x < 0.0 ? r : -r);
    };
    const auto ramp = [&](double x)
    {
        const auto d = static_cast<float>(std::abs(x));
        return static_cast<double>(points == 2 ? proto::polyblep::rampResidual<2>(d)
                                               : proto::polyblep::rampResidual<4>(d));
    };

    // The residual cancels the naive unit step at the edge: +1/2 just before,
    // -1/2 just after, so naive + residual passes through the midpoint.
    CHECK_THAT(step(-1e-7), WithinAbs(0.5, 1e-6));
    CHECK_THAT(step(0.0), WithinAbs(-0.5, 1e-6));
    // Both vanish at the edge of the support.
    CHECK_THAT(step(static_cast<double>(support)), WithinAbs(0.0, 1e-7));
    CHECK_THAT(ramp(static_cast<double>(support)), WithinAbs(0.0, 1e-7));
    // A band-limited step has the same area as the naive one, so the residual
    // integrates to zero, and the ramp residual is its running integral.
    CHECK_THAT(integrate(step, -support, support), WithinAbs(0.0, 1e-6));
    for (const double x : {-1.5, -0.7, -0.2, 0.3, 0.9, 1.6})
    {
        if (std::abs(x) < support)
        {
            CAPTURE(x);
            CHECK_THAT(ramp(x), WithinAbs(integrate(step, -support, x), 1e-5));
        }
    }
    // Continuity where the polynomial pieces meet (x = +-1 for 4 points).
    CHECK_THAT(step(0.999999), WithinAbs(step(1.000001), 1e-5));
    CHECK_THAT(ramp(0.999999), WithinAbs(ramp(1.000001), 1e-5));
}

TEST_CASE("minBLEP tables: a minimum-phase step residual and its compensated integral", "[dsp][oscillator][d6]")
{
    for (const auto* table : {&minBlepTable32(), &minBlepTable64()})
    {
        CAPTURE(table->taps());
        // At the edge the band-limited step has barely started (minimum
        // phase: the energy comes first, nothing before), so the residual
        // is about -1 there, and it settles at exactly 0 by the last tap.
        CHECK_THAT(table->step(0, 0), WithinAbs(-1.0, 0.05));
        CHECK_THAT(table->step(table->phases(), table->taps() - 1), WithinAbs(0.0, 1e-7));
        CHECK_THAT(table->ramp(table->phases(), table->taps() - 1), WithinAbs(0.0, 1e-6));
        // The ramp table starts at the step's group delay (so the delayed
        // naive slope it compensates is continuous) and the delay is short:
        // most of a minimum-phase filter's energy is in its first samples.
        CHECK_THAT(table->ramp(0, 0), WithinRel(static_cast<double>(table->delay()), 1e-3));
        CHECK(table->delay() > 0.5f);
        CHECK(table->delay() < 4.0f);
        // Row p + 1 of tap i continues row p: the polyphase layout is one
        // smooth function sampled every 1 / phases() of a sample.
        CHECK_THAT(table->step(table->phases(), 3), WithinAbs(table->step(0, 4), 1e-7));
    }
}

TEST_CASE("Wavetable bank: each level is the band-limited Fourier series", "[dsp][oscillator][d6]")
{
    const auto& bank = wavetableBank();
    constexpr auto size = proto::WavetableBank::tableSize;

    for (std::size_t level = 0; level < proto::WavetableBank::levels; ++level)
    {
        const auto harmonics = proto::WavetableBank::harmonicsAt(level);
        CAPTURE(level, harmonics);
        const auto* saw = bank.table(proto::Shape::saw, level);
        const auto* triangle = bank.table(proto::Shape::triangle, level);
        // Guard point for interpolation.
        CHECK_THAT(saw[size], WithinULP(saw[0], 0));
        for (const std::size_t i : {std::size_t{1}, size / 7, size / 3, size - 5})
        {
            const auto t = static_cast<double>(i) / static_cast<double>(size);
            double sawSum = 0.0;
            double triangleSum = 0.0;
            for (std::size_t k = 1; k <= harmonics; ++k)
            {
                const auto w = 2.0 * std::numbers::pi * static_cast<double>(k) * t;
                sawSum -= std::sin(w) * idealHarmonic(proto::Shape::saw, k);
                triangleSum -= std::cos(w) * idealHarmonic(proto::Shape::triangle, k);
            }
            CHECK_THAT(saw[i], WithinAbs(sawSum, 1e-6));
            CHECK_THAT(triangle[i], WithinAbs(triangleSum, 1e-6));
        }
    }
}

TEST_CASE("Wavetable level selection: the richest level whose harmonics stay above the audible band",
          "[dsp][oscillator][d6]")
{
    // A level is safe when its top harmonic H f0 is at most fs - 20 kHz:
    // anything above Nyquist then folds back above 20 kHz.
    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        const auto limit = std::max(0.5, 1.0 - (20000.0 / sampleRate));
        for (const double hz : {10.0, 27.5, 100.0, 440.0, 1000.0, 3520.0, 8000.0, 15000.0})
        {
            CAPTURE(sampleRate, hz);
            const auto level =
                proto::WavetableOscillator::levelFor(static_cast<float>(hz / sampleRate), static_cast<float>(limit));
            const auto top = static_cast<double>(proto::WavetableBank::harmonicsAt(level)) * hz / sampleRate;
            CHECK(top <= limit);
            if (level > 0)
            {
                // The next richer level would not be safe.
                CHECK(2.0 * top > limit);
            }
        }
    }
}
