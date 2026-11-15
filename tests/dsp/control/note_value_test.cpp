// Tests for the note values (libs/dsp/include/anoesis/dsp/control/note_value.hpp),
// chosen in docs/dsp/tempo-sync.md: exact fractions of a whole note,
// straight, triplet (x 2/3), and dotted (x 3/2); the 30-value sync list; and
// the conversions to seconds, hertz, samples, and the LFO's 64-bit phase
// increment, which take plain values (the tempo and the sample rate).
//
// References are exact: Ratio arithmetic, and the increment's exact value
// from long division in integers. Tolerances are derived next to each use.

#include <anoesis/dsp/control/note_value.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

using anoesis::dsp::noteHertz;
using anoesis::dsp::NoteKind;
using anoesis::dsp::notePhaseIncrement;
using anoesis::dsp::noteSamples;
using anoesis::dsp::noteSeconds;
using anoesis::dsp::NoteValue;
using anoesis::dsp::Ratio;
using anoesis::dsp::syncNoteValues;
using Catch::Matchers::WithinULP;

namespace
{

constexpr std::array<Ratio, 10> bases{
    Ratio{8, 1}, Ratio{4, 1}, Ratio{2, 1},  Ratio{1, 1},  Ratio{1, 2},
    Ratio{1, 4}, Ratio{1, 8}, Ratio{1, 16}, Ratio{1, 32}, Ratio{1, 64},
};

// Integer tempos, and fractional ones that double holds exactly (93.75 and
// 128.5 BPM), so the exact references below are exact for what the
// conversions are given.
constexpr std::array<Ratio, 6> tempos{
    Ratio{60, 1}, Ratio{93, 1}, Ratio{120, 1}, Ratio{174, 1}, Ratio{375, 4}, Ratio{257, 2},
};
constexpr std::array<std::uint64_t, 4> integerRates{44100, 48000, 96000, 192000};

/// floor(a x 2^64 / b) and the remainder, for a < b < 2^63: long division,
/// one bit at a time.
struct Quotient
{
    std::uint64_t quotient = 0;
    std::uint64_t remainder = 0;
};

Quotient fixedQuotient(std::uint64_t a, std::uint64_t b)
{
    Quotient q{.quotient = 0, .remainder = a};
    for (int bit = 0; bit < 64; ++bit)
    {
        q.remainder <<= 1U;
        q.quotient <<= 1U;
        if (q.remainder >= b)
        {
            q.remainder -= b;
            q.quotient |= 1U;
        }
    }
    return q;
}

/// Cycles per sample for a cycle of `value` at a tempo and an integer sample
/// rate, exactly: bpm x quarters.den / (60 x fs x quarters.num).
Ratio cyclesPerSample(NoteValue value, Ratio bpm, std::uint64_t fs)
{
    const Ratio q = value.quarterNotes();
    return bpm * Ratio{q.denominator(), 60 * fs * q.numerator()};
}

/// A wrapped phase difference (2^64 per cycle) as signed cycles.
double signedCycles(std::uint64_t difference)
{
    return std::ldexp(static_cast<double>(static_cast<std::int64_t>(difference)), -64);
}

} // namespace

TEST_CASE("Note values: a Ratio is exact and always reduced", "[dsp][tempo]")
{
    STATIC_CHECK((Ratio{4, 6} == Ratio{2, 3}));
    CHECK(Ratio{6, 8}.numerator() == 3);
    CHECK(Ratio{6, 8}.denominator() == 4);
    CHECK(Ratio{0, 5}.numerator() == 0);
    CHECK(Ratio{0, 5}.denominator() == 1);
    CHECK(Ratio{1, 8} * Ratio{2, 3} == Ratio{1, 12});
    CHECK(Ratio{3, 4} * Ratio{8, 9} == Ratio{2, 3});
    CHECK(Ratio{1, 8} + Ratio{1, 16} == Ratio{3, 16});
    CHECK(Ratio{1, 6} + Ratio{1, 6} + Ratio{1, 6} == Ratio{1, 2});
    CHECK(Ratio{1, 3} < Ratio{3, 8});
    CHECK(Ratio{3, 8} > Ratio{1, 3});
    CHECK((Ratio{2, 6} <=> Ratio{1, 3}) == 0);
    CHECK(Ratio{1, 3} != Ratio{1, 4});
    CHECK_THAT((Ratio{3, 8}.toDouble()), WithinULP(0.375, 0));
}

TEST_CASE("Note values: a triplet is 2/3 and a dotted value 3/2 of the length, exactly", "[dsp][tempo]")
{
    for (const Ratio base : bases)
    {
        const auto num = base.numerator();
        const auto den = base.denominator();
        CHECK(NoteValue::straight(num, den).wholeNotes() == base);
        CHECK(NoteValue::triplet(num, den).wholeNotes() == base * Ratio{2, 3});
        CHECK(NoteValue::dotted(num, den).wholeNotes() == base * Ratio{3, 2});
        CHECK(NoteValue::straight(num, den).quarterNotes() == base * Ratio{4, 1});
        CHECK(NoteValue::triplet(num, den).kind == NoteKind::triplet);
        CHECK(NoteValue::dotted(num, den).kind == NoteKind::dotted);
    }

    // "One bar contains four beats" (manual p. 12): a whole note is four
    // quarter notes.
    CHECK(NoteValue::straight(1, 1).quarterNotes() == Ratio{4, 1});

    // The developer's worked numbers (KVR p=4006903): 8/1 is 32 beats, 2/1
    // is 8, 2/1T is 16/3, and 2/1D is 12.
    CHECK(NoteValue::straight(8, 1).quarterNotes() == Ratio{32, 1});
    CHECK(NoteValue::straight(2, 1).quarterNotes() == Ratio{8, 1});
    CHECK(NoteValue::triplet(2, 1).quarterNotes() == Ratio{16, 3});
    CHECK(NoteValue::dotted(2, 1).quarterNotes() == Ratio{12, 1});

    // Identities that a table of rounded multipliers can miss.
    const auto times = [](NoteValue v, std::uint64_t n)
    {
        return v.wholeNotes() * Ratio{n, 1};
    };
    CHECK(times(NoteValue::triplet(1, 8), 3) == NoteValue::straight(1, 4).wholeNotes());
    CHECK(times(NoteValue::triplet(1, 64), 96) == NoteValue::straight(1, 1).wholeNotes());
    CHECK(times(NoteValue::dotted(1, 8), 2) == NoteValue::dotted(1, 4).wholeNotes());
    CHECK(times(NoteValue::triplet(1, 8), 9) == NoteValue::dotted(1, 2).wholeNotes());
    CHECK(NoteValue::dotted(1, 8).wholeNotes() == Ratio{1, 8} + Ratio{1, 16});
    CHECK(NoteValue::triplet(1, 4) == NoteValue::triplet(2, 8));
    CHECK(!(NoteValue::triplet(1, 4) == NoteValue::straight(1, 6)));
}

TEST_CASE("Note values: the sync list is every straight value from 8/1 to 1/64 with its dotted and triplet value, "
          "longest first",
          "[dsp][tempo]")
{
    REQUIRE(syncNoteValues.size() == 30);
    CHECK(syncNoteValues.front() == NoteValue::dotted(8, 1));
    CHECK(syncNoteValues.back() == NoteValue::triplet(1, 64));
    for (std::size_t i = 1; i < syncNoteValues.size(); ++i)
    {
        INFO("entry " << i);
        CHECK(syncNoteValues[i].wholeNotes() < syncNoteValues[i - 1].wholeNotes());
    }
    for (const Ratio base : bases)
    {
        for (const NoteKind kind : {NoteKind::straight, NoteKind::triplet, NoteKind::dotted})
        {
            const NoteValue value{.base = base, .kind = kind};
            CHECK(std::ranges::count(syncNoteValues, value) == 1);
        }
    }
    // Where the dotted and triplet values interleave (1/2T is shorter than
    // 1/4D): 1/2, 1/4D, 1/2T, 1/4.
    CHECK(syncNoteValues[13] == NoteValue::straight(1, 2));
    CHECK(syncNoteValues[14] == NoteValue::dotted(1, 4));
    CHECK(syncNoteValues[15] == NoteValue::triplet(1, 2));
    CHECK(syncNoteValues[16] == NoteValue::straight(1, 4));
}

TEST_CASE("Note values: seconds, hertz, and samples round once from the exact fraction", "[dsp][tempo]")
{
    // Known values at 120 BPM.
    CHECK_THAT(noteSeconds(NoteValue::straight(1, 4), 120.0), WithinULP(0.5, 0));
    CHECK_THAT(noteSeconds(NoteValue::dotted(8, 1), 120.0), WithinULP(24.0, 0));
    CHECK_THAT(noteHertz(NoteValue::triplet(1, 64), 120.0), WithinULP(48.0, 0));
    CHECK_THAT(noteSamples(NoteValue::triplet(1, 64), 120.0, 48000.0), WithinULP(1000.0, 0));
    CHECK_THAT(noteSamples(NoteValue::dotted(1, 8), 120.0, 44100.0), WithinULP(16537.5, 0));

    // At these tempos and rates the numerator and denominator are exact in
    // double, so each conversion is one correctly rounded division: the same
    // double as the exact fraction's.
    for (const Ratio bpm : tempos)
    {
        const Ratio perBpm{bpm.denominator(), bpm.numerator()};
        for (const auto fs : integerRates)
        {
            for (const NoteValue v : syncNoteValues)
            {
                INFO(bpm.toDouble() << " BPM, " << fs << " Hz, " << v.base.numerator() << "/" << v.base.denominator());
                const Ratio q = v.quarterNotes();
                const Ratio seconds = Ratio{60 * q.numerator(), q.denominator()} * perBpm;
                const Ratio hertz = bpm * Ratio{q.denominator(), 60 * q.numerator()};
                const Ratio samples = Ratio{60 * fs * q.numerator(), q.denominator()} * perBpm;
                const double bpmD = bpm.toDouble();
                CHECK_THAT(noteSeconds(v, bpmD), WithinULP(seconds.toDouble(), 0));
                CHECK_THAT(noteHertz(v, bpmD), WithinULP(hertz.toDouble(), 0));
                CHECK_THAT(noteSamples(v, bpmD, static_cast<double>(fs)), WithinULP(samples.toDouble(), 0));
            }
        }
    }
}

TEST_CASE("Note values: the phase increment is the exact cycles per sample, to double precision", "[dsp][tempo]")
{
    for (const Ratio bpm : tempos)
    {
        for (const auto fs : integerRates)
        {
            for (const NoteValue v : syncNoteValues)
            {
                INFO(bpm.toDouble() << " BPM, " << fs << " Hz, " << v.base.numerator() << "/" << v.base.denominator());
                const Ratio c = cyclesPerSample(v, bpm, fs);
                REQUIRE(c < Ratio{1, 2});
                const Quotient exact = fixedQuotient(c.numerator(), c.denominator());
                const std::uint64_t increment = notePhaseIncrement(v, bpm.toDouble(), static_cast<double>(fs));
                // One division in double (2^-53 relative), scaled exactly by
                // 2^64 and rounded to the nearest integer: within half a unit
                // plus the division's rounding.
                const double error =
                    std::abs(static_cast<double>(static_cast<std::int64_t>(increment - exact.quotient)) -
                             (static_cast<double>(exact.remainder) / static_cast<double>(c.denominator())));
                CHECK(error <= 0.5 + (static_cast<double>(exact.quotient) * 0x1p-53));
            }
        }
    }

    // Half a cycle per sample at most (Nyquist), as Lfo::setFrequency.
    CHECK(notePhaseIncrement(NoteValue::triplet(1, 64), 999.0, 100.0) == std::uint64_t{1} << 63U);

    // No rate from a tempo or sample rate that isn't positive and finite.
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();
    const NoteValue quarter = NoteValue::straight(1, 4);
    for (const double bad : {0.0, -120.0, nan, inf})
    {
        CHECK(notePhaseIncrement(quarter, bad, 48000.0) == 0);
        CHECK(notePhaseIncrement(quarter, 120.0, bad) == 0);
    }
}

TEST_CASE("Note values: three hours of a synced phase stay within 1e-10 cycles of the exact grid", "[dsp][tempo]")
{
    // The phase after N samples is N x increment (mod 2^64), exactly: the
    // LFO adds the increment in integers. The exact phase is
    // frac(N x a / b), from long division.
    for (const NoteValue v : {NoteValue::straight(8, 1), NoteValue::triplet(1, 64)})
    {
        for (const Ratio bpm : tempos)
        {
            for (const auto fs : integerRates)
            {
                INFO(bpm.toDouble() << " BPM, " << fs << " Hz, " << v.base.numerator() << "/" << v.base.denominator());
                const std::uint64_t n = std::uint64_t{3} * 3600 * fs;
                const Ratio c = cyclesPerSample(v, bpm, fs);
                REQUIRE(n < std::numeric_limits<std::uint64_t>::max() / c.numerator());
                const std::uint64_t increment = notePhaseIncrement(v, bpm.toDouble(), static_cast<double>(fs));
                const std::uint64_t exact =
                    fixedQuotient((n * c.numerator()) % c.denominator(), c.denominator()).quotient;
                const double error = std::abs(signedCycles((n * increment) - exact));
                // The increment's error (above) times N, plus the exact
                // phase's own truncation.
                const double bound = std::ldexp(
                    (static_cast<double>(n) * (0.5 + (static_cast<double>(increment) * 0x1p-53))) + 1.0, -64);
                CHECK(error <= bound);
                CHECK(error < 1e-10);
            }
        }
    }
}
