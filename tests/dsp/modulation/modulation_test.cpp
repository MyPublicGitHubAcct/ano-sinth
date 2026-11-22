// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The modulation model's prototypes (M1.5 task 2, stage 1; docs/dsp/
// modulation.md), checked in the default run: the three registries agree
// and keep their IDs apart from their order; each source's bound holds over
// random segments; ModVoice with no routes is dsp::Voice to the bit; with
// routes, its output doesn't depend on the host's block size and nothing
// allocates. The measurements are the hidden [modulation-report].

#include "modulation_candidates.hpp"
#include "registry_candidates.hpp"
#include "registry_size_probe.hpp"
#include "source_bounds.hpp"

#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

using namespace anoesis::test::modulation;
using anoesis::test::Buffer;
using Catch::Matchers::WithinAbs;

namespace
{

constexpr float rate = 48000.0f;
const dsp::ProcessSpec spec{.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 2};

std::array<Route, routeCount> randomPatch(dsp::Random& random)
{
    std::array<Route, routeCount> patch{};
    for (auto& r : patch)
    {
        // About a quarter of the slots empty, a few at amount 0.
        const auto s = random.nextUint32();
        r.source = static_cast<std::uint8_t>(s % sourceCount);
        r.destination = static_cast<std::uint8_t>((s >> 8U) % destinationCount);
        r.amount = (s >> 16U) % 5 == 0 ? 0.0f : 10.0f * random.nextBipolar();
    }
    return patch;
}

} // namespace

TEST_CASE("Modulation registries: the three candidates compute the same sums and bounds", "[modulation-model]")
{
    dsp::Random random{7};
    for (int trial = 0; trial < 200; ++trial)
    {
        const auto patch = randomPatch(random);
        std::array<float, sourceCount> values{};
        for (auto& v : values)
        {
            v = random.nextBipolar();
        }
        ModSums a{};
        ModSums b{};
        ModSums c{};
        const auto ca = table_registry::compile(patch);
        const auto cb = function_registry::compile(patch);
        const auto cc = type_registry::compile(patch);
        REQUIRE(ca.count == cb.count);
        REQUIRE(ca.count == cc.count);
        table_registry::accumulate(ca, values, a);
        function_registry::accumulate(cb, values, b);
        type_registry::accumulate(cc, values, c);
        for (std::size_t t = 0; t < targetCount; ++t)
        {
            for (std::size_t p = 0; p < 2; ++p)
            {
                // (a) scales the amount by the units first, (b) and (c) the
                // product: an ulp or two apart for Pitch's 12.
                CHECK_THAT(b[t][p], WithinAbs(a[t][p], 1e-4));
                CHECK_THAT(c[t][p], WithinAbs(a[t][p], 1e-4));
            }
        }
        const BoundInputs in{
            .ends = {.lo = -0.25f, .hi = 0.5f},
            .envelope = {.stage = dsp::AdsrStage::decay, .level = 0.8f, .sustain = 0.3f},
            .lfo = {.lo = -0.4f, .hi = 0.1f},
        };
        for (std::size_t s = 0; s < sourceCount; ++s)
        {
            const auto x = table_registry::bound(s, in);
            const auto y = function_registry::bound(s, in);
            const auto z = type_registry::bound(s, in);
            CHECK_THAT(y.lo, WithinAbs(x.lo, 0.0));
            CHECK_THAT(y.hi, WithinAbs(x.hi, 0.0));
            CHECK_THAT(z.lo, WithinAbs(x.lo, 0.0));
            CHECK_THAT(z.hi, WithinAbs(x.hi, 0.0));
        }
    }
}

TEST_CASE("Modulation registries: an empty slot or an amount of 0 adds an exact 0", "[modulation-model]")
{
    std::array<Route, routeCount> patch{};
    patch[0] = {.source = id(SourceId::lfo1), .destination = id(DestinationId::pitchA), .amount = 0.0f};
    patch[1] = {.source = 0, .destination = id(DestinationId::mixA), .amount = 5.0f};
    patch[2] = {.source = id(SourceId::velocity), .destination = 0, .amount = 5.0f};
    CHECK(table_registry::compile(patch).count == 0);
    CHECK(function_registry::compile(patch).count == 0);
    CHECK(type_registry::compile(patch).count == 0);
}

TEST_CASE("Modulation registries: each destination is found by its key, not by its label or position",
          "[modulation-model]")
{
    for (std::size_t i = 0; i < destinationRows.size(); ++i)
    {
        const auto& row = destinationRows[i];
        CHECK(table_registry::findDestination(row.key) == static_cast<DestinationId>(i));
        CHECK(function_registry::findDestination(row.key) == i);
        CHECK(type_registry::findDestination(row.key) == i);
        if (i > 0)
        {
            CHECK_FALSE(table_registry::findDestination(row.name).has_value());
        }
    }
}

TEST_CASE("Modulation registries: the size probes compile each candidate on its own", "[modulation-model]")
{
    std::array<Route, routeCount> patch{};
    patch[0] = {.source = id(SourceId::velocity), .destination = id(DestinationId::volumeAB), .amount = 2.0f};
    std::array<float, sourceCount> values{};
    values[id(SourceId::velocity)] = 0.5f;
    for (auto* probe : {
             &size_probe::evaluateTable,
             &size_probe::evaluateTableDsp,
             &size_probe::evaluateFunctions,
             &size_probe::evaluateTypes,
         })
    {
        const ModSums s = probe(patch, values);
        CHECK_THAT(s[index(Target::volume)][0], WithinAbs(1.0, 0.0));
        CHECK_THAT(s[index(Target::volume)][1], WithinAbs(1.0, 0.0));
    }
}

TEST_CASE("Modulation bounds: an envelope's bound holds over random segments", "[modulation-model]")
{
    dsp::Random random{11};
    std::size_t stageViolations = 0;
    std::size_t planViolations = 0;
    std::size_t segments = 0;
    for (int trial = 0; trial < 60; ++trial)
    {
        dsp::Adsr e;
        e.prepare(spec);
        e.setAttack(0.0005f + (0.05f * random.nextUnipolar()));
        e.setDecay(0.0005f + (0.05f * random.nextUnipolar()));
        e.setSustain(trial % 4 == 0 ? 0.0f : random.nextUnipolar());
        e.setRelease(0.0005f + (0.05f * random.nextUnipolar()));
        e.gateOn();
        std::array<float, 32> y{};
        for (int seg = 0; seg < 200; ++seg)
        {
            // Events between segments only: a gate, or a sustain change.
            const auto r = random.nextUint32() % 40;
            if (r == 0)
            {
                e.gateOff();
            }
            else if (r == 1)
            {
                e.gateOn();
            }
            else if (r == 2)
            {
                e.setSustain(random.nextUnipolar());
            }
            const auto n = 1 + (random.nextUint32() % 32);
            const auto state = envelopeState(e);
            const auto stage = envelopeStageBound(state);
            const auto plan = envelopePlanBound(state);
            e.process(std::span{y}.first(n));
            for (std::size_t i = 0; i < n; ++i)
            {
                stageViolations += stage.contains(y[i]) ? 0U : 1U;
                planViolations += plan.contains(y[i]) ? 0U : 1U;
            }
            ++segments;
        }
    }
    CHECK(segments == 12000);
    CHECK(stageViolations == 0);
    CHECK(planViolations == 0);
}

TEST_CASE("Modulation bounds: an LFO's range and its segment rendered ahead hold over random segments",
          "[modulation-model]")
{
    dsp::Random random{13};
    std::size_t rangeViolations = 0;
    for (int trial = 0; trial < 88; ++trial)
    {
        dsp::Lfo lfo{static_cast<std::uint64_t>(trial)};
        lfo.prepare(spec);
        lfo.setWaveform(static_cast<dsp::LfoWaveform>(static_cast<std::size_t>(trial) % dsp::lfoWaveformCount));
        lfo.setFrequency(0.1f * std::exp2(13.0f * random.nextUnipolar()));
        std::array<float, 32> wave{};
        for (int seg = 0; seg < 300; ++seg)
        {
            if (random.nextUint32() % 50 == 0)
            {
                lfo.retrigger();
            }
            const float gain = random.nextUnipolar();
            const float offset = random.nextBipolar();
            lfo.process(wave);
            const auto exact =
                lfoSourceBound(renderedBound(wave), {.lo = gain, .hi = gain}, {.lo = offset, .hi = offset});
            const auto range = lfoSourceBound(lfoWaveRange, {.lo = 0.0f, .hi = 1.0f}, {.lo = -1.0f, .hi = 1.0f});
            for (const float w : wave)
            {
                const float source = 0.5f * gain * (w + offset);
                REQUIRE(exact.contains(source));
                rangeViolations += range.contains(source) ? 0U : 1U;
            }
        }
    }
    CHECK(rangeViolations == 0);
}

namespace
{

using Allocator = dsp::VoiceAllocator<dsp::Voice<>, 16>;
using ModAllocator = dsp::VoiceAllocator<ModVoice<>, 16>;

template <typename V>
void configure(V& v, bool twoParts)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        v.slot(s).setWaveform(dsp::Waveform::saw);
        v.slot(s).setVoices(s == 0 || (twoParts && s == 2) ? 1 : 0);
    }
    v.slot(2).setTuning({.semitones = 7});
    v.envelope(0).setRelease(0.05f);
    v.envelope(1).setRelease(0.05f);
}

std::vector<dsp::VoiceEvent> sequence()
{
    using K = dsp::VoiceEventKind;
    return {
        {.offset = 0, .kind = K::noteOn, .value = 57, .velocity = 100},
        {.offset = 3001, .kind = K::noteOn, .value = 61, .velocity = 70},
        {.offset = 7013, .kind = K::noteOn, .value = 64, .velocity = 120},
        {.offset = 9100, .kind = K::noteOff, .value = 57},
        {.offset = 12007, .kind = K::noteOn, .value = 57, .velocity = 90},
        {.offset = 15000, .kind = K::noteOff, .value = 61},
        {.offset = 18000, .kind = K::noteOff, .value = 64},
        {.offset = 19000, .kind = K::noteOff, .value = 57},
    };
}

template <typename A>
Buffer renderAllocator(A& allocator, std::size_t blockSize, std::size_t length, const dsp::VoiceKnobs& knobs,
                       std::size_t* allocations = nullptr)
{
    Buffer out{2, length};
    const auto events = sequence();
    std::size_t e = 0;
    for (std::size_t start = 0; start < length; start += blockSize)
    {
        const auto n = std::min(blockSize, length - start);
        std::vector<dsp::VoiceEvent> here;
        while (e < events.size() && events[e].offset < start + n)
        {
            auto ev = events[e];
            ev.offset -= start;
            here.push_back(ev);
            ++e;
        }
        auto const block = out.block().subBlock(start, n);
        const anoesis::test::AllocationCounter counter;
        allocator.process(block, here, knobs);
        if (allocations != nullptr)
        {
            *allocations += counter.count();
        }
    }
    return out;
}

dsp::VoiceKnobs stillKnobs(std::array<dsp::LinearSmoother, 3>& ramps)
{
    for (auto& r : ramps)
    {
        r.setCurrentAndTarget(5.0f);
    }
    dsp::VoiceKnobs k;
    k.cutoff = {ramps.data(), ramps.data()};
    k.controlCutoff = &ramps[1];
    k.keytrack = 0.0f;
    return k;
}

ModPatch busyPatch(EvalRate lfoRates)
{
    ModPatch p;
    p.modEnvelopes[0] = {.attack = 0.01f, .decay = 0.1f, .sustain = 0.3f, .release = 0.05f};
    p.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = 6.0f, .gain = 8.0f};
    p.lfos[1] = {.waveform = dsp::LfoWaveform::triangle, .rate = 3.0f, .gain = 10.0f};
    p.route(0, SourceId::modEnv1, DestinationId::cutoffA, 3.0f);
    p.route(1, SourceId::modEnv1, DestinationId::mixB, -4.0f);
    p.route(4, SourceId::lfo1, DestinationId::pitchA, 0.2f);
    p.route(5, SourceId::lfo1, DestinationId::lfo2Rate, 3.0f);
    p.route(6, SourceId::lfo2, DestinationId::lfo1Gain, 5.0f);
    p.route(7, SourceId::lfo2, DestinationId::panAB, 4.0f);
    p.route(8, SourceId::velocity, DestinationId::volumeA, -3.0f);
    p.route(9, SourceId::keytrack, DestinationId::phaseB, 2.0f);
    p.route(10, SourceId::modWheel, DestinationId::resonanceAB, 2.0f);
    p.route(11, SourceId::lfo1, DestinationId::lfo1Rate, 2.0f);
    p.rates[index(Target::lfoRate)] = lfoRates;
    p.rates[index(Target::lfoGain)] = lfoRates == EvalRate::perSample ? EvalRate::perSample : EvalRate::gridRamped;
    return p;
}

} // namespace

TEST_CASE("ModVoice with no routes is dsp::Voice to the bit", "[modulation-model]")
{
    const auto& table = anoesis::test::d6::minBlepTable64();
    for (const bool twoParts : {false, true})
    {
        for (const std::size_t blockSize : {std::size_t{512}, std::size_t{37}})
        {
            std::array<dsp::LinearSmoother, 3> ramps{};
            const auto knobs = stillKnobs(ramps);
            auto library = std::make_unique<Allocator>(3, table);
            library->forEachVoice([&](auto& v) { configure(v, twoParts); });
            library->prepare(spec);
            library->setPolyphony(2);
            const ModPatch empty{};
            ControlRamp const wheel;
            const ModContext context{.patch = &empty, .modWheel = &wheel};
            auto prototype = std::make_unique<ModAllocator>(3, table, &context);
            prototype->forEachVoice([&](auto& v) { configure(v, twoParts); });
            prototype->prepare(spec);
            prototype->setPolyphony(2);
            const auto a = renderAllocator(*library, blockSize, 24000, knobs);
            const auto b = renderAllocator(*prototype, blockSize, 24000, knobs);
            CHECK(anoesis::test::bitIdentical(a, b));
        }
    }
}

TEST_CASE("ModVoice with routes: the output doesn't depend on the host's block size", "[modulation-model]")
{
    const auto& table = anoesis::test::d6::minBlepTable64();
    for (const auto lfoRates : {EvalRate::gridHeld, EvalRate::perSample})
    {
        for (const auto rateOfRest : evalRates)
        {
            auto patch = busyPatch(lfoRates);
            for (const auto t : {Target::pitch, Target::volume, Target::pan, Target::phase})
            {
                patch.rates[index(t)] = rateOfRest;
            }
            std::vector<Buffer> renders;
            for (const std::size_t blockSize : {std::size_t{4096}, std::size_t{37}, std::size_t{1}})
            {
                std::array<dsp::LinearSmoother, 3> ramps{};
                const auto knobs = stillKnobs(ramps);
                ControlRamp wheel;
                wheel.set(0, 0.6f);
                const ModContext context{.patch = &patch, .modWheel = &wheel};
                auto voices = std::make_unique<ModAllocator>(5, table, &context);
                voices->forEachVoice([&](auto& v) { configure(v, true); });
                voices->prepare(spec);
                voices->setPolyphony(2);
                std::size_t allocations = 0;
                renders.push_back(renderAllocator(*voices, blockSize, 21000, knobs, &allocations));
                CHECK(allocations == 0);
            }
            CHECK(anoesis::test::bitIdentical(renders[0], renders[1]));
            CHECK(anoesis::test::bitIdentical(renders[0], renders[2]));
        }
    }
}
