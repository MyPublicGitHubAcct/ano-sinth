// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The modulation sources as built in dsp::Voice (M1.5 task 3, stage 2;
// docs/dsp/modulation-sources.md, "As built"): two mod envelopes and two
// LFOs per voice, velocity, keytrack, the amp envelopes, the mod wheel and
// channel pressure (modulation::ControllerRamp), and Step Velocity's
// stand-in, delivered folded into the per-sample Pitch and Mix sums and as
// they stand at the grid points to Cutoff, Reso and the LFOs' Rate and Gain.
//
// - dsp::Voice is the stage-1 prototype in its decided configuration
//   (SourceVoice with SourceSettings' defaults), to the bit, at host blocks
//   of 1, 37 and 4096, with no allocation, through the real VoiceAllocator.
// - The init patch renders bit-identically to dsp::Voice as it was: against
//   the prototype with no routes, which stage 1 pinned to it.
// - Each source's value is its definition (a standalone Adsr, Lfo, Glide,
//   PitchBend, ControllerRamp).
// - Each source's bound holds against its values over random scenarios at
//   44.1, 48 and 96 kHz and host blocks of 1, 37 and 4096, and is the
//   prototype's; deliberately small bounds fail.
// - A free LFO's copy is the global instance, exactly, through a steal and
//   from a route that appears mid-note.
// - Running only what a route reads gives the output of running everything
//   wherever the prototype says it should, and differs where it says so.

#include "voice_sources_support.hpp"

#include "source_candidates.hpp"
#include "source_support.hpp"

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/modulation/controller_ramp.hpp>
#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/modulation/route_compiler.hpp>
#include <anoesis/dsp/modulation/source_bound.hpp>
#include <anoesis/dsp/modulation/source_context.hpp>
#include <anoesis/dsp/voice/glide.hpp>
#include <anoesis/dsp/voice/mix_law.hpp>
#include <anoesis/dsp/voice/pitch_bend.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace anoesis::test::voice_sources;
using anoesis::test::Buffer;
using proto::bend;
using proto::identical;
using proto::noteOff;
using proto::noteOn;
using proto::pressure;
using proto::route;
using proto::step;
using proto::wheel;

namespace
{

constexpr std::array hostBlocks{std::size_t{1}, std::size_t{37}, std::size_t{4096}};

[[nodiscard]] bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

struct Scenario
{
    std::string name;
    proto::SourceSettings settings;
    Voicing voicing;
    std::vector<SourceEvent> events;
    std::size_t length = 24000;
    float sampleRate = 48000.0f;
};

/// Free LFOs through steals, one with its rate keytracked so each voice's
/// copy runs off the global instance; an S&H; the wheel and pressure moving.
Scenario freeLfos()
{
    Scenario s{.name = "free LFOs, steals, keytracked rate"};
    s.settings.lfos[0] = {.waveform = dsp::LfoWaveform::sampleHold, .rate = 6.5f, .gain = 9.0f, .free = true};
    s.settings.lfos[1] = {
        .waveform = dsp::LfoWaveform::sine,
        .rate = 5.0f,
        .gain = 10.0f,
        .offset = -3.0f,
        .free = true,
    };
    s.voicing = Voicing{.voices = {1, 0, 1, 0}, .release = {0.05f, 0.05f}, .polyphony = 1};
    s.events = {
        route(0, 4, Source::lfo1, Dest::pitchA, 0.05f),
        route(0, 6, Source::lfo2, Dest::mixAB, -4.0f),
        route(0, 8, Source::keytrack, Dest::lfo2Rate, 5.0f),
        route(0, 9, Source::modWheel, Dest::lfo1Gain, -5.0f),
        noteOn(0, 57),
        wheel(3001, 90),
        noteOn(6007, 64),
        noteOn(12345, 52, 70),
        pressure(13000, 64),
        noteOff(19000, 52),
    };
    return s;
}

/// Routes that appear, change and go mid-note: each source started at the
/// voice's next grid point (a mod envelope with its key already up gated off
/// there), a free LFO copied there.
Scenario lateRoutes()
{
    Scenario s{.name = "routes that appear mid-note"};
    s.settings.modEnvelopes[0] = {.attack = 0.05f, .decay = 0.2f, .sustain = 0.5f, .release = 0.3f};
    s.settings.modEnvelopes[1] = {.attack = 0.01f, .decay = 0.1f, .sustain = 0.7f, .release = 0.4f};
    s.settings.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = 6.0f, .gain = 10.0f};
    s.settings.lfos[1] = {.waveform = dsp::LfoWaveform::triangle, .rate = 4.0f, .gain = 7.0f, .free = true};
    s.voicing = Voicing{.voices = {1, 0, 1, 0}, .release = {0.2f, 0.2f}, .polyphony = 2};
    s.events = {
        noteOn(0, 57),
        noteOn(1500, 61),
        route(4011, 0, Source::modEnv1, Dest::mixA, -5.0f),
        route(5003, 4, Source::lfo1, Dest::pitchA, 0.05f),
        noteOff(6000, 61),
        route(7019, 6, Source::lfo2, Dest::pitchB, 0.08f),
        route(8001, 2, Source::modEnv2, Dest::mixB, 4.0f),   // note 61's key is up
        route(11000, 4, Source::lfo1, Dest::none, 0.0f),     // gone ...
        route(14017, 4, Source::lfo1, Dest::pitchAB, 0.03f), // ... and back
        route(15000, 0, Source::modEnv1, Dest::mixA, -2.0f), // a new amount
        noteOn(16000, 64),
        noteOff(21000, 57),
        noteOff(22000, 64),
    };
    return s;
}

/// Mono Legato with portamento: legato notes leave the mod envelopes and
/// LFOs alone; the velocity ramps over the glide.
Scenario legato()
{
    Scenario s{.name = "Mono Legato, glides"};
    s.settings.modEnvelopes[0] = {.attack = 0.05f, .decay = 0.2f, .sustain = 0.5f, .release = 0.3f};
    s.settings.lfos[0] = {.waveform = dsp::LfoWaveform::triangle, .rate = 5.5f, .gain = 10.0f};
    s.voicing = Voicing{.release = {0.1f, 0.1f}, .polyphony = 1, .portamento = 0.08f, .monoLegato = true};
    s.events = {
        route(0, 0, Source::modEnv1, Dest::pitchA, 0.05f),
        route(0, 4, Source::lfo1, Dest::mixA, -3.0f),
        route(0, 8, Source::velocity, Dest::mixA, 4.0f),
        route(0, 9, Source::keytrack, Dest::cutoffA, 4.0f),
        noteOn(0, 57, 120),
        noteOn(4001, 64, 20),
        noteOff(8000, 57),
        bend(9000, 0),
        noteOn(12007, 60, 90),
        noteOff(14000, 64),
        noteOff(18000, 60),
    };
    return s;
}

/// The filters' and the Filter Control's terms at the grid points, LFO Rate
/// and Gain from other sources, and task 4's destinations (Volume, Pan,
/// Phase), whose routes compile and are ignored.
Scenario gridTargets()
{
    Scenario s{.name = "Cutoff, Reso, LFO Rate and Gain; Volume, Pan and Phase ignored"};
    s.settings.modEnvelopes[0] = {.attack = 0.01f, .decay = 0.15f, .sustain = 0.3f, .release = 0.1f};
    s.settings.modEnvelopes[1] = {.attack = 0.2f, .decay = 0.1f, .sustain = 0.9f, .release = 0.1f};
    s.settings.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = 7.0f, .gain = 6.0f};
    s.settings.lfos[1] = {.waveform = dsp::LfoWaveform::saw, .rate = 3.0f, .gain = 5.0f, .offset = 4.0f};
    s.voicing = Voicing{
        .voices = {1, 0, 1, 0},
        .inputs = {dsp::FilterInput::both, dsp::FilterInput::own},
        .resonance = 4.0f,
        .release = {0.05f, 0.05f},
        .polyphony = 3,
    };
    s.events = {
        route(0, 0, Source::modEnv1, Dest::cutoffA, 4.0f),
        route(0, 1, Source::modEnv1, Dest::resonanceAB, 3.0f),
        route(0, 2, Source::modEnv2, Dest::cutoffAB, -2.0f),
        route(0, 3, Source::modEnv2, Dest::lfo1Rate, 3.0f),
        route(0, 4, Source::lfo1, Dest::resonanceB, 2.0f),
        route(0, 5, Source::lfo1, Dest::lfo1Gain, 4.0f), // its own gain
        route(0, 6, Source::lfo2, Dest::cutoffB, 3.0f),
        route(0, 7, Source::lfo2, Dest::lfo1Rate, -2.0f),
        route(0, 8, Source::velocity, Dest::volumeA, 5.0f),
        route(0, 9, Source::keytrack, Dest::panAB, 3.0f),
        route(0, 10, Source::aftertouch, Dest::phaseB, 2.0f),
        noteOn(0, 45, 90),
        noteOn(2003, 52, 40),
        pressure(3000, 100),
        noteOn(7011, 57, 127),
        noteOff(12000, 45),
        noteOff(16000, 52),
        noteOff(20000, 57),
    };
    return s;
}

std::vector<Scenario> scenarios()
{
    std::vector<Scenario> all;
    all.push_back({
        .name = "busy patch",
        .settings = busySettings(),
        .voicing = busyVoicing,
        .events = busyEvents(),
        .length = 26000,
    });
    all.push_back(freeLfos());
    all.push_back(lateRoutes());
    all.push_back(legato());
    all.push_back(gridTargets());
    dsp::Random random{0xC0FFEE};
    for (const std::size_t rate : {std::size_t{44100}, std::size_t{96000}})
    {
        all.push_back({
            .name = "random at " + std::to_string(rate) + " Hz",
            .settings = busySettings(),
            .voicing = busyVoicing,
            .events = randomScenario(random, rate / 3, rate),
            .length = rate / 3,
            .sampleRate = static_cast<float>(rate),
        });
    }
    return all;
}

} // namespace

TEST_CASE("Sources: dsp::Voice is the stage-1 prototype as decided, to the bit at host blocks 1, 37 and 4096, "
          "with no allocation",
          "[modulation]")
{
    // Every voice primed (one note with its LFOs running), and a non-free
    // LFO's S&H on TriSaw (comparable()): see voice_sources_support.hpp.
    for (const auto& sc : scenarios())
    {
        const auto settings = comparable(sc.settings);
        const auto prototype = renderPrototype(decided(settings), sc.voicing, sc.events, sc.length,
                                               {.blockSize = 4096, .sampleRate = sc.sampleRate});
        CHECK(energy(prototype.out) > 1.0);
        for (const std::size_t block : hostBlocks)
        {
            INFO(sc.name << ", host blocks of " << block);
            std::size_t allocations = 0;
            const auto library =
                renderLibrary(settings, sc.voicing, sc.events, sc.length,
                              {.blockSize = block, .sampleRate = sc.sampleRate, .allocations = &allocations});
            CHECK(identical(library, prototype.out));
            CHECK(allocations == 0);
        }
    }
    // Unprimed, a voice's first note differs: the prototype restarts its LFOs
    // with the waveform they last had (a new voice's is the sine) and sets the
    // patch's (a triangle and an S&H here) only after; the library sets it
    // first (the flaw found in stage 2, fixed in the library).
    const auto unprimed = comparable(busySettings());
    const auto prototype = renderPrototype(decided(unprimed), busyVoicing, busyEvents(), 26000, {.prime = false});
    CHECK_FALSE(identical(renderLibrary(unprimed, busyVoicing, busyEvents(), 26000, {.prime = false}), prototype.out));
}

TEST_CASE("Sources: the init patch renders bit-identically to dsp::Voice as it was", "[modulation]")
{
    // The prototype with no routes is dsp::Voice before this stage (stage 1
    // pinned it, [sources-model]). The library voice with no context at all
    // (every caller before this stage), with a context whose patch is empty,
    // and with routes that drop out (None, an amount of 0): the same.
    std::vector<SourceEvent> const events{
        noteOn(0, 57),      bend(2500, 12000), noteOn(5001, 61),    noteOn(9013, 64),   noteOff(15000, 57),
        noteOff(17000, 61), wheel(17500, 100), pressure(18000, 64), noteOff(19000, 64), step(19500, 0.7f),
    };
    std::vector<SourceEvent> dropped = events;
    dropped.push_back(route(0, 0, Source::modEnv1, Dest::none, 3.0f));
    dropped.push_back(route(0, 1, Source::none, Dest::pitchA, 3.0f));
    dropped.push_back(route(0, 8, Source::velocity, Dest::mixA, 0.0f));
    const std::array voicings{
        Voicing{.release = {0.05f, 0.05f}, .polyphony = 2},
        Voicing{.voices = {1, 0, 1, 0}, .release = {0.05f, 0.05f}, .polyphony = 2},
        Voicing{.voices = {8, 8, 8, 8}, .resonance = 6.0f, .release = {0.05f, 0.05f}, .polyphony = 2},
        Voicing{
            .voices = {1, 0, 1, 0},
            .inputs = {dsp::FilterInput::both, dsp::FilterInput::none},
            .release = {0.05f, 0.05f},
            .polyphony = 2,
        },
        Voicing{.release = {0.1f, 0.1f}, .polyphony = 1, .portamento = 0.05f, .monoLegato = true},
    };
    proto::SourceSettings settings;
    settings.lfos[0] = {.waveform = dsp::LfoWaveform::sampleHold, .free = true};
    settings.modEnvelopes[0] = {.attack = 0.01f, .sustain = 0.5f};
    const std::size_t length = 22000;
    for (std::size_t v = 0; v < voicings.size(); ++v)
    {
        const auto asItWas = renderPrototype(decided(settings), voicings[v], events, length, {.prime = false});
        CHECK(energy(asItWas.out) > 1.0);
        for (const std::size_t block : hostBlocks)
        {
            INFO("voicing " << v << ", host blocks of " << block);
            std::size_t allocations = 0;
            CHECK(identical(
                renderLibrary(settings, voicings[v], events, length,
                              {.blockSize = block, .allocations = &allocations, .attach = false, .prime = false}),
                asItWas.out));
            CHECK(identical(renderLibrary(settings, voicings[v], events, length,
                                          {.blockSize = block, .allocations = &allocations, .prime = false}),
                            asItWas.out));
            CHECK(identical(renderLibrary(settings, voicings[v], dropped, length,
                                          {.blockSize = block, .allocations = &allocations, .prime = false}),
                            asItWas.out));
            CHECK(allocations == 0);
        }
    }
}

namespace
{

/// One voice driven directly (no allocator), observed: each source's value
/// at each absolute sample.
class Recorder final : public mod::SourceObserver
{
public:
    explicit Recorder(std::size_t length) : values(sourceCount, std::vector<float>(length, -99.0f)) {}

    void observe(const mod::SourceObservation& chunk) noexcept override
    {
        for (std::size_t s = 0; s < sourceCount; ++s)
        {
            for (std::size_t i = 0; i < chunk.length; ++i)
            {
                const auto t = static_cast<std::size_t>(chunk.time) + i;
                if (t < values[s].size())
                {
                    values[s][t] = chunk.values[s][i];
                }
            }
        }
        observed += chunk.length;
    }

    std::vector<std::vector<float>> values;
    std::size_t observed = 0;
};

/// A single voice with its own knobs, bend, controllers and context; events
/// applied between process() calls at their samples.
struct SingleVoice
{
    dsp::Voice<> voice{proto::table(), 7};
    dsp::PitchBend pitchBend;
    mod::ControllerRamp modWheel;
    mod::ControllerRamp aftertouch;
    std::array<dsp::Lfo, 2> freeLfos{dsp::Lfo{}, dsp::Lfo{}};
    mod::Patch patch;
    mod::SourceContext context;
    dsp::LinearSmoother cutoff;
    dsp::LinearSmoother mixA;
    dsp::VoiceKnobs knobs;
    Buffer out;
    std::size_t time = 0;
    std::size_t block = 37;

    explicit SingleVoice(std::size_t length, float sampleRate = 48000.0f) : out(2, length)
    {
        // The init patch: A1 a saw, the other three oscillators off.
        voice.slot(0).setWaveform(dsp::Waveform::saw);
        for (std::size_t s = 1; s < 4; ++s)
        {
            voice.slot(s).setVoices(0);
        }
        voice.drift().setDepthCents(0.0f);
        const dsp::ProcessSpec spec{.sampleRate = sampleRate, .maxBlockSize = 4096, .numChannels = 2};
        voice.prepare(spec);
        pitchBend.prepare(sampleRate);
        pitchBend.setRange(12.0f);
        modWheel.prepare(sampleRate);
        aftertouch.prepare(sampleRate);
        for (auto& l : freeLfos)
        {
            l.prepare(spec);
        }
        context.patch = &patch;
        context.modWheel = &modWheel;
        context.aftertouch = &aftertouch;
        context.freeLfos = &freeLfos;
        cutoff.setCurrentAndTarget(5.0f);
        knobs.cutoff = {&cutoff, &cutoff};
        knobs.controlCutoff = &cutoff;
        knobs.bend = &pitchBend;
        knobs.sources = &context;
    }

    [[nodiscard]] dsp::VoiceKnobs now() const noexcept
    {
        dsp::VoiceKnobs k = knobs;
        k.time = time;
        return k;
    }

    /// Renders up to the absolute sample `to` in blocks of `block`.
    void runTo(std::size_t to)
    {
        while (time < to)
        {
            const auto n = std::min(block, to - time);
            voice.process(out.block().subBlock(time, n), now());
            time += n;
        }
    }
};

} // namespace

namespace
{

/// Every source's value at every sample against a standalone reference,
/// with LFO 1 and LFO 2 on `waves`.
void checkDefinitions(std::array<dsp::LfoWaveform, 2> waves)
{
    const std::size_t length = 20000;
    SingleVoice v{length};
    Recorder recorder{length};
    v.voice.setSourceObserver(&recorder);
    v.voice.envelope(0).setAttack(0.01f);
    v.voice.envelope(0).setRelease(0.05f);
    v.voice.envelope(1).setAttack(0.03f);
    v.voice.envelope(1).setSustain(0.4f);
    v.voice.envelope(1).setRelease(0.02f);
    v.context.modEnvelopes[0] = {.attack = 0.02f, .decay = 0.1f, .sustain = 0.6f, .release = 0.04f};
    v.context.modEnvelopes[1] = {.attack = 0.001f, .decay = 0.05f, .sustain = 0.0f, .release = 0.01f};
    v.context.lfos[0] = {.waveform = waves[0], .rate = 6.0f, .gain = 7.0f, .offset = 2.0f};
    v.context.lfos[1] = {.waveform = waves[1], .rate = 4.0f, .gain = 10.0f};
    // Each envelope and LFO read by a route (into the grid's Cutoff, which
    // moves none of them).
    v.patch.set(0, {.source = Source::modEnv1, .destination = Dest::cutoffA, .amount = 1.0f});
    v.patch.set(2, {.source = Source::modEnv2, .destination = Dest::cutoffA, .amount = 1.0f});
    v.patch.set(4, {.source = Source::lfo1, .destination = Dest::cutoffA, .amount = 1.0f});
    v.patch.set(6, {.source = Source::lfo2, .destination = Dest::cutoffA, .amount = 1.0f});

    // The references, gated and set at the same samples.
    dsp::Adsr ampA;
    dsp::Adsr ampB;
    std::array<dsp::Adsr, 2> modEnv{};
    std::array<dsp::Lfo, 2> lfo{dsp::Lfo{}, dsp::Lfo{}};
    const dsp::ProcessSpec spec{.sampleRate = 48000.0f, .maxBlockSize = 4096, .numChannels = 1};
    for (auto* e : {&ampA, &ampB, modEnv.data(), &modEnv[1]})
    {
        e->prepare(spec);
    }
    ampA.setAttack(0.01f);
    ampA.setRelease(0.05f);
    ampB.setAttack(0.03f);
    ampB.setSustain(0.4f);
    ampB.setRelease(0.02f);
    for (std::size_t k = 0; k < 2; ++k)
    {
        const auto& m = v.context.modEnvelopes[k];
        modEnv[k].setAttack(m.attack);
        modEnv[k].setDecay(m.decay);
        modEnv[k].setSustain(m.sustain);
        modEnv[k].setRelease(m.release);
        lfo[k].setSeed(7U ^ (0xA54FF53A5F1D36F1U * (k + 1))); // the voice's (seed 7)
        lfo[k].prepare(spec);
        lfo[k].setWaveform(v.context.lfos[k].waveform);
        lfo[k].setFrequency(mod::lfoRateHz(v.context.lfos[k].rate));
        lfo[k].restart();
    }
    dsp::Glide pitch;
    dsp::Glide velocity;
    pitch.prepare(48000.0);
    velocity.prepare(48000.0);
    dsp::PitchBend bendRef;
    bendRef.prepare(48000.0);
    bendRef.setRange(12.0f);
    mod::ControllerRamp wheelRef;
    mod::ControllerRamp pressureRef;
    wheelRef.prepare(48000.0);
    pressureRef.prepare(48000.0);
    std::vector<float> step(length, 0.0f);

    // Note-on at 0 (velocity 0.8); a legato note at 5000 gliding 0.1 s to
    // E4 at velocity 0.2; the bend, the wheel, pressure and Step Velocity
    // moving; note-off at 15000.
    v.voice.noteOn(57.0f, v.now(), {.velocity = 0.8f});
    pitch.jump(57.0f);
    velocity.jump(0.8f);
    for (auto* e : {&ampA, &ampB, modEnv.data(), &modEnv[1]})
    {
        e->gateOn();
    }
    struct Event
    {
        std::size_t time;
        int kind;
    };
    const std::array timeline{
        Event{.time = 1003, .kind = 0},  Event{.time = 2011, .kind = 1}, Event{.time = 3001, .kind = 2},
        Event{.time = 5000, .kind = 3},  Event{.time = 7013, .kind = 4}, Event{.time = 9000, .kind = 5},
        Event{.time = 15000, .kind = 6},
    };
    std::size_t t = 0;
    float stepNow = 0.0f;
    std::array<std::size_t, sourceCount> wrong{};
    const auto expect = [&](Source s, float expected)
    {
        wrong[at(s)] += sameBits(recorder.values[at(s)][t], expected) ? 0U : 1U;
    };
    const auto advanceRefs = [&](std::size_t to)
    {
        for (; t < to; ++t)
        {
            const auto when = static_cast<std::uint64_t>(t);
            const float semitones = pitch.at(0) + bendRef.semitonesAt(when);
            expect(Source::velocity, velocity.at(0));
            expect(Source::keytrack, dsp::keytrackSource(semitones));
            expect(Source::ampEnvA, ampA.next());
            expect(Source::ampEnvB, ampB.next());
            expect(Source::modEnv1, modEnv[0].next());
            expect(Source::modEnv2, modEnv[1].next());
            for (std::size_t k = 0; k < 2; ++k)
            {
                const auto& ls = v.context.lfos[k];
                const float w = lfo[k].next();
                expect(k == 0 ? Source::lfo1 : Source::lfo2,
                       mod::lfoSourceValue(w, ls.gain / 10.0f, ls.offset / 10.0f));
            }
            expect(Source::modWheel, wheelRef.valueAt(when));
            expect(Source::aftertouch, pressureRef.valueAt(when));
            expect(Source::stepVelocity, step[t]);
            pitch.advance(1);
            velocity.advance(1);
        }
    };
    for (const auto& e : timeline)
    {
        v.runTo(e.time);
        std::fill(step.begin() + static_cast<std::ptrdiff_t>(t), step.begin() + static_cast<std::ptrdiff_t>(e.time),
                  stepNow);
        advanceRefs(e.time);
        const auto when = static_cast<std::uint64_t>(e.time);
        switch (e.kind)
        {
        case 0:
            v.pitchBend.setValue(when, 16383);
            bendRef.setValue(when, 16383);
            break;
        case 1:
            v.modWheel.setMidi(when, 90);
            wheelRef.setMidi(when, 90);
            break;
        case 2:
            v.aftertouch.setMidi(when, 33);
            pressureRef.setMidi(when, 33);
            break;
        case 3:
        {
            const dsp::NoteStart start{
                .velocity = 0.2f,
                .glide = true,
                .from = pitch.at(0),
                .glideSeconds = 0.1f,
                .fromVelocity = 0.8f,
            };
            v.voice.legatoNoteOn(64.0f, v.now(), start);
            pitch.start(start.from, 64.0f, start.glideSeconds);
            velocity.startSamples(velocity.at(0), 0.2f, pitch.lengthSamples());
            for (auto* env : {&ampA, &ampB, modEnv.data(), &modEnv[1]})
            {
                env->legatoGateOn();
            }
            break;
        }
        case 4:
            v.context.stepVelocity = 0.75f;
            stepNow = 0.75f;
            break;
        case 5:
            v.pitchBend.setValue(when, 0);
            bendRef.setValue(when, 0);
            break;
        default:
            v.voice.noteOff();
            for (auto* env : {&ampA, &ampB, modEnv.data(), &modEnv[1]})
            {
                env->gateOff();
            }
            break;
        }
    }
    v.runTo(length);
    std::fill(step.begin() + static_cast<std::ptrdiff_t>(t), step.end(), stepNow);
    advanceRefs(length);
    CHECK(recorder.observed == length);
    for (std::size_t s = 1; s < sourceCount; ++s)
    {
        INFO("source " << s);
        CHECK(wrong[s] == 0);
    }
}

} // namespace

TEST_CASE("Sources: each source's value is its definition", "[modulation]")
{
    // A voice's first note starts its LFOs at their own waveforms' starts (the
    // prototype started a triangle from the sine's: fixed in the library).
    for (const auto& waves : {
             std::array{dsp::LfoWaveform::sine, dsp::LfoWaveform::triangle},
             std::array{dsp::LfoWaveform::sampleHold, dsp::LfoWaveform::saw},
         })
    {
        INFO("LFOs " << static_cast<int>(waves[0]) << " and " << static_cast<int>(waves[1]));
        checkDefinitions(waves);
    }
}

TEST_CASE("Sources: a source no route reads doesn't run and reads 0; its gates are still tracked", "[modulation]")
{
    const std::size_t length = 6000;
    SingleVoice v{length};
    Recorder recorder{length};
    v.voice.setSourceObserver(&recorder);
    v.voice.envelope(0).setRelease(1.0f);
    v.context.modEnvelopes[0] = {.attack = 0.01f, .sustain = 0.5f, .release = 0.02f};
    v.patch.set(8, {.source = Source::velocity, .destination = Dest::mixA, .amount = 1.0f});
    v.voice.noteOn(57.0f, v.now(), {.velocity = 0.5f});
    v.runTo(2000);
    v.voice.noteOff(); // the key goes up while Mod Env 1 is dormant
    v.runTo(3000);
    // Mod Env 1 appears at 3000: it starts at the voice's next grid point
    // (3008: the grid counts from the note-on at 0), gated on and, the key
    // being up, off at once, so it releases from its first level.
    v.patch.set(0, {.source = Source::modEnv1, .destination = Dest::pitchA, .amount = 0.1f});
    v.runTo(length);
    REQUIRE(recorder.observed == length);
    for (std::size_t t = 0; t < 3008; ++t)
    {
        CHECK(sameBits(recorder.values[at(Source::modEnv1)][t], 0.0f));
        CHECK(sameBits(recorder.values[at(Source::lfo1)][t], 0.0f));
    }
    dsp::Adsr reference;
    reference.prepare({.sampleRate = 48000.0f, .maxBlockSize = 4096, .numChannels = 1});
    reference.setAttack(0.01f);
    reference.setSustain(0.5f);
    reference.setRelease(0.02f);
    reference.gateOn();
    reference.gateOff();
    for (std::size_t t = 3008; t < length; ++t)
    {
        CHECK(sameBits(recorder.values[at(Source::modEnv1)][t], reference.next()));
    }
    CHECK(sameBits(recorder.values[at(Source::lfo1)][length - 1], 0.0f));
}

TEST_CASE("Sources: a mod envelope's settings reach a sounding voice at its next grid point", "[modulation]")
{
    const std::size_t length = 8000;
    SingleVoice v{length};
    Recorder recorder{length};
    v.voice.setSourceObserver(&recorder);
    v.context.modEnvelopes[0] = {.attack = 0.005f, .decay = 0.02f, .sustain = 0.8f, .release = 0.05f};
    v.patch.set(0, {.source = Source::modEnv1, .destination = Dest::pitchA, .amount = 0.1f});
    v.voice.noteOn(57.0f, v.now());
    v.runTo(3000);
    v.context.modEnvelopes[0].sustain = 0.3f; // at 3000, mid-segment
    v.runTo(length);
    dsp::Adsr reference;
    reference.prepare({.sampleRate = 48000.0f, .maxBlockSize = 4096, .numChannels = 1});
    reference.setAttack(0.005f);
    reference.setDecay(0.02f);
    reference.setSustain(0.8f);
    reference.setRelease(0.05f);
    reference.gateOn();
    for (std::size_t t = 0; t < length; ++t)
    {
        if (t == 3008) // the voice's next grid point
        {
            reference.setSustain(0.3f);
        }
        CHECK(sameBits(recorder.values[at(Source::modEnv1)][t], reference.next()));
    }
}

TEST_CASE("Sources: a mod envelope doesn't keep its voice alive", "[modulation]")
{
    const std::size_t length = 6000;
    SingleVoice v{length};
    Recorder recorder{length};
    v.voice.setSourceObserver(&recorder);
    v.voice.envelope(0).setRelease(0.01f);
    v.context.modEnvelopes[0] = {.attack = 0.001f, .sustain = 1.0f, .release = 5.0f};
    v.patch.set(0, {.source = Source::modEnv1, .destination = Dest::mixA, .amount = 1.0f});
    v.voice.noteOn(57.0f, v.now());
    v.runTo(1000);
    v.voice.noteOff();
    v.runTo(1000 + 480 + 32);
    // The amp envelope has released to 0 (480 samples); Mod Env 1, still
    // releasing over 5 s, doesn't hold the voice.
    CHECK_FALSE(v.voice.isActive());
    CHECK(recorder.values[at(Source::modEnv1)][1000 + 480 + 31] > 0.9f);
    CHECK(sameBits(recorder.values[at(Source::ampEnvA)][1000 + 480 + 31], 0.0f));
}

TEST_CASE("Sources: Mix's modulation lifts a part above Mix 0 and the skip test hears it", "[modulation]")
{
    for (const float amount : {5.0f, -5.0f, 0.0f})
    {
        INFO("Velocity on Mix A at " << amount);
        const std::size_t length = 4000;
        SingleVoice v{length};
        v.mixA.setCurrentAndTarget(0.0f); // Mix A at 0: silent without modulation
        v.knobs.mix = {&v.mixA, nullptr};
        v.patch.set(8, {.source = Source::velocity, .destination = Dest::mixA, .amount = amount});
        v.voice.noteOn(57.0f, v.now(), {.velocity = 1.0f});
        v.runTo(length);
        double e = 0.0;
        for (const float x : v.out.channel(0))
        {
            e += static_cast<double>(x) * static_cast<double>(x);
        }
        if (amount > 0.0f)
        {
            CHECK(e > 1.0);
            // At internal 5 the gain is half unity (mix_law.hpp): the
            // voice's output against the same voice at Mix 5 with no route.
            SingleVoice plain{length};
            plain.mixA.setCurrentAndTarget(5.0f);
            plain.knobs.mix = {&plain.mixA, nullptr};
            plain.voice.noteOn(57.0f, plain.now(), {.velocity = 1.0f});
            plain.runTo(length);
            CHECK(identical(v.out, plain.out));
        }
        else
        {
            CHECK(e <= 0.0);
        }
    }
}

TEST_CASE("Sources: each bound holds against its values over random scenarios at 44.1, 48 and 96 kHz and host "
          "blocks 1, 37 and 4096; it is the prototype's, and small bounds fail",
          "[modulation]")
{
    // Each bound against its values, the S&H included (the library alone);
    // and, primed with S&H on TriSaw, the same bounds as the prototype's
    // (each sample's width, summed): every source but the LFOs, whose
    // widths include the priming phase, where the prototype's flaw shows.
    BoundTotals library;
    BoundTotals compared;
    proto::BoundStats prototype;
    dsp::Random random{0x5EED5};
    for (const std::size_t rate : {std::size_t{44100}, std::size_t{48000}, std::size_t{96000}})
    {
        for (const std::size_t block : hostBlocks)
        {
            const std::size_t length = rate / 2;
            const auto events = randomScenario(random, length, rate);
            const auto sampleRate = static_cast<float>(rate);
            renderLibrary(busySettings(), busyVoicing, events, length,
                          {.blockSize = block, .sampleRate = sampleRate, .bounds = &library, .prime = false});
            const auto settings = comparable(busySettings());
            const auto withLibrary = renderLibrary(settings, busyVoicing, events, length,
                                                   {.blockSize = block, .sampleRate = sampleRate, .bounds = &compared});
            auto checked = decided(settings);
            checked.checkBounds = true;
            const auto p =
                renderPrototype(checked, busyVoicing, events, length, {.blockSize = block, .sampleRate = sampleRate});
            CHECK(identical(withLibrary, p.out));
            proto::addStats(prototype, p.stats);
        }
    }
    std::uint64_t checked = 0;
    for (std::size_t s = 1; s < sourceCount; ++s)
    {
        INFO("source " << s);
        checked += library.checked[s];
        CHECK(library.outside[s] == 0);
        CHECK(compared.outside[s] == 0);
        CHECK(compared.checked[s] == prototype.checked[s]);
        const bool lfo = static_cast<Source>(s) == Source::lfo1 || static_cast<Source>(s) == Source::lfo2;
        if (!lfo)
        {
            CHECK_THAT(compared.width[s], Catch::Matchers::WithinULP(prototype.width[s], 0));
        }
        // Step Velocity's stand-in is constant between its events: exact
        // bounds of width 0 (stage 1's measurement too).
        CHECK((library.width[s] > 0.0) == (static_cast<Source>(s) != Source::stepVelocity));
    }
    CHECK(checked > 3'000'000);
    const auto ownEnds = static_cast<std::size_t>(SmallRule::ownEnds);
    const auto levelOnly = static_cast<std::size_t>(SmallRule::levelOnly);
    CHECK(library.small[ownEnds][at(Source::keytrack)] > 0);
    CHECK(library.small[ownEnds][at(Source::lfo1)] > 0);
    CHECK(library.small[ownEnds][at(Source::lfo2)] > 0);
    CHECK(library.small[levelOnly][at(Source::modEnv1)] > 1000);
    CHECK(library.small[levelOnly][at(Source::velocity)] > 0);
    CHECK(library.small[levelOnly][at(Source::modWheel)] > 0);
}

namespace
{

/// The global instance's values, as the engine moves it: 0.5 x gain x
/// (wave + offset), per absolute sample.
std::vector<float> globalLfoValues(const proto::SourceSettings& settings, std::size_t k, std::size_t length,
                                   std::uint64_t seed)
{
    dsp::Lfo l;
    l.setSeed(seed ^ (0xBB67AE8584CAA73BU * (k + 1)));
    l.prepare({.sampleRate = 48000.0f, .maxBlockSize = 4096, .numChannels = 1});
    const auto& ls = settings.lfos[k];
    l.setWaveform(ls.waveform);
    l.setFrequency(mod::lfoRateHz(ls.rate));
    l.restart();
    std::vector<float> v(length);
    for (auto& x : v)
    {
        x = mod::lfoSourceValue(l.next(), ls.gain / 10.0f, ls.offset / 10.0f);
    }
    return v;
}

/// Voice 0's LFO 1 values through LibraryEngine, per absolute sample (-99
/// where it wasn't rendered).
std::vector<float> voiceLfoValues(const proto::SourceSettings& settings, const Voicing& voicing,
                                  std::vector<SourceEvent> events, std::size_t length, std::size_t block)
{
    std::ranges::stable_sort(events, {}, &SourceEvent::offset);
    LibraryEngine engine{proto::table(), settings, 3};
    engine.voices().forEachVoice([&](auto& v) { proto::configure(v, voicing); });
    Recorder recorder{length};
    engine.voices().voice(0).setSourceObserver(&recorder);
    engine.prepare({.sampleRate = 48000.0f, .maxBlockSize = 4096, .numChannels = 2});
    proto::configureAllocator(engine.voices(), voicing);
    Buffer out{2, length};
    std::size_t e = 0;
    std::vector<SourceEvent> here;
    for (std::size_t start = 0; start < length; start += block)
    {
        const auto n = std::min(block, length - start);
        here.clear();
        while (e < events.size() && events[e].offset < start + n)
        {
            auto ev = events[e];
            ev.offset -= start;
            here.push_back(ev);
            ++e;
        }
        engine.process(out.block().subBlock(start, n), here);
    }
    return recorder.values[at(Source::lfo1)];
}

} // namespace

TEST_CASE("Sources: a free LFO's copy is the global instance, exactly, through a steal and from a late route",
          "[modulation]")
{
    const std::size_t length = 24000;
    proto::SourceSettings settings;
    settings.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = 6.3f, .gain = 9.0f, .offset = 1.0f, .free = true};
    const Voicing voicing{.release = {0.1f, 0.1f}, .polyphony = 1};
    const auto global = globalLfoValues(settings, 0, length, 3);
    for (const std::size_t block : hostBlocks)
    {
        INFO("host blocks of " << block);
        // Routed from the start; a steal at 12345 keeps the copy.
        {
            const auto values = voiceLfoValues(
                settings, voicing,
                {route(0, 4, Source::lfo1, Dest::pitchA, 0.05f), noteOn(0, 57), noteOn(12345, 60), noteOff(20000, 60)},
                length, block);
            std::size_t matched = 0;
            for (std::size_t t = 0; t < 20000; ++t)
            {
                matched += sameBits(values[t], global[t]) ? 1U : 0U;
            }
            CHECK(matched == 20000);
        }
        // Routed mid-note at 7001: 0 until the voice's next grid point
        // (7008), then the global instance there.
        {
            const auto values = voiceLfoValues(
                settings, voicing,
                {noteOn(0, 57), route(7001, 4, Source::lfo1, Dest::pitchA, 0.05f), noteOff(20000, 57)}, length, block);
            std::size_t zeros = 0;
            std::size_t matched = 0;
            for (std::size_t t = 0; t < 7008; ++t)
            {
                zeros += sameBits(values[t], 0.0f) ? 1U : 0U;
            }
            for (std::size_t t = 7008; t < 20000; ++t)
            {
                matched += sameBits(values[t], global[t]) ? 1U : 0U;
            }
            CHECK(zeros == 7008);
            CHECK(matched == 20000 - 7008);
        }
    }
}

TEST_CASE("Sources: running only what a route reads is running everything, wherever the prototype says it is",
          "[modulation]")
{
    // The prototype's measurement (modulation-sources.md, question 2): the
    // same on every patch whose routes are there from the note-on, and for a
    // free LFO's late route; different for a mod envelope's or a non-free
    // LFO's late route, which starts from its note-on state.
    struct Case
    {
        std::string name;
        Scenario scenario;
        bool same;
    };
    std::vector<Case> cases;
    cases.push_back({
        .name = "busy patch",
        .scenario = {.settings = busySettings(), .voicing = busyVoicing, .events = busyEvents(), .length = 26000},
        .same = true,
    });
    cases.push_back({.name = "free LFOs", .scenario = freeLfos(), .same = true});
    const Voicing held{.release = {0.1f, 0.1f}, .polyphony = 1};
    for (const auto source : {Source::modEnv1, Source::lfo1})
    {
        for (const bool free : {false, true})
        {
            if (source == Source::modEnv1 && free)
            {
                continue;
            }
            Scenario s{.voicing = held, .length = 30000};
            s.settings = busySettings();
            s.settings.modEnvelopes[0] = {.attack = 0.05f, .decay = 0.2f, .sustain = 0.5f, .release = 0.3f};
            s.settings.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = 6.0f, .free = free};
            s.events = {
                noteOn(0, 57),
                noteOn(6000, 59),
                route(12345, 0, source, Dest::pitchA, 0.1f),
                noteOff(20000, 59),
            };
            cases.push_back({
                .name = std::string(source == Source::modEnv1 ? "Mod Env 1" : "LFO 1") + (free ? " (free)" : "") +
                        ", routed mid-note",
                .scenario = s,
                .same = free,
            });
        }
    }
    for (const auto& c : cases)
    {
        const auto settings = comparable(c.scenario.settings);
        auto always = settings;
        always.run = proto::RunPolicy::always;
        const auto everything =
            renderPrototype(always, c.scenario.voicing, c.scenario.events, c.scenario.length, {.blockSize = 4096});
        for (const std::size_t block : hostBlocks)
        {
            INFO(c.name << ", host blocks of " << block);
            const auto library =
                renderLibrary(settings, c.scenario.voicing, c.scenario.events, c.scenario.length, {.blockSize = block});
            CHECK(identical(library, everything.out) == c.same);
        }
    }
}
