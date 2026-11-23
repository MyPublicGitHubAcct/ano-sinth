// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// CPU cost of the modulation sources as built in dsp::Voice (M1.5 task 3,
// stage 2; docs/dsp/modulation-sources.md, "As built"), one voice held at
// the init patch (A1 one saw), 512 samples per run at 48 kHz, stereo, as
// stage 1 measured them (source_benchmark.cpp):
//
// - dsp::Voice with no context (every caller before this stage), with an
//   empty patch, and with 0, 4 and 16 routes (stage 1's sets), the wheel and
//   pressure still or moving;
// - the stage-1 prototype in its decided configuration (folded, used only)
//   on the same routes, in the same session.
//
// Tagged [!benchmark][voice-sources]: run with
// tools/bench.sh "[voice-sources]" in relwithdebinfo.

#include "../modulation/source_candidates.hpp"
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
#include <memory>
#include <string>
#include <string_view>

namespace
{

namespace dsp = anoesis::dsp;
namespace mod = anoesis::dsp::modulation;
namespace proto = anoesis::test::sources;
using anoesis::test::Buffer;
using Dest = mod::Destination;
using mod::Source;

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;
const dsp::ProcessSpec spec{.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2};

template <typename V>
void initPatch(V& v)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        v.slot(s).setWaveform(dsp::Waveform::saw);
        v.slot(s).setVoices(s == 0 ? 1 : 0);
    }
}

/// Stage 1's settings (source_benchmark.cpp).
proto::SourceSettings stageOneSettings()
{
    proto::SourceSettings s;
    s.modEnvelopes[0] = {.attack = 0.05f, .decay = 0.3f, .sustain = 0.5f};
    s.modEnvelopes[1] = {.attack = 0.2f, .decay = 0.1f, .sustain = 0.8f};
    s.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = 5.0f, .gain = 10.0f};
    s.lfos[1] = {.waveform = dsp::LfoWaveform::triangle, .rate = 4.0f, .gain = 10.0f};
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

/// Every source: per-sample destinations and grid ones (stage 1's 16).
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

using Routes = void (*)(mod::Patch&);

/// dsp::Voice held past its attacks, with the engine's modulation state.
struct Held
{
    mod::ControllerRamp wheel;
    mod::ControllerRamp pressure;
    mod::Patch patch;
    std::array<dsp::Lfo, 2> freeLfos{dsp::Lfo{}, dsp::Lfo{}};
    mod::SourceContext context;
    std::unique_ptr<dsp::Voice<>> voice;
    Buffer out{2, blockSize};
    dsp::LinearSmoother cutoff;
    dsp::VoiceKnobs knobs;
    bool moving = false;
    int value = 0;

    /// attach false: no context (VoiceKnobs::sources null).
    Held(Routes routes, bool controllersMoving, bool attach = true) : moving(controllersMoving)
    {
        wheel.prepare(rate);
        pressure.prepare(rate);
        wheel.set(0, 0.5f);
        pressure.set(0, 0.3f);
        if (routes != nullptr)
        {
            routes(patch);
        }
        const auto s = stageOneSettings();
        for (std::size_t k = 0; k < 2; ++k)
        {
            const auto& e = s.modEnvelopes[k];
            context.modEnvelopes[k] = {.attack = e.attack, .decay = e.decay, .sustain = e.sustain};
            context.lfos[k] = {.waveform = s.lfos[k].waveform, .rate = s.lfos[k].rate, .gain = s.lfos[k].gain};
        }
        context.patch = &patch;
        context.modWheel = &wheel;
        context.aftertouch = &pressure;
        context.freeLfos = &freeLfos;
        context.stepVelocity = 0.5f;
        voice = std::make_unique<dsp::Voice<>>(anoesis::test::d6::minBlepTable64(), 7);
        initPatch(*voice);
        voice->prepare(spec);
        cutoff.setCurrentAndTarget(5.0f);
        knobs.cutoff = {&cutoff, &cutoff};
        knobs.controlCutoff = &cutoff;
        knobs.sources = attach ? &context : nullptr;
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
            value = (value + 1) % 128;
            wheel.setMidi(knobs.time, value);
            pressure.setMidi(knobs.time, 127 - value);
        }
        out.block().clear();
        voice->process(out.block(), knobs);
        knobs.time += blockSize;
        return out.channel(0)[blockSize - 1];
    }
};

/// The prototype in its decided configuration (folded, used only), as
/// stage 1's benchmark held it.
struct HeldPrototype
{
    proto::Controller wheel;
    proto::Controller pressure;
    mod::Patch patch;
    proto::SourceSettings settings = stageOneSettings();
    std::array<dsp::Lfo, 2> freeLfos{dsp::Lfo{}, dsp::Lfo{}};
    float stepVelocity = 0.5f;
    proto::SourceContext context;
    std::unique_ptr<proto::SourceVoice<>> voice;
    Buffer out{2, blockSize};
    dsp::LinearSmoother cutoff;
    dsp::VoiceKnobs knobs;
    bool moving = false;
    int value = 0;

    HeldPrototype(Routes routes, bool controllersMoving) : moving(controllersMoving)
    {
        wheel.configure(proto::Smoothing::linear, rate, 5.0f);
        pressure.configure(proto::Smoothing::linear, rate, 5.0f);
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
        voice = std::make_unique<proto::SourceVoice<>>(anoesis::test::d6::minBlepTable64(), &context, 7);
        initPatch(*voice);
        voice->prepare(spec);
        cutoff.setCurrentAndTarget(5.0f);
        knobs.cutoff = {&cutoff, &cutoff};
        knobs.controlCutoff = &cutoff;
        voice->noteOn(57.0f, knobs);
        for (int k = 0; k < 40; ++k)
        {
            run();
        }
    }

    float run()
    {
        if (moving)
        {
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

std::string_view label(Routes routes)
{
    if (routes == nullptr)
    {
        return "0 routes";
    }
    return routes == &fourRoutes ? "4 routes" : "16 routes";
}

} // namespace

TEST_CASE("Sources as built: CPU per voice, by routes, against the prototype", "[!benchmark][voice-sources]")
{
    {
        Held h{nullptr, false, false};
        BENCHMARK("voice sources: dsp::Voice, no context x512")
        {
            return h.run();
        };
    }
    for (const bool moving : {false, true})
    {
        for (const Routes routes : {Routes{nullptr}, Routes{&fourRoutes}, Routes{&sixteenRoutes}})
        {
            if (routes == nullptr && moving)
            {
                continue;
            }
            const std::string what = std::string(label(routes)) + (moving ? ", controllers moving" : "");
            {
                Held h{routes, moving};
                BENCHMARK("voice sources: dsp::Voice, " + what + " x512")
                {
                    return h.run();
                };
            }
            {
                HeldPrototype h{routes, moving};
                BENCHMARK("voice sources: prototype (folded, used only), " + what + " x512")
                {
                    return h.run();
                };
            }
        }
    }
}
