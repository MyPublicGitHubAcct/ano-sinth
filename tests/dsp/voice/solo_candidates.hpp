// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The part-solo and skipping candidates (M1.4 task 5, stage 1; docs/dsp/
// part-solo-and-skipping.md). Prototypes for measurement, not library code.
//
// SkipVoice is a copy of dsp::Voice as built (tasks 1-4), on copies of the
// filter and the slot that can skip (skip_ladder_filter.hpp,
// skip_oscillator_slot.hpp). With everything off it is dsp::Voice to the bit
// (the report checks it). What it adds, each switchable through SkipContext:
//
// - Solo (manual p. 9: "you will hear only the sound of the currently
//   selected part"): a global gain per part, 0 or 1, ramped over 10 ms or
//   instant, multiplied into the part's Mix gain; or a routing change (the
//   soloed-out filter as "--" at the voice's next grid point, from rest on
//   its return). Two readings: Solo mutes the other part's filter output
//   (filterOutput: Syntorial's "where you're actually soloing is the
//   filters output"), or also its oscillators where they cross into the
//   selected part's filter (partSources).
// - The skip test, per voice and per chunk (at most 32 samples, never
//   across a process() call, so it sees every event): can this part's
//   filter output be non-zero here? An upper bound on each factor of its
//   gain: the amp envelope (idle, or held at a sustain of 0), the Mix knob
//   ramp (its larger end) plus a bound on its modulation (a stand-in for
//   M1.5: a free LFO, or a pluck from each note-on), the solo ramp. If the
//   bound is 0 the output is exactly 0 and isn't computed.
// - What a silent output lets it skip (FilterSkip): nothing (never); the
//   filter only where its input is exact zeros and it is at rest (atRest:
//   exact by construction); or the filter whenever its output gain is 0,
//   its state kept (freeze) or cleared to rest when it comes back (clear).
//   A skipped filter's grid and ramps still advance (SkipLadderFilter::
//   skip), and it is still fed at its grid points.
// - Oscillators: a slot settled at Volume 0, and every slot of a part that
//   no running filter takes, skipped with their phases advanced (or frozen,
//   for contrast).
// - When the test is made: per chunk (where a process() call or the grid
//   splits the block, so where the host's blocks fall); per segment (a skip
//   starts only at the voice's grid points, over the whole segment, and a
//   call mid-segment can only end it, which only an event can make it do);
//   per segment with hysteresis (skipped for its gain only after that gain
//   has been 0 for a while); or once at the note-on.
// - The mono path (docs/dsp/voice-architecture.md, question 4): decided at
//   a note-on from silence when every slot is centred; left, at the first
//   chunk where any slot isn't, by setting each filter's channel 1 from its
//   channel 0; never re-entered within the note.
//
// SoloEngine runs the real dsp::VoiceAllocator over 16 SkipVoices, with the
// Mix A/B and solo ramps (global, 10 ms) and the events this task needs
// (Mix, Solo, Part select, a slot's Volume or Pan), splitting each host block
// at them.

#pragma once

#include "skip_ladder_filter.hpp"
#include "skip_oscillator_slot.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/pitch_drift.hpp>
#include <anoesis/dsp/voice/glide.hpp>
#include <anoesis/dsp/voice/mix_law.hpp>
#include <anoesis/dsp/voice/pitch_bend.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace anoesis::test::solo
{

namespace dsp = anoesis::dsp;

// --- The candidates' switches -------------------------------------------------------------

/// What a filter whose output can't be heard does.
enum class FilterSkip : std::uint8_t
{
    never,  ///< as built: a filter with an input runs
    atRest, ///< skipped while its input is exact zeros and it is at rest (exact)
    freeze, ///< also skipped while its output gain is 0; its state kept as it was
    clear,  ///< also skipped while its output gain is 0; cleared to rest when it comes back
};

/// When the skip test is made.
enum class SkipTiming : std::uint8_t
{
    perChunk,   ///< every chunk: at most 32 samples, split at every event and every host block
    perSegment, ///< a skip starts only at a grid point, for the segment; mid-segment it can only end
    hysteresis, ///< per segment, a filter skipped for its gain only after that many samples of 0
    noteOn,     ///< once, at the note-on: the gain's test kept for the note
};

/// What Solo silences.
enum class SoloReading : std::uint8_t
{
    filterOutput, ///< the other part's filter output (its part's Mix x solo)
    partSources,  ///< that, and its oscillators where they cross into the other filter
};

/// The stand-in for M1.5's modulation of Mix.
enum class ModulationKind : std::uint8_t
{
    none,
    lfo,   ///< amount x sin(2 pi rate t), t absolute: global
    pluck, ///< amount x e^(-n / tau), n from each note-on: per voice
};

/// How the skip test bounds the modulation over a chunk.
enum class BoundKind : std::uint8_t
{
    conservative, ///< the source's range: |amount| for the LFO, the chunk's first value for the pluck
    tight,        ///< the source's largest value over the chunk
};

struct MixModulation
{
    ModulationKind kind = ModulationKind::none;
    float amount = 0.0f; ///< internal Mix units
    float rateHz = 1.0f;
    float tauSeconds = 0.02f;
    std::array<bool, 2> parts{false, true}; ///< which parts' Mix it reaches
};

/// Engine-wide settings and global state each voice reads.
struct SkipContext
{
    FilterSkip filterSkip = FilterSkip::never;
    bool skipOscillators = false;
    bool advancePhase = true;
    SkipTiming timing = SkipTiming::perSegment;
    std::size_t hysteresisSamples = 128;
    bool mono = false;
    SoloReading soloReading = SoloReading::filterOutput;
    bool soloByRouting = false;
    std::array<const dsp::LinearSmoother*, 2> solo{}; ///< each part's solo gain (0-1), read like Mix; null: 1
    std::array<bool, 2> soloedOut{};                  ///< routing: the parts Solo removes
    MixModulation modulation{};
    BoundKind bound = BoundKind::conservative;
    float sampleRate = 48000.0f;
};

/// What a voice skipped, in samples.
struct SkipStats
{
    std::uint64_t filterSamples = 0; ///< samples of filters with an input (not "--", not bypassed)
    std::uint64_t filterSkipped = 0; ///< ... not solved
    std::uint64_t slotSamples = 0;   ///< samples of slots with voices sounding
    std::uint64_t slotSkipped = 0;   ///< ... not rendered
    std::uint64_t outputSamples = 0; ///< samples of filter outputs with an input
    std::uint64_t outputSkipped = 0; ///< ... not added (gain 0)
    std::uint64_t filterResumes = 0; ///< a filter skipped for its gain, then run
    std::uint64_t tests = 0;         ///< skip tests made (parts x chunks)
    std::uint64_t monoSamples = 0;   ///< samples rendered on the mono path
    std::uint64_t monoLeft = 0;      ///< notes that left the mono path

    void add(const SkipStats& o) noexcept
    {
        filterSamples += o.filterSamples;
        filterSkipped += o.filterSkipped;
        slotSamples += o.slotSamples;
        slotSkipped += o.slotSkipped;
        outputSamples += o.outputSamples;
        outputSkipped += o.outputSkipped;
        filterResumes += o.filterResumes;
        tests += o.tests;
        monoSamples += o.monoSamples;
        monoLeft += o.monoLeft;
    }
};

/// The modulation of part p's Mix at absolute sample `time`, `since` samples
/// after the voice's note-on.
[[nodiscard]] inline float modulationAt(const SkipContext& c, std::size_t part, std::uint64_t time,
                                        std::uint64_t since) noexcept
{
    const auto& m = c.modulation;
    if (!m.parts[part])
    {
        return 0.0f;
    }
    switch (m.kind)
    {
    case ModulationKind::none:
        return 0.0f;
    case ModulationKind::lfo:
        return m.amount * static_cast<float>(std::sin(2.0 * std::numbers::pi * static_cast<double>(m.rateHz) *
                                                      static_cast<double>(time) / static_cast<double>(c.sampleRate)));
    case ModulationKind::pluck:
        return m.amount * static_cast<float>(std::exp(-static_cast<double>(since) /
                                                      (static_cast<double>(m.tauSeconds) * c.sampleRate)));
    }
    return 0.0f;
}

/// An upper bound on modulationAt() over `length` samples from (time, since).
[[nodiscard]] inline float modulationBound(const SkipContext& c, std::size_t part, std::uint64_t time,
                                           std::uint64_t since, std::size_t length) noexcept
{
    const auto& m = c.modulation;
    if (!m.parts[part] || m.kind == ModulationKind::none || length == 0)
    {
        return 0.0f;
    }
    const auto last = length - 1;
    if (m.kind == ModulationKind::pluck)
    {
        // Monotonic from the note-on: the larger end.
        return std::max(modulationAt(c, part, time, since), modulationAt(c, part, time + last, since + last));
    }
    if (c.bound == BoundKind::conservative)
    {
        return std::abs(m.amount);
    }
    // Tight: the larger end, or a crest inside the chunk.
    const float ends = std::max(modulationAt(c, part, time, since), modulationAt(c, part, time + last, since + last));
    const double twoPi = 2.0 * std::numbers::pi;
    const double w = twoPi * m.rateHz / c.sampleRate;
    const double from = w * static_cast<double>(time);
    const double to = w * static_cast<double>(time + last);
    const double crest = m.amount >= 0.0f ? 0.25 : 0.75; // where amount x sin is largest, in cycles
    const double first = std::ceil((from / twoPi) - crest);
    const bool inside = ((first + crest) * twoPi) <= to;
    return inside ? std::abs(m.amount) : ends;
}

// --- The voice ----------------------------------------------------------------------------

template <std::size_t Taps = dsp::minBlepTaps>
class SkipVoice
{
public:
    using Slot = SkipSlot<Taps>;
    using Knobs = dsp::VoiceKnobs;
    using FilterInput = dsp::FilterInput;
    static constexpr std::size_t partCount = 2;
    static constexpr std::size_t slotsPerPart = 2;
    static constexpr std::size_t slotCount = partCount * slotsPerPart;
    static constexpr std::size_t gridSamples = SkipLadderFilter::gridSamples;

    /// The allocator constructs it as V{table, context, seed}.
    SkipVoice(const dsp::MinBlepTable& residuals, const SkipContext* settings, std::uint64_t seed = 0) noexcept
        : slots{Slot{residuals}, Slot{residuals}, Slot{residuals}, Slot{residuals}}, context(settings)
    {
        for (std::size_t s = 0; s < slotCount; ++s)
        {
            slots[s].setRandomSeed(seed ^ (0x9E3779B97F4A7C15U * (s + 1)));
        }
        pitchDrift.setSeed(seed ^ 0x3C6EF372FE94F82BU);
        updateRoutes();
    }

    [[nodiscard]] Slot& slot(std::size_t index) noexcept
    {
        return slots[index];
    }
    [[nodiscard]] dsp::Adsr& envelope(std::size_t part) noexcept
    {
        return envelopes[part];
    }
    [[nodiscard]] dsp::PitchDrift& drift() noexcept
    {
        return pitchDrift;
    }
    [[nodiscard]] const SkipLadderFilter& filter(std::size_t part) const noexcept
    {
        return filters[part];
    }
    void setFilterInput(std::size_t part, FilterInput input) noexcept
    {
        pendingInputs[part] = input;
    }
    void setFilterBypassed(std::size_t part, bool bypassed) noexcept
    {
        pendingBypass[part] = bypassed;
    }
    void setFilterResponse(std::size_t part, dsp::FilterResponse response) noexcept
    {
        filters[part].setResponse(response);
    }
    void setFilterDrive(std::size_t part, float gain) noexcept
    {
        filters[part].setDrive(gain);
    }
    void setWarm(bool on) noexcept
    {
        for (auto& f : filters)
        {
            f.setWarm(on);
        }
    }

    [[nodiscard]] const SkipStats& stats() const noexcept
    {
        return counters;
    }
    void clearStats() noexcept
    {
        counters = {};
    }
    [[nodiscard]] bool isMono() const noexcept
    {
        return mono;
    }

    void prepare(const dsp::ProcessSpec& spec) noexcept
    {
        const dsp::ProcessSpec stereo{
            .sampleRate = spec.sampleRate,
            .maxBlockSize = spec.maxBlockSize,
            .numChannels = 2,
        };
        for (auto& s : slots)
        {
            s.prepare(stereo);
            s.setAdvancePhase(context->advancePhase);
        }
        for (auto& f : filters)
        {
            f.prepare(stereo);
        }
        for (auto& e : envelopes)
        {
            e.prepare(spec);
        }
        pitchDrift.prepare(spec);
        glide.prepare(spec.sampleRate);
        velocityRamp.prepare(spec.sampleRate);
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : slots)
        {
            s.reset();
        }
        for (auto& e : envelopes)
        {
            e.reset();
        }
        pitchDrift.reset();
        applyPending();
        for (auto& f : filters)
        {
            f.reset();
        }
        gridPosition = 0;
        lastOctaves = {};
        lastK = {};
        glide.jump(dsp::referenceNote);
        velocityRamp.jump(1.0f);
        played = false;
        mono = false;
        sinceNoteOn = 0;
        skippedForGain = {};
        silentRun = {};
        segmentSilent = {};
        slotSkipping = {};
        counters = {};
    }

    void noteOn(float note, const Knobs& knobs, const dsp::NoteStart& start = {}) noexcept
    {
        const bool sounding = isActive();
        startPitch(note, start);
        if (glide.isGliding())
        {
            velocityRamp.startSamples(start.fromVelocity, start.velocity, glide.lengthSamples());
        }
        else
        {
            velocityRamp.jump(start.velocity);
        }
        played = true;
        sinceNoteOn = 0;
        for (auto& s : slots)
        {
            s.noteOn();
        }
        pitchDrift.noteOn();
        for (auto& e : envelopes)
        {
            e.gateOn();
        }
        decideAtNoteOn(knobs);
        if (sounding)
        {
            return;
        }
        applyPending();
        feedFilters(knobs, 0, keytrackPitch(knobs, 0, 0));
        for (auto& f : filters)
        {
            f.reset();
        }
        skippedForGain = {};
        silentRun = {};
        segmentSilent = {};
        slotSkipping = {};
        gridPosition = 0;
        mono = context->mono && allCentred();
    }

    void legatoNoteOn(float note, const Knobs& knobs, const dsp::NoteStart& start) noexcept
    {
        if (!isActive())
        {
            noteOn(note, knobs, start);
            return;
        }
        startPitch(note, start);
        velocityRamp.startSamples(velocityRamp.at(0), start.velocity, glide.lengthSamples());
        played = true;
        for (auto& e : envelopes)
        {
            e.legatoGateOn();
        }
    }

    void noteOff() noexcept
    {
        for (auto& e : envelopes)
        {
            e.gateOff();
        }
    }

    void stop() noexcept
    {
        for (auto& s : slots)
        {
            s.stop();
        }
        pitchDrift.stop();
        for (auto& e : envelopes)
        {
            e.reset();
        }
    }

    [[nodiscard]] float currentPitch() const noexcept
    {
        return glide.at(0);
    }
    [[nodiscard]] bool hasPlayed() const noexcept
    {
        return played;
    }
    [[nodiscard]] float velocity() const noexcept
    {
        return velocityRamp.at(0);
    }

    [[nodiscard]] bool isActive() const noexcept
    {
        for (std::size_t f = 0; f < partCount; ++f)
        {
            const bool fed = partSounds(f) || (routes[f].both && partSounds(1 - f));
            if (routes[f].active && fed && envelopes[f].isActive())
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::size_t activeOscillatorVoices() const noexcept
    {
        std::size_t n = 0;
        for (std::size_t p = 0; p < partCount; ++p)
        {
            if (heard[p] && envelopes[p].isActive())
            {
                n += partVoices(p);
            }
        }
        return n;
    }

    [[nodiscard]] std::size_t oscillatorVoicesPerNote() const noexcept
    {
        return partVoices(0) + partVoices(1);
    }

    void process(const dsp::AudioBlock& out, const Knobs& knobs) noexcept
    {
        const auto n = out.numSamples();
        std::size_t done = 0;
        while (done < n)
        {
            if (gridPosition == 0)
            {
                applyPendingAtGrid(knobs, done);
                feedFilters(knobs, done + gridSamples, keytrackPitch(knobs, done, gridSamples));
            }
            const auto length = std::min(n - done, gridSamples - gridPosition);
            renderChunk(out.subBlock(done, length), knobs, done);
            glide.advance(length);
            velocityRamp.advance(length);
            sinceNoteOn += length;
            done += length;
            gridPosition = (gridPosition + length) % gridSamples;
        }
    }

private:
    using Chunk = std::array<float, gridSamples>;
    using StereoChunk = std::array<Chunk, 2>;

    struct Route
    {
        bool active = false;
        bool both = false;
    };

    /// What one chunk runs.
    struct Plan
    {
        std::array<bool, partCount> gainSilent{}; ///< the part's output gain is 0 over the chunk
        std::array<bool, partCount> output{};     ///< the filter's output is added (its gain can be non-zero)
        std::array<bool, partCount> run{};        ///< the filter is solved
        std::array<bool, partCount> skip{};       ///< the filter is skipped (grid and ramps advanced)
        std::array<bool, partCount> render{};     ///< the part's slots are rendered (or skipped, if heard)
        std::array<bool, partCount> crossing{};   ///< the part is added into the other filter's input
    };

    void startPitch(float note, const dsp::NoteStart& start) noexcept
    {
        if (start.glide)
        {
            glide.start(start.from, note, start.glideSeconds);
        }
        else
        {
            glide.jump(note);
        }
    }

    [[nodiscard]] static float bendAt(const Knobs& knobs, std::size_t ahead) noexcept
    {
        return knobs.bend != nullptr ? knobs.bend->semitonesAt(knobs.time + ahead) : 0.0f;
    }
    [[nodiscard]] float pitchAt(const Knobs& knobs, std::size_t done, std::size_t ahead) const noexcept
    {
        return glide.at(ahead) + bendAt(knobs, done + ahead);
    }
    [[nodiscard]] float keytrackPitch(const Knobs& knobs, std::size_t done, std::size_t ahead) const noexcept
    {
        return pitchAt(knobs, done, ahead) + masterTuneOffset;
    }

    [[nodiscard]] std::size_t partVoices(std::size_t p) const noexcept
    {
        return slots[p * slotsPerPart].voices() + slots[(p * slotsPerPart) + 1].voices();
    }

    [[nodiscard]] bool partSounds(std::size_t p) const noexcept
    {
        for (std::size_t s = 0; s < slotsPerPart; ++s)
        {
            const auto& slot = slots[(p * slotsPerPart) + s];
            if (slot.voices() > 0 || slot.soundingVoices() > 0)
            {
                return true;
            }
        }
        return false;
    }

    /// Whether part p's slots can add anything this chunk.
    [[nodiscard]] bool partProduces(std::size_t p) const noexcept
    {
        for (std::size_t s = 0; s < slotsPerPart; ++s)
        {
            const auto& slot = slots[(p * slotsPerPart) + s];
            if (slot.soundingVoices() > 0 && !slot.isSilent())
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool allCentred() const noexcept
    {
        return std::ranges::all_of(slots, [](const Slot& s) { return s.soundingVoices() == 0 || s.isCentred(); });
    }

    void updateRoutes() noexcept
    {
        for (std::size_t f = 0; f < partCount; ++f)
        {
            routes[f] = {.active = inputs[f] != FilterInput::none, .both = inputs[f] == FilterInput::both};
        }
        if (context->soloByRouting)
        {
            for (std::size_t f = 0; f < partCount; ++f)
            {
                if (context->soloedOut[f])
                {
                    routes[f] = {};
                }
                else if (context->soloReading == SoloReading::partSources && context->soloedOut[1 - f])
                {
                    routes[f].both = false;
                }
            }
        }
        for (std::size_t p = 0; p < partCount; ++p)
        {
            heard[p] = routes[p].active || routes[1 - p].both;
        }
    }

    void applyPending() noexcept
    {
        inputs = pendingInputs;
        for (std::size_t f = 0; f < partCount; ++f)
        {
            filters[f].setBypassed(pendingBypass[f]);
        }
        updateRoutes();
    }

    void applyPendingAtGrid(const Knobs& knobs, std::size_t ahead) noexcept
    {
        std::array<bool, partCount> wasRunning{};
        for (std::size_t f = 0; f < partCount; ++f)
        {
            wasRunning[f] = routes[f].active && !filters[f].isBypassed();
        }
        applyPending();
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (routes[f].active && !filters[f].isBypassed() && !wasRunning[f])
            {
                feedFilter(f, knobs, ahead,
                           dsp::keytrackOctaves(keytrackPitch(knobs, ahead, 0), knobs.keytrack / 10.0f));
                filters[f].reset();
            }
        }
    }

    [[nodiscard]] static float read(const dsp::LinearSmoother* ramp, std::size_t ahead) noexcept
    {
        return ramp != nullptr ? ramp->valueAfter(ahead) : 0.0f;
    }

    void feedFilter(std::size_t f, const Knobs& knobs, std::size_t ahead, float keytrack) noexcept
    {
        ahead += knobs.offset;
        const float octaves = dsp::filterCutoffOctaves({
            .cutoff = read(knobs.cutoff[f], ahead),
            .controlCutoff = read(knobs.controlCutoff, ahead),
            .keytrack = keytrack,
        });
        const float k = dsp::filterResonanceK({
            .resonance = read(knobs.resonance[f], ahead),
            .controlResonance = read(knobs.controlResonance, ahead),
        });
        if (std::isfinite(octaves))
        {
            lastOctaves[f] = octaves;
        }
        if (std::isfinite(k))
        {
            lastK[f] = k;
        }
        filters[f].setCutoff(dsp::filterControlLaw.anchorHz);
        filters[f].setCutoffModulation(lastOctaves[f]);
        filters[f].setResonance(0.0f);
        filters[f].setResonanceModulation(lastK[f]);
    }

    void feedFilters(const Knobs& knobs, std::size_t ahead, float pitch) noexcept
    {
        const float keytrack = dsp::keytrackOctaves(pitch, knobs.keytrack / 10.0f);
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (routes[f].active && !filters[f].isBypassed())
            {
                feedFilter(f, knobs, ahead, keytrack);
            }
        }
    }

    [[nodiscard]] dsp::AudioBlock block(StereoChunk& chunk, std::array<float*, 2>& pointers, std::size_t length,
                                        std::size_t channels = 2) noexcept
    {
        pointers = {chunk[0].data(), chunk[1].data()};
        return {pointers.data(), channels, length};
    }

    // --- The skip test --------------------------------------------------------------------

    /// The amp envelope can't be above 0 in this chunk: idle, or held at a
    /// sustain of 0 (a gate or setSustain() only acts between process()
    /// calls).
    [[nodiscard]] bool envelopeSilent(std::size_t p) const noexcept
    {
        const auto& e = envelopes[p];
        const bool zeroHold = e.stage() == dsp::AdsrStage::sustain && e.level() <= 0.0f && e.sustainLevel() <= 0.0f;
        return !e.isActive() || zeroHold;
    }

    /// The upper bound of the solo ramp over the chunk is 0.
    [[nodiscard]] bool soloSilent(std::size_t p, std::size_t at, std::size_t length) const noexcept
    {
        const auto* ramp = context->solo[p];
        if (ramp == nullptr)
        {
            return false;
        }
        return std::max(ramp->valueAfter(at), ramp->valueAfter(at + length - 1)) <= 0.0f;
    }

    /// Mix's upper bound over the chunk, in internal units, is 0 or less:
    /// the ramp's larger end plus the modulation's bound.
    [[nodiscard]] bool mixSilent(std::size_t p, const Knobs& knobs, std::size_t done, std::size_t length) const noexcept
    {
        const auto* ramp = knobs.mix[p];
        if (ramp == nullptr)
        {
            return false;
        }
        const auto at = knobs.offset + done;
        const float knob = std::max(ramp->valueAfter(at), ramp->valueAfter(at + length - 1));
        const float modulation = modulationBound(*context, p, knobs.time + done, sinceNoteOn, length);
        return knob + modulation <= dsp::mixLaw.knobMin;
    }

    /// The gain's test at a note-on (SkipTiming::noteOn): the knobs and
    /// Solo at that sample, the modulation's range; the envelope can't be
    /// tested (it is attacking).
    void decideAtNoteOn(const Knobs& knobs) noexcept
    {
        for (std::size_t p = 0; p < partCount; ++p)
        {
            noteOnSilent[p] = mixSilent(p, knobs, 0, 1) || soloSilent(p, knobs.offset, 1);
        }
    }

    [[nodiscard]] Plan plan(const Knobs& knobs, std::size_t done, std::size_t length) noexcept
    {
        const bool skipping = context->filterSkip != FilterSkip::never;
        std::array<bool, partCount> gainSilent{};
        std::array<bool, partCount> produces{};
        for (std::size_t p = 0; p < partCount; ++p)
        {
            produces[p] = partProduces(p);
            if (!skipping && context->timing != SkipTiming::noteOn)
            {
                continue;
            }
            ++counters.tests;
            if (context->timing == SkipTiming::noteOn)
            {
                gainSilent[p] = noteOnSilent[p];
                continue;
            }
            if (context->timing == SkipTiming::perChunk)
            {
                gainSilent[p] = envelopeSilent(p) || mixSilent(p, knobs, done, length) ||
                                soloSilent(p, knobs.offset + done, length);
                continue;
            }
            // Per segment: tested over the whole segment at its grid point;
            // mid-segment (a process() call starting there: an event or a host
            // block) only re-tested while skipping, over what is left of it,
            // to end the skip. Without an event the rest's bound can't exceed
            // the whole segment's, so only an event ends it, and the output
            // doesn't depend on where the host's blocks fall.
            const std::size_t rest = gridSamples - gridPosition;
            if (gridPosition == 0)
            {
                const bool silent =
                    envelopeSilent(p) || mixSilent(p, knobs, done, rest) || soloSilent(p, knobs.offset + done, rest);
                if (context->timing == SkipTiming::hysteresis)
                {
                    segmentSilent[p] = silent && silentRun[p] >= context->hysteresisSamples;
                    silentRun[p] = silent ? silentRun[p] + gridSamples : 0;
                }
                else
                {
                    segmentSilent[p] = silent;
                }
            }
            else if (segmentSilent[p])
            {
                segmentSilent[p] =
                    envelopeSilent(p) || mixSilent(p, knobs, done, rest) || soloSilent(p, knobs.offset + done, rest);
                if (!segmentSilent[p])
                {
                    silentRun[p] = 0;
                }
            }
            gainSilent[p] = segmentSilent[p];
        }
        Plan plan;
        plan.gainSilent = gainSilent;
        const bool soloCrossing = context->soloReading == SoloReading::partSources && !context->soloByRouting;
        for (std::size_t q = 0; q < partCount; ++q)
        {
            // Part q into the other filter (AB): as built, or, when skipping,
            // only while it can add anything.
            const bool audible =
                produces[q] && !envelopeSilent(q) && !(soloCrossing && soloSilent(q, knobs.offset + done, length));
            plan.crossing[q] = routes[1 - q].both && (!skipping || audible);
        }
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (!routes[f].active)
            {
                continue;
            }
            plan.output[f] = !gainSilent[f];
            if (filters[f].isBypassed())
            {
                continue;
            }
            const bool inputSilent = !produces[f] && !plan.crossing[1 - f];
            bool skipFilter = false;
            switch (context->filterSkip)
            {
            case FilterSkip::never:
                skipFilter = false;
                break;
            case FilterSkip::atRest:
                skipFilter = inputSilent && filters[f].isAtRest();
                break;
            case FilterSkip::freeze:
            case FilterSkip::clear:
                skipFilter = (inputSilent && filters[f].isAtRest()) || gainSilent[f];
                break;
            }
            plan.run[f] = !skipFilter;
            plan.skip[f] = skipFilter;
        }
        // The slots: rendered for a running filter (or a bypassed one that is
        // heard), skipped otherwise (when allowed), as built when not. Part p
        // is taken by the other filter when it crosses into it: crossing[p].
        // (Stage 1 read crossing[other] here, so a part on AB whose own
        // filter didn't take it, filter B on "--" or soloed out, was skipped
        // and the other filter got zeros for it; stage 2 found and fixed it.)
        for (std::size_t p = 0; p < partCount; ++p)
        {
            const std::size_t other = 1 - p;
            const bool ownTakes = routes[p].active && (plan.run[p] || (filters[p].isBypassed() && plan.output[p]));
            const bool otherTakes =
                plan.crossing[p] && (plan.run[other] || (filters[other].isBypassed() && plan.output[other]));
            plan.render[p] = context->skipOscillators ? (ownTakes || otherTakes) : heard[p];
        }
        return plan;
    }

    /// Mono path: left at the first chunk where a slot isn't centred.
    void checkMono() noexcept
    {
        if (mono && !allCentred())
        {
            for (auto& f : filters)
            {
                f.copyChannel(0, 1);
            }
            mono = false;
            ++counters.monoLeft;
        }
    }

    void renderChunk(const dsp::AudioBlock& out, const Knobs& knobs, std::size_t done) noexcept
    {
        const auto length = out.numSamples();
        checkMono();
        const std::size_t channels = mono ? 1 : 2;
        if (mono)
        {
            counters.monoSamples += length;
        }
        const Plan p = plan(knobs, done, length);

        // 1. The oscillators.
        const bool moving =
            glide.isGliding() || (knobs.bend != nullptr && !knobs.bend->isSteadyFrom(knobs.time + done));
        const float steadyPitch = pitchAt(knobs, done, 0);
        if (moving)
        {
            for (std::size_t i = 0; i < length; ++i)
            {
                pitches[i] = pitchAt(knobs, done, i);
            }
        }
        std::array<dsp::AudioBlock, partCount> parts{};
        for (std::size_t q = 0; q < partCount; ++q)
        {
            parts[q] = block(partBuffers[q], partPointers[q], length);
            if (heard[q] && p.render[q])
            {
                parts[q].clear();
            }
        }
        // A slot's skip starts only at a grid point, under every timing, and
        // mid-segment can only end. (Stage 1 decided it per drift run within
        // the chunk; that only showed once stage 2 let a saw be skipped after
        // a Volume change while it sounded, when the skip's start, and so
        // the residual its phase's jump leaves at the return, followed the
        // host's blocks.)
        for (std::size_t q = 0; q < partCount; ++q)
        {
            for (std::size_t s = 0; s < slotsPerPart; ++s)
            {
                const auto index = (q * slotsPerPart) + s;
                const auto& slot = slots[index];
                const bool skippable = context->skipOscillators && (!p.render[q] || slot.isSilent()) && slot.canSkip();
                slotSkipping[index] = skippable && (gridPosition == 0 || slotSkipping[index]);
            }
        }
        pitchDrift.process(length,
                           [&](std::size_t start, std::size_t run, float offset) noexcept
                           {
                               if (moving)
                               {
                                   for (std::size_t i = 0; i < run; ++i)
                                   {
                                       notes[i] = pitches[start + i] + offset;
                                   }
                               }
                               for (std::size_t q = 0; q < partCount; ++q)
                               {
                                   if (!heard[q])
                                   {
                                       continue;
                                   }
                                   const auto piece = parts[q].subBlock(start, run);
                                   for (std::size_t s = 0; s < slotsPerPart; ++s)
                                   {
                                       auto& slot = slots[(q * slotsPerPart) + s];
                                       if (slot.soundingVoices() == 0)
                                       {
                                           continue;
                                       }
                                       counters.slotSamples += run;
                                       if (slotSkipping[(q * slotsPerPart) + s])
                                       {
                                           counters.slotSkipped += run;
                                           if (moving)
                                           {
                                               slot.skip(std::span<const float>{notes}.first(run));
                                           }
                                           else
                                           {
                                               slot.skip(run, steadyPitch + offset);
                                           }
                                           continue;
                                       }
                                       if (!p.render[q])
                                       {
                                           // Can't be skipped exactly (a ramp moving): rendered,
                                           // into a part buffer nobody reads this chunk.
                                           if (!rendered[q])
                                           {
                                               parts[q].clear();
                                               rendered[q] = true;
                                           }
                                       }
                                       if (moving)
                                       {
                                           slot.process(piece, std::span<const float>{notes}.first(run));
                                       }
                                       else
                                       {
                                           slot.process(piece, steadyPitch + offset);
                                       }
                                   }
                               }
                           });
        rendered = {};

        // 2. The envelopes (always), and each heard output's gains.
        for (std::size_t q = 0; q < partCount; ++q)
        {
            envelopes[q].process(std::span{envelopeBuffers[q]}.first(length));
            if (routes[q].active && p.output[q])
            {
                fillMixGains(q, knobs, done, length);
            }
        }
        const bool soloCrossing = context->soloReading == SoloReading::partSources && !context->soloByRouting;
        if (soloCrossing)
        {
            for (std::size_t q = 0; q < partCount; ++q)
            {
                fillSolo(soloGains[q], q, knobs.offset + done, length);
            }
        }

        // 3-5. Each filter.
        const auto left = out.channel(0);
        const auto right = out.channel(1);
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (!routes[f].active)
            {
                continue;
            }
            const bool bypassed = filters[f].isBypassed();
            if (!bypassed)
            {
                counters.filterSamples += length;
            }
            counters.outputSamples += length;
            const auto input = block(filterBuffers[f], filterPointers[f], length, channels);
            if (p.skip[f])
            {
                filters[f].skip(length);
                counters.filterSkipped += length;
                skippedForGain[f] = skippedForGain[f] || p.gainSilent[f];
            }
            else if (p.run[f] || bypassed)
            {
                if (p.run[f] && skippedForGain[f])
                {
                    ++counters.filterResumes;
                    if (context->filterSkip == FilterSkip::clear)
                    {
                        filters[f].clearState();
                    }
                }
                skippedForGain[f] = false;
                buildInput(f, length, channels, p.crossing[1 - f], soloCrossing);
                filters[f].process(input);
            }
            if (!p.output[f] || p.skip[f])
            {
                // Its gain is 0, or it is at rest on zeros: exact zeros.
                if (!p.output[f])
                {
                    counters.outputSkipped += length;
                }
                continue;
            }
            const auto& e = envelopeBuffers[f];
            const auto& g = mixGains[f];
            const auto y0 = input.channel(0);
            if (mono)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    // The stereo path's expression on each channel, so that a
                    // compiler that contracts it into a fused multiply-add
                    // (Apple Clang does) does so here too: a product rounded
                    // in its own statement differs by an ulp wherever ge isn't
                    // exactly representable times y.
                    const float ge = g[i] * e[i];
                    left[i] += ge * y0[i];
                    right[i] += ge * y0[i];
                }
            }
            else
            {
                const auto y1 = input.channel(1);
                for (std::size_t i = 0; i < length; ++i)
                {
                    const float ge = g[i] * e[i];
                    left[i] += ge * y0[i];
                    right[i] += ge * y1[i];
                }
            }
        }
    }

    void fillSolo(Chunk& out, std::size_t p, std::size_t at, std::size_t length) const noexcept
    {
        const auto* ramp = context->solo[p];
        if (ramp == nullptr)
        {
            std::fill_n(out.begin(), length, 1.0f);
            return;
        }
        for (std::size_t i = 0; i < length; ++i)
        {
            out[i] = ramp->valueAfter(at + i);
        }
    }

    /// As built, times the solo gain (gain modes), with the modulation
    /// stand-in.
    void fillMixGains(std::size_t p, const Knobs& knobs, std::size_t done, std::size_t length) noexcept
    {
        auto& g = mixGains[p];
        const auto* ramp = knobs.mix[p];
        const auto at = knobs.offset + done;
        const bool modulated = context->modulation.kind != ModulationKind::none && context->modulation.parts[p];
        if (ramp == nullptr && !modulated)
        {
            std::fill_n(g.begin(), length, dsp::mixGain({.mix = dsp::mixLaw.unity}));
        }
        else if (!modulated)
        {
            const float first = ramp->valueAfter(at);
            const float last = ramp->valueAfter(at + length);
            if (first <= last && first >= last)
            {
                std::fill_n(g.begin(), length, dsp::mixGain({.mix = first}));
            }
            else
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    g[i] = dsp::mixGain({.mix = ramp->valueAfter(at + i)});
                }
            }
        }
        else
        {
            for (std::size_t i = 0; i < length; ++i)
            {
                const float knob = ramp != nullptr ? ramp->valueAfter(at + i) : dsp::mixLaw.unity;
                g[i] = dsp::mixGain(
                    {.mix = knob, .modulation = modulationAt(*context, p, knobs.time + done + i, sinceNoteOn + i)});
            }
        }
        if (context->solo[p] != nullptr && !context->soloByRouting)
        {
            const auto* solo = context->solo[p];
            for (std::size_t i = 0; i < length; ++i)
            {
                g[i] *= solo->valueAfter(at + i);
            }
        }
    }

    void buildInput(std::size_t f, std::size_t length, std::size_t channels, bool crossing, bool soloCrossing) noexcept
    {
        const std::size_t other = 1 - f;
        const auto& envelopeOther = envelopeBuffers[other];
        const auto& soloOther = soloGains[other];
        for (std::size_t c = 0; c < channels; ++c)
        {
            const auto& own = partBuffers[f][c];
            const auto& cross = partBuffers[other][c];
            auto& dst = filterBuffers[f][c];
            if (routes[f].both && crossing)
            {
                if (soloCrossing)
                {
                    for (std::size_t i = 0; i < length; ++i)
                    {
                        const float gain = envelopeOther[i] * soloOther[i];
                        const float enveloped = gain * cross[i];
                        dst[i] = own[i] + enveloped;
                    }
                }
                else
                {
                    for (std::size_t i = 0; i < length; ++i)
                    {
                        const float enveloped = envelopeOther[i] * cross[i];
                        dst[i] = own[i] + enveloped;
                    }
                }
            }
            else
            {
                std::copy_n(own.begin(), length, dst.begin());
            }
        }
    }

    std::array<Slot, slotCount> slots;
    std::array<SkipLadderFilter, partCount> filters{};
    std::array<dsp::Adsr, partCount> envelopes{};
    dsp::PitchDrift pitchDrift;
    const SkipContext* context;

    std::array<FilterInput, partCount> pendingInputs{FilterInput::own, FilterInput::own};
    std::array<bool, partCount> pendingBypass{};
    std::array<FilterInput, partCount> inputs{FilterInput::own, FilterInput::own};
    std::array<Route, partCount> routes{};
    std::array<bool, partCount> heard{};

    std::array<StereoChunk, partCount> partBuffers{};
    std::array<StereoChunk, partCount> filterBuffers{};
    std::array<Chunk, partCount> envelopeBuffers{};
    std::array<Chunk, partCount> mixGains{};
    std::array<Chunk, partCount> soloGains{};
    std::array<std::array<float*, 2>, partCount> partPointers{};
    std::array<std::array<float*, 2>, partCount> filterPointers{};

    std::array<float, partCount> lastOctaves{};
    std::array<float, partCount> lastK{};
    dsp::Glide glide;
    dsp::Glide velocityRamp;
    Chunk pitches{};
    Chunk notes{};
    float masterTuneOffset = 0.0f;
    bool played = false;
    std::size_t gridPosition = 0;

    bool mono = false;
    std::uint64_t sinceNoteOn = 0;
    std::array<bool, partCount> skippedForGain{};
    std::array<std::size_t, partCount> silentRun{};
    std::array<bool, partCount> segmentSilent{};
    std::array<bool, partCount> noteOnSilent{};
    std::array<bool, partCount> rendered{};
    std::array<bool, slotCount> slotSkipping{};
    SkipStats counters;
};

// --- The engine ---------------------------------------------------------------------------

/// The filter knobs every voice reads: Cutoff 5 + the Filter Control's 5
/// (4,677 Hz), Resonance 0, unless set; constant.
struct FilterKnobs
{
    FilterKnobs()
    {
        set(5.0f, 0.0f);
    }
    void set(float cutoffKnob, float resonanceKnob)
    {
        cutoff.setCurrentAndTarget(cutoffKnob);
        control.setCurrentAndTarget(5.0f);
        resonance.setCurrentAndTarget(resonanceKnob);
        zero.setCurrentAndTarget(0.0f);
    }
    [[nodiscard]] dsp::VoiceKnobs knobs() const noexcept
    {
        return {
            .cutoff = {&cutoff, &cutoff},
            .resonance = {&resonance, &resonance},
            .controlCutoff = &control,
            .controlResonance = &zero,
        };
    }
    dsp::LinearSmoother cutoff;
    dsp::LinearSmoother control;
    dsp::LinearSmoother resonance;
    dsp::LinearSmoother zero;
};

enum class EngineEventKind : std::uint8_t
{
    voice,      ///< a VoiceEvent for the allocator
    mix,        ///< Mix A (part 0) or B (1), internal units
    solo,       ///< Solo off (0) or on
    select,     ///< Part select: part 0 or 1
    slotVolume, ///< slot `part` (0-3): its Volume, linear, on every voice
    slotPan,    ///< slot `part` (0-3): its Pan, -1 to 1, on every voice
};

struct EngineEvent
{
    std::size_t offset = 0;
    EngineEventKind kind = EngineEventKind::voice;
    dsp::VoiceEvent voice{};
    std::size_t part = 0;
    float value = 0.0f;
};

/// The candidates around the real VoiceAllocator: 16 SkipVoices, the Mix A/B
/// ramps (10 ms) and the solo gains (10 ms, or none for instant Solo), Solo
/// and Part select. Each host block is split at its non-note events. The
/// output is written, not added to.
class SoloEngine
{
public:
    using Voice = SkipVoice<>;
    using Allocator = dsp::VoiceAllocator<Voice, 16>;
    static constexpr float rampSeconds = 0.01f;

    SoloEngine(const dsp::MinBlepTable& table, const SkipContext& settings, std::uint64_t seed = 1) : context(settings)
    {
        if (!context.soloByRouting)
        {
            context.solo = {soloRamps.data(), soloRamps.data() + 1};
        }
        allocator = std::make_unique<Allocator>(seed, table, &context);
        for (auto& r : mixRamps)
        {
            r.setRampTime(rampSeconds);
            r.setCurrentAndTarget(10.0f);
        }
        for (auto& r : soloRamps)
        {
            r.setRampTime(rampSeconds);
            r.setCurrentAndTarget(1.0f);
        }
    }

    [[nodiscard]] Allocator& voices() noexcept
    {
        return *allocator;
    }
    [[nodiscard]] FilterKnobs& filterKnobs() noexcept
    {
        return filters;
    }
    [[nodiscard]] SkipContext& settings() noexcept
    {
        return context;
    }
    /// Solo's ramp: 10 ms (true) or none, for the next prepare().
    void setSoloSmoothing(bool on) noexcept
    {
        soloRampSeconds = on ? rampSeconds : 0.0f;
    }
    void setMix(std::size_t part, float internal) noexcept
    {
        mixRamps[part].setCurrentAndTarget(internal);
    }
    void setSolo(bool on, std::size_t selected) noexcept
    {
        soloOn = on;
        selectedPart = selected;
        updateSolo(true);
    }
    /// What every voice skipped.
    [[nodiscard]] SkipStats stats() const noexcept
    {
        SkipStats s;
        for (std::size_t v = 0; v < 16; ++v)
        {
            s.add(allocator->voice(v).stats());
        }
        return s;
    }

    void prepare(const dsp::ProcessSpec& spec)
    {
        context.sampleRate = spec.sampleRate;
        for (auto& r : mixRamps)
        {
            r.prepare(spec);
        }
        for (auto& r : soloRamps)
        {
            r.prepare(spec);
            r.setRampTime(soloRampSeconds);
        }
        allocator->prepare({.sampleRate = spec.sampleRate, .maxBlockSize = spec.maxBlockSize, .numChannels = 2});
        notes.assign(256, {});
        clock = 0;
    }

    void process(const dsp::AudioBlock& out, std::span<const EngineEvent> events) noexcept
    {
        const auto n = out.numSamples();
        out.clear();
        std::size_t from = 0;
        std::size_t e = 0;
        while (from < n)
        {
            std::size_t count = 0;
            std::size_t to = n;
            while (e < events.size())
            {
                const auto at = std::min(events[e].offset, n);
                if (events[e].kind != EngineEventKind::voice)
                {
                    if (at > from)
                    {
                        to = at;
                        break;
                    }
                    apply(events[e]);
                }
                else if (count < notes.size())
                {
                    notes[count] = events[e].voice;
                    notes[count].offset = at - from;
                    ++count;
                }
                ++e;
            }
            render(out.subBlock(from, to - from), std::span{notes}.first(count));
            from = to;
        }
        std::size_t count = 0;
        for (; e < events.size(); ++e)
        {
            if (events[e].kind != EngineEventKind::voice)
            {
                apply(events[e]);
            }
            else if (count < notes.size())
            {
                notes[count] = events[e].voice;
                notes[count].offset = 0;
                ++count;
            }
        }
        if (count > 0)
        {
            allocator->process(out.subBlock(n, 0), std::span{notes}.first(count), knobs());
        }
    }

private:
    [[nodiscard]] dsp::VoiceKnobs knobs() const noexcept
    {
        auto k = filters.knobs();
        k.mix = {mixRamps.data(), mixRamps.data() + 1};
        k.time = clock;
        return k;
    }

    void updateSolo(bool immediately) noexcept
    {
        for (std::size_t p = 0; p < 2; ++p)
        {
            const bool out = soloOn && p != selectedPart;
            context.soloedOut[p] = out;
            if (immediately)
            {
                soloRamps[p].setCurrentAndTarget(out ? 0.0f : 1.0f);
            }
            else
            {
                soloRamps[p].setTarget(out ? 0.0f : 1.0f);
            }
        }
    }

    void apply(const EngineEvent& event) noexcept
    {
        switch (event.kind)
        {
        case EngineEventKind::voice:
            break;
        case EngineEventKind::mix:
            mixRamps[std::min<std::size_t>(event.part, 1)].setTarget(event.value);
            break;
        case EngineEventKind::solo:
            soloOn = event.value > 0.5f;
            updateSolo(false);
            break;
        case EngineEventKind::select:
            selectedPart = std::min<std::size_t>(event.part, 1);
            updateSolo(false);
            break;
        case EngineEventKind::slotVolume:
            allocator->forEachVoice([&](Voice& v) { v.slot(event.part).setVolume(event.value); });
            break;
        case EngineEventKind::slotPan:
            allocator->forEachVoice([&](Voice& v) { v.slot(event.part).setPan(event.value); });
            break;
        }
    }

    void render(const dsp::AudioBlock& piece, std::span<const dsp::VoiceEvent> pieceNotes) noexcept
    {
        const auto length = piece.numSamples();
        allocator->process(piece, pieceNotes, knobs());
        for (auto& r : mixRamps)
        {
            r.skip(length);
        }
        for (auto& r : soloRamps)
        {
            r.skip(length);
        }
        clock += length;
    }

    SkipContext context;
    std::array<dsp::LinearSmoother, 2> mixRamps{};
    std::array<dsp::LinearSmoother, 2> soloRamps{};
    float soloRampSeconds = rampSeconds;
    bool soloOn = false;
    std::size_t selectedPart = 0;
    FilterKnobs filters;
    std::unique_ptr<Allocator> allocator;
    std::vector<dsp::VoiceEvent> notes;
    std::uint64_t clock = 0;
};

} // namespace anoesis::test::solo
