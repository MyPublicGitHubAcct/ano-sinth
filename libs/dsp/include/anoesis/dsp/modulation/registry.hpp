// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

/// The modulation registries: one constexpr table of destinations and one of
/// sources, each indexed by an enum (inventory §6, M1, M2; decided in M1.5
/// task 2, docs/dsp/modulation.md, registry (a)).
///
/// - The 27 destinations and 11 misc sources, each after "None", in the
///   demo's menu order (M1, M2). The enums' order is internal: presets never
///   save a position. They save each row's string key, which lives with the
///   labels in the parameter table (M1.9), not here: libs/dsp holds only the
///   fields the DSP reads, and no strings.
/// - A destination row: the target it moves and the parts it acts on (AB is
///   both parts, except Cutoff AB and Reso AB, which are the Filter Control's
///   own targets), its scope, its evaluation rate, its units per unit of
///   amount (a knob unit; 12 semitones for Pitch), and the knob range that
///   F24's clamp holds the control plus its modulation to.
/// - A source row: its scope, the range every value lies in, and the kind of
///   bound it gives the skip test (source_bound.hpp).
/// - Rows are listed with their enum value and placed by it, and the
///   static_asserts below check that every value has exactly one row, so the
///   list's order can't matter. They also check every row's fields, and that
///   rows sharing a target agree on what the voice reads per target.
///
/// Phase 2 adds a destination or a source as an enum value and a row. Nothing
/// switches on a destination or a source.
///
/// Doc: docs/dsp/modulation.md
#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/voice/mix_law.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace anoesis::dsp::modulation
{

/// Whether a destination or a source has one value per voice or one for the
/// instrument.
enum class Scope : std::uint8_t
{
    voice,
    global,
};

/// How often a destination takes its modulation.
enum class EvalRate : std::uint8_t
{
    perSample,  ///< every sample
    gridHeld,   ///< at the voice's 32-sample grid points, held for the segment
    gridRamped, ///< at the grid points, then linearly to the next one's value (one segment late)
};

/// What a destination moves, whichever part (or LFO) it acts on. The voice
/// reads its modulation per target and part (routing.hpp).
enum class Target : std::uint8_t
{
    none,
    volume,
    pitch,
    phase,
    pan,
    cutoff,           ///< filter A's or B's own Cutoff
    controlCutoff,    ///< the Filter Control's Cutoff: Cutoff AB, clamped with that knob (filter_control.hpp)
    resonance,        ///< filter A's or B's own Resonance
    controlResonance, ///< the Filter Control's Resonance: Reso AB
    mix,
    lfoRate,
    lfoGain,
    phaserFrequency,
    distortionAmount,
    count,
};

inline constexpr std::size_t targetCount = static_cast<std::size_t>(Target::count);

/// The parts a destination acts on: part A and part B, or for the LFO
/// targets LFO 1 and LFO 2. A global target uses `a`.
enum class Parts : std::uint8_t
{
    none = 0,
    a = 1,
    b = 2,
    ab = 3,
};

/// Each target has a sum for each of two parts (or LFOs).
inline constexpr std::size_t partCount = 2;

/// True if `parts` includes part `part` (0 or 1).
[[nodiscard]] constexpr bool includes(Parts parts, std::size_t part) noexcept
{
    return part < partCount && (static_cast<unsigned>(parts) & (1U << part)) != 0;
}

inline constexpr float unbounded = std::numeric_limits<float>::infinity();

/// The range F24's clamp holds a control plus its modulation to, in the
/// knob's units, or a wrap (Phase: 10 is a whole cycle).
struct KnobRange
{
    float min = 0.0f;
    float max = 10.0f;
    bool wraps = false;

    [[nodiscard]] constexpr bool isUnbounded() const noexcept
    {
        return min <= -unbounded && max >= unbounded;
    }
};

/// A knob from 0 to 10: the filters' (filter_control.hpp), the LFOs' Rate
/// and Gain, Pan, Volume, and the effects'.
inline constexpr KnobRange knob10{.min = filterControlLaw.knobMin, .max = filterControlLaw.knobMax};
/// Mix's internal units, 0 to 15 (mix_law.hpp).
inline constexpr KnobRange mix15{.min = mixLaw.knobMin, .max = mixLaw.knobMax};
/// Phase: 0 to 10 for 360 degrees, wrapping.
inline constexpr KnobRange phase10{.min = 0.0f, .max = 10.0f, .wraps = true};
/// Pitch: no knob, so no clamp (M9's hypothesis).
inline constexpr KnobRange noRange{.min = -unbounded, .max = unbounded};

/// Pitch's units per unit of amount: 12 semitones, an octave (the developer:
/// "at 1.0 it'll pitch up 1 octave").
inline constexpr float semitonesPerAmount = 12.0f;

// --- Destinations ------------------------------------------------------------------------

/// Every destination, "None" first, then the demo's menu order (M1). The
/// value is internal: never saved, free to change.
enum class Destination : std::uint8_t
{
    none,
    volumeA,
    volumeB,
    volumeAB,
    pitchA,
    pitchB,
    pitchAB,
    phaseA,
    phaseB,
    phaseAB,
    panA,
    panB,
    panAB,
    cutoffA,
    cutoffB,
    cutoffAB,
    resonanceA,
    resonanceB,
    resonanceAB,
    phaserFrequency,
    mixA,
    mixB,
    mixAB,
    lfo1Rate,
    lfo1Gain,
    lfo2Rate,
    lfo2Gain,
    distortionAmount,
    count,
};

inline constexpr std::size_t destinationCount = static_cast<std::size_t>(Destination::count);

/// What the DSP reads of a destination.
struct DestinationRow
{
    Target target = Target::none;
    Parts parts = Parts::none;
    Scope scope = Scope::voice;
    EvalRate rate = EvalRate::perSample;
    /// The target's units added by amount x source = 1.
    float unitsPerAmount = 1.0f;
    KnobRange range{};
};

/// A row with the destination it describes, for the list below.
struct DestinationEntry
{
    Destination id = Destination::none;
    DestinationRow row{};
};

namespace detail
{
[[nodiscard]] constexpr DestinationEntry voiceRow(Destination id, Target target, Parts parts, EvalRate rate,
                                                  float units, KnobRange range) noexcept
{
    return {
        .id = id,
        .row =
            {
                .target = target,
                .parts = parts,
                .scope = Scope::voice,
                .rate = rate,
                .unitsPerAmount = units,
                .range = range,
            },
    };
}
[[nodiscard]] constexpr DestinationEntry filterRow(Destination id, Target target, Parts parts) noexcept
{
    // Cutoff and Resonance: into the Filter Control's modulation inputs at the
    // voice's grid points (M1.3 task 6).
    return voiceRow(id, target, parts, EvalRate::gridHeld, 1.0f, knob10);
}
[[nodiscard]] constexpr DestinationEntry globalRow(Destination id, Target target) noexcept
{
    return {
        .id = id,
        .row =
            {
                .target = target,
                .parts = Parts::a,
                .scope = Scope::global,
                .rate = EvalRate::gridRamped,
                .unitsPerAmount = 1.0f,
                .range = knob10,
            },
    };
}
} // namespace detail

/// Every destination with its row. The rates are the decision (M1.5 task 2):
/// Pitch, Phase, Volume and Mix per sample; Pan ramped on the grid; Cutoff
/// and Reso at the grid points; LFO Rate held and Gain ramped on the grid
/// (segment order); the global destinations ramped on the absolute grid.
inline constexpr std::array destinationEntries{
    DestinationEntry{.id = Destination::none},
    detail::voiceRow(Destination::volumeA, Target::volume, Parts::a, EvalRate::perSample, 1.0f, knob10),
    detail::voiceRow(Destination::volumeB, Target::volume, Parts::b, EvalRate::perSample, 1.0f, knob10),
    detail::voiceRow(Destination::volumeAB, Target::volume, Parts::ab, EvalRate::perSample, 1.0f, knob10),
    detail::voiceRow(Destination::pitchA, Target::pitch, Parts::a, EvalRate::perSample, semitonesPerAmount, noRange),
    detail::voiceRow(Destination::pitchB, Target::pitch, Parts::b, EvalRate::perSample, semitonesPerAmount, noRange),
    detail::voiceRow(Destination::pitchAB, Target::pitch, Parts::ab, EvalRate::perSample, semitonesPerAmount, noRange),
    detail::voiceRow(Destination::phaseA, Target::phase, Parts::a, EvalRate::perSample, 1.0f, phase10),
    detail::voiceRow(Destination::phaseB, Target::phase, Parts::b, EvalRate::perSample, 1.0f, phase10),
    detail::voiceRow(Destination::phaseAB, Target::phase, Parts::ab, EvalRate::perSample, 1.0f, phase10),
    detail::voiceRow(Destination::panA, Target::pan, Parts::a, EvalRate::gridRamped, 1.0f, knob10),
    detail::voiceRow(Destination::panB, Target::pan, Parts::b, EvalRate::gridRamped, 1.0f, knob10),
    detail::voiceRow(Destination::panAB, Target::pan, Parts::ab, EvalRate::gridRamped, 1.0f, knob10),
    detail::filterRow(Destination::cutoffA, Target::cutoff, Parts::a),
    detail::filterRow(Destination::cutoffB, Target::cutoff, Parts::b),
    detail::filterRow(Destination::cutoffAB, Target::controlCutoff, Parts::ab),
    detail::filterRow(Destination::resonanceA, Target::resonance, Parts::a),
    detail::filterRow(Destination::resonanceB, Target::resonance, Parts::b),
    detail::filterRow(Destination::resonanceAB, Target::controlResonance, Parts::ab),
    detail::globalRow(Destination::phaserFrequency, Target::phaserFrequency),
    detail::voiceRow(Destination::mixA, Target::mix, Parts::a, EvalRate::perSample, 1.0f, mix15),
    detail::voiceRow(Destination::mixB, Target::mix, Parts::b, EvalRate::perSample, 1.0f, mix15),
    detail::voiceRow(Destination::mixAB, Target::mix, Parts::ab, EvalRate::perSample, 1.0f, mix15),
    detail::voiceRow(Destination::lfo1Rate, Target::lfoRate, Parts::a, EvalRate::gridHeld, 1.0f, knob10),
    detail::voiceRow(Destination::lfo1Gain, Target::lfoGain, Parts::a, EvalRate::gridRamped, 1.0f, knob10),
    detail::voiceRow(Destination::lfo2Rate, Target::lfoRate, Parts::b, EvalRate::gridHeld, 1.0f, knob10),
    detail::voiceRow(Destination::lfo2Gain, Target::lfoGain, Parts::b, EvalRate::gridRamped, 1.0f, knob10),
    detail::globalRow(Destination::distortionAmount, Target::distortionAmount),
};

// --- Sources -----------------------------------------------------------------------------

/// Every source, "None" first, then the misc selector's order (M2). ModEnv
/// 1/2 and LFO 1/2 also own two slots each (routing.hpp).
enum class Source : std::uint8_t
{
    none,
    velocity,
    modWheel,
    keytrack,
    ampEnvA,
    ampEnvB,
    modEnv1,
    modEnv2,
    lfo1,
    lfo2,
    aftertouch,
    stepVelocity,
    count,
};

inline constexpr std::size_t sourceCount = static_cast<std::size_t>(Source::count);

/// How a source bounds its values over a stretch of samples
/// (source_bound.hpp).
enum class BoundKind : std::uint8_t
{
    none,       ///< no source: exactly 0
    linearEnds, ///< linear (or constant) between events: its two ends
    envelope,   ///< an Adsr: by its stage
    lfo,        ///< an LFO: its segment rendered ahead, gain and offset as intervals
    range,      ///< any other source: its range
};

/// What the DSP reads of a source.
struct SourceRow
{
    Scope scope = Scope::voice;
    float min = 0.0f; ///< every value lies in [min, max]
    float max = 0.0f;
    BoundKind bound = BoundKind::none;
};

struct SourceEntry
{
    Source id = Source::none;
    SourceRow row{};
};

/// An LFO as a source is half of gain x (wave + offset), gain 0-1 and offset
/// -1 to +1 (M6: a Sine at Gain 10 is +-0.5).
inline constexpr float lfoSourceScale = 0.5f;
/// The LFO's wave can pass +-1 by up to 0.25 while a jump is slewed (lfo.hpp).
inline constexpr float lfoWaveLimit = 1.25f;
/// So its range as a source is +-1.125.
inline constexpr float lfoSourceLimit = lfoSourceScale * (lfoWaveLimit + 1.0f);
/// KeyTrack, (pitch - 9) / 120 (keytrack.hpp), for a pitch from MIDI 0 with
/// the bend at -24 and a master tune of 400 Hz, to MIDI 127 at +24 and 480 Hz.
inline constexpr float keytrackMin = -0.32f;
inline constexpr float keytrackMax = 1.22f;

namespace detail
{
[[nodiscard]] constexpr SourceEntry sourceRow(Source id, Scope scope, float min, float max, BoundKind bound) noexcept
{
    return {.id = id, .row = {.scope = scope, .min = min, .max = max, .bound = bound}};
}
} // namespace detail

inline constexpr std::array sourceEntries{
    SourceEntry{.id = Source::none},
    detail::sourceRow(Source::velocity, Scope::voice, 0.0f, 1.0f, BoundKind::linearEnds),
    detail::sourceRow(Source::modWheel, Scope::global, 0.0f, 1.0f, BoundKind::linearEnds),
    detail::sourceRow(Source::keytrack, Scope::voice, keytrackMin, keytrackMax, BoundKind::linearEnds),
    detail::sourceRow(Source::ampEnvA, Scope::voice, 0.0f, 1.0f, BoundKind::envelope),
    detail::sourceRow(Source::ampEnvB, Scope::voice, 0.0f, 1.0f, BoundKind::envelope),
    detail::sourceRow(Source::modEnv1, Scope::voice, 0.0f, 1.0f, BoundKind::envelope),
    detail::sourceRow(Source::modEnv2, Scope::voice, 0.0f, 1.0f, BoundKind::envelope),
    detail::sourceRow(Source::lfo1, Scope::voice, -lfoSourceLimit, lfoSourceLimit, BoundKind::lfo),
    detail::sourceRow(Source::lfo2, Scope::voice, -lfoSourceLimit, lfoSourceLimit, BoundKind::lfo),
    detail::sourceRow(Source::aftertouch, Scope::global, 0.0f, 1.0f, BoundKind::linearEnds),
    detail::sourceRow(Source::stepVelocity, Scope::global, 0.0f, 1.0f, BoundKind::linearEnds),
};

// --- The tables, indexed by the enums ------------------------------------------------------

namespace detail
{
/// True if every value below `count` has exactly one entry.
template <typename Id, std::size_t N, typename Entry>
[[nodiscard]] constexpr bool eachIdOnce(const std::array<Entry, N>& entries, std::size_t count) noexcept
{
    if (N != count)
    {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        std::size_t found = 0;
        for (const auto& e : entries)
        {
            found += static_cast<std::size_t>(e.id) == i ? 1U : 0U;
        }
        if (found != 1)
        {
            return false;
        }
    }
    return true;
}

/// Each entry's row at its enum value's index.
template <typename Row, std::size_t N, typename Entry>
[[nodiscard]] constexpr std::array<Row, N> placed(const std::array<Entry, N>& entries) noexcept
{
    std::array<Row, N> table{};
    for (const auto& e : entries)
    {
        table[static_cast<std::size_t>(e.id)] = e.row;
    }
    return table;
}
} // namespace detail

static_assert(detail::eachIdOnce<Destination>(destinationEntries, destinationCount),
              "every Destination needs exactly one row");
static_assert(detail::eachIdOnce<Source>(sourceEntries, sourceCount), "every Source needs exactly one row");

inline constexpr std::array<DestinationRow, destinationCount> destinations =
    detail::placed<DestinationRow>(destinationEntries);
inline constexpr std::array<SourceRow, sourceCount> sources = detail::placed<SourceRow>(sourceEntries);

/// d's row; "None"'s for a value out of range.
[[nodiscard]] constexpr const DestinationRow& destinationRow(Destination d) noexcept
{
    const auto i = static_cast<std::size_t>(d);
    return destinations[i < destinationCount ? i : 0];
}

/// s's row; "None"'s for a value out of range.
[[nodiscard]] constexpr const SourceRow& sourceRow(Source s) noexcept
{
    const auto i = static_cast<std::size_t>(s);
    return sources[i < sourceCount ? i : 0];
}

// --- Compile-time checks on the rows -------------------------------------------------------

namespace detail
{
/// Exactly equal, written so -Wfloat-equal accepts it.
[[nodiscard]] constexpr bool sameValue(float a, float b) noexcept
{
    return a >= b && a <= b;
}

[[nodiscard]] constexpr bool sameRange(const KnobRange& x, const KnobRange& y) noexcept
{
    return sameValue(x.min, y.min) && sameValue(x.max, y.max) && x.wraps == y.wraps;
}

[[nodiscard]] constexpr bool destinationRowValid(Destination id, const DestinationRow& d) noexcept
{
    if (id == Destination::none || d.target == Target::none)
    {
        // Only "None" moves nothing, and it acts on no part.
        return id == Destination::none && d.target == Target::none && d.parts == Parts::none;
    }
    if (static_cast<std::size_t>(d.target) >= targetCount)
    {
        return false;
    }
    // A global destination has no voice to be per sample in, and no part.
    const bool scopeOk =
        d.scope == Scope::voice ? d.parts != Parts::none : d.parts == Parts::a && d.rate != EvalRate::perSample;
    const bool unitsOk = d.unitsPerAmount > 0.0f && d.unitsPerAmount < unbounded;
    const bool rangeOk =
        d.range.isUnbounded() || (d.range.min < d.range.max && d.range.max < unbounded && d.range.min > -unbounded);
    // AB on a filter's own Cutoff or Resonance would add to both filters,
    // where the decision is the Filter Control's own term.
    const bool filterTarget = d.target == Target::cutoff || d.target == Target::resonance;
    const bool filterAbOk = !filterTarget || d.parts != Parts::ab;
    return scopeOk && unitsOk && rangeOk && filterAbOk;
}

/// Rows that share a target agree on everything the voice reads per target.
[[nodiscard]] constexpr bool targetsConsistent() noexcept
{
    for (std::size_t i = 0; i < destinationCount; ++i)
    {
        for (std::size_t j = i + 1; j < destinationCount; ++j)
        {
            const auto& x = destinations[i];
            const auto& y = destinations[j];
            if (x.target != y.target)
            {
                continue;
            }
            const bool same = x.scope == y.scope && x.rate == y.rate && sameRange(x.range, y.range) &&
                              sameValue(x.unitsPerAmount, y.unitsPerAmount);
            // Two rows on one target never act on the same parts.
            if (!same || x.parts == y.parts)
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] constexpr bool destinationsValid() noexcept
{
    return std::ranges::all_of(destinationEntries,
                               [](const DestinationEntry& e) { return destinationRowValid(e.id, e.row); }) &&
           targetsConsistent();
}

[[nodiscard]] constexpr bool sourceRowValid(Source id, const SourceRow& s) noexcept
{
    if (id == Source::none || s.bound == BoundKind::none)
    {
        // Only "None" has no bound, and it is exactly 0.
        return id == Source::none && s.bound == BoundKind::none && sameValue(s.min, 0.0f) && sameValue(s.max, 0.0f);
    }
    const bool kindOk = static_cast<std::uint8_t>(s.bound) <= static_cast<std::uint8_t>(BoundKind::range);
    return kindOk && s.min < s.max && s.min > -unbounded && s.max < unbounded;
}

[[nodiscard]] constexpr bool sourcesValid() noexcept
{
    return std::ranges::all_of(sourceEntries, [](const SourceEntry& e) { return sourceRowValid(e.id, e.row); });
}
} // namespace detail

static_assert(detail::destinationsValid(), "a destination row is inconsistent");
static_assert(detail::sourcesValid(), "a source row is inconsistent");
static_assert(lfoSourceLimit > 1.124f && lfoSourceLimit < 1.126f);

} // namespace anoesis::dsp::modulation
