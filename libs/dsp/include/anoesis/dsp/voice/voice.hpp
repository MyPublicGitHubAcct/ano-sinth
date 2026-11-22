// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/pitch_drift.hpp>
#include <anoesis/dsp/voice/glide.hpp>
#include <anoesis/dsp/voice/mix_law.hpp>
#include <anoesis/dsp/voice/pitch_bend.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

/// The voice: Sylenth1's per-note audio path up to the mixer (inventory §1,
/// "Voice structure and routing"). Two parts, A and B, each with two
/// oscillators (OscillatorSlot: unison, stereo), a filter (LadderFilter) and
/// an amp envelope (Adsr); one PitchDrift shared by the four slots. Chosen by
/// the project owner from measured candidates (docs/dsp/voice-architecture.md):
///
/// - Each filter's Input Select is its own part, both parts (AB), or "--".
///   AB is a plain sum, as the oscillators within a part are.
/// - A part crossing into the other part's filter goes through its own amp
///   envelope before that filter, and through the filter's part's envelope
///   after it (the developer: "osc B route through amp env B, through filter
///   A through amp env A"). A filter's own part goes through the filter, then
///   its envelope. So, for filter A on AB:
///       out += env A . F_A(g (p_A + env B . p_B)) / g
///   where g is the filter input level (1 by default; inventory V8).
/// - "--" passes nothing and the filter isn't run. A part that no filter
///   reads is silent and its oscillators aren't rendered. A bypassed filter
///   still takes its Input Select, and passes that sum unfiltered.
/// - Each filter is fed through the Filter Control (filter_control.hpp) at
///   the voice's own 32-sample grid, counted from its note-on: at each grid
///   point, the global knob ramps read for the end of the coming segment,
///   plus keytrack from the voice's note, into setCutoffModulation() (with
///   setCutoff() held at the law's anchor) and setResonanceModulation()
///   (with setResonance(0)). A non-finite result keeps the last finite one.
///   The voice's modulation of Cutoff X / AB and Reso X / AB joins the sum
///   in M1.5; until then those terms are 0.
/// - Input Select and Bypass changes apply at the voice's next grid point,
///   so a filter is always on the voice's grid; a filter switched on from
///   "--", or out of Bypass, starts from rest there.
/// - For an allocator (M1.4 task 2, docs/dsp/voice-allocation.md): a
///   note-on on a sounding voice (a steal, a retrigger in place) keeps the
///   filters and the grid; isActive() counts only what can be heard; stop()
///   silences a finished voice so its next note starts from silence.
/// - The pitch (M1.4 task 3, docs/dsp/legato-portamento.md): a Glide
///   (linear in semitones, constant time, from NoteStart) plus the global
///   PitchBend read through the knobs at absolute samples. While it moves in
///   a chunk the slots get a note per sample; otherwise one, which is the
///   same to the bit at a constant pitch. Keytrack follows the glide, the
///   bend and the master tune (setMasterTune()), fed for the segment's end.
///   legatoNoteOn() moves the pitch and leaves the envelopes to
///   Adsr::legatoGateOn(), the phases, drift, filters and grid untouched.
///   The velocity ramps over a glide (the velocity source, M1.5).
///
/// - Mix A and Mix B (M1.4 task 4, docs/dsp/mixer.md): each filter's output,
///   after its part's amp envelope, is multiplied by its part's Mix gain
///   (mix_law.hpp: linear in internal units, unity at 10, the knob plus its
///   modulation clamped to [0, 15]), per sample, with Mix's global ramp read
///   through the knobs at each sample; constant over a chunk while the ramp
///   is still. A part crossing into the other filter takes that filter's
///   part's Mix only (V3). A null Mix ramp is unity. The modulation term is 0
///   until M1.5.
///
/// Rendering runs in chunks that stop at the grid (at most 32 samples): the
/// four slots added into their part's buffer (in drift's runs of constant
/// offset), both envelopes, each part's Mix gains, each filter's input built
/// in its own buffer, the filters in place, and each filter's output times
/// its part's envelope and Mix added into channels 0 and 1 of the caller's
/// block. The scratch is fixed (14 buffers of 32 samples), so nothing is
/// allocated, prepare() included; the output does not depend on how the
/// caller splits its blocks.
///
/// Velocity's routes are the modulation system's (M1.5). About 25 KB at 64
/// taps.
/// Docs: docs/dsp/voice-architecture.md, docs/dsp/legato-portamento.md,
/// docs/dsp/mixer.md
namespace anoesis::dsp
{

/// A filter's Input Select (inventory V1): its own part, both parts (AB), or
/// nothing ("--").
enum class FilterInput : std::uint8_t
{
    none, ///< "--": the filter has no input and isn't run
    own,  ///< A for filter A, B for filter B
    both, ///< AB: both parts, summed
};

template <std::size_t Taps = minBlepTaps>
class Voice
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
    explicit Voice(const MinBlepTable& residuals, std::uint64_t seed = 0) noexcept
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
    [[nodiscard]] PitchDrift& drift() noexcept
    {
        return pitchDrift;
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
    ///   starts here.
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
            if (routes[f].active && fed && envelopes[f].isActive())
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
            if (heard[p] && envelopes[p].isActive())
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

    [[nodiscard]] AudioBlock block(StereoChunk& chunk, std::array<float*, 2>& pointers, std::size_t length) noexcept
    {
        pointers = {chunk[0].data(), chunk[1].data()};
        return {pointers.data(), 2, length};
    }

    void renderChunk(const AudioBlock& out, const VoiceKnobs& knobs, std::size_t done) noexcept
    {
        const auto length = out.numSamples();

        // 1. The oscillators of each heard part, added into its buffer at the
        // pitch plus drift, in runs of constant drift. While the pitch moves
        // (a glide, a bend's ramp) the slots take it every sample; otherwise
        // once, which is the same to the bit at a constant pitch and cheaper.
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
            if (heard[p])
            {
                parts[p].clear();
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

        // 2. The amp envelopes, and each sounding filter's part's Mix gains.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            envelopes[p].process(std::span{envelopeBuffers[p]}.first(length));
            if (routes[p].active)
            {
                fillMixGains(p, knobs, done, length);
            }
        }

        // 3. Each running filter's input, in its own buffer; 4. the filter, in
        // place; 5. its output through its part's envelope and Mix, into `out`.
        const auto left = out.channel(0);
        const auto right = out.channel(1);
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (!routes[f].active)
            {
                continue;
            }
            const auto input = block(filterBuffers[f], filterPointers[f], length);
            buildInput(f, length);
            filters[f].process(input);
            const auto& e = envelopeBuffers[f];
            const auto& g = mixGains[f];
            const auto y0 = input.channel(0);
            const auto y1 = input.channel(1);
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

    /// Part p's Mix gain at each sample of the chunk that starts at the
    /// block's sample `done`: its knob ramp read at that sample (unity if
    /// null), plus its modulation (0 until M1.5), through the law. A linear
    /// ramp is monotonic, so the same value at both ends of the chunk is a
    /// constant gain over it, computed once.
    void fillMixGains(std::size_t p, const VoiceKnobs& knobs, std::size_t done, std::size_t length) noexcept
    {
        auto& g = mixGains[p];
        const auto* ramp = knobs.mix[p];
        if (ramp == nullptr)
        {
            std::fill_n(g.begin(), length, mixGain({.mix = mixLaw.unity}));
            return;
        }
        const auto at = knobs.offset + done; // the ramps stand that many samples before this block
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

    /// own (x g), or for AB own + the other part through its envelope (x g).
    void buildInput(std::size_t f, std::size_t length) noexcept
    {
        const std::size_t other = 1 - f;
        const auto& envelopeOther = envelopeBuffers[other];
        for (std::size_t c = 0; c < 2; ++c)
        {
            const auto& own = partBuffers[f][c];
            const auto& crossing = partBuffers[other][c];
            auto& dst = filterBuffers[f][c];
            if (routes[f].both)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    // Its own statement, so the product is rounded before the
                    // sum on every compiler (no fused multiply-add).
                    const float enveloped = envelopeOther[i] * crossing[i];
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
};

} // namespace anoesis::dsp
