// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Rendering helpers for the per-voice destinations' candidates (M1.5 task 4,
// stage 1; docs/dsp/modulation-destinations.md).
//
// DestEngine is task 3's LibraryEngine (voice_sources_support.hpp) with
// DestVoice in place of dsp::Voice: the real VoiceAllocator over 16 voices
// and the engine's modulation state, plus the knob events task 4 needs
// (each slot's Volume and Pan, set on every voice at the event's sample, and
// for KnobSource::globalRamps also the engine's own 10 ms ramps in knob
// units, which it moves on with each piece it renders, as M1.8 will).

#pragma once

#include "destination_voice.hpp"
#include "source_support.hpp"
#include "voice_sources_support.hpp"

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/modulation/controller_ramp.hpp>
#include <anoesis/dsp/modulation/route_compiler.hpp>
#include <anoesis/dsp/modulation/source_context.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <anoesis/test/buffer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace anoesis::test::destinations
{

namespace proto = anoesis::test::sources;
namespace mod = anoesis::dsp::modulation;

using Dest = mod::Destination;
using mod::Source;
using proto::SourceEvent;
using proto::Voicing;

/// A source event (notes, controllers, the patch), or a slot's Volume or
/// Pan knob set at its sample.
struct DestEvent
{
    enum class Kind : std::uint8_t
    {
        source,
        volume, ///< slot `slot`'s Volume, `value` in knob units (0-10)
        pan,    ///< slot `slot`'s Pan, `value` in knob units (0-10, 5 the centre)
    };
    std::size_t offset = 0;
    Kind kind = Kind::source;
    SourceEvent event{};
    std::size_t slot = 0;
    float value = 0.0f;
};

inline DestEvent wrap(const SourceEvent& e)
{
    return {.offset = e.offset, .kind = DestEvent::Kind::source, .event = e};
}
inline std::vector<DestEvent> wrapAll(const std::vector<SourceEvent>& events)
{
    std::vector<DestEvent> out;
    out.reserve(events.size());
    for (const auto& e : events)
    {
        out.push_back(wrap(e));
    }
    return out;
}
inline DestEvent volumeKnob(std::size_t t, std::size_t slot, float units)
{
    return {.offset = t, .kind = DestEvent::Kind::volume, .slot = slot, .value = units};
}
inline DestEvent panKnob(std::size_t t, std::size_t slot, float units)
{
    return {.offset = t, .kind = DestEvent::Kind::pan, .slot = slot, .value = units};
}
inline DestEvent route(std::size_t t, std::size_t slot, Source s, Dest d, float amount)
{
    return wrap(proto::route(t, slot, s, d, amount));
}
inline DestEvent noteOn(std::size_t t, int key, int velocity = 127)
{
    return wrap(proto::noteOn(t, key, velocity));
}
inline DestEvent noteOff(std::size_t t, int key)
{
    return wrap(proto::noteOff(t, key));
}
inline DestEvent wheel(std::size_t t, int value)
{
    return wrap(proto::wheel(t, value));
}

/// The voice's patch outside the modulation: task 3's Voicing plus each
/// slot's Volume and Pan (knob units), Inv, Stereo, Retrig and Phase.
struct DestVoicing
{
    Voicing base{};
    std::array<float, 4> volume{10.0f, 10.0f, 10.0f, 10.0f};
    std::array<float, 4> pan{5.0f, 5.0f, 5.0f, 5.0f};
    std::array<bool, 4> invert{};
    std::array<float, 4> stereo{};
    std::array<bool, 4> retrigger{true, true, true, true};
    std::array<float, 4> phaseDegrees{};
};

template <typename V>
void configureSlots(V& v, const DestVoicing& d)
{
    proto::configure(v, d.base);
    for (std::size_t i = 0; i < 4; ++i)
    {
        auto& s = v.slot(i);
        s.setVolume(d.volume[i] / 10.0f);
        s.setPan((d.pan[i] / 5.0f) - 1.0f);
        s.setInverted(d.invert[i]);
        s.setStereo(d.stereo[i]);
        s.setRetrigger(d.retrigger[i]);
        s.setPhaseDegrees(d.phaseDegrees[i]);
    }
}

/// DestVoice over the real VoiceAllocator, with the engine's modulation
/// state and knob ramps.
class DestEngine
{
public:
    using Voice = DestVoice<>;
    using Allocator = dsp::VoiceAllocator<Voice, 16>;

    DestEngine(const dsp::MinBlepTable& table, const proto::SourceSettings& settings,
               const DestinationSettings& destinationSettings, std::uint64_t seed = 3)
        : scratch(4096)
    {
        voice_sources::copySettings(settings, context);
        context.patch = &patch;
        context.modWheel = &modWheel;
        context.aftertouch = &aftertouch;
        context.freeLfos = &freeLfos;
        allocator = std::make_unique<Allocator>(seed, table);
        allocator->forEachVoice(
            [&](Voice& v)
            {
                v.setDestinationSettings(destinationSettings);
                v.setKnobRamps(&volumeRamps, &panRamps);
            });
        for (auto* r : {cutoff.data(), &cutoff[1], &controlCutoff})
        {
            r->setCurrentAndTarget(5.0f);
        }
        for (std::size_t k = 0; k < 2; ++k)
        {
            freeLfos[k].setSeed(seed ^ (0xBB67AE8584CAA73BU * (k + 1)));
        }
    }

    [[nodiscard]] Allocator& voices() noexcept
    {
        return *allocator;
    }
    [[nodiscard]] mod::SourceContext& sources() noexcept
    {
        return context;
    }
    void setCutoff(float a, float control) noexcept
    {
        cutoff[0].setCurrentAndTarget(a);
        cutoff[1].setCurrentAndTarget(a);
        controlCutoff.setCurrentAndTarget(control);
    }
    void setResonance(float a, float control) noexcept
    {
        resonance[0].setCurrentAndTarget(a);
        resonance[1].setCurrentAndTarget(a);
        controlResonance.setCurrentAndTarget(control);
    }
    /// The slots' knobs before anything plays: every voice and the engine's
    /// ramps, settled.
    void setSlots(const DestVoicing& d) noexcept
    {
        allocator->forEachVoice([&](Voice& v) { configureSlots(v, d); });
        for (std::size_t i = 0; i < 4; ++i)
        {
            volumeRamps[i].setCurrentAndTarget(d.volume[i]);
            panRamps[i].setCurrentAndTarget(d.pan[i]);
        }
    }

    void prepare(const dsp::ProcessSpec& spec)
    {
        allocator->prepare(spec);
        modWheel.prepare(spec.sampleRate);
        aftertouch.prepare(spec.sampleRate);
        for (auto& r : volumeRamps)
        {
            r.setRampTime(dsp::StereoGain::defaultRampSeconds);
            r.prepare(spec);
        }
        for (auto& r : panRamps)
        {
            r.setRampTime(dsp::StereoGain::defaultRampSeconds);
            r.prepare(spec);
        }
        for (std::size_t k = 0; k < 2; ++k)
        {
            freeLfos[k].prepare(spec);
            freeLfos[k].setWaveform(context.lfos[k].waveform);
            freeLfos[k].setFrequency(mod::lfoRateHz(context.lfos[k].rate));
            freeLfos[k].restart();
        }
        time = 0;
    }

    /// One host block, its events in order of offset; written, not added to.
    void process(const dsp::AudioBlock& out, std::span<const DestEvent> events) noexcept
    {
        out.clear();
        const auto n = out.numSamples();
        std::size_t done = 0;
        std::size_t e = 0;
        while (done < n)
        {
            const std::size_t first = e;
            while (e < events.size() && events[e].offset <= done)
            {
                ++e;
            }
            std::array<dsp::VoiceEvent, 16> here{};
            std::size_t count = 0;
            for (std::size_t k = first; k < e; ++k)
            {
                apply(events[k], here, count);
            }
            const std::size_t next = e < events.size() ? std::min(events[e].offset, n) : n;
            const auto piece = out.subBlock(done, next - done);
            allocator->process(piece, std::span{here}.first(count), knobs(done));
            const auto length = next - done;
            for (auto& l : freeLfos)
            {
                l.process(std::span{scratch}.first(length));
            }
            for (auto& r : volumeRamps)
            {
                r.skip(length);
            }
            for (auto& r : panRamps)
            {
                r.skip(length);
            }
            done = next;
        }
        time += n;
    }

private:
    void apply(const DestEvent& ev, std::array<dsp::VoiceEvent, 16>& here, std::size_t& count) noexcept
    {
        switch (ev.kind)
        {
        case DestEvent::Kind::volume:
            allocator->forEachVoice([&](Voice& v) { v.slot(ev.slot).setVolume(ev.value / 10.0f); });
            volumeRamps[ev.slot].setTarget(ev.value);
            return;
        case DestEvent::Kind::pan:
            allocator->forEachVoice([&](Voice& v) { v.slot(ev.slot).setPan((ev.value / 5.0f) - 1.0f); });
            panRamps[ev.slot].setTarget(ev.value);
            return;
        case DestEvent::Kind::source:
            break;
        }
        const auto& s = ev.event;
        switch (s.kind)
        {
        case SourceEvent::Kind::modWheel:
            modWheel.setMidi(time + ev.offset, s.value);
            break;
        case SourceEvent::Kind::aftertouch:
            aftertouch.setMidi(time + ev.offset, s.value);
            break;
        case SourceEvent::Kind::slot:
            patch.set(s.slot, s.route);
            break;
        case SourceEvent::Kind::stepVelocity:
            context.stepVelocity = s.level;
            break;
        case SourceEvent::Kind::voice:
            if (count < here.size())
            {
                here[count] = s.voice;
                here[count].offset = 0;
                ++count;
            }
            break;
        }
    }

    [[nodiscard]] dsp::VoiceKnobs knobs(std::size_t done) const noexcept
    {
        dsp::VoiceKnobs k;
        k.cutoff = {cutoff.data(), &cutoff[1]};
        k.resonance = {resonance.data(), &resonance[1]};
        k.controlCutoff = &controlCutoff;
        k.controlResonance = &controlResonance;
        k.time = time + done;
        k.sources = &context;
        return k;
    }

    mod::Patch patch;
    mod::ControllerRamp modWheel;
    mod::ControllerRamp aftertouch;
    std::array<dsp::Lfo, 2> freeLfos{dsp::Lfo{}, dsp::Lfo{}};
    mod::SourceContext context;
    std::unique_ptr<Allocator> allocator;
    std::array<dsp::LinearSmoother, 2> cutoff{};
    std::array<dsp::LinearSmoother, 2> resonance{};
    dsp::LinearSmoother controlCutoff;
    dsp::LinearSmoother controlResonance;
    std::array<dsp::LinearSmoother, 4> volumeRamps{};
    std::array<dsp::LinearSmoother, 4> panRamps{};
    std::vector<float> scratch;
    std::uint64_t time = 0;
};

/// The candidates' defaults (DestinationSettings), and no mod envelope or
/// LFO settings: for renders that don't vary them.
inline const DestinationSettings defaultSettings{};
inline const proto::SourceSettings noSources{};

struct DestRenderOptions
{
    std::size_t blockSize = 512;
    float sampleRate = 48000.0f;
    std::size_t* allocations = nullptr;
    /// Called after each host block, with the engine (for measurement).
    std::function<void(DestEngine&, std::size_t end)> after;
};

/// Renders `events` (any order) through DestEngine.
inline anoesis::test::Buffer renderDest(const DestinationSettings& settings, const proto::SourceSettings& sources,
                                        const DestVoicing& voicing, std::vector<DestEvent> events, std::size_t length,
                                        const DestRenderOptions& options = {})
{
    std::ranges::stable_sort(events, {}, &DestEvent::offset);
    auto engine = std::make_unique<DestEngine>(proto::table(), sources, settings);
    engine->setSlots(voicing);
    engine->setCutoff(voicing.base.cutoff, voicing.base.controlCutoff);
    engine->setResonance(voicing.base.resonance, 0.0f);
    engine->prepare({.sampleRate = options.sampleRate, .maxBlockSize = 4096, .numChannels = 2});
    proto::configureAllocator(engine->voices(), voicing.base);
    anoesis::test::Buffer out{2, length};
    std::size_t e = 0;
    std::vector<DestEvent> here;
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
        engine->process(out.block().subBlock(start, n), here);
        if (options.allocations != nullptr)
        {
            *options.allocations += counter.count();
        }
        if (options.after)
        {
            options.after(*engine, start + n);
        }
    }
    return out;
}

/// The same voicing and source events through dsp::Voice (task 3's
/// LibraryEngine), unprimed.
inline anoesis::test::Buffer renderLibrary(const proto::SourceSettings& sources, const Voicing& voicing,
                                           const std::vector<SourceEvent>& events, std::size_t length,
                                           std::size_t blockSize = 512, float sampleRate = 48000.0f)
{
    return voice_sources::renderLibrary(sources, voicing, events, length,
                                        {.blockSize = blockSize, .sampleRate = sampleRate, .prime = false});
}

} // namespace anoesis::test::destinations
