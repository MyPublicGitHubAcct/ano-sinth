// Tests for filter keytracking (keytrackOctaves, keytrackSource,
// docs/dsp/keytracking.md): the law (linear in octaves, Keytrack / 10, an
// octave per octave at 10), the reference (A-1), the misc source (the same
// quantity, 0 at A-1 to 1 at A9, unclamped); and with LadderFilter, fed as
// the voice will feed it (each grid point, the pitch at the segment's end):
// a self-oscillating filter at keytrack 10 keeping intervals, following a
// glide, a bend, and the master tune, bit-identical across block sizes,
// allocation-free, and safe with NaN.
//
// Tolerances are derived in each test from float rounding or from the
// report's figures they pin (docs/dsp/keytracking.md).

#include "keytrack_support.hpp"

#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/math/pitch.hpp>

#include <anoesis/test/measure.hpp>

#include <catch2/catch_test_macros.hpp>
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

using anoesis::dsp::keytrackOctaves;
using anoesis::dsp::keytrackReferenceNote;
using anoesis::dsp::keytrackSource;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

namespace
{

namespace dsp = anoesis::dsp;
namespace kt = anoesis::test::keytrack;

/// Two roundings (the division, then the product; the subtraction is exact
/// for quarter-tone pitches below 2^21), each at most half an ulp: 2^-23
/// relative, and half an ulp more for rounding the double reference: 2^-22.
constexpr double twoRoundings = 2.384185791015625e-7;

/// The self-oscillation pitch in Hz over the last 0.5 s of a render started
/// by an impulse, the voice holding `note`.
double selfOscillationHz(const kt::Setup& s, double note, double seconds = 2.5)
{
    const auto r = kt::run(s, kt::kick(static_cast<std::size_t>(seconds * s.rate)), [=](std::size_t) { return note; });
    return anoesis::test::measureFrequency(std::span{r.y}.last(static_cast<std::size_t>(0.5 * s.rate)), s.rate);
}

} // namespace

TEST_CASE("Keytrack: an octave per octave at 10, nothing at the reference (A-1)", "[dsp][filter][keytrack]")
{
    // Evaluated at compile time: constexpr, no state.
    constexpr float atReference = keytrackOctaves(9.0f, 1.0f);
    constexpr float oneOctaveUp = keytrackOctaves(21.0f, 1.0f);
    CHECK_THAT(keytrackReferenceNote, WithinULP(9.0f, 0));
    CHECK_THAT(atReference, WithinULP(0.0f, 0));
    CHECK_THAT(oneOctaveUp, WithinULP(1.0f, 0));
    CHECK_THAT(keytrackOctaves(69.0f, 1.0f), WithinULP(5.0f, 0));   // A4: five octaves above A-1
    CHECK_THAT(keytrackOctaves(129.0f, 1.0f), WithinULP(10.0f, 0)); // A9
    CHECK_THAT(keytrackOctaves(0.0f, 1.0f), WithinULP(-0.75f, 0));  // below the reference: down
    // The amount scales the octaves: keytrack 5 is half tracking, 0 is none.
    CHECK_THAT(keytrackOctaves(69.0f, 0.5f), WithinULP(2.5f, 0));
    CHECK_THAT(keytrackOctaves(69.0f, 0.0f), WithinULP(0.0f, 0));
    CHECK_THAT(keytrackOctaves(127.0f, 0.0f), WithinULP(0.0f, 0));
    // The reference at every amount.
    for (const float amount : {0.0f, 0.25f, 0.5f, 0.73f, 1.0f})
    {
        CHECK_THAT(keytrackOctaves(9.0f, amount), WithinULP(0.0f, 0));
    }
    // Another reference is a parameter.
    CHECK_THAT(keytrackOctaves(69.0f, 1.0f, 69.0f), WithinULP(0.0f, 0));
    CHECK_THAT(keytrackOctaves(81.0f, 1.0f, 69.0f), WithinULP(1.0f, 0));
}

TEST_CASE("Keytrack: linear in octaves over and beyond the keyboard", "[dsp][filter][keytrack]")
{
    for (int i = -96; i <= 600; ++i)
    {
        const auto pitch = 0.25f * static_cast<float>(i); // quarter tones from note -24 to 150
        for (const float amount : {0.1f, 0.25f, 0.5f, 0.73f, 1.0f})
        {
            const double expected = static_cast<double>(amount) * (static_cast<double>(pitch) - 9.0) / 12.0;
            CAPTURE(pitch, amount);
            CHECK_THAT(static_cast<double>(keytrackOctaves(pitch, amount)), WithinRel(expected, twoRoundings));
        }
    }
}

TEST_CASE("Keytrack source: 0 at A-1, 0.5 at A4, 1 at A9, unclamped, the filter's quantity", "[dsp][filter][keytrack]")
{
    constexpr float atA4 = keytrackSource(69.0f);
    CHECK_THAT(keytrackSource(9.0f), WithinULP(0.0f, 0));
    CHECK_THAT(atA4, WithinULP(0.5f, 0));
    CHECK_THAT(keytrackSource(129.0f), WithinULP(1.0f, 0));
    // Linear and unclamped: negative below A-1, past 1 above A9.
    CHECK_THAT(keytrackSource(0.0f), WithinRel(-0.075f, 1e-6f));
    CHECK_THAT(keytrackSource(141.0f), WithinRel(1.1f, 1e-6f));
    CHECK_THAT(keytrackSource(127.0f), WithinRel(118.0f / 120.0f, 1e-6f));
    for (int i = -96; i <= 600; ++i)
    {
        const auto pitch = 0.25f * static_cast<float>(i);
        const double expected = (static_cast<double>(pitch) - 9.0) / 120.0;
        CAPTURE(pitch);
        // One rounding for the division, half an ulp for the reference.
        CHECK_THAT(static_cast<double>(keytrackSource(pitch)), WithinRel(expected, twoRoundings));
        // The same quantity: keytrack 10 on the filter is ten times the source,
        // in octaves (each side rounded twice).
        CHECK_THAT(static_cast<double>(keytrackOctaves(pitch, 1.0f)),
                   WithinRel(10.0 * static_cast<double>(keytrackSource(pitch)), 2.0 * twoRoundings));
    }
}

TEST_CASE("Keytrack 10: a self-oscillating filter plays the keyboard's intervals", "[dsp][filter][keytrack]")
{
    // The cutoff knob at A-1's frequency, 13.75 Hz: at keytrack 10 every note
    // puts the cutoff at its own frequency, and the filter sounds that, flat
    // by the k 4.4 detune (filters.md: -35.78 cents with tanh, -32.33 with
    // the cubic, pinned there within 0.2). The detune is flat across the
    // cutoff: the report measures C1-C7 within 0.08-0.36 cents up to 2 kHz at
    // k 4.05-4.4. Bound: 0.5 cents for each note against the pinned detune
    // (0.2 for the pin, 0.3 for the flatness), a tenth of an audible 5 cents.
    // A reference at A4 would put A4 at 13.75 Hz; tracking in Hz, or at 1.1:1,
    // would bend the intervals by tens of cents.
    for (const bool warm : {true, false})
    {
        const double detune = warm ? -35.78 : -32.33;
        double lo = std::numeric_limits<double>::infinity();
        double hi = -lo;
        for (const double note : {36.0, 48.0, 60.0, 69.0, 72.0, 84.0})
        {
            const kt::Setup s{.rate = 48000.0, .warm = warm, .k = 4.4f, .cutoffHz = dsp::noteToFrequency(9.0f)};
            const double cents = kt::centsBetween(selfOscillationHz(s, note), kt::noteHz(note));
            CAPTURE(warm, note, cents);
            CHECK_THAT(cents, WithinAbs(detune, 0.5));
            lo = std::min(lo, cents);
            hi = std::max(hi, cents);
        }
        CHECK(hi - lo < 0.5);
    }
}

TEST_CASE("Keytrack 5 tracks half as many octaves from A-1", "[dsp][filter][keytrack]")
{
    // A4 at keytrack 5: 2.5 octaves above the 13.75 Hz cutoff, 77.78 Hz, flat
    // by the k 4.4 detune (as above, 0.5 cents).
    const kt::Setup s{.rate = 48000.0, .k = 4.4f, .cutoffHz = 13.75, .amount = 0.5};
    const double expected = 13.75 * std::exp2(2.5);
    CHECK_THAT(kt::centsBetween(selfOscillationHz(s, 69.0, 4.0), expected), WithinAbs(-35.78, 0.5));
}

TEST_CASE("Keytrack follows the voice's pitch: a glide, a bend, the master tune", "[dsp][filter][keytrack]")
{
    constexpr double rate = 48000.0;
    constexpr std::size_t eventAt = 72000; // 1.5 s: the oscillation has settled
    const kt::Setup s{.rate = rate, .k = 4.4f};

    SECTION("a 100 ms octave glide")
    {
        // The report: 3.2 cents at worst, 0.6 RMS (the oscillation's own
        // response to a moving cutoff). Feeding the pitch at the grid point
        // instead of the segment's end lags 32 samples: 8 cents on this glide
        // (1200 cents x 32 / 4800), measured 7.9 / 6.0. Bounds 5 and 1.5
        // pass the first and fail the second.
        const kt::Glide glide{.from = 48.0, .to = 60.0, .start = eventAt, .samples = 4800};
        const kt::PitchTrack pitch = [=](std::size_t n)
        {
            return glide.noteAt(n);
        };
        const auto t = kt::tracking(s, pitch, eventAt, eventAt + 7200);
        CAPTURE(t.maxCents, t.rmsCents);
        CHECK(t.maxCents < 5.0);
        CHECK(t.rmsCents < 1.5);
    }
    SECTION("a bend of +-2 semitones at 2 Hz")
    {
        // The report: 0.1 cents. The one-segment lag would be 1.7 (25
        // semitones per second x 0.67 ms). Bound 0.5.
        const kt::PitchTrack pitch = [=](std::size_t n)
        {
            const double t = (static_cast<double>(n) - static_cast<double>(eventAt)) / rate;
            return 57.0 + ((t >= 0.0 && t < 1.0) ? 2.0 * std::sin(2.0 * std::numbers::pi * 2.0 * t) : 0.0);
        };
        const auto t = kt::tracking(s, pitch, eventAt, eventAt + 48000);
        CAPTURE(t.maxCents, t.rmsCents);
        CHECK(t.maxCents < 0.5);
    }
    SECTION("the master tune")
    {
        // The voice adds masterTuneSemitones() to its pitch, so the filter
        // moves with the oscillators: its offset from them is the detune
        // alone at 432 and 444 Hz as at 440 (the report: equal to 0.1 cents;
        // ignoring the master tune leaves it 31.8 and 15.7 cents away).
        // Bound 0.1 cents.
        const auto offset = [&](float tuneHz)
        {
            const double note = 69.0 + static_cast<double>(dsp::masterTuneSemitones(tuneHz));
            const auto oscillators = static_cast<double>(dsp::noteToFrequency(69.0f, tuneHz));
            return kt::centsBetween(selfOscillationHz(s, note), oscillators);
        };
        const double at440 = offset(440.0f);
        CHECK_THAT(offset(432.0f), WithinAbs(at440, 0.1));
        CHECK_THAT(offset(444.0f), WithinAbs(at440, 0.1));
    }
}

TEST_CASE("Keytrack fed at the grid: bit-identical across block sizes, no allocation", "[dsp][filter][keytrack]")
{
    // Legato keys with a 60 ms glide, the wheel moving, master tune 438 Hz,
    // a saw at k 3.9 and drive 2: each grid point's value is a pure function
    // of the pitch there, so the host's block size can't change it.
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 32000;
    constexpr std::array<double, 8> keys{48.0, 55.0, 67.0, 43.0, 72.0, 60.0, 64.0, 52.0};
    const auto tune = static_cast<double>(dsp::masterTuneSemitones(438.0f));
    const kt::PitchTrack pitch = [=](std::size_t i)
    {
        const std::size_t step = std::min<std::size_t>(i / 4000, keys.size() - 1);
        const kt::Glide g{
            .from = step == 0 ? keys[0] : keys[step - 1],
            .to = keys[step],
            .start = step * 4000,
            .samples = 2880,
        };
        return g.noteAt(i) + (0.7 * std::sin(2.0 * std::numbers::pi * 3.1 * static_cast<double>(i) / rate)) + tune;
    };
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double phase = std::fmod(110.0 * static_cast<double>(i) / rate, 1.0);
        x[i] = static_cast<float>(phase - 0.5);
    }
    const auto at = [&](std::size_t hostBlock)
    {
        const kt::Setup s{.rate = rate, .k = 3.9f, .drive = 2.0f, .cutoffHz = 13.75, .hostBlock = hostBlock};
        return kt::run(s, x, pitch);
    };
    const auto reference = at(4096);
    CHECK(reference.allocations == 0);
    for (const std::size_t size : {std::size_t{1}, std::size_t{37}})
    {
        const auto y = at(size).y;
        CAPTURE(size);
        CHECK(std::ranges::equal(y, reference.y, [](float p, float q)
                                 { return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q); }));
    }
}

TEST_CASE("Keytrack: a NaN pitch is no modulation", "[dsp][filter][keytrack]")
{
    // NaN propagates through the offset; the filter treats a NaN modulation
    // as 0, so the output is the filter with no keytrack, bit for bit.
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(std::isnan(keytrackOctaves(nan, 1.0f)));
    CHECK(std::isnan(keytrackSource(nan)));
    const auto x = kt::kick(4800);
    const kt::Setup s{.k = 3.0f, .cutoffHz = 1000.0};
    const auto withNan = kt::run(s, x, [](std::size_t) { return std::numeric_limits<double>::quiet_NaN(); }).y;
    const auto none = kt::run(s, x, [](std::size_t) { return 9.0; }).y; // at the reference: 0 octaves
    CHECK(std::ranges::all_of(withNan, [](float v) { return std::isfinite(v); }));
    CHECK(std::ranges::equal(withNan, none, [](float p, float q)
                             { return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q); }));
}
