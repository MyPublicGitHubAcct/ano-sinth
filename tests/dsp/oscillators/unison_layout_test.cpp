// Tests for the unison laws (unison.hpp): where each voice sits in pitch and
// in the stereo field, how the oscillator's Pan combines with Stereo (O8),
// and the noise mix that stands in for N noise voices.
// Doc: docs/dsp/oscillator-unison.md

#include <anoesis/dsp/math/pan.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/unison.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

using anoesis::dsp::maxUnisonVoices;
using anoesis::dsp::panGains;
using anoesis::dsp::StereoGains;
using anoesis::dsp::unisonLayout;
using anoesis::dsp::unisonNoiseMix;
using anoesis::dsp::unisonPosition;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinULP;

TEST_CASE("Unison detune: voices evenly spaced in semitones, lowest first, symmetric about the centre",
          "[dsp][oscillator][unison]")
{
    // The working hypothesis (inventory O13): linear in semitones, evenly
    // spaced, the outermost voices at -1 and +1 of the spread.
    CHECK_THAT(unisonLayout(1).detune[0], WithinULP(0.0f, 0));
    for (std::size_t count = 2; count <= maxUnisonVoices; ++count)
    {
        CAPTURE(count);
        const auto& layout = unisonLayout(count);
        CHECK_THAT(layout.detune[0], WithinULP(-1.0f, 0));
        CHECK_THAT(layout.detune[count - 1], WithinULP(1.0f, 0));
        const double spacing = 2.0 / static_cast<double>(count - 1);
        for (std::size_t k = 0; k < count; ++k)
        {
            CAPTURE(k);
            const double expected = -1.0 + (spacing * static_cast<double>(k));
            CHECK_THAT(layout.detune[k], WithinAbs(expected, 1e-7));
            // Exactly antisymmetric: +-d is one pair.
            CHECK_THAT(layout.detune[k], WithinULP(-layout.detune[count - 1 - k], 0));
        }
        for (std::size_t k = count; k < maxUnisonVoices; ++k)
        {
            CHECK_THAT(layout.detune[k], WithinULP(0.0f, 0));
        }
    }
    CHECK_THAT(unisonLayout(7).detune[3], WithinULP(0.0f, 0)); // the centre voice is in tune
}

TEST_CASE("Unison stereo sides follow the reported orders, each +-d pair split left and right",
          "[dsp][oscillator][unison]")
{
    // BlitBit's orders at Stereo 10 (inventory §2, O14), read from the
    // lowest voice to the highest.
    constexpr std::array<std::string_view, maxUnisonVoices + 1> orders{
        "", "C", "LR", "LCR", "LRLR", "LRCLR", "LRRLLR", "LRLCRLR", "LRRLRLLR",
    };
    const auto sideOf = [](char c)
    {
        if (c == 'L')
        {
            return -1.0f;
        }
        return c == 'R' ? 1.0f : 0.0f;
    };
    for (std::size_t count = 0; count <= maxUnisonVoices; ++count)
    {
        CAPTURE(count);
        const auto& layout = unisonLayout(count);
        for (std::size_t k = 0; k < maxUnisonVoices; ++k)
        {
            CAPTURE(k);
            const float expected = k < count ? sideOf(orders[count][k]) : 0.0f;
            CHECK_THAT(layout.side[k], WithinULP(expected, 0));
            if (k < count)
            {
                CHECK_THAT(layout.side[k], WithinULP(-layout.side[count - 1 - k], 0));
            }
        }
        if (count > 0)
        {
            CHECK_THAT(layout.side[0], WithinULP(count == 1 ? 0.0f : -1.0f, 0)); // the lowest voice is left
        }
    }
    // Out-of-range counts clamp to 8.
    CHECK_THAT(unisonLayout(100).detune[7], WithinULP(1.0f, 0));
}

TEST_CASE("Unison position: Pan offsets every voice, clipped at the sides (O8)", "[dsp][oscillator][unison]")
{
    CHECK_THAT(unisonPosition(0.0f, 0.0f, -1.0f), WithinULP(0.0f, 0));
    CHECK_THAT(unisonPosition(0.0f, 1.0f, -1.0f), WithinULP(-1.0f, 0));
    CHECK_THAT(unisonPosition(0.0f, 0.5f, 1.0f), WithinULP(0.5f, 0));
    CHECK_THAT(unisonPosition(0.5f, 1.0f, -1.0f), WithinULP(-0.5f, 0));
    CHECK_THAT(unisonPosition(0.5f, 1.0f, 1.0f), WithinULP(1.0f, 0)); // 1.5, clipped
    CHECK_THAT(unisonPosition(-1.0f, 1.0f, -1.0f), WithinULP(-1.0f, 0));
    CHECK_THAT(unisonPosition(-1.0f, 1.0f, 1.0f), WithinULP(0.0f, 0));
    CHECK_THAT(unisonPosition(0.25f, 0.6f, 0.0f), WithinULP(0.25f, 0)); // the centre voice is the pan

    // The width is clamped to [0, 1]; NaN counts as 0 (pan) and the centre.
    CHECK_THAT(unisonPosition(0.0f, 3.0f, 1.0f), WithinULP(1.0f, 0));
    CHECK_THAT(unisonPosition(0.0f, -1.0f, 1.0f), WithinULP(0.0f, 0));
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_THAT(unisonPosition(nan, 1.0f, 1.0f), WithinULP(1.0f, 0));
    CHECK_THAT(unisonPosition(0.5f, nan, 1.0f), WithinULP(0.5f, 0));
}

namespace
{

// The covariance the mix should reproduce: for unit-variance sources, left
// power sum(l^2), right power sum(r^2), and cross term sum(l r).
struct Covariance
{
    double left = 0.0;
    double right = 0.0;
    double cross = 0.0;
};

Covariance covarianceOf(std::span<const StereoGains> voices)
{
    Covariance c;
    for (const auto& g : voices)
    {
        c.left += static_cast<double>(g.left) * g.left;
        c.right += static_cast<double>(g.right) * g.right;
        c.cross += static_cast<double>(g.left) * g.right;
    }
    return c;
}

} // namespace

TEST_CASE("Unison noise mix: two sources mixed to the power and correlation of N noise voices",
          "[dsp][oscillator][unison][noise]")
{
    // L = left n1 + cross n2, R = cross n1 + right n2 with n1, n2
    // independent: the mix matrix squared must be the voices' covariance.
    const auto checkSquare = [](std::span<const StereoGains> voices)
    {
        const auto mix = unisonNoiseMix(voices, false);
        const auto c = covarianceOf(voices);
        const double a = mix.left;
        const double b = mix.right;
        const double x = mix.cross;
        const double scale = std::max(1.0, c.left + c.right);
        CHECK_THAT((a * a) + (x * x), WithinAbs(c.left, 2e-6 * scale));
        CHECK_THAT((b * b) + (x * x), WithinAbs(c.right, 2e-6 * scale));
        CHECK_THAT(x * (a + b), WithinAbs(c.cross, 2e-6 * scale));
    };

    anoesis::dsp::Random random{7};
    for (int trial = 0; trial < 200; ++trial)
    {
        CAPTURE(trial);
        const auto count = 1 + (random.nextUint32() % maxUnisonVoices);
        std::vector<StereoGains> voices;
        for (std::uint32_t v = 0; v < count; ++v)
        {
            voices.push_back(panGains(random.nextBipolar()));
            voices.back().left *= 1.5f * random.nextUnipolar();
            voices.back().right *= 1.5f * random.nextUnipolar();
        }
        checkSquare(voices);
    }

    // One voice hard left: the left source alone, exactly.
    const std::array hardLeft{panGains(-1.0f)};
    const auto one = unisonNoiseMix(hardLeft, false);
    CHECK_THAT(one.left, WithinULP(1.0f, 0));
    CHECK_THAT(one.right, WithinULP(0.0f, 0));
    CHECK_THAT(one.cross, WithinULP(0.0f, 0));

    // 8 voices at the centre (Stereo 0): fully correlated, L and R the same.
    std::array<StereoGains, 8> centre{};
    centre.fill(panGains(0.0f));
    const auto mono = unisonNoiseMix(centre, false);
    CHECK_THAT(mono.left, WithinULP(mono.cross, 0));
    CHECK_THAT(mono.right, WithinULP(mono.cross, 0));
    checkSquare(centre);

    // 4 left, 4 right (Stereo 10): independent channels, each the power of 4.
    std::array<StereoGains, 8> split{};
    for (std::size_t v = 0; v < split.size(); ++v)
    {
        split[v] = panGains(v % 2 == 0 ? -1.0f : 1.0f);
    }
    const auto wide = unisonNoiseMix(split, false);
    CHECK_THAT(wide.left, WithinAbs(2.0, 1e-6));
    CHECK_THAT(wide.right, WithinAbs(2.0, 1e-6));
    CHECK_THAT(wide.cross, WithinAbs(0.0, 1e-6));

    // Invert negates the whole mix; no voices is silence.
    const auto inverted = unisonNoiseMix(split, true);
    CHECK_THAT(inverted.left, WithinULP(-wide.left, 0));
    CHECK_THAT(inverted.right, WithinULP(-wide.right, 0));
    CHECK_THAT(inverted.cross, WithinULP(-wide.cross, 0));
    const auto none = unisonNoiseMix(std::span<const StereoGains>{}, false);
    CHECK_THAT(none.left, WithinULP(0.0f, 0));
    CHECK_THAT(none.right, WithinULP(0.0f, 0));
    CHECK_THAT(none.cross, WithinULP(0.0f, 0));
}
