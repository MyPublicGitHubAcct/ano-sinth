// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The per-voice destinations' prototypes (M1.5 task 4, stage 1;
// docs/dsp/modulation-destinations.md), checked in the default run under
// [destinations-model]: DestVoice is dsp::Voice to the bit wherever the
// library is built (no Volume, Pan or Phase route); every candidate gives
// the same output at host blocks of 1, 37 and 4096 with no allocation; each
// destination moves in its own units and stops at its clamp; Volume lifts a
// slot at Volume 0, and the skip test hears it; a skipped slot keeps its
// Phase; a held note at Volume 0 stays busy; a deliberately small Volume
// bound skips audio. The measurements are the hidden [destinations-report].

#include "destination_support.hpp"

#include <anoesis/test/buffer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

using namespace anoesis::test::destinations;
using anoesis::test::Buffer;
namespace vs = anoesis::test::voice_sources;
namespace dsp = anoesis::dsp;

constexpr std::array blockSizes{std::size_t{1}, std::size_t{37}, std::size_t{4096}};

bool same(const Buffer& a, const Buffer& b)
{
    return proto::identical(a, b);
}

double energyFrom(const Buffer& b, std::size_t from, std::size_t to)
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

/// A sine on A1 alone, filters bypassed: what a destination does reads
/// cleanly.
DestVoicing sineVoicing()
{
    DestVoicing d;
    d.base.waves = {dsp::Waveform::sine, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw};
    d.base.voices = {1, 0, 0, 0};
    d.base.bypass = true;
    d.base.polyphony = 1;
    return d;
}

/// The candidates the block-size test covers: every knob source with every
/// amount rule, the Noise and skip rules alternating among them (9 of the
/// 36 combinations, each value of each switch at least three times).
std::vector<DestinationSettings> everyCandidate()
{
    std::vector<DestinationSettings> all;
    std::size_t k = 0;
    for (const auto knobs : {KnobSource::targets, KnobSource::slotRamps, KnobSource::globalRamps})
    {
        for (const std::size_t fade : {std::size_t{0}, std::size_t{1}, std::size_t{15}})
        {
            all.push_back({
                .knobs = knobs,
                .amounts = fade == 0 ? AmountChange::step : AmountChange::crossfade,
                .fadeSegments = fade == 0 ? 1 : fade,
                .noise = k % 2 == 0 ? NoiseModulation::mixed : NoiseModulation::ignored,
                .volumeSkip = (k / 2) % 2 == 0 ? VolumeSkip::bound : VolumeSkip::never,
            });
            ++k;
        }
    }
    return all;
}

/// A busy patch on the new destinations: Volume, Pan and Phase routes from
/// moving sources, a Noise slot, unison, knob changes and patch changes
/// mid-note, steals.
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
             route(7003, 12, Source::modWheel, Dest::volumeA, 5.0f),
             noteOn(9013, 64, 127),
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

} // namespace

TEST_CASE("DestVoice with no Volume, Pan or Phase route is dsp::Voice to the bit", "[destinations-model]")
{
    // Task 3's busy patch (Pitch, Mix, Cutoff and the LFOs, every source)
    // and the init patch, through the real VoiceAllocator.
    const auto busy = vs::busyEvents();
    const std::vector<SourceEvent> init{
        proto::noteOn(0, 57),
        proto::noteOn(4000, 64),
        proto::noteOff(9000, 57),
        proto::noteOff(12000, 64),
    };
    const std::vector<DestinationSettings> settings{
        {.knobs = KnobSource::targets, .noise = NoiseModulation::ignored, .volumeSkip = VolumeSkip::never},
        {.knobs = KnobSource::slotRamps},
        {.knobs = KnobSource::globalRamps, .amounts = AmountChange::crossfade, .fadeSegments = 15},
    };
    for (const auto blockSize : blockSizes)
    {
        for (const auto* events : {&busy, &init})
        {
            const auto sources = events == &busy ? vs::busySettings() : proto::SourceSettings{};
            const auto voicing = events == &busy ? vs::busyVoicing : Voicing{};
            const auto library = renderLibrary(sources, voicing, *events, 24000, blockSize);
            for (const auto& s : settings)
            {
                DestVoicing d;
                d.base = voicing;
                const auto out = renderDest(s, sources, d, wrapAll(*events), 24000, {.blockSize = blockSize});
                CHECK(same(out, library));
            }
        }
    }
}

TEST_CASE("Every candidate gives the same output at any host block size, with no allocation", "[destinations-model]")
{
    const auto events = busyDestinations();
    for (const auto& s : everyCandidate())
    {
        std::size_t allocations = 0;
        const auto reference = renderDest(s, busySources(), busyVoicing(), events, 26000,
                                          {.blockSize = 4096, .allocations = &allocations});
        CHECK(energyFrom(reference, 0, reference.numSamples()) > 1.0);
        for (const auto blockSize : {std::size_t{1}, std::size_t{37}})
        {
            const auto out = renderDest(s, busySources(), busyVoicing(), events, 26000,
                                        {.blockSize = blockSize, .allocations = &allocations});
            CHECK(same(out, reference));
        }
        CHECK(allocations == 0);
    }
}

TEST_CASE("Each destination moves in its own units and stops at its clamp", "[destinations-model]")
{
    // The wheel set long before the note, so the source is a constant 1
    // (or 0.5) by then. A route's effect is compared with the knob turned
    // to where knob + amount x source lands (clamped): the same to the bit.
    const std::size_t at = 2400;
    const auto render = [&](const DestVoicing& d, std::vector<DestEvent> e, int wheelValue = 127)
    {
        e.push_back(wheel(0, wheelValue));
        e.push_back(noteOn(at, 57));
        return renderDest(defaultSettings, noSources, d, e, 9600);
    };
    const auto knobAt = [](float volume, float pan)
    {
        auto d = sineVoicing();
        d.volume[0] = volume;
        d.pan[0] = pan;
        return d;
    };
    SECTION("Volume: a knob unit per unit of amount, clamped to [0, 10]")
    {
        CHECK(same(render(knobAt(2.0f, 5.0f), {route(0, 8, Source::modWheel, Dest::volumeA, 3.0f)}),
                   render(knobAt(5.0f, 5.0f), {})));
        CHECK(same(render(knobAt(0.0f, 5.0f), {route(0, 8, Source::modWheel, Dest::volumeAB, 7.0f)}),
                   render(knobAt(7.0f, 5.0f), {})));
        CHECK(same(render(knobAt(8.0f, 5.0f), {route(0, 8, Source::modWheel, Dest::volumeA, 5.0f)}),
                   render(knobAt(10.0f, 5.0f), {})));
        // Below 0 it is silence, not the waveform inverted.
        const auto under = render(knobAt(3.0f, 5.0f), {route(0, 8, Source::modWheel, Dest::volumeA, -8.0f)});
        CHECK(energyFrom(under, 0, under.numSamples()) <= 0.0);
        // Volume B leaves part A alone.
        CHECK(same(render(knobAt(4.0f, 5.0f), {route(0, 8, Source::modWheel, Dest::volumeB, 5.0f)}),
                   render(knobAt(4.0f, 5.0f), {})));
    }
    SECTION("Pan: a knob unit per unit of amount (5 the centre), clamped to [0, 10]")
    {
        CHECK(same(render(knobAt(10.0f, 5.0f), {route(0, 8, Source::modWheel, Dest::panA, 2.5f)}),
                   render(knobAt(10.0f, 7.5f), {})));
        CHECK(same(render(knobAt(10.0f, 3.0f), {route(0, 8, Source::modWheel, Dest::panAB, 10.0f)}),
                   render(knobAt(10.0f, 10.0f), {})));
        CHECK(same(render(knobAt(10.0f, 3.0f), {route(0, 8, Source::modWheel, Dest::panA, -10.0f)}),
                   render(knobAt(10.0f, 0.0f), {})));
    }
    SECTION("Phase: 10 units a cycle, wrapping")
    {
        auto quarter = sineVoicing();
        quarter.phaseDegrees[0] = 90.0f;
        CHECK(same(render(sineVoicing(), {route(0, 8, Source::modWheel, Dest::phaseA, 2.5f)}), render(quarter, {})));
        CHECK(same(render(sineVoicing(), {route(0, 8, Source::modWheel, Dest::phaseA, 10.0f)}),
                   render(sineVoicing(), {})));
        auto threeQuarters = sineVoicing();
        threeQuarters.phaseDegrees[0] = 270.0f;
        CHECK(same(render(sineVoicing(), {route(0, 8, Source::modWheel, Dest::phaseA, -2.5f)}),
                   render(threeQuarters, {})));
    }
    SECTION("Pitch: an octave per unit, unclamped, and finite at the extremes")
    {
        // 16 routes at +10 and at -10 from a source at 1: +-160 octaves.
        for (const float amount : {10.0f, -10.0f})
        {
            std::vector<DestEvent> e;
            e.reserve(16);
            for (std::size_t slot = 0; slot < 16; ++slot)
            {
                e.push_back(route(0, slot, slot < 8 ? Source::velocity : Source::modWheel, Dest::pitchAB, amount));
            }
            auto d = sineVoicing();
            d.base.waves[0] = dsp::Waveform::saw;
            d.base.bypass = false;
            const auto out = render(d, e);
            bool finite = true;
            for (std::size_t c = 0; c < 2; ++c)
            {
                for (const float x : out.channel(c))
                {
                    finite = finite && std::isfinite(x);
                }
            }
            CHECK(finite);
        }
    }
}

TEST_CASE("A Volume route lifts a slot at Volume 0, and a held note there stays busy", "[destinations-model]")
{
    auto d = sineVoicing();
    d.volume[0] = 0.0f;
    // A note held at Volume 0, then a route lifts it at 9000.
    const std::vector<DestEvent> e{
        wheel(0, 127),
        noteOn(2400, 57),
        route(9000, 8, Source::modWheel, Dest::volumeA, 5.0f),
    };
    bool activeWhileSilent = true;
    const auto out = renderDest(defaultSettings, noSources, d, e, 14400,
                                {
                                    .blockSize = 600,
                                    .after =
                                        [&](DestEngine& engine, std::size_t end)
                                    {
                                        if (end > 2400 && end <= 9000)
                                        {
                                            activeWhileSilent = activeWhileSilent && engine.voices().activeNotes() == 1;
                                        }
                                    },
                                });
    CHECK(activeWhileSilent);
    CHECK(energyFrom(out, 0, 9000) <= 0.0);
    CHECK(energyFrom(out, 9040, 14400) > 1.0);
    // At amount -5 it can't lift it: silent throughout.
    const std::vector<DestEvent> down{
        wheel(0, 127),
        noteOn(2400, 57),
        route(9000, 8, Source::modWheel, Dest::volumeA, -5.0f),
    };
    const auto none = renderDest(defaultSettings, noSources, d, down, 14400);
    CHECK(energyFrom(none, 0, none.numSamples()) <= 0.0);
}

TEST_CASE("A deliberately small Volume bound skips audio; the routes' bound doesn't", "[destinations-model]")
{
    // A1 a saw at Volume 5, and LFO 1 a unipolar pulse at 40 Hz (0 or 1)
    // on Volume A at -10: silent while the pulse is up. Where it falls
    // inside a segment, its value at the grid point (1) says "silent" for
    // a segment that becomes audible.
    DestVoicing d;
    d.base.voices = {1, 0, 0, 0};
    d.base.polyphony = 4;
    d.volume[0] = 5.0f;
    proto::SourceSettings s;
    s.lfos[0] = {.waveform = dsp::LfoWaveform::pulse, .rate = 8.67f, .gain = 10.0f, .offset = 10.0f};
    std::vector<DestEvent> e{route(0, 4, Source::lfo1, Dest::volumeA, -10.0f)};
    for (int k = 0; k < 6; ++k)
    {
        e.push_back(noteOn(1000 + (static_cast<std::size_t>(k) * 7000), 50 + k));
        e.push_back(noteOff(6000 + (static_cast<std::size_t>(k) * 7000), 50 + k));
    }
    const auto run = [&](VolumeSkip rule)
    {
        return renderDest({.volumeSkip = rule}, s, d, e, 48000);
    };
    const auto never = run(VolumeSkip::never);
    const auto bound = run(VolumeSkip::bound);
    const auto small = run(VolumeSkip::firstValue);
    const auto diff = [&](const Buffer& a)
    {
        double num = 0.0;
        for (std::size_t c = 0; c < 2; ++c)
        {
            for (std::size_t i = 0; i < a.numSamples(); ++i)
            {
                const auto x = static_cast<double>(a.channel(c)[i]) - static_cast<double>(never.channel(c)[i]);
                num += x * x;
            }
        }
        return num / energyFrom(never, 0, never.numSamples());
    };
    // Skipping a silent slot differs from rendering it only by the
    // band-limited jump's residual at its return (far below -60 dB).
    CHECK(diff(bound) < 1e-6);
    CHECK(diff(small) > 1e-3);
}

TEST_CASE("A skipped slot comes back with the Phase offset a rendered one has", "[destinations-model]")
{
    using Slot = DestSlot<64>;
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
    std::array<float, 32> const volume{};
    for (std::size_t segment = 0; segment < 40; ++segment)
    {
        for (std::size_t i = 0; i < 32; ++i)
        {
            phase[i] = 3.7f * std::sin(0.01f * static_cast<float>((segment * 32) + i));
        }
        const SlotModulation m{.volume = volume, .position = 0, .segment = 32, .phase = phase};
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
    CHECK(skipped.phaseOffset() == rendered.phaseOffset());
    for (std::size_t v = 0; v < 3; ++v)
    {
        CHECK(skipped.oscillator(v).nextPhase() == rendered.oscillator(v).nextPhase());
    }
}
