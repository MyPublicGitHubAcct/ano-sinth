// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// CPU cost of the sources' candidates (M1.5 task 3, stage 1;
// tests/dsp/modulation/source_candidates.hpp, docs/dsp/
// modulation-sources.md), one voice held at the init patch (A1 one saw),
// 512 samples per run at 48 kHz, stereo:
//
// - dsp::Voice against SourceVoice with no routes, running only the sources
//   a route reads and running every source (question 7, question 2);
// - 4 and 16 routes into the per-sample destinations (Pitch, Mix) and the
//   grid ones (Cutoff, LFO Gain), by delivery: buffered, inline, folded
//   (question 1), with the controllers steady or moving;
// - an unheard part's amp envelope run always or only while it is a source
//   (question 5).
//
// Tagged [!benchmark][sources]: run with tools/bench.sh "[sources]" in
// relwithdebinfo.

#include "../modulation/source_candidates.hpp"
#include "../oscillators/oscillator_support.hpp"

#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/voice/voice.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string>

using namespace anoesis::test::sources;
using anoesis::test::Buffer;
using Dest = anoesis::dsp::modulation::Destination;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;
const ProcessSpec spec{.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2};

template <typename V>
void initPatch(V& v, bool partBUnheard = false)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        v.slot(s).setWaveform(dsp::Waveform::saw);
        v.slot(s).setVoices(s == 0 ? 1 : 0);
    }
    if (partBUnheard)
    {
        v.setFilterInput(1, FilterInput::none);
    }
}

SourceSettings sourceSettings(Delivery d, RunPolicy run)
{
    SourceSettings s;
    s.modEnvelopes[0] = {.attack = 0.05f, .decay = 0.3f, .sustain = 0.5f};
    s.modEnvelopes[1] = {.attack = 0.2f, .decay = 0.1f, .sustain = 0.8f};
    s.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = 5.0f, .gain = 10.0f};
    s.lfos[1] = {.waveform = dsp::LfoWaveform::triangle, .rate = 4.0f, .gain = 10.0f};
    s.delivery = d;
    s.run = run;
    return s;
}

/// One of each kind: an envelope, an LFO, velocity, the wheel.
void fourRoutes(mod::Patch& p)
{
    p.set(0, {.source = Source::modEnv1, .destination = Dest::pitchA, .amount = 0.01f});
    p.set(4, {.source = Source::lfo1, .destination = Dest::mixA, .amount = 0.5f});
    p.set(8, {.source = Source::velocity, .destination = Dest::mixA, .amount = 0.5f});
    p.set(10, {.source = Source::modWheel, .destination = Dest::pitchA, .amount = 0.01f});
}

/// Every source, two per owner, the misc slots on velocity, keytrack, the
/// wheel, pressure, both amp envelopes and Step Velocity: per-sample
/// destinations and grid ones.
void sixteenRoutes(mod::Patch& p)
{
    const std::array<mod::Slot, 16> slots{
        {
            {.source = Source::modEnv1, .destination = Dest::pitchA, .amount = 0.01f},
            {.source = Source::modEnv1, .destination = Dest::cutoffA, .amount = 2.0f},
            {.source = Source::modEnv2, .destination = Dest::mixA, .amount = 0.5f},
            {.source = Source::modEnv2, .destination = Dest::pitchAB, .amount = 0.01f},
            {.source = Source::lfo1, .destination = Dest::pitchA, .amount = 0.01f},
            {.source = Source::lfo1, .destination = Dest::lfo2Gain, .amount = 1.0f},
            {.source = Source::lfo2, .destination = Dest::mixA, .amount = 0.5f},
            {.source = Source::lfo2, .destination = Dest::cutoffAB, .amount = 1.0f},
            {.source = Source::velocity, .destination = Dest::mixA, .amount = 0.5f},
            {.source = Source::keytrack, .destination = Dest::pitchA, .amount = 0.01f},
            {.source = Source::modWheel, .destination = Dest::pitchA, .amount = 0.01f},
            {.source = Source::aftertouch, .destination = Dest::mixAB, .amount = 0.5f},
            {.source = Source::ampEnvA, .destination = Dest::pitchA, .amount = 0.01f},
            {.source = Source::ampEnvB, .destination = Dest::mixA, .amount = 0.5f},
            {.source = Source::stepVelocity, .destination = Dest::mixA, .amount = 0.5f},
            {.source = Source::velocity, .destination = Dest::cutoffA, .amount = 1.0f},
        },
    };
    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        p.set(i, slots[i]);
    }
}

struct Held
{
    Controller wheel;
    Controller pressure;
    mod::Patch patch;
    SourceSettings settings;
    std::array<dsp::Lfo, 2> freeLfos{dsp::Lfo{}, dsp::Lfo{}};
    float stepVelocity = 0.5f;
    SourceContext context;
    std::unique_ptr<SourceVoice<>> voice;
    Buffer out{2, blockSize};
    dsp::LinearSmoother cutoff;
    VoiceKnobs knobs;
    bool moving = false;
    int value = 0;

    Held(const SourceSettings& s, void (*routes)(mod::Patch&), bool controllersMoving = false,
         bool partBUnheard = false)
        : settings(s), moving(controllersMoving)
    {
        wheel.configure(Smoothing::linear, rate, 5.0f);
        pressure.configure(Smoothing::linear, rate, 5.0f);
        wheel.set(0, 0.5f);
        pressure.set(0, 0.3f);
        if (routes != nullptr)
        {
            routes(patch);
        }
        context = {
            .patch = &patch,
            .settings = &settings,
            .modWheel = &wheel,
            .aftertouch = &pressure,
            .freeLfos = &freeLfos,
            .stepVelocity = &stepVelocity,
        };
        voice = std::make_unique<SourceVoice<>>(anoesis::test::d6::minBlepTable64(), &context, 7);
        initPatch(*voice, partBUnheard);
        voice->prepare(spec);
        cutoff.setCurrentAndTarget(5.0f);
        knobs.cutoff = {&cutoff, &cutoff};
        knobs.controlCutoff = &cutoff;
        voice->noteOn(57.0f, knobs);
        for (int k = 0; k < 40; ++k) // past the attacks
        {
            run();
        }
    }

    float run()
    {
        if (moving)
        {
            // A new value every block: the wheel and pressure ramp over the
            // first 5 ms of each.
            value = (value + 1) % 128;
            wheel.set(knobs.time, static_cast<float>(value) / 127.0f);
            pressure.set(knobs.time, static_cast<float>(127 - value) / 127.0f);
        }
        out.block().clear();
        voice->process(out.block(), knobs);
        knobs.time += blockSize;
        return out.channel(0)[blockSize - 1];
    }
};

std::string_view name(Delivery d)
{
    switch (d)
    {
    case Delivery::buffered:
        return "buffered";
    case Delivery::inlined:
        return "inline";
    case Delivery::folded:
        return "folded";
    }
    return "?";
}

} // namespace

TEST_CASE("Sources: CPU per voice, by delivery, routes and policy", "[!benchmark][sources]")
{
    for (const bool unheard : {false, true})
    {
        auto v = std::make_unique<dsp::Voice<>>(anoesis::test::d6::minBlepTable64(), 7);
        initPatch(*v, unheard);
        v->prepare(spec);
        dsp::LinearSmoother cutoff;
        cutoff.setCurrentAndTarget(5.0f);
        VoiceKnobs knobs;
        knobs.cutoff = {&cutoff, &cutoff};
        knobs.controlCutoff = &cutoff;
        v->noteOn(57.0f, knobs);
        Buffer out{2, blockSize};
        BENCHMARK(std::string("sources: dsp::Voice, init") + (unheard ? ", filter B --" : "") + " x512")
        {
            out.block().clear();
            v->process(out.block(), knobs);
            return out.channel(0)[blockSize - 1];
        };
    }
    {
        Held h{sourceSettings(Delivery::folded, RunPolicy::usedOnly), nullptr};
        BENCHMARK("sources: no routes, used only x512")
        {
            return h.run();
        };
    }
    {
        Held h{sourceSettings(Delivery::buffered, RunPolicy::always), nullptr};
        BENCHMARK("sources: no routes, every source always x512")
        {
            return h.run();
        };
    }
    for (const bool moving : {false, true})
    {
        for (const auto d : {Delivery::buffered, Delivery::inlined, Delivery::folded})
        {
            for (const auto routes : {&fourRoutes, &sixteenRoutes})
            {
                const std::string count = routes == &fourRoutes ? "4" : "16";
                Held h{sourceSettings(d, RunPolicy::usedOnly), routes, moving};
                BENCHMARK("sources: " + count + " routes, " + std::string(name(d)) +
                          (moving ? ", controllers moving" : "") + " x512")
                {
                    return h.run();
                };
            }
        }
    }
    for (const auto run : {RunPolicy::usedOnly, RunPolicy::always})
    {
        // The worst case for running every source: both mod envelopes in a
        // 10 s attack, both LFOs on the Lorenz row.
        auto s = sourceSettings(Delivery::folded, run);
        s.modEnvelopes[0] = {.attack = 10.0f, .decay = 1.0f, .sustain = 0.5f};
        s.modEnvelopes[1] = {.attack = 10.0f, .decay = 1.0f, .sustain = 0.5f};
        s.lfos[0].waveform = dsp::LfoWaveform::lorenz;
        s.lfos[1].waveform = dsp::LfoWaveform::lorenz;
        Held h{s, nullptr};
        BENCHMARK(std::string("sources: no routes, envelopes attacking, Lorenz LFOs, ") +
                  (run == RunPolicy::always ? "every source always" : "used only") + " x512")
        {
            return h.run();
        };
    }
    {
        Held h{sourceSettings(Delivery::folded, RunPolicy::always), &fourRoutes};
        BENCHMARK("sources: 4 routes, folded, every source always x512")
        {
            return h.run();
        };
    }
    for (const auto amp : {AmpEnvelopeRun::always, AmpEnvelopeRun::heardOrUsed})
    {
        auto s = sourceSettings(Delivery::folded, RunPolicy::usedOnly);
        s.ampEnvelopes = amp;
        Held h{s, nullptr, false, true};
        BENCHMARK(std::string("sources: filter B --, its amp envelope ") +
                  (amp == AmpEnvelopeRun::always ? "always run" : "run only as a source") + " x512")
        {
            return h.run();
        };
    }
}
