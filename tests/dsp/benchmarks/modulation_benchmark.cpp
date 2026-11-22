// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// CPU cost of the modulation candidates (M1.5 task 2, stage 1;
// tests/dsp/modulation/modulation_candidates.hpp, docs/dsp/modulation.md),
// 512 samples per run at 48 kHz, stereo:
//
// - one voice held at the init patch (A1 one saw): dsp::Voice against
//   ModVoice with no routes;
// - per destination (Pitch A, Phase A, Volume A, Pan A, LFO 1 Rate, LFO 1
//   Gain) and per rate (per sample, grid held, grid ramped): 4 and 16 routes
//   into it, from every kind of source (LFOs, mod envelopes, velocity,
//   keytrack, the mod wheel); for the LFO targets, LFO 1 also on Pitch A, and
//   that one route alone is their 0;
// - a mixed patch of 16 routes at the recommended rates, one voice and 16;
// - the registries: compiling and accumulating 16 routes, each candidate.
//
// Tagged [!benchmark][modulation]: run with tools/bench.sh "[modulation]"
// in relwithdebinfo.

#include "../modulation/modulation_candidates.hpp"
#include "../modulation/registry_candidates.hpp"
#include "../oscillators/oscillator_support.hpp"

#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace anoesis::test::modulation;
using anoesis::test::Buffer;

namespace
{

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

/// The 16 slots' sources, in slot order (two per owner, then the misc).
constexpr std::array<SourceId, 16> slotSources{
    SourceId::modEnv1,  SourceId::modEnv1,  SourceId::modEnv2,  SourceId::modEnv2,
    SourceId::lfo1,     SourceId::lfo1,     SourceId::lfo2,     SourceId::lfo2,
    SourceId::velocity, SourceId::keytrack, SourceId::modWheel, SourceId::aftertouch,
    SourceId::velocity, SourceId::keytrack, SourceId::modWheel, SourceId::aftertouch,
};

/// The 4-route sets take one of each kind: an envelope, an LFO, velocity, the wheel.
constexpr std::array<std::size_t, 4> fourSlots{0, 4, 8, 10};

ModPatch destinationPatch(DestinationId d, Target t, EvalRate r, std::size_t routes)
{
    ModPatch p;
    p.modEnvelopes[0] = {.attack = 0.05f, .decay = 0.3f, .sustain = 0.5f};
    p.modEnvelopes[1] = {.attack = 0.2f, .decay = 0.1f, .sustain = 0.8f};
    p.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = 5.0f, .gain = 10.0f};
    p.lfos[1] = {.waveform = dsp::LfoWaveform::triangle, .rate = 4.0f, .gain = 10.0f};
    const bool lfoTarget = t == Target::lfoRate || t == Target::lfoGain;
    if (lfoTarget)
    {
        p.route(5, SourceId::lfo1, DestinationId::pitchA, 0.02f);
        p.rates[index(Target::lfoRate)] = r == EvalRate::perSample ? EvalRate::perSample : EvalRate::gridHeld;
        p.rates[index(Target::lfoGain)] = r == EvalRate::perSample ? EvalRate::perSample : EvalRate::gridHeld;
    }
    p.rates[index(t)] = r;
    const float amount = t == Target::pitch ? 0.01f : 0.5f;
    std::vector<std::size_t> slots;
    if (routes == 4)
    {
        slots.assign(fourSlots.begin(), fourSlots.end());
    }
    else if (routes == 16)
    {
        for (std::size_t s = 0; s < 16; ++s)
        {
            slots.push_back(s);
        }
    }
    for (const auto s : slots)
    {
        if (lfoTarget && s == 5)
        {
            continue; // LFO 1 on Pitch A stays
        }
        p.route(s, slotSources[s], d, amount);
    }
    return p;
}

/// 16 routes at the rows' recommended rates.
ModPatch mixedPatch()
{
    ModPatch p;
    p.modEnvelopes[0] = {.attack = 0.05f, .decay = 0.3f, .sustain = 0.5f};
    p.modEnvelopes[1] = {.attack = 0.2f, .decay = 0.1f, .sustain = 0.8f};
    p.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = 5.0f, .gain = 10.0f};
    p.lfos[1] = {.waveform = dsp::LfoWaveform::triangle, .rate = 4.0f, .gain = 10.0f};
    p.route(0, SourceId::modEnv1, DestinationId::cutoffA, 3.0f);
    p.route(1, SourceId::modEnv1, DestinationId::resonanceA, 1.0f);
    p.route(2, SourceId::modEnv2, DestinationId::mixA, 1.0f);
    p.route(3, SourceId::modEnv2, DestinationId::pitchA, 0.01f);
    p.route(4, SourceId::lfo1, DestinationId::pitchA, 0.02f);
    p.route(5, SourceId::lfo1, DestinationId::lfo2Rate, 1.0f);
    p.route(6, SourceId::lfo2, DestinationId::panA, 2.0f);
    p.route(7, SourceId::lfo2, DestinationId::lfo1Gain, 2.0f);
    p.route(8, SourceId::velocity, DestinationId::volumeA, -2.0f);
    p.route(9, SourceId::keytrack, DestinationId::cutoffAB, 2.0f);
    p.route(10, SourceId::modWheel, DestinationId::phaseA, 1.0f);
    p.route(11, SourceId::aftertouch, DestinationId::mixA, 1.0f);
    p.route(12, SourceId::velocity, DestinationId::cutoffB, 1.0f);
    p.route(13, SourceId::keytrack, DestinationId::panAB, 1.0f);
    p.route(14, SourceId::modWheel, DestinationId::resonanceAB, 1.0f);
    p.route(15, SourceId::lfo1, DestinationId::volumeAB, 1.0f);
    return p;
}

struct Held
{
    ControlRamp wheel;
    ControlRamp aftertouch;
    ModPatch patch;
    ModContext context;
    std::unique_ptr<ModVoice<>> voice;
    Buffer out{2, blockSize};
    dsp::LinearSmoother cutoff;
    dsp::VoiceKnobs knobs;

    explicit Held(const ModPatch& p) : patch(p)
    {
        wheel.set(0, 0.5f);
        aftertouch.set(0, 0.3f);
        context = {.patch = &patch, .modWheel = &wheel, .aftertouch = &aftertouch};
        voice = std::make_unique<ModVoice<>>(anoesis::test::d6::minBlepTable64(), &context, 7);
        initPatch(*voice);
        voice->prepare(spec);
        cutoff.setCurrentAndTarget(5.0f);
        knobs.cutoff = {&cutoff, &cutoff};
        knobs.controlCutoff = &cutoff;
        voice->noteOn(57.0f, knobs);
        // Past the attacks.
        for (int k = 0; k < 40; ++k)
        {
            out.block().clear();
            voice->process(out.block(), knobs);
            knobs.time += blockSize;
        }
    }

    float run()
    {
        out.block().clear();
        voice->process(out.block(), knobs);
        knobs.time += blockSize;
        return out.channel(0)[blockSize - 1];
    }
};

} // namespace

TEST_CASE("Modulation: CPU per voice, by destination, rate and routes", "[!benchmark][modulation]")
{
    {
        auto v = std::make_unique<dsp::Voice<>>(anoesis::test::d6::minBlepTable64(), 7);
        initPatch(*v);
        v->prepare(spec);
        dsp::LinearSmoother cutoff;
        cutoff.setCurrentAndTarget(5.0f);
        dsp::VoiceKnobs knobs;
        knobs.cutoff = {&cutoff, &cutoff};
        knobs.controlCutoff = &cutoff;
        v->noteOn(57.0f, knobs);
        Buffer out{2, blockSize};
        BENCHMARK("modulation: dsp::Voice, init x512")
        {
            out.block().clear();
            v->process(out.block(), knobs);
            return out.channel(0)[blockSize - 1];
        };
    }
    {
        Held h{ModPatch{}};
        BENCHMARK("modulation: ModVoice, no routes x512")
        {
            return h.run();
        };
    }
    struct Destination
    {
        std::string name;
        DestinationId id;
        Target target;
    };
    const std::array destinations{
        Destination{.name = "Pitch A", .id = DestinationId::pitchA, .target = Target::pitch},
        Destination{.name = "Phase A", .id = DestinationId::phaseA, .target = Target::phase},
        Destination{.name = "Volume A", .id = DestinationId::volumeA, .target = Target::volume},
        Destination{.name = "Pan A", .id = DestinationId::panA, .target = Target::pan},
        Destination{.name = "LFO 1 Rate", .id = DestinationId::lfo1Rate, .target = Target::lfoRate},
        Destination{.name = "LFO 1 Gain", .id = DestinationId::lfo1Gain, .target = Target::lfoGain},
    };
    for (const auto& d : destinations)
    {
        for (const auto r : evalRates)
        {
            if (d.target == Target::lfoRate && r == EvalRate::gridRamped)
            {
                continue;
            }
            for (const std::size_t routes : {std::size_t{0}, std::size_t{4}, std::size_t{16}})
            {
                if (routes == 0 && (d.target != Target::lfoRate && d.target != Target::lfoGain))
                {
                    continue; // the no-routes row above
                }
                Held h{destinationPatch(d.id, d.target, r, routes)};
                BENCHMARK("modulation: " + d.name + ", " + std::string(rateName(r)) + ", " + std::to_string(routes) +
                          " routes x512")
                {
                    return h.run();
                };
            }
        }
    }
    {
        Held h{mixedPatch()};
        BENCHMARK("modulation: 16 routes, mixed, recommended rates x512")
        {
            return h.run();
        };
    }
}

TEST_CASE("Modulation: CPU, 16 voices held", "[!benchmark][modulation]")
{
    for (const bool routes : {false, true})
    {
        ControlRamp wheel;
        wheel.set(0, 0.5f);
        ControlRamp const aftertouch;
        const ModPatch patch = routes ? mixedPatch() : ModPatch{};
        const ModContext context{.patch = &patch, .modWheel = &wheel, .aftertouch = &aftertouch};
        auto allocator =
            std::make_unique<dsp::VoiceAllocator<ModVoice<>, 16>>(3, anoesis::test::d6::minBlepTable64(), &context);
        allocator->forEachVoice([](auto& v) { initPatch(v); });
        allocator->prepare(spec);
        dsp::LinearSmoother cutoff;
        cutoff.setCurrentAndTarget(5.0f);
        dsp::VoiceKnobs knobs;
        knobs.cutoff = {&cutoff, &cutoff};
        knobs.controlCutoff = &cutoff;
        std::vector<dsp::VoiceEvent> notes;
        notes.reserve(16);
        for (int k = 0; k < 16; ++k)
        {
            notes.push_back({.offset = 0, .kind = dsp::VoiceEventKind::noteOn, .value = 40 + k});
        }
        Buffer out{2, blockSize};
        allocator->process(out.block(), notes, knobs);
        for (int k = 0; k < 40; ++k)
        {
            out.block().clear();
            allocator->process(out.block(), {}, knobs);
        }
        BENCHMARK(std::string("modulation: 16 voices, ") + (routes ? "16 routes, mixed" : "no routes") + " x512")
        {
            out.block().clear();
            allocator->process(out.block(), {}, knobs);
            return out.channel(0)[blockSize - 1];
        };
    }
}

TEST_CASE("Modulation: CPU of the registries, 16 routes", "[!benchmark][modulation]")
{
    std::array<Route, routeCount> patch{};
    const auto p = mixedPatch();
    patch = p.routes;
    // Runtime inputs, so the compiler can't fold the work away.
    dsp::Random random{5};
    std::array<float, sourceCount> values{};
    for (auto& v : values)
    {
        v = random.nextBipolar();
    }
    std::array<BoundInputs, sourceCount> inputs{};
    for (auto& in : inputs)
    {
        const float a = random.nextUnipolar();
        const float b = random.nextUnipolar();
        in = {
            .ends = {.lo = std::min(a, b), .hi = std::max(a, b)},
            .envelope =
                {
                    .stage = static_cast<dsp::AdsrStage>(random.nextUint32() % 5),
                    .level = random.nextUnipolar(),
                    .sustain = random.nextUnipolar(),
                },
            .lfo = {.lo = -a, .hi = b},
        };
    }
    // The sources a voice's 16 routes would ask about, in a runtime order.
    std::array<std::size_t, routeCount> asked{};
    for (auto& a : asked)
    {
        a = 1 + (random.nextUint32() % (sourceCount - 1));
    }
    const auto ca = table_registry::compile(patch);
    const auto cb = function_registry::compile(patch);
    const auto cc = type_registry::compile(patch);
    BENCHMARK("modulation: registry (a) table, accumulate 16 routes")
    {
        ModSums s{};
        table_registry::accumulate(ca, values, s);
        return s[1][0];
    };
    BENCHMARK("modulation: registry (b) functions, accumulate 16 routes")
    {
        ModSums s{};
        function_registry::accumulate(cb, values, s);
        return s[1][0];
    };
    BENCHMARK("modulation: registry (c) types, accumulate 16 routes")
    {
        ModSums s{};
        type_registry::accumulate(cc, values, s);
        return s[1][0];
    };
    BENCHMARK("modulation: registry (a) table, compile 16 routes")
    {
        return table_registry::compile(patch).count;
    };
    BENCHMARK("modulation: registry (a) table, 16 bounds")
    {
        float sum = 0.0f;
        for (const auto s : asked)
        {
            sum += table_registry::bound(s, inputs[s]).hi;
        }
        return sum;
    };
    BENCHMARK("modulation: registry (b) functions, 16 bounds")
    {
        float sum = 0.0f;
        for (const auto s : asked)
        {
            sum += function_registry::bound(s, inputs[s]).hi;
        }
        return sum;
    };
    BENCHMARK("modulation: registry (c) types, 16 bounds")
    {
        float sum = 0.0f;
        for (const auto s : asked)
        {
            sum += type_registry::bound(s, inputs[s]).hi;
        }
        return sum;
    };
}
