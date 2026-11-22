// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The modulation model's vocabulary (M1.5 task 2, stage 1; docs/dsp/
// modulation.md): what a destination descriptor and a source descriptor
// hold, and the rows themselves, shared by the three registry candidates
// (registry_candidates.hpp) and the prototype voice (modulation_
// candidates.hpp). Prototypes for measurement, not library code.
//
// The rows come from task 1's lists (inventory §6, M1, M2): 27 destinations
// and 11 misc sources, each after "None". Only the names a user sees come
// from the third-party template; the keys (the persisted IDs), the internal
// order, the units and the ranges are ours (D7), and the keys can't be the
// template's numbering because they aren't numbers.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace anoesis::test::modulation
{

enum class Scope : std::uint8_t
{
    voice,  ///< one value per voice
    global, ///< one value for the instrument
};

/// How often a destination takes its modulation.
enum class EvalRate : std::uint8_t
{
    perSample,  ///< every sample
    gridHeld,   ///< at the voice's 32-sample grid points, held for the segment
    gridRamped, ///< at the grid points, then linearly to the next one's value (one segment late)
};

inline constexpr std::array evalRates{EvalRate::perSample, EvalRate::gridHeld, EvalRate::gridRamped};

[[nodiscard]] constexpr std::string_view rateName(EvalRate r) noexcept
{
    switch (r)
    {
    case EvalRate::perSample:
        return "per sample";
    case EvalRate::gridHeld:
        return "grid, held";
    case EvalRate::gridRamped:
        return "grid, ramped";
    }
    return "?";
}

/// The popup's groups (M1, confirmed by the demo: three columns).
enum class Group : std::uint8_t
{
    none,
    oscillators,
    filters,
    misc,
};

/// What a destination moves, whichever part (or LFO) it acts on. The voice
/// reads its modulation per target and part, so a row is data: Volume AB is
/// the volume target on both parts. Cutoff AB and Reso AB are not: the
/// Filter Control's law (decided in M1.3 task 6) clamps each control with
/// its own modulation, so AB is the Filter Control's term, which differs
/// from A plus B wherever a clamp acts.
enum class Target : std::uint8_t
{
    none,
    volume,
    pitch,
    phase,
    pan,
    cutoff,           ///< filter A's or B's own Cutoff (part A, part B)
    controlCutoff,    ///< the Filter Control's Cutoff: Cutoff AB, clamped with that knob (filter_control.hpp)
    resonance,        ///< filter A's or B's own Resonance
    controlResonance, ///< the Filter Control's Resonance: Reso AB
    mix,
    lfoRate,
    lfoGain,
    phaserFrequency,
    distortionAmount,
};

inline constexpr std::size_t targetCount = 14;

/// The parts a destination acts on (bit 0 part A, bit 1 part B), or for the
/// LFO targets the LFOs (bit 0 LFO 1, bit 1 LFO 2). A global target uses bit
/// 0.
inline constexpr std::uint8_t partA = 1;
inline constexpr std::uint8_t partB = 2;
inline constexpr std::uint8_t partAB = 3;

inline constexpr float unbounded = std::numeric_limits<float>::infinity();

/// The range F24's clamp holds a control plus its modulation to, in the
/// knob's units, or a wrap (Phase: 370 degrees is 10).
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

/// A destination's descriptor.
struct DestinationInfo
{
    /// The persisted ID: what a preset saves for the slot's choice. Stable
    /// once released, never reused, and separate from the menu's order.
    std::string_view key;
    std::string_view name; ///< the popup's label (ours)
    Group group = Group::none;
    Scope scope = Scope::voice;
    EvalRate rate = EvalRate::perSample;
    Target target = Target::none;
    std::uint8_t parts = 0;
    /// The target's units added by amount x source = 1: a knob unit, or 12
    /// semitones for Pitch (an octave, the developer).
    float unitsPerAmount = 1.0f;
    KnobRange range{};
};

/// How a source bounds its values over a stretch of samples
/// (source_bounds.hpp).
enum class BoundKind : std::uint8_t
{
    none,       ///< no source: 0
    linearEnds, ///< linear (or constant) between events: its two ends
    envelope,   ///< an Adsr: by its stage
    lfo,        ///< an LFO: its segment, rendered ahead
};

/// A source's descriptor.
struct SourceInfo
{
    std::string_view key; ///< the persisted ID, as a destination's
    std::string_view name;
    Scope scope = Scope::voice;
    float min = 0.0f; ///< the range: every value lies in it
    float max = 0.0f;
    BoundKind bound = BoundKind::none;
};

/// A closed interval of values.
struct Interval
{
    float lo = 0.0f;
    float hi = 0.0f;

    [[nodiscard]] constexpr bool contains(float x) const noexcept
    {
        return x >= lo && x <= hi;
    }
};

[[nodiscard]] constexpr Interval operator+(Interval a, Interval b) noexcept
{
    return {.lo = a.lo + b.lo, .hi = a.hi + b.hi};
}

/// amount x [lo, hi].
[[nodiscard]] constexpr Interval scaled(Interval x, float amount) noexcept
{
    return amount >= 0.0f ? Interval{.lo = amount * x.lo, .hi = amount * x.hi}
                          : Interval{.lo = amount * x.hi, .hi = amount * x.lo};
}

[[nodiscard]] constexpr Interval hull(Interval a, Interval b) noexcept
{
    return {.lo = a.lo < b.lo ? a.lo : b.lo, .hi = a.hi > b.hi ? a.hi : b.hi};
}

// --- The rows ---------------------------------------------------------------------------

inline constexpr std::size_t destinationCount = 28; // "None" and 27
inline constexpr std::size_t sourceCount = 12;      // "None" and 11

/// A knob from 0 to 10, clamped.
inline constexpr KnobRange knob10{.min = 0.0f, .max = 10.0f};
/// Mix's internal units (mix_law.hpp).
inline constexpr KnobRange mix15{.min = 0.0f, .max = 15.0f};
/// Phase: 0-10 for 360 degrees, wrapping.
inline constexpr KnobRange phase10{.min = 0.0f, .max = 10.0f, .wraps = true};
/// Pitch: no knob, no clamp (M9's hypothesis).
inline constexpr KnobRange noRange{.min = -unbounded, .max = unbounded};

namespace detail
{
[[nodiscard]] constexpr DestinationInfo osc(std::string_view key, std::string_view name, Target target,
                                            std::uint8_t parts, EvalRate rate, float units, KnobRange range) noexcept
{
    return {
        .key = key,
        .name = name,
        .group = Group::oscillators,
        .scope = Scope::voice,
        .rate = rate,
        .target = target,
        .parts = parts,
        .unitsPerAmount = units,
        .range = range,
    };
}
[[nodiscard]] constexpr DestinationInfo flt(std::string_view key, std::string_view name, Target target,
                                            std::uint8_t parts) noexcept
{
    // Cutoff and Resonance: settled in M1.3 task 6 (into the Filter
    // Control's modulation inputs at the voice's grid points).
    return {
        .key = key,
        .name = name,
        .group = Group::filters,
        .scope = Scope::voice,
        .rate = EvalRate::gridHeld,
        .target = target,
        .parts = parts,
        .unitsPerAmount = 1.0f,
        .range = knob10,
    };
}
[[nodiscard]] constexpr DestinationInfo misc(std::string_view key, std::string_view name, Scope scope, EvalRate rate,
                                             Target target, std::uint8_t parts, KnobRange range) noexcept
{
    return {
        .key = key,
        .name = name,
        .group = Group::misc,
        .scope = scope,
        .rate = rate,
        .target = target,
        .parts = parts,
        .unitsPerAmount = 1.0f,
        .range = range,
    };
}
} // namespace detail

/// Every destination, "None" first. The internal order is ours; the
/// popup's order (M1) happens to be the same, and is still a separate
/// presentation list. The rates are the decision (docs/dsp/modulation.md);
/// the prototype voice takes a rate per target from its patch instead.
inline constexpr std::array<DestinationInfo, destinationCount> destinationRows{
    DestinationInfo{.key = "none", .name = "None"},
    detail::osc("osc.volume.a", "Volume A", Target::volume, partA, EvalRate::perSample, 1.0f, knob10),
    detail::osc("osc.volume.b", "Volume B", Target::volume, partB, EvalRate::perSample, 1.0f, knob10),
    detail::osc("osc.volume.ab", "Volume AB", Target::volume, partAB, EvalRate::perSample, 1.0f, knob10),
    detail::osc("osc.pitch.a", "Pitch A", Target::pitch, partA, EvalRate::perSample, 12.0f, noRange),
    detail::osc("osc.pitch.b", "Pitch B", Target::pitch, partB, EvalRate::perSample, 12.0f, noRange),
    detail::osc("osc.pitch.ab", "Pitch AB", Target::pitch, partAB, EvalRate::perSample, 12.0f, noRange),
    detail::osc("osc.phase.a", "Phase A", Target::phase, partA, EvalRate::perSample, 1.0f, phase10),
    detail::osc("osc.phase.b", "Phase B", Target::phase, partB, EvalRate::perSample, 1.0f, phase10),
    detail::osc("osc.phase.ab", "Phase AB", Target::phase, partAB, EvalRate::perSample, 1.0f, phase10),
    detail::osc("osc.pan.a", "Pan A", Target::pan, partA, EvalRate::gridRamped, 1.0f, knob10),
    detail::osc("osc.pan.b", "Pan B", Target::pan, partB, EvalRate::gridRamped, 1.0f, knob10),
    detail::osc("osc.pan.ab", "Pan AB", Target::pan, partAB, EvalRate::gridRamped, 1.0f, knob10),
    detail::flt("filter.cutoff.a", "Cutoff A", Target::cutoff, partA),
    detail::flt("filter.cutoff.b", "Cutoff B", Target::cutoff, partB),
    detail::flt("filter.cutoff.ab", "Cutoff AB", Target::controlCutoff, partAB),
    detail::flt("filter.resonance.a", "Reso A", Target::resonance, partA),
    detail::flt("filter.resonance.b", "Reso B", Target::resonance, partB),
    detail::flt("filter.resonance.ab", "Reso AB", Target::controlResonance, partAB),
    detail::misc("fx.phaser.frequency", "Phaser Freq", Scope::global, EvalRate::gridRamped, Target::phaserFrequency,
                 partA, knob10),
    detail::misc("mix.a", "Mix A", Scope::voice, EvalRate::perSample, Target::mix, partA, mix15),
    detail::misc("mix.b", "Mix B", Scope::voice, EvalRate::perSample, Target::mix, partB, mix15),
    detail::misc("mix.ab", "Mix AB", Scope::voice, EvalRate::perSample, Target::mix, partAB, mix15),
    detail::misc("lfo.1.rate", "LFO 1 Rate", Scope::voice, EvalRate::gridHeld, Target::lfoRate, partA, knob10),
    detail::misc("lfo.1.gain", "LFO 1 Gain", Scope::voice, EvalRate::gridRamped, Target::lfoGain, partA, knob10),
    detail::misc("lfo.2.rate", "LFO 2 Rate", Scope::voice, EvalRate::gridHeld, Target::lfoRate, partB, knob10),
    detail::misc("lfo.2.gain", "LFO 2 Gain", Scope::voice, EvalRate::gridRamped, Target::lfoGain, partB, knob10),
    detail::misc("fx.distortion.amount", "Dist Amount", Scope::global, EvalRate::gridRamped, Target::distortionAmount,
                 partA, knob10),
};

/// The LFO's range as a source: half of gain x (wave + offset) (M6), where
/// a slew can carry the wave 0.25 past +-1 (lfo.hpp): +-1.125.
inline constexpr float lfoSourceLimit = 0.5f * (1.25f + 1.0f);
/// KeyTrack, (pitch - 9) / 120, for a pitch from MIDI 0 with the bend at
/// -24 and a master tune of 400 Hz, to MIDI 127 at +24 and 480 Hz.
inline constexpr float keytrackMin = -0.32f;
inline constexpr float keytrackMax = 1.22f;

/// Every source, "None" first: the misc selector's 11 (M2), of which ModEnv
/// 1/2 and LFO 1/2 also own two routes each.
inline constexpr std::array<SourceInfo, sourceCount> sourceRows{
    SourceInfo{.key = "none", .name = "None"},
    SourceInfo{
        .key = "velocity",
        .name = "Velocity",
        .scope = Scope::voice,
        .min = 0.0f,
        .max = 1.0f,
        .bound = BoundKind::linearEnds,
    },
    SourceInfo{
        .key = "mod_wheel",
        .name = "Mod Wheel",
        .scope = Scope::global,
        .min = 0.0f,
        .max = 1.0f,
        .bound = BoundKind::linearEnds,
    },
    SourceInfo{
        .key = "keytrack",
        .name = "Key Track",
        .scope = Scope::voice,
        .min = keytrackMin,
        .max = keytrackMax,
        .bound = BoundKind::linearEnds,
    },
    SourceInfo{
        .key = "amp_env.a",
        .name = "AmpEnv A",
        .scope = Scope::voice,
        .min = 0.0f,
        .max = 1.0f,
        .bound = BoundKind::envelope,
    },
    SourceInfo{
        .key = "amp_env.b",
        .name = "AmpEnv B",
        .scope = Scope::voice,
        .min = 0.0f,
        .max = 1.0f,
        .bound = BoundKind::envelope,
    },
    SourceInfo{
        .key = "mod_env.1",
        .name = "ModEnv 1",
        .scope = Scope::voice,
        .min = 0.0f,
        .max = 1.0f,
        .bound = BoundKind::envelope,
    },
    SourceInfo{
        .key = "mod_env.2",
        .name = "ModEnv 2",
        .scope = Scope::voice,
        .min = 0.0f,
        .max = 1.0f,
        .bound = BoundKind::envelope,
    },
    SourceInfo{
        .key = "lfo.1",
        .name = "LFO 1",
        .scope = Scope::voice,
        .min = -lfoSourceLimit,
        .max = lfoSourceLimit,
        .bound = BoundKind::lfo,
    },
    SourceInfo{
        .key = "lfo.2",
        .name = "LFO 2",
        .scope = Scope::voice,
        .min = -lfoSourceLimit,
        .max = lfoSourceLimit,
        .bound = BoundKind::lfo,
    },
    SourceInfo{
        .key = "aftertouch",
        .name = "Aftertouch",
        .scope = Scope::global,
        .min = 0.0f,
        .max = 1.0f,
        .bound = BoundKind::linearEnds,
    },
    SourceInfo{
        .key = "step_velocity",
        .name = "Step Velocity",
        .scope = Scope::global,
        .min = 0.0f,
        .max = 1.0f,
        .bound = BoundKind::linearEnds,
    },
};

// --- Compile-time checks, shared by every candidate ------------------------------------------

[[nodiscard]] constexpr bool keyCharactersValid(std::string_view key) noexcept
{
    const auto allowed = [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
    };
    return !key.empty() && std::ranges::all_of(key, allowed);
}

template <typename Row, std::size_t N>
[[nodiscard]] constexpr bool keysUnique(const std::array<Row, N>& rows) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!keyCharactersValid(rows[i].key))
        {
            return false;
        }
        for (std::size_t j = i + 1; j < N; ++j)
        {
            if (rows[i].key == rows[j].key)
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] constexpr bool destinationRowValid(const DestinationInfo& d) noexcept
{
    if (d.target == Target::none)
    {
        return d.parts == 0;
    }
    const bool partsOk = d.scope == Scope::global ? d.parts == partA : (d.parts >= partA && d.parts <= partAB);
    const bool rangeOk = d.range.isUnbounded() || d.range.min < d.range.max;
    const bool groupOk = d.group != Group::none;
    // A global destination can't be per sample per voice: it has no voice.
    const bool scopeOk = d.scope == Scope::voice || d.rate != EvalRate::perSample;
    return partsOk && rangeOk && groupOk && scopeOk && d.unitsPerAmount > 0.0f && !d.name.empty();
}

[[nodiscard]] constexpr bool sourceRowValid(const SourceInfo& s) noexcept
{
    if (s.bound == BoundKind::none)
    {
        return s.min <= 0.0f && s.max >= 0.0f;
    }
    return s.min < s.max && !s.name.empty();
}

template <std::size_t N>
[[nodiscard]] constexpr bool destinationsValid(const std::array<DestinationInfo, N>& rows) noexcept
{
    for (const auto& d : rows)
    {
        if (!destinationRowValid(d))
        {
            return false;
        }
    }
    return keysUnique(rows) && rows[0].target == Target::none;
}

template <std::size_t N>
[[nodiscard]] constexpr bool sourcesValid(const std::array<SourceInfo, N>& rows) noexcept
{
    for (const auto& s : rows)
    {
        if (!sourceRowValid(s))
        {
            return false;
        }
    }
    return keysUnique(rows) && rows[0].bound == BoundKind::none;
}

static_assert(destinationsValid(destinationRows));
static_assert(sourcesValid(sourceRows));

/// The per-voice sums the routes produce: per target, per part (or LFO).
/// A global target uses index 0.
using ModSums = std::array<std::array<float, 2>, targetCount>;

/// A slot's choice, as a preset or the panel sets it: indices into the rows.
struct Route
{
    std::uint8_t source = 0;
    std::uint8_t destination = 0;
    float amount = 0.0f; ///< -10 to +10
};

inline constexpr std::size_t routeCount = 16;

} // namespace anoesis::test::modulation
