// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Tests for modulation routing and the bounds as built (libs/dsp
// modulation/routing.hpp, source_bound.hpp; docs/dsp/modulation.md, as
// built): compile() and accumulate() are the stage-1 prototype's registry
// (a) bit for bit (registry_candidates.hpp, table_registry), and so are each
// source's bound and a target's bound from its routes, on 400 random
// patches; an empty slot, an amount of 0 and the init patch add an exact 0;
// AB is both parts except Cutoff AB and Reso AB; the amount is held to
// [-10, +10]; nothing allocates; and the same jobs run as constant
// expressions.

#include "registry_candidates.hpp"
#include "source_bounds.hpp"

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/modulation/routing.hpp>
#include <anoesis/dsp/modulation/source_bound.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <tuple>

namespace mod = anoesis::dsp::modulation;
namespace proto = anoesis::test::modulation;
using anoesis::dsp::AdsrStage;
using anoesis::dsp::Random;

namespace
{

bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

/// A random patch as the prototype's tests draw it: about a quarter of the
/// slots empty, a fifth at amount 0, the rest -10 to +10.
std::array<proto::Route, proto::routeCount> randomPrototypePatch(Random& random)
{
    std::array<proto::Route, proto::routeCount> patch{};
    for (auto& r : patch)
    {
        const auto s = random.nextUint32();
        r.source = static_cast<std::uint8_t>(s % proto::sourceCount);
        r.destination = static_cast<std::uint8_t>((s >> 8U) % proto::destinationCount);
        r.amount = (s >> 16U) % 5 == 0 ? 0.0f : 10.0f * random.nextBipolar();
    }
    return patch;
}

mod::Slots toSlots(const std::array<proto::Route, proto::routeCount>& patch)
{
    mod::Slots slots{};
    for (std::size_t i = 0; i < mod::slotCount; ++i)
    {
        slots[i] = {
            .source = static_cast<mod::Source>(patch[i].source),
            .destination = static_cast<mod::Destination>(patch[i].destination),
            .amount = patch[i].amount,
        };
    }
    return slots;
}

proto::Interval toPrototype(mod::Interval x)
{
    return {.lo = x.lo, .hi = x.hi};
}

mod::Interval randomInterval(Random& random)
{
    const float a = random.nextBipolar();
    const float b = random.nextBipolar();
    return {.lo = std::fmin(a, b), .hi = std::fmax(a, b)};
}

mod::BoundInputs randomInputs(Random& random)
{
    return {
        .ends = randomInterval(random),
        .envelope =
            {
                .stage = static_cast<AdsrStage>(random.nextUint32() % 5),
                .level = random.nextUnipolar(),
                .sustain = random.nextUnipolar(),
            },
        .lfo = randomInterval(random),
    };
}

proto::BoundInputs toPrototype(const mod::BoundInputs& in)
{
    return {
        .ends = toPrototype(in.ends),
        .envelope = {.stage = in.envelope.stage, .level = in.envelope.level, .sustain = in.envelope.sustain},
        .lfo = toPrototype(in.lfo),
    };
}

/// The prototype's target bound (ModVoice::modulationBound(), and its lo the
/// same way): amount x each route's source interval, summed in route order.
proto::Interval prototypeTargetBound(const proto::table_registry::Compiled& c, proto::Target t, std::size_t part,
                                     const std::array<proto::Interval, proto::sourceCount>& bounds)
{
    proto::Interval sum{};
    for (std::size_t i = 0; i < c.count; ++i)
    {
        const auto& r = c.routes[i];
        if (r.target == t && (r.parts & (1U << part)) != 0)
        {
            const auto x = proto::scaled(bounds[r.source], r.amount);
            sum.lo += x.lo;
            sum.hi += x.hi;
        }
    }
    return sum;
}

bool allPositiveZero(const mod::Sums& sums)
{
    for (const auto& row : sums.values)
    {
        for (const float v : row)
        {
            if (std::bit_cast<std::uint32_t>(v) != 0)
            {
                return false;
            }
        }
    }
    return true;
}

// --- Compile-time: the jobs run as constant expressions ---------------------------------------

// A route to Volume AB at +2 from velocity 0.5 adds 1 to both parts' volume;
// an empty slot and an amount of 0 add nothing.
static_assert(
    []
    {
        mod::Slots slots{};
        slots[0] = {.source = mod::Source::velocity, .destination = mod::Destination::volumeAB, .amount = 2.0f};
        slots[1] = {.source = mod::Source::lfo1, .destination = mod::Destination::pitchA, .amount = 0.0f};
        slots[2] = {.source = mod::Source::none, .destination = mod::Destination::mixA, .amount = 5.0f};
        slots[3] = {.source = mod::Source::velocity, .destination = mod::Destination::none, .amount = 5.0f};
        const auto c = mod::compile(slots);
        mod::SourceValues values{};
        values[static_cast<std::size_t>(mod::Source::velocity)] = 0.5f;
        mod::Sums sums{};
        mod::accumulate(c, values, sums);
        const auto v = [&](mod::Target t, std::size_t p)
        {
            return sums.at(t, p);
        };
        return c.count == 1 && v(mod::Target::volume, 0) >= 1.0f && v(mod::Target::volume, 0) <= 1.0f &&
               v(mod::Target::volume, 1) >= 1.0f && v(mod::Target::volume, 1) <= 1.0f &&
               c.uses(mod::Source::velocity) && !c.uses(mod::Source::lfo1) && c.routesTo(mod::Target::volume, 0) &&
               !c.routesTo(mod::Target::pitch, 0);
    }());

// Cutoff AB goes to the Filter Control's term, not to the filters' own.
static_assert(
    []
    {
        mod::Slots slots{};
        slots[8] = {.source = mod::Source::modWheel, .destination = mod::Destination::cutoffAB, .amount = 4.0f};
        const auto c = mod::compile(slots);
        mod::SourceValues values{};
        values[static_cast<std::size_t>(mod::Source::modWheel)] = 0.25f;
        mod::Sums sums{};
        mod::accumulate(c, values, sums);
        return sums.at(mod::Target::controlCutoff, 0) >= 1.0f && sums.at(mod::Target::controlCutoff, 0) <= 1.0f &&
               sums.at(mod::Target::controlCutoff, 1) >= 1.0f && !c.routesTo(mod::Target::cutoff, 0) &&
               !c.routesTo(mod::Target::cutoff, 1) && !(sums.at(mod::Target::cutoff, 0) > 0.0f);
    }());

// A route's bound: amount -2 x an envelope in its release from 0.5 is [-1, 0].
static_assert(
    []
    {
        mod::Slots slots{};
        slots[0] = {.source = mod::Source::modEnv1, .destination = mod::Destination::mixB, .amount = -2.0f};
        const auto c = mod::compile(slots);
        mod::SourceBounds bounds{};
        bounds[static_cast<std::size_t>(mod::Source::modEnv1)] = mod::sourceBound(
            mod::Source::modEnv1, {.envelope = {.stage = AdsrStage::release, .level = 0.5f, .sustain = 0.8f}});
        const auto b = mod::targetBound(c, mod::Target::mix, 1, bounds);
        const auto none = mod::targetBound(c, mod::Target::mix, 0, bounds);
        return b.lo >= -1.0f && b.lo <= -1.0f && b.hi >= 0.0f && b.hi <= 0.0f && none.lo >= 0.0f && none.lo <= 0.0f &&
               none.hi >= 0.0f && none.hi <= 0.0f;
    }());

} // namespace

TEST_CASE("Modulation routing: compile() and accumulate() are the prototype's registry (a), bit for bit",
          "[modulation]")
{
    Random random{7};
    std::size_t live = 0;
    for (int trial = 0; trial < 400; ++trial)
    {
        CAPTURE(trial);
        const auto patch = randomPrototypePatch(random);
        const auto ref = proto::table_registry::compile(patch);
        const auto lib = mod::compile(toSlots(patch));
        REQUIRE(lib.count == ref.count);
        live += lib.count;
        for (std::size_t i = 0; i < lib.count; ++i)
        {
            CHECK(static_cast<std::size_t>(lib.routes[i].source) == ref.routes[i].source);
            CHECK(static_cast<std::size_t>(lib.routes[i].target) == static_cast<std::size_t>(ref.routes[i].target));
            CHECK(static_cast<unsigned>(lib.routes[i].parts) == ref.routes[i].parts);
            CHECK(sameBits(lib.routes[i].amount, ref.routes[i].amount));
        }
        // The same sums from the same values, starting from the same sums.
        mod::SourceValues values{};
        for (auto& v : values)
        {
            v = 2.0f * random.nextBipolar();
        }
        mod::Sums libSums{};
        proto::ModSums refSums{};
        for (std::size_t t = 0; t < mod::targetCount; ++t)
        {
            for (std::size_t p = 0; p < mod::partCount; ++p)
            {
                const float start = trial % 2 == 0 ? 0.0f : random.nextBipolar();
                libSums.values[t][p] = start;
                refSums[t][p] = start;
            }
        }
        mod::accumulate(lib, values, libSums);
        proto::table_registry::accumulate(ref, values, refSums);
        for (std::size_t t = 0; t < mod::targetCount; ++t)
        {
            for (std::size_t p = 0; p < mod::partCount; ++p)
            {
                CAPTURE(t, p);
                CHECK(sameBits(libSums.values[t][p], refSums[t][p]));
                CHECK(sameBits(libSums.at(static_cast<mod::Target>(t), p), refSums[t][p]));
            }
        }
        // routesTo() and uses() are the prototype voice's `routed` and `sourceUsed`.
        std::array<std::array<bool, 2>, mod::targetCount> routed{};
        std::array<bool, mod::sourceCount> used{};
        for (std::size_t i = 0; i < ref.count; ++i)
        {
            for (std::size_t p = 0; p < 2; ++p)
            {
                routed[static_cast<std::size_t>(ref.routes[i].target)][p] |= (ref.routes[i].parts & (1U << p)) != 0;
            }
            used[ref.routes[i].source] = true;
        }
        for (std::size_t t = 0; t < mod::targetCount; ++t)
        {
            CHECK(lib.routesTo(static_cast<mod::Target>(t), 0) == routed[t][0]);
            CHECK(lib.routesTo(static_cast<mod::Target>(t), 1) == routed[t][1]);
        }
        for (std::size_t s = 0; s < mod::sourceCount; ++s)
        {
            CHECK(lib.uses(static_cast<mod::Source>(s)) == used[s]);
        }
    }
    // Enough live routes for the comparison to mean something.
    CHECK(live > 400 * 8);
}

TEST_CASE("Modulation bounds: each source's bound and each target's are the prototype's, bit for bit", "[modulation]")
{
    Random random{17};
    for (int trial = 0; trial < 400; ++trial)
    {
        CAPTURE(trial);
        const auto patch = randomPrototypePatch(random);
        const auto ref = proto::table_registry::compile(patch);
        const auto lib = mod::compile(toSlots(patch));
        mod::SourceBounds libBounds{};
        std::array<proto::Interval, proto::sourceCount> refBounds{};
        for (std::size_t s = 0; s < mod::sourceCount; ++s)
        {
            CAPTURE(s);
            const auto in = randomInputs(random);
            libBounds[s] = mod::sourceBound(static_cast<mod::Source>(s), in);
            refBounds[s] = proto::table_registry::bound(s, toPrototype(in));
            CHECK(sameBits(libBounds[s].lo, refBounds[s].lo));
            CHECK(sameBits(libBounds[s].hi, refBounds[s].hi));
            // The prototype's DSP-only table gives the same.
            const auto dsp = proto::table_registry::boundDsp(s, toPrototype(in));
            CHECK(sameBits(libBounds[s].lo, dsp.lo));
            CHECK(sameBits(libBounds[s].hi, dsp.hi));
        }
        for (std::size_t t = 0; t < mod::targetCount; ++t)
        {
            for (std::size_t p = 0; p < mod::partCount; ++p)
            {
                CAPTURE(t, p);
                const auto a = mod::targetBound(lib, static_cast<mod::Target>(t), p, libBounds);
                const auto b = prototypeTargetBound(ref, static_cast<proto::Target>(t), p, refBounds);
                CHECK(sameBits(a.lo, b.lo));
                CHECK(sameBits(a.hi, b.hi));
            }
        }
    }
}

TEST_CASE("Modulation bounds: each kind's functions are the prototype's, bit for bit", "[modulation]")
{
    Random random{19};
    for (int trial = 0; trial < 2000; ++trial)
    {
        CAPTURE(trial);
        const auto in = randomInputs(random);
        const auto e = mod::envelopeBound(in.envelope);
        const auto pe = proto::envelopeStageBound(toPrototype(in).envelope);
        CHECK(sameBits(e.lo, pe.lo));
        CHECK(sameBits(e.hi, pe.hi));
        const float a = random.nextBipolar();
        const float b = random.nextBipolar();
        const auto l = mod::linearBound(a, b);
        const auto pl = proto::linearBound(a, b);
        CHECK(sameBits(l.lo, pl.lo));
        CHECK(sameBits(l.hi, pl.hi));
        const auto wave = randomInterval(random);
        const float g0 = random.nextUnipolar();
        const float g1 = random.nextUnipolar();
        const mod::Interval gain{.lo = std::fmin(g0, g1), .hi = std::fmax(g0, g1)};
        const auto offset = randomInterval(random);
        const auto x = mod::lfoBound(wave, gain, offset);
        const auto px = proto::lfoSourceBound(toPrototype(wave), toPrototype(gain), toPrototype(offset));
        CHECK(sameBits(x.lo, px.lo));
        CHECK(sameBits(x.hi, px.hi));
        std::array<float, 32> rendered{};
        for (auto& w : rendered)
        {
            w = 1.25f * random.nextBipolar();
        }
        const auto n = 1 + (random.nextUint32() % 32);
        const auto r = mod::waveBound(std::span<const float>{rendered}.first(n));
        const auto pr = proto::renderedBound(std::span<const float>{rendered}.first(n));
        CHECK(sameBits(r.lo, pr.lo));
        CHECK(sameBits(r.hi, pr.hi));
        // The source's value: the prototype voice's 0.5 x gain x (wave + offset).
        const float v = mod::lfoSourceValue(rendered[0], g0, offset.lo);
        CHECK(sameBits(v, 0.5f * g0 * (rendered[0] + offset.lo)));
    }
    // An empty wave bounds nothing: 0.
    const auto empty = mod::waveBound({});
    CHECK(sameBits(empty.lo, 0.0f));
    CHECK(sameBits(empty.hi, 0.0f));
}

TEST_CASE("Modulation bounds: a source of the range kind gives its row's range, None exactly 0", "[modulation]")
{
    // No source of the 11 is of the range kind; it is the fallback for a
    // Phase 2 source with nothing tighter.
    for (const auto& s : mod::sources)
    {
        CHECK(s.bound != mod::BoundKind::range);
    }
    const mod::BoundInputs in{
        .ends = {.lo = -0.25f, .hi = 0.5f},
        .envelope = {.stage = AdsrStage::decay, .level = 0.8f, .sustain = 0.3f},
        .lfo = {.lo = -0.4f, .hi = 0.1f},
    };
    const auto none = mod::sourceBound(mod::Source::none, in);
    CHECK(sameBits(none.lo, 0.0f));
    CHECK(sameBits(none.hi, 0.0f));
    const auto outOfRange = mod::sourceBound(static_cast<mod::Source>(99), in);
    CHECK(sameBits(outOfRange.lo, 0.0f));
    CHECK(sameBits(outOfRange.hi, 0.0f));
    const mod::SourceRow wide{.scope = mod::Scope::voice, .min = -3.0f, .max = 2.0f, .bound = mod::BoundKind::range};
    const auto r = mod::boundOfRow(wide, in);
    CHECK(sameBits(r.lo, -3.0f));
    CHECK(sameBits(r.hi, 2.0f));
    // Each other kind reads its own input.
    CHECK(sameBits(mod::sourceBound(mod::Source::velocity, in).lo, -0.25f));
    CHECK(sameBits(mod::sourceBound(mod::Source::modEnv2, in).lo, 0.3f));
    CHECK(sameBits(mod::sourceBound(mod::Source::modEnv2, in).hi, 0.8f));
    CHECK(sameBits(mod::sourceBound(mod::Source::lfo2, in).lo, -0.4f));
}

TEST_CASE("Modulation routing: an empty slot, an amount of 0 and the init patch add an exact 0", "[modulation]")
{
    mod::SourceValues values{};
    for (auto& v : values)
    {
        v = 0.75f;
    }
    SECTION("the init patch")
    {
        const mod::Slots init{};
        const auto c = mod::compile(init);
        CHECK(c.empty());
        CHECK(c.live().empty());
        mod::Sums sums{};
        mod::accumulate(c, values, sums);
        CHECK(allPositiveZero(sums));
        for (std::size_t t = 0; t < mod::targetCount; ++t)
        {
            CHECK_FALSE(c.routesTo(static_cast<mod::Target>(t), 0));
            CHECK_FALSE(c.routesTo(static_cast<mod::Target>(t), 1));
        }
        for (std::size_t s = 0; s < mod::sourceCount; ++s)
        {
            CHECK_FALSE(c.uses(static_cast<mod::Source>(s)));
        }
        const mod::SourceBounds bounds{};
        for (std::size_t t = 0; t < mod::targetCount; ++t)
        {
            const auto b = mod::targetBound(c, static_cast<mod::Target>(t), 0, bounds);
            CHECK(std::bit_cast<std::uint32_t>(b.lo) == 0);
            CHECK(std::bit_cast<std::uint32_t>(b.hi) == 0);
        }
    }
    SECTION("empty slots and amounts of 0, among sums already there")
    {
        mod::Slots slots{};
        slots[0] = {.source = mod::Source::lfo1, .destination = mod::Destination::pitchA, .amount = 0.0f};
        slots[1] = {.source = mod::Source::lfo1, .destination = mod::Destination::pitchA, .amount = -0.0f};
        slots[2] = {.source = mod::Source::none, .destination = mod::Destination::mixA, .amount = 5.0f};
        slots[3] = {.source = mod::Source::velocity, .destination = mod::Destination::none, .amount = 5.0f};
        slots[4] = {.source = static_cast<mod::Source>(40), .destination = mod::Destination::mixA, .amount = 5.0f};
        slots[5] = {.source = mod::Source::velocity, .destination = static_cast<mod::Destination>(40), .amount = 5.0f};
        slots[6] = {
            .source = mod::Source::velocity,
            .destination = mod::Destination::mixA,
            .amount = std::numeric_limits<float>::quiet_NaN(),
        };
        const auto c = mod::compile(slots);
        CHECK(c.empty());
        mod::Sums sums{};
        Random random{3};
        for (auto& row : sums.values)
        {
            for (auto& v : row)
            {
                v = random.nextBipolar();
            }
        }
        const auto before = sums;
        mod::accumulate(c, values, sums);
        for (std::size_t t = 0; t < mod::targetCount; ++t)
        {
            for (std::size_t p = 0; p < mod::partCount; ++p)
            {
                CHECK(sameBits(sums.values[t][p], before.values[t][p]));
            }
        }
    }
    SECTION("an empty slot between live ones changes nothing")
    {
        mod::Slots slots{};
        slots[0] = {.source = mod::Source::velocity, .destination = mod::Destination::mixA, .amount = 3.0f};
        slots[9] = {.source = mod::Source::keytrack, .destination = mod::Destination::mixA, .amount = -2.0f};
        auto withEmpty = slots;
        withEmpty[4] = {.source = mod::Source::lfo1, .destination = mod::Destination::mixA, .amount = 0.0f};
        mod::Sums a{};
        mod::Sums b{};
        mod::accumulate(mod::compile(slots), values, a);
        mod::accumulate(mod::compile(withEmpty), values, b);
        CHECK(sameBits(a.at(mod::Target::mix, 0), b.at(mod::Target::mix, 0)));
        CHECK(sameBits(a.at(mod::Target::mix, 0), (3.0f * 0.75f) + (-2.0f * 0.75f)));
    }
}

TEST_CASE("Modulation routing: AB is both parts, except Cutoff AB and Reso AB; routes add", "[modulation]")
{
    mod::SourceValues values{};
    values[static_cast<std::size_t>(mod::Source::velocity)] = 0.5f;
    for (const auto d : {
             mod::Destination::volumeAB,
             mod::Destination::pitchAB,
             mod::Destination::phaseAB,
             mod::Destination::panAB,
             mod::Destination::mixAB,
         })
    {
        CAPTURE(static_cast<int>(d));
        mod::Slots slots{};
        slots[8] = {.source = mod::Source::velocity, .destination = d, .amount = 2.0f};
        mod::Sums sums{};
        mod::accumulate(mod::compile(slots), values, sums);
        const auto& row = mod::destinationRow(d);
        CHECK(sameBits(sums.at(row.target, 0), row.unitsPerAmount));
        CHECK(sameBits(sums.at(row.target, 1), row.unitsPerAmount));
    }
    for (const auto [d, own, control] : {
             std::tuple{mod::Destination::cutoffAB, mod::Target::cutoff, mod::Target::controlCutoff},
             std::tuple{mod::Destination::resonanceAB, mod::Target::resonance, mod::Target::controlResonance},
         })
    {
        mod::Slots slots{};
        slots[8] = {.source = mod::Source::velocity, .destination = d, .amount = 2.0f};
        mod::Sums sums{};
        mod::accumulate(mod::compile(slots), values, sums);
        CHECK(std::bit_cast<std::uint32_t>(sums.at(own, 0)) == 0);
        CHECK(std::bit_cast<std::uint32_t>(sums.at(own, 1)) == 0);
        CHECK(sameBits(sums.at(control, 0), 1.0f));
    }
    // Pitch is an octave per unit of amount.
    {
        mod::Slots slots{};
        slots[8] = {.source = mod::Source::velocity, .destination = mod::Destination::pitchB, .amount = 1.0f};
        values[static_cast<std::size_t>(mod::Source::velocity)] = 1.0f;
        mod::Sums sums{};
        mod::accumulate(mod::compile(slots), values, sums);
        CHECK(sameBits(sums.at(mod::Target::pitch, 1), 12.0f));
        CHECK(std::bit_cast<std::uint32_t>(sums.at(mod::Target::pitch, 0)) == 0);
    }
    // Two routes to Mix A at +10 from a source at 0.75: "+15 (in 2 mod dest slots)".
    {
        mod::Slots slots{};
        values[static_cast<std::size_t>(mod::Source::modWheel)] = 0.75f;
        slots[8] = {.source = mod::Source::modWheel, .destination = mod::Destination::mixA, .amount = 10.0f};
        slots[9] = {.source = mod::Source::modWheel, .destination = mod::Destination::mixA, .amount = 10.0f};
        mod::Sums sums{};
        mod::accumulate(mod::compile(slots), values, sums);
        CHECK(sameBits(sums.at(mod::Target::mix, 0), 15.0f));
    }
    // The LFO targets: bit 0 is LFO 1, bit 1 LFO 2.
    {
        mod::Slots slots{};
        slots[4] = {.source = mod::Source::lfo1, .destination = mod::Destination::lfo2Rate, .amount = 3.0f};
        values[static_cast<std::size_t>(mod::Source::lfo1)] = 0.5f;
        const auto c = mod::compile(slots);
        mod::Sums sums{};
        mod::accumulate(c, values, sums);
        CHECK(sameBits(sums.at(mod::Target::lfoRate, 1), 1.5f));
        CHECK(std::bit_cast<std::uint32_t>(sums.at(mod::Target::lfoRate, 0)) == 0);
        CHECK(c.routesTo(mod::Target::lfoRate, 1));
        CHECK_FALSE(c.routesTo(mod::Target::lfoRate, 0));
    }
    // Out of range reads 0.
    {
        const mod::Sums sums{};
        CHECK(std::bit_cast<std::uint32_t>(sums.at(mod::Target::count, 0)) == 0);
        CHECK(std::bit_cast<std::uint32_t>(sums.at(mod::Target::mix, 2)) == 0);
    }
}

TEST_CASE("Modulation routing: the amount is held to [-10, +10]", "[modulation]")
{
    mod::Slots slots{};
    slots[8] = {.source = mod::Source::velocity, .destination = mod::Destination::mixA, .amount = 25.0f};
    slots[9] = {.source = mod::Source::velocity, .destination = mod::Destination::mixB, .amount = -1e30f};
    slots[10] = {
        .source = mod::Source::velocity,
        .destination = mod::Destination::pitchA,
        .amount = std::numeric_limits<float>::infinity(),
    };
    slots[11] = {.source = mod::Source::velocity, .destination = mod::Destination::volumeA, .amount = 9.5f};
    const auto c = mod::compile(slots);
    REQUIRE(c.count == 4);
    CHECK(sameBits(c.routes[0].amount, 10.0f));
    CHECK(sameBits(c.routes[1].amount, -10.0f));
    CHECK(sameBits(c.routes[2].amount, 120.0f));
    CHECK(sameBits(c.routes[3].amount, 9.5f));
}

TEST_CASE("Modulation routing: compiling, accumulating and bounding allocate nothing", "[modulation]")
{
    Random random{23};
    std::size_t allocations = 0;
    float keep = 0.0f;
    for (int trial = 0; trial < 50; ++trial)
    {
        const auto slots = toSlots(randomPrototypePatch(random));
        mod::SourceValues values{};
        values.fill(0.5f);
        const auto in = randomInputs(random);
        const anoesis::test::AllocationCounter counter;
        const auto c = mod::compile(slots);
        mod::Sums sums{};
        mod::accumulate(c, values, sums);
        mod::SourceBounds bounds{};
        for (std::size_t s = 0; s < mod::sourceCount; ++s)
        {
            bounds[s] = mod::sourceBound(static_cast<mod::Source>(s), in);
        }
        keep += mod::targetBound(c, mod::Target::mix, 0, bounds).hi + sums.at(mod::Target::mix, 0);
        allocations += counter.count();
    }
    CHECK(allocations == 0);
    CHECK(std::isfinite(keep));
}
