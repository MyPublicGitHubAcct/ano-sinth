// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The registry candidates (M1.5 task 2, stage 1; docs/dsp/modulation.md,
// deliverable 1): three ways to hold the destination and source descriptors
// (modulation_model.hpp) so that Phase 2 adds rows without editing a switch,
// with no heap and no RTTI, checkable at compile time, and small enough for
// STM32. Each does the same three jobs, which the report and the benchmarks
// compare:
//
// - compile(): a patch's 16 routes into the list the voice evaluates (an
//   empty slot or an amount of 0 drops out, so it costs nothing);
// - accumulate(): amount x source added into the per-target, per-part sums;
// - bound(): a source's interval over a segment, for the skip test.
//
// (a) table: constexpr descriptor tables indexed by an enum. A route
//     compiles to plain data (source, target, parts, amount); evaluation is
//     one loop with no dispatch; a source's bound is a switch on its *kind*
//     (BoundKind), not on the source, so a new source of an existing kind is
//     a row.
// (b) functions: each descriptor carries functions (accumulate into its
//     target, a source's bound). A route compiles to (source, function,
//     amount); evaluation calls through a pointer per route.
// (c) types: each destination and source is a type with static data and
//     functions, listed in a TypeList; a runtime choice (a preset's) is
//     dispatched by a fold over the list.
//
// Prototypes for measurement, not library code.

#pragma once

#include "modulation_model.hpp"
#include "source_bounds.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace anoesis::test::modulation
{

/// What a source's bound reads, filled by the voice at a grid point.
struct BoundInputs
{
    Interval ends;          ///< a linear source's first and last value over the segment
    EnvelopeState envelope; ///< an envelope source's state
    Interval lfo;           ///< an LFO source's interval (already the source's units)
};

/// Adds v to target t on the parts in `parts`.
constexpr void addTo(ModSums& sums, Target t, std::uint8_t parts, float v) noexcept
{
    auto& row = sums[static_cast<std::size_t>(t)];
    if ((parts & partA) != 0)
    {
        row[0] += v;
    }
    if ((parts & partB) != 0)
    {
        row[1] += v;
    }
}

/// The interval for `kind` (shared by the candidates whose bound is data).
constexpr Interval boundOfKind(BoundKind kind, const BoundInputs& in) noexcept
{
    switch (kind)
    {
    case BoundKind::none:
        return {};
    case BoundKind::linearEnds:
        return in.ends;
    case BoundKind::envelope:
        return envelopeStageBound(in.envelope);
    case BoundKind::lfo:
        return in.lfo;
    }
    return {};
}

/// True if `sums` has exactly v at target t's parts and 0 everywhere else:
/// for compile-time checks of a destination's behaviour.
constexpr bool addsExactly(const ModSums& sums, Target t, std::uint8_t parts, float v) noexcept
{
    for (std::size_t target = 0; target < targetCount; ++target)
    {
        for (std::size_t p = 0; p < 2; ++p)
        {
            const bool here = target == static_cast<std::size_t>(t) && t != Target::none &&
                              (parts & static_cast<std::uint8_t>(1U << p)) != 0;
            const float want = here ? v : 0.0f;
            if (sums[target][p] < want || sums[target][p] > want)
            {
                return false;
            }
        }
    }
    return true;
}

// --- (a) constexpr tables indexed by an enum ---------------------------------------------

namespace table_registry
{

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

inline constexpr const auto& destinations = destinationRows;
inline constexpr const auto& sources = sourceRows;

static_assert(static_cast<std::size_t>(Destination::count) == destinations.size());
static_assert(static_cast<std::size_t>(Source::count) == sources.size());
// The enum and the rows agree where a consumer names one.
static_assert(destinations[static_cast<std::size_t>(Destination::mixAB)].key == "mix.ab");
static_assert(destinations[static_cast<std::size_t>(Destination::lfo2Gain)].key == "lfo.2.gain");
static_assert(sources[static_cast<std::size_t>(Source::lfo2)].key == "lfo.2");

[[nodiscard]] constexpr const DestinationInfo& info(Destination d) noexcept
{
    const auto i = static_cast<std::size_t>(d);
    return destinations[i < destinations.size() ? i : 0];
}

[[nodiscard]] constexpr std::optional<Destination> findDestination(std::string_view key) noexcept
{
    for (std::size_t i = 0; i < destinations.size(); ++i)
    {
        if (destinations[i].key == key)
        {
            return static_cast<Destination>(i);
        }
    }
    return std::nullopt;
}

static_assert(findDestination("osc.pitch.ab") == Destination::pitchAB);
static_assert(!findDestination("Pitch AB").has_value()); // the label is not the ID

struct CompiledRoute
{
    std::uint8_t source = 0;
    Target target = Target::none;
    std::uint8_t parts = 0;
    float amount = 0.0f; ///< amount x the destination's units per amount
};

struct Compiled
{
    std::array<CompiledRoute, routeCount> routes{};
    std::size_t count = 0;
};

[[nodiscard]] constexpr Compiled compile(const std::array<Route, routeCount>& patch) noexcept
{
    Compiled c;
    for (const auto& r : patch)
    {
        const bool valid = r.source > 0 && r.source < sources.size() && r.destination > 0 &&
                           r.destination < destinations.size() && (r.amount < 0.0f || r.amount > 0.0f);
        if (!valid)
        {
            continue;
        }
        const auto& d = destinations[r.destination];
        c.routes[c.count] = {
            .source = r.source,
            .target = d.target,
            .parts = d.parts,
            .amount = r.amount * d.unitsPerAmount,
        };
        ++c.count;
    }
    return c;
}

/// sums += each route's amount x its source's value.
constexpr void accumulate(const Compiled& c, std::span<const float, sourceCount> values, ModSums& sums) noexcept
{
    for (std::size_t i = 0; i < c.count; ++i)
    {
        const auto& r = c.routes[i];
        addTo(sums, r.target, r.parts, r.amount * values[r.source]);
    }
}

[[nodiscard]] constexpr Interval bound(std::size_t source, const BoundInputs& in) noexcept
{
    return boundOfKind(sources[source < sources.size() ? source : 0].bound, in);
}

/// (a) as libs/dsp would hold it: each destination's DSP fields only. The
/// keys and labels are for presets and the interface, so they live with
/// the parameter table (M1.9); a compile-time check there ties each key to
/// its enum value.
struct DspDestination
{
    Target target = Target::none;
    std::uint8_t parts = 0;
    Scope scope = Scope::voice;
    EvalRate rate = EvalRate::perSample;
    float unitsPerAmount = 1.0f;
    KnobRange range{};
};

inline constexpr auto dspDestinations = []
{
    std::array<DspDestination, destinationCount> t{};
    for (std::size_t i = 0; i < destinationCount; ++i)
    {
        const auto& d = destinationRows[i];
        t[i] = {
            .target = d.target,
            .parts = d.parts,
            .scope = d.scope,
            .rate = d.rate,
            .unitsPerAmount = d.unitsPerAmount,
            .range = d.range,
        };
    }
    return t;
}();

inline constexpr auto dspSourceKinds = []
{
    std::array<BoundKind, sourceCount> t{};
    for (std::size_t i = 0; i < sourceCount; ++i)
    {
        t[i] = sourceRows[i].bound;
    }
    return t;
}();

/// compile() on the DSP fields alone.
[[nodiscard]] constexpr Compiled compileDsp(const std::array<Route, routeCount>& patch) noexcept
{
    Compiled c;
    for (const auto& r : patch)
    {
        const bool valid = r.source > 0 && r.source < sourceCount && r.destination > 0 &&
                           r.destination < destinationCount && (r.amount < 0.0f || r.amount > 0.0f);
        if (!valid)
        {
            continue;
        }
        const auto& d = dspDestinations[r.destination];
        c.routes[c.count] = {
            .source = r.source,
            .target = d.target,
            .parts = d.parts,
            .amount = r.amount * d.unitsPerAmount,
        };
        ++c.count;
    }
    return c;
}

[[nodiscard]] constexpr Interval boundDsp(std::size_t source, const BoundInputs& in) noexcept
{
    return boundOfKind(dspSourceKinds[source < sourceCount ? source : 0], in);
}

// Compile-time: a route to Volume AB at +2 from velocity 0.5 adds 1 to both
// parts' volume, and an empty slot or a 0 amount adds nothing.
static_assert(
    []
    {
        std::array<Route, routeCount> patch{};
        patch[0] = {
            .source = static_cast<std::uint8_t>(Source::velocity),
            .destination = static_cast<std::uint8_t>(Destination::volumeAB),
            .amount = 2.0f,
        };
        patch[1] = {
            .source = static_cast<std::uint8_t>(Source::lfo1),
            .destination = static_cast<std::uint8_t>(Destination::pitchA),
            .amount = 0.0f,
        };
        patch[2] = {.source = 0, .destination = static_cast<std::uint8_t>(Destination::mixA), .amount = 5.0f};
        const auto c = compile(patch);
        std::array<float, sourceCount> values{};
        values[static_cast<std::size_t>(Source::velocity)] = 0.5f;
        ModSums sums{};
        accumulate(c, values, sums);
        return c.count == 1 && addsExactly(sums, Target::volume, partAB, 1.0f);
    }());

} // namespace table_registry

// --- (b) descriptors with a function per entry ---------------------------------------------

namespace function_registry
{

using Accumulate = void (*)(ModSums&, float) noexcept;
using Bound = Interval (*)(const BoundInputs&) noexcept;

struct DestinationEntry
{
    const DestinationInfo* info = nullptr;
    Accumulate accumulate = nullptr;
};

struct SourceEntry
{
    const SourceInfo* info = nullptr;
    Bound bound = nullptr;
};

template <std::size_t Row>
constexpr void accumulateRow(ModSums& sums, float v) noexcept
{
    constexpr const DestinationInfo& d = destinationRows[Row];
    addTo(sums, d.target, d.parts, v * d.unitsPerAmount);
}

/// One entry per row. Written as a list so that a Phase 2 row with its own
/// behaviour (a function that isn't accumulateRow) is one more line.
inline constexpr std::array<DestinationEntry, destinationCount> destinations{
    {
        {.info = destinationRows.data(), .accumulate = &accumulateRow<0>},
        {.info = &destinationRows[1], .accumulate = &accumulateRow<1>},
        {.info = &destinationRows[2], .accumulate = &accumulateRow<2>},
        {.info = &destinationRows[3], .accumulate = &accumulateRow<3>},
        {.info = &destinationRows[4], .accumulate = &accumulateRow<4>},
        {.info = &destinationRows[5], .accumulate = &accumulateRow<5>},
        {.info = &destinationRows[6], .accumulate = &accumulateRow<6>},
        {.info = &destinationRows[7], .accumulate = &accumulateRow<7>},
        {.info = &destinationRows[8], .accumulate = &accumulateRow<8>},
        {.info = &destinationRows[9], .accumulate = &accumulateRow<9>},
        {.info = &destinationRows[10], .accumulate = &accumulateRow<10>},
        {.info = &destinationRows[11], .accumulate = &accumulateRow<11>},
        {.info = &destinationRows[12], .accumulate = &accumulateRow<12>},
        {.info = &destinationRows[13], .accumulate = &accumulateRow<13>},
        {.info = &destinationRows[14], .accumulate = &accumulateRow<14>},
        {.info = &destinationRows[15], .accumulate = &accumulateRow<15>},
        {.info = &destinationRows[16], .accumulate = &accumulateRow<16>},
        {.info = &destinationRows[17], .accumulate = &accumulateRow<17>},
        {.info = &destinationRows[18], .accumulate = &accumulateRow<18>},
        {.info = &destinationRows[19], .accumulate = &accumulateRow<19>},
        {.info = &destinationRows[20], .accumulate = &accumulateRow<20>},
        {.info = &destinationRows[21], .accumulate = &accumulateRow<21>},
        {.info = &destinationRows[22], .accumulate = &accumulateRow<22>},
        {.info = &destinationRows[23], .accumulate = &accumulateRow<23>},
        {.info = &destinationRows[24], .accumulate = &accumulateRow<24>},
        {.info = &destinationRows[25], .accumulate = &accumulateRow<25>},
        {.info = &destinationRows[26], .accumulate = &accumulateRow<26>},
        {.info = &destinationRows[27], .accumulate = &accumulateRow<27>},
    },
};

constexpr Interval noBound(const BoundInputs& /*in*/) noexcept
{
    return {};
}
constexpr Interval endsBound(const BoundInputs& in) noexcept
{
    return in.ends;
}
constexpr Interval envelopeBound(const BoundInputs& in) noexcept
{
    return envelopeStageBound(in.envelope);
}
constexpr Interval lfoBound(const BoundInputs& in) noexcept
{
    return in.lfo;
}

inline constexpr std::array<SourceEntry, sourceCount> sources{
    {
        {.info = sourceRows.data(), .bound = &noBound},
        {.info = &sourceRows[1], .bound = &endsBound},
        {.info = &sourceRows[2], .bound = &endsBound},
        {.info = &sourceRows[3], .bound = &endsBound},
        {.info = &sourceRows[4], .bound = &envelopeBound},
        {.info = &sourceRows[5], .bound = &envelopeBound},
        {.info = &sourceRows[6], .bound = &envelopeBound},
        {.info = &sourceRows[7], .bound = &envelopeBound},
        {.info = &sourceRows[8], .bound = &lfoBound},
        {.info = &sourceRows[9], .bound = &lfoBound},
        {.info = &sourceRows[10], .bound = &endsBound},
        {.info = &sourceRows[11], .bound = &endsBound},
    },
};

// Compile-time: every entry points at its own row and has its functions,
// and each destination's function adds into its own target and parts only.
static_assert(
    []
    {
        for (std::size_t i = 0; i < destinations.size(); ++i)
        {
            const auto& e = destinations[i];
            if (e.info != &destinationRows[i] || e.accumulate == nullptr)
            {
                return false;
            }
            ModSums sums{};
            e.accumulate(sums, 1.0f);
            if (!addsExactly(sums, e.info->target, e.info->parts, e.info->unitsPerAmount))
            {
                return false;
            }
        }
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            if (sources[i].info != &sourceRows[i] || sources[i].bound == nullptr)
            {
                return false;
            }
        }
        return true;
    }());

struct CompiledRoute
{
    std::uint8_t source = 0;
    Accumulate accumulate = nullptr;
    float amount = 0.0f;
};

struct Compiled
{
    std::array<CompiledRoute, routeCount> routes{};
    std::size_t count = 0;
};

[[nodiscard]] constexpr Compiled compile(const std::array<Route, routeCount>& patch) noexcept
{
    Compiled c;
    for (const auto& r : patch)
    {
        const bool valid = r.source > 0 && r.source < sources.size() && r.destination > 0 &&
                           r.destination < destinations.size() && (r.amount < 0.0f || r.amount > 0.0f);
        if (!valid)
        {
            continue;
        }
        c.routes[c.count] = {
            .source = r.source,
            .accumulate = destinations[r.destination].accumulate,
            .amount = r.amount,
        };
        ++c.count;
    }
    return c;
}

constexpr void accumulate(const Compiled& c, std::span<const float, sourceCount> values, ModSums& sums) noexcept
{
    for (std::size_t i = 0; i < c.count; ++i)
    {
        const auto& r = c.routes[i];
        r.accumulate(sums, r.amount * values[r.source]);
    }
}

[[nodiscard]] constexpr Interval bound(std::size_t source, const BoundInputs& in) noexcept
{
    return sources[source < sources.size() ? source : 0].bound(in);
}

[[nodiscard]] constexpr std::optional<std::size_t> findDestination(std::string_view key) noexcept
{
    for (std::size_t i = 0; i < destinations.size(); ++i)
    {
        if (destinations[i].info->key == key)
        {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace function_registry

// --- (c) a compile-time type list -------------------------------------------------------

namespace type_registry
{

template <typename... Ts>
struct TypeList
{
    static constexpr std::size_t size = sizeof...(Ts);
};

/// The behaviour every plain destination shares: add into its target and
/// parts. A Phase 2 destination with other behaviour defines its own
/// accumulate().
template <std::size_t Row>
struct DestinationRow
{
    static constexpr const DestinationInfo& info = destinationRows[Row];
    static constexpr void accumulate(ModSums& sums, float v) noexcept
    {
        addTo(sums, info.target, info.parts, v * info.unitsPerAmount);
    }
};

// One type per destination: what Phase 2 adds, and what a consumer can name
// at compile time (indexOf<VolumeA>).
struct NoDestination : DestinationRow<0>
{
};
struct VolumeA : DestinationRow<1>
{
};
struct VolumeB : DestinationRow<2>
{
};
struct VolumeAB : DestinationRow<3>
{
};
struct PitchA : DestinationRow<4>
{
};
struct PitchB : DestinationRow<5>
{
};
struct PitchAB : DestinationRow<6>
{
};
struct PhaseA : DestinationRow<7>
{
};
struct PhaseB : DestinationRow<8>
{
};
struct PhaseAB : DestinationRow<9>
{
};
struct PanA : DestinationRow<10>
{
};
struct PanB : DestinationRow<11>
{
};
struct PanAB : DestinationRow<12>
{
};
struct CutoffA : DestinationRow<13>
{
};
struct CutoffB : DestinationRow<14>
{
};
struct CutoffAB : DestinationRow<15>
{
};
struct ResonanceA : DestinationRow<16>
{
};
struct ResonanceB : DestinationRow<17>
{
};
struct ResonanceAB : DestinationRow<18>
{
};
struct PhaserFrequency : DestinationRow<19>
{
};
struct MixA : DestinationRow<20>
{
};
struct MixB : DestinationRow<21>
{
};
struct MixAB : DestinationRow<22>
{
};
struct Lfo1Rate : DestinationRow<23>
{
};
struct Lfo1Gain : DestinationRow<24>
{
};
struct Lfo2Rate : DestinationRow<25>
{
};
struct Lfo2Gain : DestinationRow<26>
{
};
struct DistortionAmount : DestinationRow<27>
{
};

using Destinations =
    TypeList<NoDestination, VolumeA, VolumeB, VolumeAB, PitchA, PitchB, PitchAB, PhaseA, PhaseB, PhaseAB, PanA, PanB,
             PanAB, CutoffA, CutoffB, CutoffAB, ResonanceA, ResonanceB, ResonanceAB, PhaserFrequency, MixA, MixB, MixAB,
             Lfo1Rate, Lfo1Gain, Lfo2Rate, Lfo2Gain, DistortionAmount>;

template <std::size_t Row, typename BoundOf>
struct SourceRow
{
    static constexpr const SourceInfo& info = sourceRows[Row];
    static constexpr Interval bound(const BoundInputs& in) noexcept
    {
        return BoundOf::of(in);
    }
};
struct NoBound
{
    static constexpr Interval of(const BoundInputs& /*in*/) noexcept
    {
        return {};
    }
};
struct EndsBound
{
    static constexpr Interval of(const BoundInputs& in) noexcept
    {
        return in.ends;
    }
};
struct EnvelopeBound
{
    static constexpr Interval of(const BoundInputs& in) noexcept
    {
        return envelopeStageBound(in.envelope);
    }
};
struct LfoBound
{
    static constexpr Interval of(const BoundInputs& in) noexcept
    {
        return in.lfo;
    }
};

struct NoSource : SourceRow<0, NoBound>
{
};
struct Velocity : SourceRow<1, EndsBound>
{
};
struct ModWheel : SourceRow<2, EndsBound>
{
};
struct KeyTrack : SourceRow<3, EndsBound>
{
};
struct AmpEnvA : SourceRow<4, EnvelopeBound>
{
};
struct AmpEnvB : SourceRow<5, EnvelopeBound>
{
};
struct ModEnv1 : SourceRow<6, EnvelopeBound>
{
};
struct ModEnv2 : SourceRow<7, EnvelopeBound>
{
};
struct Lfo1 : SourceRow<8, LfoBound>
{
};
struct Lfo2 : SourceRow<9, LfoBound>
{
};
struct Aftertouch : SourceRow<10, EndsBound>
{
};
struct StepVelocity : SourceRow<11, EndsBound>
{
};

using Sources = TypeList<NoSource, Velocity, ModWheel, KeyTrack, AmpEnvA, AmpEnvB, ModEnv1, ModEnv2, Lfo1, Lfo2,
                         Aftertouch, StepVelocity>;

static_assert(Destinations::size == destinationCount);
static_assert(Sources::size == sourceCount);

/// The index of T in the list.
template <typename T, typename List>
struct IndexOf;
template <typename T, typename... Ts>
struct IndexOf<T, TypeList<Ts...>>
{
    static constexpr std::size_t value = []
    {
        std::size_t i = 0;
        std::size_t found = sizeof...(Ts);
        ((std::is_same_v<T, Ts> ? (found = i, ++i) : ++i), ...);
        return found;
    }();
};
template <typename T>
inline constexpr std::size_t indexOf = IndexOf<T, Destinations>::value;

static_assert(indexOf<MixAB> == 22);
static_assert(MixAB::info.key == "mix.ab");

template <typename List>
struct Dispatch;
template <typename... Ts>
struct Dispatch<TypeList<Ts...>>
{
    static constexpr std::array<const DestinationInfo*, sizeof...(Ts)> infos{&Ts::info...};

    /// Ts[i]::accumulate(sums, v), through a fold the compiler can turn
    /// into a jump table.
    static constexpr void accumulate(std::size_t i, ModSums& sums, float v) noexcept
    {
        std::size_t k = 0;
        (void)((k++ == i ? (Ts::accumulate(sums, v), true) : false) || ...);
    }

    /// Every type's accumulate adds into its own target only.
    static constexpr bool valid() noexcept
    {
        return (
            []
            {
                ModSums sums{};
                Ts::accumulate(sums, 1.0f);
                return addsExactly(sums, Ts::info.target, Ts::info.parts, Ts::info.unitsPerAmount);
            }() &&
            ...);
    }
};
template <typename... Ts>
struct SourceDispatch;
template <typename... Ts>
struct SourceDispatch<TypeList<Ts...>>
{
    static constexpr Interval bound(std::size_t i, const BoundInputs& in) noexcept
    {
        Interval out{};
        std::size_t k = 0;
        (void)((k++ == i ? (out = Ts::bound(in), true) : false) || ...);
        return out;
    }
};

static_assert(Dispatch<Destinations>::valid());

struct Compiled
{
    struct Entry
    {
        std::uint8_t source = 0;
        std::uint8_t destination = 0;
        float amount = 0.0f;
    };
    std::array<Entry, routeCount> routes{};
    std::size_t count = 0;
};

[[nodiscard]] constexpr Compiled compile(const std::array<Route, routeCount>& patch) noexcept
{
    Compiled c;
    for (const auto& r : patch)
    {
        const bool valid = r.source > 0 && r.source < Sources::size && r.destination > 0 &&
                           r.destination < Destinations::size && (r.amount < 0.0f || r.amount > 0.0f);
        if (!valid)
        {
            continue;
        }
        c.routes[c.count] = {.source = r.source, .destination = r.destination, .amount = r.amount};
        ++c.count;
    }
    return c;
}

constexpr void accumulate(const Compiled& c, std::span<const float, sourceCount> values, ModSums& sums) noexcept
{
    for (std::size_t i = 0; i < c.count; ++i)
    {
        const auto& r = c.routes[i];
        Dispatch<Destinations>::accumulate(r.destination, sums, r.amount * values[r.source]);
    }
}

[[nodiscard]] constexpr Interval bound(std::size_t source, const BoundInputs& in) noexcept
{
    return SourceDispatch<Sources>::bound(source, in);
}

[[nodiscard]] constexpr std::optional<std::size_t> findDestination(std::string_view key) noexcept
{
    const auto& infos = Dispatch<Destinations>::infos;
    for (std::size_t i = 0; i < infos.size(); ++i)
    {
        if (infos[i]->key == key)
        {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace type_registry

} // namespace anoesis::test::modulation
