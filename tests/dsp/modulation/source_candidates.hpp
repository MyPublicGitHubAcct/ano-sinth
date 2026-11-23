// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The modulation sources' candidates (M1.5 task 3, stage 1;
// docs/dsp/modulation-sources.md). Prototypes for measurement, not library
// code.
//
// SourceVoice is a copy of dsp::Voice as built (M1.4, on the real
// OscillatorSlot, LadderFilter and Adsr) with the library's modulation model
// (anoesis::dsp::modulation: compile(), accumulate(), the bounds,
// RouteCompiler) and the sources as task 3 would build them. With no routes
// it is dsp::Voice to the bit (the default run checks it). It adds:
//
// - The sources: two mod envelopes (Adsr) and two LFOs (Lfo) per voice;
//   velocity, keytrack and the amp envelopes; ModWheel and Aftertouch read
//   from global Controllers at absolute samples (each smoothing candidate a
//   mode of Controller); Step Velocity a global stand-in input. An LFO runs
//   in segment order (task 2's decision): its rate (held) and gain (ramped)
//   set at each grid point, its wave rendered for the segment ahead at gain
//   1 and offset 0, and scaled outside, 0.5 x gain x (wave + offset). A
//   free LFO copies the engine's global instance.
// - The destinations the questions need: Pitch A/B/AB and Mix A/B/AB per
//   sample; Cutoff and Reso (A, B, and the Filter Control's AB) at the grid
//   points, from the sources as they stand there; LFO Rate (held) and Gain
//   (ramped) in segment order. Volume, Phase and Pan are task 4's: their
//   routes compile and are ignored.
// - Each question's candidates, as switches in SourceSettings:
//   1. how per-sample destinations get their sources: each used source
//      rendered into a chunk buffer and the routes summed over it
//      (buffered); each source ticked inline per sample into SourceValues
//      and accumulate()d (inlined); or buffered with a source that is
//      constant over the chunk added as one term (folded);
//   2. every source always run, or only while a route reads it; then, for a
//      route that appears mid-note, the source starting at the next note,
//      at the next grid point from its note-on state, or reconstructed from
//      the note's elapsed time (a replay of its gates);
//   3. the controllers' smoothing (Controller);
//   4. velocity and keytrack per sample or held at the grid point;
//   5. an unheard part's amp envelope run always, or only while it is a
//      source; and a switch that turns every skip off, as the reference;
//   6. the bounds, tight (task 2's decision, keytrack from its parts) or one
//      of four deliberately small rules, with a check of every source's
//      value against its bound at every sample.
//
// SourceEngine runs the real dsp::VoiceAllocator over 16 SourceVoices with
// the engine's state the plan gives M1.8: the modulation::Patch, the
// controllers, the free LFOs' global instances and Step Velocity.

#pragma once

#include <algorithm>
#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/modulation/route_compiler.hpp>
#include <anoesis/dsp/modulation/routing.hpp>
#include <anoesis/dsp/modulation/source_bound.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/pitch_drift.hpp>
#include <anoesis/dsp/voice/glide.hpp>
#include <anoesis/dsp/voice/mix_law.hpp>
#include <anoesis/dsp/voice/pitch_bend.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace anoesis::test::sources
{

namespace dsp = anoesis::dsp;
namespace mod = anoesis::dsp::modulation;

using dsp::AdsrStage;
using dsp::AudioBlock;
using dsp::FilterInput;
using dsp::LinearSmoother;
using dsp::MinBlepTable;
using dsp::ProcessSpec;
using dsp::VoiceKnobs;
using mod::Interval;
using mod::Source;
using mod::Target;

inline constexpr std::size_t sourceCount = mod::sourceCount;

[[nodiscard]] constexpr std::size_t at(Source s) noexcept
{
    return static_cast<std::size_t>(s);
}
[[nodiscard]] constexpr std::size_t at(Target t) noexcept
{
    return static_cast<std::size_t>(t);
}

/// The LFO Rate knob's law (inventory L6, a placeholder; the prototype of
/// task 2's): 0.1 Hz x 1000^(x / 10) for x in [0, 10].
[[nodiscard]] inline float lfoRateHz(float units) noexcept
{
    const float x = std::clamp(units, 0.0f, 10.0f);
    return 0.1f * std::exp2(x * (9.965784285f / 10.0f));
}

// --- The controllers (question 3) ---------------------------------------------------------

/// How a 7-bit controller's steps are smoothed.
enum class Smoothing : std::uint8_t
{
    step,     ///< each value at once
    linear,   ///< a linear ramp over `ms` from where it is, at absolute samples (as PitchBend)
    onePole,  ///< an exponential approach, time constant `ms`, in closed form at absolute samples
    perBlock, ///< a one-pole advanced once per host block and held over it (the control: block-dependent)
};

/// A global controller (ModWheel, Aftertouch): values at absolute samples,
/// read by every voice at its own absolute samples.
class Controller
{
public:
    void configure(Smoothing mode, double sampleRate, float milliseconds) noexcept
    {
        smoothing = mode;
        const double n = std::max(0.0, static_cast<double>(milliseconds) * 1e-3 * sampleRate);
        length = static_cast<std::uint64_t>(std::llround(n));
        tau = std::max(1.0, n);
        reset();
    }

    void reset() noexcept
    {
        from = 0.0f;
        to = 0.0f;
        start = 0;
        held = 0.0f;
    }

    /// A new value (0-1) at the absolute sample `time`.
    void set(std::uint64_t time, float value) noexcept
    {
        from = valueAt(time);
        to = value;
        start = time;
    }

    /// For perBlock only: the host block [time, time + n) begins.
    void beginBlock(std::uint64_t /*time*/, std::size_t n) noexcept
    {
        if (smoothing != Smoothing::perBlock)
        {
            return;
        }
        const auto a = static_cast<float>(1.0 - std::exp(-static_cast<double>(n) / tau));
        const float step = (to - held) * a;
        held += step;
    }

    [[nodiscard]] float valueAt(std::uint64_t time) const noexcept
    {
        switch (smoothing)
        {
        case Smoothing::step:
            return time >= start ? to : from;
        case Smoothing::linear:
        {
            if (time >= start + length)
            {
                return to;
            }
            if (time <= start)
            {
                return from;
            }
            const float t = static_cast<float>(time - start) / static_cast<float>(length);
            const float d = (to - from) * t;
            return from + d;
        }
        case Smoothing::onePole:
        {
            if (time <= start)
            {
                return from;
            }
            const auto decay = static_cast<float>(std::exp(-static_cast<double>(time - start) / tau));
            const float rest = (from - to) * decay;
            return std::abs(rest) < 0x1p-24f ? to : to + rest;
        }
        case Smoothing::perBlock:
            return held;
        }
        return to;
    }

    /// The value doesn't move from `time` on (until the next set()).
    [[nodiscard]] bool isSteadyFrom(std::uint64_t time) const noexcept
    {
        switch (smoothing)
        {
        case Smoothing::step:
            return time >= start;
        case Smoothing::linear:
            return time >= start + length;
        case Smoothing::onePole:
        {
            const float v = valueAt(time);
            return v <= to && v >= to; // reached, without -Wfloat-equal
        }
        case Smoothing::perBlock:
            return true; // held over the host block
        }
        return true;
    }

private:
    Smoothing smoothing = Smoothing::linear;
    std::uint64_t length = 240;
    double tau = 240.0;
    float from = 0.0f;
    float to = 0.0f;
    std::uint64_t start = 0;
    float held = 0.0f;
};

// --- The settings ---------------------------------------------------------------------------

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
    bool free = false;
};

enum class Delivery : std::uint8_t
{
    buffered, ///< (a) each used source into a chunk buffer, the routes summed over it
    inlined,  ///< (b) each used source ticked per sample into SourceValues, accumulate()d
    folded,   ///< (c) as (a), a source constant over the chunk added as one term
};

enum class RunPolicy : std::uint8_t
{
    always,   ///< every source runs while the voice does
    usedOnly, ///< a source runs only while a route reads it
};

/// A source that a route starts reading mid-note (RunPolicy::usedOnly).
enum class LateStart : std::uint8_t
{
    nextNote,    ///< it stays at 0 until the voice's next note-on
    nextGrid,    ///< it starts at the next grid point, from its note-on state
    reconstruct, ///< at the next grid point, from the note's elapsed time (its gates replayed)
};

enum class LinearRate : std::uint8_t
{
    perSample, ///< velocity and keytrack read at every sample
    held,      ///< read at the grid point, held over the segment
};

enum class BoundRule : std::uint8_t
{
    tight,        ///< task 2's decision; keytrack from its parts (the glide's ends plus the bend's)
    keytrackEnds, ///< keytrack from its own two ends (too small when a glide and a bend oppose)
    levelOnly,    ///< every source its value at the chunk's start (too small)
    lfoEnds,      ///< an LFO from its segment's two ends (too small at crests and slews)
};

/// A free LFO at a note-on on a sounding voice (a steal, a retrigger).
enum class FreeCopy : std::uint8_t
{
    fromSilence, ///< copied from the global instance at a note-on from silence only
    everyNoteOn, ///< copied again at every note-on
};

enum class AmpEnvelopeRun : std::uint8_t
{
    always,      ///< both amp envelopes run (dsp::Voice)
    heardOrUsed, ///< an unheard part's runs only while it is a source
};

struct SourceSettings
{
    std::array<EnvelopeSettings, 2> modEnvelopes{};
    std::array<LfoSettings, 2> lfos{};
    Delivery delivery = Delivery::folded;
    RunPolicy run = RunPolicy::usedOnly;
    LateStart late = LateStart::nextGrid;
    LinearRate linearRate = LinearRate::perSample;
    BoundRule bounds = BoundRule::tight;
    FreeCopy freeCopy = FreeCopy::fromSilence;
    AmpEnvelopeRun ampEnvelopes = AmpEnvelopeRun::always;
    bool neverSkip = false;   ///< the reference: nothing skipped (exact skips run)
    bool checkBounds = false; ///< every source's value checked against its bound at every sample
};

/// What every voice reads that isn't its own (a constructor argument, so it
/// passes through VoiceAllocator).
struct SourceContext
{
    const mod::Patch* patch = nullptr;
    const SourceSettings* settings = nullptr;
    const Controller* modWheel = nullptr;
    const Controller* aftertouch = nullptr;
    const std::array<dsp::Lfo, 2>* freeLfos = nullptr; ///< the global instances, at the block's sample
    const float* stepVelocity = nullptr;
};

/// Per source: samples checked and samples outside the bound.
struct BoundStats
{
    std::array<std::uint64_t, sourceCount> checked{};
    std::array<std::uint64_t, sourceCount> outside{};
    std::array<double, sourceCount> width{}; ///< summed bound width per check
    std::array<float, sourceCount> worst{};  ///< the largest excess
};

template <std::size_t Taps = dsp::minBlepTaps>
class SourceVoice
{
public:
    using Slot = dsp::OscillatorSlot<Taps>;
    using Knobs = VoiceKnobs;
    static constexpr std::size_t partCount = 2;
    static constexpr std::size_t slotsPerPart = 2;
    static constexpr std::size_t slotCount = partCount * slotsPerPart;
    static constexpr std::size_t gridSamples = dsp::LadderFilter::gridSamples;

    SourceVoice(const MinBlepTable& residuals, const SourceContext* sourceContext, std::uint64_t seed = 0) noexcept
        : slots{Slot{residuals}, Slot{residuals}, Slot{residuals}, Slot{residuals}}, context(sourceContext)
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
    [[nodiscard]] dsp::Adsr& envelope(std::size_t part) noexcept
    {
        return envelopes[part];
    }
    [[nodiscard]] dsp::PitchDrift& drift() noexcept
    {
        return pitchDrift;
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
    void setWarm(bool on) noexcept
    {
        for (auto& f : filters)
        {
            f.setWarm(on);
        }
    }

    // --- For measurement ------------------------------------------------------------------

    [[nodiscard]] const BoundStats& boundStats() const noexcept
    {
        return stats;
    }
    /// Samples of reconstruction replayed (LateStart::reconstruct).
    [[nodiscard]] std::uint64_t replayedSamples() const noexcept
    {
        return replayed;
    }
    /// The last value a source produced (0 for one that doesn't run).
    [[nodiscard]] float lastValue(Source s) const noexcept
    {
        return lastValues[at(s)];
    }
    [[nodiscard]] bool isLive(Source s) const noexcept
    {
        return live[at(s)];
    }

    void prepare(const ProcessSpec& spec) noexcept
    {
        const ProcessSpec stereo{.sampleRate = spec.sampleRate, .maxBlockSize = spec.maxBlockSize, .numChannels = 2};
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
        compiler.reset();
        live = {};
        lastValues = {};
        gridSums = {};
        lfoGainBefore = {};
        lfoGainNow = {};
        segmentBegun = false;
        logCount = 0;
        stats = {};
        replayed = 0;
    }

    void noteOn(float note, const VoiceKnobs& knobs, const dsp::NoteStart& start = {}) noexcept
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
        for (auto& s : slots)
        {
            s.noteOn();
        }
        pitchDrift.noteOn();
        for (auto& e : envelopes)
        {
            e.gateOn();
        }
        keyHeld = true;
        sourcesNoteOn(knobs, sounding);
        if (sounding)
        {
            return;
        }
        applyPending();
        gridPosition = 0;
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

    void legatoNoteOn(float note, const VoiceKnobs& knobs, const dsp::NoteStart& start) noexcept
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
        keyHeld = true;
        log(knobs.time, Gate::legatoOn);
        eventSinceTest = true;
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
        keyHeld = false;
        log(lastTime, Gate::off);
        eventSinceTest = true;
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
        bool callStart = true;
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
            renderChunk(out.subBlock(done, length), knobs, done, callStart);
            callStart = false;
            glide.advance(length);
            velocityRamp.advance(length);
            done += length;
            gridPosition = (gridPosition + length) % gridSamples;
            if (gridPosition == 0)
            {
                segmentBegun = false;
            }
        }
        lastTime = knobs.time + n;
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

    enum class Gate : std::uint8_t
    {
        on,
        legatoOn,
        off,
    };
    struct LoggedGate
    {
        std::uint64_t time = 0;
        Gate gate = Gate::on;
    };
    static constexpr std::size_t logCapacity = 32;

    [[nodiscard]] static dsp::AdsrGate adsrGate(Gate gate) noexcept
    {
        switch (gate)
        {
        case Gate::on:
            return dsp::AdsrGate::on;
        case Gate::legatoOn:
            return dsp::AdsrGate::legatoOn;
        case Gate::off:
            return dsp::AdsrGate::off;
        }
        return dsp::AdsrGate::off;
    }

    // --- The sources (new) --------------------------------------------------------------------

    [[nodiscard]] const SourceSettings& settings() const noexcept
    {
        static const SourceSettings defaults{};
        return context != nullptr && context->settings != nullptr ? *context->settings : defaults;
    }
    [[nodiscard]] const mod::CompiledRoutes& compiled() const noexcept
    {
        return compiler.routes();
    }
    [[nodiscard]] bool routesTo(Target t, std::size_t part) const noexcept
    {
        return compiled().routesTo(t, part);
    }

    /// Whether source s runs: always, or while a route reads it.
    [[nodiscard]] bool wanted(Source s) const noexcept
    {
        return settings().run == RunPolicy::always || compiled().uses(s);
    }

    void log(std::uint64_t time, Gate gate) noexcept
    {
        if (logCount < logCapacity)
        {
            gateLog[logCount] = {.time = time, .gate = gate};
        }
        ++logCount;
    }

    void applyEnvelopeSettings(std::size_t k) noexcept
    {
        const auto& s = settings().modEnvelopes[k];
        modEnvelopes[k].setAttack(s.attack);
        modEnvelopes[k].setDecay(s.decay);
        modEnvelopes[k].setSustain(s.sustain);
        modEnvelopes[k].setRelease(s.release);
    }

    [[nodiscard]] static Source modEnvSource(std::size_t k) noexcept
    {
        return k == 0 ? Source::modEnv1 : Source::modEnv2;
    }
    [[nodiscard]] static Source lfoSource(std::size_t k) noexcept
    {
        return k == 0 ? Source::lfo1 : Source::lfo2;
    }

    /// At every note-on: the gates, and each source's start.
    void sourcesNoteOn(const VoiceKnobs& knobs, bool sounding) noexcept
    {
        if (!sounding)
        {
            logCount = 0;
            startTime = knobs.time;
            compiler.reset();
            compiler.update(patchOrEmpty());
        }
        log(knobs.time, Gate::on);
        eventSinceTest = true;
        for (std::size_t k = 0; k < 2; ++k)
        {
            // Mod envelopes: gated as the amp envelopes; one that wasn't
            // running starts from 0.
            const auto s = modEnvSource(k);
            applyEnvelopeSettings(k);
            if (wanted(s))
            {
                if (!live[at(s)])
                {
                    modEnvelopes[k].reset();
                }
                modEnvelopes[k].gateOn();
                live[at(s)] = true;
            }
            else
            {
                modEnvelopes[k].gateOn(); // the gate is kept; it doesn't run
                live[at(s)] = false;
            }
            // LFOs: restarted from silence, retriggered on a sounding voice;
            // a free one copies the global instance.
            const auto l = lfoSource(k);
            const auto& ls = settings().lfos[k];
            const bool wasLive = live[at(l)];
            live[at(l)] = wanted(l);
            if (!sounding)
            {
                lfoLast[k] = 0.0f;
            }
            if (ls.free)
            {
                if (live[at(l)] && (!sounding || !wasLive || settings().freeCopy == FreeCopy::everyNoteOn))
                {
                    copyFree(k, knobs.offset);
                    if (sounding)
                    {
                        renderRestOfSegment(k);
                    }
                }
                continue;
            }
            if (!live[at(l)])
            {
                // Dormant: restarted only from silence, so that a replay of
                // its retriggers finds it as a running one would have been.
                if (!sounding)
                {
                    lfos[k].restart();
                }
                continue;
            }
            if (!sounding)
            {
                lfos[k].restart();
            }
            else if (!wasLive)
            {
                lfos[k].restart();
                renderRestOfSegment(k);
            }
            else
            {
                retriggerLfo(k);
            }
        }
    }

    /// An LFO that starts mid-segment (a note-on on a sounding voice): the
    /// rest of the segment from where it now stands, at the knobs' rate and
    /// gain until the next grid point.
    void renderRestOfSegment(std::size_t k) noexcept
    {
        if (!segmentBegun)
        {
            return;
        }
        const auto& ls = settings().lfos[k];
        lfos[k].setWaveform(ls.waveform);
        lfos[k].setFrequency(lfoRateHz(ls.rate));
        lfoGainBefore[k] = std::clamp(ls.gain, 0.0f, 10.0f) / 10.0f;
        lfoGainNow[k] = lfoGainBefore[k];
        lfoAtSegmentStart[k] = lfos[k];
        lfos[k].process(std::span{lfoWave[k]}.subspan(gridPosition));
    }

    /// A free LFO copies the global instance. The engine moves the global
    /// on with each piece it renders, so it stands at the start of the
    /// allocator's call: the copy is moved on to the voice's own sample,
    /// `ahead` samples later (0 at a note-on, which falls at a call's start).
    void copyFree(std::size_t k, std::size_t ahead) noexcept
    {
        if (context != nullptr && context->freeLfos != nullptr)
        {
            lfos[k] = (*context->freeLfos)[k];
            Chunk scratch{};
            while (ahead > 0)
            {
                const auto m = std::min(ahead, gridSamples);
                lfos[k].process(std::span{scratch}.first(m));
                ahead -= m;
            }
        }
    }

    /// A note-on on a sounding voice: the LFO's cycle restarts, slewed; in
    /// segment order mid-segment, the rest of the segment is rendered again
    /// from a copy taken at the grid point (task 2's decision).
    void retriggerLfo(std::size_t k) noexcept
    {
        if (live[at(lfoSource(k))] && segmentBegun)
        {
            lfos[k] = lfoAtSegmentStart[k];
            Chunk scratch{};
            lfos[k].process(std::span{scratch}.first(gridPosition));
            lfos[k].retrigger();
            lfos[k].process(std::span{lfoWave[k]}.subspan(gridPosition));
        }
        else
        {
            lfos[k].retrigger();
        }
    }

    [[nodiscard]] const mod::Patch& patchOrEmpty() const noexcept
    {
        static const mod::Patch empty{};
        return context != nullptr && context->patch != nullptr ? *context->patch : empty;
    }

    [[nodiscard]] float lfoGainFrom(std::size_t k, const mod::Sums& sums) const noexcept
    {
        const float units = settings().lfos[k].gain + sums.at(Target::lfoGain, k);
        return std::clamp(units, 0.0f, 10.0f) / 10.0f;
    }
    [[nodiscard]] float lfoRateFrom(std::size_t k, const mod::Sums& sums) const noexcept
    {
        return lfoRateHz(settings().lfos[k].rate + sums.at(Target::lfoRate, k));
    }
    [[nodiscard]] float lfoOffset(std::size_t k) const noexcept
    {
        return std::clamp(settings().lfos[k].offset, -10.0f, 10.0f) / 10.0f;
    }
    /// LFO k as a source at the segment's sample `position`.
    [[nodiscard]] float lfoValueAt(std::size_t k, std::size_t position) const noexcept
    {
        const float t = static_cast<float>(position) / static_cast<float>(gridSamples);
        const float g = lfoGainBefore[k] + ((lfoGainNow[k] - lfoGainBefore[k]) * t);
        return mod::lfoSourceValue(lfoWave[k][position], g, lfoOffset(k));
    }

    /// Each source as it stands at the block's sample `done` (envelopes and
    /// LFOs: their last output).
    void valuesNow(const VoiceKnobs& knobs, std::size_t done, mod::SourceValues& v) const noexcept
    {
        v = {};
        v[at(Source::velocity)] = velocityRamp.at(0);
        v[at(Source::keytrack)] = dsp::keytrackSource(keytrackPitch(knobs, done, 0));
        v[at(Source::ampEnvA)] = envelopes[0].level();
        v[at(Source::ampEnvB)] = envelopes[1].level();
        for (std::size_t k = 0; k < 2; ++k)
        {
            v[at(modEnvSource(k))] = live[at(modEnvSource(k))] ? modEnvelopes[k].level() : 0.0f;
            v[at(lfoSource(k))] = live[at(lfoSource(k))] ? lfoLast[k] : 0.0f;
        }
        const auto t = knobs.time + done;
        v[at(Source::modWheel)] =
            context != nullptr && context->modWheel != nullptr ? context->modWheel->valueAt(t) : 0.0f;
        v[at(Source::aftertouch)] =
            context != nullptr && context->aftertouch != nullptr ? context->aftertouch->valueAt(t) : 0.0f;
        v[at(Source::stepVelocity)] = stepVelocityNow();
    }

    [[nodiscard]] float stepVelocityNow() const noexcept
    {
        return context != nullptr && context->stepVelocity != nullptr ? *context->stepVelocity : 0.0f;
    }

    /// A source a route starts reading mid-note (or after it stopped).
    void lateStart(std::size_t k, bool envelope, const VoiceKnobs& knobs, std::size_t done) noexcept
    {
        const auto s = envelope ? modEnvSource(k) : lfoSource(k);
        if (settings().late == LateStart::nextNote)
        {
            return; // stays at 0 until the next note-on
        }
        live[at(s)] = true;
        if (!envelope)
        {
            lfoLast[k] = 0.0f; // as it stands: it hasn't produced anything yet
        }
        if (!envelope && settings().lfos[k].free)
        {
            copyFree(k, knobs.offset + done); // exact either way: the global instance is where it would be
            return;
        }
        const bool replay = settings().late == LateStart::reconstruct && logCount <= logCapacity;
        const std::uint64_t now = knobs.time + done;
        if (envelope)
        {
            modEnvelopes[k].reset();
            applyEnvelopeSettings(k);
            if (!replay)
            {
                modEnvelopes[k].gateOn();
                if (!keyHeld)
                {
                    modEnvelopes[k].gateOff();
                }
                return;
            }
            std::uint64_t cursor = startTime;
            for (std::size_t i = 0; i < logCount; ++i)
            {
                advanceEnvelope(k, gateLog[i].time - cursor);
                cursor = gateLog[i].time;
                modEnvelopes[k].apply(adsrGate(gateLog[i].gate));
            }
            advanceEnvelope(k, now - cursor);
            return;
        }
        // A non-free LFO: from its note-on state (restart() was called then),
        // with its retriggers replayed at the knob's rate.
        if (!replay)
        {
            lfos[k].restart();
            return;
        }
        std::uint64_t cursor = startTime;
        lfos[k].setWaveform(settings().lfos[k].waveform);
        lfos[k].setFrequency(lfoRateHz(settings().lfos[k].rate));
        for (std::size_t i = 1; i < logCount; ++i)
        {
            if (gateLog[i].gate == Gate::on)
            {
                advanceLfo(k, gateLog[i].time - cursor);
                cursor = gateLog[i].time;
                lfos[k].retrigger();
            }
        }
        advanceLfo(k, now - cursor);
    }

    void advanceEnvelope(std::size_t k, std::uint64_t n) noexcept
    {
        Chunk scratch{};
        while (n > 0)
        {
            const auto m = static_cast<std::size_t>(std::min<std::uint64_t>(n, gridSamples));
            modEnvelopes[k].process(std::span{scratch}.first(m));
            n -= m;
            replayed += m;
        }
    }
    void advanceLfo(std::size_t k, std::uint64_t n) noexcept
    {
        Chunk scratch{};
        while (n > 0)
        {
            const auto m = static_cast<std::size_t>(std::min<std::uint64_t>(n, gridSamples));
            lfos[k].process(std::span{scratch}.first(m));
            n -= m;
            replayed += m;
        }
    }

    /// At a grid point (or a note-on from silence): recompile if needed;
    /// start a source a new route reads; the grid sums from the sources as
    /// they stand; each LFO's rate and gain, and its wave for the segment.
    void beginSegment(const VoiceKnobs& knobs, std::size_t done, bool atNoteOn) noexcept
    {
        compiler.update(patchOrEmpty());
        for (std::size_t k = 0; k < 2; ++k)
        {
            if (!wanted(modEnvSource(k)))
            {
                live[at(modEnvSource(k))] = false;
            }
            else if (!live[at(modEnvSource(k))])
            {
                lateStart(k, true, knobs, done);
            }
            if (!wanted(lfoSource(k)))
            {
                live[at(lfoSource(k))] = false;
            }
            else if (!live[at(lfoSource(k))])
            {
                lateStart(k, false, knobs, done);
                if (live[at(lfoSource(k))])
                {
                    lfoGainNow[k] = -1.0f; // marks a start: the gain doesn't ramp from a stale value
                }
            }
        }
        mod::SourceValues now{};
        valuesNow(knobs, done, now);
        held = now;
        mod::Sums sums{};
        mod::accumulate(compiled(), now, sums);
        for (std::size_t k = 0; k < 2; ++k)
        {
            if (!live[at(lfoSource(k))])
            {
                continue;
            }
            const auto& ls = settings().lfos[k];
            lfos[k].setWaveform(ls.waveform);
            lfos[k].setFrequency(lfoRateFrom(k, sums));
            const float g = lfoGainFrom(k, sums);
            lfoGainBefore[k] = atNoteOn || lfoGainNow[k] < 0.0f ? g : lfoGainNow[k];
            lfoGainNow[k] = g;
            lfoAtSegmentStart[k] = lfos[k];
            lfos[k].process(lfoWave[k]);
        }
        gridSums = sums;
        segmentBegun = true;
        updatePerSample();
    }

    /// Which sources the per-sample targets (Pitch, Mix) read.
    void updatePerSample() noexcept
    {
        perSample = {};
        perSampleCount = 0;
        for (const auto& r : compiled().live())
        {
            if (r.target == Target::pitch || r.target == Target::mix)
            {
                perSample[at(r.source)] = true;
                perSampleRoutes.routes[perSampleCount] = r;
                ++perSampleCount;
            }
        }
        perSampleRoutes.count = perSampleCount;
    }

    /// Source s at the chunk's sample i, for the inline delivery and the
    /// bound check (the envelopes already in their buffers).
    [[nodiscard]] float valueAt(Source s, const VoiceKnobs& knobs, std::size_t done, std::size_t i) const noexcept
    {
        const bool heldLinear = settings().linearRate == LinearRate::held;
        switch (s)
        {
        case Source::velocity:
            return heldLinear ? held[at(s)] : velocityRamp.at(i);
        case Source::keytrack:
            return heldLinear ? held[at(s)] : dsp::keytrackSource(keytrackPitch(knobs, done, i));
        case Source::ampEnvA:
            return envelopeBuffers[0][i];
        case Source::ampEnvB:
            return envelopeBuffers[1][i];
        case Source::modEnv1:
        case Source::modEnv2:
        {
            const std::size_t k = s == Source::modEnv1 ? 0 : 1;
            return live[at(s)] ? modEnvelopeBuffers[k][i] : 0.0f;
        }
        case Source::lfo1:
        case Source::lfo2:
        {
            const std::size_t k = s == Source::lfo1 ? 0 : 1;
            return live[at(s)] ? lfoValueAt(k, gridPosition + i) : 0.0f;
        }
        case Source::modWheel:
            return context != nullptr && context->modWheel != nullptr
                       ? context->modWheel->valueAt(knobs.time + done + i)
                       : 0.0f;
        case Source::aftertouch:
            return context != nullptr && context->aftertouch != nullptr
                       ? context->aftertouch->valueAt(knobs.time + done + i)
                       : 0.0f;
        case Source::stepVelocity:
            return stepVelocityNow();
        default:
            return 0.0f;
        }
    }

    /// Whether source s is constant over the chunk (folded delivery).
    [[nodiscard]] bool constantOver(Source s, const VoiceKnobs& knobs, std::size_t done) const noexcept
    {
        const bool heldLinear = settings().linearRate == LinearRate::held;
        switch (s)
        {
        case Source::velocity:
            return heldLinear || !velocityRamp.isGliding();
        case Source::keytrack:
            return heldLinear ||
                   (!glide.isGliding() && (knobs.bend == nullptr || knobs.bend->isSteadyFrom(knobs.time + done)));
        case Source::modWheel:
            return context == nullptr || context->modWheel == nullptr ||
                   context->modWheel->isSteadyFrom(knobs.time + done);
        case Source::aftertouch:
            return context == nullptr || context->aftertouch == nullptr ||
                   context->aftertouch->isSteadyFrom(knobs.time + done);
        case Source::stepVelocity:
            return true;
        default:
            return false;
        }
    }

    /// The sources over the chunk: the mod envelopes (and amp envelopes,
    /// already run), the LFOs' values, and the per-sample sums of Pitch and
    /// Mix by the chosen delivery.
    void renderSources(const VoiceKnobs& knobs, std::size_t done, std::size_t length) noexcept
    {
        const auto delivery = settings().delivery;
        const bool inlineEnvelopes = delivery == Delivery::inlined;
        for (std::size_t k = 0; k < 2; ++k)
        {
            const auto s = modEnvSource(k);
            if (live[at(s)] && !(inlineEnvelopes && perSample[at(s)]))
            {
                modEnvelopes[k].process(std::span{modEnvelopeBuffers[k]}.first(length));
            }
        }
        for (auto const t : {Target::pitch, Target::mix})
        {
            for (std::size_t p = 0; p < partCount; ++p)
            {
                if (routesTo(t, p))
                {
                    std::fill_n(sampleSums[sumIndex(t)][p].begin(), length, 0.0f);
                }
            }
        }
        if (perSampleCount > 0)
        {
            if (delivery == Delivery::inlined)
            {
                sumInline(knobs, done, length);
            }
            else
            {
                sumBuffered(knobs, done, length, delivery == Delivery::folded);
            }
        }
        // The values as they stand after the chunk.
        for (std::size_t k = 0; k < 2; ++k)
        {
            if (live[at(lfoSource(k))])
            {
                lfoLast[k] = lfoValueAt(k, gridPosition + length - 1);
            }
        }
    }

    [[nodiscard]] static std::size_t sumIndex(Target t) noexcept
    {
        return t == Target::pitch ? 0 : 1;
    }

    /// (a) and (c): each per-sample source into a buffer (the envelopes'
    /// own), then each route summed over it; (c) adds a source constant over
    /// the chunk as one term.
    void sumBuffered(const VoiceKnobs& knobs, std::size_t done, std::size_t length, bool fold) noexcept
    {
        std::array<const float*, sourceCount> src{};
        std::array<bool, sourceCount> constant{};
        for (std::size_t s = 1; s < sourceCount; ++s)
        {
            if (!perSample[s])
            {
                continue;
            }
            const auto id = static_cast<Source>(s);
            if (id == Source::ampEnvA || id == Source::ampEnvB)
            {
                src[s] = envelopeBuffers[id == Source::ampEnvA ? 0 : 1].data();
                continue;
            }
            if ((id == Source::modEnv1 || id == Source::modEnv2) && live[s])
            {
                src[s] = modEnvelopeBuffers[id == Source::modEnv1 ? 0 : 1].data();
                continue;
            }
            if (fold && constantOver(id, knobs, done))
            {
                constant[s] = true;
                sourceBuffers[s][0] = valueAt(id, knobs, done, 0);
                src[s] = sourceBuffers[s].data();
                continue;
            }
            auto& b = sourceBuffers[s];
            for (std::size_t i = 0; i < length; ++i)
            {
                b[i] = valueAt(id, knobs, done, i);
            }
            src[s] = b.data();
        }
        for (std::size_t r = 0; r < perSampleCount; ++r)
        {
            const auto& route = perSampleRoutes.routes[r];
            const auto s = at(route.source);
            const float* values = src[s];
            for (std::size_t p = 0; p < partCount; ++p)
            {
                if (!mod::includes(route.parts, p))
                {
                    continue;
                }
                auto& dst = sampleSums[sumIndex(route.target)][p];
                if (constant[s])
                {
                    const float term = route.amount * values[0];
                    for (std::size_t i = 0; i < length; ++i)
                    {
                        dst[i] += term;
                    }
                    continue;
                }
                for (std::size_t i = 0; i < length; ++i)
                {
                    // The product in its own statement, as accumulate() forms it.
                    const float term = route.amount * values[i];
                    dst[i] += term;
                }
            }
        }
    }

    /// (b): every sample, each per-sample source into SourceValues (a mod
    /// envelope ticked with next()), then accumulate() over the per-sample
    /// routes.
    void sumInline(const VoiceKnobs& knobs, std::size_t done, std::size_t length) noexcept
    {
        mod::SourceValues v{};
        for (std::size_t i = 0; i < length; ++i)
        {
            for (std::size_t s = 1; s < sourceCount; ++s)
            {
                if (!perSample[s])
                {
                    continue;
                }
                const auto id = static_cast<Source>(s);
                if ((id == Source::modEnv1 || id == Source::modEnv2) && live[s])
                {
                    const std::size_t k = id == Source::modEnv1 ? 0 : 1;
                    const float e = modEnvelopes[k].next();
                    modEnvelopeBuffers[k][i] = e;
                    v[s] = e;
                    continue;
                }
                v[s] = valueAt(id, knobs, done, i);
            }
            mod::Sums sums{};
            mod::accumulate(perSampleRoutes, v, sums);
            for (auto const t : {Target::pitch, Target::mix})
            {
                for (std::size_t p = 0; p < partCount; ++p)
                {
                    if (routesTo(t, p))
                    {
                        sampleSums[sumIndex(t)][p][i] = sums.at(t, p);
                    }
                }
            }
        }
    }

    /// Source s's interval over the `length` samples from the block's
    /// sample `done` (before the chunk is rendered), by the chosen rule.
    [[nodiscard]] Interval boundOf(Source s, const VoiceKnobs& knobs, std::size_t done,
                                   std::size_t length) const noexcept
    {
        const auto rule = settings().bounds;
        const auto last = length - 1;
        const bool heldLinear = settings().linearRate == LinearRate::held;
        if (rule == BoundRule::levelOnly)
        {
            float v = 0.0f;
            switch (s)
            {
            case Source::ampEnvA:
                v = envelopes[0].level();
                break;
            case Source::ampEnvB:
                v = envelopes[1].level();
                break;
            case Source::modEnv1:
                v = live[at(s)] ? modEnvelopes[0].level() : 0.0f;
                break;
            case Source::modEnv2:
                v = live[at(s)] ? modEnvelopes[1].level() : 0.0f;
                break;
            case Source::lfo1:
            case Source::lfo2:
                v = live[at(s)] ? lfoValueAt(s == Source::lfo1 ? 0 : 1, gridPosition) : 0.0f;
                break;
            default:
                v = valueAt(s, knobs, done, 0);
                break;
            }
            return {.lo = v, .hi = v};
        }
        switch (s)
        {
        case Source::velocity:
            if (heldLinear)
            {
                return {.lo = held[at(s)], .hi = held[at(s)]};
            }
            return mod::linearBound(velocityRamp.at(0), velocityRamp.at(last));
        case Source::keytrack:
        {
            if (heldLinear)
            {
                return {.lo = held[at(s)], .hi = held[at(s)]};
            }
            if (rule == BoundRule::keytrackEnds)
            {
                return mod::linearBound(dsp::keytrackSource(keytrackPitch(knobs, done, 0)),
                                        dsp::keytrackSource(keytrackPitch(knobs, done, last)));
            }
            // Its parts: the glide and the bend are each monotonic between
            // events, but not their sum.
            const Interval g = mod::linearBound(glide.at(0), glide.at(last));
            const Interval b = mod::linearBound(bendAt(knobs, done), bendAt(knobs, done + last));
            const Interval pitch = g + b;
            return {
                .lo = dsp::keytrackSource(pitch.lo + masterTuneOffset),
                .hi = dsp::keytrackSource(pitch.hi + masterTuneOffset),
            };
        }
        case Source::ampEnvA:
        case Source::ampEnvB:
            return mod::envelopeBound(mod::envelopeState(envelopes[s == Source::ampEnvA ? 0 : 1]));
        case Source::modEnv1:
        case Source::modEnv2:
            if (!live[at(s)])
            {
                return {};
            }
            return mod::envelopeBound(mod::envelopeState(modEnvelopes[s == Source::modEnv1 ? 0 : 1]));
        case Source::lfo1:
        case Source::lfo2:
        {
            const std::size_t k = s == Source::lfo1 ? 0 : 1;
            if (!live[at(s)])
            {
                return {};
            }
            if (rule == BoundRule::lfoEnds)
            {
                return mod::linearBound(lfoValueAt(k, gridPosition), lfoValueAt(k, gridPosition + last));
            }
            const auto wave = mod::waveBound(std::span<const float>{lfoWave[k]}.subspan(gridPosition, length));
            const Interval gain = mod::linearBound(lfoGainBefore[k], lfoGainNow[k]);
            const float o = lfoOffset(k);
            return mod::lfoBound(wave, gain, {.lo = o, .hi = o});
        }
        case Source::modWheel:
        case Source::aftertouch:
        case Source::stepVelocity:
            return mod::linearBound(valueAt(s, knobs, done, 0), valueAt(s, knobs, done, last));
        default:
            return {};
        }
    }

    /// The bound on Mix's modulation (M1.4 task 5's seam), from the bounds
    /// of the sources its routes read.
    [[nodiscard]] float mixModulationBound(std::size_t part, const VoiceKnobs& knobs, std::size_t done,
                                           std::size_t length) const noexcept
    {
        if (!routesTo(Target::mix, part))
        {
            return 0.0f;
        }
        mod::SourceBounds bounds{};
        for (const auto& r : compiled().live())
        {
            if (r.target == Target::mix)
            {
                bounds[at(r.source)] = boundOf(r.source, knobs, done, length);
            }
        }
        return mod::targetBound(compiled(), Target::mix, part, bounds).hi;
    }

    /// The bound check: at each grid point and each call's start (where the
    /// skip test can re-test), every source's bound to the segment's end;
    /// every sample's value against it.
    void checkBounds(const VoiceKnobs& knobs, std::size_t done, std::size_t length, bool callStart) noexcept
    {
        if (gridPosition == 0 || callStart || eventSinceTest)
        {
            const std::size_t rest = gridSamples - gridPosition;
            for (std::size_t s = 1; s < sourceCount; ++s)
            {
                checkBound[s] = boundOf(static_cast<Source>(s), knobs, done, rest);
            }
            eventSinceTest = false;
        }
        // Velocity and keytrack, the LFOs and the controllers read here; the
        // envelopes from their buffers, after the chunk renders them.
        pendingCheck = true;
        checkDone = done;
        checkLength = length;
        checkKnobs = knobs;
    }

    void finishCheck() noexcept
    {
        if (!pendingCheck)
        {
            return;
        }
        pendingCheck = false;
        for (std::size_t s = 1; s < sourceCount; ++s)
        {
            const auto id = static_cast<Source>(s);
            for (std::size_t i = 0; i < checkLength; ++i)
            {
                const float v = valueAt(id, checkKnobs, checkDone, i);
                const auto& b = checkBound[s];
                ++stats.checked[s];
                stats.width[s] += static_cast<double>(b.hi - b.lo);
                if (!b.contains(v))
                {
                    ++stats.outside[s];
                    stats.worst[s] = std::max({stats.worst[s], b.lo - v, v - b.hi});
                }
            }
        }
    }

    // --- dsp::Voice, with the modulation's terms ------------------------------------------

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
        const float octaves = dsp::filterCutoffOctaves({
            .cutoff = read(knobs.cutoff[f], ahead),
            .cutoffModulation = gridSums.at(Target::cutoff, f),
            .controlCutoff = read(knobs.controlCutoff, ahead),
            .controlModulation = gridSums.at(Target::controlCutoff, 0),
            .keytrack = keytrack,
        });
        const float k = dsp::filterResonanceK({
            .resonance = read(knobs.resonance[f], ahead),
            .resonanceModulation = gridSums.at(Target::resonance, f),
            .controlResonance = read(knobs.controlResonance, ahead),
            .controlModulation = gridSums.at(Target::controlResonance, 0),
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
        const bool zeroHold = e.stage() == AdsrStage::sustain && e.level() <= 0.0f && e.sustainLevel() <= 0.0f;
        return !e.isActive() || zeroHold;
    }
    [[nodiscard]] bool mixSilent(std::size_t p, const VoiceKnobs& knobs, std::size_t done,
                                 std::size_t length) const noexcept
    {
        const auto* ramp = knobs.mix[p];
        const auto t = knobs.offset + done;
        const float knob =
            ramp != nullptr ? std::max(ramp->valueAfter(t), ramp->valueAfter(t + length - 1)) : dsp::mixLaw.unity;
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
        const auto t = knobs.offset + done;
        return std::max(ramp->valueAfter(t), ramp->valueAfter(t + length - 1)) <= 0.0f;
    }
    [[nodiscard]] bool gainSilent(std::size_t p, const VoiceKnobs& knobs, std::size_t done,
                                  std::size_t length) const noexcept
    {
        return envelopeSilent(p) || mixSilent(p, knobs, done, length) || soloSilent(p, knobs, done, length);
    }

    [[nodiscard]] Plan planChunk(const VoiceKnobs& knobs, std::size_t done) noexcept
    {
        const std::size_t rest = gridSamples - gridPosition;
        const bool skipping = !settings().neverSkip;
        std::array<bool, partCount> produces{};
        for (std::size_t p = 0; p < partCount; ++p)
        {
            produces[p] = partProduces(p) || !skipping;
            if (gridPosition == 0 || segmentSilent[p])
            {
                segmentSilent[p] = skipping && gainSilent(p, knobs, done, rest);
            }
        }
        Plan plan;
        for (std::size_t q = 0; q < partCount; ++q)
        {
            plan.crossing[q] = routes[1 - q].both && produces[q] && (!skipping || !envelopeSilent(q));
        }
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (!routes[f].active)
            {
                continue;
            }
            plan.output[f] = !segmentSilent[f];
            if (!filters[f].isBypassed() && skipping)
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

    void renderChunk(const AudioBlock& out, const VoiceKnobs& knobs, std::size_t done, bool callStart) noexcept
    {
        const auto length = out.numSamples();
        leaveMonoIfMoved();
        const std::size_t channels = mono ? 1 : 2;
        const Plan plan = planChunk(knobs, done);
        if (settings().checkBounds)
        {
            checkBounds(knobs, done, length, callStart);
        }

        // 0. The amp envelopes (sources too: nothing they compute depends on
        // the slots), then the other sources and the per-sample sums.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            const bool run = settings().ampEnvelopes == AmpEnvelopeRun::always || heard[p] ||
                             compiled().uses(p == 0 ? Source::ampEnvA : Source::ampEnvB);
            if (run)
            {
                envelopes[p].process(std::span{envelopeBuffers[p]}.first(length));
            }
        }
        const bool modulated = !compiled().empty() || settings().run == RunPolicy::always;
        if (modulated)
        {
            renderSources(knobs, done, length);
        }
        finishCheck();
        for (std::size_t s = 1; s < sourceCount && settings().checkBounds; ++s)
        {
            lastValues[s] = valueAt(static_cast<Source>(s), knobs, done, length - 1);
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
        std::array<bool, partCount> pitchRouted{};
        for (std::size_t p = 0; p < partCount; ++p)
        {
            pitchRouted[p] = routesTo(Target::pitch, p);
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
                const bool skippable = !settings().neverSkip && (!plan.render[p] || slot.isSilent()) && slot.canSkip();
                slotSkipping[index] = skippable && (gridPosition == 0 || slotSkipping[index]);
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
                                   const bool perSampleNotes = moving || pitchRouted[p];
                                   std::span<const float> partNotes = std::span<const float>{notes}.first(run);
                                   if (pitchRouted[p])
                                   {
                                       auto& own = partNoteBuffers[p];
                                       const auto& sums = sampleSums[0][p];
                                       for (std::size_t i = 0; i < run; ++i)
                                       {
                                           const float base = moving ? notes[i] : steadyPitch + offset;
                                           own[i] = base + sums[start + i];
                                       }
                                       partNotes = std::span<const float>{own}.first(run);
                                   }
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
                                           if (perSampleNotes)
                                           {
                                               slot.skip(partNotes);
                                           }
                                           else
                                           {
                                               slot.skip(run, steadyPitch + offset);
                                           }
                                           continue;
                                       }
                                       if (!plan.render[p] && !cleared[p])
                                       {
                                           parts[p].clear();
                                           cleared[p] = true;
                                       }
                                       if (perSampleNotes)
                                       {
                                           slot.process(piece, partNotes);
                                       }
                                       else
                                       {
                                           slot.process(piece, steadyPitch + offset);
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

    /// Part p's Mix gain per sample: the knob plus its modulation (per
    /// sample, while routed), then times Solo.
    void fillMixGains(std::size_t p, const VoiceKnobs& knobs, std::size_t done, std::size_t length) noexcept
    {
        auto& g = mixGains[p];
        const auto t = knobs.offset + done;
        if (routesTo(Target::mix, p))
        {
            const auto* ramp = knobs.mix[p];
            const auto& m = sampleSums[1][p];
            for (std::size_t i = 0; i < length; ++i)
            {
                const float knob = ramp != nullptr ? ramp->valueAfter(t + i) : dsp::mixLaw.unity;
                g[i] = dsp::mixGain({.mix = knob, .modulation = m[i]});
            }
        }
        else
        {
            fillKnobGains(g, knobs.mix[p], t, length);
        }
        const auto* solo = knobs.solo[p];
        if (solo == nullptr)
        {
            return;
        }
        const float first = solo->valueAfter(t);
        const float last = solo->valueAfter(t + length);
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
            g[i] *= solo->valueAfter(t + i);
        }
    }
    static void fillKnobGains(Chunk& g, const LinearSmoother* ramp, std::size_t t, std::size_t length) noexcept
    {
        if (ramp == nullptr)
        {
            std::fill_n(g.begin(), length, dsp::mixGain({.mix = dsp::mixLaw.unity}));
            return;
        }
        const float first = ramp->valueAfter(t);
        const float last = ramp->valueAfter(t + length);
        if (first <= last && first >= last)
        {
            std::fill_n(g.begin(), length, dsp::mixGain({.mix = first}));
            return;
        }
        for (std::size_t i = 0; i < length; ++i)
        {
            g[i] = dsp::mixGain({.mix = ramp->valueAfter(t + i)});
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
    std::array<dsp::LadderFilter, partCount> filters{};
    std::array<dsp::Adsr, partCount> envelopes{};
    dsp::PitchDrift pitchDrift;
    const SourceContext* context = nullptr;

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
    float masterTuneOffset = 0.0f;
    bool played = false;
    std::size_t gridPosition = 0;
    bool mono = false;
    std::array<bool, partCount> segmentSilent{};
    std::array<bool, slotCount> slotSkipping{};

    // The sources.
    std::array<dsp::Adsr, 2> modEnvelopes{};
    std::array<dsp::Lfo, 2> lfos{dsp::Lfo{}, dsp::Lfo{}};
    std::array<dsp::Lfo, 2> lfoAtSegmentStart{dsp::Lfo{}, dsp::Lfo{}};
    std::array<Chunk, 2> modEnvelopeBuffers{};
    std::array<Chunk, 2> lfoWave{};
    std::array<float, 2> lfoGainBefore{};
    std::array<float, 2> lfoGainNow{};
    std::array<float, 2> lfoLast{};
    mod::RouteCompiler compiler;
    std::array<bool, sourceCount> live{};
    std::array<bool, sourceCount> perSample{};
    mod::CompiledRoutes perSampleRoutes{};
    std::size_t perSampleCount = 0;
    std::array<Chunk, sourceCount> sourceBuffers{};
    std::array<std::array<Chunk, partCount>, 2> sampleSums{}; ///< Pitch, Mix: per part, per sample
    std::array<Chunk, partCount> partNoteBuffers{};
    mod::Sums gridSums{};
    mod::SourceValues held{};
    bool segmentBegun = false;
    bool keyHeld = false;
    std::uint64_t startTime = 0;
    std::uint64_t lastTime = 0;
    std::array<LoggedGate, logCapacity> gateLog{};
    std::size_t logCount = 0;
    std::uint64_t replayed = 0;

    // The bound check.
    BoundStats stats;
    std::array<Interval, sourceCount> checkBound{};
    std::array<float, sourceCount> lastValues{};
    bool eventSinceTest = false;
    bool pendingCheck = false;
    std::size_t checkDone = 0;
    std::size_t checkLength = 0;
    VoiceKnobs checkKnobs{};
};

// --- The engine ----------------------------------------------------------------------------

struct SourceEvent
{
    enum class Kind : std::uint8_t
    {
        voice,        ///< a VoiceEvent (notes, bend, pedal, ...)
        modWheel,     ///< CC 1, value 0-127
        aftertouch,   ///< channel pressure, 0-127
        slot,         ///< the patch's slot `slot` set to `route`
        stepVelocity, ///< the stand-in's value, 0-1
    };
    std::size_t offset = 0;
    Kind kind = Kind::voice;
    dsp::VoiceEvent voice{};
    int value = 0;
    std::size_t slot = 0;
    mod::Slot route{};
    float level = 0.0f;
};

/// The real VoiceAllocator over 16 SourceVoices and the engine's state: the
/// modulation::Patch, the controllers, the free LFOs' global instances, Step
/// Velocity, and the filter knobs. The output is written, not added to.
class SourceEngine
{
public:
    using Voice = SourceVoice<>;
    using Allocator = dsp::VoiceAllocator<Voice, 16>;

    SourceEngine(const MinBlepTable& table, const SourceSettings& sourceSettings, std::uint64_t seed = 1)
        : settings(sourceSettings), scratch(4096)
    {
        context = {
            .patch = &patch,
            .settings = &settings,
            .modWheel = &modWheel,
            .aftertouch = &aftertouch,
            .freeLfos = &freeLfos,
            .stepVelocity = &stepVelocity,
        };
        allocator = std::make_unique<Allocator>(seed, table, &context);
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
    [[nodiscard]] mod::Patch& modulation() noexcept
    {
        return patch;
    }
    [[nodiscard]] SourceSettings& sourceSettings() noexcept
    {
        return settings;
    }
    [[nodiscard]] const std::array<dsp::Lfo, 2>& globalLfos() const noexcept
    {
        return freeLfos;
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
    void setControllers(Smoothing mode, float milliseconds) noexcept
    {
        smoothing = mode;
        smoothingMs = milliseconds;
    }

    void prepare(const ProcessSpec& spec)
    {
        allocator->prepare(spec);
        modWheel.configure(smoothing, spec.sampleRate, smoothingMs);
        aftertouch.configure(smoothing, spec.sampleRate, smoothingMs);
        for (std::size_t k = 0; k < 2; ++k)
        {
            freeLfos[k].prepare(spec);
            freeLfos[k].setWaveform(settings.lfos[k].waveform);
            freeLfos[k].setFrequency(lfoRateHz(settings.lfos[k].rate));
            freeLfos[k].restart();
        }
        time = 0;
    }

    /// One host block, its events in order of offset.
    void process(const AudioBlock& out, std::span<const SourceEvent> events) noexcept
    {
        out.clear();
        const auto n = out.numSamples();
        modWheel.beginBlock(time, n);
        aftertouch.beginBlock(time, n);
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
                    modWheel.set(time + done, static_cast<float>(ev.value) / 127.0f);
                    break;
                case SourceEvent::Kind::aftertouch:
                    aftertouch.set(time + done, static_cast<float>(ev.value) / 127.0f);
                    break;
                case SourceEvent::Kind::slot:
                    patch.set(ev.slot, ev.route);
                    break;
                case SourceEvent::Kind::stepVelocity:
                    stepVelocity = ev.level;
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
            // The free LFOs move on with the piece, so a note-on at its end
            // copies them where they stand.
            for (auto& l : freeLfos)
            {
                l.process(std::span{scratch}.first(next - done));
            }
            done = next;
        }
        time += n;
    }

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

    SourceSettings settings;
    mod::Patch patch;
    Controller modWheel;
    Controller aftertouch;
    Smoothing smoothing = Smoothing::linear;
    float smoothingMs = 5.0f;
    std::array<dsp::Lfo, 2> freeLfos{dsp::Lfo{}, dsp::Lfo{}};
    float stepVelocity = 0.0f;
    SourceContext context;
    std::unique_ptr<Allocator> allocator;
    std::array<LinearSmoother, 2> cutoff{};
    std::array<LinearSmoother, 2> resonance{};
    LinearSmoother controlCutoff;
    LinearSmoother controlResonance;
    std::vector<float> scratch;
    std::uint64_t time = 0;
};

} // namespace anoesis::test::sources
