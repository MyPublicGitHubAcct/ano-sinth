// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Candidate voice allocators for M1.4 task 2 (inventory §7 "Voice
// allocation", §4 E5, E10, E11; docs/dsp/voice-allocation.md), measured by
// the hidden [voice-allocation-report] and the [voice-allocation]
// benchmarks on dsp::Voice. Nothing here is library code: it is stage 1's
// prototype, for the project owner to choose from before an allocator is
// built in libs/dsp.
//
// The allocator owns a fixed pool of voices (Pool, 32 by default: 16 for
// the notes and 16 spares for a fade on a spare voice), constructed in
// place and never allocated. A host block is split at its events (note-on,
// note-off, a Polyphony change, each at its sample); every voice is rendered
// exactly up to each event, so each decision sees the same state whatever
// the host's block size.
//
// What the candidates vary (Policy):
//
// - Steal: which sounding voice a new note takes when Polyphony voices are
//   busy: the oldest note; the quietest (its amp envelopes' level); the
//   oldest released (key up) one first, then the oldest; a voice already on
//   the same key first, then the oldest.
// - RepeatedKey: a key played again while its last note still sounds takes
//   a new voice (the developer, 2016: "overlapping release times, as it
//   should"), or its own voice back ("voice recycling").
// - StealEnd: how the stolen note ends: cut at once (the voice stopped, the
//   new note from silence); faded over fadeSeconds, the new note starting
//   on that voice when the fade ends (a latency); faded on its own while
//   the new note starts at once on a spare voice; or retriggered in place
//   (the slots jump, the envelopes attack from their level; dsp::Voice
//   resets its filters, the prototype voice can keep them).
// - Freeing: a voice is free when its amp envelopes have released to
//   exactly 0 (E10, as decided for the Adsr); or below a threshold once
//   every gate is off; or also when every envelope sustains at exactly 0
//   with the key held (E11).
// - Busy: whether every amp envelope keeps a voice busy, or only those that
//   can be heard (a part nothing routes, or with no oscillators, doesn't).
// - PolyphonyDrop: what lowering Polyphony below the notes sounding does:
//   nothing to them (only new notes obey), release them, or cut them.
//
// Freeing is checked at every event and at each voice's own 32-sample
// points counted from its latest note-on, never at host-block boundaries,
// so the output is the same at any block size. A freed voice is stopped
// (its slots and drift silenced, its envelopes idle), so its next note
// starts from silence; Policy::stopOnFree off measures what happens
// without it.

#pragma once

#include "voice_candidates.hpp"

#include "../filters/filter_control_support.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/pitch_drift.hpp>
#include <anoesis/dsp/voice/voice.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace anoesis::test::allocation
{

namespace dsp = anoesis::dsp;
using filter_control::KnobRamps;
using filter_control::Knobs;

inline constexpr std::size_t maxPolyphony = 16;
/// Freeing is checked every 32 samples from a voice's latest note-on.
inline constexpr std::size_t checkSamples = 32;
inline constexpr std::size_t partCount = 2;
inline constexpr std::size_t slotsPerPart = 2;

enum class Steal : std::uint8_t
{
    oldest,              ///< the earliest note-on
    quietest,            ///< the lowest amp envelope level (the larger of the two), then the oldest
    releasingThenOldest, ///< the oldest whose key is up, else the oldest
    sameKeyThenOldest,   ///< a voice on the new note's key, else the oldest
};

enum class RepeatedKey : std::uint8_t
{
    newVoice, ///< a new voice; the last note's release overlaps (the developer, 2016)
    ownVoice, ///< its own sounding voice, retriggered in place ("voice recycling")
};

enum class StealEnd : std::uint8_t
{
    cut,           ///< stopped at once; the new note from silence on that voice
    fadeThenStart, ///< faded over fadeSeconds, then the new note from silence on it
    fadeOnSpare,   ///< faded over fadeSeconds; the new note at once on a spare voice
    retrigger,     ///< noteOn() on the sounding voice
};

enum class Freeing : std::uint8_t
{
    releaseEnd,     ///< the envelopes at exactly 0 (Adsr::isActive() false): E10 as decided
    threshold,      ///< also once every gate is off and the level is below thresholdDb
    sustainZeroToo, ///< also when every envelope sustains at exactly 0, key held (E11)
};

enum class Busy : std::uint8_t
{
    anyEnvelope,      ///< dsp::Voice::isActive(): either amp envelope
    audibleEnvelopes, ///< only the envelopes of parts that can be heard
};

enum class PolyphonyDrop : std::uint8_t
{
    leave,   ///< sounding notes play on; only new notes obey the new limit
    release, ///< the oldest notes over the limit are released
    cut,     ///< the oldest notes over the limit are stopped at once
};

struct Policy
{
    Steal steal = Steal::releasingThenOldest;
    RepeatedKey repeatedKey = RepeatedKey::newVoice;
    StealEnd end = StealEnd::retrigger;
    float fadeSeconds = 0.005f;
    Freeing freeing = Freeing::releaseEnd;
    float thresholdDb = -80.0f;
    Busy busy = Busy::anyEnvelope;
    PolyphonyDrop drop = PolyphonyDrop::leave;
    bool stopOnFree = true; ///< off: a freed voice is left as it stood (what dsp::Voice offers today)
};

enum class EventKind : std::uint8_t
{
    noteOn,
    noteOff,
    polyphony,
};

/// A host event at its absolute sample: a note-on or note-off (value: the
/// key) or a Polyphony change (value: 0-16).
struct Event
{
    std::size_t at = 0;
    EventKind kind = EventKind::noteOn;
    int value = 0;
};

/// The global knob ramps every voice reads (filter A's Cutoff and Resonance
/// with the Filter Control's in `a`, filter B's in `b`), positioned at the
/// first sample of the host block being processed.
struct GlobalKnobs
{
    GlobalKnobs(const Knobs& knobsA, const Knobs& knobsB, double rate) : a{knobsA, rate}, b{knobsB, rate} {}

    void advance(std::size_t samples)
    {
        a.advance(samples);
        b.advance(samples);
    }

    KnobRamps a;
    KnobRamps b;
    float keytrack = 0.0f; ///< the Keytrack knob, 0-10
};

/// What every voice in a pool is built from.
struct Context
{
    const dsp::MinBlepTable* table = nullptr;
    GlobalKnobs* knobs = nullptr;
    std::uint64_t seed = 0; ///< the instance's; each voice's comes from it and the voice's index
    voice::Engine engine{}; ///< for the prototype voice
};

/// Each voice's seed: the instance's and its index (a voice's random
/// sequences differ from every other's).
[[nodiscard]] constexpr std::uint64_t voiceSeed(std::uint64_t seed, std::size_t index) noexcept
{
    return seed ^ (0xD1B54A32D192ED03U * (static_cast<std::uint64_t>(index) + 1));
}

/// Whether part p's amp envelope can affect the output, from the filter
/// inputs (voice::Input) and each part's oscillator voice count: its own
/// filter runs with something sounding in its input, or its oscillators
/// cross into the other filter (its envelope acts before that filter).
[[nodiscard]] constexpr bool envelopeAudible(std::size_t p, const std::array<voice::Input, partCount>& inputs,
                                             const std::array<std::size_t, partCount>& oscillatorVoices) noexcept
{
    const std::size_t other = 1 - p;
    const bool ownFilterFed =
        inputs[p] != voice::Input::none &&
        (oscillatorVoices[p] > 0 || (inputs[p] == voice::Input::both && oscillatorVoices[other] > 0));
    const bool crosses = inputs[other] == voice::Input::both && oscillatorVoices[p] > 0;
    return ownFilterFed || crosses;
}

/// dsp::Voice in the allocator's terms.
class BuiltVoice
{
public:
    using Voice = dsp::Voice<dsp::minBlepTaps>;
    using Slot = Voice::Slot;

    BuiltVoice(const Context& context, std::size_t index) noexcept
        : v(*context.table, voiceSeed(context.seed, index)), knobs(context.knobs)
    {
    }

    [[nodiscard]] Voice& voice() noexcept
    {
        return v;
    }
    [[nodiscard]] Slot& slot(std::size_t index) noexcept
    {
        return v.slot(index);
    }
    [[nodiscard]] dsp::Adsr& envelope(std::size_t part) noexcept
    {
        return v.envelope(part);
    }
    [[nodiscard]] dsp::PitchDrift& drift() noexcept
    {
        return v.drift();
    }

    void setFilters(const std::array<voice::FilterSettings, partCount>& filters, bool warm) noexcept
    {
        for (std::size_t f = 0; f < partCount; ++f)
        {
            v.setFilterInput(f, builtInput(filters[f].input));
            v.setFilterBypassed(f, filters[f].bypassed);
            v.setFilterResponse(f, filters[f].response);
            v.setFilterDrive(f, filters[f].drive);
        }
        v.setWarm(warm);
    }

    [[nodiscard]] std::array<voice::Input, partCount> inputs() const noexcept
    {
        std::array<voice::Input, partCount> result{};
        for (std::size_t f = 0; f < partCount; ++f)
        {
            switch (v.filterInput(f))
            {
            case dsp::FilterInput::none:
                result[f] = voice::Input::none;
                break;
            case dsp::FilterInput::both:
                result[f] = voice::Input::both;
                break;
            case dsp::FilterInput::own:
                result[f] = voice::Input::own;
                break;
            }
        }
        return result;
    }

    [[nodiscard]] std::size_t partVoices(std::size_t part) const noexcept
    {
        return v.slot(part * slotsPerPart).voices() + v.slot((part * slotsPerPart) + 1).voices();
    }

    void prepare(const dsp::ProcessSpec& spec) noexcept
    {
        v.prepare(spec);
    }
    [[nodiscard]] bool isActive() const noexcept
    {
        return v.isActive();
    }
    void start(float note, std::size_t at) noexcept
    {
        v.noteOn(note, positioned(at));
    }
    void release() noexcept
    {
        v.noteOff();
    }
    /// Silences the voice without restarting its random sequences: what a
    /// finished voice needs before its next note (dsp::Voice has no call for
    /// it yet).
    void stop() noexcept
    {
        for (std::size_t s = 0; s < Voice::slotCount; ++s)
        {
            v.slot(s).stop();
        }
        v.drift().stop();
        v.envelope(0).reset();
        v.envelope(1).reset();
    }
    void render(const dsp::AudioBlock& out, std::size_t at) noexcept
    {
        v.process(out, positioned(at));
    }

private:
    [[nodiscard]] static dsp::FilterInput builtInput(voice::Input input) noexcept
    {
        switch (input)
        {
        case voice::Input::none:
            return dsp::FilterInput::none;
        case voice::Input::both:
            return dsp::FilterInput::both;
        case voice::Input::own:
            break;
        }
        return dsp::FilterInput::own;
    }

    /// The global ramps copied and moved on to the absolute sample `at`
    /// (dsp::Voice reads its knobs positioned at the block it is given).
    [[nodiscard]] dsp::VoiceFilterKnobs positioned(std::size_t at) noexcept
    {
        held = {
            knobs->a.cutoff,    knobs->b.cutoff,        knobs->a.resonance,
            knobs->b.resonance, knobs->a.controlCutoff, knobs->a.controlResonance,
        };
        const auto ahead = at - knobs->a.where();
        if (ahead > 0)
        {
            for (auto& ramp : held)
            {
                ramp.skip(ahead);
            }
        }
        return {
            .cutoff = {held.data(), &held[1]},
            .resonance = {&held[2], &held[3]},
            .controlCutoff = &held[4],
            .controlResonance = &held[5],
            .keytrack = knobs->keytrack,
        };
    }

    Voice v;
    GlobalKnobs* knobs;
    std::array<dsp::LinearSmoother, 6> held{};
};

/// The stage-1 prototype voice (voice_candidates.hpp) in the decided
/// configuration (I2, per-filter buffers, the grid schedule, unheard parts
/// skipped), identical to dsp::Voice to the bit; with
/// Engine::keepFiltersOnRetrigger, a note-on on a sounding voice keeps the
/// filters and the grid.
class PrototypeAdapter
{
public:
    using Voice = voice::PrototypeVoice<dsp::minBlepTaps>;
    using Slot = Voice::Slot;

    PrototypeAdapter(const Context& context, std::size_t index) noexcept
        : v(*context.table, readings(), decided(context.engine), voiceSeed(context.seed, index)), knobs(context.knobs)
    {
    }

    [[nodiscard]] Voice& voice() noexcept
    {
        return v;
    }
    [[nodiscard]] Slot& slot(std::size_t index) noexcept
    {
        return v.slot(index);
    }
    [[nodiscard]] dsp::Adsr& envelope(std::size_t part) noexcept
    {
        return v.envelope(part);
    }
    [[nodiscard]] dsp::PitchDrift& drift() noexcept
    {
        return v.drift();
    }

    void setFilters(const std::array<voice::FilterSettings, partCount>& filters, bool warm) noexcept
    {
        patch.filters = filters;
        patch.warm = warm;
        v.setPatch(patch);
    }

    [[nodiscard]] std::array<voice::Input, partCount> inputs() const noexcept
    {
        return {patch.filters[0].input, patch.filters[1].input};
    }

    [[nodiscard]] std::size_t partVoices(std::size_t part) noexcept
    {
        return v.slot(part * slotsPerPart).voices() + v.slot((part * slotsPerPart) + 1).voices();
    }

    /// Allocates the prototype's scratch (32 samples per buffer).
    void prepare(const dsp::ProcessSpec& spec)
    {
        v.prepare(spec);
    }
    [[nodiscard]] bool isActive() const noexcept
    {
        return v.isActive();
    }
    void start(float note, std::size_t at) noexcept
    {
        v.noteOn(note, feed(), at);
    }
    void release() noexcept
    {
        v.noteOff();
    }
    void stop() noexcept
    {
        for (std::size_t s = 0; s < Voice::slotCount; ++s)
        {
            v.slot(s).stop();
        }
        v.drift().stop();
        v.envelope(0).reset();
        v.envelope(1).reset();
    }
    void render(const dsp::AudioBlock& out, std::size_t at) noexcept
    {
        v.render(out, feed(), at);
    }

private:
    [[nodiscard]] static const voice::Readings& readings() noexcept
    {
        static const voice::Readings developer;
        return developer;
    }

    [[nodiscard]] static voice::Engine decided(voice::Engine engine) noexcept
    {
        engine.layout = voice::Layout::perFilter;
        engine.schedule = voice::Schedule::grid32;
        engine.skipUnheard = true;
        engine.monoWhenCentred = false;
        return engine;
    }

    [[nodiscard]] voice::FilterFeed feed() const noexcept
    {
        return {.a = &knobs->a, .b = &knobs->b, .keytrack = knobs->keytrack};
    }

    Voice v;
    GlobalKnobs* knobs;
    voice::Patch patch{};
};

/// A voice that renders nothing: its envelopes only follow the gates, so it
/// never finishes unless released with a release of 0. For the cost of the
/// allocator's own bookkeeping per event.
class StubVoice
{
public:
    StubVoice(const Context& /*context*/, std::size_t /*index*/) noexcept {}

    [[nodiscard]] dsp::Adsr& envelope(std::size_t part) noexcept
    {
        return envelopes[part];
    }
    [[nodiscard]] static std::array<voice::Input, partCount> inputs() noexcept
    {
        return {voice::Input::own, voice::Input::own};
    }
    [[nodiscard]] static std::size_t partVoices(std::size_t part) noexcept
    {
        return part == 0 ? 1 : 0;
    }
    void prepare(const dsp::ProcessSpec& spec) noexcept
    {
        for (auto& e : envelopes)
        {
            e.prepare(spec);
        }
    }
    [[nodiscard]] bool isActive() const noexcept
    {
        return envelopes[0].isActive() || envelopes[1].isActive();
    }
    void start(float /*note*/, std::size_t /*at*/) noexcept
    {
        for (auto& e : envelopes)
        {
            e.gateOn();
        }
    }
    void release() noexcept
    {
        for (auto& e : envelopes)
        {
            e.gateOff();
        }
    }
    void stop() noexcept
    {
        for (auto& e : envelopes)
        {
            e.reset();
        }
    }
    void render(const dsp::AudioBlock& /*out*/, std::size_t /*at*/) noexcept {}

private:
    std::array<dsp::Adsr, partCount> envelopes{};
};

/// The Voices readout's candidate counts (manual p. 11: "the number of
/// active voices and the maximum total number of voices").
struct Readout
{
    std::size_t notes = 0;         ///< busy voices (notes), fades on spares not included
    std::size_t fading = 0;        ///< voices fading out on a spare
    std::size_t perNoteVoices = 0; ///< R1: busy notes x every oscillator's unison voices
    std::size_t perPartVoices = 0; ///< R2: per busy note, the voices of the parts whose amp envelope is active
    std::size_t maximum = 0;       ///< the manual's: every oscillator's voices x Polyphony
};

struct Stats
{
    std::size_t noteOns = 0;
    std::size_t steals = 0;
    std::size_t stolenHeld = 0;     ///< steals of a note whose key was still down
    std::size_t ignored = 0;        ///< note-ons at Polyphony 0
    std::size_t spareExhausted = 0; ///< fades on a spare that found none free (cut instead)
    std::size_t maxFading = 0;
    std::size_t frees = 0;
};

/// One freeing: the voice, the sample, the key.
struct Freed
{
    std::size_t voice = 0;
    std::size_t at = 0;
    int note = -1;
};

template <typename V, std::size_t Pool = 2 * maxPolyphony>
class Allocator
{
public:
    static constexpr std::size_t poolSize = Pool;
    static constexpr std::size_t freeLogSize = 256;

    Allocator(const Context& context, const Policy& policy) noexcept
        : voices(make(context, std::make_index_sequence<Pool>{})), policy(policy)
    {
    }

    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;
    Allocator(Allocator&&) = delete;
    Allocator& operator=(Allocator&&) = delete;
    ~Allocator() = default;

    [[nodiscard]] V& voice(std::size_t index) noexcept
    {
        return voices[index];
    }

    /// Configures every voice the same way (a patch).
    template <typename F>
    void forEachVoice(const F& configure)
    {
        for (std::size_t i = 0; i < Pool; ++i)
        {
            configure(voices[i]);
        }
    }

    void prepare(const dsp::ProcessSpec& spec)
    {
        for (auto& v : voices)
        {
            v.prepare(spec);
        }
        fadeSamples = static_cast<std::size_t>(
            std::lround(static_cast<double>(policy.fadeSeconds) * static_cast<double>(spec.sampleRate)));
        thresholdLevel = std::pow(10.0f, policy.thresholdDb / 20.0f);
        records = {};
        counter = 0;
        statistics = {};
        log = {};
        logged = 0;
    }

    /// Before the first block; later changes are events.
    void setPolyphony(std::size_t notes) noexcept
    {
        limit = std::min(notes, maxPolyphony);
    }

    /// Adds every voice into channels 0 and 1 of `out`, whose first sample is
    /// the absolute sample `at`; `events` are this block's, sorted by sample.
    void process(const dsp::AudioBlock& out, std::span<const Event> events, std::size_t at) noexcept
    {
        const auto end = at + out.numSamples();
        std::size_t from = at;
        std::size_t e = 0;
        while (from < end)
        {
            while (e < events.size() && events[e].at <= from)
            {
                handle(events[e], from);
                ++e;
            }
            const auto to = e < events.size() ? std::min(end, events[e].at) : end;
            for (std::size_t i = 0; i < Pool; ++i)
            {
                if (records[i].state != State::free)
                {
                    renderVoice(i, out.subBlock(from - at, to - from), from);
                }
            }
            from = to;
        }
    }

    [[nodiscard]] Readout readout() noexcept
    {
        Readout r;
        const auto perNote = voices[0].partVoices(0) + voices[0].partVoices(1);
        r.maximum = perNote * limit;
        for (std::size_t i = 0; i < Pool; ++i)
        {
            const auto& rec = records[i];
            if (rec.state == State::free)
            {
                continue;
            }
            if (rec.state == State::fadingOut)
            {
                ++r.fading;
                continue;
            }
            ++r.notes;
            r.perNoteVoices += perNote;
            for (std::size_t p = 0; p < partCount; ++p)
            {
                r.perPartVoices += voices[i].envelope(p).isActive() ? voices[i].partVoices(p) : 0;
            }
        }
        return r;
    }

    [[nodiscard]] const Stats& stats() const noexcept
    {
        return statistics;
    }

    [[nodiscard]] std::span<const Freed> freed() const noexcept
    {
        return std::span{log}.first(std::min(logged, freeLogSize));
    }

    /// The key of a voice's note (-1 when free) and whether its key is down.
    [[nodiscard]] int noteOf(std::size_t index) const noexcept
    {
        return records[index].state == State::free ? -1 : records[index].note;
    }
    [[nodiscard]] bool keyDown(std::size_t index) const noexcept
    {
        return records[index].state != State::free && records[index].keyDown;
    }
    [[nodiscard]] bool isCounted(std::size_t index) const noexcept
    {
        return counts(records[index]);
    }

private:
    enum class State : std::uint8_t
    {
        free,
        sounding,     ///< a note, key down or released
        fadingOut,    ///< fading on its own, not counted; free at the fade's end (fadeOnSpare)
        fadingToNote, ///< fading, then `pending` starts on it (fadeThenStart)
    };

    struct Record
    {
        State state = State::free;
        int note = -1;
        bool keyDown = false;
        std::uint64_t age = 0;
        std::size_t sinceCheck = 0;
        std::size_t fadeLeft = 0;
        std::size_t fadeLength = 0;
        int pending = -1;
        bool pendingKeyDown = false;
        std::uint64_t pendingAge = 0;
    };

    template <std::size_t... I>
    static std::array<V, Pool> make(const Context& context, std::index_sequence<I...> /*indices*/) noexcept
    {
        return {V{context, I}...};
    }

    [[nodiscard]] static bool counts(const Record& r) noexcept
    {
        return r.state == State::sounding || r.state == State::fadingToNote;
    }

    [[nodiscard]] std::size_t counted() const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(records, counts));
    }

    [[nodiscard]] bool matters(std::size_t i, std::size_t p) noexcept
    {
        if (policy.busy == Busy::anyEnvelope)
        {
            return true;
        }
        return envelopeAudible(p, voices[i].inputs(), {voices[i].partVoices(0), voices[i].partVoices(1)});
    }

    [[nodiscard]] bool busy(std::size_t i) noexcept
    {
        if (policy.busy == Busy::anyEnvelope)
        {
            return voices[i].isActive();
        }
        for (std::size_t p = 0; p < partCount; ++p)
        {
            if (matters(i, p) && voices[i].envelope(p).isActive())
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] float level(std::size_t i) noexcept
    {
        float l = 0.0f;
        for (std::size_t p = 0; p < partCount; ++p)
        {
            if (matters(i, p))
            {
                l = std::max(l, voices[i].envelope(p).level());
            }
        }
        return l;
    }

    [[nodiscard]] bool finished(std::size_t i) noexcept
    {
        if (!busy(i))
        {
            return true;
        }
        switch (policy.freeing)
        {
        case Freeing::releaseEnd:
            return false;
        case Freeing::threshold:
        {
            for (std::size_t p = 0; p < partCount; ++p)
            {
                if (matters(i, p) && voices[i].envelope(p).isGateOn())
                {
                    return false;
                }
            }
            return level(i) <= thresholdLevel;
        }
        case Freeing::sustainZeroToo:
        {
            for (std::size_t p = 0; p < partCount; ++p)
            {
                const auto& e = voices[i].envelope(p);
                const bool silentHold =
                    e.stage() == dsp::AdsrStage::sustain && e.level() <= 0.0f && e.sustainLevel() <= 0.0f;
                if (matters(i, p) && e.isActive() && !silentHold)
                {
                    return false;
                }
            }
            return true;
        }
        }
        return false;
    }

    void free(std::size_t i, std::size_t at) noexcept
    {
        auto& rec = records[i];
        if (policy.stopOnFree)
        {
            voices[i].stop();
        }
        log[logged % freeLogSize] = {.voice = i, .at = at, .note = rec.note};
        ++logged;
        ++statistics.frees;
        rec = {};
    }

    void freeFinished(std::size_t at) noexcept
    {
        for (std::size_t i = 0; i < Pool; ++i)
        {
            if (records[i].state == State::sounding && finished(i))
            {
                free(i, at);
            }
        }
    }

    [[nodiscard]] std::size_t firstFree() const noexcept
    {
        for (std::size_t i = 0; i < Pool; ++i)
        {
            if (records[i].state == State::free)
            {
                return i;
            }
        }
        return Pool;
    }

    void begin(std::size_t i, int note, std::uint64_t age, std::size_t at) noexcept
    {
        auto& rec = records[i];
        rec = {.state = State::sounding, .note = note, .keyDown = true, .age = age};
        voices[i].start(static_cast<float>(note), at);
    }

    /// The voice a new note on `note` takes when the limit is reached, among
    /// the sounding ones (Pool if none).
    [[nodiscard]] std::size_t victim(int note) noexcept
    {
        std::size_t best = Pool;
        const auto better = [&](std::size_t i) -> bool
        {
            if (best == Pool)
            {
                return true;
            }
            const auto& a = records[i];
            const auto& b = records[best];
            switch (policy.steal)
            {
            case Steal::oldest:
                break;
            case Steal::quietest:
            {
                const float li = level(i);
                const float lb = level(best);
                if (li < lb || lb < li)
                {
                    return li < lb;
                }
                break;
            }
            case Steal::releasingThenOldest:
                if (a.keyDown != b.keyDown)
                {
                    return !a.keyDown;
                }
                break;
            case Steal::sameKeyThenOldest:
                if ((a.note == note) != (b.note == note))
                {
                    return a.note == note;
                }
                break;
            }
            return a.age < b.age;
        };
        for (std::size_t i = 0; i < Pool; ++i)
        {
            if (records[i].state == State::sounding && better(i))
            {
                best = i;
            }
        }
        return best;
    }

    void noteOn(int note, std::size_t at) noexcept
    {
        ++statistics.noteOns;
        if (limit == 0)
        {
            ++statistics.ignored;
            return;
        }
        const auto age = ++counter;
        if (policy.repeatedKey == RepeatedKey::ownVoice)
        {
            std::size_t own = Pool;
            for (std::size_t i = 0; i < Pool; ++i)
            {
                if (records[i].state == State::sounding && records[i].note == note &&
                    (own == Pool || records[i].age > records[own].age))
                {
                    own = i;
                }
            }
            if (own < Pool)
            {
                begin(own, note, age, at);
                return;
            }
        }
        if (counted() < limit)
        {
            const auto i = firstFree();
            if (i < Pool)
            {
                begin(i, note, age, at);
                return;
            }
        }
        ++statistics.steals;
        const auto i = victim(note);
        if (i == Pool)
        {
            // Every counted voice is fading towards a pending note: the
            // oldest pending note gives way to this one.
            std::size_t oldestPending = Pool;
            for (std::size_t j = 0; j < Pool; ++j)
            {
                if (records[j].state == State::fadingToNote &&
                    (oldestPending == Pool || records[j].pendingAge < records[oldestPending].pendingAge))
                {
                    oldestPending = j;
                }
            }
            if (oldestPending < Pool)
            {
                records[oldestPending].pending = note;
                records[oldestPending].pendingKeyDown = true;
                records[oldestPending].pendingAge = age;
            }
            return;
        }
        statistics.stolenHeld += records[i].keyDown ? 1 : 0;
        steal(i, note, age, at);
    }

    void steal(std::size_t i, int note, std::uint64_t age, std::size_t at) noexcept
    {
        auto& rec = records[i];
        const auto end =
            fadeSamples == 0 && (policy.end == StealEnd::fadeThenStart || policy.end == StealEnd::fadeOnSpare)
                ? StealEnd::cut
                : policy.end;
        switch (end)
        {
        case StealEnd::cut:
            voices[i].stop();
            begin(i, note, age, at);
            return;
        case StealEnd::retrigger:
            begin(i, note, age, at);
            return;
        case StealEnd::fadeThenStart:
            rec.state = State::fadingToNote;
            rec.fadeLeft = fadeSamples;
            rec.fadeLength = fadeSamples;
            rec.pending = note;
            rec.pendingKeyDown = true;
            rec.pendingAge = age;
            return;
        case StealEnd::fadeOnSpare:
        {
            const auto spare = firstFree();
            if (spare == Pool)
            {
                ++statistics.spareExhausted;
                voices[i].stop();
                begin(i, note, age, at);
                return;
            }
            rec.state = State::fadingOut;
            rec.fadeLeft = fadeSamples;
            rec.fadeLength = fadeSamples;
            begin(spare, note, age, at);
            const auto fading = static_cast<std::size_t>(
                std::ranges::count_if(records, [](const Record& r) { return r.state == State::fadingOut; }));
            statistics.maxFading = std::max(statistics.maxFading, fading);
            return;
        }
        }
    }

    void noteOff(int note) noexcept
    {
        for (std::size_t i = 0; i < Pool; ++i)
        {
            auto& rec = records[i];
            if (rec.state == State::sounding && rec.note == note && rec.keyDown)
            {
                rec.keyDown = false;
                voices[i].release();
            }
            if (rec.state == State::fadingToNote && rec.pending == note)
            {
                rec.pendingKeyDown = false;
            }
        }
    }

    void changePolyphony(std::size_t notes) noexcept
    {
        limit = std::min(notes, maxPolyphony);
        if (policy.drop == PolyphonyDrop::leave)
        {
            return;
        }
        // The oldest notes over the limit (by the steal policy's order).
        auto excess = counted() > limit ? counted() - limit : 0;
        std::array<bool, Pool> chosen{};
        while (excess > 0)
        {
            std::size_t oldest = Pool;
            for (std::size_t i = 0; i < Pool; ++i)
            {
                if (records[i].state == State::sounding && !chosen[i] &&
                    (oldest == Pool || records[i].age < records[oldest].age))
                {
                    oldest = i;
                }
            }
            if (oldest == Pool)
            {
                break;
            }
            chosen[oldest] = true;
            --excess;
        }
        for (std::size_t i = 0; i < Pool; ++i)
        {
            if (!chosen[i])
            {
                continue;
            }
            if (policy.drop == PolyphonyDrop::release)
            {
                records[i].keyDown = false;
                voices[i].release();
            }
            else
            {
                voices[i].stop();
                records[i] = {};
            }
        }
    }

    void handle(const Event& event, std::size_t at) noexcept
    {
        freeFinished(at);
        switch (event.kind)
        {
        case EventKind::noteOn:
            noteOn(event.value, at);
            break;
        case EventKind::noteOff:
            noteOff(event.value);
            break;
        case EventKind::polyphony:
            changePolyphony(static_cast<std::size_t>(std::max(event.value, 0)));
            break;
        }
    }

    /// Renders voice i over `out` (absolute sample `at`), in pieces that stop
    /// at its check points and its fade's end.
    void renderVoice(std::size_t i, const dsp::AudioBlock& out, std::size_t at) noexcept
    {
        auto& rec = records[i];
        const auto n = out.numSamples();
        std::size_t done = 0;
        while (done < n && rec.state != State::free)
        {
            auto length = std::min(n - done, checkSamples - rec.sinceCheck);
            const bool fading = rec.state == State::fadingOut || rec.state == State::fadingToNote;
            if (fading)
            {
                length = std::min(length, rec.fadeLeft);
            }
            const auto piece = out.subBlock(done, length);
            if (fading)
            {
                renderFading(i, piece, at + done);
            }
            else
            {
                voices[i].render(piece, at + done);
            }
            done += length;
            rec.sinceCheck = (rec.sinceCheck + length) % checkSamples;
            if (fading)
            {
                rec.fadeLeft -= length;
                if (rec.fadeLeft == 0)
                {
                    endFade(i, at + done);
                }
            }
            else if (rec.sinceCheck == 0 && finished(i))
            {
                free(i, at + done);
            }
        }
    }

    /// The fade: a linear gain from (N - 1)/N down to 0 over N samples.
    void renderFading(std::size_t i, const dsp::AudioBlock& out, std::size_t at) noexcept
    {
        const auto& rec = records[i];
        const auto length = out.numSamples();
        for (auto& channel : scratch)
        {
            std::fill_n(channel.begin(), length, 0.0f);
        }
        std::array<float*, 2> pointers{scratch[0].data(), scratch[1].data()};
        voices[i].render({pointers.data(), 2, length}, at);
        const auto done = rec.fadeLength - rec.fadeLeft;
        const float step = 1.0f / static_cast<float>(rec.fadeLength);
        for (std::size_t c = 0; c < 2; ++c)
        {
            auto* dst = out.channel(c).data();
            for (std::size_t k = 0; k < length; ++k)
            {
                const float gain = static_cast<float>(rec.fadeLength - 1 - (done + k)) * step;
                dst[k] += scratch[c][k] * gain;
            }
        }
    }

    void endFade(std::size_t i, std::size_t at) noexcept
    {
        auto& rec = records[i];
        voices[i].stop();
        if (rec.state == State::fadingToNote)
        {
            const auto pending = rec.pending;
            const auto keyIsDown = rec.pendingKeyDown;
            const auto age = rec.pendingAge;
            begin(i, pending, age, at);
            if (!keyIsDown)
            {
                rec.keyDown = false;
                voices[i].release();
            }
            return;
        }
        rec = {};
    }

    std::array<V, Pool> voices;
    std::array<Record, Pool> records{};
    Policy policy;
    std::size_t limit = maxPolyphony;
    std::size_t fadeSamples = 0;
    float thresholdLevel = 0.0f;
    std::uint64_t counter = 0;
    Stats statistics{};
    std::array<Freed, freeLogSize> log{};
    std::size_t logged = 0;
    std::array<std::array<float, checkSamples>, 2> scratch{};
};

} // namespace anoesis::test::allocation
