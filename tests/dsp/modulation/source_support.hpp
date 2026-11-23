// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Rendering helpers for the sources' candidates (M1.5 task 3, stage 1):
// SourceEngine and, as the reference, dsp::Voice through the same
// VoiceAllocator and knobs.

#pragma once

#include "source_candidates.hpp"

#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>

#include <anoesis/test/buffer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace anoesis::test::sources
{

inline const MinBlepTable& table()
{
    return anoesis::test::d6::minBlepTable64();
}

/// The voice's patch, outside the modulation.
struct Voicing
{
    std::array<dsp::Waveform, 4> waves{dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw};
    std::array<int, 4> voices{1, 0, 0, 0};
    std::array<int, 4> semitones{0, 0, 7, 0};
    std::array<FilterInput, 2> inputs{FilterInput::own, FilterInput::own};
    bool bypass = false;
    float cutoff = 5.0f;
    float controlCutoff = 5.0f;
    float resonance = 0.0f;
    std::array<float, 2> attack{0.0f, 0.0f};
    std::array<float, 2> release{0.0f, 0.0f};
    std::size_t polyphony = 16;
    float portamento = 0.0f;
    bool slide = false;
    bool monoLegato = false;
};

template <typename V>
void configure(V& v, const Voicing& s)
{
    for (std::size_t i = 0; i < 4; ++i)
    {
        v.slot(i).setWaveform(s.waves[i]);
        v.slot(i).setVoices(s.voices[i]);
        v.slot(i).setTuning({.semitones = s.semitones[i]});
    }
    v.drift().setDepthCents(0.0f);
    for (std::size_t p = 0; p < 2; ++p)
    {
        v.setFilterInput(p, s.inputs[p]);
        v.setFilterBypassed(p, s.bypass);
        v.envelope(p).setAttack(s.attack[p]);
        v.envelope(p).setRelease(s.release[p]);
    }
}

template <typename A>
void configureAllocator(A& a, const Voicing& s)
{
    a.setPolyphony(s.polyphony);
    a.setPortamento(s.portamento);
    a.setPortamentoMode(s.slide ? dsp::PortamentoMode::slide : dsp::PortamentoMode::normal);
    a.setMonoLegato(s.monoLegato);
}

inline SourceEvent noteOn(std::size_t t, int key, int velocity = 127)
{
    return {
        .offset = t,
        .kind = SourceEvent::Kind::voice,
        .voice = {.kind = dsp::VoiceEventKind::noteOn, .value = key, .velocity = velocity},
    };
}
inline SourceEvent noteOff(std::size_t t, int key)
{
    return {
        .offset = t,
        .kind = SourceEvent::Kind::voice,
        .voice = {.kind = dsp::VoiceEventKind::noteOff, .value = key},
    };
}
inline SourceEvent bend(std::size_t t, int value)
{
    return {
        .offset = t,
        .kind = SourceEvent::Kind::voice,
        .voice = {.kind = dsp::VoiceEventKind::pitchBend, .value = value},
    };
}
inline SourceEvent wheel(std::size_t t, int value)
{
    return {.offset = t, .kind = SourceEvent::Kind::modWheel, .value = value};
}
inline SourceEvent pressure(std::size_t t, int value)
{
    return {.offset = t, .kind = SourceEvent::Kind::aftertouch, .value = value};
}
inline SourceEvent route(std::size_t t, std::size_t slot, Source s, mod::Destination d, float amount)
{
    return {
        .offset = t,
        .kind = SourceEvent::Kind::slot,
        .slot = slot,
        .route = {.source = s, .destination = d, .amount = amount},
    };
}
inline SourceEvent step(std::size_t t, float level)
{
    return {.offset = t, .kind = SourceEvent::Kind::stepVelocity, .level = level};
}

struct RenderOptions
{
    std::size_t blockSize = 512;
    float sampleRate = 48000.0f;
    Smoothing smoothing = Smoothing::linear;
    float smoothingMs = 5.0f;
    std::size_t* allocations = nullptr;
};

struct Render
{
    anoesis::test::Buffer out;
    BoundStats stats;
    std::uint64_t replayed = 0;
};

inline void addStats(BoundStats& total, const BoundStats& s)
{
    for (std::size_t i = 0; i < sourceCount; ++i)
    {
        total.checked[i] += s.checked[i];
        total.outside[i] += s.outside[i];
        total.width[i] += s.width[i];
        total.worst[i] = std::max(total.worst[i], s.worst[i]);
    }
}

/// Renders `events` (any order; slot events included) through SourceEngine.
inline Render render(const SourceSettings& settings, const Voicing& voicing, std::vector<SourceEvent> events,
                     std::size_t length, const RenderOptions& options = {})
{
    std::ranges::stable_sort(events, {}, &SourceEvent::offset);
    auto engine = std::make_unique<SourceEngine>(table(), settings, 3);
    engine->voices().forEachVoice([&](auto& v) { configure(v, voicing); });
    engine->setCutoff(voicing.cutoff, voicing.controlCutoff);
    engine->setResonance(voicing.resonance, 0.0f);
    engine->setControllers(options.smoothing, options.smoothingMs);
    engine->prepare({.sampleRate = options.sampleRate, .maxBlockSize = 4096, .numChannels = 2});
    configureAllocator(engine->voices(), voicing);
    Render r{.out = anoesis::test::Buffer{2, length}};
    std::size_t e = 0;
    std::vector<SourceEvent> here;
    here.reserve(events.size());
    for (std::size_t start = 0; start < length; start += options.blockSize)
    {
        const auto n = std::min(options.blockSize, length - start);
        here.clear();
        while (e < events.size() && events[e].offset < start + n)
        {
            auto ev = events[e];
            ev.offset -= start;
            here.push_back(ev);
            ++e;
        }
        const anoesis::test::AllocationCounter counter;
        engine->process(r.out.block().subBlock(start, n), here);
        if (options.allocations != nullptr)
        {
            *options.allocations += counter.count();
        }
    }
    engine->voices().forEachVoice(
        [&](auto& v)
        {
            addStats(r.stats, v.boundStats());
            r.replayed += v.replayedSamples();
        });
    return r;
}

/// The same voicing and note events through dsp::Voice and VoiceAllocator,
/// with the engine's knobs (slot and controller events are ignored).
inline anoesis::test::Buffer renderLibrary(const Voicing& voicing, std::vector<SourceEvent> events, std::size_t length,
                                           std::size_t blockSize = 512, float sampleRate = 48000.0f)
{
    std::ranges::stable_sort(events, {}, &SourceEvent::offset);
    auto a = std::make_unique<dsp::VoiceAllocator<dsp::Voice<>, 16>>(3, table());
    a->forEachVoice([&](auto& v) { configure(v, voicing); });
    a->prepare({.sampleRate = sampleRate, .maxBlockSize = 4096, .numChannels = 2});
    configureAllocator(*a, voicing);
    std::array<LinearSmoother, 2> cutoff{};
    std::array<LinearSmoother, 2> resonance{};
    LinearSmoother controlCutoff;
    LinearSmoother controlResonance;
    cutoff[0].setCurrentAndTarget(voicing.cutoff);
    cutoff[1].setCurrentAndTarget(voicing.cutoff);
    controlCutoff.setCurrentAndTarget(voicing.controlCutoff);
    resonance[0].setCurrentAndTarget(voicing.resonance);
    resonance[1].setCurrentAndTarget(voicing.resonance);
    VoiceKnobs k;
    k.cutoff = {cutoff.data(), &cutoff[1]};
    k.resonance = {resonance.data(), &resonance[1]};
    k.controlCutoff = &controlCutoff;
    k.controlResonance = &controlResonance;
    anoesis::test::Buffer out{2, length};
    std::size_t e = 0;
    for (std::size_t start = 0; start < length; start += blockSize)
    {
        const auto n = std::min(blockSize, length - start);
        auto const block = out.block().subBlock(start, n);
        block.clear();
        // Split at the events, as SourceEngine does.
        std::size_t done = 0;
        while (done < n)
        {
            std::array<dsp::VoiceEvent, 16> here{};
            std::size_t count = 0;
            while (e < events.size() && events[e].offset <= start + done)
            {
                if (events[e].kind == SourceEvent::Kind::voice && count < here.size())
                {
                    here[count] = events[e].voice;
                    here[count].offset = 0;
                    ++count;
                }
                ++e;
            }
            const std::size_t next = e < events.size() ? std::min(events[e].offset - start, n) : n;
            a->process(block.subBlock(done, next - done), std::span{here}.first(count), k);
            done = next;
        }
    }
    return out;
}

[[nodiscard]] inline bool identical(const anoesis::test::Buffer& a, const anoesis::test::Buffer& b)
{
    for (std::size_t c = 0; c < 2; ++c)
    {
        const auto x = a.channel(c);
        const auto y = b.channel(c);
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            if (std::bit_cast<std::uint32_t>(x[i]) != std::bit_cast<std::uint32_t>(y[i]))
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace anoesis::test::sources
