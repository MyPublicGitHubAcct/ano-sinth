// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/modulation/route_compiler.hpp>
#include <anoesis/dsp/modulation/routing.hpp>
#include <anoesis/dsp/modulation/source_bound.hpp>
#include <anoesis/dsp/modulation/source_context.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/pitch_drift.hpp>
#include <anoesis/dsp/oscillators/slot_modulation.hpp>
#include <anoesis/dsp/voice/glide.hpp>
#include <anoesis/dsp/voice/mix_law.hpp>
#include <anoesis/dsp/voice/pitch_bend.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <algorithm>
#include <array>
#include <bit>
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
///   The modulation of Cutoff X / AB and Reso X / AB (M1.5) joins the sum
///   there, from the sources as they stand at the grid point.
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
///   part's Mix only (V3). A null Mix ramp is unity. Mix's modulation (M1.5)
///   is added per sample, before the clamp.
///
/// - Solo, and skipping what can't be heard (M1.4 task 5, docs/dsp/
///   part-solo-and-skipping.md):
///   - Solo's gains (VoiceKnobs::solo, from PartSolo) are multiplied into
///     each part's Mix gain per sample, after its filter: the other part's
///     filter output is silenced, and a part crossing into the selected
///     part's filter still sounds through it. A null one is 1.
///   - At each grid point, for the whole coming segment, each part's output
///     gain is tested with an upper bound per factor: its amp envelope (idle,
///     or held at a sustain of exactly 0), Mix's ramp at its larger end plus
///     the bound on Mix's modulation (mixModulationBound(): each route's
///     amount times its source's bound, M1.5),
///     and Solo's ramp at its larger end. A process() call mid-segment
///     re-tests only a part whose gain is 0, over the rest of the segment,
///     which can only end its skip; so a skip starts only at the voice's
///     grid points, and the output doesn't depend on the caller's blocks.
///   - Exact skips: a filter at rest whose input can't be non-zero isn't
///     solved (LadderFilter::skip(): its grid and ramps advance, and it is
///     still fed); an output stage whose gain is 0 isn't run; a crossing
///     part whose envelope can't be above 0 isn't added into the other
///     filter. Each is the same to the bit as running it.
///   - A part whose gain is 0 while its filter is fed has the filter
///     skipped too, its state frozen for its return (where it then differs
///     from a filter that ran, by -64 to -70 dB over 50 ms on a plain saw,
///     -39 to -44 dB resonant).
///   - A slot is rendered only for a filter that takes it (one that runs, or
///     a bypassed one that is heard); otherwise, or when it is silent, it is
///     skipped (OscillatorSlot::skip(), its phase advanced) when no gain
///     ramp in it moves. Its skip, too, starts only at a grid point.
///   - The mono path: at a note-on from silence, if every sounding slot is
///     centred (OscillatorSlot::isCentred(); Noise never is), the filters'
///     inputs, the filters and the output stage run on channel 0 alone, the
///     output written to both channels with the stereo path's own
///     expression (so a compiler that fuses it into a multiply-add does so
///     on both paths). At the first chunk where a slot isn't centred, each
///     filter's channel 1 is set from channel 0 (LadderFilter::
///     copyChannel()) and the voice is stereo until its next note-on from
///     silence. The same to the bit as the stereo path.
///   isActive() and the allocator's view are unchanged.
///
/// - The modulation sources (M1.5 task 3, docs/dsp/modulation-sources.md),
///   with what the voice reads from the engine in VoiceKnobs::sources
///   (modulation/source_context.hpp; null is the init patch):
///   - One modulation::RouteCompiler: reset and compiled at a note-on from
///     silence, updated at each of the voice's own grid points, so a patch
///     change reaches the voice at its next grid point wherever the host's
///     blocks fall. A note-on on a sounding voice keeps its routes and grid.
///   - Two mod envelopes (Adsr), the same as the amp envelopes in every way:
///     gated at each note-on, legatoGateOn() at a legato note, released at a
///     note-off, ending at exactly 0, no velocity. They don't keep the voice
///     alive. They take the context's settings at each note-on; a change
///     reaches a sounding voice at its next grid point.
///   - Two LFOs (Lfo) in segment order: at each grid point each one's rate
///     (held) and gain (ramped from the previous grid point's) are set from
///     the sources as they stand there, and its wave is rendered for the
///     segment ahead; its value is 0.5 x gain x (wave + offset). Restarted at
///     a note-on from silence, retriggered on a sounding voice (the rest of
///     the segment rendered again from a copy taken at the grid point), left
///     alone by a legato note. A free one is copied from the context's
///     global instance at a note-on from silence, moved on to the voice's own
///     sample, and kept through a steal.
///   - A mod envelope or an LFO runs only while a route reads it; its gates
///     are tracked either way. A route that appears mid-note starts it at the
///     voice's next grid point from its note-on state (an envelope gated on,
///     and off if the key is already up; an LFO restarted; a free one copied).
///   - Velocity (ramped over a glide) and keytrack (the glide, the bend and
///     the master tune) per sample; both amp envelopes, always run, so the
///     skip plan is unchanged; the mod wheel, channel pressure and Step
///     Velocity read from the context at absolute samples.
///   - Folded delivery: in each chunk, each source that a route to Pitch or
///     Mix reads goes into a chunk buffer or, if it is constant over the
///     chunk, is added as one term; each route adds amount x value into its
///     part's per-sample sum, the product in a statement of its own (a fused
///     multiply-add would move the result by an ulp). A part whose Pitch is
///     routed feeds its slots a note per sample; Mix's sum goes into
///     mixGain(). Cutoff, Reso, the Filter Control's terms and the LFOs'
///     Rate and Gain take the sums of the sources as they stand at each grid
///     point.
///   - Each source's bound (source_bound.hpp, by its kind; keytrack's from
///     its parts, the glide's ends plus the bend's) feeds
///     mixModulationBound().
///   With no routes nothing runs and nothing is summed, so the init patch is
///   the voice as it was, to the bit.
///
/// - The per-voice destinations (M1.5 task 4, docs/dsp/
///   modulation-destinations.md):
///   - Volume and Phase join Pitch and Mix in the folded per-sample sums; Pan
///     is ramped on the grid, from the previous grid point's sum to this
///     one's. Each slot of a part takes them through its SlotModulation
///     (OscillatorSlot: its own knob ramps, Volume and Pan clamped to
///     [0, 10], Phase wrapping), and a part with none of them takes the
///     slots' plain calls, so the init patch costs what it did.
///   - Any change of the routes at a grid point (an amount, a destination, a
///     source) crossfades every sum from the old routes' to the new ones'
///     over routeCrossfadeSegments() segments (10 ms), per sample for Pitch,
///     Mix, Volume and Phase and at each grid point for the rest; the old
///     routes' sources run until it ends, a further change waits for its
///     end, and each bound is the larger of the two. A note-on from silence
///     compiles at once.
///   - A note from silence starts each slot at Phase plus its part's Phase
///     sum there (the slots' note-on comes after the sources'); a slot with
///     no voice sounding still follows its Phase sum (trackPhase()).
///   - The skip test takes each part's Volume bound (modulationBound()); a
///     Pan route takes its part off the mono path.
///
/// Rendering runs in chunks that stop at the grid (at most 32 samples): the
/// plan for the chunk (what runs, what is skipped); both amp envelopes, the
/// other sources and the per-sample sums; the four slots added into their
/// part's buffer (in drift's runs of constant offset); each heard part's Mix
/// and Solo gains, each filter's input built in its own buffer, the filters
/// in place, and each filter's output times its part's envelope and gains
/// added into channels 0 and 1 of the caller's block. The scratch is fixed
/// (14 buffers of 32 samples for the audio, 22 for the sources), so nothing
/// is allocated, prepare() included; the output does not depend on how the
/// caller splits its blocks.
///
/// About 30 KB at 64 taps (25 KB before the sources).
/// Docs: docs/dsp/voice-architecture.md, docs/dsp/legato-portamento.md,
/// docs/dsp/mixer.md, docs/dsp/part-solo-and-skipping.md,
/// docs/dsp/modulation-sources.md
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
    /// (the slots' free-running phases and noise, the drift, the LFOs' random
    /// rows): give each voice its own.
    explicit Voice(const MinBlepTable& residuals, std::uint64_t seed = 0) noexcept
        : slots{Slot{residuals}, Slot{residuals}, Slot{residuals}, Slot{residuals}}
    {
        for (std::size_t s = 0; s < slotCount; ++s)
        {
            slots[s].setRandomSeed(seed ^ (0x9E3779B97F4A7C15U * (s + 1)));
        }
        pitchDrift.setSeed(seed ^ 0x3C6EF372FE94F82BU);
        for (std::size_t k = 0; k < lfoCount; ++k)
        {
            lfos[k].setSeed(seed ^ (0xA54FF53A5F1D36F1U * (k + 1)));
        }
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

    /// Whether the voice is on its mono path (channel 0 only, copied to
    /// both), for measurement: taken at a note-on from silence when every
    /// sounding slot is centred, left for good within the note at the first
    /// chunk where one isn't.
    [[nodiscard]] bool isMono() const noexcept
    {
        return mono;
    }

    /// For measurement and tests: told about every chunk this voice renders
    /// (its sources' values, and their bounds where it takes them); null for
    /// none. It must outlive its use.
    void setSourceObserver(modulation::SourceObserver* sourceObserver) noexcept
    {
        observer = sourceObserver;
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
        for (auto& e : modEnvelopes)
        {
            e.prepare(spec);
        }
        for (auto& l : lfos)
        {
            l.prepare(spec);
        }
        pitchDrift.prepare(spec);
        fadeSegments = modulation::routeCrossfadeSegments(spec.sampleRate);
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
        glide.jump(referenceNote);
        velocityRamp.jump(1.0f);
        played = false;
        mono = false;
        segmentSilent = {};
        slotSkipping = {};
        compiler.reset();
        live = {};
        gridSums = {};
        lfoGainBefore = {};
        lfoGainNow = {};
        lfoOffsetNow = {};
        lfoLast = {};
        segmentBegun = false;
        keyHeld = false;
        fading = false;
        fadeIndex = 0;
        panBefore = {};
        panNow = {};
    }

    /// A note-on before the next process() call, with the knobs positioned at
    /// its sample. The pitch jumps to `note`, or glides to it from
    /// `start.from` over `start.glideSeconds` (linear in semitones); the
    /// velocity is `start.velocity`, or, while it glides, ramps there from
    /// `start.fromVelocity` over the same time. The slots, the drift and the
    /// envelopes start (or, on a sounding voice, retrigger: the slots as
    /// Retrig says, the envelopes attacking from their level), and so do the
    /// modulation sources (restarted from silence; on a sounding voice the
    /// LFOs retrigger and a free one keeps its copy).
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
        pitchDrift.noteOn();
        for (auto& e : envelopes)
        {
            e.gateOn();
        }
        keyHeld = true;
        sourcesNoteOn(knobs, sounding);
        // The slots after the sources (nothing either computes depends on
        // the other), so that a note from silence starts at Phase plus the
        // Phase sum as it stands at the note-on.
        if (!sounding)
        {
            phaseOffsetsAtNoteOn(knobs);
        }
        for (auto& s : slots)
        {
            s.noteOn();
        }
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
        for (auto& e : modEnvelopes)
        {
            e.legatoGateOn();
        }
        keyHeld = true;
    }

    /// Releases the amp and mod envelopes (a dormant mod envelope's gate is
    /// kept for a route that appears later).
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
        for (auto& e : modEnvelopes)
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

    // --- The modulation sources (M1.5 task 3, docs/dsp/modulation-sources.md) ---------------

    using Source = modulation::Source;
    using Target = modulation::Target;
    using SourceContext = modulation::SourceContext;
    static constexpr std::size_t sourceCount = modulation::sourceCount;
    static constexpr std::size_t lfoCount = modulation::lfoCount;
    static constexpr std::size_t modEnvelopeCount = modulation::modEnvelopeCount;

    [[nodiscard]] static constexpr std::size_t index(Source s) noexcept
    {
        return static_cast<std::size_t>(s);
    }
    [[nodiscard]] static constexpr Source modEnvSource(std::size_t k) noexcept
    {
        return k == 0 ? Source::modEnv1 : Source::modEnv2;
    }
    [[nodiscard]] static constexpr Source lfoSource(std::size_t k) noexcept
    {
        return k == 0 ? Source::lfo1 : Source::lfo2;
    }

    /// The context the knobs carry; none is the init patch.
    [[nodiscard]] static const SourceContext& contextOf(const VoiceKnobs& knobs) noexcept
    {
        static constexpr SourceContext none{};
        return knobs.sources != nullptr ? *knobs.sources : none;
    }
    [[nodiscard]] static const modulation::Patch& patchOf(const SourceContext& context) noexcept
    {
        static constexpr modulation::Patch empty{};
        return context.patch != nullptr ? *context.patch : empty;
    }

    [[nodiscard]] const modulation::CompiledRoutes& compiled() const noexcept
    {
        return compiler.routes();
    }
    [[nodiscard]] bool routesTo(Target t, std::size_t part) const noexcept
    {
        return compiled().routesTo(t, part) || (fading && fadeFrom.routesTo(t, part));
    }
    /// A mod envelope or an LFO runs only while a route reads it (during a
    /// crossfade, the old routes' too).
    [[nodiscard]] bool wanted(Source s) const noexcept
    {
        return compiled().uses(s) || (fading && fadeFrom.uses(s));
    }
    /// No route, old or new: nothing runs and nothing is summed.
    [[nodiscard]] bool noRoutes() const noexcept
    {
        return compiled().empty() && !fading;
    }

    /// At a grid point (M1.5 task 4): a change of the patch starts a
    /// crossfade from the routes in force over fadeSegments segments; while
    /// one runs, the patch waits. At a note-on from silence it compiles at
    /// once.
    void updateRoutesAt(const SourceContext& context, bool atNoteOn) noexcept
    {
        if (fading)
        {
            ++fadeIndex;
            if (fadeIndex < fadeSegments)
            {
                return;
            }
            fading = false;
        }
        const auto before = compiled();
        const bool changed = compiler.update(patchOf(context));
        if (changed && !atNoteOn)
        {
            fadeFrom = before;
            fading = true;
            fadeIndex = 0;
        }
    }

    /// The crossfade's weight on the new routes at the segment's sample
    /// `position` (1 when none runs): it reaches 1 on its last sample.
    [[nodiscard]] float fadeWeight(std::size_t position) const noexcept
    {
        if (!fading)
        {
            return 1.0f;
        }
        const auto n = static_cast<float>(fadeSegments * gridSamples);
        return static_cast<float>((fadeIndex * gridSamples) + position + 1) / n;
    }

    /// Each sum from the old routes' value to the new ones' by weight w,
    /// each product in a statement of its own.
    static void blend(modulation::Sums& sums, const modulation::Sums& old, float w) noexcept
    {
        for (std::size_t t = 0; t < modulation::targetCount; ++t)
        {
            for (std::size_t p = 0; p < partCount; ++p)
            {
                const float d = sums.values[t][p] - old.values[t][p];
                const float dw = d * w;
                sums.values[t][p] = old.values[t][p] + dw;
            }
        }
    }

    /// At a note-on from silence: each part's Phase sum as the sources stand
    /// there, for its slots' onsets.
    void phaseOffsetsAtNoteOn(const VoiceKnobs& knobs) noexcept
    {
        modulation::Sums sums{};
        if (!compiled().empty())
        {
            modulation::SourceValues now{};
            valuesNow(knobs, contextOf(knobs), 0, now);
            modulation::accumulate(compiled(), now, sums);
        }
        for (std::size_t s = 0; s < slotCount; ++s)
        {
            slots[s].setPhaseOffset(sums.at(Target::phase, s / slotsPerPart));
        }
    }

    void applyEnvelopeSettings(std::size_t k, const SourceContext& context) noexcept
    {
        const auto& e = context.modEnvelopes[k];
        modEnvelopes[k].setAttack(e.attack);
        modEnvelopes[k].setDecay(e.decay);
        modEnvelopes[k].setSustain(e.sustain);
        modEnvelopes[k].setRelease(e.release);
        appliedEnvelopes[k] = e;
    }

    [[nodiscard]] static bool sameSettings(const modulation::ModEnvelopeSettings& a,
                                           const modulation::ModEnvelopeSettings& b) noexcept
    {
        const auto same = [](float x, float y)
        {
            return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
        };
        return same(a.attack, b.attack) && same(a.decay, b.decay) && same(a.sustain, b.sustain) &&
               same(a.release, b.release);
    }

    /// At every note-on: the routes (from silence), the mod envelopes' gates
    /// and each LFO's start. After every grid point and every note-on from
    /// silence a source is live exactly while a route reads it, and a note-on
    /// on a sounding voice doesn't recompile, so there it only retriggers.
    void sourcesNoteOn(const VoiceKnobs& knobs, bool sounding) noexcept
    {
        const auto& context = contextOf(knobs);
        if (!sounding)
        {
            compiler.reset();
            compiler.update(patchOf(context));
            fading = false;
            fadeIndex = 0;
        }
        for (std::size_t k = 0; k < modEnvelopeCount; ++k)
        {
            // Gated as the amp envelopes; a dormant one keeps its gate for a
            // route that appears later; one that starts from silence starts
            // from 0.
            const auto e = index(modEnvSource(k));
            applyEnvelopeSettings(k, context);
            if (!sounding)
            {
                const bool want = wanted(modEnvSource(k));
                if (want && !live[e])
                {
                    modEnvelopes[k].reset();
                }
                live[e] = want;
            }
            modEnvelopes[k].gateOn();
        }
        for (std::size_t k = 0; k < lfoCount; ++k)
        {
            // Restarted from silence, retriggered on a sounding voice; a free
            // one copied from the global instance from silence and kept
            // through a steal.
            const auto l = index(lfoSource(k));
            const bool free = context.lfos[k].free;
            if (!sounding)
            {
                live[l] = wanted(lfoSource(k));
                lfoLast[k] = 0.0f;
                if (!free)
                {
                    restartLfo(k, context);
                }
                else if (live[l])
                {
                    copyFree(k, context, knobs.offset);
                }
            }
            else if (live[l] && !free)
            {
                retriggerLfo(k);
            }
        }
    }

    /// A free LFO copies the global instance. The engine moves the global on
    /// with each piece it renders, so it stands where the knob ramps stand,
    /// at the start of the call: the copy is moved on to the voice's own
    /// sample, `ahead` samples later (a copy taken at the call's start was 7
    /// samples stale in stage 1).
    void copyFree(std::size_t k, const SourceContext& context, std::size_t ahead) noexcept
    {
        if (context.freeLfos == nullptr)
        {
            return;
        }
        lfos[k] = (*context.freeLfos)[k];
        Chunk scratch{};
        while (ahead > 0)
        {
            const auto m = std::min(ahead, gridSamples);
            lfos[k].process(std::span{scratch}.first(m));
            ahead -= m;
        }
    }

    /// A note-on on a sounding voice: the cycle restarts, slewed. Mid-segment
    /// the rest of the segment is rendered again from the copy taken at the
    /// grid point, moved on to here.
    void retriggerLfo(std::size_t k) noexcept
    {
        if (!segmentBegun)
        {
            lfos[k].retrigger();
            return;
        }
        lfos[k] = lfoAtSegmentStart[k];
        Chunk scratch{};
        lfos[k].process(std::span{scratch}.first(gridPosition));
        lfos[k].retrigger();
        lfos[k].process(std::span{lfoWave[k]}.subspan(gridPosition));
    }

    /// A source that a route starts reading mid-note, at the voice's grid
    /// point (block sample `done`), from its note-on state.
    void lateStartEnvelope(std::size_t k, const SourceContext& context) noexcept
    {
        live[index(modEnvSource(k))] = true;
        modEnvelopes[k].reset();
        applyEnvelopeSettings(k, context);
        modEnvelopes[k].gateOn();
        if (!keyHeld)
        {
            modEnvelopes[k].gateOff();
        }
    }
    void lateStartLfo(std::size_t k, const SourceContext& context, std::size_t ahead) noexcept
    {
        live[index(lfoSource(k))] = true;
        lfoLast[k] = 0.0f; // it hasn't produced anything yet
        if (context.lfos[k].free)
        {
            copyFree(k, context, ahead); // exact: the global instance is where a running copy would be
        }
        else
        {
            restartLfo(k, context);
        }
        lfoGainNow[k] = -1.0f; // a start: the gain doesn't ramp from a stale value
    }

    /// The cycle's start, of the waveform it is set to now. The waveform is
    /// set first: restarted with the one it last had (a new voice's is the
    /// sine), it would start at that one's start and slew into this one,
    /// out of phase for the whole note (the stage-1 prototype's flaw).
    void restartLfo(std::size_t k, const SourceContext& context) noexcept
    {
        lfos[k].setWaveform(context.lfos[k].waveform);
        lfos[k].restart();
    }

    [[nodiscard]] static float lfoOffsetOf(const modulation::LfoSettings& l) noexcept
    {
        return std::clamp(l.offset, -10.0f, 10.0f) / 10.0f;
    }

    /// LFO k as a source at the segment's sample `position`: the gain ramped
    /// from the previous grid point's value to this one's (t < 1, so it stays
    /// between them).
    [[nodiscard]] float lfoValueAt(std::size_t k, std::size_t position) const noexcept
    {
        const float t = static_cast<float>(position) / static_cast<float>(gridSamples);
        const float g = lfoGainBefore[k] + ((lfoGainNow[k] - lfoGainBefore[k]) * t);
        return modulation::lfoSourceValue(lfoWave[k][position], g, lfoOffsetNow[k]);
    }

    /// Each source as it stands at the block's sample `done` (the envelopes
    /// and LFOs: their last output).
    void valuesNow(const VoiceKnobs& knobs, const SourceContext& context, std::size_t done,
                   modulation::SourceValues& v) const noexcept
    {
        v = {};
        v[index(Source::velocity)] = velocityRamp.at(0);
        v[index(Source::keytrack)] = keytrackSource(keytrackPitch(knobs, done, 0));
        v[index(Source::ampEnvA)] = envelopes[0].level();
        v[index(Source::ampEnvB)] = envelopes[1].level();
        for (std::size_t k = 0; k < lfoCount; ++k)
        {
            const auto e = index(modEnvSource(k));
            const auto l = index(lfoSource(k));
            v[e] = live[e] ? modEnvelopes[k].level() : 0.0f;
            v[l] = live[l] ? lfoLast[k] : 0.0f;
        }
        const auto t = knobs.time + done;
        v[index(Source::modWheel)] = context.modWheel != nullptr ? context.modWheel->valueAt(t) : 0.0f;
        v[index(Source::aftertouch)] = context.aftertouch != nullptr ? context.aftertouch->valueAt(t) : 0.0f;
        v[index(Source::stepVelocity)] = context.stepVelocity;
    }

    /// At a grid point (or a note-on from silence, `atNoteOn`): recompile if
    /// the patch changed; start a source a new route reads; the grid sums
    /// from the sources as they stand; each live LFO's rate (held) and gain
    /// (ramped), and its wave for the segment.
    void beginSegment(const VoiceKnobs& knobs, std::size_t done, bool atNoteOn) noexcept
    {
        const auto& context = contextOf(knobs);
        updateRoutesAt(context, atNoteOn);
        for (std::size_t k = 0; k < modEnvelopeCount; ++k)
        {
            if (!sameSettings(context.modEnvelopes[k], appliedEnvelopes[k]))
            {
                applyEnvelopeSettings(k, context);
            }
            const auto e = modEnvSource(k);
            if (!wanted(e))
            {
                live[index(e)] = false;
            }
            else if (!live[index(e)])
            {
                lateStartEnvelope(k, context);
            }
        }
        for (std::size_t k = 0; k < lfoCount; ++k)
        {
            const auto l = lfoSource(k);
            if (!wanted(l))
            {
                live[index(l)] = false;
            }
            else if (!live[index(l)])
            {
                lateStartLfo(k, context, knobs.offset + done);
            }
        }
        segmentBegun = true;
        updatePerSample();
        if (noRoutes())
        {
            gridSums = {}; // nothing reads a source: the exact 0s
            updatePan(atNoteOn);
            return;
        }
        modulation::SourceValues now{};
        valuesNow(knobs, context, done, now);
        modulation::Sums sums{};
        modulation::accumulate(compiled(), now, sums);
        if (fading)
        {
            modulation::Sums old{};
            modulation::accumulate(fadeFrom, now, old);
            blend(sums, old, static_cast<float>(fadeIndex + 1) / static_cast<float>(fadeSegments));
        }
        for (std::size_t k = 0; k < lfoCount; ++k)
        {
            if (!live[index(lfoSource(k))])
            {
                continue;
            }
            const auto& l = context.lfos[k];
            lfos[k].setWaveform(l.waveform);
            lfos[k].setFrequency(modulation::lfoRateHz(l.rate + sums.at(Target::lfoRate, k)));
            const float g = std::clamp(l.gain + sums.at(Target::lfoGain, k), 0.0f, 10.0f) / 10.0f;
            lfoGainBefore[k] = atNoteOn || lfoGainNow[k] < 0.0f ? g : lfoGainNow[k];
            lfoGainNow[k] = g;
            lfoOffsetNow[k] = lfoOffsetOf(l);
            lfoAtSegmentStart[k] = lfos[k];
            lfos[k].process(lfoWave[k]);
        }
        gridSums = sums;
        updatePan(atNoteOn);
    }

    /// Pan ramped on the grid, a segment late: from the previous grid
    /// point's sum to this one's (at a note-on from silence, this one's).
    void updatePan(bool atNoteOn) noexcept
    {
        for (std::size_t p = 0; p < partCount; ++p)
        {
            const float now = gridSums.at(Target::pan, p);
            panBefore[p] = atNoteOn ? now : panNow[p];
            panNow[p] = now;
        }
    }

    /// Part p's Pan is modulated this segment: a route, or a sum still
    /// returning to 0.
    [[nodiscard]] bool panModulated(std::size_t p) const noexcept
    {
        return routesTo(Target::pan, p) || panBefore[p] < 0.0f || panBefore[p] > 0.0f || panNow[p] < 0.0f ||
               panNow[p] > 0.0f;
    }

    /// The routes to the per-sample targets (Pitch, Mix), and the sources
    /// they read.
    void updatePerSample() noexcept
    {
        perSample = {};
        const auto collect = [this](const modulation::CompiledRoutes& from, modulation::CompiledRoutes& to)
        {
            std::size_t count = 0;
            for (const auto& r : from.live())
            {
                if (isPerSample(r.target))
                {
                    perSample[index(r.source)] = true;
                    to.routes[count] = r;
                    ++count;
                }
            }
            to.count = count;
        };
        collect(compiled(), perSampleRoutes);
        fadePerSampleRoutes.count = 0;
        if (fading)
        {
            collect(fadeFrom, fadePerSampleRoutes);
        }
    }

    [[nodiscard]] static constexpr bool isPerSample(Target t) noexcept
    {
        return t == Target::pitch || t == Target::mix || t == Target::volume || t == Target::phase;
    }

    /// Source s at sample i of the chunk starting at the block's sample
    /// `done` (the envelopes from their buffers, once rendered).
    [[nodiscard]] float valueAt(Source s, const VoiceKnobs& knobs, const SourceContext& context, std::size_t done,
                                std::size_t i) const noexcept
    {
        switch (s)
        {
        case Source::velocity:
            return velocityRamp.at(i);
        case Source::keytrack:
            return keytrackSource(keytrackPitch(knobs, done, i));
        case Source::ampEnvA:
            return envelopeBuffers[0][i];
        case Source::ampEnvB:
            return envelopeBuffers[1][i];
        case Source::modEnv1:
        case Source::modEnv2:
        {
            const std::size_t k = s == Source::modEnv1 ? 0 : 1;
            return live[index(s)] ? modEnvelopeBuffers[k][i] : 0.0f;
        }
        case Source::lfo1:
        case Source::lfo2:
        {
            const std::size_t k = s == Source::lfo1 ? 0 : 1;
            return live[index(s)] ? lfoValueAt(k, gridPosition + i) : 0.0f;
        }
        case Source::modWheel:
            return context.modWheel != nullptr ? context.modWheel->valueAt(knobs.time + done + i) : 0.0f;
        case Source::aftertouch:
            return context.aftertouch != nullptr ? context.aftertouch->valueAt(knobs.time + done + i) : 0.0f;
        case Source::stepVelocity:
            return context.stepVelocity;
        case Source::none:
        case Source::count:
            break;
        }
        return 0.0f;
    }

    /// Whether source s is constant over the chunk starting at `done`, so
    /// that its routes add one term each (velocity, keytrack, the
    /// controllers and Step Velocity can be; the envelopes and LFOs are
    /// read from their buffers).
    [[nodiscard]] bool constantOver(Source s, const VoiceKnobs& knobs, const SourceContext& context,
                                    std::size_t done) const noexcept
    {
        const auto t = knobs.time + done;
        switch (s)
        {
        case Source::velocity:
            return !velocityRamp.isGliding();
        case Source::keytrack:
            return !glide.isGliding() && (knobs.bend == nullptr || knobs.bend->isSteadyFrom(t));
        case Source::modWheel:
            return context.modWheel == nullptr || context.modWheel->isSteadyFrom(t);
        case Source::aftertouch:
            return context.aftertouch == nullptr || context.aftertouch->isSteadyFrom(t);
        case Source::stepVelocity:
            return true;
        default:
            return false;
        }
    }

    /// The per-sample targets' sums: Pitch, Mix, Volume, Phase.
    static constexpr std::size_t perSampleTargetCount = 4;
    [[nodiscard]] static constexpr std::size_t sumIndex(Target t) noexcept
    {
        switch (t)
        {
        case Target::pitch:
            return 0;
        case Target::mix:
            return 1;
        case Target::volume:
            return 2;
        default:
            return 3; // phase
        }
    }
    static constexpr std::array perSampleTargets{Target::pitch, Target::mix, Target::volume, Target::phase};

    /// The sources over the chunk: the mod envelopes run, the per-sample
    /// sums of Pitch and Mix, and the LFOs' values as they stand after it.
    void renderSources(const VoiceKnobs& knobs, const SourceContext& context, std::size_t done,
                       std::size_t length) noexcept
    {
        for (std::size_t k = 0; k < modEnvelopeCount; ++k)
        {
            if (live[index(modEnvSource(k))])
            {
                modEnvelopes[k].process(std::span{modEnvelopeBuffers[k]}.first(length));
            }
        }
        for (const auto t : perSampleTargets)
        {
            for (std::size_t p = 0; p < partCount; ++p)
            {
                if (routesTo(t, p))
                {
                    std::fill_n(sampleSums[sumIndex(t)][p].begin(), length, 0.0f);
                    if (fading)
                    {
                        std::fill_n(fadeSums[sumIndex(t)][p].begin(), length, 0.0f);
                    }
                }
            }
        }
        if (perSampleRoutes.count > 0 || fadePerSampleRoutes.count > 0)
        {
            sumFolded(knobs, context, done, length);
        }
        for (std::size_t k = 0; k < lfoCount; ++k)
        {
            if (live[index(lfoSource(k))])
            {
                lfoLast[k] = lfoValueAt(k, gridPosition + length - 1);
            }
        }
    }

    /// Folded delivery: each per-sample source into a chunk buffer (an
    /// envelope's own), or, constant over the chunk, one value; then each
    /// route's terms into its parts' sums, in slot order from 0.
    void sumFolded(const VoiceKnobs& knobs, const SourceContext& context, std::size_t done, std::size_t length) noexcept
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
            auto& b = sourceBuffers[s];
            if (constantOver(id, knobs, context, done))
            {
                constant[s] = true;
                b[0] = valueAt(id, knobs, context, done, 0);
                src[s] = b.data();
                continue;
            }
            for (std::size_t i = 0; i < length; ++i)
            {
                b[i] = valueAt(id, knobs, context, done, i);
            }
            src[s] = b.data();
        }
        addRoutes(perSampleRoutes, src, constant, length, sampleSums);
        if (!fading)
        {
            return;
        }
        addRoutes(fadePerSampleRoutes, src, constant, length, fadeSums);
        // The crossfade: each per-sample sum from the old routes' to the new
        // ones', each product in a statement of its own.
        for (const auto t : perSampleTargets)
        {
            for (std::size_t p = 0; p < partCount; ++p)
            {
                if (!routesTo(t, p))
                {
                    continue;
                }
                auto& dst = sampleSums[sumIndex(t)][p];
                const auto& old = fadeSums[sumIndex(t)][p];
                for (std::size_t i = 0; i < length; ++i)
                {
                    const float d = dst[i] - old[i];
                    const float dw = d * fadeWeight(gridPosition + i);
                    dst[i] = old[i] + dw;
                }
            }
        }
    }

    using PerSampleSums = std::array<std::array<Chunk, partCount>, perSampleTargetCount>;

    /// Each route's terms into its parts' sums in `sums`, in slot order.
    static void addRoutes(const modulation::CompiledRoutes& list, const std::array<const float*, sourceCount>& src,
                          const std::array<bool, sourceCount>& constant, std::size_t length,
                          PerSampleSums& sums) noexcept
    {
        for (const auto& route : list.live())
        {
            const auto s = index(route.source);
            const float* values = src[s];
            for (std::size_t p = 0; p < partCount; ++p)
            {
                if (!modulation::includes(route.parts, p))
                {
                    continue;
                }
                auto& dst = sums[sumIndex(route.target)][p];
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
                    // The product in a statement of its own, as accumulate()
                    // forms it (no fused multiply-add).
                    const float term = route.amount * values[i];
                    dst[i] += term;
                }
            }
        }
    }

    /// Source s's interval over the `length` samples from the block's
    /// sample `done` (before the chunk is rendered), by its kind
    /// (modulation::sourceBound()).
    [[nodiscard]] modulation::Interval boundOf(Source s, const VoiceKnobs& knobs, const SourceContext& context,
                                               std::size_t done, std::size_t length) const noexcept
    {
        const auto last = length - 1;
        modulation::BoundInputs in{};
        switch (s)
        {
        case Source::velocity:
            in.ends = modulation::linearBound(velocityRamp.at(0), velocityRamp.at(last));
            break;
        case Source::keytrack:
        {
            // Its parts: the glide and the bend's ramp are each linear between
            // events, but their sum isn't monotonic when they oppose.
            const auto g = modulation::linearBound(glide.at(0), glide.at(last));
            const auto b = modulation::linearBound(bendAt(knobs, done), bendAt(knobs, done + last));
            const auto pitch = g + b;
            in.ends = {
                .lo = keytrackSource(pitch.lo + masterTuneOffset),
                .hi = keytrackSource(pitch.hi + masterTuneOffset),
            };
            break;
        }
        case Source::ampEnvA:
        case Source::ampEnvB:
            in.envelope = modulation::envelopeState(envelopes[s == Source::ampEnvA ? 0 : 1]);
            break;
        case Source::modEnv1:
        case Source::modEnv2:
            if (live[index(s)])
            {
                in.envelope = modulation::envelopeState(modEnvelopes[s == Source::modEnv1 ? 0 : 1]);
            }
            break;
        case Source::lfo1:
        case Source::lfo2:
        {
            const std::size_t k = s == Source::lfo1 ? 0 : 1;
            if (live[index(s)])
            {
                const auto wave =
                    modulation::waveBound(std::span<const float>{lfoWave[k]}.subspan(gridPosition, length));
                const auto gain = modulation::linearBound(lfoGainBefore[k], lfoGainNow[k]);
                const float o = lfoOffsetNow[k];
                in.lfo = modulation::lfoBound(wave, gain, {.lo = o, .hi = o});
            }
            break;
        }
        case Source::modWheel:
        case Source::aftertouch:
        case Source::stepVelocity:
            in.ends =
                modulation::linearBound(valueAt(s, knobs, context, done, 0), valueAt(s, knobs, context, done, last));
            break;
        case Source::none:
        case Source::count:
            break;
        }
        return modulation::sourceBound(s, in);
    }

    /// For the observer: every source's values over the chunk, and, where
    /// the voice took them, every source's bounds.
    void observe(const VoiceKnobs& knobs, const SourceContext& context, std::size_t done, std::size_t length,
                 bool rebound, const modulation::SourceBounds& bounds) const noexcept
    {
        modulation::SourceObservation chunk{};
        chunk.length = length;
        chunk.time = knobs.time + done;
        chunk.rebound = rebound;
        chunk.bounds = bounds;
        for (std::size_t s = 1; s < sourceCount; ++s)
        {
            for (std::size_t i = 0; i < length; ++i)
            {
                chunk.values[s][i] = valueAt(static_cast<Source>(s), knobs, context, done, i);
            }
        }
        observer->observe(chunk);
    }

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

    /// Whether part p's slots can add anything over the next `length`
    /// samples (volumeBounds: each part's Volume bound there).
    [[nodiscard]] bool partProduces(std::size_t p, std::size_t length) const noexcept
    {
        for (std::size_t s = 0; s < slotsPerPart; ++s)
        {
            const auto index = (p * slotsPerPart) + s;
            const auto& slot = slots[index];
            if (slot.soundingVoices() == 0)
            {
                continue;
            }
            const bool silent = slotsModulated[p]
                                    ? slot.isSilentUnder(slotModulation(index, 0, length), volumeBounds[p], length)
                                    : slot.isSilent();
            if (!silent)
            {
                return true;
            }
        }
        return false;
    }

    /// Every sounding slot's left channel is its right, to the bit.
    [[nodiscard]] bool allCentred() const noexcept
    {
        for (std::size_t i = 0; i < slotCount; ++i)
        {
            const auto& s = slots[i];
            if (s.soundingVoices() > 0 && (!s.isCentred() || panModulated(i / slotsPerPart)))
            {
                return false;
            }
        }
        return true;
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
        // The modulation: the grid sums, from the sources as they stood at
        // the grid point (0 with no routes there).
        const float octaves = filterCutoffOctaves({
            .cutoff = read(knobs.cutoff[f], ahead),
            .cutoffModulation = gridSums.at(Target::cutoff, f),
            .controlCutoff = read(knobs.controlCutoff, ahead),
            .controlModulation = gridSums.at(Target::controlCutoff, 0),
            .keytrack = keytrack,
        });
        const float k = filterResonanceK({
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
    [[nodiscard]] bool envelopeSilent(std::size_t p) const noexcept
    {
        const auto& e = envelopes[p];
        const bool zeroHold = e.stage() == AdsrStage::sustain && e.level() <= 0.0f && e.sustainLevel() <= 0.0f;
        return !e.isActive() || zeroHold;
    }

    /// An upper bound on the modulation of part p's Mix, in internal units,
    /// over the `length` samples from the block's sample `done`: each route
    /// to Mix adds its amount times its source's interval there
    /// (modulation::targetBound()). Exactly 0 with no route to it.
    [[nodiscard]] float mixModulationBound(std::size_t part, const VoiceKnobs& knobs, std::size_t done,
                                           std::size_t length) const noexcept
    {
        return modulationBound(Target::mix, part, knobs, done, length);
    }

    /// An upper bound on target t's sum on part `part` over those samples:
    /// each route's amount times its source's interval; during a crossfade
    /// the larger of the old routes' and the new ones' (the sum lies between
    /// them). Exactly 0 with no route to it.
    [[nodiscard]] float modulationBound(Target t, std::size_t part, const VoiceKnobs& knobs, std::size_t done,
                                        std::size_t length) const noexcept
    {
        if (!routesTo(t, part))
        {
            return 0.0f;
        }
        const auto& context = contextOf(knobs);
        modulation::SourceBounds bounds{};
        const auto take = [&](const modulation::CompiledRoutes& list)
        {
            for (const auto& r : list.live())
            {
                if (r.target == t)
                {
                    bounds[index(r.source)] = boundOf(r.source, knobs, context, done, length);
                }
            }
        };
        take(compiled());
        float hi = modulation::targetBound(compiled(), t, part, bounds).hi;
        if (fading)
        {
            take(fadeFrom);
            hi = std::max(hi, modulation::targetBound(fadeFrom, t, part, bounds).hi);
        }
        return hi;
    }

    /// Slot `index`'s modulation for the samples [start, start + run) of the
    /// chunk that starts at the block's sample `done`.
    [[nodiscard]] SlotModulation slotModulation(std::size_t index, std::size_t start, std::size_t run) const noexcept
    {
        const std::size_t p = index / slotsPerPart;
        SlotModulation m;
        if (routesTo(Target::volume, p))
        {
            m.volume = std::span<const float>{sampleSums[2][p]}.subspan(start, run);
        }
        if (panModulated(p))
        {
            m.pan = true;
            m.panBefore = panBefore[p];
            m.panNow = panNow[p];
        }
        m.position = gridPosition + start;
        m.segment = gridSamples;
        // A route that goes crossfades its sum to exactly 0 on the
        // crossfade's last sample, so the offset in force returns to 0 with
        // it: nothing else is needed once the routes are gone.
        if (routesTo(Target::phase, p))
        {
            m.phase = std::span<const float>{sampleSums[3][p]}.subspan(start, run);
        }
        return m;
    }

    /// Part p's Mix, knob plus modulation, can't be above the law's 0 over
    /// the `length` samples from the block's sample `done`: the ramp is
    /// linear, so its larger end bounds it.
    [[nodiscard]] bool mixSilent(std::size_t p, const VoiceKnobs& knobs, std::size_t done,
                                 std::size_t length) const noexcept
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
            volumeBounds[p] = modulationBound(Target::volume, p, knobs, done, rest);
            produces[p] = partProduces(p, rest);
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

    void renderChunk(const AudioBlock& out, const VoiceKnobs& knobs, std::size_t done, bool callStart) noexcept
    {
        const auto length = out.numSamples();
        leaveMonoIfMoved();
        const std::size_t channels = mono ? 1 : 2;
        // A part with no Volume, Pan or Phase modulation takes the slots'
        // plain calls: its SlotModulation would be empty, which is the same
        // to the bit, and the bookkeeping would cost the init patch.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            slotsModulated[p] = routesTo(Target::volume, p) || panModulated(p) || routesTo(Target::phase, p);
        }
        const Plan plan = planChunk(knobs, done);
        const auto& context = contextOf(knobs);

        // For an observer: every source's bound, at a grid point or a call's
        // start (where the skip test may test again), to the segment's end.
        const bool rebound = observer != nullptr && (gridPosition == 0 || callStart);
        modulation::SourceBounds bounds{};
        for (std::size_t s = 1; rebound && s < sourceCount; ++s)
        {
            bounds[s] = boundOf(static_cast<Source>(s), knobs, context, done, gridSamples - gridPosition);
        }

        // 0. The amp envelopes (always; sources too, and nothing they compute
        // depends on the slots), then the other sources and the per-sample
        // sums of Pitch and Mix. With no routes, nothing.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            envelopes[p].process(std::span{envelopeBuffers[p]}.first(length));
        }
        if (!noRoutes())
        {
            renderSources(knobs, context, done, length);
        }
        if (observer != nullptr)
        {
            observe(knobs, context, done, length, rebound, bounds);
        }

        // 1. The oscillators of each heard part, added into its buffer at the
        // pitch plus drift, in runs of constant drift. While the pitch moves
        // (a glide, a bend's ramp) the slots take it every sample; otherwise
        // once, which is the same to the bit at a constant pitch and cheaper.
        // A part whose Pitch is routed takes its own note per sample, the
        // pitch plus its sum. A slot no filter takes, or a silent one, is
        // skipped where that is exact (no gain ramp moving in it); one that
        // can't be is rendered into its part's buffer, which nothing reads
        // this chunk.
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
                bool silent = false;
                bool canSkip = false;
                if (slotsModulated[p])
                {
                    const auto m = slotModulation(index, 0, length);
                    silent = slot.isSilentUnder(m, volumeBounds[p], gridSamples - gridPosition);
                    canSkip = slot.canSkipUnder(m);
                }
                else
                {
                    silent = slot.isSilent();
                    canSkip = slot.canSkip();
                }
                const bool skippable = (!plan.render[p] || silent) && canSkip;
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
                                   const bool perSampleNotes = moving || pitchRouted[p];
                                   auto partNotes = std::span<const float>{notes}.first(run);
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
                                       const auto index = (p * slotsPerPart) + s;
                                       auto& slot = slots[index];
                                       const SlotModulation m =
                                           slotsModulated[p] ? slotModulation(index, start, run) : SlotModulation{};
                                       if (slot.soundingVoices() == 0)
                                       {
                                           slot.trackPhase(m);
                                           continue;
                                       }
                                       if (slotSkipping[index])
                                       {
                                           if (perSampleNotes)
                                           {
                                               slot.skip(partNotes, m);
                                           }
                                           else
                                           {
                                               slot.skip(run, steadyPitch + offset, m);
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
                                           slot.process(piece, partNotes, m);
                                       }
                                       else
                                       {
                                           slot.process(piece, steadyPitch + offset, m);
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
            const auto& e = envelopeBuffers[f];
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

    /// Part p's Mix gain at each sample of the chunk that starts at the
    /// block's sample `done`: its knob ramp read at that sample (unity if
    /// null), plus its modulation's per-sample sum while a route reaches it,
    /// through the law; then times its Solo gain there (none if null).
    /// Unmodulated, a linear ramp is monotonic, so the same value at both
    /// ends of the chunk is a constant over it, computed once (and a
    /// constant Solo of exactly 1 changes nothing).
    void fillMixGains(std::size_t p, const VoiceKnobs& knobs, std::size_t done, std::size_t length) noexcept
    {
        auto& g = mixGains[p];
        const auto at = knobs.offset + done; // the ramps stand that many samples before this block
        if (routesTo(Target::mix, p))
        {
            const auto* ramp = knobs.mix[p];
            const auto& m = sampleSums[1][p];
            for (std::size_t i = 0; i < length; ++i)
            {
                const float knob = ramp != nullptr ? ramp->valueAfter(at + i) : mixLaw.unity;
                g[i] = mixGain({.mix = knob, .modulation = m[i]});
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

    // The modulation sources (M1.5 task 3).
    modulation::RouteCompiler compiler;
    std::array<Adsr, modEnvelopeCount> modEnvelopes{};
    std::array<modulation::ModEnvelopeSettings, modEnvelopeCount> appliedEnvelopes{}; ///< each's last settings
    std::array<Lfo, lfoCount> lfos{Lfo{}, Lfo{}};
    std::array<Lfo, lfoCount> lfoAtSegmentStart{Lfo{}, Lfo{}}; ///< each as it stood at the grid point
    std::array<Chunk, modEnvelopeCount> modEnvelopeBuffers{};
    std::array<Chunk, lfoCount> lfoWave{}; ///< each LFO's wave for the segment, rendered ahead
    std::array<float, lfoCount> lfoGainBefore{};
    std::array<float, lfoCount> lfoGainNow{}; ///< -1: started at this grid point
    std::array<float, lfoCount> lfoOffsetNow{};
    std::array<float, lfoCount> lfoLast{}; ///< each LFO's latest value as a source
    std::array<bool, sourceCount> live{};  ///< a mod envelope or LFO running
    std::array<bool, sourceCount> perSample{};
    modulation::CompiledRoutes perSampleRoutes{};   ///< the routes to Pitch and Mix
    std::array<Chunk, sourceCount> sourceBuffers{}; ///< a moving source's values over the chunk
    PerSampleSums sampleSums{};                     ///< Pitch, Mix, Volume, Phase: each part's per-sample sum
    std::array<Chunk, partCount> partNoteBuffers{}; ///< each part's notes while its Pitch is routed
    modulation::Sums gridSums{};                    ///< the sums at the grid point
    bool segmentBegun = false;
    bool keyHeld = false;
    modulation::SourceObserver* observer = nullptr;

    // The per-voice destinations (M1.5 task 4).
    std::array<float, partCount> panBefore{}; ///< Pan's sum at the previous grid point
    std::array<float, partCount> panNow{};    ///< ... and at this one
    std::array<float, partCount> volumeBounds{};
    std::array<bool, partCount> slotsModulated{};     ///< a part's slots take Volume, Pan or Phase modulation
    modulation::CompiledRoutes fadeFrom{};            ///< the routes a crossfade leaves
    modulation::CompiledRoutes fadePerSampleRoutes{}; ///< ... to the per-sample targets
    PerSampleSums fadeSums{};
    bool fading = false;
    std::size_t fadeIndex = 0;    ///< the crossfade's segments done
    std::size_t fadeSegments = 1; ///< its length (routeCrossfadeSegments())
};

} // namespace anoesis::dsp
