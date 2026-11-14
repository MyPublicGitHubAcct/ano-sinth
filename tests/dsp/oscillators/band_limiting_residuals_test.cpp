// Unit tests for the band-limiting building blocks: the minBLEP tables (the
// D6 method) and the PolyBLEP/PolyBLAMP residual polynomials of the test-only
// reference.
// Doc: docs/dsp/band-limited-oscillators.md

#include "oscillator_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>

using namespace anoesis::test::d6;
namespace reference = anoesis::test::reference;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

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
        const auto r = points == 2 ? reference::polyblep::stepResidual<2>(d) : reference::polyblep::stepResidual<4>(d);
        return static_cast<double>(x < 0.0 ? r : -r);
    };
    const auto ramp = [&](double x)
    {
        const auto d = static_cast<float>(std::abs(x));
        return static_cast<double>(points == 2 ? reference::polyblep::rampResidual<2>(d)
                                               : reference::polyblep::rampResidual<4>(d));
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
