// Tests for the Filter Control's combining law (filter_control.hpp,
// docs/dsp/filter-control.md): knob units added through one exponential law
// at an octave per unit from 4.5675 Hz; each control plus its own modulation
// clamped to [0, 10], then added, the sum unclamped; keytrack after the
// clamps, unclamped, and the same number as keytrackOctaves; resonance added
// the same way, k = 0.6 per unit, 0 to 12; where the DSP's ceiling takes
// over. Then on the real LadderFilter, fed as the voice will feed it
// (filter_control_support.hpp): the -12 dB point, the self-oscillation
// threshold, the voice's ramp against the filter's, a clamp during a ramp,
// each voice reading the global ramp at its own grid, block sizes,
// allocation.
//
// Tolerances are derived in each test from float rounding or from the
// filter's own pinned bounds (ladder_filter_test.cpp).

#include "filter_control_support.hpp"

#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>

#include <anoesis/test/signals.hpp>

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
#include <utility>
#include <vector>

using anoesis::dsp::clampToKnob;
using anoesis::dsp::FilterControlLaw;
using anoesis::dsp::filterControlLaw;
using anoesis::dsp::filterCutoffOctaves;
using anoesis::dsp::FilterCutoffTerms;
using anoesis::dsp::filterCutoffUnits;
using anoesis::dsp::filterResonanceK;
using anoesis::dsp::filterResonanceUnits;
using anoesis::dsp::keytrackOctaves;
using anoesis::dsp::keytrackSource;
using anoesis::dsp::LadderFilter;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

namespace
{

namespace fc = anoesis::test::filter_control;
constexpr std::size_t grid = LadderFilter::gridSamples;

/// One float rounding, relative: half an ulp, 2^-24.
constexpr double oneRounding = 0x1p-24;

/// The cutoff LadderFilter uses for a law output, as Hz in double: the
/// anchor times 2^octaves, then the DSP's [1 Hz, 0.495 fs].
double lawHz(double octaves, double rate)
{
    const double hz = static_cast<double>(filterControlLaw.anchorHz) * std::exp2(octaves);
    return std::clamp(hz, 1.0, static_cast<double>(LadderFilter::maxCutoffRatio * static_cast<float>(rate)));
}

/// A settled filter fed as the voice feeds it: the anchor held in
/// setCutoff(), the whole cutoff in the modulation input.
float fedCutoff(float octaves, double rate)
{
    return fc::fedFilter(octaves, rate).currentCutoff();
}

/// Float rounding in log2 of the cutoff along a ramp, counted per path in
/// units of half an ulp at [16, 32) octaves, 2^-20 octaves (0.0011 cents).
/// The filter's own ramp (W1): exp2 and the anchor's product in the knobs'
/// Hz (0.3), log2 in setCutoff (1), the ramp's step over at most 20 octaves
/// (1.25), its product and sum (1.5), exp2 in the filter (0.2): 4.25. The
/// voice's (W2): log2 of the anchor (0.25), each knob ramp's step, product,
/// and sum (1.6 each), the knobs' sum and keytrack's (2), the anchor plus the
/// octaves (0.5), exp2 (0.2): 6.15. Between the two, or either and the exact
/// ramp in double, at most 12 units.
constexpr double rampRoundingOctaves = 12.0 * 0x1p-20;

/// The RMS of y over cycles [from, to) of a period in samples
/// (ladder_filter_test.cpp's threshold measure).
double cycleRms(std::span<const float> y, double period, double from, double to)
{
    const auto a = static_cast<std::size_t>(from * period);
    const auto b = static_cast<std::size_t>(to * period);
    double sum = 0.0;
    for (std::size_t i = a; i < b; ++i)
    {
        sum += static_cast<double>(y[i]) * static_cast<double>(y[i]);
    }
    return std::sqrt(sum / static_cast<double>(b - a));
}

bool bitIdentical(std::span<const float> a, std::span<const float> b)
{
    return std::ranges::equal(a, b, [](float p, float q)
                              { return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q); });
}

/// A knob's 10 ms ramp from `from` to `to` starting at sample `at`, in
/// closed form, at sample n.
double rampAt(double from, double to, std::size_t at, std::size_t n, double rate)
{
    const double length = std::round(static_cast<double>(LadderFilter::rampSeconds) * rate);
    const double t = std::clamp((static_cast<double>(n) - static_cast<double>(at)) / length, 0.0, 1.0);
    return from + ((to - from) * t);
}

/// The filter's log2 of the cutoff against the law in double: log2 of the
/// anchor (about 2.19) within one ulp there, 2^-22 octaves; the sum with the
/// octaves (below 32) within half an ulp, 2^-20; exp2 within one ulp, 2^-23
/// relative. Together under 2^-19 relative.
constexpr double filterRounding = 0x1p-19;

} // namespace

TEST_CASE("Filter Control law: 4.5675 Hz at S = 0, 146.16 Hz for one knob at 5, 4,677 Hz at the defaults",
          "[dsp][filter][filter-control]")
{
    // Evaluated at compile time: constexpr, no state.
    constexpr float bottom = filterCutoffOctaves({});
    constexpr float oneKnob = filterCutoffOctaves({.cutoff = 5.0f});
    constexpr float otherKnob = filterCutoffOctaves({.controlCutoff = 5.0f});
    constexpr float defaults = filterCutoffOctaves({.cutoff = 5.0f, .controlCutoff = 5.0f});
    constexpr float tops = filterCutoffOctaves({.cutoff = 10.0f, .controlCutoff = 10.0f});
    CHECK_THAT(bottom, WithinULP(0.0f, 0));
    CHECK_THAT(oneKnob, WithinULP(5.0f, 0));
    CHECK_THAT(otherKnob, WithinULP(5.0f, 0));
    CHECK_THAT(defaults, WithinULP(10.0f, 0));
    CHECK_THAT(tops, WithinULP(20.0f, 0));
    CHECK_THAT(filterCutoffUnits({.cutoff = 5.0f, .controlCutoff = 5.0f}), WithinULP(10.0f, 0));
    // In static_assert, too (compared without ==, which -Wfloat-equal flags);
    // and none of it can throw.
    STATIC_CHECK(defaults >= 10.0f);
    STATIC_CHECK(defaults <= 10.0f);
    STATIC_CHECK(filterCutoffUnits({.cutoff = 9.0f, .cutoffModulation = 3.0f, .keytrack = 1.0f}) >= 11.0f);
    STATIC_CHECK(filterCutoffUnits({.cutoff = 9.0f, .cutoffModulation = 3.0f, .keytrack = 1.0f}) <= 11.0f);
    STATIC_CHECK(filterResonanceUnits({.resonance = 12.0f, .controlResonance = -1.0f}) >= 10.0f);
    STATIC_CHECK(filterResonanceUnits({.resonance = 12.0f, .controlResonance = -1.0f}) <= 10.0f);
    STATIC_CHECK(noexcept(filterCutoffOctaves({})));
    STATIC_CHECK(noexcept(filterResonanceK({})));
    STATIC_CHECK(noexcept(clampToKnob(0.0f)));

    // The law's numbers. The anchor is 146.16 Hz five octaves down, exactly
    // (a power-of-two scale keeps the nearest float).
    CHECK_THAT(filterControlLaw.anchorHz, WithinULP(4.5675f, 0));
    CHECK_THAT(filterControlLaw.anchorHz * 32.0f, WithinULP(146.16f, 0));
    CHECK_THAT(filterControlLaw.octavesPerUnit, WithinULP(1.0f, 0));
    CHECK_THAT(filterControlLaw.knobMin, WithinULP(0.0f, 0));
    CHECK_THAT(filterControlLaw.knobMax, WithinULP(10.0f, 0));
    CHECK_THAT(filterControlLaw.kPerUnit, WithinULP(0.6f, 0));

    // In Hz, on the filter (44.1 kHz): 4.5675, 146.16, 4,677.12; the
    // anchor's own float rounding is one more 2^-24.
    for (const auto [octaves, hz] :
         {std::pair{bottom, 4.5675}, std::pair{oneKnob, 146.16}, std::pair{defaults, 4677.12}})
    {
        CAPTURE(octaves);
        CHECK_THAT(static_cast<double>(fedCutoff(octaves, 44100.0)), WithinRel(hz, filterRounding + oneRounding));
    }
}

TEST_CASE("Filter Control law: each control plus its own modulation is clamped to the knob's range, the sum is not",
          "[dsp][filter][filter-control]")
{
    // Modulation past a control's own end is lost, even with room in the
    // other: per control, not the sum to [0, 20].
    CHECK_THAT(filterCutoffUnits({.cutoff = 8.0f, .cutoffModulation = 5.0f, .controlCutoff = 5.0f}),
               WithinULP(15.0f, 0));
    CHECK_THAT(filterCutoffUnits({.cutoff = 9.0f, .cutoffModulation = 3.0f}), WithinULP(10.0f, 0));
    CHECK_THAT(filterCutoffUnits({.cutoff = 2.0f, .cutoffModulation = -5.0f, .controlCutoff = 5.0f}),
               WithinULP(5.0f, 0));
    CHECK_THAT(filterCutoffUnits({.cutoffModulation = -4.0f, .controlCutoff = 5.0f}), WithinULP(5.0f, 0));
    // The Filter Control with Cutoff AB, the same way.
    CHECK_THAT(filterCutoffUnits({.cutoff = 5.0f, .controlCutoff = 8.0f, .controlModulation = 5.0f}),
               WithinULP(15.0f, 0));
    CHECK_THAT(filterCutoffUnits({.cutoff = 5.0f, .controlCutoff = 1.0f, .controlModulation = -3.0f}),
               WithinULP(5.0f, 0));
    // Each control's modulation is its own: Cutoff AB does not reach into
    // filter X's room, nor the other way round.
    CHECK_THAT(filterCutoffUnits({.cutoff = 10.0f, .controlCutoff = 0.0f, .controlModulation = 4.0f}),
               WithinULP(14.0f, 0));
    CHECK_THAT(filterCutoffUnits({.cutoff = 10.0f, .cutoffModulation = 4.0f, .controlCutoff = 0.0f}),
               WithinULP(10.0f, 0));
    // Modulation brings a control at its end back into range.
    CHECK_THAT(filterCutoffUnits({.cutoff = 10.0f, .cutoffModulation = -3.0f}), WithinULP(7.0f, 0));
    // Every slot at full depth (16 x 10 units) on both: the sum spans 0-20,
    // not clamped to [0, 10].
    CHECK_THAT(filterCutoffUnits(
                   {.cutoff = 10.0f, .cutoffModulation = 160.0f, .controlCutoff = 10.0f, .controlModulation = 160.0f}),
               WithinULP(20.0f, 0));
    CHECK_THAT(
        filterCutoffUnits(
            {.cutoff = 10.0f, .cutoffModulation = -160.0f, .controlCutoff = 10.0f, .controlModulation = -160.0f}),
        WithinULP(0.0f, 0));
    // The clamp itself, and the range as a parameter.
    CHECK_THAT(clampToKnob(-0.5f), WithinULP(0.0f, 0));
    CHECK_THAT(clampToKnob(10.5f), WithinULP(10.0f, 0));
    CHECK_THAT(clampToKnob(3.25f), WithinULP(3.25f, 0));
    constexpr FilterControlLaw unitKnobs{.knobMin = -1.0f, .knobMax = 1.0f};
    CHECK_THAT(clampToKnob(1.5f, unitKnobs), WithinULP(1.0f, 0));
    CHECK_THAT(clampToKnob(-1.5f, unitKnobs), WithinULP(-1.0f, 0));
    CHECK_THAT(filterCutoffUnits({.cutoff = 0.75f, .cutoffModulation = 0.5f, .controlCutoff = -2.0f}, unitKnobs),
               WithinULP(0.0f, 0));
}

TEST_CASE("Filter Control law: keytrack goes in after the clamps, unclamped, as keytrackOctaves exactly",
          "[dsp][filter][filter-control]")
{
    // After the clamps: a filter at its top still rises with the note, and a
    // closed one falls below the bottom.
    CHECK_THAT(filterCutoffUnits({.cutoff = 9.0f, .keytrack = 3.0f}), WithinULP(12.0f, 0));
    CHECK_THAT(filterCutoffUnits({.cutoff = 10.0f, .cutoffModulation = 5.0f, .controlCutoff = 10.0f, .keytrack = 3.0f}),
               WithinULP(23.0f, 0));
    CHECK_THAT(filterCutoffUnits({.keytrack = -0.75f}), WithinULP(-0.75f, 0));
    // Keytrack 10 over MIDI 0-127 with both knobs at the extremes.
    CHECK_THAT(filterCutoffOctaves({.keytrack = keytrackOctaves(0.0f, 1.0f)}), WithinULP(-0.75f, 0));
    CHECK_THAT(
        filterCutoffOctaves({.cutoff = 10.0f, .controlCutoff = 10.0f, .keytrack = keytrackOctaves(129.0f, 1.0f)}),
        WithinULP(30.0f, 0));

    // At one octave per unit the knob-unit term is keytrackOctaves' number,
    // bit for bit, at every pitch and amount: keytrack 10 tracks 1:1 and the
    // keytrack round's function needs no change.
    for (int i = -96; i <= 600; ++i)
    {
        const auto pitch = 0.25f * static_cast<float>(i); // quarter tones, note -24 to 150
        for (const float keytrack : {0.0f, 1.0f, 2.5f, 5.0f, 7.3f, 10.0f})
        {
            CAPTURE(pitch, keytrack);
            const float term = keytrackOctaves(pitch, keytrack / 10.0f);
            CHECK_THAT(filterCutoffOctaves({.keytrack = term}), WithinULP(term, 0));
            // And it is the developer's misc source times the knob,
            // Keytrack x (pitch - 9) / 120, to float rounding: three roundings
            // on one side (Keytrack / 10, the division, the product), two on
            // the other.
            CHECK_THAT(static_cast<double>(term),
                       WithinRel(static_cast<double>(keytrack * keytrackSource(pitch)), 5.0 * oneRounding));
        }
    }
}

TEST_CASE("Filter Control law: resonance adds in knob units, each control clamped, k from 0 to 12",
          "[dsp][filter][filter-control]")
{
    constexpr float none = filterResonanceK({});
    constexpr float both = filterResonanceK({.resonance = 10.0f, .controlResonance = 10.0f});
    CHECK_THAT(none, WithinULP(0.0f, 0));
    // 0.6f x 20 rounds once.
    CHECK_THAT(static_cast<double>(both), WithinRel(12.0, 2.0 * oneRounding));
    CHECK_THAT(static_cast<double>(filterResonanceK({.resonance = 10.0f})), WithinRel(6.0, 2.0 * oneRounding));
    CHECK_THAT(static_cast<double>(filterResonanceK({.controlResonance = 5.0f})), WithinRel(3.0, 2.0 * oneRounding));
    CHECK_THAT(filterResonanceUnits({.resonance = 3.0f, .controlResonance = 4.5f}), WithinULP(7.5f, 0));

    // Self-oscillation (k = 4) from a sum of 20/3: Resonance A alone at 6.67,
    // or at 1.67 with the Filter Control at 5.
    CHECK_THAT(static_cast<double>(filterResonanceK({.resonance = 20.0f / 3.0f})), WithinRel(4.0, 3.0 * oneRounding));
    CHECK_THAT(static_cast<double>(filterResonanceK({.resonance = 5.0f / 3.0f, .controlResonance = 5.0f})),
               WithinRel(4.0, 4.0 * oneRounding));

    // Each control plus its own modulation to [0, 10].
    CHECK_THAT(filterResonanceUnits({.resonance = 8.0f, .resonanceModulation = 5.0f, .controlResonance = 2.0f}),
               WithinULP(12.0f, 0));
    CHECK_THAT(filterResonanceUnits({.resonance = 2.0f, .resonanceModulation = -5.0f, .controlResonance = 2.0f}),
               WithinULP(2.0f, 0));
    CHECK_THAT(filterResonanceUnits({.resonance = 2.0f, .controlResonance = 9.0f, .controlModulation = 4.0f}),
               WithinULP(12.0f, 0));
    CHECK_THAT(filterResonanceUnits({.resonance = 2.0f, .controlResonance = 1.0f, .controlModulation = -4.0f}),
               WithinULP(2.0f, 0));
    // Every slot at full depth: k stays within 0-12.
    CHECK_THAT(static_cast<double>(filterResonanceK({.resonance = 10.0f,
                                                     .resonanceModulation = 160.0f,
                                                     .controlResonance = 10.0f,
                                                     .controlModulation = 160.0f})),
               WithinRel(12.0, 2.0 * oneRounding));
    CHECK_THAT(filterResonanceK({.resonance = 10.0f,
                                 .resonanceModulation = -160.0f,
                                 .controlResonance = 10.0f,
                                 .controlModulation = -160.0f}),
               WithinULP(0.0f, 0));
    // k per unit is a parameter.
    constexpr FilterControlLaw steeper{.kPerUnit = 1.2f};
    CHECK_THAT(static_cast<double>(filterResonanceK({.resonance = 10.0f, .controlResonance = 10.0f}, steeper)),
               WithinRel(24.0, 2.0 * oneRounding));
}

TEST_CASE("Filter Control law: the anchor and octaves per unit are parameters", "[dsp][filter][filter-control]")
{
    // F3's 1.1 octaves per unit: the same sum, 1.1 times the octaves, and
    // keytrack scaled with it (1320 cents per octave at keytrack 10: why the
    // decision is one octave per unit).
    constexpr FilterControlLaw f3{.anchorHz = 10.0f, .octavesPerUnit = 1.1f};
    CHECK_THAT(static_cast<double>(filterCutoffOctaves({.cutoff = 5.0f, .controlCutoff = 5.0f}, f3)),
               WithinRel(11.0, oneRounding));
    CHECK_THAT(static_cast<double>(filterCutoffOctaves({.keytrack = 1.0f}, f3)), WithinRel(1.1, oneRounding));
    // The units don't depend on the law's slope or anchor.
    CHECK_THAT(filterCutoffUnits({.cutoff = 5.0f, .controlCutoff = 5.0f}, f3), WithinULP(10.0f, 0));
}

TEST_CASE("Filter Control law: where the DSP's ceiling takes over, at 44.1, 48, 96, and 192 kHz",
          "[dsp][filter][filter-control]")
{
    // S* = log2(0.495 fs / 4.5675 Hz): 12.22, 12.34, 13.34, 14.34 (the
    // stage-1 figures, two decimals). Above it the filter holds 0.495 fs;
    // below it the filter is the law. 0.01 unit either side is 12 cents,
    // far outside the rounding (filterRounding, about 0.002 cents).
    for (const auto [rate, printed] :
         {std::pair{44100.0, 12.22}, std::pair{48000.0, 12.34}, std::pair{96000.0, 13.34}, std::pair{192000.0, 14.34}})
    {
        CAPTURE(rate);
        const auto ceiling = static_cast<double>(LadderFilter::maxCutoffRatio * static_cast<float>(rate));
        const double sStar = std::log2(ceiling / static_cast<double>(filterControlLaw.anchorHz));
        CHECK_THAT(sStar, WithinAbs(printed, 0.005));

        const auto below = static_cast<float>(sStar - 0.01);
        const auto above = static_cast<float>(sStar + 0.01);
        const float hzBelow = fedCutoff(filterCutoffOctaves({.cutoff = 10.0f, .controlCutoff = below - 10.0f}), rate);
        CHECK(static_cast<double>(hzBelow) < ceiling);
        CHECK_THAT(static_cast<double>(hzBelow),
                   WithinRel(lawHz(static_cast<double>(below), rate), filterRounding + oneRounding));
        const float hzAbove = fedCutoff(filterCutoffOctaves({.cutoff = 10.0f, .controlCutoff = above - 10.0f}), rate);
        CHECK_THAT(hzAbove, WithinULP(LadderFilter::maxCutoffRatio * static_cast<float>(rate), 0));
    }
}

TEST_CASE("Filter Control law: NaN in gives NaN out", "[dsp][filter][filter-control]")
{
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(std::isnan(clampToKnob(nan)));
    CHECK(std::isnan(filterCutoffOctaves({.cutoff = 5.0f, .cutoffModulation = nan})));
    CHECK(std::isnan(filterCutoffOctaves({.cutoff = 5.0f, .controlModulation = nan})));
    CHECK(std::isnan(filterCutoffOctaves({.cutoff = 5.0f, .keytrack = nan})));
    CHECK(std::isnan(filterResonanceK({.resonance = 5.0f, .resonanceModulation = nan})));
    CHECK(std::isnan(filterResonanceK({.resonance = 5.0f, .controlModulation = nan})));
}

TEST_CASE("Filter Control on the filter: the -12 dB point is the law", "[dsp][filter][filter-control]")
{
    // The prewarped ladder puts LP24's -12.04 dB point (k 0) exactly at the
    // cutoff. Fed as the voice feeds it, the point measured on the impulse
    // response must be the law after the DSP's clamp. Tolerance (stage 1's):
    // the filter tests pin the small-signal response to 4e-5 of its peak; at
    // the cutoff |H| = 1/4, so 1.6e-4 relative, 0.0014 dB, and LP24 falls
    // 12.04 dB per octave there: 0.14 cents. The law's own float rounding
    // (a few 2^-20 octaves, under 0.01 cents) is inside it.
    const double toleranceCents = 1200.0 * (20.0 * std::log10(1.0 + (4e-5 / 0.25))) / 12.0412;
    const std::array<FilterCutoffTerms, 10> cases{
        FilterCutoffTerms{.cutoff = 0.0f},                          // S 0: 4.57 Hz
        FilterCutoffTerms{.cutoff = 2.5f, .controlCutoff = 2.5f},   // 146.2 Hz
        FilterCutoffTerms{.cutoff = 5.0f, .controlCutoff = 5.0f},   // the defaults: 4,677 Hz
        FilterCutoffTerms{.cutoff = 7.5f, .controlCutoff = 2.5f},   // the same sum
        FilterCutoffTerms{.cutoff = 10.0f, .controlCutoff = 1.5f},  // 11.5: under the ceiling everywhere
        FilterCutoffTerms{.cutoff = 5.0f, .controlCutoff = 7.5f},   // 12.5: the ceiling at 44.1, not at 96
        FilterCutoffTerms{.cutoff = 10.0f, .controlCutoff = 10.0f}, // 20: the ceiling
        FilterCutoffTerms{.cutoff = 8.0f, .cutoffModulation = 5.0f, .controlCutoff = 1.0f}, // clamped: 11
        FilterCutoffTerms{.cutoff = 3.0f, .keytrack = keytrackOctaves(96.0f, 1.0f)},        // keytrack 10 at C7: 10.25
        FilterCutoffTerms{.keytrack = keytrackOctaves(0.0f, 1.0f)}, // keytrack below the bottom: 2.72 Hz
    };
    for (const double rate : {44100.0, 96000.0})
    {
        for (const auto& terms : cases)
        {
            const float octaves = filterCutoffOctaves(terms);
            const double expected = lawHz(static_cast<double>(octaves), rate);
            CAPTURE(rate, octaves, expected);
            const double measured = fc::minus12DbPoint(fc::fedFilter(octaves, rate), expected, rate);
            CHECK(std::fabs(1200.0 * std::log2(measured / expected)) <= toleranceCents);
        }
    }
}

TEST_CASE("Filter Control on the filter: self-oscillation from a resonance sum of 6.67, either knob",
          "[dsp][filter][filter-control]")
{
    // k = 0.6 x the sum, and the ladder self-oscillates from k = 4: a sum of
    // 20/3. The filter tests pin the threshold between k 3.99 (decays) and
    // 4.01 (grows) by the RMS of cycles 35-40 against 10-15 from a 1e-15
    // impulse; in knob units that is 1/60 either side: 6.65 and 6.6833. The
    // same with Resonance A alone and at 1.65 / 1.6833 with the Filter
    // Control's at 5, fed through the voice (the resonance modulation input).
    constexpr double rate = 48000.0;
    constexpr double cutoff = 1760.0;
    const double period = rate / cutoff;
    std::vector<float> kick(static_cast<std::size_t>(40.0 * period) + 1, 0.0f);
    kick[0] = 1e-15f;
    // Cutoff A + the Filter Control's Cutoff at log2(1760 / 4.5675).
    const auto sum = static_cast<float>(std::log2(cutoff / static_cast<double>(filterControlLaw.anchorHz)));
    const fc::Voice still;
    for (const bool warm : {true, false})
    {
        for (const float control : {0.0f, 5.0f})
        {
            const auto grows = [&](float resonance)
            {
                const fc::Knobs knobs{
                    .cutoff = fc::constant(sum - 4.0f),
                    .controlCutoff = fc::constant(4.0f),
                    .resonance = fc::constant(resonance),
                    .controlResonance = fc::constant(control),
                };
                const auto r = fc::runOne({.rate = rate, .warm = warm}, knobs, still, kick);
                return cycleRms(r.y, period, 35.0, 40.0) > cycleRms(r.y, period, 10.0, 15.0);
            };
            CAPTURE(warm, control);
            const float threshold = (20.0f / 3.0f) - control;
            CHECK_FALSE(grows(threshold - (1.0f / 60.0f)));
            CHECK(grows(threshold + (1.0f / 60.0f)));
        }
    }
}

TEST_CASE("Filter Control on the filter: the voice's ramp is the filter's wherever no clamp bites",
          "[dsp][filter][filter-control]")
{
    // W2 (the knobs' global ramps in the voice's sum, everything through the
    // modulation input) against W1 (the knobs' sum through setCutoff(), the
    // filter's own 10 ms log2 ramp), events on the grid: the cutoff at every
    // grid point the same to float rounding (rampRoundingOctaves, 0.014
    // cents). A Filter Control step 3 -> 6 (A 5), and a 7-bit CC sweep 0 ->
    // 10 in host blocks of 512 (A 3). No ramp at all (W4) misses by tens of
    // cents.
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 49152;
    constexpr std::size_t at = 24576;
    const auto x = anoesis::test::sine(150.0, rate, n, 0.25);
    const fc::Signal step = [](std::size_t i)
    {
        return i < at ? 3.0f : 6.0f;
    };
    const fc::Signal cc = [](std::size_t i)
    {
        const double t = std::min(1.0, static_cast<double>(i - (i % 512)) / static_cast<double>(n));
        return static_cast<float>(std::round(127.0 * t) / 127.0 * 10.0);
    };
    const fc::Voice still;
    for (const auto& [a, c] : {std::pair{5.0f, step}, std::pair{3.0f, cc}})
    {
        const fc::Knobs knobs{.cutoff = fc::constant(a), .controlCutoff = c, .resonance = fc::constant(6.0f)};
        const auto w1 = fc::runOne({.rate = rate, .feed = fc::Feed::filterRamp}, knobs, still, x);
        const auto w2 = fc::runOne({.rate = rate, .feed = fc::Feed::voiceRamp}, knobs, still, x);
        const auto w4 = fc::runOne({.rate = rate, .feed = fc::Feed::noRamp}, knobs, still, x);
        REQUIRE(w1.log2Cutoff.size() == w2.log2Cutoff.size());
        double worst = 0.0;
        double unramped = 0.0;
        for (std::size_t i = 0; i < w1.log2Cutoff.size(); ++i)
        {
            worst = std::max(worst, std::fabs(w2.log2Cutoff[i] - w1.log2Cutoff[i]));
            unramped = std::max(unramped, std::fabs(w4.log2Cutoff[i] - w1.log2Cutoff[i]));
        }
        CAPTURE(a, worst * 1200.0, unramped * 1200.0);
        CHECK(worst <= rampRoundingOctaves);
        CHECK(unramped > 0.02); // 24 cents
    }
}

TEST_CASE("Filter Control on the filter: exact where a clamp bites during a ramp", "[dsp][filter][filter-control]")
{
    // Cutoff A 2 -> 9 with +3 of modulation on Cutoff A and the Filter Control
    // at 2: each control clamped, so the sum runs 7 -> 12 and stops when the
    // ramping A passes 7. The voice's sum is the clamp of the ramped knob at
    // every grid point (the ramp in closed form, in double), to float
    // rounding. The filter's ramp can only clamp against the target, so its
    // modulation part steps at once: two octaves off (stage 1: 2,400 cents).
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 32768;
    constexpr std::size_t at = 16384;
    const auto x = anoesis::test::sine(150.0, rate, n, 0.25);
    const fc::Knobs knobs{
        .cutoff = [](std::size_t i) { return i < at ? 2.0f : 9.0f; },
        .controlCutoff = fc::constant(2.0f),
        .resonance = fc::constant(6.0f),
    };
    const fc::Voice voice{.cutoffModulation = fc::constant(3.0f)};
    const double log2Anchor = std::log2(static_cast<double>(filterControlLaw.anchorHz));
    const auto worstFromExact = [&](fc::Feed feed)
    {
        const auto r = fc::runOne({.rate = rate, .feed = feed}, knobs, voice, x);
        double worst = 0.0;
        for (std::size_t i = 0; i < r.log2Cutoff.size(); ++i)
        {
            const double a = rampAt(2.0, 9.0, at, (i + 1) * grid, rate);
            const double exact = log2Anchor + std::min(10.0, a + 3.0) + 2.0;
            worst = std::max(worst, std::fabs(r.log2Cutoff[i] - exact));
        }
        return worst;
    };
    CHECK(worstFromExact(fc::Feed::voiceRamp) <= rampRoundingOctaves);
    CHECK(worstFromExact(fc::Feed::filterRamp) > 1.0); // an octave
}

TEST_CASE("Filter Control on the filter: each voice reads the global ramp at its own grid points",
          "[dsp][filter][filter-control]")
{
    // Voices' grids count from their own note-ons, so they are not aligned.
    // One global ramp, started at its event's sample (here 1000, on neither
    // grid), read in closed form: each voice's cutoff at the end of each of
    // its segments is the ramp there, to float rounding. The one exception is
    // the segment that holds the event: its end was computed at its grid
    // point, before the event, so it keeps the old value (the filter latches
    // a segment's end at its start; W1 is as late there, then lags for the
    // whole ramp). And a voice's output does not depend on the other voices.
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 4096;
    constexpr std::size_t at = 1000;
    const auto x = anoesis::test::sine(150.0, rate, n, 0.25);
    const fc::Knobs knobs{
        .cutoff = fc::constant(5.0f),
        .controlCutoff = [](std::size_t i) { return i < at ? 3.0f : 6.0f; },
        .resonance = fc::constant(3.0f),
    };
    const std::array<fc::Voice, 2> voices{fc::Voice{.noteOn = 0}, fc::Voice{.noteOn = 13}};
    const auto both = fc::run({.rate = rate, .hostBlock = 256}, knobs, voices, x);
    const double log2Anchor = std::log2(static_cast<double>(filterControlLaw.anchorHz));
    for (std::size_t v = 0; v < voices.size(); ++v)
    {
        const auto& r = both.voices[v];
        double worst = 0.0;
        for (std::size_t i = 0; i < r.log2Cutoff.size(); ++i)
        {
            const auto end = voices[v].noteOn + ((i + 1) * grid);
            const double control = end - grid < at ? 3.0 : rampAt(3.0, 6.0, at, end, rate);
            worst = std::max(worst, std::fabs(r.log2Cutoff[i] - (log2Anchor + 5.0 + control)));
        }
        CAPTURE(v, worst * 1200.0);
        CHECK(worst <= rampRoundingOctaves);
        const auto alone = fc::run({.rate = rate, .hostBlock = 256}, knobs, std::span{&voices[v], 1}, x);
        CHECK(bitIdentical(alone.voices.front().y, r.y));
    }
}

TEST_CASE("Filter Control on the filter: bit-identical across block sizes, with no allocation",
          "[dsp][filter][filter-control]")
{
    // Knob events at arbitrary samples, two voices on different grids, each
    // with an LFO on Cutoff X and Reso AB and keytrack following a glide:
    // host blocks of 1 and 37 against 4096 give the same output bit for bit
    // (the ramps start at their events' samples, and each voice reads them
    // at its own grid), and nothing allocates.
    constexpr double rate = 44100.0;
    constexpr std::size_t n = 12000;
    const auto x = anoesis::test::sine(97.0, rate, n, 0.8);
    const fc::Knobs knobs{
        .cutoff =
            [](std::size_t i)
        {
            if (i < 1500)
            {
                return 4.0f;
            }
            return i < 7001 ? 6.5f : 2.0f;
        },
        .controlCutoff = [](std::size_t i) { return i < 3333 ? 5.0f : 8.0f; },
        .resonance = [](std::size_t i) { return i < 4444 ? 2.0f : 7.0f; },
        .controlResonance = [](std::size_t i) { return i < 9000 ? 0.0f : 4.0f; },
    };
    const auto lfo = [](double hz, double depth)
    {
        return [=](std::size_t i)
        {
            return static_cast<float>(depth * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
        };
    };
    const auto glide = [](std::size_t i)
    {
        return 48.0f + (12.0f * std::min(1.0f, static_cast<float>(i) / 6000.0f));
    };
    const std::array<fc::Voice, 2> voices{
        fc::Voice{
            .noteOn = 0,
            .cutoffModulation = lfo(3.0, 6.0),
            .controlResonanceModulation = lfo(7.0, 4.0),
            .pitch = glide,
        },
        fc::Voice{
            .noteOn = 517,
            .cutoffModulation = lfo(5.0, -3.0),
            .resonanceModulation = lfo(2.0, 5.0),
            .pitch = glide,
        },
    };
    const auto render = [&](std::size_t block)
    {
        return fc::run({.rate = rate, .keytrack = 7.0f, .hostBlock = block}, knobs, voices, x);
    };
    const auto reference = render(4096);
    CHECK(reference.allocations == 0);
    for (const std::size_t block : {std::size_t{1}, std::size_t{37}})
    {
        const auto r = render(block);
        CAPTURE(block);
        CHECK(r.allocations == 0);
        for (std::size_t v = 0; v < voices.size(); ++v)
        {
            CHECK(bitIdentical(r.voices[v].y, reference.voices[v].y));
        }
    }
}
