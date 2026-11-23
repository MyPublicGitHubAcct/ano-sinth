// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// CPU cost of the modulation model as built (libs/dsp modulation/;
// docs/dsp/modulation.md, as built), per call:
//
// - compile(): the 16 slots of stage 1's mixed patch (16 live routes) into
//   the list a voice evaluates;
// - accumulate(): 0, 4 and 16 routes into the sums (the 4 are slots 0, 4, 8
//   and 10 of the mixed patch: an envelope, an LFO, velocity, the wheel);
// - each source's bound for the 16 routes' sources, in a runtime order, and
//   Mix A's bound from the 16 routes;
// - RouteCompiler::update() at a grid point with the patch unchanged (what
//   every grid point costs) and changed;
// - stage 1's registry (a) on the same 16 routes, in the same session.
//
// Tagged [!benchmark][modulation]: run with tools/bench.sh "[modulation]"
// in relwithdebinfo.

#include "../modulation/registry_candidates.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/modulation/route_compiler.hpp>
#include <anoesis/dsp/modulation/routing.hpp>
#include <anoesis/dsp/modulation/source_bound.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace mod = anoesis::dsp::modulation;
namespace proto = anoesis::test::modulation;

namespace
{

using D = mod::Destination;
using S = mod::Source;

/// Stage 1's mixed patch (modulation_benchmark.cpp), every slot live.
mod::Slots mixedSlots()
{
    const std::array<mod::Slot, mod::slotCount> slots{
        mod::Slot{.source = S::modEnv1, .destination = D::cutoffA, .amount = 3.0f},
        mod::Slot{.source = S::modEnv1, .destination = D::resonanceA, .amount = 1.0f},
        mod::Slot{.source = S::modEnv2, .destination = D::mixA, .amount = 1.0f},
        mod::Slot{.source = S::modEnv2, .destination = D::pitchA, .amount = 0.01f},
        mod::Slot{.source = S::lfo1, .destination = D::pitchA, .amount = 0.02f},
        mod::Slot{.source = S::lfo1, .destination = D::lfo2Rate, .amount = 1.0f},
        mod::Slot{.source = S::lfo2, .destination = D::panA, .amount = 2.0f},
        mod::Slot{.source = S::lfo2, .destination = D::lfo1Gain, .amount = 2.0f},
        mod::Slot{.source = S::velocity, .destination = D::volumeA, .amount = -2.0f},
        mod::Slot{.source = S::keytrack, .destination = D::cutoffAB, .amount = 2.0f},
        mod::Slot{.source = S::modWheel, .destination = D::phaseA, .amount = 1.0f},
        mod::Slot{.source = S::aftertouch, .destination = D::mixA, .amount = 1.0f},
        mod::Slot{.source = S::velocity, .destination = D::cutoffB, .amount = 1.0f},
        mod::Slot{.source = S::keytrack, .destination = D::panAB, .amount = 1.0f},
        mod::Slot{.source = S::modWheel, .destination = D::resonanceAB, .amount = 1.0f},
        mod::Slot{.source = S::lfo1, .destination = D::volumeAB, .amount = 1.0f},
    };
    return slots;
}

/// `all` with 0, 4 (slots 0, 4, 8 and 10) or all 16 slots kept, the rest empty.
mod::Slots firstOf(const mod::Slots& all, std::size_t n)
{
    constexpr std::array<std::size_t, 4> four{0, 4, 8, 10};
    mod::Slots out{};
    if (n == 4)
    {
        for (const auto i : four)
        {
            out[i] = all[i];
        }
    }
    else if (n == mod::slotCount)
    {
        out = all;
    }
    return out;
}

std::array<proto::Route, proto::routeCount> toPrototype(const mod::Slots& slots)
{
    std::array<proto::Route, proto::routeCount> patch{};
    for (std::size_t i = 0; i < mod::slotCount; ++i)
    {
        patch[i] = {
            .source = static_cast<std::uint8_t>(slots[i].source),
            .destination = static_cast<std::uint8_t>(slots[i].destination),
            .amount = slots[i].amount,
        };
    }
    return patch;
}

} // namespace

TEST_CASE("Modulation: CPU of the model as built", "[!benchmark][modulation]")
{
    // Runtime inputs, so the compiler can't fold the work away.
    anoesis::dsp::Random random{5};
    mod::SourceValues values{};
    for (auto& v : values)
    {
        v = random.nextBipolar();
    }
    std::array<mod::BoundInputs, mod::sourceCount> inputs{};
    for (auto& in : inputs)
    {
        const float a = random.nextUnipolar();
        const float b = random.nextUnipolar();
        in = {
            .ends = {.lo = std::min(a, b), .hi = std::max(a, b)},
            .envelope =
                {
                    .stage = static_cast<anoesis::dsp::AdsrStage>(random.nextUint32() % 5),
                    .level = random.nextUnipolar(),
                    .sustain = random.nextUnipolar(),
                },
            .lfo = {.lo = -a, .hi = b},
        };
    }
    // The sources a voice's 16 routes would ask about, in a runtime order.
    std::array<mod::Source, mod::slotCount> asked{};
    for (auto& s : asked)
    {
        s = static_cast<mod::Source>(1 + (random.nextUint32() % (mod::sourceCount - 1)));
    }
    const auto all = mixedSlots();
    for (const std::size_t n : {std::size_t{0}, std::size_t{4}, std::size_t{16}})
    {
        const auto routes = mod::compile(firstOf(all, n));
        BENCHMARK("modulation: library, accumulate " + std::to_string(n) + " routes")
        {
            mod::Sums s{};
            mod::accumulate(routes, values, s);
            return s.values[1][0];
        };
    }
    BENCHMARK("modulation: library, compile 16 routes")
    {
        return mod::compile(all).count;
    };
    BENCHMARK("modulation: library, 16 source bounds")
    {
        float sum = 0.0f;
        for (const auto s : asked)
        {
            sum += mod::sourceBound(s, inputs[static_cast<std::size_t>(s)]).hi;
        }
        return sum;
    };
    const auto routes = mod::compile(all);
    mod::SourceBounds bounds{};
    for (std::size_t s = 0; s < mod::sourceCount; ++s)
    {
        bounds[s] = mod::sourceBound(static_cast<mod::Source>(s), inputs[s]);
    }
    BENCHMARK("modulation: library, Mix A's bound from 16 routes")
    {
        return mod::targetBound(routes, mod::Target::mix, 0, bounds).hi;
    };
    mod::Patch patch;
    patch.set(all);
    mod::RouteCompiler compiler;
    compiler.update(patch);
    BENCHMARK("modulation: library, update() unchanged")
    {
        return compiler.update(patch);
    };
    BENCHMARK("modulation: library, update() after a change")
    {
        patch.set(15, all[15]);
        return compiler.update(patch);
    };
    // Stage 1's registry (a), the same routes, this session.
    const auto refPatch = toPrototype(all);
    const auto ref = proto::table_registry::compile(refPatch);
    BENCHMARK("modulation: prototype (a), accumulate 16 routes")
    {
        proto::ModSums s{};
        proto::table_registry::accumulate(ref, values, s);
        return s[1][0];
    };
    BENCHMARK("modulation: prototype (a), compile 16 routes")
    {
        return proto::table_registry::compile(refPatch).count;
    };
}
