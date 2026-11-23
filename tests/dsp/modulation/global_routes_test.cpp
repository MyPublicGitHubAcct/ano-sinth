// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Routes to the global destinations (M1.5 task 5; docs/dsp/modulation-hand-offs.md).
//
// Phaser Frequency and Distortion Amount act on the master effects, which
// read each per-voice source's global copy (M1.7, decided in task 2). The
// voice never reads their sums. Stage 1 measured two candidates for what
// the voice does with such a route:
//
// (a) every route compiles into each voice (the library before stage 2), so
//     a route to a global destination runs its source in every voice and a
//     change to it starts the voice's 10 ms crossfade, which holds back the
//     voice's other changes;
// (b) the voice compiles only its own (Scope::voice) routes, so a global
//     route is invisible to it.
//
// Decided 2026-11-23: (b). (b) renders exactly as the same events with the
// global routes removed, which is the reference here. As built:
//
// - compile(slots, scope) keeps exactly the routes of that scope, and a
//   RouteCompiler compiles for its scope (Scope::voice by default);
// - dsp::Voice with any global routes is dsp::Voice without them, to the
//   bit, at host blocks of 1, 37 and 4096, with no allocation: the init
//   patch, the busy patch, a per-voice change made just after a global one
//   (which (a) held back), and a late route on an LFO that a global route
//   reads (which (a) found already running).
//
// The hidden report ("[global-routes-report]") prints the library against
// the reference; stage 1's numbers (a against b) are from commit a455b3e.
// CPU: the "[global-routes]" benchmarks (destination_benchmark.cpp).

#include "destination_support.hpp"

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/modulation/route_compiler.hpp>
#include <anoesis/dsp/modulation/routing.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

namespace
{

using namespace anoesis::test::destinations;
using anoesis::test::Buffer;
namespace vs = anoesis::test::voice_sources;
namespace dsp = anoesis::dsp;

/// The slots the global routes use here; the busy patch leaves them empty.
constexpr std::array<std::size_t, 3> globalSlots{3, 7, 15};

bool isGlobalSlot(std::size_t slot)
{
    return std::ranges::find(globalSlots, slot) != globalSlots.end();
}

/// Global routes appearing, changing and going, on slots 3, 7 and 15: an
/// S&H LFO, velocity and a mod envelope (sources nothing else reads).
std::vector<DestEvent> globalRoutes()
{
    return {
        route(0, 3, Source::lfo2, Dest::phaserFrequency, 4.0f),
        route(0, 7, Source::velocity, Dest::distortionAmount, -3.0f),
        route(3001, 15, Source::modEnv2, Dest::phaserFrequency, 6.0f),
        route(7003, 3, Source::lfo2, Dest::phaserFrequency, -2.5f),
        route(10500, 7, Source::lfo1, Dest::distortionAmount, 1.0f),
        route(12000, 15, Source::none, Dest::none, 0.0f),
    };
}

/// The reference: the same events with every global route removed.
std::vector<DestEvent> withoutGlobal(const std::vector<DestEvent>& events)
{
    std::vector<DestEvent> out;
    for (const auto& e : events)
    {
        const bool globalRoute = e.kind == DestEvent::Kind::source && e.event.kind == proto::SourceEvent::Kind::slot &&
                                 isGlobalSlot(e.event.slot);
        if (!globalRoute)
        {
            out.push_back(e);
        }
    }
    return out;
}

std::vector<DestEvent> plus(std::vector<DestEvent> a, const std::vector<DestEvent>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

/// The init voicing with a Noise slot (its random stream would show any
/// draw the global routes moved).
DestVoicing initWithNoise()
{
    DestVoicing d;
    d.base.voices = {1, 1, 0, 0};
    d.base.waves = {dsp::Waveform::saw, dsp::Waveform::noise, dsp::Waveform::saw, dsp::Waveform::saw};
    return d;
}

proto::SourceSettings sampleHoldSources()
{
    auto s = vs::busySettings();
    s.lfos[1].waveform = dsp::LfoWaveform::sampleHold;
    return s;
}

std::vector<DestEvent> initNotes()
{
    return {
        noteOn(0, 57, 90),     noteOn(4000, 64, 30), noteOff(9000, 57),
        noteOn(9500, 50, 127), noteOff(12000, 64),   noteOff(15000, 50),
    };
}

/// The busy patch (task 3's) with a per-voice change at 7100: Mix A's route
/// from Mod Env 1 at +5 to -2.
std::vector<DestEvent> busyWithChange()
{
    auto e = wrapAll(vs::busyEvents());
    e.push_back(route(7100, 1, Source::modEnv1, Dest::mixA, -2.0f));
    return e;
}

/// A route on LFO 2 to Mix A appearing at 9001, LFO 2 read by a global
/// route from the note-on.
std::vector<DestEvent> lateRouteOnAGlobalLfo()
{
    return {
        noteOn(0, 57, 100),
        route(0, 3, Source::lfo2, Dest::phaserFrequency, 4.0f),
        route(9001, 6, Source::lfo2, Dest::mixA, 4.0f),
        noteOff(20000, 57),
    };
}

double maxDifferenceDb(const Buffer& a, const Buffer& b)
{
    double m = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < a.numSamples(); ++i)
        {
            m = std::max(m, std::abs(static_cast<double>(a.channel(c)[i]) - static_cast<double>(b.channel(c)[i])));
        }
    }
    return m > 0.0 ? 20.0 * std::log10(m) : -999.0;
}

constexpr std::size_t length = 24000;
constexpr std::array<std::size_t, 3> hostBlocks{1, 37, 4096};

bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

/// Two compiled lists the same (this test's own, against which the library's
/// sameRoutes() is checked): routes in order, to the bit, and the
/// bookkeeping.
bool routesMatch(const mod::CompiledRoutes& a, const mod::CompiledRoutes& b)
{
    if (a.count != b.count || a.routed != b.routed || a.used != b.used)
    {
        return false;
    }
    for (std::size_t i = 0; i < a.count; ++i)
    {
        const auto& x = a.routes[i];
        const auto& y = b.routes[i];
        if (x.source != y.source || x.target != y.target || x.parts != y.parts || !sameBits(x.amount, y.amount))
        {
            return false;
        }
    }
    return true;
}

/// A random patch: any source, any destination, about a fifth at amount 0.
mod::Slots randomSlots(dsp::Random& random)
{
    mod::Slots slots{};
    for (auto& slot : slots)
    {
        const auto r = random.nextUint32();
        slot.source = static_cast<mod::Source>(r % mod::sourceCount);
        slot.destination = static_cast<mod::Destination>((r >> 8U) % mod::destinationCount);
        slot.amount = (r >> 16U) % 5 == 0 ? 0.0f : 10.0f * random.nextBipolar();
    }
    return slots;
}

/// The slots with every route of the other scope emptied.
mod::Slots onlyScope(mod::Slots slots, mod::Scope scope)
{
    for (auto& slot : slots)
    {
        const auto d = static_cast<std::size_t>(slot.destination);
        if (d < mod::destinationCount && mod::destinations[d].scope != scope)
        {
            slot = {};
        }
    }
    return slots;
}

// A global route is no route to a voice; the engine's compile keeps it.
static_assert(
    []
    {
        mod::Slots slots{};
        slots[0] = {.source = mod::Source::lfo1, .destination = mod::Destination::phaserFrequency, .amount = 3.0f};
        slots[1] = {.source = mod::Source::velocity, .destination = mod::Destination::cutoffA, .amount = 2.0f};
        const auto v = mod::compile(slots, mod::Scope::voice);
        const auto g = mod::compile(slots, mod::Scope::global);
        return v.count == 1 && !v.uses(mod::Source::lfo1) && v.uses(mod::Source::velocity) && g.count == 1 &&
               g.uses(mod::Source::lfo1) && g.routesTo(mod::Target::phaserFrequency, 0);
    }());

// sameRoutes(): each field of a route counts, the amount to the bit.
static_assert(
    []
    {
        mod::Slots slots{};
        slots[0] = {.source = mod::Source::velocity, .destination = mod::Destination::mixA, .amount = -5.0f};
        const auto base = mod::compile(slots, mod::Scope::voice);
        auto amount = slots;
        amount[0].amount = -10.0f;
        auto source = slots;
        source[0].source = mod::Source::modWheel;
        auto destination = slots;
        destination[0].destination = mod::Destination::mixB;
        auto moved = mod::Slots{};
        moved[1] = slots[0];
        auto longer = slots;
        longer[1] = {.source = mod::Source::modWheel, .destination = mod::Destination::mixB, .amount = 1.0f};
        auto global = slots;
        global[5] = {.source = mod::Source::lfo1, .destination = mod::Destination::phaserFrequency, .amount = 2.0f};
        return mod::sameRoutes(base, mod::compile(moved, mod::Scope::voice)) &&
               mod::sameRoutes(base, mod::compile(global, mod::Scope::voice)) &&
               !mod::sameRoutes(base, mod::compile(amount, mod::Scope::voice)) &&
               !mod::sameRoutes(base, mod::compile(source, mod::Scope::voice)) &&
               !mod::sameRoutes(base, mod::compile(destination, mod::Scope::voice)) &&
               !mod::sameRoutes(base, mod::compile(longer, mod::Scope::voice)) &&
               !mod::sameRoutes(base, mod::CompiledRoutes{});
    }());

double energyOf(const Buffer& b, std::size_t from, std::size_t to)
{
    double e = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = from; i < to && i < b.numSamples(); ++i)
        {
            const auto x = static_cast<double>(b.channel(c)[i]);
            e += x * x;
        }
    }
    return e;
}

} // namespace

TEST_CASE("Global routes: compile() by scope keeps exactly that scope's routes, in slot order", "[modulation]")
{
    dsp::Random random{11};
    std::size_t voiceRoutes = 0;
    std::size_t globalRoutes = 0;
    for (int trial = 0; trial < 400; ++trial)
    {
        CAPTURE(trial);
        const auto slots = randomSlots(random);
        const auto v = mod::compile(slots, mod::Scope::voice);
        const auto g = mod::compile(slots, mod::Scope::global);
        CHECK(routesMatch(v, mod::compile(onlyScope(slots, mod::Scope::voice))));
        CHECK(routesMatch(g, mod::compile(onlyScope(slots, mod::Scope::global))));
        CHECK(v.count + g.count == mod::compile(slots).count);
        // The library's sameRoutes() agrees with routesMatch().
        CHECK(mod::sameRoutes(v, mod::compile(onlyScope(slots, mod::Scope::voice))));
        CHECK_FALSE(mod::sameRoutes(v, g) != routesMatch(v, g));
        voiceRoutes += v.count;
        globalRoutes += g.count;
    }
    // The draw reaches both scopes.
    CHECK(voiceRoutes > 1000);
    CHECK(globalRoutes > 100);
}

TEST_CASE("Global routes: a RouteCompiler compiles for its scope, the voice's by default", "[modulation]")
{
    mod::Patch patch;
    patch.set(0, {.source = mod::Source::lfo2, .destination = mod::Destination::distortionAmount, .amount = 5.0f});
    patch.set(4, {.source = mod::Source::modEnv1, .destination = mod::Destination::pitchA, .amount = 1.0f});

    mod::RouteCompiler voice;
    mod::RouteCompiler engine{mod::Scope::global};
    CHECK(voice.scope() == mod::Scope::voice);
    CHECK(engine.scope() == mod::Scope::global);

    const anoesis::test::AllocationCounter counter;
    CHECK(voice.update(patch));
    CHECK(engine.update(patch));
    CHECK(counter.count() == 0);
    CHECK(routesMatch(voice.routes(), mod::compile(patch.slots(), mod::Scope::voice)));
    CHECK(routesMatch(engine.routes(), mod::compile(patch.slots(), mod::Scope::global)));
    CHECK(voice.routes().count == 1);
    CHECK_FALSE(voice.routes().uses(mod::Source::lfo2));
    CHECK(engine.routes().count == 1);
    CHECK(engine.routes().uses(mod::Source::lfo2));
}

TEST_CASE("Global routes: only global routes render the init patch to the bit", "[modulation]")
{
    const auto events = plus(initNotes(), globalRoutes());
    const auto voicing = initWithNoise();
    const auto sources = sampleHoldSources();
    for (const std::size_t block : hostBlocks)
    {
        std::size_t allocations = 0;
        const auto a = renderBuilt(sources, voicing, events, length, {.blockSize = block, .allocations = &allocations});
        const auto b = renderBuilt(sources, voicing, withoutGlobal(events), length, {.blockSize = block});
        CHECK(proto::identical(a, b));
        CHECK(allocations == 0);
    }
}

TEST_CASE("Global routes: with per-voice routes, the voice is the patch without its global routes", "[modulation]")
{
    DestVoicing d;
    d.base = vs::busyVoicing;
    // The busy patch with global routes from 0.
    const auto busy = plus(wrapAll(vs::busyEvents()), {
                                                          route(0, 3, Source::lfo2, Dest::phaserFrequency, 4.0f),
                                                          route(0, 7, Source::modEnv2, Dest::distortionAmount, -3.0f),
                                                          route(0, 15, Source::velocity, Dest::phaserFrequency, 6.0f),
                                                      });
    // A global change 97 samples before a per-voice change: stage 1's (a)
    // held Mix A's change back for the global one's crossfade (-14.1 dBFS).
    const auto delayed = plus(busyWithChange(), {
                                                    route(7003, 3, Source::lfo2, Dest::phaserFrequency, 2.0f),
                                                });
    for (const std::size_t block : hostBlocks)
    {
        CAPTURE(block);
        for (const auto* events : {&busy, &delayed})
        {
            const auto a = renderBuilt(vs::busySettings(), d, *events, length, {.blockSize = block});
            const auto b = renderBuilt(vs::busySettings(), d, withoutGlobal(*events), length, {.blockSize = block});
            CHECK(proto::identical(a, b));
        }
    }
}

TEST_CASE("Global routes: a slot set to what it already holds is no change of the routes", "[modulation]")
{
    // Every set bumps the patch's version, so the voice recompiles. Found in
    // stage 2: a recompile that gave the same routes still started a
    // crossfade, which held back the next real change, so a change to a
    // global slot (or a per-voice slot re-sent unchanged, as automation may)
    // delayed Mix A's change at 7100.
    DestVoicing d;
    d.base = vs::busyVoicing;
    const auto resent = plus(busyWithChange(), {
                                                   route(7003, 1, Source::modEnv1, Dest::mixA, 5.0f),
                                               });
    for (const std::size_t block : hostBlocks)
    {
        const auto a = renderBuilt(vs::busySettings(), d, resent, length, {.blockSize = block});
        const auto b = renderBuilt(vs::busySettings(), d, busyWithChange(), length, {.blockSize = block});
        CHECK(proto::identical(a, b));
    }
}

TEST_CASE("Global routes: an amount's change alone still crossfades", "[modulation]")
{
    // Task 4's crossfade, for a change of the amount only: velocity 127 on
    // Mix A from -5 to -10, from Mix's unity, reaches exact silence on the
    // crossfade's last sample, not at the grid point.
    DestVoicing d;
    d.base.waves = {dsp::Waveform::sine, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw};
    d.base.voices = {1, 0, 0, 0};
    d.base.bypass = true;
    d.base.polyphony = 1;
    const std::size_t note = 1000;
    const std::size_t grid = note + (std::size_t{32} * 100);
    const auto n = mod::routeCrossfadeSegments(48000.0f) * 32;
    const std::vector<DestEvent> e{
        route(0, 8, Source::velocity, Dest::mixA, -5.0f),
        noteOn(note, 57),
        route(grid - 20, 8, Source::velocity, Dest::mixA, -10.0f),
    };
    const auto out = renderBuilt(noSources, d, e, grid + n + 2000);
    CHECK(energyOf(out, grid + n - 1, out.numSamples()) <= 0.0);
    CHECK(energyOf(out, grid + n - 40, grid + n - 1) > 0.0);
}

TEST_CASE("Global routes: a late route on an LFO a global route reads starts it from its note-on state", "[modulation]")
{
    // Task 3's rule for any late route; stage 1's (a) found the LFO already
    // running (-26.0 dBFS on a sine, -21.6 on S&H).
    const auto events = lateRouteOnAGlobalLfo();
    const DestVoicing d;
    for (const auto wave : {dsp::LfoWaveform::sine, dsp::LfoWaveform::sampleHold})
    {
        auto s = vs::busySettings();
        s.lfos[1].waveform = wave;
        for (const std::size_t block : hostBlocks)
        {
            const auto a = renderBuilt(s, d, events, length, {.blockSize = block});
            const auto b = renderBuilt(s, d, withoutGlobal(events), length, {.blockSize = block});
            CHECK(proto::identical(a, b));
        }
    }
}

TEST_CASE("Global routes report", "[.][global-routes-report]")
{
    // The library against the reference (the patch without its global
    // routes). Stage 1 printed (a) against (b) here (commit a455b3e).
    DestVoicing busy;
    busy.base = vs::busyVoicing;
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "\n# Global routes in the voice (M1.5 task 5): dsp::Voice against the patch without them\n\n";

    for (const std::size_t gap : {std::size_t{0}, std::size_t{97}, std::size_t{300}, std::size_t{600}})
    {
        const auto events =
            plus(busyWithChange(),
                 {route(7100 - std::min<std::size_t>(gap, 7100), 3, Source::lfo2, Dest::phaserFrequency, 2.0f)});
        const auto a = renderBuilt(vs::busySettings(), busy, events, length);
        const auto b = renderBuilt(vs::busySettings(), busy, withoutGlobal(events), length);
        std::cout << "global change " << gap << " samples before Mix A's: max difference " << maxDifferenceDb(a, b)
                  << " dBFS\n";
    }
    for (const auto wave : {dsp::LfoWaveform::sine, dsp::LfoWaveform::sampleHold})
    {
        auto s = vs::busySettings();
        s.lfos[1].waveform = wave;
        const auto events = lateRouteOnAGlobalLfo();
        const DestVoicing d;
        const auto a = renderBuilt(s, d, events, length);
        const auto b = renderBuilt(s, d, withoutGlobal(events), length);
        std::cout << "late Mix A route on LFO 2 (" << (wave == dsp::LfoWaveform::sine ? "sine" : "S&H")
                  << "): max difference " << maxDifferenceDb(a, b) << " dBFS\n";
    }
    SUCCEED();
}
