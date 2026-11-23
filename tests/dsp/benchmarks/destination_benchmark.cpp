// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// CPU cost of the per-voice destinations' candidates (M1.5 task 4, stage 1;
// docs/dsp/modulation-destinations.md). One voice held at the init patch
// (A1 one saw, A3), past its attacks, 512 samples per run at 48 kHz, as
// task 3's benchmarks (voice_sources_benchmark.cpp):
//
// - dsp::Voice and DestVoice with no routes;
// - 4 and 16 routes into Volume A, Pan A and Phase A at the decided rates
//   (the knobs from the slot's own ramps);
// - 4 routes into Volume A under each knob source;
// - 16 routes across every per-voice destination;
// - 16 routes with the patch changing at every grid point: amounts stepping,
//   or crossfading (so a crossfade always runs);
// - a Noise slot, and 4 x 8 unison, with 4 routes into Volume and Pan;
// - 16 voices on question 4's pluck patch: the Volume skip test from the
//   routes' bounds, or never.
//
// Tagged [!benchmark][destinations]: run with
// tools/bench.sh "[destinations]" in relwithdebinfo.

#include "../modulation/destination_support.hpp"
#include "../oscillators/oscillator_support.hpp"

#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/modulation/controller_ramp.hpp>
#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/modulation/route_compiler.hpp>
#include <anoesis/dsp/modulation/source_context.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{

namespace dsp = anoesis::dsp;
namespace mod = anoesis::dsp::modulation;
namespace dest = anoesis::test::destinations;
namespace proto = anoesis::test::sources;
using anoesis::test::Buffer;
using Dest = mod::Destination;
using mod::Source;

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;
const dsp::ProcessSpec spec{.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2};

/// Task 3's stage-1 settings (source_benchmark.cpp).
mod::SourceContext stageOneContext()
{
    mod::SourceContext c;
    c.modEnvelopes[0] = {.attack = 0.05f, .decay = 0.3f, .sustain = 0.5f};
    c.modEnvelopes[1] = {.attack = 0.2f, .decay = 0.1f, .sustain = 0.8f};
    c.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = 5.0f, .gain = 10.0f};
    c.lfos[1] = {.waveform = dsp::LfoWaveform::triangle, .rate = 4.0f, .gain = 10.0f};
    return c;
}

using Routes = std::function<void(mod::Patch&)>;

/// One of each kind into `d`: an envelope, an LFO, velocity, the wheel.
Routes four(Dest d)
{
    return [d](mod::Patch& p)
    {
        p.set(0, {.source = Source::modEnv1, .destination = d, .amount = 0.5f});
        p.set(4, {.source = Source::lfo1, .destination = d, .amount = 0.5f});
        p.set(8, {.source = Source::velocity, .destination = d, .amount = 0.5f});
        p.set(10, {.source = Source::modWheel, .destination = d, .amount = 0.5f});
    };
}

/// Every source into `d`.
Routes sixteen(Dest d)
{
    return [d](mod::Patch& p)
    {
        const std::array<Source, 16> sources{
            Source::modEnv1,  Source::modEnv1,  Source::modEnv2,      Source::modEnv2,
            Source::lfo1,     Source::lfo1,     Source::lfo2,         Source::lfo2,
            Source::velocity, Source::keytrack, Source::modWheel,     Source::aftertouch,
            Source::ampEnvA,  Source::ampEnvB,  Source::stepVelocity, Source::velocity,
        };
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            p.set(i, {.source = sources[i], .destination = d, .amount = 0.05f});
        }
    };
}

/// Every per-voice destination: two routes each to Volume, Pan and Phase,
/// and Pitch, Mix, Cutoff, Reso and the LFOs.
void mixed(mod::Patch& p)
{
    const std::array<mod::Slot, 16> slots{
        {
            {.source = Source::modEnv1, .destination = Dest::volumeA, .amount = -2.0f},
            {.source = Source::modEnv1, .destination = Dest::cutoffA, .amount = 2.0f},
            {.source = Source::modEnv2, .destination = Dest::pitchA, .amount = 0.01f},
            {.source = Source::modEnv2, .destination = Dest::phaseA, .amount = 1.0f},
            {.source = Source::lfo1, .destination = Dest::panA, .amount = 2.0f},
            {.source = Source::lfo1, .destination = Dest::lfo2Gain, .amount = 1.0f},
            {.source = Source::lfo2, .destination = Dest::mixA, .amount = 0.5f},
            {.source = Source::lfo2, .destination = Dest::phaseAB, .amount = 0.5f},
            {.source = Source::velocity, .destination = Dest::volumeAB, .amount = -1.0f},
            {.source = Source::keytrack, .destination = Dest::pitchA, .amount = 0.01f},
            {.source = Source::modWheel, .destination = Dest::panAB, .amount = 1.0f},
            {.source = Source::aftertouch, .destination = Dest::mixAB, .amount = 0.5f},
            {.source = Source::ampEnvA, .destination = Dest::resonanceA, .amount = 1.0f},
            {.source = Source::ampEnvB, .destination = Dest::lfo1Rate, .amount = 0.5f},
            {.source = Source::stepVelocity, .destination = Dest::mixA, .amount = 0.5f},
            {.source = Source::velocity, .destination = Dest::cutoffA, .amount = 1.0f},
        },
    };
    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        p.set(i, slots[i]);
    }
}

/// A voice (dsp::Voice or DestVoice) held past its attacks, with the
/// engine's state. `changing` sets slot 0's amount anew at every grid point.
template <typename V>
struct Held
{
    mod::ControllerRamp wheel;
    mod::ControllerRamp pressure;
    mod::Patch patch;
    std::array<dsp::Lfo, 2> freeLfos{dsp::Lfo{}, dsp::Lfo{}};
    mod::SourceContext context = stageOneContext();
    std::array<dsp::LinearSmoother, 4> volumeRamps{};
    std::array<dsp::LinearSmoother, 4> panRamps{};
    std::unique_ptr<V> voice;
    Buffer out{2, blockSize};
    dsp::LinearSmoother cutoff;
    dsp::VoiceKnobs knobs;
    bool changing = false;
    float amount = 0.5f;

    Held(const Routes& routes, const std::function<void(V&)>& setUp, bool changeEverySegment = false)
        : changing(changeEverySegment)
    {
        wheel.prepare(rate);
        pressure.prepare(rate);
        wheel.set(0, 0.5f);
        pressure.set(0, 0.3f);
        if (routes)
        {
            routes(patch);
        }
        context.patch = &patch;
        context.modWheel = &wheel;
        context.aftertouch = &pressure;
        context.freeLfos = &freeLfos;
        context.stepVelocity = 0.5f;
        for (std::size_t i = 0; i < 4; ++i)
        {
            volumeRamps[i].setCurrentAndTarget(10.0f);
            panRamps[i].setCurrentAndTarget(5.0f);
        }
        voice = std::make_unique<V>(anoesis::test::d6::minBlepTable64(), 7);
        for (std::size_t s = 0; s < 4; ++s)
        {
            voice->slot(s).setWaveform(dsp::Waveform::saw);
            voice->slot(s).setVoices(s == 0 ? 1 : 0);
        }
        if (setUp)
        {
            setUp(*voice);
        }
        voice->prepare(spec);
        cutoff.setCurrentAndTarget(5.0f);
        knobs.cutoff = {&cutoff, &cutoff};
        knobs.controlCutoff = &cutoff;
        knobs.sources = &context;
        voice->noteOn(57.0f, knobs);
        for (int k = 0; k < 40; ++k) // past the attacks
        {
            run();
        }
    }

    float run()
    {
        out.block().clear();
        if (!changing)
        {
            voice->process(out.block(), knobs);
            knobs.time += blockSize;
            return out.channel(0)[blockSize - 1];
        }
        for (std::size_t start = 0; start < blockSize; start += 32)
        {
            amount = amount > 0.4f ? 0.3f : 0.5f;
            auto slot = patch.slots()[0];
            slot.amount = amount;
            patch.set(0, slot);
            auto k = knobs;
            k.offset = 0;
            voice->process(out.block().subBlock(start, 32), k);
            knobs.time += 32;
        }
        return out.channel(0)[blockSize - 1];
    }
};

using DestV = dest::DestVoice<>;

std::function<void(DestV&)> with(const dest::DestinationSettings& s, const std::function<void(DestV&)>& more = {})
{
    return [s, more](DestV& v)
    {
        v.setDestinationSettings(s);
        if (more)
        {
            more(v);
        }
    };
}

template <typename H>
void bench(const std::string& name, H& h)
{
    BENCHMARK(name + " x512")
    {
        return h.run();
    };
}

} // namespace

TEST_CASE("Destinations: CPU per voice, by destination and candidate", "[!benchmark][destinations]")
{
    {
        Held<dsp::Voice<>> h{nullptr, {}};
        bench("destinations: dsp::Voice, no routes", h);
    }
    {
        Held<DestV> h{nullptr, with(dest::defaultSettings)};
        bench("destinations: prototype, no routes", h);
    }
    for (const Dest d : {Dest::volumeA, Dest::panA, Dest::phaseA})
    {
        std::string name = "Phase A";
        if (d != Dest::phaseA)
        {
            name = d == Dest::volumeA ? "Volume A" : "Pan A";
        }
        {
            Held<DestV> h{four(d), with(dest::defaultSettings)};
            bench("destinations: prototype, " + name + ", 4 routes", h);
        }
        {
            Held<DestV> h{sixteen(d), with(dest::defaultSettings)};
            bench("destinations: prototype, " + name + ", 16 routes", h);
        }
    }
    for (const auto k : {dest::KnobSource::targets, dest::KnobSource::slotRamps, dest::KnobSource::globalRamps})
    {
        std::string name = "(c) global ramps";
        if (k != dest::KnobSource::globalRamps)
        {
            name = k == dest::KnobSource::targets ? "(a) targets" : "(b) slot ramps";
        }
        Held<DestV> h{four(Dest::volumeA), with({.knobs = k})};
        h.voice->setKnobRamps(&h.volumeRamps, &h.panRamps); // still: the same before as after
        bench("destinations: prototype, Volume A, 4 routes, knobs " + name, h);
    }
    {
        Held<DestV> h{&mixed, with(dest::defaultSettings)};
        bench("destinations: prototype, 16 routes, every destination", h);
    }
    {
        Held<DestV> h{&mixed, with(dest::defaultSettings), true};
        bench("destinations: prototype, 16 routes, a change every segment, (a) step", h);
    }
    {
        Held<DestV> h{&mixed, with({.amounts = dest::AmountChange::crossfade, .fadeSegments = 15}), true};
        bench("destinations: prototype, 16 routes, a change every segment, (c) crossfade 10 ms", h);
    }
    {
        Held<DestV> h{&mixed, with({.amounts = dest::AmountChange::crossfade, .fadeSegments = 15})};
        bench("destinations: prototype, 16 routes, no change, (c) crossfade 10 ms", h);
    }
    const auto noise = [](DestV& v)
    {
        v.slot(0).setWaveform(dsp::Waveform::noise);
    };
    {
        Held<DestV> h{nullptr, with(dest::defaultSettings, noise)};
        bench("destinations: prototype, Noise, no routes", h);
    }
    {
        Held<DestV> h{four(Dest::volumeA), with(dest::defaultSettings, noise)};
        bench("destinations: prototype, Noise, Volume A, 4 routes, (b) mixed", h);
    }
    {
        Held<DestV> h{four(Dest::volumeA), with({.noise = dest::NoiseModulation::ignored}, noise)};
        bench("destinations: prototype, Noise, Volume A, 4 routes, (a) ignored", h);
    }
    const auto unison = [](DestV& v)
    {
        for (std::size_t s = 0; s < 4; ++s)
        {
            v.slot(s).setVoices(8);
            v.slot(s).setDetune(0.3f);
            v.slot(s).setStereo(0.5f);
        }
    };
    for (const Dest d : {Dest::volumeAB, Dest::panAB})
    {
        {
            Held<DestV> h{nullptr, with(dest::defaultSettings, unison)};
            if (d == Dest::volumeAB)
            {
                bench("destinations: prototype, 4 x 8 unison, no routes", h);
            }
        }
        Held<DestV> h{four(d), with(dest::defaultSettings, unison)};
        bench(std::string("destinations: prototype, 4 x 8 unison, ") + (d == Dest::volumeAB ? "Volume" : "Pan") +
                  " AB, 4 routes",
              h);
    }
}

TEST_CASE("Destinations: the Volume skip test over 16 voices", "[!benchmark][destinations]")
{
    // Question 4's pluck patch: A1 and B1 saws, B1 at Volume 0 lifted by Mod
    // Env 1 (decay 50 ms, sustain 0) at +10; 16 notes held.
    for (const auto rule : {dest::VolumeSkip::never, dest::VolumeSkip::bound})
    {
        proto::SourceSettings s;
        s.modEnvelopes[0] = {.attack = 0.0f, .decay = 0.05f, .sustain = 0.0f, .release = 0.05f};
        dest::DestVoicing d;
        d.base.voices = {1, 0, 1, 0};
        d.base.polyphony = 16;
        d.volume[2] = 0.0f;
        auto engine =
            std::make_unique<dest::DestEngine>(proto::table(), s, dest::DestinationSettings{.volumeSkip = rule});
        engine->setSlots(d);
        engine->prepare(spec);
        proto::configureAllocator(engine->voices(), d.base);
        std::vector<dest::DestEvent> start{dest::route(0, 0, Source::modEnv1, Dest::volumeB, 10.0f)};
        for (int k = 0; k < 16; ++k)
        {
            start.push_back(dest::noteOn(0, 40 + k));
        }
        Buffer out{2, blockSize};
        engine->process(out.block(), start);
        for (int k = 0; k < 20; ++k) // past the plucks
        {
            engine->process(out.block(), {});
        }
        BENCHMARK(std::string("destinations: 16 voices, B1 lifted by a pluck, skip ") +
                  (rule == dest::VolumeSkip::never ? "never" : "from the routes' bound") + " x512")
        {
            engine->process(out.block(), {});
            return out.channel(0)[blockSize - 1];
        };
    }
}
