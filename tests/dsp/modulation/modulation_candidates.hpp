// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The modulation candidates (M1.5 task 2, stage 1; docs/dsp/modulation.md).
// Prototypes for measurement, not library code.
//
// ModVoice is a copy of dsp::Voice as built (M1.4 tasks 1-5) on the slot
// that can be modulated (mod_oscillator_slot.hpp), with what the model
// needs. With no routes it is dsp::Voice to the bit (the report checks it).
// What it adds:
//
// - The sources: two mod envelopes (Adsr) and two LFOs (Lfo) per voice, as
//   task 3 will build them (an LFO restarted at the note-on from silence and
//   retriggered on a sounding voice; Free isn't modelled), plus velocity,
//   keytrack, the amp envelopes, and three global stand-ins (ModWheel and
//   Aftertouch as ControlRamps read at absolute samples, Step Velocity a
//   constant). An LFO is the Lfo's wave at gain 1 and offset 0, scaled
//   outside: 0.5 x gain x (wave + offset) (M6), so its Gain can be
//   modulated without the Lfo's own 10 ms gain ramp.
// - The routes: the patch's 16 slots compiled with registry (a)
//   (registry_candidates.hpp) whenever the patch changes; an empty slot or
//   an amount of 0 drops out. The voice reads the sums per target and part.
// - A rate per target (ModPatch::rates): per sample; at the voice's grid
//   points, held (the sources as they stand there); or ramped (from the
//   previous grid point's sum to this one's, over the segment). Cutoff and
//   Reso are fed at the grid points as M1.3 decided (setCutoffModulation(),
//   setResonanceModulation()), with the sources as they stand there, or
//   (cutoffAtSegmentEnd) with the LFOs' value at the segment's end. Mix is
//   per sample, as M1.4 decided.
// - The LFOs' order (deliverable 4): with LFO Rate and Gain on the grid,
//   each LFO's rate and gain for a segment are set at its grid point from
//   the sources as they stand there (LFOs: their last output), and its wave
//   is rendered for the whole segment ahead (a delay of one segment through
//   a cycle); a retrigger mid-segment re-renders the rest from a copy taken
//   at the grid point. With either per sample, the LFOs run sample by
//   sample, each sample's rate and gain from the sources at the sample
//   before (a delay of one sample).
// - The skip test's bounds (deliverable 6): each route to Mix adds
//   amount x its source's bound over the segment (mixModulationBound()),
//   and a slot at Volume 0 isn't silent while its Volume routes can lift it.
//   The bounds: each source's range, or tight (an envelope by stage, an LFO
//   from its segment rendered ahead, a linear source its ends).
//
// ModEngine runs the real dsp::VoiceAllocator over 16 ModVoices, with the
// global ramps and the patch's events (ModWheel). GlobalEngine (deliverable
// 5) adds a stand-in global destination fed from the voices by each rule.

#pragma once

#include "mod_oscillator_slot.hpp"
#include "modulation_model.hpp"
#include "registry_candidates.hpp"
#include "source_bounds.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
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
#include <span>
#include <vector>

namespace anoesis::test::modulation
{

using dsp::Adsr;
using dsp::FilterInput;
using dsp::LadderFilter;
using dsp::Lfo;
using dsp::NoteStart;
using dsp::VoiceKnobs;

using SourceId = table_registry::Source;
using DestinationId = table_registry::Destination;

[[nodiscard]] constexpr std::uint8_t id(SourceId s) noexcept
{
    return static_cast<std::uint8_t>(s);
}
[[nodiscard]] constexpr std::uint8_t id(DestinationId d) noexcept
{
    return static_cast<std::uint8_t>(d);
}
[[nodiscard]] constexpr std::size_t index(Target t) noexcept
{
    return static_cast<std::size_t>(t);
}

/// The LFO Rate knob's law (inventory L6, a placeholder): 0.1 Hz x 1000^(x /
/// 10) for x in [0, 10], about an octave per unit.
[[nodiscard]] inline float lfoRateHz(float units) noexcept
{
    const float x = std::clamp(units, 0.0f, 10.0f);
    return 0.1f * std::exp2(x * (9.965784285f / 10.0f)); // log2(1000) = 9.9658
}

/// A global controller (ModWheel, Aftertouch) for the prototype: steps at
/// absolute samples, each ramped linearly over `rampSamples` (0: a step).
/// Read by every voice at its own absolute samples, as PitchBend is. A
/// stand-in for task 3's, whose smoothing of 7-bit steps (M13) is open.
class ControlRamp
{
public:
    void set(std::uint64_t time, float value, std::uint64_t rampSamples = 0) noexcept
    {
        from = valueAt(time);
        to = value;
        start = time;
        length = rampSamples;
    }
    [[nodiscard]] float valueAt(std::uint64_t time) const noexcept
    {
        if (time <= start)
        {
            return time == start && length == 0 ? to : from;
        }
        const std::uint64_t elapsed = time - start;
        if (elapsed >= length)
        {
            return to;
        }
        const float t = static_cast<float>(elapsed) / static_cast<float>(length);
        return from + ((to - from) * t);
    }

private:
    float from = 0.0f;
    float to = 0.0f;
    std::uint64_t start = 0;
    std::uint64_t length = 0;
};

struct EnvelopeSettings
{
    float attack = 0.0f; ///< seconds
    float decay = 0.0f;
    float sustain = 1.0f; ///< 0-1
    float release = 0.0f;
};

struct LfoSettings
{
    dsp::LfoWaveform waveform = dsp::LfoWaveform::sine;
    float rate = 5.0f;   ///< the Rate knob, 0-10 (lfoRateHz)
    float gain = 10.0f;  ///< the Gain knob, 0-10
    float offset = 0.0f; ///< the Offset knob, -10 to +10
};

/// Which bound the skip test takes for each source.
enum class BoundMode : std::uint8_t
{
    range, ///< each source's range (sourceRows)
    tight, ///< by stage, from the segment rendered ahead, or the ends
};

/// The rate per target, from the rows' recommendations.
[[nodiscard]] constexpr std::array<EvalRate, targetCount> defaultRates() noexcept
{
    std::array<EvalRate, targetCount> rates{};
    for (const auto& row : destinationRows)
    {
        rates[index(row.target)] = row.rate;
    }
    rates[index(Target::mix)] = EvalRate::perSample;
    return rates;
}

/// The patch's modulation: the 16 slots and the sources' settings.
struct ModPatch
{
    std::array<Route, routeCount> routes{};
    std::array<EvalRate, targetCount> rates = defaultRates();
    std::array<EnvelopeSettings, 2> modEnvelopes{};
    std::array<LfoSettings, 2> lfos{};
    float stepVelocity = 0.0f;
    BoundMode bounds = BoundMode::tight;
    /// Cutoff and Reso fed with the LFOs' value at the segment's end (they
    /// are rendered ahead) rather than as they stand at the grid point.
    bool cutoffAtSegmentEnd = false;
    /// Bumped by every change, so voices recompile at their next grid point.
    std::uint32_t version = 0;

    /// Slot `slot` (0-15; 0-1 ModEnv 1, 2-3 ModEnv 2, 4-5 LFO 1, 6-7 LFO 2,
    /// 8-15 the misc slots).
    void route(std::size_t slot, SourceId source, DestinationId destination, float amount) noexcept
    {
        routes[slot] = {.source = id(source), .destination = id(destination), .amount = amount};
        ++version;
    }
};

/// What every voice reads that isn't its own (constructor argument, so it
/// passes through VoiceAllocator).
struct ModContext
{
    const ModPatch* patch = nullptr;
    const ControlRamp* modWheel = nullptr;
    const ControlRamp* aftertouch = nullptr;
};

template <std::size_t Taps = dsp::minBlepTaps>
class ModVoice
{
public:
    using Slot = ModOscillatorSlot<Taps>;
    using Knobs = VoiceKnobs;
    static constexpr std::size_t partCount = 2;
    static constexpr std::size_t slotsPerPart = 2;
    static constexpr std::size_t slotCount = partCount * slotsPerPart;
    static constexpr std::size_t gridSamples = LadderFilter::gridSamples;
    static constexpr float minInputLevel = 1e-3f;
    static constexpr float maxInputLevel = 1e3f;

    ModVoice(const MinBlepTable& residuals, const ModContext* modContext, std::uint64_t seed = 0) noexcept
        : slots{Slot{residuals}, Slot{residuals}, Slot{residuals}, Slot{residuals}}, context(modContext)
    {
        for (std::size_t s = 0; s < slotCount; ++s)
        {
            slots[s].setRandomSeed(seed ^ (0x9E3779B97F4A7C15U * (s + 1)));
        }
        pitchDrift.setSeed(seed ^ 0x3C6EF372FE94F82BU);
        for (std::size_t k = 0; k < 2; ++k)
        {
            lfos[k].setSeed(seed ^ (0xA54FF53A5F1D36F1U * (k + 1)));
        }
        updateRoutes();
    }

    [[nodiscard]] Slot& slot(std::size_t index) noexcept
    {
        return slots[index];
    }
    [[nodiscard]] const Slot& slot(std::size_t index) const noexcept
    {
        return slots[index];
    }
    [[nodiscard]] Adsr& envelope(std::size_t part) noexcept
    {
        return envelopes[part];
    }
    [[nodiscard]] dsp::PitchDrift& drift() noexcept
    {
        return pitchDrift;
    }
    [[nodiscard]] const LadderFilter& filter(std::size_t part) const noexcept
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
    [[nodiscard]] bool isMono() const noexcept
    {
        return mono;
    }

    // --- Modulation, for measurement ------------------------------------------------------

    /// A source's value as it stands (the last sample rendered).
    [[nodiscard]] float sourceNow(SourceId s) const noexcept
    {
        switch (s)
        {
        case SourceId::velocity:
            return velocityRamp.at(0);
        case SourceId::ampEnvA:
            return envelopes[0].level();
        case SourceId::ampEnvB:
            return envelopes[1].level();
        case SourceId::modEnv1:
            return modEnvelopes[0].level();
        case SourceId::modEnv2:
            return modEnvelopes[1].level();
        case SourceId::lfo1:
            return lfoLast[0];
        case SourceId::lfo2:
            return lfoLast[1];
        default:
            return 0.0f;
        }
    }
    [[nodiscard]] const Adsr& modEnvelope(std::size_t k) const noexcept
    {
        return modEnvelopes[k];
    }
    [[nodiscard]] const Lfo& lfo(std::size_t k) const noexcept
    {
        return lfos[k];
    }
    /// The absolute sample of the latest note-on.
    [[nodiscard]] std::uint64_t noteOnTime() const noexcept
    {
        return startTime;
    }
    /// Segments whose Mix output was skipped for its gain (the bound at 0).
    [[nodiscard]] std::uint64_t skippedOutputs() const noexcept
    {
        return outputSkips;
    }
    [[nodiscard]] std::uint64_t testedOutputs() const noexcept
    {
        return outputTests;
    }
    /// Chunks in which a slot at Volume 0 was rendered because its Volume
    /// routes could lift it.
    [[nodiscard]] std::uint64_t liftedSlots() const noexcept
    {
        return lifted;
    }

    void prepare(const ProcessSpec& spec) noexcept
    {
        const ProcessSpec stereo{
            .sampleRate = spec.sampleRate,
            .maxBlockSize = spec.maxBlockSize,
            .numChannels = 2,
        };
        for (auto& s : slots)
        {
            s.prepare(stereo);
        }
        for (auto& f : filters)
        {
            f.prepare(stereo);
        }
        for (auto& e : envelopes)
        {
            e.prepare(spec);
        }
        for (auto& e : modEnvelopes)
        {
            e.prepare(spec);
        }
        for (auto& l : lfos)
        {
            l.prepare(spec);
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
        for (auto& e : modEnvelopes)
        {
            e.reset();
        }
        for (auto& l : lfos)
        {
            l.reset();
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
        segmentSilent = {};
        slotSkipping = {};
        resetModulation();
    }

    void noteOn(float note, const VoiceKnobs& knobs, const NoteStart& start = {}) noexcept
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
        startTime = knobs.time;
        for (auto& s : slots)
        {
            s.noteOn();
        }
        pitchDrift.noteOn();
        for (auto& e : envelopes)
        {
            e.gateOn();
        }
        applyEnvelopeSettings();
        for (auto& e : modEnvelopes)
        {
            e.gateOn();
        }
        if (sounding)
        {
            retriggerLfos();
            return;
        }
        for (auto& l : lfos)
        {
            l.restart();
        }
        lfoLast = {};
        applyPending();
        gridPosition = 0;
        segmentBegun = false;
        beginSegment(knobs, 0, true);
        feedFilters(knobs, 0, keytrackPitch(knobs, 0, 0));
        for (auto& f : filters)
        {
            f.reset();
        }
        segmentSilent = {};
        slotSkipping = {};
        mono = allCentred();
    }

    void legatoNoteOn(float note, const VoiceKnobs& knobs, const NoteStart& start) noexcept
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
        for (auto& e : modEnvelopes)
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
        for (auto& e : modEnvelopes)
        {
            e.gateOff();
        }
    }

    void setMasterTune(float referenceHz) noexcept
    {
        const bool valid = std::isfinite(referenceHz) && referenceHz > 0.0f;
        masterTuneHz = valid ? referenceHz : dsp::concertPitchHz;
        masterTuneOffset = dsp::masterTuneSemitones(masterTuneHz);
        for (auto& s : slots)
        {
            s.setReferenceHz(masterTuneHz);
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
        for (auto& e : modEnvelopes)
        {
            e.reset();
        }
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

    void process(const AudioBlock& out, const VoiceKnobs& knobs) noexcept
    {
        const auto n = out.numSamples();
        std::size_t done = 0;
        while (done < n)
        {
            if (gridPosition == 0)
            {
                if (!segmentBegun)
                {
                    beginSegment(knobs, done, false);
                }
                applyPendingAtGrid(knobs, done);
                feedFilters(knobs, done + gridSamples, keytrackPitch(knobs, done, gridSamples));
            }
            const auto length = std::min(n - done, gridSamples - gridPosition);
            renderChunk(out.subBlock(done, length), knobs, done);
            glide.advance(length);
            velocityRamp.advance(length);
            done += length;
            gridPosition = (gridPosition + length) % gridSamples;
            if (gridPosition == 0)
            {
                segmentBegun = false;
            }
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

    struct Plan
    {
        std::array<bool, partCount> output{};
        std::array<bool, partCount> skip{};
        std::array<bool, partCount> render{};
        std::array<bool, partCount> crossing{};
    };

    // --- The modulation (new) --------------------------------------------------------------

    [[nodiscard]] const ModPatch& patch() const noexcept
    {
        static const ModPatch empty{};
        return context != nullptr && context->patch != nullptr ? *context->patch : empty;
    }

    [[nodiscard]] EvalRate rateOf(Target t) const noexcept
    {
        return patch().rates[index(t)];
    }

    [[nodiscard]] bool routedTo(Target t, std::size_t part) const noexcept
    {
        return routed[index(t)][part];
    }

    /// The LFOs run sample by sample when their Rate or Gain is per sample.
    [[nodiscard]] bool lfosPerSample() const noexcept
    {
        return rateOf(Target::lfoRate) == EvalRate::perSample || rateOf(Target::lfoGain) == EvalRate::perSample;
    }

    void resetModulation() noexcept
    {
        compiledVersion = ~std::uint32_t{0};
        compiled = {};
        routed = {};
        sourceUsed = {};
        sourcePerSample = {};
        gridSums = {};
        previousSums = {};
        endSums = {};
        lfoLast = {};
        lfoGainNow = {};
        lfoGainBefore = {};
        segmentBegun = false;
        previousValues = {};
        outputSkips = 0;
        outputTests = 0;
        lifted = 0;
    }

    void applyEnvelopeSettings() noexcept
    {
        const auto& settings = patch().modEnvelopes;
        for (std::size_t k = 0; k < 2; ++k)
        {
            modEnvelopes[k].setAttack(settings[k].attack);
            modEnvelopes[k].setDecay(settings[k].decay);
            modEnvelopes[k].setSustain(settings[k].sustain);
            modEnvelopes[k].setRelease(settings[k].release);
        }
    }

    /// The patch's routes into the list the voice evaluates, if it changed.
    void compileIfChanged() noexcept
    {
        const auto& p = patch();
        if (p.version == compiledVersion)
        {
            return;
        }
        compiledVersion = p.version;
        compiled = table_registry::compile(p.routes);
        routed = {};
        sourceUsed = {};
        sourcePerSample = {};
        const bool lfoSampleMode = lfosPerSample();
        for (std::size_t i = 0; i < compiled.count; ++i)
        {
            const auto& r = compiled.routes[i];
            for (std::size_t q = 0; q < 2; ++q)
            {
                if ((r.parts & (1U << q)) != 0)
                {
                    routed[index(r.target)][q] = true;
                }
            }
            sourceUsed[r.source] = true;
            const bool lfoTarget = r.target == Target::lfoRate || r.target == Target::lfoGain;
            if (p.rates[index(r.target)] == EvalRate::perSample || (lfoTarget && lfoSampleMode))
            {
                sourcePerSample[r.source] = true;
            }
        }
        // An LFO that modulates anything per sample is read per sample.
        for (std::size_t k = 0; k < 2; ++k)
        {
            const auto lfoId = static_cast<std::size_t>(k == 0 ? SourceId::lfo1 : SourceId::lfo2);
            if (sourceUsed[lfoId])
            {
                lfoNeeded[k] = true;
            }
            else
            {
                lfoNeeded[k] = false;
            }
        }
    }

    /// Each source as it stands at the block's sample `done` (envelopes and
    /// LFOs: their last output).
    void valuesNow(const VoiceKnobs& knobs, std::size_t done, std::array<float, sourceCount>& v) const noexcept
    {
        v = {};
        v[id(SourceId::velocity)] = velocityRamp.at(0);
        v[id(SourceId::keytrack)] = dsp::keytrackSource(keytrackPitch(knobs, done, 0));
        v[id(SourceId::ampEnvA)] = envelopes[0].level();
        v[id(SourceId::ampEnvB)] = envelopes[1].level();
        v[id(SourceId::modEnv1)] = modEnvelopes[0].level();
        v[id(SourceId::modEnv2)] = modEnvelopes[1].level();
        v[id(SourceId::lfo1)] = lfoLast[0];
        v[id(SourceId::lfo2)] = lfoLast[1];
        const auto t = knobs.time + done;
        v[id(SourceId::modWheel)] =
            context != nullptr && context->modWheel != nullptr ? context->modWheel->valueAt(t) : 0.0f;
        v[id(SourceId::aftertouch)] =
            context != nullptr && context->aftertouch != nullptr ? context->aftertouch->valueAt(t) : 0.0f;
        v[id(SourceId::stepVelocity)] = patch().stepVelocity;
    }

    [[nodiscard]] float lfoGainFrom(std::size_t k, const ModSums& sums) const noexcept
    {
        const float units =
            patch().lfos[k].gain + (routedTo(Target::lfoGain, k) ? sums[index(Target::lfoGain)][k] : 0.0f);
        return std::clamp(units, 0.0f, 10.0f) / 10.0f;
    }

    [[nodiscard]] float lfoRateFrom(std::size_t k, const ModSums& sums) const noexcept
    {
        const float units =
            patch().lfos[k].rate + (routedTo(Target::lfoRate, k) ? sums[index(Target::lfoRate)][k] : 0.0f);
        return lfoRateHz(units);
    }

    [[nodiscard]] float lfoOffset(std::size_t k) const noexcept
    {
        return std::clamp(patch().lfos[k].offset, -10.0f, 10.0f) / 10.0f;
    }

    /// At a grid point (or a note-on from silence): recompile if needed; in
    /// segment order, each LFO's rate and gain from the sources as they
    /// stand, and its wave rendered for the whole segment; the grid sums.
    void beginSegment(const VoiceKnobs& knobs, std::size_t done, bool atNoteOn) noexcept
    {
        compileIfChanged();
        std::array<float, sourceCount> now{};
        valuesNow(knobs, done, now);
        ModSums sums{};
        table_registry::accumulate(compiled, now, sums);
        if (!lfosPerSample())
        {
            for (std::size_t k = 0; k < 2; ++k)
            {
                if (!lfoNeeded[k])
                {
                    continue;
                }
                const auto& settings = patch().lfos[k];
                lfos[k].setWaveform(settings.waveform);
                lfos[k].setFrequency(lfoRateFrom(k, sums));
                lfoGainBefore[k] = atNoteOn ? lfoGainFrom(k, sums) : lfoGainNow[k];
                lfoGainNow[k] = lfoGainFrom(k, sums);
                lfoAtSegmentStart[k] = lfos[k];
                lfos[k].process(lfoWave[k]);
            }
        }
        previousSums = atNoteOn ? sums : gridSums;
        gridSums = sums;
        if (patch().cutoffAtSegmentEnd)
        {
            // The LFOs at the segment's last sample; everything else as now.
            auto end = now;
            for (std::size_t k = 0; k < 2; ++k)
            {
                if (lfoNeeded[k] && !lfosPerSample())
                {
                    end[k == 0 ? id(SourceId::lfo1) : id(SourceId::lfo2)] =
                        0.5f * lfoGainNow[k] * (lfoWave[k][gridSamples - 1] + lfoOffset(k));
                }
            }
            endSums = {};
            table_registry::accumulate(compiled, end, endSums);
        }
        segmentBegun = true;
    }

    /// A note-on on a sounding voice: each LFO's cycle restarts, slewed. In
    /// segment order mid-segment, the rest of the segment is rendered again
    /// from a copy taken at the grid point.
    void retriggerLfos() noexcept
    {
        for (std::size_t k = 0; k < 2; ++k)
        {
            if (lfoNeeded[k] && !lfosPerSample() && segmentBegun)
            {
                lfos[k] = lfoAtSegmentStart[k];
                std::array<float, gridSamples> scratch{};
                lfos[k].process(std::span{scratch}.first(gridPosition));
                lfos[k].retrigger();
                lfos[k].process(std::span{lfoWave[k]}.subspan(gridPosition));
            }
            else
            {
                lfos[k].retrigger();
            }
        }
    }

    /// The sources a per-sample target reads, for the chunk at the block's
    /// sample `done` (the envelopes already rendered into their buffers).
    void fillSources(const VoiceKnobs& knobs, std::size_t done, std::size_t length) noexcept
    {
        const auto fill = [&](SourceId s, auto&& valueAt)
        {
            auto& b = sourceBuffers[id(s)];
            for (std::size_t i = 0; i < length; ++i)
            {
                b[i] = valueAt(i);
            }
        };
        if (sourcePerSample[id(SourceId::velocity)])
        {
            fill(SourceId::velocity, [&](std::size_t i) { return velocityRamp.at(i); });
        }
        if (sourcePerSample[id(SourceId::keytrack)])
        {
            fill(SourceId::keytrack, [&](std::size_t i) { return dsp::keytrackSource(keytrackPitch(knobs, done, i)); });
        }
        if (sourcePerSample[id(SourceId::ampEnvA)])
        {
            fill(SourceId::ampEnvA, [&](std::size_t i) { return envelopeBuffers[0][i]; });
        }
        if (sourcePerSample[id(SourceId::ampEnvB)])
        {
            fill(SourceId::ampEnvB, [&](std::size_t i) { return envelopeBuffers[1][i]; });
        }
        if (sourcePerSample[id(SourceId::modEnv1)])
        {
            fill(SourceId::modEnv1, [&](std::size_t i) { return modEnvelopeBuffers[0][i]; });
        }
        if (sourcePerSample[id(SourceId::modEnv2)])
        {
            fill(SourceId::modEnv2, [&](std::size_t i) { return modEnvelopeBuffers[1][i]; });
        }
        const auto t = knobs.time + done;
        if (sourcePerSample[id(SourceId::modWheel)] && context->modWheel != nullptr)
        {
            fill(SourceId::modWheel, [&](std::size_t i) { return context->modWheel->valueAt(t + i); });
        }
        if (sourcePerSample[id(SourceId::aftertouch)] && context->aftertouch != nullptr)
        {
            fill(SourceId::aftertouch, [&](std::size_t i) { return context->aftertouch->valueAt(t + i); });
        }
        if (sourcePerSample[id(SourceId::stepVelocity)])
        {
            const float v = patch().stepVelocity;
            fill(SourceId::stepVelocity, [&](std::size_t /*i*/) { return v; });
        }
    }

    /// The LFOs' output over the chunk, into their source buffers.
    void runLfos(std::size_t length) noexcept
    {
        if (!lfoNeeded[0] && !lfoNeeded[1])
        {
            return;
        }
        if (!lfosPerSample())
        {
            for (std::size_t k = 0; k < 2; ++k)
            {
                if (!lfoNeeded[k])
                {
                    continue;
                }
                auto& b = sourceBuffers[k == 0 ? id(SourceId::lfo1) : id(SourceId::lfo2)];
                const float o = lfoOffset(k);
                const bool ramped = rateOf(Target::lfoGain) == EvalRate::gridRamped;
                for (std::size_t i = 0; i < length; ++i)
                {
                    const float t = static_cast<float>(gridPosition + i) / static_cast<float>(gridSamples);
                    const float g =
                        ramped ? lfoGainBefore[k] + ((lfoGainNow[k] - lfoGainBefore[k]) * t) : lfoGainNow[k];
                    b[i] = 0.5f * g * (lfoWave[k][gridPosition + i] + o);
                }
                lfoLast[k] = b[length - 1];
            }
            return;
        }
        // Sample by sample: each LFO's rate and gain from the sources at the
        // sample before (a delay of one sample through any cycle).
        auto& b1 = sourceBuffers[id(SourceId::lfo1)];
        auto& b2 = sourceBuffers[id(SourceId::lfo2)];
        for (std::size_t i = 0; i < length; ++i)
        {
            ModSums sums{};
            accumulateLfoTargets(previousValues, sums);
            for (std::size_t k = 0; k < 2; ++k)
            {
                if (!lfoNeeded[k])
                {
                    continue;
                }
                const auto& settings = patch().lfos[k];
                lfos[k].setWaveform(settings.waveform);
                lfos[k].setFrequency(lfoRateFrom(k, sums));
                const float g = lfoGainFrom(k, sums);
                const float w = lfos[k].next();
                (k == 0 ? b1 : b2)[i] = 0.5f * g * (w + lfoOffset(k));
            }
            // The values at this sample, for the next.
            for (std::size_t s = 0; s < sourceCount; ++s)
            {
                if (sourcePerSample[s])
                {
                    previousValues[s] = sourceBuffers[s][i];
                }
            }
            previousValues[id(SourceId::lfo1)] = lfoNeeded[0] ? b1[i] : 0.0f;
            previousValues[id(SourceId::lfo2)] = lfoNeeded[1] ? b2[i] : 0.0f;
        }
        lfoLast[0] = lfoNeeded[0] ? b1[length - 1] : 0.0f;
        lfoLast[1] = lfoNeeded[1] ? b2[length - 1] : 0.0f;
    }

    void accumulateLfoTargets(const std::array<float, sourceCount>& values, ModSums& sums) const noexcept
    {
        for (std::size_t i = 0; i < compiled.count; ++i)
        {
            const auto& r = compiled.routes[i];
            if (r.target == Target::lfoRate || r.target == Target::lfoGain)
            {
                addTo(sums, r.target, r.parts, r.amount * values[r.source]);
            }
        }
    }

    /// The per-sample sums of every per-sample target, over the chunk.
    void sumPerSample(std::size_t length) noexcept
    {
        for (std::size_t t = 0; t < targetCount; ++t)
        {
            if (patch().rates[t] != EvalRate::perSample)
            {
                continue;
            }
            for (std::size_t q = 0; q < 2; ++q)
            {
                if (routed[t][q])
                {
                    std::fill_n(sampleSums[t][q].begin(), length, 0.0f);
                }
            }
        }
        for (std::size_t r = 0; r < compiled.count; ++r)
        {
            const auto& route = compiled.routes[r];
            const auto t = index(route.target);
            if (patch().rates[t] != EvalRate::perSample)
            {
                continue;
            }
            const auto& src = sourceBuffers[route.source];
            for (std::size_t q = 0; q < 2; ++q)
            {
                if ((route.parts & (1U << q)) == 0)
                {
                    continue;
                }
                auto& dst = sampleSums[t][q];
                for (std::size_t i = 0; i < length; ++i)
                {
                    dst[i] += route.amount * src[i];
                }
            }
        }
    }

    /// Target t's modulation of part q as a slot signal, for the samples
    /// from the chunk's sample `start`.
    [[nodiscard]] ModSignal signalOf(Target t, std::size_t q, std::size_t start, std::size_t run) const noexcept
    {
        if (!routedTo(t, q))
        {
            return {};
        }
        switch (rateOf(t))
        {
        case EvalRate::perSample:
            return {
                .kind = ModSignal::Kind::perSample,
                .values = std::span<const float>{sampleSums[index(t)][q]}.subspan(start, run),
            };
        case EvalRate::gridHeld:
            return {.kind = ModSignal::Kind::constant, .value = gridSums[index(t)][q]};
        case EvalRate::gridRamped:
            return {
                .kind = ModSignal::Kind::linear,
                .value = previousSums[index(t)][q],
                .end = gridSums[index(t)][q],
                .position = gridPosition + start,
                .segment = gridSamples,
            };
        }
        return {};
    }

    [[nodiscard]] SlotModulation slotModulation(std::size_t p, std::size_t start, std::size_t run) const noexcept
    {
        return {
            .volume = signalOf(Target::volume, p, start, run),
            .pan = signalOf(Target::pan, p, start, run),
            .phase = signalOf(Target::phase, p, start, run),
        };
    }

    /// Source s's interval over the `length` samples from the block's
    /// sample `done` (the chunk's start, before it is rendered).
    [[nodiscard]] Interval sourceBound(std::size_t s, const VoiceKnobs& knobs, std::size_t done,
                                       std::size_t length) const noexcept
    {
        const auto& row = sourceRows[s];
        if (patch().bounds == BoundMode::range)
        {
            return {.lo = row.min, .hi = row.max};
        }
        switch (static_cast<SourceId>(s))
        {
        case SourceId::velocity:
            return linearBound(velocityRamp.at(0), velocityRamp.at(length - 1));
        case SourceId::keytrack:
            return linearBound(dsp::keytrackSource(keytrackPitch(knobs, done, 0)),
                               dsp::keytrackSource(keytrackPitch(knobs, done, length - 1)));
        case SourceId::ampEnvA:
        case SourceId::ampEnvB:
            return envelopeStageBound(envelopeState(envelopes[s == id(SourceId::ampEnvA) ? 0 : 1]));
        case SourceId::modEnv1:
        case SourceId::modEnv2:
            return envelopeStageBound(envelopeState(modEnvelopes[s == id(SourceId::modEnv1) ? 0 : 1]));
        case SourceId::lfo1:
        case SourceId::lfo2:
        {
            const std::size_t k = s == id(SourceId::lfo1) ? 0 : 1;
            if (lfosPerSample() || !lfoNeeded[k])
            {
                return {.lo = row.min, .hi = row.max};
            }
            const auto wave = renderedBound(std::span<const float>{lfoWave[k]}.subspan(gridPosition, length));
            const bool ramped = rateOf(Target::lfoGain) == EvalRate::gridRamped;
            const Interval gain = ramped ? linearBound(lfoGainBefore[k], lfoGainNow[k])
                                         : Interval{.lo = lfoGainNow[k], .hi = lfoGainNow[k]};
            const float o = lfoOffset(k);
            return lfoSourceBound(wave, gain, {.lo = o, .hi = o});
        }
        case SourceId::modWheel:
        case SourceId::aftertouch:
        {
            const auto* ramp = s == id(SourceId::modWheel) ? context->modWheel : context->aftertouch;
            if (ramp == nullptr)
            {
                return {};
            }
            const auto t = knobs.time + done;
            return linearBound(ramp->valueAt(t), ramp->valueAt(t + length - 1));
        }
        case SourceId::stepVelocity:
            return {.lo = patch().stepVelocity, .hi = patch().stepVelocity};
        default:
            return {};
        }
    }

    /// The largest the modulation of target t on part q can be over those
    /// samples: the sum of each route's amount x its source's interval.
    [[nodiscard]] float modulationBound(Target t, std::size_t q, const VoiceKnobs& knobs, std::size_t done,
                                        std::size_t length) const noexcept
    {
        float hi = 0.0f;
        for (std::size_t i = 0; i < compiled.count; ++i)
        {
            const auto& r = compiled.routes[i];
            if (r.target == t && (r.parts & (1U << q)) != 0)
            {
                hi += scaled(sourceBound(r.source, knobs, done, length), r.amount).hi;
            }
        }
        return hi;
    }

    // --- dsp::Voice, with the modulation's terms --------------------------------------------

    void startPitch(float note, const NoteStart& start) noexcept
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

    [[nodiscard]] static float bendAt(const VoiceKnobs& knobs, std::size_t ahead) noexcept
    {
        return knobs.bend != nullptr ? knobs.bend->semitonesAt(knobs.time + ahead) : 0.0f;
    }

    [[nodiscard]] float pitchAt(const VoiceKnobs& knobs, std::size_t done, std::size_t ahead) const noexcept
    {
        return glide.at(ahead) + bendAt(knobs, done + ahead);
    }

    [[nodiscard]] float keytrackPitch(const VoiceKnobs& knobs, std::size_t done, std::size_t ahead) const noexcept
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

    /// A slot is silent only if its gains are at 0 and its Volume routes
    /// can't lift it over the segment (the bound per factor).
    [[nodiscard]] bool slotSilent(std::size_t p, const Slot& slot) const noexcept
    {
        if (!slot.isSilent())
        {
            return false;
        }
        return !volumeLifts[p];
    }

    [[nodiscard]] bool partProduces(std::size_t p) const noexcept
    {
        for (std::size_t s = 0; s < slotsPerPart; ++s)
        {
            const auto& slot = slots[(p * slotsPerPart) + s];
            if (slot.soundingVoices() > 0 && !slotSilent(p, slot))
            {
                return true;
            }
        }
        return false;
    }

    /// Every sounding slot's left channel is its right, and no route moves
    /// a part's Pan.
    [[nodiscard]] bool allCentred() const noexcept
    {
        for (std::size_t p = 0; p < partCount; ++p)
        {
            if (routedTo(Target::pan, p))
            {
                return false;
            }
        }
        return std::ranges::all_of(slots, [](const Slot& s) { return s.soundingVoices() == 0 || s.isCentred(); });
    }

    void updateRoutes() noexcept
    {
        for (std::size_t f = 0; f < partCount; ++f)
        {
            routes[f] = {.active = inputs[f] != FilterInput::none, .both = inputs[f] == FilterInput::both};
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

    void applyPendingAtGrid(const VoiceKnobs& knobs, std::size_t ahead) noexcept
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

    [[nodiscard]] static float read(const LinearSmoother* ramp, std::size_t ahead) noexcept
    {
        return ramp != nullptr ? ramp->valueAfter(ahead) : 0.0f;
    }

    void feedFilter(std::size_t f, const VoiceKnobs& knobs, std::size_t ahead, float keytrack) noexcept
    {
        ahead += knobs.offset;
        const ModSums& m = patch().cutoffAtSegmentEnd ? endSums : gridSums;
        const float octaves = dsp::filterCutoffOctaves({
            .cutoff = read(knobs.cutoff[f], ahead),
            .cutoffModulation = m[index(Target::cutoff)][f],
            .controlCutoff = read(knobs.controlCutoff, ahead),
            .controlModulation = m[index(Target::controlCutoff)][f],
            .keytrack = keytrack,
        });
        const float k = dsp::filterResonanceK({
            .resonance = read(knobs.resonance[f], ahead),
            .resonanceModulation = m[index(Target::resonance)][f],
            .controlResonance = read(knobs.controlResonance, ahead),
            .controlModulation = m[index(Target::controlResonance)][f],
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

    void feedFilters(const VoiceKnobs& knobs, std::size_t ahead, float pitch) noexcept
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

    [[nodiscard]] AudioBlock block(StereoChunk& chunk, std::array<float*, 2>& pointers, std::size_t length,
                                   std::size_t channels = 2) noexcept
    {
        pointers = {chunk[0].data(), chunk[1].data()};
        return {pointers.data(), channels, length};
    }

    [[nodiscard]] bool envelopeSilent(std::size_t p) const noexcept
    {
        const auto& e = envelopes[p];
        const bool zeroHold = e.stage() == dsp::AdsrStage::sustain && e.level() <= 0.0f && e.sustainLevel() <= 0.0f;
        return !e.isActive() || zeroHold;
    }

    /// The bound on Mix's modulation (M1.4 task 5's seam, filled).
    [[nodiscard]] float mixModulationBound(std::size_t part, const VoiceKnobs& knobs, std::size_t done,
                                           std::size_t length) const noexcept
    {
        if (!routedTo(Target::mix, part))
        {
            return 0.0f;
        }
        return modulationBound(Target::mix, part, knobs, done, length);
    }

    [[nodiscard]] bool mixSilent(std::size_t p, const VoiceKnobs& knobs, std::size_t done,
                                 std::size_t length) const noexcept
    {
        const auto* ramp = knobs.mix[p];
        const auto at = knobs.offset + done;
        const float knob =
            ramp != nullptr ? std::max(ramp->valueAfter(at), ramp->valueAfter(at + length - 1)) : dsp::mixLaw.unity;
        return knob + mixModulationBound(p, knobs, done, length) <= dsp::mixLaw.knobMin;
    }

    [[nodiscard]] static bool soloSilent(std::size_t p, const VoiceKnobs& knobs, std::size_t done,
                                         std::size_t length) noexcept
    {
        const auto* ramp = knobs.solo[p];
        if (ramp == nullptr)
        {
            return false;
        }
        const auto at = knobs.offset + done;
        return std::max(ramp->valueAfter(at), ramp->valueAfter(at + length - 1)) <= 0.0f;
    }

    [[nodiscard]] bool gainSilent(std::size_t p, const VoiceKnobs& knobs, std::size_t done,
                                  std::size_t length) const noexcept
    {
        return envelopeSilent(p) || mixSilent(p, knobs, done, length) || soloSilent(p, knobs, done, length);
    }

    [[nodiscard]] Plan planChunk(const VoiceKnobs& knobs, std::size_t done) noexcept
    {
        const std::size_t rest = gridSamples - gridPosition;
        // A slot at Volume 0 counts as silent only if its part's Volume
        // routes can't lift it over the rest of the segment.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            volumeLifts[p] =
                routedTo(Target::volume, p) && modulationBound(Target::volume, p, knobs, done, rest) > 0.0f;
        }
        std::array<bool, partCount> produces{};
        for (std::size_t p = 0; p < partCount; ++p)
        {
            produces[p] = partProduces(p);
            if (gridPosition == 0 || segmentSilent[p])
            {
                segmentSilent[p] = gainSilent(p, knobs, done, rest);
                if (gridPosition == 0 && routes[p].active && routedTo(Target::mix, p))
                {
                    ++outputTests;
                    if (segmentSilent[p])
                    {
                        ++outputSkips;
                    }
                }
            }
        }
        Plan plan;
        for (std::size_t q = 0; q < partCount; ++q)
        {
            plan.crossing[q] = routes[1 - q].both && produces[q] && !envelopeSilent(q);
        }
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (!routes[f].active)
            {
                continue;
            }
            plan.output[f] = !segmentSilent[f];
            if (!filters[f].isBypassed())
            {
                const bool inputSilent = !produces[f] && !plan.crossing[1 - f];
                plan.skip[f] = segmentSilent[f] || (inputSilent && filters[f].isAtRest());
            }
        }
        const auto takes = [&](std::size_t f)
        {
            return routes[f].active && !plan.skip[f] && (!filters[f].isBypassed() || plan.output[f]);
        };
        for (std::size_t p = 0; p < partCount; ++p)
        {
            plan.render[p] = takes(p) || (plan.crossing[p] && takes(1 - p));
        }
        return plan;
    }

    void leaveMonoIfMoved() noexcept
    {
        if (mono && !allCentred())
        {
            for (auto& f : filters)
            {
                f.copyChannel(0, 1);
            }
            mono = false;
        }
    }

    void renderChunk(const AudioBlock& out, const VoiceKnobs& knobs, std::size_t done) noexcept
    {
        const auto length = out.numSamples();
        leaveMonoIfMoved();
        const std::size_t channels = mono ? 1 : 2;
        const Plan plan = planChunk(knobs, done);

        // 0. The envelopes first (new: they are sources; nothing they compute
        // depends on the slots), then the sources a per-sample target reads,
        // the LFOs, and the per-sample sums.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            envelopes[p].process(std::span{envelopeBuffers[p]}.first(length));
            modEnvelopes[p].process(std::span{modEnvelopeBuffers[p]}.first(length));
        }
        const bool modulated = compiled.count > 0;
        if (modulated)
        {
            fillSources(knobs, done, length);
            runLfos(length);
            sumPerSample(length);
        }

        // 1. The oscillators, each part at its own pitch while Pitch is
        // modulated.
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
        std::array<bool, partCount> pitchPerSample{};
        std::array<float, partCount> pitchHeld{};
        for (std::size_t p = 0; p < partCount; ++p)
        {
            if (routedTo(Target::pitch, p))
            {
                pitchPerSample[p] = rateOf(Target::pitch) != EvalRate::gridHeld;
                pitchHeld[p] = gridSums[index(Target::pitch)][p];
            }
        }
        std::array<AudioBlock, partCount> parts{};
        for (std::size_t p = 0; p < partCount; ++p)
        {
            parts[p] = block(partBuffers[p], partPointers[p], length);
            if (heard[p] && plan.render[p])
            {
                parts[p].clear();
            }
        }
        for (std::size_t p = 0; p < partCount; ++p)
        {
            for (std::size_t s = 0; s < slotsPerPart; ++s)
            {
                const auto index = (p * slotsPerPart) + s;
                const auto& slot = slots[index];
                const bool skippable = (!plan.render[p] || slotSilent(p, slot)) && slot.canSkip();
                slotSkipping[index] = skippable && (gridPosition == 0 || slotSkipping[index]);
                if (slot.isSilent() && volumeLifts[p] && plan.render[p])
                {
                    ++lifted;
                }
            }
        }
        std::array<bool, partCount> cleared{};
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
                               for (std::size_t p = 0; p < partCount; ++p)
                               {
                                   if (!heard[p])
                                   {
                                       continue;
                                   }
                                   // This part's notes: the voice's, plus its Pitch
                                   // modulation (semitones).
                                   const bool perSample = moving || pitchPerSample[p];
                                   std::span<const float> partNotes = std::span<const float>{notes}.first(run);
                                   float partNote = steadyPitch + offset;
                                   if (routedTo(Target::pitch, p))
                                   {
                                       auto& own = modNotes[p];
                                       for (std::size_t i = 0; i < run; ++i)
                                       {
                                           const float base = moving ? notes[i] : steadyPitch + offset;
                                           own[i] = base + pitchModulation(p, start + i);
                                       }
                                       partNotes = std::span<const float>{own}.first(run);
                                       partNote = (steadyPitch + offset) + pitchHeld[p];
                                   }
                                   const auto mod = slotModulation(p, start, run);
                                   const auto piece = parts[p].subBlock(start, run);
                                   for (std::size_t s = 0; s < slotsPerPart; ++s)
                                   {
                                       auto& slot = slots[(p * slotsPerPart) + s];
                                       if (slot.soundingVoices() == 0)
                                       {
                                           continue;
                                       }
                                       if (slotSkipping[(p * slotsPerPart) + s])
                                       {
                                           if (perSample)
                                           {
                                               slot.skip(partNotes);
                                           }
                                           else
                                           {
                                               slot.skip(run, partNote);
                                           }
                                           continue;
                                       }
                                       if (!plan.render[p] && !cleared[p])
                                       {
                                           parts[p].clear();
                                           cleared[p] = true;
                                       }
                                       if (perSample)
                                       {
                                           slot.process(piece, partNotes, mod);
                                       }
                                       else
                                       {
                                           slot.process(piece, partNote, mod);
                                       }
                                   }
                               }
                           });

        // 2. Each heard filter's part's Mix and Solo gains.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            if (routes[p].active && plan.output[p])
            {
                fillMixGains(p, knobs, done, length);
            }
        }

        // 3-5 as dsp::Voice.
        const auto left = out.channel(0);
        const auto right = out.channel(1);
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (!routes[f].active)
            {
                continue;
            }
            if (plan.skip[f])
            {
                filters[f].skip(length);
                continue;
            }
            if (!plan.output[f])
            {
                continue;
            }
            const auto input = block(filterBuffers[f], filterPointers[f], length, channels);
            buildInput(f, length, channels, plan.crossing[1 - f]);
            filters[f].process(input);
            const auto& e = envelopeBuffers[f];
            const auto& g = mixGains[f];
            const auto y0 = input.channel(0);
            const auto y1 = channels == 2 ? input.channel(1) : y0;
            for (std::size_t i = 0; i < length; ++i)
            {
                const float ge = g[i] * e[i];
                left[i] += ge * y0[i];
                right[i] += ge * y1[i];
            }
        }
    }

    /// Part p's Pitch modulation (semitones) at the chunk's sample i.
    [[nodiscard]] float pitchModulation(std::size_t p, std::size_t i) const noexcept
    {
        switch (rateOf(Target::pitch))
        {
        case EvalRate::perSample:
            return sampleSums[index(Target::pitch)][p][i];
        case EvalRate::gridHeld:
            return gridSums[index(Target::pitch)][p];
        case EvalRate::gridRamped:
        {
            const float a = previousSums[index(Target::pitch)][p];
            const float b = gridSums[index(Target::pitch)][p];
            return a + ((b - a) * (static_cast<float>(gridPosition + i) / static_cast<float>(gridSamples)));
        }
        }
        return 0.0f;
    }

    void fillMixGains(std::size_t p, const VoiceKnobs& knobs, std::size_t done, std::size_t length) noexcept
    {
        auto& g = mixGains[p];
        const auto at = knobs.offset + done;
        if (routedTo(Target::mix, p))
        {
            // Mix per sample with its modulation (M1.4 task 4).
            const auto* ramp = knobs.mix[p];
            const auto& m = sampleSums[index(Target::mix)][p];
            for (std::size_t i = 0; i < length; ++i)
            {
                const float knob = ramp != nullptr ? ramp->valueAfter(at + i) : dsp::mixLaw.unity;
                g[i] = dsp::mixGain({.mix = knob, .modulation = m[i]});
            }
        }
        else
        {
            fillKnobGains(g, knobs.mix[p], at, length);
        }
        const auto* solo = knobs.solo[p];
        if (solo == nullptr)
        {
            return;
        }
        const float first = solo->valueAfter(at);
        const float last = solo->valueAfter(at + length);
        if (first <= last && first >= last)
        {
            if (first < 1.0f || first > 1.0f)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    g[i] *= first;
                }
            }
            return;
        }
        for (std::size_t i = 0; i < length; ++i)
        {
            g[i] *= solo->valueAfter(at + i);
        }
    }

    static void fillKnobGains(Chunk& g, const LinearSmoother* ramp, std::size_t at, std::size_t length) noexcept
    {
        if (ramp == nullptr)
        {
            std::fill_n(g.begin(), length, dsp::mixGain({.mix = dsp::mixLaw.unity}));
            return;
        }
        const float first = ramp->valueAfter(at);
        const float last = ramp->valueAfter(at + length);
        if (first <= last && first >= last)
        {
            std::fill_n(g.begin(), length, dsp::mixGain({.mix = first}));
            return;
        }
        for (std::size_t i = 0; i < length; ++i)
        {
            g[i] = dsp::mixGain({.mix = ramp->valueAfter(at + i)});
        }
    }

    void buildInput(std::size_t f, std::size_t length, std::size_t channels, bool crossing) noexcept
    {
        const std::size_t other = 1 - f;
        const auto& envelopeOther = envelopeBuffers[other];
        for (std::size_t c = 0; c < channels; ++c)
        {
            const auto& own = partBuffers[f][c];
            const auto& crossingPart = partBuffers[other][c];
            auto& dst = filterBuffers[f][c];
            if (routes[f].both && crossing)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    const float enveloped = envelopeOther[i] * crossingPart[i];
                    dst[i] = own[i] + enveloped;
                }
            }
            else
            {
                std::copy_n(own.begin(), length, dst.begin());
            }
        }
    }

    std::array<Slot, slotCount> slots;
    std::array<LadderFilter, partCount> filters{};
    std::array<Adsr, partCount> envelopes{};
    dsp::PitchDrift pitchDrift;

    std::array<FilterInput, partCount> pendingInputs{FilterInput::own, FilterInput::own};
    std::array<bool, partCount> pendingBypass{};
    std::array<FilterInput, partCount> inputs{FilterInput::own, FilterInput::own};
    std::array<Route, partCount> routes{};
    std::array<bool, partCount> heard{};

    std::array<StereoChunk, partCount> partBuffers{};
    std::array<StereoChunk, partCount> filterBuffers{};
    std::array<Chunk, partCount> envelopeBuffers{};
    std::array<Chunk, partCount> mixGains{};
    std::array<std::array<float*, 2>, partCount> partPointers{};
    std::array<std::array<float*, 2>, partCount> filterPointers{};

    std::array<float, partCount> lastOctaves{};
    std::array<float, partCount> lastK{};
    dsp::Glide glide;
    dsp::Glide velocityRamp;
    Chunk pitches{};
    Chunk notes{};
    float masterTuneHz = dsp::concertPitchHz;
    float masterTuneOffset = 0.0f;
    bool played = false;
    std::size_t gridPosition = 0;
    bool mono = false;
    std::array<bool, partCount> segmentSilent{};
    std::array<bool, slotCount> slotSkipping{};

    // The modulation (new).
    const ModContext* context = nullptr;
    std::array<Adsr, 2> modEnvelopes{};
    std::array<Lfo, 2> lfos{Lfo{0}, Lfo{0}};
    std::array<Lfo, 2> lfoAtSegmentStart{Lfo{0}, Lfo{0}};
    std::array<Chunk, 2> lfoWave{}; ///< each LFO's wave over the segment, rendered at its grid point
    std::array<float, 2> lfoLast{};
    std::array<float, 2> lfoGainNow{};
    std::array<float, 2> lfoGainBefore{};
    std::array<bool, 2> lfoNeeded{};
    std::array<Chunk, partCount> modEnvelopeBuffers{};
    table_registry::Compiled compiled{};
    std::uint32_t compiledVersion = ~std::uint32_t{0};
    std::array<std::array<bool, 2>, targetCount> routed{};
    std::array<bool, sourceCount> sourceUsed{};
    std::array<bool, sourceCount> sourcePerSample{};
    std::array<Chunk, sourceCount> sourceBuffers{};
    std::array<std::array<Chunk, 2>, targetCount> sampleSums{};
    std::array<Chunk, partCount> modNotes{};
    ModSums gridSums{};
    ModSums previousSums{};
    ModSums endSums{};
    std::array<float, sourceCount> previousValues{};
    std::array<bool, partCount> volumeLifts{};
    bool segmentBegun = false;
    std::uint64_t startTime = 0;
    std::uint64_t outputSkips = 0;
    std::uint64_t outputTests = 0;
    std::uint64_t lifted = 0;
};

// --- The engine ------------------------------------------------------------------------

/// An event for ModEngine.
struct EngineEvent
{
    enum class Kind : std::uint8_t
    {
        voice,    ///< a note on or off (VoiceEvent)
        modWheel, ///< the mod wheel to `value` (0-1)
    };
    std::size_t offset = 0;
    Kind kind = Kind::voice;
    dsp::VoiceEvent voice{};
    float value = 0.0f;
};

/// The real VoiceAllocator over 16 ModVoices, the filter knobs (still), the
/// mod wheel, and its events. The output is written, not added to.
class ModEngine
{
public:
    using Voice = ModVoice<>;
    using Allocator = dsp::VoiceAllocator<Voice, 16>;

    ModEngine(const MinBlepTable& table, ModPatch modPatch, std::uint64_t seed = 1) : patch(modPatch)
    {
        context = {.patch = &patch, .modWheel = &modWheel, .aftertouch = &aftertouch};
        allocator = std::make_unique<Allocator>(seed, table, &context);
        for (auto* r : {cutoff.data(), &cutoff[1], &controlCutoff})
        {
            r->setCurrentAndTarget(5.0f);
        }
    }

    [[nodiscard]] Allocator& voices() noexcept
    {
        return *allocator;
    }
    [[nodiscard]] ModPatch& modulation() noexcept
    {
        return patch;
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
    void prepare(const ProcessSpec& spec)
    {
        allocator->prepare(spec);
        time = 0;
    }

    /// One host block, its events in order of offset.
    void process(const AudioBlock& out, std::span<const EngineEvent> events) noexcept
    {
        out.clear();
        std::size_t done = 0;
        std::size_t e = 0;
        const auto n = out.numSamples();
        while (done < n)
        {
            std::size_t const first = e;
            while (e < events.size() && events[e].offset <= done)
            {
                ++e;
            }
            // Apply every non-voice event at `done`; voice events go to the
            // allocator at their offset within the piece.
            std::array<dsp::VoiceEvent, 16> notesHere{};
            std::size_t count = 0;
            for (std::size_t k = first; k < e; ++k)
            {
                if (events[k].kind == EngineEvent::Kind::modWheel)
                {
                    modWheel.set(time + done, events[k].value, rampSamples);
                }
                else if (count < notesHere.size())
                {
                    notesHere[count] = events[k].voice;
                    notesHere[count].offset = 0;
                    ++count;
                }
            }
            const std::size_t next = e < events.size() ? std::min(events[e].offset, n) : n;
            const auto piece = out.subBlock(done, next - done);
            allocator->process(piece, std::span{notesHere}.first(count), knobs(done));
            done = next;
        }
        time += n;
    }

    /// The 7-bit steps' ramp, in samples (0: a step).
    std::uint64_t rampSamples = 0;

private:
    [[nodiscard]] VoiceKnobs knobs(std::size_t done) const noexcept
    {
        VoiceKnobs k;
        k.cutoff = {cutoff.data(), &cutoff[1]};
        k.resonance = {resonance.data(), &resonance[1]};
        k.controlCutoff = &controlCutoff;
        k.controlResonance = &controlResonance;
        k.time = time + done;
        return k;
    }

    ModPatch patch;
    ControlRamp modWheel;
    ControlRamp aftertouch;
    ModContext context;
    std::unique_ptr<Allocator> allocator;
    std::array<LinearSmoother, 2> cutoff{};
    std::array<LinearSmoother, 2> resonance{};
    LinearSmoother controlCutoff;
    LinearSmoother controlResonance;
    std::uint64_t time = 0;
};

} // namespace anoesis::test::modulation
