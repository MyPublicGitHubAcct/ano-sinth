// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The sources' prototypes (M1.5 task 3, stage 1; docs/dsp/
// modulation-sources.md), checked in the default run: SourceVoice with no
// routes is dsp::Voice to the bit under every candidate; the three ways of
// delivering sources give the same output to the bit at host blocks of 1,
// 37 and 4096, with no allocation; every source's bound holds over random
// scenarios at 44.1, 48 and 96 kHz, and deliberately small bounds fail; a
// source reconstructed when a route appears mid-note is the always-run
// source to the bit. The measurements are the hidden [sources-report].

#include "source_candidates.hpp"
#include "source_support.hpp"

#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/modulation/registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace anoesis::test::sources;
using Dest = anoesis::dsp::modulation::Destination;

namespace
{

/// A patch that reads every source into Pitch, Mix, Cutoff and the LFOs.
std::vector<SourceEvent> busyRoutes()
{
    return {
        route(0, 0, Source::modEnv1, Dest::pitchA, 0.1f),    route(0, 1, Source::modEnv1, Dest::mixA, 5.0f),
        route(0, 2, Source::modEnv2, Dest::cutoffA, 3.0f),   route(0, 4, Source::lfo1, Dest::pitchAB, 0.05f),
        route(0, 5, Source::lfo1, Dest::lfo2Gain, 5.0f),     route(0, 6, Source::lfo2, Dest::mixB, 4.0f),
        route(0, 8, Source::velocity, Dest::mixAB, 3.0f),    route(0, 9, Source::keytrack, Dest::pitchB, -0.5f),
        route(0, 10, Source::modWheel, Dest::pitchA, 0.2f),  route(0, 11, Source::aftertouch, Dest::mixA, -3.0f),
        route(0, 12, Source::ampEnvB, Dest::pitchA, 0.1f),   route(0, 13, Source::stepVelocity, Dest::mixB, 2.0f),
        route(0, 14, Source::ampEnvA, Dest::lfo1Rate, 2.0f),
    };
}

SourceSettings busySettings()
{
    SourceSettings s;
    s.modEnvelopes[0] = {.attack = 0.02f, .decay = 0.1f, .sustain = 0.4f, .release = 0.05f};
    s.modEnvelopes[1] = {.attack = 0.005f, .decay = 0.3f, .sustain = 0.0f, .release = 0.02f};
    s.lfos[0] = {.waveform = anoesis::dsp::LfoWaveform::triangle, .rate = 6.0f, .gain = 8.0f, .offset = 2.0f};
    s.lfos[1] = {.waveform = anoesis::dsp::LfoWaveform::sampleHold, .rate = 5.5f, .gain = 4.0f};
    return s;
}

const Voicing busyVoicing{
    .voices = {1, 0, 1, 0},
    .release = {0.05f, 0.08f},
    .polyphony = 2,
    .portamento = 0.03f,
    .slide = true,
};

/// Notes with steals and glides, the bend, the wheel, pressure and Step
/// Velocity all moving.
std::vector<SourceEvent> busyEvents()
{
    auto e = busyRoutes();
    for (const auto& ev : {
             noteOn(0, 57, 100),
             bend(1203, 12000),
             wheel(2001, 40),
             noteOn(5001, 61, 60),
             pressure(6117, 90),
             step(7000, 0.5f),
             noteOn(9013, 64, 127),
             bend(9500, 2000),
             wheel(10007, 127),
             noteOff(15000, 57),
             pressure(15500, 0),
             noteOff(17000, 61),
             noteOn(18000, 50, 30),
             noteOff(19000, 64),
             noteOff(23000, 50),
         })
    {
        e.push_back(ev);
    }
    return e;
}

/// A random scenario for the bound check: glides, opposing bends, legato
/// notes, controllers, retriggers.
std::vector<SourceEvent> randomScenario(anoesis::dsp::Random& random, std::size_t length, std::size_t sampleRate)
{
    std::vector<SourceEvent> e = busyRoutes();
    std::size_t t = 0;
    int key = 60;
    while (t < length)
    {
        const auto r = random.nextUint32();
        switch (r % 6)
        {
        case 0:
            // A note that glides (Slide), with a bend against it a moment
            // later: keytrack's two parts move apart.
            key = 40 + static_cast<int>((r >> 8U) % 40);
            e.push_back(noteOn(t, key, 1 + static_cast<int>((r >> 16U) % 127)));
            e.push_back(bend(t + 1 + ((r >> 4U) % 200), (r & 1U) != 0 ? 0 : 16383));
            break;
        case 1:
            e.push_back(noteOff(t, key));
            break;
        case 2:
            e.push_back(bend(t, static_cast<int>((r >> 8U) % 16384)));
            break;
        case 3:
            e.push_back(wheel(t, static_cast<int>((r >> 8U) % 128)));
            break;
        case 4:
            e.push_back(pressure(t, static_cast<int>((r >> 8U) % 128)));
            break;
        default:
            e.push_back(step(t, random.nextUnipolar()));
            break;
        }
        t += 1 + ((r >> 20U) % (sampleRate / 20));
    }
    return e;
}

} // namespace

TEST_CASE("SourceVoice with no routes is dsp::Voice to the bit under every candidate", "[sources-model]")
{
    const std::vector<SourceEvent> events{
        noteOn(0, 57), noteOn(5001, 61), noteOn(9013, 64), noteOff(15000, 57), noteOff(17000, 61), noteOff(19000, 64),
    };
    const std::array voicings{
        Voicing{.release = {0.05f, 0.05f}, .polyphony = 2},
        Voicing{.voices = {1, 0, 1, 0}, .release = {0.05f, 0.05f}, .polyphony = 2},
        Voicing{.voices = {8, 8, 8, 8}, .resonance = 6.0f, .release = {0.05f, 0.05f}, .polyphony = 2},
    };
    std::array<SourceSettings, 4> candidates{};
    candidates[1].delivery = Delivery::buffered;
    candidates[1].run = RunPolicy::always;
    candidates[2].delivery = Delivery::inlined;
    candidates[2].run = RunPolicy::always;
    candidates[3].ampEnvelopes = AmpEnvelopeRun::heardOrUsed;
    for (auto& c : candidates)
    {
        c.lfos[0] = {.waveform = anoesis::dsp::LfoWaveform::sampleHold, .free = true};
        c.modEnvelopes[0] = {.attack = 0.01f, .sustain = 0.5f};
    }
    const std::size_t length = 24000;
    for (const auto& voicing : voicings)
    {
        for (const std::size_t block : {std::size_t{512}, std::size_t{37}})
        {
            const auto library = renderLibrary(voicing, events, length, block);
            for (const auto& c : candidates)
            {
                std::size_t allocations = 0;
                const auto r = render(c, voicing, events, length, {.blockSize = block, .allocations = &allocations});
                CHECK(identical(r.out, library));
                CHECK(allocations == 0);
            }
        }
    }
}

TEST_CASE("The three deliveries agree to the bit at any host block size, with no allocation", "[sources-model]")
{
    const std::size_t length = 26000;
    auto const reference = busySettings();
    const auto expected = render(reference, busyVoicing, busyEvents(), length, {.blockSize = 4096});
    // The patch is audible and modulated: it differs from no routes.
    CHECK_FALSE(identical(expected.out, renderLibrary(busyVoicing, busyEvents(), length, 4096)));
    for (const auto delivery : {Delivery::buffered, Delivery::inlined, Delivery::folded})
    {
        for (const auto run : {RunPolicy::always, RunPolicy::usedOnly})
        {
            for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
            {
                auto s = busySettings();
                s.delivery = delivery;
                s.run = run;
                std::size_t allocations = 0;
                const auto r =
                    render(s, busyVoicing, busyEvents(), length, {.blockSize = block, .allocations = &allocations});
                CHECK(identical(r.out, expected.out));
                CHECK(allocations == 0);
            }
        }
    }
}

TEST_CASE("Every source's bound holds over random scenarios, and small bounds fail", "[sources-model]")
{
    struct Totals
    {
        std::uint64_t checked = 0;
        std::array<std::uint64_t, sourceCount> outside{};
    };
    std::array<Totals, 4> totals{};
    const std::array rules{BoundRule::tight, BoundRule::keytrackEnds, BoundRule::levelOnly, BoundRule::lfoEnds};
    anoesis::dsp::Random random{0x5EED5};
    for (const std::size_t sampleRate : {std::size_t{44100}, std::size_t{48000}, std::size_t{96000}})
    {
        for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
        {
            const std::size_t length = sampleRate / 2;
            const auto events = randomScenario(random, length, sampleRate);
            for (std::size_t r = 0; r < rules.size(); ++r)
            {
                auto s = busySettings();
                s.run = RunPolicy::always;
                s.checkBounds = true;
                s.bounds = rules[r];
                const auto out = render(s, busyVoicing, events, length,
                                        {.blockSize = block, .sampleRate = static_cast<float>(sampleRate)});
                for (std::size_t i = 1; i < sourceCount; ++i)
                {
                    totals[r].checked += out.stats.checked[i];
                    totals[r].outside[i] += out.stats.outside[i];
                }
            }
        }
    }
    CHECK(totals[0].checked > 1'000'000);
    for (std::size_t i = 1; i < sourceCount; ++i)
    {
        INFO("source " << i);
        CHECK(totals[0].outside[i] == 0);
    }
    CHECK(totals[1].outside[at(Source::keytrack)] > 0);
    CHECK(totals[2].outside[at(Source::modEnv1)] > 1000);
    CHECK(totals[2].outside[at(Source::velocity)] > 0);
    CHECK(totals[3].outside[at(Source::lfo1)] > 0);
}

TEST_CASE("A source reconstructed when its route appears mid-note is the always-run source to the bit",
          "[sources-model]")
{
    const std::size_t length = 30000;
    const Voicing voicing{.release = {0.1f, 0.1f}, .polyphony = 1};
    struct Case
    {
        Source source = Source::none;
        bool free = false;
    };
    for (const auto& c :
         {Case{.source = Source::modEnv1}, Case{.source = Source::lfo1}, Case{.source = Source::lfo1, .free = true}})
    {
        INFO("source " << at(c.source) << (c.free ? " (free)" : ""));
        // Note 2 steals the voice (Polyphony 1), a retrigger the replay must include.
        const std::vector<SourceEvent> events{
            noteOn(0, 57),
            noteOn(6000, 59),
            route(12345, 0, c.source, Dest::pitchA, 0.1f),
            noteOff(20000, 59),
        };
        auto s = busySettings();
        s.modEnvelopes[0] = {.attack = 0.05f, .decay = 0.2f, .sustain = 0.5f, .release = 0.3f};
        s.lfos[0] = {.waveform = anoesis::dsp::LfoWaveform::sine, .rate = 6.0f, .free = c.free};
        s.run = RunPolicy::always;
        const auto always = render(s, voicing, events, length);
        s.run = RunPolicy::usedOnly;
        s.late = LateStart::reconstruct;
        const auto rebuilt = render(s, voicing, events, length, {.blockSize = 37});
        CHECK(identical(rebuilt.out, always.out));
        CHECK((rebuilt.replayed > 0) == !c.free);
        // From its note-on state instead: exact only for a free LFO, which
        // copies the global instance either way.
        s.late = LateStart::nextGrid;
        CHECK(identical(render(s, voicing, events, length).out, always.out) == c.free);
    }
}
