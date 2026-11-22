// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The AmpEnv A/B link candidates (M1.4 task 6, stage 1; docs/dsp/
// amp-envelope-link.md). Prototypes for measurement, not library code.
//
// LinkVoice is a copy of dsp::Voice as built (tasks 1-5). With LinkKind::none
// it is dsp::Voice to the bit (the report checks it). What it adds, chosen
// through LinkSettings:
//
// - parameter: the DSP of dsp::Voice, plus pending envelope settings that a
//   part's envelope takes at the voice's next note-on (for the "next note"
//   transition of a parameter link; the engine mirrors the knobs).
// - shared: a parameter link in which the voice runs one envelope (the
//   master's) for both parts while linked, from a note-on from silence (when
//   the two are the same to the bit); at unlink the other envelope takes the
//   master's state and runs again.
// - signal: the other part's gain reads the master's envelope; its own knobs
//   are kept and its own envelope keeps running on them, unused. The switch
//   is a snap, a 10 ms linear crossfade, or waits for the voice's next
//   note-on (not a legato note); at a note-on from silence it is immediate.
//
// Wherever a part's envelope is read (the output stage, a crossing part
// before the other filter, task 5's skip test, isActive() and the readout),
// LinkVoice reads the envelope that applies; `wrongReading` reads each
// part's own instead, to measure what that costs.
//
// LinkEngine runs the real dsp::VoiceAllocator over 16 voices (dsp::Voice or
// LinkVoice), with the envelope knobs of both parts and the link as
// engine-level state (a stand-in for M1.9's parameter layer: mirroring, the
// master, restoring on unlink), splitting each host block at its events.

#pragma once

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
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

namespace anoesis::test::amp_link
{

namespace dsp = anoesis::dsp;

using dsp::Adsr;
using dsp::AdsrStage;
using dsp::AudioBlock;
using dsp::concertPitchHz;
using dsp::filterControlLaw;
using dsp::filterCutoffOctaves;
using dsp::FilterInput;
using dsp::filterResonanceK;
using dsp::FilterResponse;
using dsp::Glide;
using dsp::keytrackOctaves;
using dsp::LadderFilter;
using dsp::LinearSmoother;
using dsp::masterTuneSemitones;
using dsp::MinBlepTable;
using dsp::minBlepTaps;
using dsp::mixGain;
using dsp::mixLaw;
using dsp::NoteStart;
using dsp::OscillatorSlot;
using dsp::PitchDrift;
using dsp::ProcessSpec;
using dsp::referenceNote;
using dsp::VoiceKnobs;

// --- The candidates' switches -------------------------------------------------------------

enum class LinkKind : std::uint8_t
{
    none,      ///< dsp::Voice: no link in the voice
    parameter, ///< (a) the knobs mirrored by the engine; the DSP unchanged
    shared,    ///< (b) (a), and one envelope run for both parts while linked
    signal,    ///< (c) the other part's gain reads the master's envelope
};

/// How a signal link (or a parameter link's settings) changes mid-note.
enum class LinkTransition : std::uint8_t
{
    snap,     ///< at once
    ramp,     ///< a signal link: a 10 ms linear crossfade between the envelopes
    nextNote, ///< at the voice's next note-on (not a legato note)
};

struct LinkSettings
{
    LinkKind kind = LinkKind::none;
    LinkTransition transition = LinkTransition::snap;
    std::size_t master = 0;    ///< the part whose envelope (or knobs) the other follows
    bool wrongReading = false; ///< the skip test and isActive() read each part's own envelope
};

/// One envelope's four knobs, in the DSP's units (seconds, a linear level).
struct EnvelopeParams
{
    float attack = 0.0f;
    float decay = 0.0f;
    float sustain = 1.0f;
    float release = 0.0f;
};

inline void applyParams(Adsr& e, const EnvelopeParams& p) noexcept
{
    e.setAttack(p.attack);
    e.setDecay(p.decay);
    e.setSustain(p.sustain);
    e.setRelease(p.release);
}

/// The skip test's bookkeeping, for the report.
struct LinkStats
{
    std::uint64_t chunks = 0;         ///< chunks a filter with an input saw
    std::uint64_t outputsSkipped = 0; ///< of those, with its output stage not run
    void add(const LinkStats& s) noexcept
    {
        chunks += s.chunks;
        outputsSkipped += s.outputsSkipped;
    }
};

template <std::size_t Taps = minBlepTaps>
class LinkVoice
{
public:
    using Slot = OscillatorSlot<Taps>;
    using Knobs = VoiceKnobs;
    static constexpr std::size_t partCount = 2;
    static constexpr std::size_t slotsPerPart = 2;
    static constexpr std::size_t slotCount = partCount * slotsPerPart;
    static constexpr std::size_t gridSamples = LadderFilter::gridSamples;
    /// The filter input level's range; anything else (NaN, 0, negative, inf)
    /// counts as 1.
    static constexpr float minInputLevel = 1e-3f;
    static constexpr float maxInputLevel = 1e3f;

    /// The table must outlive the voice. `seed` sets every random sequence
    /// (the slots' free-running phases and noise, the drift): give each voice
    /// its own.
    explicit LinkVoice(const MinBlepTable& residuals, std::uint64_t seed = 0) noexcept
        : slots{Slot{residuals}, Slot{residuals}, Slot{residuals}, Slot{residuals}}
    {
        for (std::size_t s = 0; s < slotCount; ++s)
        {
            slots[s].setRandomSeed(seed ^ (0x9E3779B97F4A7C15U * (s + 1)));
        }
        pitchDrift.setSeed(seed ^ 0x3C6EF372FE94F82BU);
        updateRoutes();
    }

    /// Slots 0 and 1 are A1 and A2; 2 and 3 are B1 and B2.
    [[nodiscard]] Slot& slot(std::size_t index) noexcept
    {
        return slots[index];
    }
    [[nodiscard]] const Slot& slot(std::size_t index) const noexcept
    {
        return slots[index];
    }
    /// The amp envelope of part 0 (A) or 1 (B).
    [[nodiscard]] Adsr& envelope(std::size_t part) noexcept
    {
        return envelopes[part];
    }
    [[nodiscard]] const Adsr& envelope(std::size_t part) const noexcept
    {
        return envelopes[part];
    }
    [[nodiscard]] PitchDrift& drift() noexcept
    {
        return pitchDrift;
    }

    // --- The link (the candidates) --------------------------------------------------------

    /// Before prepare().
    void setLinkSettings(const LinkSettings& s) noexcept
    {
        link = s;
    }
    [[nodiscard]] const LinkSettings& linkSettings() const noexcept
    {
        return link;
    }
    /// The link turned on or off. shared: one envelope from the next note-on
    /// from silence; at unlink the other envelope takes the master's state.
    /// signal: the other part's gain moves to (or from) the master's envelope
    /// as the transition says; at once on a voice that isn't sounding.
    void setLinked(bool on) noexcept
    {
        linked = on;
        const auto other = 1 - link.master;
        switch (link.kind)
        {
        case LinkKind::shared:
            if (!on && sharing)
            {
                envelopes[other] = envelopes[link.master];
                sharing = false;
            }
            break;
        case LinkKind::signal:
            following[other] = on;
            if (!isActive() || link.transition == LinkTransition::snap)
            {
                follow[other] = on ? rampSamples : 0;
            }
            break;
        case LinkKind::none:
        case LinkKind::parameter:
            break;
        }
    }
    [[nodiscard]] bool isLinked() const noexcept
    {
        return linked;
    }
    /// A part's envelope settings, taken at the voice's next note-on (not a
    /// legato note): a parameter link's "next note" transition.
    void setPendingEnvelope(std::size_t part, const EnvelopeParams& p) noexcept
    {
        pendingParams[part] = p;
        hasPendingParams[part] = true;
    }
    /// Whether one envelope runs for both parts (shared).
    [[nodiscard]] bool isSharing() const noexcept
    {
        return sharing;
    }
    /// The skip test's bookkeeping since prepare().
    [[nodiscard]] const LinkStats& stats() const noexcept
    {
        return counters;
    }
    /// A part's filter, for measurement. The voice owns its controls: use the
    /// setters below.
    [[nodiscard]] const LadderFilter& filter(std::size_t part) const noexcept
    {
        return filters[part];
    }

    /// From the next grid point while a note sounds; at once before the
    /// first note-on, at a note-on, and at reset().
    void setFilterInput(std::size_t part, FilterInput input) noexcept
    {
        pendingInputs[part] = input;
    }
    [[nodiscard]] FilterInput filterInput(std::size_t part) const noexcept
    {
        return pendingInputs[part];
    }
    /// As setFilterInput(). Leaving Bypass starts the filter from rest.
    void setFilterBypassed(std::size_t part, bool bypassed) noexcept
    {
        pendingBypass[part] = bypassed;
    }
    /// Crossfaded by the filter over 10 ms.
    void setFilterResponse(std::size_t part, FilterResponse response) noexcept
    {
        filters[part].setResponse(response);
    }
    /// The drive gain (inventory F8), ramped by the filter over 10 ms.
    void setFilterDrive(std::size_t part, float gain) noexcept
    {
        filters[part].setDrive(gain);
    }
    /// Warm Drive, for both filters.
    void setWarm(bool on) noexcept
    {
        for (auto& f : filters)
        {
            f.setWarm(on);
        }
    }
    /// The filter input level g (inventory V8): each filter's input is scaled
    /// by g and its output by 1/g, so the clean level is unchanged and the
    /// saturation's knee moves. 1 by default; values outside
    /// [minInputLevel, maxInputLevel], and NaN, count as 1.
    void setFilterInputLevel(float gain) noexcept
    {
        const bool valid = gain >= minInputLevel && gain <= maxInputLevel; // false for NaN
        inputLevel = valid ? gain : 1.0f;
        outputLevel = 1.0f / inputLevel;
        unityLevel = inputLevel >= 1.0f && inputLevel <= 1.0f; // exactly 1, without -Wfloat-equal
    }
    [[nodiscard]] float filterInputLevel() const noexcept
    {
        return inputLevel;
    }

    /// Whether the voice is on its mono path (channel 0 only, copied to
    /// both), for measurement: taken at a note-on from silence when every
    /// sounding slot is centred, left for good within the note at the first
    /// chunk where one isn't.
    [[nodiscard]] bool isMono() const noexcept
    {
        return mono;
    }

    /// Stereo throughout; nothing is allocated.
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
        pitchDrift.prepare(spec);
        glide.prepare(spec.sampleRate);
        velocityRamp.prepare(spec.sampleRate);
        rampSamples = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(0.01 * spec.sampleRate)));
        counters = {};
        reset();
    }

    /// As freshly prepared: every random sequence restarts from the seed.
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
        glide.jump(referenceNote);
        velocityRamp.jump(1.0f);
        played = false;
        mono = false;
        segmentSilent = {};
        slotSkipping = {};
        sharing = false; // pending settings are kept, like the envelopes' own
        for (std::size_t p = 0; p < partCount; ++p)
        {
            follow[p] = following[p] ? rampSamples : 0;
        }
    }

    /// A note-on before the next process() call, with the knobs positioned at
    /// its sample. The pitch jumps to `note`, or glides to it from
    /// `start.from` over `start.glideSeconds` (linear in semitones); the
    /// velocity is `start.velocity`, or, while it glides, ramps there from
    /// `start.fromVelocity` over the same time. The slots, the drift and both
    /// envelopes start (or, on a sounding voice, retrigger: the slots as
    /// Retrig says, the envelopes attacking from their level).
    ///
    /// - From silence (isActive() false): pending routing applies, and each
    ///   filter is fed for this sample and reset(), so the voice's grid
    ///   starts here; the voice takes the mono path if every sounding slot is
    ///   centred.
    /// - On a sounding voice (a steal, or a retrigger in place): the filters
    ///   keep their state and the voice keeps its grid, counted from the
    ///   note-on from silence; the new note's keytrack reaches the filters at
    ///   the next grid point, and pending routing applies there, as during
    ///   any note (M1.4 task 2: a reset there clicks as hard as a cut). The
    ///   oscillators take the new pitch at once.
    ///
    /// A voice that has finished should be stop()ped before its next note.
    void noteOn(float note, const VoiceKnobs& knobs, const NoteStart& start = {}) noexcept
    {
        const bool sounding = isActive();
        for (std::size_t p = 0; p < partCount; ++p)
        {
            if (hasPendingParams[p])
            {
                applyParams(envelopes[p], pendingParams[p]);
                hasPendingParams[p] = false;
            }
            if (!sounding || link.transition == LinkTransition::nextNote)
            {
                follow[p] = following[p] ? rampSamples : 0;
            }
        }
        if (!sounding)
        {
            sharing = link.kind == LinkKind::shared && linked;
        }
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
        gridPosition = 0;
        segmentSilent = {};
        slotSkipping = {};
        mono = allCentred();
    }

    /// A legato note (Mono Legato, M1.4 task 3): the pitch jumps or glides to
    /// `note` as at noteOn(), and the velocity ramps from where it is to
    /// `start.velocity` over the glide (at once without one). Nothing else
    /// restarts: the envelopes are left to Adsr::legatoGateOn() (a held one
    /// goes on; a released one opens again), the oscillators keep their
    /// phases whatever Retrig says, and the drift, the filters and the grid
    /// carry on. On a voice that isn't sounding it is noteOn().
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
    }

    void noteOff() noexcept
    {
        for (auto& e : envelopes)
        {
            e.gateOff();
        }
    }

    /// The master tune: the frequency of A4 in Hz, for the four slots and for
    /// keytrack (inventory F19: the filter follows it). Anything not finite
    /// and positive counts as 440.
    void setMasterTune(float referenceHz) noexcept
    {
        const bool valid = std::isfinite(referenceHz) && referenceHz > 0.0f;
        masterTuneHz = valid ? referenceHz : concertPitchHz;
        masterTuneOffset = masterTuneSemitones(masterTuneHz);
        for (auto& s : slots)
        {
            s.setReferenceHz(masterTuneHz);
        }
    }
    [[nodiscard]] float masterTune() const noexcept
    {
        return masterTuneHz;
    }

    /// The pitch the glide has reached (a fractional note, without the bend
    /// or the master tune), and the note it is heading for. stop() keeps
    /// them, so a note after the voice has finished can glide from there;
    /// reset() puts them at A4.
    [[nodiscard]] float currentPitch() const noexcept
    {
        return glide.at(0);
    }
    [[nodiscard]] float targetPitch() const noexcept
    {
        return glide.target();
    }
    /// Whether a note has played since prepare() or reset().
    [[nodiscard]] bool hasPlayed() const noexcept
    {
        return played;
    }
    /// The note's velocity now, 0-1 (the velocity modulation source, M1.5).
    [[nodiscard]] float velocity() const noexcept
    {
        return velocityRamp.at(0);
    }

    /// Silences the voice at once: the slots and the drift stop and the
    /// envelopes go idle (the next note-on, from silence, resets the filters
    /// and the grid). Unlike reset(), the
    /// random sequences carry on, so the next note from silence draws new
    /// free-running phases and a new drift state. Call it when the voice
    /// has finished (isActive() false) and before it takes another note: a
    /// finished voice that isn't processed still counts its slots as
    /// sounding, so its next note would jump from where they stopped.
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

    /// True while anything can still be heard: a filter with an input
    /// (not "--") that has oscillators in it (its own part's, or the other
    /// part's on AB), whose part's amp envelope hasn't released to 0. An
    /// envelope on a part nothing hears, or with no oscillators, doesn't
    /// count; nor does a crossing part's envelope once the envelope after
    /// its filter has closed. False before the first note-on.
    [[nodiscard]] bool isActive() const noexcept
    {
        for (std::size_t f = 0; f < partCount; ++f)
        {
            const bool fed = partSounds(f) || (routes[f].both && partSounds(1 - f));
            if (routes[f].active && fed && envelopeActive(f))
            {
                return true;
            }
        }
        return false;
    }

    /// The Voices readout's active count for this voice (manual p. 11;
    /// decided 2026-11-21, docs/dsp/voice-allocation.md): the unison voices
    /// of each part whose amp envelope is active and which something hears
    /// (its filter has an input, or its oscillators cross into the other
    /// filter). 0 before the first note-on.
    [[nodiscard]] std::size_t activeOscillatorVoices() const noexcept
    {
        std::size_t n = 0;
        for (std::size_t p = 0; p < partCount; ++p)
        {
            if (heard[p] && envelopeActive(p))
            {
                n += partVoices(p);
            }
        }
        return n;
    }

    /// Every oscillator's unison voices: the manual's voices per note, which
    /// times Polyphony is the readout's maximum. An oscillator at Voices 0
    /// adds nothing.
    [[nodiscard]] std::size_t oscillatorVoicesPerNote() const noexcept
    {
        return partVoices(0) + partVoices(1);
    }

    /// Adds the voice into channels 0 and 1 of `out` (two channels needed),
    /// with the knobs positioned at its first sample.
    void process(const AudioBlock& out, const VoiceKnobs& knobs) noexcept
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
            done += length;
            gridPosition = (gridPosition + length) % gridSamples;
        }
    }

private:
    using Chunk = std::array<float, gridSamples>;
    using StereoChunk = std::array<Chunk, 2>;

    struct Route
    {
        bool active = false; ///< the filter has an input (not "--")
        bool both = false;   ///< AB
    };

    /// What one chunk runs.
    struct Plan
    {
        std::array<bool, partCount> output{};   ///< the filter's output is added: its gain can be above 0
        std::array<bool, partCount> skip{};     ///< the filter isn't solved (LadderFilter::skip())
        std::array<bool, partCount> render{};   ///< the part's slots are rendered (else skipped, where exact)
        std::array<bool, partCount> crossing{}; ///< the part is added into the other filter's input (AB)
    };

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

    /// The bend `ahead` samples into the block these knobs are for.
    [[nodiscard]] static float bendAt(const VoiceKnobs& knobs, std::size_t ahead) noexcept
    {
        return knobs.bend != nullptr ? knobs.bend->semitonesAt(knobs.time + ahead) : 0.0f;
    }

    /// The oscillators' pitch `ahead` samples after the block's sample `done`,
    /// where the glide stands (the glide plus the bend; the master tune is
    /// the slots').
    [[nodiscard]] float pitchAt(const VoiceKnobs& knobs, std::size_t done, std::size_t ahead) const noexcept
    {
        return glide.at(ahead) + bendAt(knobs, done + ahead);
    }

    /// The pitch keytrack follows (F18, F19): the oscillators' plus the
    /// master tune.
    [[nodiscard]] float keytrackPitch(const VoiceKnobs& knobs, std::size_t done, std::size_t ahead) const noexcept
    {
        return pitchAt(knobs, done, ahead) + masterTuneOffset;
    }

    [[nodiscard]] std::size_t partVoices(std::size_t p) const noexcept
    {
        return slots[p * slotsPerPart].voices() + slots[(p * slotsPerPart) + 1].voices();
    }

    /// Whether part p has an oscillator voice on, or one still fading out.
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

    /// Every sounding slot's left channel is its right, to the bit.
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

    /// At a grid point: a filter that comes on (from "--" or out of Bypass)
    /// is fed for this sample and starts from rest, on the voice's grid.
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
                feedFilter(f, knobs, ahead, keytrackOctaves(keytrackPitch(knobs, ahead, 0), knobs.keytrack / 10.0f));
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
        ahead += knobs.offset; // the ramps stand that many samples before this block
        const float octaves = filterCutoffOctaves({
            .cutoff = read(knobs.cutoff[f], ahead),
            .controlCutoff = read(knobs.controlCutoff, ahead),
            .keytrack = keytrack,
        });
        const float k = filterResonanceK({
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
        filters[f].setCutoff(filterControlLaw.anchorHz);
        filters[f].setCutoffModulation(lastOctaves[f]);
        filters[f].setResonance(0.0f);
        filters[f].setResonanceModulation(lastK[f]);
    }

    /// Each running filter, for the sample `ahead` samples into the block.
    void feedFilters(const VoiceKnobs& knobs, std::size_t ahead, float pitch) noexcept
    {
        const float keytrack = keytrackOctaves(pitch, knobs.keytrack / 10.0f); // once for both
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

    // --- The skip test (docs/dsp/part-solo-and-skipping.md) ----------------------------------

    /// Part p's amp envelope can't be above 0 for the rest of this
    /// process() call: idle, or held at a sustain of exactly 0 (a gate, or
    /// setSustain(), only acts between calls). With AmpEnv A/B link (M1.4
    /// task 6), this must read the envelope that applies to the part, as
    /// isActive() must.
    [[nodiscard]] static bool adsrSilent(const Adsr& e) noexcept
    {
        const bool zeroHold = e.stage() == AdsrStage::sustain && e.level() <= 0.0f && e.sustainLevel() <= 0.0f;
        return !e.isActive() || zeroHold;
    }

    /// Which envelopes apply to part p now: its own, the master's, or both
    /// (a signal link's crossfade).
    struct Applies
    {
        bool own = true;
        bool master = false;
    };
    [[nodiscard]] Applies applies(std::size_t p) const noexcept
    {
        if (link.wrongReading)
        {
            return {};
        }
        if (sharing && p != link.master)
        {
            return {.own = false, .master = true};
        }
        if (link.kind != LinkKind::signal || p == link.master)
        {
            return {};
        }
        if (!following[p] && follow[p] == 0)
        {
            return {};
        }
        if (following[p] && follow[p] == rampSamples)
        {
            return {.own = false, .master = true};
        }
        if (link.transition != LinkTransition::ramp)
        {
            // Waiting for the next note: whatever it follows now.
            return follow[p] == rampSamples ? Applies{.own = false, .master = true} : Applies{};
        }
        return {.own = true, .master = true};
    }

    /// Part p's applied envelope can't be above 0 for the rest of this
    /// process() call: every envelope that applies to it is idle, or held at
    /// a sustain of exactly 0.
    [[nodiscard]] bool envelopeSilent(std::size_t p) const noexcept
    {
        const auto a = applies(p);
        return (!a.own || adsrSilent(envelopes[p])) && (!a.master || adsrSilent(envelopes[link.master]));
    }

    /// Part p's applied envelope hasn't released to 0.
    [[nodiscard]] bool envelopeActive(std::size_t p) const noexcept
    {
        const auto a = applies(p);
        return (a.own && envelopes[p].isActive()) || (a.master && envelopes[link.master].isActive());
    }

    /// An upper bound on the modulation of part p's Mix, in internal units,
    /// over the `length` samples from the block's sample `done`: the seam
    /// M1.5 fills. Each modulation route to Mix X adds one term, its amount
    /// times its source's bound over those samples (an LFO's largest value
    /// there, a mod envelope's 0 when idle or held at 0, otherwise the
    /// source's range); the test below doesn't change. 0 until then: Mix has
    /// no modulation.
    [[nodiscard]] static float mixModulationBound(std::size_t /*part*/, const VoiceKnobs& /*knobs*/,
                                                  std::size_t /*done*/, std::size_t /*length*/) noexcept
    {
        return 0.0f;
    }

    /// Part p's Mix, knob plus modulation, can't be above the law's 0 over
    /// the `length` samples from the block's sample `done`: the ramp is
    /// linear, so its larger end bounds it.
    [[nodiscard]] static bool mixSilent(std::size_t p, const VoiceKnobs& knobs, std::size_t done,
                                        std::size_t length) noexcept
    {
        const auto* ramp = knobs.mix[p];
        const auto at = knobs.offset + done;
        const float knob =
            ramp != nullptr ? std::max(ramp->valueAfter(at), ramp->valueAfter(at + length - 1)) : mixLaw.unity;
        return knob + mixModulationBound(p, knobs, done, length) <= mixLaw.knobMin;
    }

    /// Part p's Solo gain is 0 over those samples (a null one is 1).
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

    /// Part p's output gain (envelope x Mix x Solo) can't be above 0 over
    /// those samples: one factor's upper bound is 0.
    [[nodiscard]] bool gainSilent(std::size_t p, const VoiceKnobs& knobs, std::size_t done,
                                  std::size_t length) const noexcept
    {
        return envelopeSilent(p) || mixSilent(p, knobs, done, length) || soloSilent(p, knobs, done, length);
    }

    /// What the chunk starting at the block's sample `done` runs.
    [[nodiscard]] Plan planChunk(const VoiceKnobs& knobs, std::size_t done) noexcept
    {
        // The gain's test, per segment: at a grid point for the whole coming
        // segment; mid-segment (a call starting there, at an event or where
        // the caller's block ended) only for a part whose gain is 0, over the
        // rest of the segment. Without an event that bound can't be above
        // the whole segment's, so only an event ends a skip there, and none
        // starts.
        const std::size_t rest = gridSamples - gridPosition;
        std::array<bool, partCount> produces{};
        for (std::size_t p = 0; p < partCount; ++p)
        {
            produces[p] = partProduces(p);
            if (gridPosition == 0 || segmentSilent[p])
            {
                segmentSilent[p] = gainSilent(p, knobs, done, rest);
            }
        }
        Plan plan;
        for (std::size_t q = 0; q < partCount; ++q)
        {
            // Part q into the other filter, only while it can add anything.
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
        // The slots: rendered for a filter that takes them, one that runs or
        // a bypassed one that is heard (their own, or the other on AB).
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

    /// The mono path is left at the first chunk where a slot isn't centred:
    /// channel 1 of each filter takes channel 0's state, which it would have
    /// had.
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
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (routes[f].active)
            {
                ++counters.chunks;
                counters.outputsSkipped += plan.output[f] ? 0U : 1U;
            }
        }

        // 1. The oscillators of each heard part, added into its buffer at the
        // pitch plus drift, in runs of constant drift. While the pitch moves
        // (a glide, a bend's ramp) the slots take it every sample; otherwise
        // once, which is the same to the bit at a constant pitch and cheaper.
        // A slot no filter takes, or a silent one, is skipped where that is
        // exact (no gain ramp moving in it); one that can't be is rendered
        // into its part's buffer, which nothing reads this chunk.
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
        std::array<AudioBlock, partCount> parts{};
        for (std::size_t p = 0; p < partCount; ++p)
        {
            parts[p] = block(partBuffers[p], partPointers[p], length);
            if (heard[p] && plan.render[p])
            {
                parts[p].clear();
            }
        }
        // A slot's skip, like a filter's, starts only at a grid point: where it
        // starts decides what its phase's jump leaves in flight at the
        // return, so it mustn't depend on where the caller's block ended.
        // Mid-segment it can only end.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            for (std::size_t s = 0; s < slotsPerPart; ++s)
            {
                const auto index = (p * slotsPerPart) + s;
                const auto& slot = slots[index];
                const bool skippable = (!plan.render[p] || slot.isSilent()) && slot.canSkip();
                slotSkipping[index] = skippable && (gridPosition == 0 || slotSkipping[index]);
            }
        }
        std::array<bool, partCount> cleared{}; // an untaken part's buffer, cleared for a slot that can't skip
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
                                       if (!plan.render[p] && !cleared[p])
                                       {
                                           parts[p].clear();
                                           cleared[p] = true;
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

        // 2. The amp envelopes (always), and each heard filter's part's Mix
        // and Solo gains.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            if (!sharing || p == link.master)
            {
                envelopes[p].process(std::span{envelopeBuffers[p]}.first(length));
            }
        }
        for (std::size_t p = 0; p < partCount; ++p)
        {
            fillApplied(p, length);
            if (routes[p].active && plan.output[p])
            {
                fillMixGains(p, knobs, done, length);
            }
        }

        // 3. Each filter that runs gets its input, in its own buffer; 4. the
        // filter, in place; 5. its output through its part's envelope, Mix and
        // Solo, into `out`. A skipped filter's grid and ramps advance; a
        // filter whose gain is 0 adds nothing.
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
                continue; // bypassed, and its gain is 0
            }
            const auto input = block(filterBuffers[f], filterPointers[f], length, channels);
            buildInput(f, length, channels, plan.crossing[1 - f]);
            filters[f].process(input);
            const auto& e = *applied[f];
            const auto& g = mixGains[f];
            // On the mono path both channels take channel 0, through the
            // stereo path's own expression: a compiler that contracts it into
            // a fused multiply-add (Apple Clang does) then does so on both
            // paths, and they stay the same to the bit.
            const auto y0 = input.channel(0);
            const auto y1 = channels == 2 ? input.channel(1) : y0;
            if (unityLevel)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    const float ge = g[i] * e[i]; // exactly e[i] at unity
                    left[i] += ge * y0[i];
                    right[i] += ge * y1[i];
                }
            }
            else
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    const float ge = g[i] * e[i];
                    left[i] += (ge * y0[i]) * outputLevel;
                    right[i] += (ge * y1[i]) * outputLevel;
                }
            }
        }
    }

    /// The envelope that applies to part p over this chunk: its own, the
    /// master's (shared, or a signal link followed), or a linear crossfade
    /// between them while a signal link's ramp runs (the end values exactly
    /// the envelopes'). The ramp advances whether or not the part is heard.
    void fillApplied(std::size_t p, std::size_t length) noexcept
    {
        const auto m = link.master;
        if (sharing && p != m)
        {
            applied[p] = &envelopeBuffers[m];
            return;
        }
        if (link.kind != LinkKind::signal || p == m)
        {
            applied[p] = &envelopeBuffers[p];
            return;
        }
        if (!following[p] && follow[p] == 0)
        {
            applied[p] = &envelopeBuffers[p];
            return;
        }
        if (following[p] && follow[p] == rampSamples)
        {
            applied[p] = &envelopeBuffers[m];
            return;
        }
        if (link.transition != LinkTransition::ramp)
        {
            // Waiting for the next note (only a ramp moves between them).
            applied[p] = follow[p] == rampSamples ? &envelopeBuffers[m] : &envelopeBuffers[p];
            return;
        }
        const auto& own = envelopeBuffers[p];
        const auto& master = envelopeBuffers[m];
        auto& mixed = crossfades[p];
        const auto n = static_cast<float>(rampSamples);
        for (std::size_t i = 0; i < length; ++i)
        {
            if (following[p])
            {
                follow[p] = std::min(follow[p] + 1, rampSamples);
            }
            else if (follow[p] > 0)
            {
                --follow[p];
            }
            if (follow[p] == rampSamples)
            {
                mixed[i] = master[i];
            }
            else if (follow[p] == 0)
            {
                mixed[i] = own[i];
            }
            else
            {
                const float w = static_cast<float>(follow[p]) / n;
                mixed[i] = own[i] + (w * (master[i] - own[i]));
            }
        }
        applied[p] = &mixed;
    }

    /// Part p's Mix gain at each sample of the chunk that starts at the
    /// block's sample `done`: its knob ramp read at that sample (unity if
    /// null), plus its modulation (0 until M1.5), through the law; then
    /// times its Solo gain there (none if null). A linear ramp is monotonic,
    /// so the same value at both ends of the chunk is a constant over it,
    /// computed once (and a constant Solo of exactly 1 changes nothing).
    void fillMixGains(std::size_t p, const VoiceKnobs& knobs, std::size_t done, std::size_t length) noexcept
    {
        auto& g = mixGains[p];
        const auto at = knobs.offset + done; // the ramps stand that many samples before this block
        fillKnobGains(g, knobs.mix[p], at, length);
        const auto* solo = knobs.solo[p];
        if (solo == nullptr)
        {
            return;
        }
        const float first = solo->valueAfter(at);
        const float last = solo->valueAfter(at + length);
        if (first <= last && first >= last) // equal, without -Wfloat-equal
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
            std::fill_n(g.begin(), length, mixGain({.mix = mixLaw.unity}));
            return;
        }
        const float first = ramp->valueAfter(at);
        const float last = ramp->valueAfter(at + length);
        if (first <= last && first >= last) // equal, without -Wfloat-equal
        {
            std::fill_n(g.begin(), length, mixGain({.mix = first}));
            return;
        }
        for (std::size_t i = 0; i < length; ++i)
        {
            g[i] = mixGain({.mix = ramp->valueAfter(at + i)});
        }
    }

    /// own (x g), or for AB own + the other part through its envelope (x g),
    /// on `channels` channels (1 on the mono path). `crossing` false leaves
    /// the other part out: its envelope or its slots can't add anything.
    void buildInput(std::size_t f, std::size_t length, std::size_t channels, bool crossing) noexcept
    {
        const std::size_t other = 1 - f;
        const auto& envelopeOther = *applied[other];
        for (std::size_t c = 0; c < channels; ++c)
        {
            const auto& own = partBuffers[f][c];
            const auto& crossingPart = partBuffers[other][c];
            auto& dst = filterBuffers[f][c];
            if (routes[f].both && crossing)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    // Its own statement, so the product is rounded before the
                    // sum on every compiler (no fused multiply-add).
                    const float enveloped = envelopeOther[i] * crossingPart[i];
                    dst[i] = own[i] + enveloped;
                }
            }
            else
            {
                std::copy_n(own.begin(), length, dst.begin());
            }
            if (!unityLevel)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    dst[i] *= inputLevel;
                }
            }
        }
    }

    std::array<Slot, slotCount> slots;
    std::array<LadderFilter, partCount> filters{};
    std::array<Adsr, partCount> envelopes{};
    PitchDrift pitchDrift;

    std::array<FilterInput, partCount> pendingInputs{FilterInput::own, FilterInput::own};
    std::array<bool, partCount> pendingBypass{};
    std::array<FilterInput, partCount> inputs{FilterInput::own, FilterInput::own};
    std::array<Route, partCount> routes{};
    std::array<bool, partCount> heard{};

    float inputLevel = 1.0f;
    float outputLevel = 1.0f;
    bool unityLevel = true;

    std::array<StereoChunk, partCount> partBuffers{};
    std::array<StereoChunk, partCount> filterBuffers{};
    std::array<Chunk, partCount> envelopeBuffers{};
    std::array<Chunk, partCount> mixGains{}; ///< each part's Mix gain over the chunk
    std::array<std::array<float*, 2>, partCount> partPointers{};
    std::array<std::array<float*, 2>, partCount> filterPointers{};

    std::array<float, partCount> lastOctaves{};
    std::array<float, partCount> lastK{};
    Glide glide;
    Glide velocityRamp; ///< the note's velocity, 0-1, ramped over a glide
    Chunk pitches{};    ///< the pitch per sample of a chunk while it moves
    Chunk notes{};      ///< ... plus drift, for one of drift's runs
    float masterTuneHz = concertPitchHz;
    float masterTuneOffset = 0.0f; ///< masterTuneSemitones(masterTuneHz): 0 at 440
    bool played = false;
    std::size_t gridPosition = 0;
    bool mono = false;                           ///< on the mono path (channel 0 only)
    std::array<bool, partCount> segmentSilent{}; ///< each part's gain is 0 over this segment
    std::array<bool, slotCount> slotSkipping{};  ///< each slot is being skipped

    // The link.
    LinkSettings link{};
    bool linked = false;
    bool sharing = false;                          ///< shared: only the master's envelope runs
    std::array<bool, partCount> following{};       ///< signal: the part follows the master's envelope
    std::array<std::uint32_t, partCount> follow{}; ///< signal: 0 own ... rampSamples the master's
    std::uint32_t rampSamples = 480;               ///< 10 ms
    std::array<EnvelopeParams, partCount> pendingParams{};
    std::array<bool, partCount> hasPendingParams{};
    std::array<Chunk, partCount> crossfades{};
    std::array<const Chunk*, partCount> applied{envelopeBuffers.data(), envelopeBuffers.data() + 1};
    LinkStats counters;
};

// --- The engine ---------------------------------------------------------------------------

/// The filter knobs every voice reads: Cutoff 5 + the Filter Control's 5
/// (4,677 Hz), Resonance 0, unless set; constant (as task 5's).
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
    [[nodiscard]] VoiceKnobs knobs() const noexcept
    {
        return {
            .cutoff = {&cutoff, &cutoff},
            .resonance = {&resonance, &resonance},
            .controlCutoff = &control,
            .controlResonance = &zero,
        };
    }
    LinearSmoother cutoff;
    LinearSmoother control;
    LinearSmoother resonance;
    LinearSmoother zero;
};

enum class EngineEventKind : std::uint8_t
{
    voice,    ///< a VoiceEvent for the allocator
    link,     ///< the link turned on (`on`) or off
    envelope, ///< part `part`'s four envelope knobs set to `params` (an edit, automation)
};

struct EngineEvent
{
    std::size_t offset = 0;
    EngineEventKind kind = EngineEventKind::voice;
    dsp::VoiceEvent voice{};
    std::size_t part = 0;
    EnvelopeParams params{};
    bool on = false;
};

/// The parameter layer's side of the link (a stand-in for M1.9): what the
/// voices are told, and what happens to the knobs.
struct LinkPolicy
{
    LinkSettings voice{};           ///< for LinkVoice (ignored by dsp::Voice)
    bool mirror = false;            ///< while linked, the other part's envelope takes the master's knobs (a, b)
    bool restoreOnUnlink = true;    ///< at unlink the other part's own knobs return (else it keeps the master's)
    bool pendingAtNextNote = false; ///< the link's change reaches each voice at its next note-on (LinkVoice)
};

template <typename V>
concept LinkCapable = requires(V& v) {
    v.setLinked(true);
    v.setLinkSettings(LinkSettings{});
};

/// The candidates around the real VoiceAllocator: 16 voices (dsp::Voice or
/// LinkVoice), the envelope knobs of both parts and the link. Each host
/// block is split at its non-note events. The output is written, not added
/// to.
template <typename V>
class LinkEngine
{
public:
    using Voice = V;
    using Allocator = dsp::VoiceAllocator<V, 16>;
    static constexpr bool linkVoice = LinkCapable<V>;

    LinkEngine(const MinBlepTable& table, const LinkPolicy& p, std::uint64_t seed = 1)
        : policy(p), allocator(std::make_unique<Allocator>(seed, table))
    {
        if constexpr (linkVoice)
        {
            allocator->forEachVoice([&](V& v) { v.setLinkSettings(policy.voice); });
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
    [[nodiscard]] const LinkPolicy& linkPolicy() const noexcept
    {
        return policy;
    }
    /// A part's envelope knobs (before prepare(), or between blocks).
    void setEnvelope(std::size_t part, const EnvelopeParams& params) noexcept
    {
        edit(part, params);
    }
    /// The link (before prepare(), or between blocks).
    void setLinked(bool on) noexcept
    {
        linkTo(on);
    }
    [[nodiscard]] bool isLinked() const noexcept
    {
        return linked;
    }
    /// What every voice's skip test did (LinkVoice).
    [[nodiscard]] LinkStats stats() const noexcept
    {
        LinkStats s;
        if constexpr (linkVoice)
        {
            for (std::size_t v = 0; v < 16; ++v)
            {
                s.add(allocator->voice(v).stats());
            }
        }
        return s;
    }

    void prepare(const ProcessSpec& spec)
    {
        allocator->prepare({.sampleRate = spec.sampleRate, .maxBlockSize = spec.maxBlockSize, .numChannels = 2});
        notes.assign(256, {});
        clock = 0;
    }

    void process(const AudioBlock& out, std::span<const EngineEvent> events) noexcept
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
    [[nodiscard]] VoiceKnobs knobs() const noexcept
    {
        auto k = filters.knobs();
        k.time = clock;
        return k;
    }

    [[nodiscard]] EnvelopeParams effective(std::size_t p) const noexcept
    {
        const auto m = policy.voice.master;
        return policy.mirror && linked && p != m ? values[m] : values[p];
    }

    void push(std::size_t p, bool pending) noexcept
    {
        const auto params = effective(p);
        allocator->forEachVoice(
            [&](V& v)
            {
                if constexpr (linkVoice)
                {
                    if (pending)
                    {
                        v.setPendingEnvelope(p, params);
                        return;
                    }
                }
                applyParams(v.envelope(p), params);
            });
    }

    void edit(std::size_t part, const EnvelopeParams& params) noexcept
    {
        values[part] = params;
        push(part, false);
        const auto m = policy.voice.master;
        if (policy.mirror && linked && part == m)
        {
            push(1 - m, false);
        }
    }

    void linkTo(bool on) noexcept
    {
        const auto m = policy.voice.master;
        const auto other = 1 - m;
        if (!on && linked && policy.mirror && !policy.restoreOnUnlink)
        {
            values[other] = values[m];
        }
        linked = on;
        if constexpr (linkVoice)
        {
            // Before the knobs: a shared envelope hands its state over first.
            allocator->forEachVoice([&](V& v) { v.setLinked(on); });
        }
        if (policy.mirror)
        {
            push(other, policy.pendingAtNextNote);
        }
    }

    void apply(const EngineEvent& event) noexcept
    {
        switch (event.kind)
        {
        case EngineEventKind::voice:
            break;
        case EngineEventKind::link:
            linkTo(event.on);
            break;
        case EngineEventKind::envelope:
            edit(std::min<std::size_t>(event.part, 1), event.params);
            break;
        }
    }

    void render(const AudioBlock& piece, std::span<const dsp::VoiceEvent> pieceNotes) noexcept
    {
        allocator->process(piece, pieceNotes, knobs());
        clock += piece.numSamples();
    }

    LinkPolicy policy;
    std::array<EnvelopeParams, 2> values{};
    bool linked = false;
    FilterKnobs filters;
    std::unique_ptr<Allocator> allocator;
    std::vector<dsp::VoiceEvent> notes;
    std::uint64_t clock = 0;
};

} // namespace anoesis::test::amp_link
