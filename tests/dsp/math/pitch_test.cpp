// Tests for note <-> frequency conversion with master tuning.

#include <anoesis/dsp/math/pitch.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using anoesis::dsp::centsToRatio;
using anoesis::dsp::frequencyToNote;
using anoesis::dsp::noteToFrequency;
using anoesis::dsp::referenceFromCents;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

TEST_CASE("noteToFrequency: A4 (note 69) is the reference, octaves are exact", "[dsp][math]")
{
    CHECK_THAT(noteToFrequency(69.0f), WithinULP(440.0f, 0));
    CHECK_THAT(noteToFrequency(81.0f), WithinULP(880.0f, 0));
    CHECK_THAT(noteToFrequency(57.0f), WithinULP(220.0f, 0));
    CHECK_THAT(noteToFrequency(9.0f), WithinULP(13.75f, 0));
}

TEST_CASE("noteToFrequency: 12-tone equal temperament across the MIDI range", "[dsp][math]")
{
    // Tolerance 1e-6 relative = 0.0017 cents, far below the ~5 cent threshold
    // of pitch discrimination. Sources of error: rounding (note - 69) / 12 in
    // float (|arg| <= 5.75, so <= 3.4e-7 absolute, giving ln2 * 3.4e-7 = 2.4e-7
    // relative), plus exp2 and the multiply (about one ulp, 1.2e-7, each).
    for (int i = 0; i <= 127 * 4; ++i)
    {
        const auto note = 0.25f * static_cast<float>(i); // quarter tones
        const auto expected = 440.0 * std::exp2((static_cast<double>(note) - 69.0) / 12.0);
        CAPTURE(note);
        CHECK_THAT(noteToFrequency(note), WithinRel(static_cast<float>(expected), 1e-6f));
    }
    CHECK_THAT(noteToFrequency(60.0f), WithinRel(261.625565f, 1e-6f)); // middle C
}

TEST_CASE("noteToFrequency: master tune moves the reference", "[dsp][math]")
{
    CHECK_THAT(noteToFrequency(69.0f, 432.0f), WithinULP(432.0f, 0));
    CHECK_THAT(noteToFrequency(81.0f, 415.0f), WithinULP(830.0f, 0));
    // Every note scales by the same ratio.
    CHECK_THAT(noteToFrequency(60.0f, 444.0f) / noteToFrequency(60.0f), WithinRel(444.0f / 440.0f, 1e-6f));
}

TEST_CASE("Master tune in cents: referenceFromCents and centsToRatio", "[dsp][math]")
{
    CHECK_THAT(centsToRatio(0.0f), WithinULP(1.0f, 0));
    CHECK_THAT(centsToRatio(1200.0f), WithinULP(2.0f, 0));
    CHECK_THAT(centsToRatio(-1200.0f), WithinULP(0.5f, 0));
    CHECK_THAT(centsToRatio(100.0f), WithinRel(1.05946309f, 1e-6f)); // one semitone

    CHECK_THAT(referenceFromCents(0.0f), WithinULP(440.0f, 0));
    CHECK_THAT(referenceFromCents(100.0f), WithinRel(noteToFrequency(70.0f), 1e-6f)); // +1 semitone = A#4
    CHECK_THAT(referenceFromCents(-31.766f), WithinRel(432.0f, 1e-5f));               // 432 Hz tuning
}

TEST_CASE("frequencyToNote: inverse of noteToFrequency", "[dsp][math]")
{
    CHECK_THAT(frequencyToNote(440.0f), WithinULP(69.0f, 0));
    CHECK_THAT(frequencyToNote(432.0f, 432.0f), WithinULP(69.0f, 0));
    // Round trip. Tolerance 1e-4 semitones = 0.01 cents: the two float
    // conversions each lose a few ulps of a value near 100.
    for (int note = 0; note <= 127; ++note)
    {
        const auto n = static_cast<float>(note);
        CAPTURE(n);
        CHECK_THAT(frequencyToNote(noteToFrequency(n, 443.0f), 443.0f), WithinAbs(n, 1e-4));
    }
}
