// Tests for the pan laws: panGains(position, law) maps a pan position in
// [-1, 1] to a left and a right gain. Doc: docs/dsp/oscillator-controls.md

#include <anoesis/dsp/math/pan.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <numbers>

using anoesis::dsp::panGains;
using anoesis::dsp::PanLaw;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

namespace
{

// Positions from -1 to 1 in steps of 1/64 (exact in float).
constexpr int steps = 128;

float positionAt(int i)
{
    return -1.0f + (static_cast<float>(i) / 64.0f);
}

} // namespace

TEST_CASE("Equal-power pan: hard sides are exact, the centre is -3 dB", "[dsp][math][pan]")
{
    const auto hardLeft = panGains(-1.0f, PanLaw::equalPower);
    CHECK_THAT(hardLeft.left, WithinULP(1.0f, 0));
    CHECK_THAT(hardLeft.right, WithinULP(0.0f, 0));

    const auto hardRight = panGains(1.0f, PanLaw::equalPower);
    CHECK_THAT(hardRight.left, WithinULP(0.0f, 0));
    CHECK_THAT(hardRight.right, WithinULP(1.0f, 0));

    // sqrt(1/2) = -3.01 dB on each side. 1e-6 relative: sin(pi/4) rounded
    // to float twice (the angle and the result), about 1 ulp each.
    const auto centre = panGains(0.0f, PanLaw::equalPower);
    CHECK_THAT(centre.left, WithinRel(std::numbers::sqrt2_v<float> / 2.0f, 1e-6f));
    CHECK_THAT(centre.left, WithinULP(centre.right, 0));
    CHECK_THAT(20.0f * std::log10(centre.left), WithinAbs(-3.0103f, 1e-4f));

    // The default law is equal power.
    CHECK_THAT(panGains(0.3f).left, WithinULP(panGains(0.3f, PanLaw::equalPower).left, 0));
}

TEST_CASE("Equal-power pan: sin/cos law, constant power, mirror symmetric", "[dsp][math][pan]")
{
    for (int i = 0; i <= steps; ++i)
    {
        const auto p = positionAt(i);
        CAPTURE(p);
        const auto g = panGains(p, PanLaw::equalPower);

        // The sin/cos law in double. 1e-6: the float angle and sin, ~1 ulp each.
        const auto angle = (static_cast<double>(p) + 1.0) * std::numbers::pi / 4.0;
        CHECK_THAT(g.left, WithinAbs(std::cos(angle), 1e-6));
        CHECK_THAT(g.right, WithinAbs(std::sin(angle), 1e-6));

        // Constant power: L^2 + R^2 = 1, to the same rounding (2 x 1e-6).
        CHECK_THAT((g.left * g.left) + (g.right * g.right), WithinAbs(1.0f, 2e-6f));

        // Mirror: the left gain at p is exactly the right gain at -p.
        const auto mirrored = panGains(-p, PanLaw::equalPower);
        CHECK_THAT(g.left, WithinULP(mirrored.right, 0));
        CHECK_THAT(g.right, WithinULP(mirrored.left, 0));

        if (i > 0)
        {
            const auto before = panGains(positionAt(i - 1), PanLaw::equalPower);
            CHECK(g.left < before.left);
            CHECK(g.right > before.right);
        }
    }

    // Halfway to the left: the point that tells sin/cos from the square-root
    // law (inventory O6): cos(pi/8) = 0.924 and sin(pi/8) = 0.383, against
    // 0.866 and 0.5.
    const auto halfLeft = panGains(-0.5f, PanLaw::equalPower);
    CHECK_THAT(halfLeft.left, WithinAbs(0.9238795, 1e-6));
    CHECK_THAT(halfLeft.right, WithinAbs(0.3826834, 1e-6));
}

TEST_CASE("Linear pan: -6 dB at the centre, the gains sum to 1", "[dsp][math][pan]")
{
    CHECK_THAT(panGains(0.0f, PanLaw::linear).left, WithinULP(0.5f, 0));
    CHECK_THAT(panGains(0.0f, PanLaw::linear).right, WithinULP(0.5f, 0));
    CHECK_THAT(panGains(-1.0f, PanLaw::linear).left, WithinULP(1.0f, 0));
    CHECK_THAT(panGains(-1.0f, PanLaw::linear).right, WithinULP(0.0f, 0));
    for (int i = 0; i <= steps; ++i)
    {
        const auto p = positionAt(i);
        CAPTURE(p);
        const auto g = panGains(p, PanLaw::linear);
        CHECK_THAT(g.left + g.right, WithinULP(1.0f, 1));
        CHECK_THAT(g.right, WithinULP((p + 1.0f) * 0.5f, 0));
    }
}

TEST_CASE("Balance pan: unity at the centre, only the far side fades", "[dsp][math][pan]")
{
    CHECK_THAT(panGains(0.0f, PanLaw::balance).left, WithinULP(1.0f, 0));
    CHECK_THAT(panGains(0.0f, PanLaw::balance).right, WithinULP(1.0f, 0));
    CHECK_THAT(panGains(0.5f, PanLaw::balance).left, WithinULP(0.5f, 0));
    CHECK_THAT(panGains(0.5f, PanLaw::balance).right, WithinULP(1.0f, 0));
    CHECK_THAT(panGains(-1.0f, PanLaw::balance).left, WithinULP(1.0f, 0));
    CHECK_THAT(panGains(-1.0f, PanLaw::balance).right, WithinULP(0.0f, 0));
}

TEST_CASE("Pan positions outside [-1, 1] clamp, and NaN is the centre", "[dsp][math][pan]")
{
    for (const auto law : {PanLaw::equalPower, PanLaw::linear, PanLaw::balance})
    {
        CAPTURE(static_cast<int>(law));
        CHECK_THAT(panGains(-7.0f, law).left, WithinULP(panGains(-1.0f, law).left, 0));
        CHECK_THAT(panGains(-7.0f, law).right, WithinULP(panGains(-1.0f, law).right, 0));
        CHECK_THAT(panGains(2.0f, law).right, WithinULP(panGains(1.0f, law).right, 0));
        CHECK_THAT(panGains(2.0f, law).left, WithinULP(panGains(1.0f, law).left, 0));
        const auto nan = panGains(std::numeric_limits<float>::quiet_NaN(), law);
        CHECK_THAT(nan.left, WithinULP(panGains(0.0f, law).left, 0));
        CHECK_THAT(nan.right, WithinULP(panGains(0.0f, law).right, 0));
    }
}
