// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Routes to the global destinations in the voice (M1.5 task 5, stage 1;
// docs/dsp/modulation.md, "Task 5: the global destinations in the voice").
//
// Phaser Frequency and Distortion Amount act on the master effects, which
// read each per-voice source's global copy (M1.7, decided in task 2). The
// voice never reads their sums. Two candidates for what the voice does with
// such a route:
//
// (a) today: every route compiles into each voice, so a route to a global
//     destination runs the source it reads in every voice, takes the voice
//     off its no-routes path, and a change to it starts the voice's 10 ms
//     crossfade of the routes (which holds back any other change until it
//     ends);
// (b) the voice compiles only its own (Scope::voice) routes: a global route
//     is invisible to it.
//
// (b) renders exactly as the same events with the global routes removed,
// which is how it is rendered here. What this pins down:
//
// - with only global routes, (a) is the init patch to the bit, as (b) is;
// - with per-voice routes too, (a) and (b) agree to the bit while no route
//   changes mid-note;
// - they differ in two cases, measured by the hidden report: a change to a
//   global route delays a per-voice change made within its 10 ms, and a
//   per-voice route appearing mid-note on an LFO that a global route was
//   already running finds it running (a) instead of starting it from its
//   note-on state (b).
//
// CPU: the "[global-routes]" benchmarks (destination_benchmark.cpp).

#include "destination_support.hpp"

#include <anoesis/test/buffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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
/// S&H LFO, velocity and a mod envelope (so the voice would run sources
/// that nothing else reads).
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

/// Candidate (b): the same events with every global route removed.
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
/// draw the global routes moved) and an S&H LFO 2.
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

} // namespace

TEST_CASE("Global routes: only global routes render the init patch to the bit, as (b) does", "[modulation]")
{
    const auto events = plus(initNotes(), globalRoutes());
    const auto voicing = initWithNoise();
    const auto sources = sampleHoldSources();
    for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
    {
        std::size_t allocations = 0;
        const auto a = renderBuilt(sources, voicing, events, length, {.blockSize = block, .allocations = &allocations});
        const auto b = renderBuilt(sources, voicing, withoutGlobal(events), length, {.blockSize = block});
        CHECK(proto::identical(a, b));
        CHECK(allocations == 0);
    }
}

TEST_CASE("Global routes: with per-voice routes and no change mid-note, (a) and (b) agree to the bit", "[modulation]")
{
    // The global routes all at 0, the busy patch's routes all at 0.
    std::vector<DestEvent> events = wrapAll(vs::busyEvents());
    events.push_back(route(0, 3, Source::lfo2, Dest::phaserFrequency, 4.0f));
    events.push_back(route(0, 7, Source::modEnv2, Dest::distortionAmount, -3.0f));
    events.push_back(route(0, 15, Source::velocity, Dest::phaserFrequency, 6.0f));
    DestVoicing d;
    d.base = vs::busyVoicing;
    for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
    {
        const auto a = renderBuilt(vs::busySettings(), d, events, length, {.blockSize = block});
        const auto b = renderBuilt(vs::busySettings(), d, withoutGlobal(events), length, {.blockSize = block});
        CHECK(proto::identical(a, b));
    }
}

TEST_CASE("Global routes: a global change delays a per-voice change in (a), not in (b)", "[modulation]")
{
    // A global amount changed at 7003 starts a 10 ms crossfade in (a), so
    // Mix A's change at 7100 waits for it; (b) takes it at its grid point.
    DestVoicing d;
    d.base = vs::busyVoicing;
    const auto events = plus(busyWithChange(), {
                                                   route(7003, 3, Source::lfo2, Dest::phaserFrequency, 2.0f),
                                               });
    const auto a = renderBuilt(vs::busySettings(), d, events, length);
    const auto b = renderBuilt(vs::busySettings(), d, withoutGlobal(events), length);
    CHECK_FALSE(proto::identical(a, b));
    // Without the per-voice change they agree again.
    const auto aOnly = plus(wrapAll(vs::busyEvents()), {route(7003, 3, Source::lfo2, Dest::phaserFrequency, 2.0f)});
    CHECK(proto::identical(renderBuilt(vs::busySettings(), d, aOnly, length),
                           renderBuilt(vs::busySettings(), d, withoutGlobal(aOnly), length)));
}

TEST_CASE("Global routes report", "[.][global-routes-report]")
{
    DestVoicing busy;
    busy.base = vs::busyVoicing;
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "\n# Global routes in the voice (M1.5 task 5, stage 1)\n\n";

    // 1. A global change just before a per-voice one.
    for (const std::size_t gap : {std::size_t{0}, std::size_t{97}, std::size_t{300}, std::size_t{600}})
    {
        const auto events =
            plus(busyWithChange(),
                 {route(7100 - std::min<std::size_t>(gap, 7100), 3, Source::lfo2, Dest::phaserFrequency, 2.0f)});
        const auto a = renderBuilt(vs::busySettings(), busy, events, length);
        const auto b = renderBuilt(vs::busySettings(), busy, withoutGlobal(events), length);
        std::cout << "global change " << gap << " samples before Mix A's: (a) against (b) max difference "
                  << maxDifferenceDb(a, b) << " dBFS\n";
    }

    // 2. A per-voice route appearing mid-note on LFO 2, which a global
    // route has been running since the note-on (a) or not (b).
    for (const auto wave : {dsp::LfoWaveform::sine, dsp::LfoWaveform::sampleHold})
    {
        auto s = vs::busySettings();
        s.lfos[1].waveform = wave;
        const std::vector<DestEvent> events{
            noteOn(0, 57, 100),
            route(0, 3, Source::lfo2, Dest::phaserFrequency, 4.0f),
            route(9001, 6, Source::lfo2, Dest::mixA, 4.0f),
            noteOff(20000, 57),
        };
        const DestVoicing d;
        const auto a = renderBuilt(s, d, events, length);
        const auto b = renderBuilt(s, d, withoutGlobal(events), length);
        std::cout << "late Mix A route on LFO 2 (" << (wave == dsp::LfoWaveform::sine ? "sine" : "S&H")
                  << "): (a) against (b) max difference " << maxDifferenceDb(a, b) << " dBFS\n";
    }
    SUCCEED();
}
