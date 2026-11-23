// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The per-voice destinations as built (M1.5 task 4, stage 2;
// docs/dsp/modulation-destinations.md, "As built"): Volume and Phase per
// sample, Pan ramped on the grid, each through its units and clamp; the
// slot's own knob ramps, gain ramps that move on (StereoGain::skip()) and
// presence ramps for unison's fades; a crossfade of any change of the
// routes over ceil(10 ms x fs / 32) segments; Noise mixed; the Volume skip
// test from the routes' bound.
//
// - dsp::Voice is the stage-1 prototype in its decided configuration
//   (DestVoice with decidedAt()), to the bit, at host blocks of 1, 37 and
//   4096, with no allocation, through the real VoiceAllocator.
// - With no Volume, Pan or Phase route and no change of the routes
//   mid-note, it is dsp::Voice as it was (the prototype stepping, which
//   stage 1 pinned to it).
// - Each destination moves in its own units and stops at its clamp, to the
//   bit against the knob; a crossfade takes exactly its segments; a held
//   note at Volume 0 stays busy; a skipped slot keeps its Phase; Noise moves
//   as its knob does.
// - volumeLaw, and StereoGain::skip() against processing.

#include "destination_support.hpp"

#include <anoesis/dsp/control/stereo_gain.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/modulation/route_compiler.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/slot_modulation.hpp>
#include <anoesis/dsp/oscillators/volume_law.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace
{

using namespace anoesis::test::destinations;
using anoesis::test::Buffer;
namespace vs = anoesis::test::voice_sources;
namespace dsp = anoesis::dsp;

bool same(const Buffer& a, const Buffer& b)
{
    return proto::identical(a, b);
}

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

double rmsOf(const Buffer& b, std::size_t channel, std::size_t from, std::size_t to)
{
    double e = 0.0;
    for (std::size_t i = from; i < to; ++i)
    {
        const auto x = static_cast<double>(b.channel(channel)[i]);
        e += x * x;
    }
    return std::sqrt(e / static_cast<double>(to - from));
}

/// A sine on A1 alone, filters bypassed.
DestVoicing sineVoicing()
{
    DestVoicing d;
    d.base.waves = {dsp::Waveform::sine, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw};
    d.base.voices = {1, 0, 0, 0};
    d.base.bypass = true;
    d.base.polyphony = 1;
    return d;
}

/// Stage 1's busy patch on the new destinations (destination_candidates_test),
/// with unison counts changing mid-note.
std::vector<DestEvent> busyDestinations()
{
    std::vector<DestEvent> e{
        route(0, 0, Source::modEnv1, Dest::volumeA, -6.0f),  route(0, 1, Source::modEnv1, Dest::panB, 4.0f),
        route(0, 4, Source::lfo1, Dest::phaseA, 1.5f),       route(0, 5, Source::lfo1, Dest::volumeB, 3.0f),
        route(0, 6, Source::lfo2, Dest::panAB, -2.0f),       route(0, 8, Source::velocity, Dest::volumeAB, -2.0f),
        route(0, 9, Source::modWheel, Dest::phaseB, 2.5f),   route(0, 10, Source::keytrack, Dest::pitchA, 0.2f),
        route(0, 11, Source::aftertouch, Dest::mixA, -3.0f),
    };
    for (const auto& ev : {
             noteOn(0, 57, 100),
             wheel(2001, 64),
             noteOn(5001, 61, 60),
             volumeKnob(6100, 0, 3.0f),
             panKnob(6600, 2, 8.0f),
             unison(6900, 0, 5),
             route(7003, 12, Source::modWheel, Dest::volumeA, 5.0f),
             noteOn(9013, 64, 127),
             unison(10500, 2, 1),
             route(11000, 4, Source::lfo1, Dest::phaseA, -3.0f),
             route(12500, 1, Source::none, Dest::none, 0.0f),
             volumeKnob(13000, 3, 0.0f),
             noteOff(15000, 57),
             route(16000, 6, Source::none, Dest::none, 0.0f),
             noteOff(17000, 61),
             noteOn(18000, 50, 30),
             wheel(18500, 127),
             noteOff(19000, 64),
             noteOff(23000, 50),
         })
    {
        e.push_back(ev);
    }
    return e;
}

proto::SourceSettings busySources()
{
    auto s = vs::busySettings();
    s.lfos[1].waveform = dsp::LfoWaveform::saw;
    return s;
}

DestVoicing busyVoicing()
{
    DestVoicing d;
    d.base = vs::busyVoicing;
    d.base.voices = {3, 1, 2, 1};
    d.base.waves = {dsp::Waveform::saw, dsp::Waveform::pulse, dsp::Waveform::saw, dsp::Waveform::noise};
    d.base.inputs = {FilterInput::own, FilterInput::both};
    d.volume = {8.0f, 6.0f, 10.0f, 4.0f};
    d.pan = {5.0f, 3.0f, 6.0f, 5.0f};
    d.stereo = {0.6f, 0.0f, 0.3f, 0.5f};
    d.invert = {false, true, false, false};
    d.phaseDegrees = {0.0f, 90.0f, 45.0f, 0.0f};
    return d;
}

/// A random scenario on the new destinations: notes, the wheel, knob and
/// unison changes, and the patch changing mid-note (crossfades queueing).
std::vector<DestEvent> randomDestinations(dsp::Random& random, std::size_t length, std::size_t sampleRate)
{
    constexpr std::array destinations{
        Dest::volumeA, Dest::volumeB, Dest::volumeAB, Dest::panA,   Dest::panB,  Dest::panAB,
        Dest::phaseA,  Dest::phaseB,  Dest::phaseAB,  Dest::pitchA, Dest::mixAB, Dest::cutoffA,
    };
    constexpr std::array sources{
        Source::modEnv1,  Source::modEnv2,  Source::lfo1,       Source::lfo2,
        Source::velocity, Source::modWheel, Source::aftertouch, Source::keytrack,
    };
    std::vector<DestEvent> e;
    const auto pickRoute = [&](std::size_t t)
    {
        const auto r = random.nextUint32();
        const auto slot = static_cast<std::size_t>(r % 16);
        const auto d = destinations[(r >> 4U) % destinations.size()];
        const auto s = sources[(r >> 8U) % sources.size()];
        const float amount = ((random.nextUnipolar() * 2.0f) - 1.0f) * 6.0f;
        e.push_back(route(t, slot, s, d, amount));
    };
    for (int k = 0; k < 6; ++k)
    {
        pickRoute(0);
    }
    std::size_t t = 0;
    int key = 60;
    while (t < length)
    {
        const auto r = random.nextUint32();
        switch (r % 7)
        {
        case 0:
            key = 40 + static_cast<int>((r >> 8U) % 40);
            e.push_back(noteOn(t, key, 1 + static_cast<int>((r >> 16U) % 127)));
            break;
        case 1:
            e.push_back(noteOff(t, key));
            break;
        case 2:
            e.push_back(wheel(t, static_cast<int>((r >> 8U) % 128)));
            break;
        case 3:
            pickRoute(t);
            break;
        case 4:
            e.push_back(volumeKnob(t, (r >> 8U) % 4, static_cast<float>((r >> 12U) % 11)));
            break;
        case 5:
            e.push_back(panKnob(t, (r >> 8U) % 4, static_cast<float>((r >> 12U) % 11)));
            break;
        default:
            e.push_back(unison(t, (r >> 8U) % 4, static_cast<int>((r >> 12U) % 5)));
            break;
        }
        t += 1 + ((r >> 20U) % (sampleRate / 25));
    }
    return e;
}

} // namespace

TEST_CASE("Destinations: dsp::Voice is the stage-1 prototype as decided, to the bit at host blocks 1, 37 and 4096, "
          "with no allocation",
          "[modulation]")
{
    SECTION("The busy patch at 48 kHz")
    {
        const auto events = busyDestinations();
        for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
        {
            std::size_t allocations = 0;
            const auto built = renderBuilt(busySources(), busyVoicing(), events, 26000,
                                           {.blockSize = block, .allocations = &allocations});
            const auto prototype =
                renderDest(decidedAt(48000.0f), busySources(), busyVoicing(), events, 26000, {.blockSize = block});
            CHECK(energyOf(built, 0, built.numSamples()) > 1.0);
            CHECK(same(built, prototype));
            CHECK(allocations == 0);
        }
    }
    SECTION("A Pan move on a slot skipped at Volume 0, then a Volume route that lifts it")
    {
        // At Volume 0 a Pan move leaves the slot's gains at 0 (no ramp), so
        // the slot is skipped while its own Pan ramp moves; the route that
        // lifts it 1 ms later reads that ramp, which the skip must have moved
        // on.
        auto d = sineVoicing();
        d.base.waves[0] = dsp::Waveform::saw;
        d.volume[0] = 0.0f;
        const std::vector<DestEvent> e{
            wheel(0, 127),
            noteOn(2400, 57),
            panKnob(6000, 0, 10.0f),
            route(6048, 8, Source::modWheel, Dest::volumeA, 10.0f),
        };
        for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
        {
            const auto built = renderBuilt(noSources, d, e, 12000, {.blockSize = block});
            const auto prototype = renderDest(decidedAt(48000.0f), noSources, d, e, 12000, {.blockSize = block});
            CHECK(energyOf(built, 6100, 12000) > 1.0);
            CHECK(same(built, prototype));
        }
    }
    SECTION("Random scenarios at 44.1 and 96 kHz")
    {
        dsp::Random random{0x5EED};
        for (const float rate : {44100.0f, 96000.0f})
        {
            const auto length = static_cast<std::size_t>(rate * 0.8f);
            const auto events = randomDestinations(random, length, static_cast<std::size_t>(rate));
            auto voicing = busyVoicing();
            voicing.base.polyphony = 4;
            const auto prototype = renderDest(decidedAt(rate), busySources(), voicing, events, length,
                                              {.blockSize = 4096, .sampleRate = rate});
            for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
            {
                std::size_t allocations = 0;
                const auto built = renderBuilt(busySources(), voicing, events, length,
                                               {.blockSize = block, .sampleRate = rate, .allocations = &allocations});
                CHECK(same(built, prototype));
                CHECK(allocations == 0);
            }
        }
    }
}

TEST_CASE("Destinations: a slot that unison brings back starts at Phase plus the offset as it stands, whatever the "
          "host's blocks",
          "[modulation]")
{
    // Found in stage 2: a slot with no voice sounding isn't called, so its
    // offset in force stopped where the last call left it, which depended on
    // the host's blocks (a call runs on to its chunk's end); a voice that
    // unison brought back started there. The scenario that found it: the
    // random scenario at 44.1 kHz with B2 a saw (its unison goes to 0 at
    // 5187 and back to 3 at 6786, under Phase routes).
    dsp::Random random{0x5EED};
    const auto events = randomDestinations(random, 35280, 44100);
    auto voicing = busyVoicing();
    voicing.base.polyphony = 4;
    voicing.base.waves[3] = dsp::Waveform::saw;
    const DestRenderOptions options{.blockSize = 4096, .sampleRate = 44100.0f};
    const auto reference = renderBuilt(busySources(), voicing, events, 35280, options);
    for (const std::size_t block : {std::size_t{1}, std::size_t{37}})
    {
        auto o = options;
        o.blockSize = block;
        CHECK(same(renderBuilt(busySources(), voicing, events, 35280, o), reference));
    }
}

TEST_CASE("Destinations: a Phase route that goes takes its offset back to 0", "[modulation]")
{
    // The wheel at 127 to Phase A at +2.5 (90 degrees), cleared on a held
    // note: once its crossfade is done and the oscillator's band-limiting
    // residuals have passed (64 samples), the note is the one that never had
    // the route, to the bit.
    auto d = sineVoicing();
    d.base.waves[0] = dsp::Waveform::saw;
    const std::size_t clear = 2400 + (32 * 100) - 10; // taken at the grid point 5600
    const std::vector<DestEvent> plain{wheel(0, 127), noteOn(2400, 57)};
    auto routed = plain;
    routed.push_back(route(0, 8, Source::modWheel, Dest::phaseA, 2.5f));
    routed.push_back(route(clear, 8, Source::none, Dest::none, 0.0f));
    const auto a = renderBuilt(noSources, d, plain, 12000);
    const auto b = renderBuilt(noSources, d, routed, 12000);
    const std::size_t settled = 5600 + (dsp::modulation::routeCrossfadeSegments(48000.0f) * 32) + 64;
    CHECK(energyOf(b, 2400, 5600) > 1.0);
    bool equal = true;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = settled; i < 12000; ++i)
        {
            equal =
                equal && std::bit_cast<std::uint32_t>(a.channel(c)[i]) == std::bit_cast<std::uint32_t>(b.channel(c)[i]);
        }
    }
    CHECK(equal);
}

TEST_CASE("Destinations: with no Volume, Pan or Phase route and no change mid-note, dsp::Voice is as it was",
          "[modulation]")
{
    // The prototype stepping with no such route is dsp::Voice before this
    // stage (pinned by stage 1's [destinations-model]).
    const auto busy = wrapAll(vs::busyEvents());
    const std::vector<DestEvent> init{noteOn(0, 57), noteOn(4000, 64), noteOff(9000, 57), noteOff(12000, 64)};
    for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
    {
        for (const auto* events : {&busy, &init})
        {
            const auto sources = events == &busy ? vs::busySettings() : noSources;
            DestVoicing d;
            d.base = events == &busy ? vs::busyVoicing : Voicing{};
            const auto built = renderBuilt(sources, d, *events, 24000, {.blockSize = block});
            const auto before = renderDest(defaultSettings, sources, d, *events, 24000, {.blockSize = block});
            CHECK(same(built, before));
        }
    }
}

TEST_CASE("Destinations: each moves in its own units and stops at its clamp, to the bit against the knob",
          "[modulation]")
{
    const std::size_t at = 2400;
    const auto render = [&](const DestVoicing& d, std::vector<DestEvent> e)
    {
        e.push_back(wheel(0, 127));
        e.push_back(noteOn(at, 57));
        return renderBuilt(noSources, d, e, 9600);
    };
    const auto knobAt = [](float volume, float pan)
    {
        auto d = sineVoicing();
        d.volume[0] = volume;
        d.pan[0] = pan;
        return d;
    };
    // Volume: a knob unit per unit of amount, clamped to [0, 10].
    CHECK(same(render(knobAt(2.0f, 5.0f), {route(0, 8, Source::modWheel, Dest::volumeA, 3.0f)}),
               render(knobAt(5.0f, 5.0f), {})));
    CHECK(same(render(knobAt(0.0f, 5.0f), {route(0, 8, Source::modWheel, Dest::volumeAB, 7.0f)}),
               render(knobAt(7.0f, 5.0f), {})));
    CHECK(same(render(knobAt(8.0f, 5.0f), {route(0, 8, Source::modWheel, Dest::volumeA, 5.0f)}),
               render(knobAt(10.0f, 5.0f), {})));
    const auto under = render(knobAt(3.0f, 5.0f), {route(0, 8, Source::modWheel, Dest::volumeA, -8.0f)});
    CHECK(energyOf(under, 0, under.numSamples()) <= 0.0);
    CHECK(same(render(knobAt(4.0f, 5.0f), {route(0, 8, Source::modWheel, Dest::volumeB, 5.0f)}),
               render(knobAt(4.0f, 5.0f), {})));
    // Pan: 5 the centre, clamped to [0, 10].
    CHECK(same(render(knobAt(10.0f, 5.0f), {route(0, 8, Source::modWheel, Dest::panA, 2.5f)}),
               render(knobAt(10.0f, 7.5f), {})));
    CHECK(same(render(knobAt(10.0f, 3.0f), {route(0, 8, Source::modWheel, Dest::panAB, 10.0f)}),
               render(knobAt(10.0f, 10.0f), {})));
    CHECK(same(render(knobAt(10.0f, 3.0f), {route(0, 8, Source::modWheel, Dest::panA, -10.0f)}),
               render(knobAt(10.0f, 0.0f), {})));
    // Phase: 10 units a cycle, wrapping.
    auto quarter = sineVoicing();
    quarter.phaseDegrees[0] = 90.0f;
    auto threeQuarters = sineVoicing();
    threeQuarters.phaseDegrees[0] = 270.0f;
    CHECK(same(render(sineVoicing(), {route(0, 8, Source::modWheel, Dest::phaseA, 2.5f)}), render(quarter, {})));
    CHECK(same(render(sineVoicing(), {route(0, 8, Source::modWheel, Dest::phaseA, 10.0f)}), render(sineVoicing(), {})));
    CHECK(same(render(sineVoicing(), {route(0, 8, Source::modWheel, Dest::phaseA, -2.5f)}), render(threeQuarters, {})));
}

TEST_CASE("Destinations: a change of the routes crossfades over ceil(10 ms x fs / 32) segments", "[modulation]")
{
    CHECK(dsp::modulation::routeCrossfadeSegments(44100.0f) == 14);
    CHECK(dsp::modulation::routeCrossfadeSegments(48000.0f) == 15);
    CHECK(dsp::modulation::routeCrossfadeSegments(96000.0f) == 30);
    // Velocity 127 to Mix A at -10, from Mix's unity: the sum reaches -10,
    // and the output exact silence, on the crossfade's last sample.
    for (const float rate : {44100.0f, 48000.0f, 96000.0f})
    {
        const std::size_t note = 1000;
        const std::size_t grid = note + (std::size_t{32} * 100);
        const auto n = dsp::modulation::routeCrossfadeSegments(rate) * 32;
        const std::vector<DestEvent> e{noteOn(note, 57), route(grid - 20, 8, Source::velocity, Dest::mixA, -10.0f)};
        const auto out = renderBuilt(noSources, sineVoicing(), e, grid + n + 2000, {.sampleRate = rate});
        CHECK(energyOf(out, grid + n - 1, out.numSamples()) <= 0.0);
        CHECK(energyOf(out, grid + n - 40, grid + n - 1) > 0.0);
        CHECK(energyOf(out, grid - 32, grid) > energyOf(out, grid + (n / 2), grid + (n / 2) + 32));
    }
}

TEST_CASE("Destinations: a held note at Volume 0 stays busy, and a route that lifts it is heard", "[modulation]")
{
    auto d = sineVoicing();
    d.volume[0] = 0.0f;
    const std::vector<DestEvent> e{
        wheel(0, 127),
        noteOn(2400, 57),
        route(9000, 8, Source::modWheel, Dest::volumeA, 5.0f),
    };
    bool busy = true;
    const auto out = renderBuilt(noSources, d, e, 14400,
                                 {
                                     .blockSize = 600,
                                     .afterLibrary =
                                         [&](LibraryDestEngine& engine, std::size_t end)
                                     {
                                         if (end > 2400 && end <= 9000)
                                         {
                                             busy = busy && engine.voices().activeNotes() == 1;
                                         }
                                     },
                                 });
    CHECK(busy);
    CHECK(energyOf(out, 0, 9000) <= 0.0);
    CHECK(energyOf(out, 9100, 14400) > 1.0);
}

TEST_CASE("Destinations: Volume and Pan routes move a Noise slot as its knob does", "[modulation]")
{
    for (const bool isPan : {false, true})
    {
        auto voicing = sineVoicing();
        voicing.base.waves[0] = dsp::Waveform::noise;
        voicing.base.voices[0] = 4;
        voicing.stereo[0] = 0.5f;
        auto reference = voicing;
        if (isPan)
        {
            reference.pan[0] = 10.0f;
        }
        else
        {
            voicing.volume[0] = 0.0f;
            reference.volume[0] = 5.0f;
        }
        const std::vector<DestEvent> base{wheel(0, 127), noteOn(2400, 57)};
        auto e = base;
        e.push_back(route(0, 8, Source::modWheel, isPan ? Dest::panA : Dest::volumeA, 5.0f));
        const auto knob = renderBuilt(noSources, reference, base, 36000);
        const auto routed = renderBuilt(noSources, voicing, e, 36000);
        for (std::size_t c = 0; c < 2; ++c)
        {
            const double ratio = rmsOf(routed, c, 12000, 36000) / rmsOf(knob, c, 12000, 36000);
            CHECK_THAT(20.0 * std::log10(ratio), Catch::Matchers::WithinAbs(0.0, 0.05)); // two noise draws: 0.05 dB
        }
    }
}

TEST_CASE("Destinations: a skipped slot comes back with the Phase offset a rendered one has", "[modulation]")
{
    using Slot = dsp::OscillatorSlot<64>;
    const dsp::ProcessSpec spec{.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 2};
    Slot rendered{proto::table()};
    Slot skipped{proto::table()};
    for (auto* s : {&rendered, &skipped})
    {
        s->prepare(spec);
        s->setWaveform(dsp::Waveform::saw);
        s->setVoices(3);
        s->setDetune(0.2f);
        s->setVolume(0.0f);
        s->noteOn();
    }
    Buffer out{2, 32};
    std::array<float, 32> phase{};
    std::array<float, 32> volume{};
    for (std::size_t segment = 0; segment < 40; ++segment)
    {
        for (std::size_t i = 0; i < 32; ++i)
        {
            phase[i] = 3.7f * std::sin(0.01f * static_cast<float>((segment * 32) + i));
        }
        const dsp::SlotModulation m{.volume = volume, .position = 0, .segment = 32, .phase = phase};
        rendered.process(out.block(), 57.0f, m);
        if (segment < 30)
        {
            REQUIRE(skipped.isSilentUnder(m, 0.0f, 32));
            REQUIRE(skipped.canSkipUnder(m));
            skipped.skip(32, 57.0f, m);
        }
        else
        {
            skipped.process(out.block(), 57.0f, m);
        }
    }
    CHECK(rendered.phaseOffset() != 0); // the modulation reached it
    CHECK(skipped.phaseOffset() == rendered.phaseOffset());
    for (std::size_t v = 0; v < 3; ++v)
    {
        CHECK(skipped.oscillator(v).nextPhase() == rendered.oscillator(v).nextPhase());
    }
}

TEST_CASE("Destinations: volumeLaw is linear, 10 the unity, the knob plus its modulation clamped to [0, 10]",
          "[modulation]")
{
    static_assert(dsp::volumeLaw.knobMin <= 0.0f && dsp::volumeLaw.knobMin >= 0.0f);
    static_assert(dsp::volumeLaw.knobMax <= 10.0f && dsp::volumeLaw.knobMax >= 10.0f);
    CHECK_THAT(dsp::volumeGain({.volume = 10.0f}), Catch::Matchers::WithinULP(1.0f, 0));
    CHECK_THAT(dsp::volumeGain({.volume = 5.0f}), Catch::Matchers::WithinULP(0.5f, 0));
    CHECK_THAT(dsp::volumeGain({.volume = 2.0f, .modulation = 3.0f}), Catch::Matchers::WithinULP(0.5f, 0));
    CHECK_THAT(dsp::volumeGain({.volume = 8.0f, .modulation = 5.0f}), Catch::Matchers::WithinULP(1.0f, 0));
    CHECK_THAT(dsp::volumeGain({.volume = 3.0f, .modulation = -8.0f}), Catch::Matchers::WithinULP(0.0f, 0));
    CHECK_THAT(dsp::volumeGain({.volume = std::nanf(""), .modulation = 1.0f}), Catch::Matchers::WithinULP(0.0f, 0));
}

TEST_CASE("Destinations: StereoGain::skip() leaves the gains where processing would", "[modulation]")
{
    const dsp::ProcessSpec spec{.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 2};
    dsp::StereoGain processed;
    dsp::StereoGain skipped;
    for (auto* g : {&processed, &skipped})
    {
        g->prepare(spec);
        g->setGain(0.2f);
        g->reset();
        g->setGain(0.9f);
        g->setPan(0.4f);
    }
    Buffer scratch{2, 512};
    std::vector<float> ones(512, 1.0f);
    for (const std::size_t n : {std::size_t{1}, std::size_t{37}, std::size_t{100}, std::size_t{512}})
    {
        processed.process(std::span{ones}.first(n), scratch.block());
        skipped.skip(n);
        CHECK_THAT(skipped.currentGains().left, Catch::Matchers::WithinULP(processed.currentGains().left, 0));
        CHECK_THAT(skipped.currentGains().right, Catch::Matchers::WithinULP(processed.currentGains().right, 0));
        CHECK(processed.isSmoothing() == skipped.isSmoothing());
    }
}
