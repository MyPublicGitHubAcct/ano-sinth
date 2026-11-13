// Accuracy and behaviour of the fast exp2 and tanh approximations. The
// measured errors are captured, so `anoesis_dsp_tests "[approximation]" -s`
// prints them. Doc: docs/dsp/fast-math.md

#include <anoesis/dsp/math/fast_math.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

using anoesis::dsp::fastExp2;
using anoesis::dsp::fastTanh;
using Catch::Matchers::WithinULP;

namespace
{

constexpr float qnan = std::numeric_limits<float>::quiet_NaN();
constexpr float inf = std::numeric_limits<float>::infinity();

} // namespace

TEST_CASE("fastExp2: exact at integers", "[dsp][math][approximation]")
{
    for (int n = -126; n <= 127; ++n)
    {
        CAPTURE(n);
        CHECK_THAT(fastExp2(static_cast<float>(n)), WithinULP(std::ldexp(1.0f, n), 0));
    }
}

TEST_CASE("fastExp2: relative error against exp2 over the full domain", "[dsp][math][approximation]")
{
    // Bound: the minimax polynomial's own error is at most 2.8e-7 relative
    // (tools/design/exp2_polynomial.py). Evaluating it in float adds a few
    // roundings of values below 2 (2^-24 = 6e-8 relative each), and whether
    // the compiler fuses them into FMAs changes the last bit. 5e-7 allows
    // about four such roundings on top of the design error.
    double maxRelError = 0.0;
    for (int i = -126 * 1024; i < 127 * 1024; ++i)
    {
        const auto x = (static_cast<float>(i) / 1024.0f) + 0.000377f; // off the integer grid
        const auto expected = std::exp2(static_cast<double>(x));
        const auto relError = std::abs(static_cast<double>(fastExp2(x)) - expected) / expected;
        maxRelError = std::max(maxRelError, relError);
    }
    CAPTURE(maxRelError);
    CHECK(maxRelError < 5e-7);
}

TEST_CASE("fastExp2: monotonic and continuous across integer boundaries", "[dsp][math][approximation]")
{
    // Monotonic on a dense grid in [-4, 4]: each step is ~1 float ulp of x.
    float previous = fastExp2(-4.0f);
    bool monotonic = true;
    for (int i = (-4 * 65536) + 1; i < 4 * 65536; ++i)
    {
        const auto y = fastExp2(static_cast<float>(i) / 65536.0f);
        monotonic = monotonic && y >= previous;
        previous = y;
    }
    CHECK(monotonic);

    // Just below an integer, the value approaches 2^n from below.
    for (int n = -10; n <= 10; ++n)
    {
        const auto at = static_cast<float>(n);
        const auto below = std::nextafter(at, -inf);
        CAPTURE(n);
        CHECK(fastExp2(below) <= fastExp2(at));
    }
}

TEST_CASE("fastExp2: clamps outside [-126, 127] and never returns inf, NaN, or denormals", "[dsp][math][approximation]")
{
    CHECK_THAT(fastExp2(-200.0f), WithinULP(std::ldexp(1.0f, -126), 0));
    CHECK_THAT(fastExp2(200.0f), WithinULP(std::ldexp(1.0f, 127), 0));
    CHECK_THAT(fastExp2(-inf), WithinULP(std::ldexp(1.0f, -126), 0));
    CHECK_THAT(fastExp2(inf), WithinULP(std::ldexp(1.0f, 127), 0));
    CHECK(std::isnormal(fastExp2(qnan))); // defined and finite: NaN maps to the lower clamp
}

TEST_CASE("fastTanh: error against tanh", "[dsp][math][approximation]")
{
    // Absolute error bound 2e-7, derived: t = m / (m + 2) with m = e^2|x| - 1
    // carrying the polynomial's relative error r = 5.6e-7. Then
    // dt = 2 * dm / (m + 2)^2 = r * 2m / (m + 2)^2, and 2m / (m + 2)^2 peaks
    // at 1/4 (m = 2), so the design contributes at most 1.4e-7. Final rounding
    // adds half an ulp of a value below 1 (3e-8): 1.7e-7, rounded up to 2e-7.
    // Above |x| = 8.6 the result is exactly 1, within 7e-8 of tanh.
    // Relative error bound 1e-6 holds everywhere, including near zero, where
    // a naive 1 - 2/(e^2x + 1) would lose all precision: there the result is
    // q(f) / (q(f) + 2) and q has a 5.6e-7 relative error by design.
    double maxAbsError = 0.0;
    double maxRelError = 0.0;
    for (int i = -12 * 8192; i <= 12 * 8192; ++i)
    {
        const auto x = (static_cast<float>(i) / 8192.0f) + 0.0000311f;
        const auto expected = std::tanh(static_cast<double>(x));
        const auto error = std::abs(static_cast<double>(fastTanh(x)) - expected);
        maxAbsError = std::max(maxAbsError, error);
        maxRelError = std::max(maxRelError, error / std::abs(expected));
    }
    for (int i = 0; i < 700; ++i) // tiny inputs, 1e-30 to 0.09: relative accuracy must hold
    {
        const auto x = 1e-30f * std::pow(1.1f, static_cast<float>(i));
        const auto expected = std::tanh(static_cast<double>(x));
        maxRelError = std::max(maxRelError, std::abs(static_cast<double>(fastTanh(x)) - expected) / expected);
    }
    CAPTURE(maxAbsError, maxRelError);
    CHECK(maxAbsError < 2e-7);
    CHECK(maxRelError < 1e-6);
}

TEST_CASE("fastTanh: odd, bounded, monotonic", "[dsp][math][approximation]")
{
    CHECK_THAT(fastTanh(0.0f), WithinULP(0.0f, 0));
    CHECK(std::signbit(fastTanh(-0.0f)));

    bool odd = true;
    bool bounded = true;
    bool monotonic = true;
    float previous = fastTanh(-12.0f);
    for (int i = -12 * 4096; i <= 12 * 4096; ++i)
    {
        const auto x = static_cast<float>(i) / 4096.0f;
        const auto y = fastTanh(x);
        odd = odd && std::bit_cast<std::uint32_t>(fastTanh(-x)) == std::bit_cast<std::uint32_t>(-y);
        bounded = bounded && std::abs(y) <= 1.0f;
        monotonic = monotonic && y >= previous;
        previous = y;
    }
    CHECK(odd);
    CHECK(bounded);
    CHECK(monotonic);
}

TEST_CASE("fastTanh: saturates to +-1 for large, infinite, and NaN inputs", "[dsp][math][approximation]")
{
    CHECK_THAT(fastTanh(50.0f), WithinULP(1.0f, 0));
    CHECK_THAT(fastTanh(-50.0f), WithinULP(-1.0f, 0));
    CHECK_THAT(fastTanh(inf), WithinULP(1.0f, 0));
    CHECK_THAT(fastTanh(-inf), WithinULP(-1.0f, 0));
    CHECK(std::abs(fastTanh(qnan)) <= 1.0f); // defined and bounded; never propagates NaN
}
