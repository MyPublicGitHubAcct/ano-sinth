// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Rendering helpers for the sources as built in dsp::Voice (M1.5 task 3,
// stage 2; docs/dsp/modulation-sources.md, "As built").
//
// LibraryEngine is the prototype's SourceEngine (source_candidates.hpp) with
// dsp::Voice in place of SourceVoice: the real VoiceAllocator over 16
// voices, and the engine's modulation state as M1.8 will hold it (the
// modulation::Patch, the two controller ramps, the free LFOs' global
// instances, Step Velocity, the mod envelopes' and LFOs' settings), passed
// to the voices through VoiceKnobs::sources. It takes the prototype's
// SourceEvents and splits its blocks at them exactly as SourceEngine does,
// so the two render the same events at the same samples.
//
// BoundCollector observes one voice (modulation::SourceObserver): every
// source's value at every sample against the bound the voice took at the
// last grid point or call start, and three deliberately small rules taken
// over the same stretch from the values themselves.

#pragma once

#include "source_candidates.hpp"
#include "source_support.hpp"

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/modulation/controller_ramp.hpp>
#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/modulation/route_compiler.hpp>
#include <anoesis/dsp/modulation/source_bound.hpp>
#include <anoesis/dsp/modulation/source_context.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <anoesis/test/buffer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace anoesis::test::voice_sources
{

namespace dsp = anoesis::dsp;
namespace mod = anoesis::dsp::modulation;
namespace proto = anoesis::test::sources;

using Dest = mod::Destination;
using mod::Source;
using proto::SourceEvent;
using proto::Voicing;

inline constexpr std::size_t sourceCount = mod::sourceCount;

[[nodiscard]] constexpr std::size_t at(Source s) noexcept
{
    return static_cast<std::size_t>(s);
}

/// The library's settings from the prototype's (the same fields).
inline void copySettings(const proto::SourceSettings& from, mod::SourceContext& to)
{
    for (std::size_t k = 0; k < 2; ++k)
    {
        const auto& e = from.modEnvelopes[k];
        to.modEnvelopes[k] = {.attack = e.attack, .decay = e.decay, .sustain = e.sustain, .release = e.release};
        const auto& l = from.lfos[k];
        to.lfos[k] = {.waveform = l.waveform, .rate = l.rate, .gain = l.gain, .offset = l.offset, .free = l.free};
    }
}

/// The prototype's decided configuration (every SourceSettings default is
/// the decision's), with the given envelopes and LFOs.
[[nodiscard]] inline proto::SourceSettings decided(const proto::SourceSettings& s)
{
    proto::SourceSettings d;
    d.modEnvelopes = s.modEnvelopes;
    d.lfos = s.lfos;
    return d;
}

/// `s` for a comparison with the prototype: a non-free LFO on a row that
/// draws random values (S&H, Smooth) on TriSaw instead. Setting the waveform
/// before the restart (the library's fix of the prototype's flaw) draws one
/// more value at a voice's first note, so from there such a voice's random
/// stream is one draw apart from the prototype's.
[[nodiscard]] inline proto::SourceSettings comparable(const proto::SourceSettings& s)
{
    auto c = s;
    for (auto& l : c.lfos)
    {
        const bool random = l.waveform == dsp::LfoWaveform::sampleHold || l.waveform == dsp::LfoWaveform::smooth;
        if (random && !l.free)
        {
            l.waveform = dsp::LfoWaveform::trisaw;
        }
    }
    return c;
}

/// dsp::Voice over the real VoiceAllocator, with the engine's modulation
/// state: SourceEngine's twin.
class LibraryEngine
{
public:
    using Voice = dsp::Voice<>;
    using Allocator = dsp::VoiceAllocator<Voice, 16>;

    /// attach false: the voices get no context (VoiceKnobs::sources null),
    /// as every caller before this stage.
    LibraryEngine(const dsp::MinBlepTable& table, const proto::SourceSettings& settings, std::uint64_t seed = 1,
                  bool attach = true)
        : attached(attach), scratch(4096)
    {
        copySettings(settings, context);
        context.patch = &patch;
        context.modWheel = &modWheel;
        context.aftertouch = &aftertouch;
        context.freeLfos = &freeLfos;
        allocator = std::make_unique<Allocator>(seed, table);
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

    void prepare(const dsp::ProcessSpec& spec)
    {
        allocator->prepare(spec);
        modWheel.prepare(spec.sampleRate);
        aftertouch.prepare(spec.sampleRate);
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
    void process(const dsp::AudioBlock& out, std::span<const SourceEvent> events) noexcept
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
                const auto& ev = events[k];
                switch (ev.kind)
                {
                case SourceEvent::Kind::modWheel:
                    modWheel.setMidi(time + done, ev.value);
                    break;
                case SourceEvent::Kind::aftertouch:
                    aftertouch.setMidi(time + done, ev.value);
                    break;
                case SourceEvent::Kind::slot:
                    patch.set(ev.slot, ev.route);
                    break;
                case SourceEvent::Kind::stepVelocity:
                    context.stepVelocity = ev.level;
                    break;
                case SourceEvent::Kind::voice:
                    if (count < here.size())
                    {
                        here[count] = ev.voice;
                        here[count].offset = 0;
                        ++count;
                    }
                    break;
                }
            }
            const std::size_t next = e < events.size() ? std::min(events[e].offset, n) : n;
            const auto piece = out.subBlock(done, next - done);
            allocator->process(piece, std::span{here}.first(count), knobs(done));
            // The free LFOs move on with the piece: they stand at the start
            // of the allocator's next call.
            for (auto& l : freeLfos)
            {
                l.process(std::span{scratch}.first(next - done));
            }
            done = next;
        }
        time += n;
    }

private:
    [[nodiscard]] dsp::VoiceKnobs knobs(std::size_t done) const noexcept
    {
        dsp::VoiceKnobs k;
        k.cutoff = {cutoff.data(), &cutoff[1]};
        k.resonance = {resonance.data(), &resonance[1]};
        k.controlCutoff = &controlCutoff;
        k.controlResonance = &controlResonance;
        k.time = time + done;
        k.sources = attached ? &context : nullptr;
        return k;
    }

    bool attached = true;
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
    std::vector<float> scratch;
    std::uint64_t time = 0;
};

/// The deliberately small rules, each taken over a stretch (from a grid
/// point or a call's start to the next) from the values themselves.
enum class SmallRule : std::uint8_t
{
    ownEnds,   ///< the stretch's first and last values (keytrack's own ends; an LFO's ends)
    levelOnly, ///< the stretch's first value
    count,
};
inline constexpr std::size_t smallRuleCount = static_cast<std::size_t>(SmallRule::count);

/// Per source: samples checked, outside the voice's bound, its summed width
/// (in the prototype's order, to compare with BoundStats), the worst excess,
/// and samples outside each small rule.
struct BoundTotals
{
    std::array<std::uint64_t, sourceCount> checked{};
    std::array<std::uint64_t, sourceCount> outside{};
    std::array<double, sourceCount> width{};
    std::array<float, sourceCount> worst{};
    std::array<std::array<std::uint64_t, sourceCount>, smallRuleCount> small{};
    std::uint64_t chunks = 0;

    void add(const BoundTotals& t)
    {
        for (std::size_t s = 0; s < sourceCount; ++s)
        {
            checked[s] += t.checked[s];
            outside[s] += t.outside[s];
            width[s] += t.width[s];
            worst[s] = std::max(worst[s], t.worst[s]);
            for (std::size_t r = 0; r < smallRuleCount; ++r)
            {
                small[r][s] += t.small[r][s];
            }
        }
        chunks += t.chunks;
    }
};

/// One voice's observer.
class BoundCollector final : public mod::SourceObserver
{
public:
    void observe(const mod::SourceObservation& chunk) noexcept override
    {
        ++totals.chunks;
        if (chunk.rebound)
        {
            finish();
            bounds = chunk.bounds;
        }
        for (std::size_t s = 1; s < sourceCount; ++s)
        {
            const auto& b = bounds[s];
            for (std::size_t i = 0; i < chunk.length; ++i)
            {
                const float v = chunk.values[s][i];
                ++totals.checked[s];
                totals.width[s] += static_cast<double>(b.hi - b.lo);
                if (!b.contains(v))
                {
                    ++totals.outside[s];
                    totals.worst[s] = std::max({totals.worst[s], b.lo - v, v - b.hi});
                }
                if (stretchLength + i < stretch[s].size())
                {
                    stretch[s][stretchLength + i] = v;
                }
            }
        }
        stretchLength += chunk.length;
    }

    /// Ends the stretch in progress: the small rules over it.
    void finish() noexcept
    {
        const auto n = std::min(stretchLength, stretch[0].size());
        for (std::size_t s = 1; s < sourceCount && n > 0; ++s)
        {
            const auto values = std::span{stretch[s]}.first(n);
            const auto ends = mod::linearBound(values.front(), values.back());
            const mod::Interval level{.lo = values.front(), .hi = values.front()};
            for (const float v : values)
            {
                totals.small[static_cast<std::size_t>(SmallRule::ownEnds)][s] += ends.contains(v) ? 0U : 1U;
                totals.small[static_cast<std::size_t>(SmallRule::levelOnly)][s] += level.contains(v) ? 0U : 1U;
            }
        }
        stretchLength = 0;
    }

    BoundTotals totals;

private:
    mod::SourceBounds bounds{};
    std::array<std::array<float, 32>, sourceCount> stretch{};
    std::size_t stretchLength = 0;
};

struct RenderOptions
{
    std::size_t blockSize = 512;
    float sampleRate = 48000.0f;
    std::size_t* allocations = nullptr;
    bool attach = true;            ///< the library: VoiceKnobs::sources set
    BoundTotals* bounds = nullptr; ///< the library: observe every voice
    /// Every voice plays one note first, with both LFOs read (below).
    bool prime = true;
};

/// The priming phase's length: every voice has finished by its end.
[[nodiscard]] inline std::size_t primeLength(float sampleRate)
{
    return static_cast<std::size_t>(0.12f * sampleRate);
}

/// The priming phase, then `events` after it. Every voice (Polyphony 16)
/// plays one 64-sample note with both LFOs routed (to Cutoff A), and the two
/// slots are cleared a sample before the scenario. The stage-1 prototype
/// restarts a voice's LFO with the waveform it last had and sets the patch's
/// only at the grid point after (its flaw; the library sets it first), so
/// until a voice has played once with its LFOs running, the two differ on
/// any waveform but the sine. After this they agree.
[[nodiscard]] inline std::vector<SourceEvent> primed(const std::vector<SourceEvent>& events, std::size_t prime)
{
    using namespace anoesis::test::sources;
    std::vector<SourceEvent> e{
        route(0, 0, Source::lfo1, Dest::cutoffA, 0.01f),
        route(0, 1, Source::lfo2, Dest::cutoffA, 0.01f),
    };
    for (int k = 0; k < 16; ++k)
    {
        e.push_back(noteOn(0, 40 + k));
        e.push_back(noteOff(64, 40 + k));
    }
    e.push_back(route(prime - 1, 0, Source::none, Dest::none, 0.0f));
    e.push_back(route(prime - 1, 1, Source::none, Dest::none, 0.0f));
    for (auto ev : events)
    {
        ev.offset += prime;
        e.push_back(ev);
    }
    return e;
}

/// Renders `events` (any order) through an engine (LibraryEngine or the
/// prototype's SourceEngine), after the priming phase if asked; the output
/// includes it.
template <typename Engine>
anoesis::test::Buffer renderEngine(Engine& engine, const Voicing& voicing, std::vector<SourceEvent> events,
                                   std::size_t length, const RenderOptions& options)
{
    const std::size_t prime = options.prime ? primeLength(options.sampleRate) : 0;
    if (options.prime)
    {
        events = primed(events, prime);
    }
    std::ranges::stable_sort(events, {}, &SourceEvent::offset);
    engine.voices().forEachVoice([&](auto& v) { proto::configure(v, voicing); });
    engine.setCutoff(voicing.cutoff, voicing.controlCutoff);
    engine.setResonance(voicing.resonance, 0.0f);
    engine.prepare({.sampleRate = options.sampleRate, .maxBlockSize = 4096, .numChannels = 2});
    Voicing priming = voicing;
    priming.polyphony = 16;
    priming.monoLegato = false;
    proto::configureAllocator(engine.voices(), options.prime ? priming : voicing);
    bool configured = !options.prime;
    const std::size_t total = prime + length;
    anoesis::test::Buffer out{2, total};
    std::size_t e = 0;
    std::vector<SourceEvent> here;
    here.reserve(events.size());
    for (std::size_t start = 0; start < total; start += options.blockSize)
    {
        const auto n = std::min(options.blockSize, total - start);
        if (!configured && start + n > prime)
        {
            proto::configureAllocator(engine.voices(), voicing); // every priming voice has finished
            configured = true;
        }
        here.clear();
        while (e < events.size() && events[e].offset < start + n)
        {
            auto ev = events[e];
            ev.offset -= start;
            here.push_back(ev);
            ++e;
        }
        const anoesis::test::AllocationCounter counter;
        engine.process(out.block().subBlock(start, n), here);
        if (options.allocations != nullptr)
        {
            *options.allocations += counter.count();
        }
    }
    if (prime == 0)
    {
        return out;
    }
    // The scenario's samples only: in the priming phase the two differ by
    // the prototype's flaw itself.
    anoesis::test::Buffer scenario{2, length};
    for (std::size_t c = 0; c < 2; ++c)
    {
        std::ranges::copy(out.channel(c).subspan(prime), scenario.channel(c).begin());
    }
    return scenario;
}

/// LibraryEngine: dsp::Voice.
inline anoesis::test::Buffer renderLibrary(const proto::SourceSettings& settings, const Voicing& voicing,
                                           const std::vector<SourceEvent>& events, std::size_t length,
                                           const RenderOptions& options = {})
{
    auto engine = std::make_unique<LibraryEngine>(proto::table(), settings, 3, options.attach);
    std::array<BoundCollector, 16> collectors{};
    for (std::size_t i = 0; i < 16 && options.bounds != nullptr; ++i)
    {
        engine->voices().voice(i).setSourceObserver(&collectors[i]);
    }
    auto out = renderEngine(*engine, voicing, events, length, options);
    if (options.bounds != nullptr)
    {
        // Per render, then into the total: the prototype's order of sums.
        BoundTotals render;
        for (auto& c : collectors)
        {
            c.finish();
            render.add(c.totals);
        }
        options.bounds->add(render);
    }
    return out;
}

/// The prototype's SourceEngine with `settings` as given (decided() for its
/// decided configuration), and its bound statistics.
inline proto::Render renderPrototype(const proto::SourceSettings& settings, const Voicing& voicing,
                                     const std::vector<SourceEvent>& events, std::size_t length,
                                     const RenderOptions& options = {})
{
    auto engine = std::make_unique<proto::SourceEngine>(proto::table(), settings, 3);
    proto::Render r{.out = renderEngine(*engine, voicing, events, length, options)};
    engine->voices().forEachVoice([&](auto& v) { proto::addStats(r.stats, v.boundStats()); });
    return r;
}

[[nodiscard]] inline double energy(const anoesis::test::Buffer& b)
{
    double e = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (const float x : b.channel(c))
        {
            e += static_cast<double>(x) * static_cast<double>(x);
        }
    }
    return e;
}

// --- Scenarios (stage 1's, from source_candidates_test.cpp) ---------------------------------

/// A patch that reads every source into Pitch, Mix, Cutoff and the LFOs.
inline std::vector<SourceEvent> busyRoutes()
{
    using proto::route;
    return {
        route(0, 0, Source::modEnv1, Dest::pitchA, 0.1f),    route(0, 1, Source::modEnv1, Dest::mixA, 5.0f),
        route(0, 2, Source::modEnv2, Dest::cutoffA, 3.0f),   route(0, 4, Source::lfo1, Dest::pitchAB, 0.05f),
        route(0, 5, Source::lfo1, Dest::lfo2Gain, 5.0f),     route(0, 6, Source::lfo2, Dest::mixB, 4.0f),
        route(0, 8, Source::velocity, Dest::mixAB, 3.0f),    route(0, 9, Source::keytrack, Dest::pitchB, -0.5f),
        route(0, 10, Source::modWheel, Dest::pitchA, 0.2f),  route(0, 11, Source::aftertouch, Dest::mixA, -3.0f),
        route(0, 12, Source::ampEnvB, Dest::pitchA, 0.1f),   route(0, 13, Source::stepVelocity, Dest::mixB, 2.0f),
        route(0, 14, Source::ampEnvA, Dest::lfo1Rate, 2.0f),
    };
}

inline proto::SourceSettings busySettings()
{
    proto::SourceSettings s;
    s.modEnvelopes[0] = {.attack = 0.02f, .decay = 0.1f, .sustain = 0.4f, .release = 0.05f};
    s.modEnvelopes[1] = {.attack = 0.005f, .decay = 0.3f, .sustain = 0.0f, .release = 0.02f};
    s.lfos[0] = {.waveform = dsp::LfoWaveform::triangle, .rate = 6.0f, .gain = 8.0f, .offset = 2.0f};
    s.lfos[1] = {.waveform = dsp::LfoWaveform::sampleHold, .rate = 5.5f, .gain = 4.0f};
    return s;
}

inline const Voicing busyVoicing{
    .voices = {1, 0, 1, 0},
    .release = {0.05f, 0.08f},
    .polyphony = 2,
    .portamento = 0.03f,
    .slide = true,
};

/// Notes with steals and glides, the bend, the wheel, pressure and Step
/// Velocity all moving.
inline std::vector<SourceEvent> busyEvents()
{
    using namespace anoesis::test::sources;
    auto e = busyRoutes();
    for (const auto& ev : {
             noteOn(0, 57, 100),
             bend(1203, 12000),
             wheel(2001, 40),
             noteOn(5001, 61, 60),
             pressure(6117, 90),
             step(7000, 0.5f),
             noteOn(9013, 64, 127),
             bend(9500, 2000),
             wheel(10007, 127),
             noteOff(15000, 57),
             pressure(15500, 0),
             noteOff(17000, 61),
             noteOn(18000, 50, 30),
             noteOff(19000, 64),
             noteOff(23000, 50),
         })
    {
        e.push_back(ev);
    }
    return e;
}

/// A random scenario: glides, opposing bends, legato notes, controllers,
/// retriggers (stage 1's bound scenario).
inline std::vector<SourceEvent> randomScenario(dsp::Random& random, std::size_t length, std::size_t sampleRate)
{
    using namespace anoesis::test::sources;
    std::vector<SourceEvent> e = busyRoutes();
    std::size_t t = 0;
    int key = 60;
    while (t < length)
    {
        const auto r = random.nextUint32();
        switch (r % 6)
        {
        case 0:
            key = 40 + static_cast<int>((r >> 8U) % 40);
            e.push_back(noteOn(t, key, 1 + static_cast<int>((r >> 16U) % 127)));
            e.push_back(bend(t + 1 + ((r >> 4U) % 200), (r & 1U) != 0 ? 0 : 16383));
            break;
        case 1:
            e.push_back(noteOff(t, key));
            break;
        case 2:
            e.push_back(bend(t, static_cast<int>((r >> 8U) % 16384)));
            break;
        case 3:
            e.push_back(wheel(t, static_cast<int>((r >> 8U) % 128)));
            break;
        case 4:
            e.push_back(pressure(t, static_cast<int>((r >> 8U) % 128)));
            break;
        default:
            e.push_back(step(t, random.nextUnipolar()));
            break;
        }
        t += 1 + ((r >> 20U) % (sampleRate / 20));
    }
    return e;
}

} // namespace anoesis::test::voice_sources
