// Tests for the seedable random number generator (xoshiro128+).
// Doc: docs/dsp/white-noise.md

#include <anoesis/dsp/math/random.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using anoesis::dsp::Random;

TEST_CASE("Random: the same seed gives the same sequence; different seeds differ", "[dsp][random]")
{
    Random a{1234};
    Random b{1234};
    Random c{1235};
    bool same = true;
    int differences = 0;
    for (int i = 0; i < 10000; ++i)
    {
        const auto x = a.nextUint32();
        same = same && x == b.nextUint32();
        differences += x != c.nextUint32() ? 1 : 0;
    }
    CHECK(same);
    CHECK(differences > 9990); // independent streams agree only by chance (2^-32 per draw)
}

TEST_CASE("Random: reset() and setSeed() restart a sequence", "[dsp][random]")
{
    Random random{42};
    std::array<std::uint32_t, 8> first{};
    for (auto& x : first)
    {
        x = random.nextUint32();
    }

    random.reset();
    for (const auto x : first)
    {
        CHECK(random.nextUint32() == x);
    }

    random.setSeed(7);
    const auto fromSeven = random.nextUint32();
    random.setSeed(42);
    CHECK(random.nextUint32() == first[0]);
    CHECK(random.seed() == 42);
    CHECK(fromSeven != first[0]);
}

TEST_CASE("Random: known-answer values pin the algorithm on every platform", "[dsp][random]")
{
    // The first outputs for seed 0 and seed 1, recorded when the generator was
    // written and cross-checked against an independent Python port of
    // SplitMix64 seeding + xoshiro128+. Integer arithmetic only, so every
    // compiler and CPU must reproduce them bit for bit. If this fails, presets
    // and golden renders that use noise will no longer match.
    Random zero{0};
    const std::array<std::uint32_t, 4> expectedZero{0xE9966C19U, 0xB8F8985EU, 0xC3536FC5U, 0x97D6A8F6U};
    for (const auto x : expectedZero)
    {
        CHECK(zero.nextUint32() == x);
    }

    Random one{1};
    const std::array<std::uint32_t, 4> expectedOne{0x47EDEA62U, 0xB3E6660BU, 0xFE08B43DU, 0x6F8BCA7DU};
    for (const auto x : expectedOne)
    {
        CHECK(one.nextUint32() == x);
    }
}

TEST_CASE("Random: float conversions stay in range", "[dsp][random]")
{
    Random random{99};
    bool bipolarInRange = true;
    bool bipolarNonZero = true;
    bool unipolarInRange = true;
    for (int i = 0; i < 100000; ++i)
    {
        const auto b = random.nextBipolar();
        bipolarInRange = bipolarInRange && b > -1.0f && b < 1.0f;
        bipolarNonZero = bipolarNonZero && (b > 0.0f || b < 0.0f);
        const auto u = random.nextUnipolar();
        unipolarInRange = unipolarInRange && u >= 0.0f && u < 1.0f;
    }
    CHECK(bipolarInRange);
    CHECK(bipolarNonZero); // the odd-numerator mapping never produces exactly 0
    CHECK(unipolarInRange);
}
