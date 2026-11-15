// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <numeric>

/// Note values for tempo sync (inventory §8), chosen from measured
/// candidates (docs/dsp/tempo-sync.md):
///
/// - A note value is exact: a Ratio of whole notes (8/1 is eight whole notes,
///   1/4 a quarter note) and a kind, straight, triplet (2/3 of the length),
///   or dotted (3/2). A whole note is four quarter notes whatever the host's
///   time signature ("one bar contains four beats", manual p. 12).
/// - The conversions take plain values, the tempo in quarter notes per
///   minute and the sample rate, and round once at the end. At integer
///   tempos and rates seconds, hertz, and samples are the exact fraction
///   correctly rounded.
/// - notePhaseIncrement() is an LFO's rate for Lfo::setPhaseIncrement():
///   cycles per sample x 2^64, within 2^-53 relative, so a synced LFO
///   drifts less than 1e-10 cycles in three hours.
/// - syncNoteValues is the list a synced control steps through: every
///   straight value from 8/1 to 1/64 with its dotted and triplet value,
///   longest first.
///
/// Everything is constexpr or a few arithmetic operations: no allocation,
/// no state. Long-running positions (an arpeggiator's steps) are counted in
/// integers, through a 64-bit phase or k x an exact fraction, never by
/// adding a rounded length (docs/dsp/tempo-sync.md, Exactness).
/// Doc: docs/dsp/tempo-sync.md
namespace anoesis::dsp
{

/// An exact non-negative fraction, always reduced (0 is 0/1). The
/// denominator must not be 0.
class Ratio
{
public:
    constexpr Ratio() noexcept = default;

    constexpr Ratio(std::uint64_t numerator, std::uint64_t denominator) noexcept
    {
        const std::uint64_t g = std::gcd(numerator, denominator);
        num = numerator / g;
        den = denominator / g;
    }

    [[nodiscard]] constexpr std::uint64_t numerator() const noexcept
    {
        return num;
    }

    [[nodiscard]] constexpr std::uint64_t denominator() const noexcept
    {
        return den;
    }

    /// The nearest double (one division).
    [[nodiscard]] constexpr double toDouble() const noexcept
    {
        return static_cast<double>(num) / static_cast<double>(den);
    }

    friend constexpr Ratio operator*(Ratio a, Ratio b) noexcept
    {
        // Cross-reduced first, so the products are no larger than the result.
        const std::uint64_t g1 = std::gcd(a.num, b.den);
        const std::uint64_t g2 = std::gcd(b.num, a.den);
        return Ratio{(a.num / g1) * (b.num / g2), (a.den / g2) * (b.den / g1)};
    }

    friend constexpr Ratio operator+(Ratio a, Ratio b) noexcept
    {
        const std::uint64_t g = std::gcd(a.den, b.den);
        return Ratio{(a.num * (b.den / g)) + (b.num * (a.den / g)), a.den * (b.den / g)};
    }

    /// Reduced, so equal fractions have equal members.
    friend constexpr bool operator==(Ratio a, Ratio b) noexcept = default;

    friend constexpr std::strong_ordering operator<=>(Ratio a, Ratio b) noexcept
    {
        return (a.num * b.den) <=> (b.num * a.den);
    }

private:
    std::uint64_t num = 0;
    std::uint64_t den = 1;
};

/// Straight, triplet (2/3 of the length), or dotted (3/2): "A triplet is 2/3
/// of the length, while dotted means 3/2" (the developer, KVR p=4006903).
enum class NoteKind : std::uint8_t
{
    straight,
    triplet,
    dotted,
};

/// A note value: a straight length in whole notes and a kind.
struct NoteValue
{
    Ratio base{1, 4};
    NoteKind kind = NoteKind::straight;

    [[nodiscard]] static constexpr NoteValue straight(std::uint64_t num, std::uint64_t den) noexcept
    {
        return {.base = Ratio{num, den}, .kind = NoteKind::straight};
    }

    [[nodiscard]] static constexpr NoteValue triplet(std::uint64_t num, std::uint64_t den) noexcept
    {
        return {.base = Ratio{num, den}, .kind = NoteKind::triplet};
    }

    [[nodiscard]] static constexpr NoteValue dotted(std::uint64_t num, std::uint64_t den) noexcept
    {
        return {.base = Ratio{num, den}, .kind = NoteKind::dotted};
    }

    /// The length in whole notes, exactly.
    [[nodiscard]] constexpr Ratio wholeNotes() const noexcept
    {
        switch (kind)
        {
        case NoteKind::triplet:
            return base * Ratio{2, 3};
        case NoteKind::dotted:
            return base * Ratio{3, 2};
        case NoteKind::straight:
            break;
        }
        return base;
    }

    /// The length in quarter notes (beats), exactly.
    [[nodiscard]] constexpr Ratio quarterNotes() const noexcept
    {
        return wholeNotes() * Ratio{4, 1};
    }

    friend constexpr bool operator==(const NoteValue& a, const NoteValue& b) noexcept = default;
};

namespace detail
{

[[nodiscard]] constexpr std::array<NoteValue, 30> makeSyncNoteValues() noexcept
{
    constexpr std::array<Ratio, 10> bases{
        Ratio{8, 1}, Ratio{4, 1}, Ratio{2, 1},  Ratio{1, 1},  Ratio{1, 2},
        Ratio{1, 4}, Ratio{1, 8}, Ratio{1, 16}, Ratio{1, 32}, Ratio{1, 64},
    };
    std::array<NoteValue, 30> list{};
    std::size_t i = 0;
    for (const Ratio base : bases)
    {
        for (const NoteKind kind : {NoteKind::dotted, NoteKind::straight, NoteKind::triplet})
        {
            list[i] = NoteValue{.base = base, .kind = kind};
            ++i;
        }
    }
    std::ranges::sort(list, [](const NoteValue& a, const NoteValue& b) { return a.wholeNotes() > b.wholeNotes(); });
    return list;
}

} // namespace detail

/// The values a synced control steps through, longest first: 8/1D, 8/1,
/// 4/1D, 8/1T, 4/1, ..., 1/64, 1/64T (inventory S1, a working hypothesis).
/// Strictly decreasing, so a knob over it is monotonic. A control may use
/// a contiguous sub-range (the delay's longest is limited by its buffer).
inline constexpr std::array<NoteValue, 30> syncNoteValues = detail::makeSyncNoteValues();

/// Seconds per note at `bpm` quarter notes per minute.
[[nodiscard]] inline double noteSeconds(NoteValue value, double bpm) noexcept
{
    const Ratio q = value.quarterNotes();
    return (60.0 * static_cast<double>(q.numerator())) / (bpm * static_cast<double>(q.denominator()));
}

/// Cycles per second of a cycle of `value`.
[[nodiscard]] inline double noteHertz(NoteValue value, double bpm) noexcept
{
    const Ratio q = value.quarterNotes();
    return (bpm * static_cast<double>(q.denominator())) / (60.0 * static_cast<double>(q.numerator()));
}

/// Samples per note (fractional).
[[nodiscard]] inline double noteSamples(NoteValue value, double bpm, double sampleRate) noexcept
{
    const Ratio q = value.quarterNotes();
    return (60.0 * sampleRate * static_cast<double>(q.numerator())) / (bpm * static_cast<double>(q.denominator()));
}

/// The phase increment of an LFO whose cycle is `value`: cycles per sample
/// x 2^64, rounded to the nearest integer, at most half a cycle (Nyquist).
/// 0 unless the tempo and the sample rate are positive and finite. For
/// Lfo::setPhaseIncrement().
[[nodiscard]] inline std::uint64_t notePhaseIncrement(NoteValue value, double bpm, double sampleRate) noexcept
{
    if (!(bpm > 0.0) || !(sampleRate > 0.0) || !std::isfinite(bpm) || !std::isfinite(sampleRate))
    {
        return 0;
    }
    const Ratio q = value.quarterNotes();
    const double cycles =
        (bpm * static_cast<double>(q.denominator())) / (60.0 * sampleRate * static_cast<double>(q.numerator()));
    // 2^64 x at most 1/2 is below 2^64, so the conversion is defined.
    return static_cast<std::uint64_t>(std::round(std::ldexp(std::min(cycles, 0.5), 64)));
}

} // namespace anoesis::dsp
