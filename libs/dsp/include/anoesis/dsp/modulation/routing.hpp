// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

/// Modulation routing (M1.5 task 2, docs/dsp/modulation.md): a patch's 16
/// slots, compiled into the list of live routes the voice evaluates, and
/// accumulated into one sum per target and part.
///
/// - A slot is a source, a destination and an amount from -10 to +10. The
///   16 are two per mod envelope, two per LFO, and two for each of the four
///   misc sources (slotOwners); which source a slot may pick is the panel's
///   business, so every slot takes any source.
/// - compile() keeps only live routes: an empty slot (either end "None", or
///   a value outside the enums) or an amount of 0 drops out, so no routes is
///   an empty list, which adds an exact 0 and costs nothing. A live route
///   holds its source, its destination's target and parts, and the amount
///   already in the target's units (amount x units per amount). The amount
///   is held to [-10, +10].
/// - compile(slots, scope) keeps only the routes of one scope (M1.5 task 5):
///   a voice compiles Scope::voice, the engine Scope::global (the master
///   effects' destinations, M1.7), from the same 16 slots.
/// - accumulate() adds amount x the source's value into the target's sum on
///   each part the route acts on: AB is both parts, except Cutoff AB and
///   Reso AB, which are the Filter Control's own targets (registry.hpp).
///   Routes add in slot order, so two routes to Mix A make "+15 (in 2 mod
///   dest slots)".
///
/// Pure constexpr: no state, no allocation, real-time safe.
/// Doc: docs/dsp/modulation.md
#include <anoesis/dsp/modulation/registry.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace anoesis::dsp::modulation
{

inline constexpr std::size_t slotCount = 16;
inline constexpr float amountMax = 10.0f;

/// One modulation slot, as a preset or the panel sets it.
struct Slot
{
    Source source = Source::none;
    Destination destination = Destination::none;
    float amount = 0.0f; ///< -10 to +10
};

/// The 16 slots. The default is the init patch: every slot empty.
using Slots = std::array<Slot, slotCount>;

/// Which source owns each slot on the panel: 0-1 ModEnv 1, 2-3 ModEnv 2, 4-5
/// LFO 1, 6-7 LFO 2; 8-15 the misc slots (none: any misc source).
inline constexpr std::array<Source, slotCount> slotOwners{
    Source::modEnv1, Source::modEnv1, Source::modEnv2, Source::modEnv2, Source::lfo1, Source::lfo1,
    Source::lfo2,    Source::lfo2,    Source::none,    Source::none,    Source::none, Source::none,
    Source::none,    Source::none,    Source::none,    Source::none,
};

/// A live route: what accumulate() reads.
struct CompiledRoute
{
    Source source = Source::none;
    Target target = Target::none;
    Parts parts = Parts::none;
    float amount = 0.0f; ///< amount x the destination's units per amount
};

/// A patch's live routes, in slot order.
struct CompiledRoutes
{
    std::array<CompiledRoute, slotCount> routes{};
    std::size_t count = 0;
    /// Per target, a bit per part a route acts on.
    std::array<std::uint8_t, targetCount> routed{};
    /// Per source, whether a route reads it.
    std::array<bool, sourceCount> used{};

    [[nodiscard]] constexpr std::span<const CompiledRoute> live() const noexcept
    {
        return std::span{routes}.first(count);
    }
    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return count == 0;
    }
    /// True if a route acts on target t's part `part`.
    [[nodiscard]] constexpr bool routesTo(Target t, std::size_t part) const noexcept
    {
        const auto i = static_cast<std::size_t>(t);
        return i < targetCount && part < partCount && (routed[i] & (1U << part)) != 0;
    }
    /// True if a route reads source s.
    [[nodiscard]] constexpr bool uses(Source s) const noexcept
    {
        const auto i = static_cast<std::size_t>(s);
        return i < sourceCount && used[i];
    }
};

/// An amount held to [-10, +10].
[[nodiscard]] constexpr float clampAmount(float amount) noexcept
{
    if (amount > amountMax)
    {
        return amountMax;
    }
    return amount < -amountMax ? -amountMax : amount;
}

namespace detail
{
/// The slots' live routes, in slot order: every scope, or only `scope`.
[[nodiscard]] constexpr CompiledRoutes compileRoutes(const Slots& slots, bool everyScope, Scope scope) noexcept
{
    CompiledRoutes c;
    for (const auto& slot : slots)
    {
        const auto s = static_cast<std::size_t>(slot.source);
        const auto d = static_cast<std::size_t>(slot.destination);
        // Not-equal-to-0 written so that NaN drops out too.
        const bool live =
            s > 0 && s < sourceCount && d > 0 && d < destinationCount && (slot.amount < 0.0f || slot.amount > 0.0f);
        if (!live)
        {
            continue;
        }
        const auto& row = destinations[d];
        if (!everyScope && row.scope != scope)
        {
            continue;
        }
        const float amount = clampAmount(slot.amount);
        c.routes[c.count] = {
            .source = slot.source,
            .target = row.target,
            .parts = row.parts,
            .amount = amount * row.unitsPerAmount,
        };
        ++c.count;
        c.routed[static_cast<std::size_t>(row.target)] |= static_cast<std::uint8_t>(row.parts);
        c.used[s] = true;
    }
    return c;
}
} // namespace detail

/// The slots' live routes, in slot order, of every scope: what the voices
/// and the engine compile between them.
[[nodiscard]] constexpr CompiledRoutes compile(const Slots& slots) noexcept
{
    return detail::compileRoutes(slots, true, Scope::voice);
}

/// The live routes of one scope, in slot order (M1.5 task 5, decided
/// 2026-11-23): Scope::voice, the routes a voice reads; Scope::global, the
/// master effects' (the engine's, M1.7). A voice never compiles a global
/// route, so it never runs a source, crossfades or leaves its no-routes
/// path for a route it doesn't read.
[[nodiscard]] constexpr CompiledRoutes compile(const Slots& slots, Scope scope) noexcept
{
    return detail::compileRoutes(slots, false, scope);
}

/// True if a and b are the same routes: in the same order, each amount to
/// the bit (routed and used follow from the routes). A recompile that gives
/// the same routes is no change of the routes (M1.5 task 5): a slot set to
/// what it holds, or a change to a route of another scope, starts no
/// crossfade.
[[nodiscard]] constexpr bool sameRoutes(const CompiledRoutes& a, const CompiledRoutes& b) noexcept
{
    if (a.count != b.count)
    {
        return false;
    }
    for (std::size_t i = 0; i < a.count; ++i)
    {
        const auto& x = a.routes[i];
        const auto& y = b.routes[i];
        if (x.source != y.source || x.target != y.target || x.parts != y.parts ||
            std::bit_cast<std::uint32_t>(x.amount) != std::bit_cast<std::uint32_t>(y.amount))
        {
            return false;
        }
    }
    return true;
}

/// The sums the routes produce: per target, per part (or LFO). A global
/// target uses part 0.
struct Sums
{
    std::array<std::array<float, partCount>, targetCount> values{};

    /// Target t's sum on part `part`; 0 out of range.
    [[nodiscard]] constexpr float at(Target t, std::size_t part) const noexcept
    {
        const auto i = static_cast<std::size_t>(t);
        return i < targetCount && part < partCount ? values[i][part] : 0.0f;
    }

    /// Adds v to target t on each of `parts`.
    constexpr void add(Target t, Parts parts, float v) noexcept
    {
        auto& row = values[static_cast<std::size_t>(t)];
        if (includes(parts, 0))
        {
            row[0] += v;
        }
        if (includes(parts, 1))
        {
            row[1] += v;
        }
    }
};

/// Each source's value, indexed by Source.
using SourceValues = std::array<float, sourceCount>;

/// sums += each route's amount x its source's value.
constexpr void accumulate(const CompiledRoutes& routes, const SourceValues& values, Sums& sums) noexcept
{
    for (const auto& r : routes.live())
    {
        sums.add(r.target, r.parts, r.amount * values[static_cast<std::size_t>(r.source)]);
    }
}

} // namespace anoesis::dsp::modulation
