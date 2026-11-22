// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Candidate models for M1.4 task 3 (inventory §7 "Voicing and keyboard",
// P7, L1-L22; §4 E5; docs/dsp/legato-portamento.md): Mono Legato,
// portamento (Normal and Slide), pitch bend with its range, and the hold
// pedal (CC 64). Measured by the hidden [legato-report] and the [legato]
// benchmarks. Nothing here is library code: it is stage 1's prototype, for
// the project owner to choose from before anything is built in libs/dsp.
//
// dsp::Voice takes its pitch once, at noteOn(), so nothing can glide or bend
// it. PitchedVoice is a copy of dsp::Voice as built (M1.4 tasks 1 and 2)
// with one thing added, a pitch input: the voice's pitch is a glide (per
// voice) plus a bend (global, read ahead like the knob ramps), and a legato
// note-on that leaves the envelopes alone. At a constant pitch it is
// dsp::Voice to the bit (the report checks it, through VoiceAllocator too).
//
// What the candidates vary:
//
// - The pitch input's rate (PitchRate): the oscillators given a pitch every
//   sample; or one per 32-sample grid point (the voice's grid, as the filter
//   feed), held from the segment's start or its midpoint, or ramped linearly
//   across the segment. Keytrack follows the same pitch at each grid point,
//   for the segment's end (F18, F22, decided).
// - The glide (GlideLaw): linear in semitones at a constant time or a
//   constant rate (the time per octave); exponential (an RC lag, constant
//   time to -60 dB of the interval); linear in Hz. An interrupted glide
//   restarts from the pitch it had reached, or from its last target.
// - The bend (BendRamp): stepped at each MIDI value, or ramped linearly to
//   each over a time; a 14-bit value scaled by 8192 both ways, or exactly to
//   both ends; reaching every sounding voice, or frozen at a note-off.
// - Legato (Engine): Mono Legato's note priority (last, low, high); a
//   released key returning to a held one legato or with a retrigger; Retrig
//   on a legato note restarting the phases or not; the velocity at a legato
//   note taken at once, ramped over the glide, or kept from the first note.
// - Portamento's modes: Normal glides only while another key is held, Slide
//   always (manual p. 29); a poly voice glides from the last note played,
//   from its own last pitch, or not at all.
// - The hold pedal: CC 64 down at >= 64, at > 0, or with hysteresis; a key
//   pressed again under the pedal takes a new voice or its sustained one; a
//   sustained note counts as held, as released, or between the two for the
//   steal order; in Mono Legato the pedal keeps released keys in the
//   held-key stack, or only delays the final release (and then a key played
//   while it holds the note retriggers, or is legato).
//
// The engine (Engine) is the decided VoiceAllocator (voice_allocator.hpp)
// with a mono mode, the pedal and the glide's bookkeeping added: the same
// pool of 16, records, steal order, freeing at events and each voice's own
// 32-sample points, and split rendering, so with none of task 3's features
// in use it is VoiceAllocator to the bit. Nothing allocates after
// construction.

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
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace anoesis::test::legato
{

namespace dsp = anoesis::dsp;

inline constexpr std::size_t keyCount = 128;

// --- The glide ------------------------------------------------------------------------------

enum class GlideCurve : std::uint8_t
{
    linearSemitones, ///< a straight line in pitch
    exponential,     ///< an RC lag: to + (from - to) e^(-t / tau), tau = T / ln 1000, landing at T
    linearHz,        ///< a straight line in frequency
};

enum class GlideTime : std::uint8_t
{
    constantTime, ///< every interval in `seconds`
    constantRate, ///< `seconds` per octave (an interval of 0 takes no time)
};

struct GlideLaw
{
    GlideCurve curve = GlideCurve::linearSemitones;
    GlideTime time = GlideTime::constantTime;
    float seconds = 0.0f; ///< 0: no glide (Portamento 0)
};

/// A voice's pitch over time, from one note to another, in closed form from
/// the samples since it began, so it reads the same at any sample whatever
/// the block size. Every curve lands exactly on the target after its length.
class Glide
{
public:
    void prepare(double sampleRate) noexcept
    {
        rate = sampleRate;
    }
    void jump(float note) noexcept
    {
        from = note;
        to = note;
        elapsed = 0;
        length = 0;
    }
    void start(float fromNote, float toNote, const GlideLaw& law) noexcept
    {
        double seconds = std::max(0.0, static_cast<double>(law.seconds));
        if (law.time == GlideTime::constantRate)
        {
            seconds *= std::abs(static_cast<double>(toNote) - fromNote) / 12.0;
        }
        const auto n = static_cast<std::uint64_t>(std::llround(seconds * rate));
        if (n == 0 || !(std::abs(toNote - fromNote) > 0.0f))
        {
            jump(toNote);
            return;
        }
        from = fromNote;
        to = toNote;
        curve = law.curve;
        elapsed = 0;
        length = n;
        tau = static_cast<double>(n) / std::log(1000.0);
    }
    /// The pitch `ahead` samples from now.
    [[nodiscard]] float at(std::size_t ahead) const noexcept
    {
        const std::uint64_t n = elapsed + ahead;
        if (n >= length)
        {
            return to;
        }
        const auto t = static_cast<double>(n);
        switch (curve)
        {
        case GlideCurve::exponential:
            return static_cast<float>(to + ((static_cast<double>(from) - to) * std::exp(-t / tau)));
        case GlideCurve::linearHz:
        {
            const double f0 = std::exp2(static_cast<double>(from) / 12.0);
            const double f1 = std::exp2(static_cast<double>(to) / 12.0);
            return static_cast<float>(12.0 * std::log2(f0 + ((f1 - f0) * t / static_cast<double>(length))));
        }
        case GlideCurve::linearSemitones:
            break;
        }
        return from + ((to - from) * static_cast<float>(t / static_cast<double>(length)));
    }
    void advance(std::size_t samples) noexcept
    {
        elapsed = std::min(elapsed + samples, length);
    }
    [[nodiscard]] float target() const noexcept
    {
        return to;
    }
    [[nodiscard]] bool isGliding() const noexcept
    {
        return elapsed < length;
    }
    [[nodiscard]] std::uint64_t lengthSamples() const noexcept
    {
        return length;
    }

private:
    double rate = 48000.0;
    float from = 69.0f;
    float to = 69.0f;
    GlideCurve curve = GlideCurve::linearSemitones;
    std::uint64_t elapsed = 0;
    std::uint64_t length = 0;
    double tau = 1.0;
};

// --- The bend ---------------------------------------------------------------------------------

/// The bend in semitones, global: a linear ramp from the value it had to each
/// new one, over a fixed number of samples (0: a step), at absolute sample
/// times, so every voice reads the same value at the same sample.
class BendRamp
{
public:
    [[nodiscard]] float at(std::uint64_t t) const noexcept
    {
        if (t >= start + length)
        {
            return to;
        }
        if (t <= start)
        {
            return from;
        }
        return from + ((to - from) * (static_cast<float>(t - start) / static_cast<float>(length)));
    }
    void set(std::uint64_t t, float semitones, std::uint64_t rampSamples) noexcept
    {
        from = at(t);
        to = semitones;
        start = t;
        length = rampSamples;
    }
    void reset() noexcept
    {
        from = 0.0f;
        to = 0.0f;
        start = 0;
        length = 0;
    }

private:
    float from = 0.0f;
    float to = 0.0f;
    std::uint64_t start = 0;
    std::uint64_t length = 0;
};

enum class BendScale : std::uint8_t
{
    symmetric, ///< (v - 8192) / 8192: down reaches -range, up range x 8191/8192
    exactEnds, ///< / 8191 above the centre, / 8192 below: both ends exact
};

/// A 14-bit pitch-bend value (0-16383, centre 8192) as a fraction of the
/// range, -1 to 1.
[[nodiscard]] constexpr float bendFraction(int value, BendScale scale) noexcept
{
    const int c = std::clamp(value, 0, 16383) - 8192;
    const float d = (scale == BendScale::exactEnds && c > 0) ? 8191.0f : 8192.0f;
    return static_cast<float>(c) / d;
}

// --- The voice --------------------------------------------------------------------------------

enum class PitchRate : std::uint8_t
{
    perSample,  ///< a pitch every sample (OscillatorSlot::process(out, notes))
    gridStart,  ///< one per grid segment, the pitch at its start, held
    gridMid,    ///< one per grid segment, the pitch at its midpoint, held
    gridRamped, ///< the pitch at each grid point, ramped linearly between them
};

struct VoiceOptions
{
    PitchRate rate = PitchRate::gridStart;
    bool freezeBendOnRelease = false; ///< a released voice keeps the bend it had at its note-off
    bool velocityToAmp = false;       ///< for measurement: the output times the velocity (a stand-in route)
};

/// The voice's knobs: dsp::Voice's filter knobs, plus the global bend ramp
/// and the absolute sample of the block's first sample (for the bend).
struct VoiceKnobs
{
    dsp::VoiceKnobs filter{};
    const BendRamp* bend = nullptr;
    std::uint64_t at = 0;

    [[nodiscard]] VoiceKnobs advanced(std::size_t samples) const noexcept
    {
        VoiceKnobs k = *this;
        k.filter = filter.advanced(samples);
        k.at += samples;
        return k;
    }
};

/// How a note-on moves the pitch: a jump to the note, or a glide from
/// `from` by `law`.
struct GlideStart
{
    bool glide = false;
    float from = 0.0f;
    GlideLaw law{};
};

enum class VelocityLegato : std::uint8_t
{
    atOnce, ///< the new note's velocity from the legato note-on
    ramped, ///< ramped linearly to it over the glide's length
    kept,   ///< the phrase's first note's velocity, unchanged
};

/// dsp::Voice (voice.hpp, as built) with a pitch input. Every line that
/// isn't about the pitch or legato is dsp::Voice's.
template <std::size_t Taps = dsp::minBlepTaps>
class PitchedVoice
{
public:
    using Slot = dsp::OscillatorSlot<Taps>;
    using Knobs = VoiceKnobs;
    static constexpr std::size_t partCount = 2;
    static constexpr std::size_t slotsPerPart = 2;
    static constexpr std::size_t slotCount = partCount * slotsPerPart;
    static constexpr std::size_t gridSamples = dsp::LadderFilter::gridSamples;

    explicit PitchedVoice(const dsp::MinBlepTable& residuals, std::uint64_t seed = 0) noexcept
        : slots{Slot{residuals}, Slot{residuals}, Slot{residuals}, Slot{residuals}}
    {
        for (std::size_t s = 0; s < slotCount; ++s)
        {
            slots[s].setRandomSeed(seed ^ (0x9E3779B97F4A7C15U * (s + 1)));
        }
        pitchDrift.setSeed(seed ^ 0x3C6EF372FE94F82BU);
        updateRoutes();
    }

    void setOptions(const VoiceOptions& o) noexcept
    {
        options = o;
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
    void setFilterInput(std::size_t part, dsp::FilterInput input) noexcept
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
        rate = spec.sampleRate;
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
        glide.jump(69.0f);
        velocityRamp.jump(1.0f);
        played = false;
        released = false;
    }

    /// dsp::Voice::noteOn() at a jump of the pitch.
    void noteOn(float note, const Knobs& knobs) noexcept
    {
        noteOn(note, knobs, {}, 1.0f);
    }
    /// A note-on (from silence, or a retrigger of a sounding voice) with the
    /// pitch jumping or gliding, and the note's velocity (0-1).
    void noteOn(float note, const Knobs& knobs, const GlideStart& start, float velocity) noexcept
    {
        const bool sounding = isActive();
        startPitch(note, start);
        velocityRamp.jump(velocity);
        played = true;
        released = false;
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
            refreshSegment(knobs);
            return;
        }
        applyPending();
        feedFilters(knobs, 0);
        for (auto& f : filters)
        {
            f.reset();
        }
        gridPosition = 0;
    }
    /// A legato note: the pitch moves to `note`, the envelopes are left alone
    /// (Adsr::legatoGateOn: a released or idle one starts), the drift and the
    /// filters carry on, and the slots restart only if `restartPhases` (then
    /// as Retrig says, a band-limited jump).
    void legatoNoteOn(float note, const Knobs& knobs, const GlideStart& start, bool restartPhases, float velocity,
                      VelocityLegato velocityRule) noexcept
    {
        if (!isActive())
        {
            noteOn(note, knobs, start, velocity);
            return;
        }
        startPitch(note, start);
        released = false;
        switch (velocityRule)
        {
        case VelocityLegato::atOnce:
            velocityRamp.jump(velocity);
            break;
        case VelocityLegato::ramped:
            velocityRamp.start(velocityRamp.at(0), velocity,
                               {.seconds = static_cast<float>(static_cast<double>(glide.lengthSamples()) / rate)});
            break;
        case VelocityLegato::kept:
            break;
        }
        if (restartPhases)
        {
            for (auto& s : slots)
            {
                s.noteOn();
            }
        }
        for (auto& e : envelopes)
        {
            e.legatoGateOn();
        }
        refreshSegment(knobs);
    }

    void noteOff() noexcept
    {
        released = true;
        for (auto& e : envelopes)
        {
            e.gateOff();
        }
    }
    /// A note-off that freezes the bend for a released voice, if the option
    /// says so.
    void noteOff(const Knobs& knobs) noexcept
    {
        frozenBend = bendAt(knobs, 0);
        noteOff();
        released = true;
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

    /// The glide's pitch now (no bend), and where it is heading.
    [[nodiscard]] float currentPitch() const noexcept
    {
        return glide.at(0);
    }
    [[nodiscard]] float targetPitch() const noexcept
    {
        return glide.target();
    }
    [[nodiscard]] bool hasPlayed() const noexcept
    {
        return played;
    }
    [[nodiscard]] float velocity() const noexcept
    {
        return velocityRamp.at(0);
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
                feedFilters(knobs, done + gridSamples);
                beginSegment(knobs, done);
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
        bool active = false;
        bool both = false;
    };

    void startPitch(float note, const GlideStart& start) noexcept
    {
        if (start.glide)
        {
            glide.start(start.from, note, start.law);
        }
        else
        {
            glide.jump(note);
        }
    }

    [[nodiscard]] float bendAt(const Knobs& knobs, std::size_t ahead) const noexcept
    {
        if (released && options.freezeBendOnRelease)
        {
            return frozenBend;
        }
        return knobs.bend != nullptr ? knobs.bend->at(knobs.at + ahead) : 0.0f;
    }
    /// The voice's pitch `ahead` samples after the sample `done` of the block
    /// whose knobs these are, where the glide stands at `done`.
    [[nodiscard]] float pitchAt(const Knobs& knobs, std::size_t done, std::size_t ahead) const noexcept
    {
        return glide.at(ahead) + bendAt(knobs, done + ahead);
    }

    /// At a grid point: the pitch the segment holds or ramps between.
    void beginSegment(const Knobs& knobs, std::size_t done) noexcept
    {
        segmentFrom = 0;
        setSegment(knobs, done, gridSamples);
    }
    /// At a note-on mid-segment: the new pitch at once, for the rest of it.
    void refreshSegment(const Knobs& knobs) noexcept
    {
        segmentFrom = gridPosition;
        setSegment(knobs, 0, gridSamples - gridPosition);
    }
    void setSegment(const Knobs& knobs, std::size_t done, std::size_t remaining) noexcept
    {
        segmentStart = pitchAt(knobs, done, 0);
        segmentEnd = pitchAt(knobs, done, remaining);
        segmentMid = pitchAt(knobs, done, remaining / 2);
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
    void updateRoutes() noexcept
    {
        for (std::size_t f = 0; f < partCount; ++f)
        {
            routes[f] = {.active = inputs[f] != dsp::FilterInput::none, .both = inputs[f] == dsp::FilterInput::both};
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
                feedFilter(f, knobs.filter, ahead,
                           dsp::keytrackOctaves(pitchAt(knobs, ahead, 0), knobs.filter.keytrack / 10.0f));
                filters[f].reset();
            }
        }
    }
    [[nodiscard]] static float read(const dsp::LinearSmoother* ramp, std::size_t ahead) noexcept
    {
        return ramp != nullptr ? ramp->valueAfter(ahead) : 0.0f;
    }
    void feedFilter(std::size_t f, const dsp::VoiceKnobs& knobs, std::size_t ahead, float keytrack) noexcept
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
    /// Each running filter, keytracking the pitch at the sample `ahead`
    /// (F22: at a grid point, the segment's end). The glide stands at the
    /// block's sample `ahead - gridSamples` at a grid point, or at 0 at a
    /// note-on from silence.
    void feedFilters(const Knobs& knobs, std::size_t ahead) noexcept
    {
        const std::size_t now = ahead >= gridSamples ? ahead - gridSamples : 0;
        const float keytrack = dsp::keytrackOctaves(pitchAt(knobs, now, ahead - now), knobs.filter.keytrack / 10.0f);
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (routes[f].active && !filters[f].isBypassed())
            {
                feedFilter(f, knobs.filter, ahead, keytrack);
            }
        }
    }
    [[nodiscard]] dsp::AudioBlock block(StereoChunk& chunk, std::array<float*, 2>& pointers,
                                        std::size_t length) noexcept
    {
        pointers = {chunk[0].data(), chunk[1].data()};
        return {pointers.data(), 2, length};
    }

    /// The voice's pitch for each sample of a chunk (not drift).
    void chunkPitches(const Knobs& knobs, std::size_t done, std::size_t length) noexcept
    {
        if (options.rate == PitchRate::perSample)
        {
            for (std::size_t i = 0; i < length; ++i)
            {
                pitches[i] = pitchAt(knobs, done, i);
            }
            return;
        }
        const auto span = static_cast<float>(gridSamples - segmentFrom);
        for (std::size_t i = 0; i < length; ++i)
        {
            const auto t = static_cast<float>(gridPosition + i - segmentFrom) / span;
            pitches[i] = segmentStart + ((segmentEnd - segmentStart) * t);
        }
    }

    void renderChunk(const dsp::AudioBlock& out, const Knobs& knobs, std::size_t done) noexcept
    {
        const auto length = out.numSamples();
        const bool held = options.rate == PitchRate::gridStart || options.rate == PitchRate::gridMid;
        const float heldPitch = options.rate == PitchRate::gridMid ? segmentMid : segmentStart;
        if (!held)
        {
            chunkPitches(knobs, done, length);
        }

        std::array<dsp::AudioBlock, partCount> parts{};
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
                               if (!held)
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
                                       if (slot.soundingVoices() > 0)
                                       {
                                           if (held)
                                           {
                                               slot.process(piece, heldPitch + offset);
                                           }
                                           else
                                           {
                                               slot.process(piece, std::span<const float>{notes}.first(run));
                                           }
                                       }
                                   }
                               }
                           });

        for (std::size_t p = 0; p < partCount; ++p)
        {
            envelopes[p].process(std::span{envelopeBuffers[p]}.first(length));
        }
        if (options.velocityToAmp)
        {
            for (std::size_t i = 0; i < length; ++i)
            {
                velocityGain[i] = velocityRamp.at(i);
            }
        }

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
            const auto y0 = input.channel(0);
            const auto y1 = input.channel(1);
            if (options.velocityToAmp)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    const float g = e[i] * velocityGain[i];
                    left[i] += g * y0[i];
                    right[i] += g * y1[i];
                }
            }
            else
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    left[i] += e[i] * y0[i];
                    right[i] += e[i] * y1[i];
                }
            }
        }
    }

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
                    const float enveloped = envelopeOther[i] * crossing[i];
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

    std::array<dsp::FilterInput, partCount> pendingInputs{dsp::FilterInput::own, dsp::FilterInput::own};
    std::array<bool, partCount> pendingBypass{};
    std::array<dsp::FilterInput, partCount> inputs{dsp::FilterInput::own, dsp::FilterInput::own};
    std::array<Route, partCount> routes{};
    std::array<bool, partCount> heard{};

    std::array<StereoChunk, partCount> partBuffers{};
    std::array<StereoChunk, partCount> filterBuffers{};
    std::array<Chunk, partCount> envelopeBuffers{};
    std::array<std::array<float*, 2>, partCount> partPointers{};
    std::array<std::array<float*, 2>, partCount> filterPointers{};
    Chunk pitches{};
    Chunk notes{};
    Chunk velocityGain{};

    std::array<float, partCount> lastOctaves{};
    std::array<float, partCount> lastK{};
    std::size_t gridPosition = 0;

    VoiceOptions options{};
    double rate = 48000.0;
    Glide glide;
    Glide velocityRamp; ///< 0-1, linear
    std::size_t segmentFrom = 0;
    float segmentStart = 69.0f;
    float segmentEnd = 69.0f;
    float segmentMid = 69.0f;
    float frozenBend = 0.0f;
    bool played = false;
    bool released = false;
};

// --- The engine -------------------------------------------------------------------------------

enum class Priority : std::uint8_t
{
    last, ///< the most recent held key
    low,  ///< the lowest held key
    high, ///< the highest held key
};

enum class ReturnTo : std::uint8_t
{
    legato,    ///< a released key returns to a held one as a legato note
    retrigger, ///< ... with the envelopes retriggered (from their level)
};

enum class PortamentoMode : std::uint8_t
{
    normal, ///< glides only while at least one other key is held (manual p. 29)
    slide,  ///< always glides to the next note
};

enum class InterruptFrom : std::uint8_t
{
    currentPitch, ///< a glide interrupted by a new note restarts from where it had got to
    lastTarget,   ///< ... from the note it was heading for (the pitch jumps there first)
};

enum class PolyGlideFrom : std::uint8_t
{
    lastNote, ///< every new poly note glides from the last note played (its pitch now, if it sounds)
    voiceOwn, ///< from the pitch its voice last had
    noGlide,  ///< poly voices don't glide
};

enum class PedalThreshold : std::uint8_t
{
    atLeast64,  ///< down at 64-127 (the MIDI convention)
    nonZero,    ///< down at 1-127
    hysteresis, ///< down at >= 64, up at < 48
};

enum class PedalReplay : std::uint8_t
{
    newVoice,  ///< a key pressed again under the pedal takes a new voice (as P3)
    sustained, ///< it retriggers its sustained voice
};

enum class PedalSteal : std::uint8_t
{
    sustainedHeld,     ///< a sustained note counts as held for the steal order
    sustainedReleased, ///< it counts as released (stolen before held keys)
    tiered,            ///< released notes first, then sustained ones, then held keys
};

enum class MonoPedal : std::uint8_t
{
    delaysRelease, ///< the held-key stack follows the keys; the pedal only holds the last note's gate
    holdsKeys,     ///< a key released under the pedal stays in the stack until pedal-up
    sustainLegato, ///< as delaysRelease, and a key played while the pedal holds the note is legato
};

struct Policy
{
    bool monoLegato = false;
    Priority priority = Priority::last;
    ReturnTo returnTo = ReturnTo::legato;
    bool legatoRestartsPhases = false; ///< Retrig on a legato note: false keeps the phases
    PortamentoMode mode = PortamentoMode::normal;
    GlideLaw glide{};
    InterruptFrom interrupt = InterruptFrom::currentPitch;
    PolyGlideFrom polyFrom = PolyGlideFrom::lastNote;
    VelocityLegato velocity = VelocityLegato::atOnce;
    float bendRange = 2.0f; ///< semitones, both ways
    BendScale bendScale = BendScale::symmetric;
    float bendRampSeconds = 0.0f;
    PedalThreshold pedalThreshold = PedalThreshold::atLeast64;
    PedalReplay replay = PedalReplay::newVoice;
    PedalSteal pedalSteal = PedalSteal::sustainedHeld;
    MonoPedal monoPedal = MonoPedal::delaysRelease;
    VoiceOptions voice{};
};

enum class EventKind : std::uint8_t
{
    noteOn,    ///< value: the key; velocity 1-127
    noteOff,   ///< value: the key
    pedal,     ///< value: CC 64, 0-127
    bend,      ///< value: 14-bit, 0-16383
    polyphony, ///< value: 0-16
};

struct Event
{
    std::size_t offset = 0;
    EventKind kind = EventKind::noteOn;
    int value = 0;
    int velocity = 100;
};

/// Counters for the report.
struct Counts
{
    std::size_t steals = 0;
    std::size_t legatoNotes = 0;
    std::size_t retriggers = 0; ///< note-ons on a sounding voice that weren't legato
    std::size_t glides = 0;
    std::size_t pedalToggles = 0;
};

template <std::size_t Pool = 16>
class Engine
{
public:
    using Voice = PitchedVoice<>;
    static constexpr std::size_t maxVoices = Pool;
    static constexpr std::size_t checkSamples = 32;

    Engine(std::uint64_t seed, const dsp::MinBlepTable& table) noexcept
        : voices(make(seed, table, std::make_index_sequence<Pool>{}))
    {
    }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;
    ~Engine() = default;

    template <typename F>
    void forEachVoice(const F& configure)
    {
        for (std::size_t i = 0; i < Pool; ++i)
        {
            configure(voices[i]);
        }
    }
    [[nodiscard]] Voice& voice(std::size_t i) noexcept
    {
        return voices[i];
    }
    void setPolicy(const Policy& p) noexcept
    {
        policy = p;
        for (auto& v : voices)
        {
            v.setOptions(p.voice);
        }
    }
    void setPolyphony(std::size_t notes) noexcept
    {
        limit = std::min(notes, Pool);
    }

    void prepare(const dsp::ProcessSpec& spec) noexcept
    {
        rate = spec.sampleRate;
        for (auto& v : voices)
        {
            v.prepare(spec);
        }
        clear();
    }

    /// Adds the busy voices into `out`, handling `events` (sorted) at their
    /// samples. `knobs.at` is the absolute sample of the block's start.
    void process(const dsp::AudioBlock& out, std::span<const Event> events, const VoiceKnobs& blockKnobs) noexcept
    {
        VoiceKnobs knobs = blockKnobs;
        knobs.bend = &bend;
        const auto n = out.numSamples();
        std::size_t from = 0;
        std::size_t e = 0;
        while (from < n)
        {
            while (e < events.size() && events[e].offset <= from)
            {
                handle(events[e], knobs.advanced(from));
                ++e;
            }
            const auto to = e < events.size() ? std::min(n, events[e].offset) : n;
            for (std::size_t i = 0; i < Pool; ++i)
            {
                if (records[i].busy)
                {
                    render(i, out, from, to, knobs);
                }
            }
            from = to;
        }
        for (; e < events.size(); ++e)
        {
            handle(events[e], knobs.advanced(n));
        }
    }

    [[nodiscard]] std::size_t activeNotes() const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(records, &Record::busy));
    }
    [[nodiscard]] bool isBusy(std::size_t i) const noexcept
    {
        return records[i].busy;
    }
    [[nodiscard]] int noteOf(std::size_t i) const noexcept
    {
        return records[i].busy ? records[i].note : -1;
    }
    [[nodiscard]] bool isGateOn(std::size_t i) const noexcept
    {
        return records[i].busy && (records[i].keyDown || records[i].sustained);
    }
    /// Mono Legato: the key sounding (-1 when none has played).
    [[nodiscard]] int soundingKey() const noexcept
    {
        return monoKey;
    }
    [[nodiscard]] bool pedalIsDown() const noexcept
    {
        return pedalDown;
    }
    [[nodiscard]] const Counts& counts() const noexcept
    {
        return counters;
    }
    /// The pitch of voice i now (no bend).
    [[nodiscard]] float pitchOf(std::size_t i) const noexcept
    {
        return voices[i].currentPitch();
    }

private:
    struct Record
    {
        bool busy = false;
        bool keyDown = false;
        bool sustained = false; ///< key up, held by the pedal
        int note = 0;
        std::uint64_t age = 0;
        std::size_t sinceCheck = 0;
    };

    template <std::size_t... I>
    static std::array<Voice, Pool> make(std::uint64_t seed, const dsp::MinBlepTable& table,
                                        std::index_sequence<I...> /*indices*/) noexcept
    {
        return {Voice{table, dsp::voiceSeed(seed, I)}...};
    }

    void clear() noexcept
    {
        records = {};
        counter = 0;
        counters = {};
        keys = {};
        stack = {};
        stackSize = 0;
        monoKey = -1;
        pedalDown = false;
        lastKey = -1;
        bend.reset();
    }

    void free(std::size_t i) noexcept
    {
        voices[i].stop();
        records[i] = {};
    }
    void freeFinished() noexcept
    {
        for (std::size_t i = 0; i < Pool; ++i)
        {
            if (records[i].busy && !voices[i].isActive())
            {
                free(i);
            }
        }
    }

    void handle(const Event& event, const VoiceKnobs& knobs) noexcept
    {
        freeFinished();
        const int key = std::clamp(event.value, 0, static_cast<int>(keyCount) - 1);
        const float velocity = static_cast<float>(std::clamp(event.velocity, 1, 127)) / 127.0f;
        switch (event.kind)
        {
        case EventKind::noteOn:
            keys[static_cast<std::size_t>(key)] = true;
            if (policy.monoLegato)
            {
                monoNoteOn(key, velocity, knobs);
            }
            else
            {
                polyNoteOn(key, velocity, knobs);
            }
            lastKey = key;
            break;
        case EventKind::noteOff:
            keys[static_cast<std::size_t>(key)] = false;
            if (policy.monoLegato)
            {
                monoNoteOff(key, knobs);
            }
            else
            {
                polyNoteOff(key, knobs);
            }
            break;
        case EventKind::pedal:
            pedal(event.value, knobs);
            break;
        case EventKind::bend:
            bend.set(knobs.at, policy.bendRange * bendFraction(event.value, policy.bendScale),
                     static_cast<std::uint64_t>(std::llround(static_cast<double>(policy.bendRampSeconds) * rate)));
            break;
        case EventKind::polyphony:
            setPolyphony(static_cast<std::size_t>(std::max(event.value, 0)));
            break;
        }
    }

    [[nodiscard]] bool otherKeyHeld(int key) const noexcept
    {
        for (std::size_t k = 0; k < keyCount; ++k)
        {
            if (keys[k] && std::cmp_not_equal(k, key))
            {
                return true;
            }
        }
        return false;
    }

    // --- Poly ---

    void polyNoteOn(int key, float velocity, const VoiceKnobs& knobs) noexcept
    {
        if (limit == 0)
        {
            return;
        }
        std::size_t chosen = Pool;
        if (pedalDown && policy.replay == PedalReplay::sustained)
        {
            for (std::size_t i = 0; i < Pool; ++i)
            {
                if (records[i].busy && records[i].sustained && records[i].note == key)
                {
                    chosen = i;
                    break;
                }
            }
        }
        if (chosen == Pool && activeNotes() < limit)
        {
            chosen = firstFree();
        }
        if (chosen == Pool)
        {
            chosen = victim();
            ++counters.steals;
        }
        auto& v = voices[chosen];
        GlideStart g{};
        const bool slides = policy.mode == PortamentoMode::slide || otherKeyHeld(key);
        if (slides && policy.glide.seconds > 0.0f)
        {
            if (policy.polyFrom == PolyGlideFrom::lastNote && lastKey >= 0)
            {
                g = {.glide = true, .from = lastPitch(), .law = policy.glide};
            }
            else if (policy.polyFrom == PolyGlideFrom::voiceOwn && v.hasPlayed())
            {
                g = {.glide = true, .from = v.currentPitch(), .law = policy.glide};
            }
        }
        counters.glides += g.glide ? 1 : 0;
        counters.retriggers += v.isActive() ? 1 : 0;
        records[chosen] = {.busy = true, .keyDown = true, .sustained = false, .note = key, .age = ++counter};
        v.noteOn(static_cast<float>(key), knobs, g, velocity);
        newest = chosen;
    }

    /// The last note played: its voice's pitch now if that voice still
    /// plays it, else its key.
    [[nodiscard]] float lastPitch() const noexcept
    {
        if (newest < Pool && records[newest].busy && records[newest].note == lastKey)
        {
            return voices[newest].currentPitch();
        }
        return static_cast<float>(lastKey);
    }

    void polyNoteOff(int key, const VoiceKnobs& knobs) noexcept
    {
        for (std::size_t i = 0; i < Pool; ++i)
        {
            auto& r = records[i];
            if (r.busy && r.keyDown && r.note == key)
            {
                r.keyDown = false;
                if (pedalDown)
                {
                    r.sustained = true;
                }
                else
                {
                    voices[i].noteOff(knobs);
                }
            }
        }
    }

    [[nodiscard]] std::size_t firstFree() const noexcept
    {
        for (std::size_t i = 0; i < Pool; ++i)
        {
            if (!records[i].busy)
            {
                return i;
            }
        }
        return Pool;
    }
    /// The steal order's rank: lower is stolen first.
    [[nodiscard]] int rank(const Record& r) const noexcept
    {
        if (r.keyDown)
        {
            return 2;
        }
        if (!r.sustained)
        {
            return 0;
        }
        switch (policy.pedalSteal)
        {
        case PedalSteal::sustainedHeld:
            return 2;
        case PedalSteal::sustainedReleased:
            return 0;
        case PedalSteal::tiered:
            break;
        }
        return 1;
    }
    [[nodiscard]] std::size_t victim() const noexcept
    {
        std::size_t best = 0;
        bool found = false;
        for (std::size_t i = 0; i < Pool; ++i)
        {
            const auto& r = records[i];
            if (!r.busy)
            {
                continue;
            }
            const auto& b = records[best];
            const int rr = rank(r);
            const int br = rank(b);
            const bool better = !found || (rr != br ? rr < br : r.age < b.age);
            if (better)
            {
                best = i;
                found = true;
            }
        }
        return best;
    }

    // --- The pedal ---

    void pedal(int value, const VoiceKnobs& knobs) noexcept
    {
        bool down = pedalDown;
        switch (policy.pedalThreshold)
        {
        case PedalThreshold::atLeast64:
            down = value >= 64;
            break;
        case PedalThreshold::nonZero:
            down = value > 0;
            break;
        case PedalThreshold::hysteresis:
            down = pedalDown ? value >= 48 : value >= 64;
            break;
        }
        if (down == pedalDown)
        {
            return;
        }
        ++counters.pedalToggles;
        pedalDown = down;
        if (down)
        {
            return;
        }
        if (policy.monoLegato)
        {
            monoPedalUp(knobs);
            return;
        }
        for (std::size_t i = 0; i < Pool; ++i)
        {
            auto& r = records[i];
            if (r.busy && r.sustained && !r.keyDown)
            {
                r.sustained = false;
                voices[i].noteOff(knobs);
            }
        }
    }

    // --- Mono Legato ---

    void push(int key) noexcept
    {
        remove(key);
        stack[stackSize++] = key;
    }
    void remove(int key) noexcept
    {
        auto* const end = stack.begin() + static_cast<std::ptrdiff_t>(stackSize);
        auto* const it = std::find(stack.begin(), end, key);
        if (it != end)
        {
            std::copy(it + 1, end, it);
            --stackSize;
        }
    }
    [[nodiscard]] int chosenKey() const noexcept
    {
        const auto held = std::span{stack}.first(stackSize);
        switch (policy.priority)
        {
        case Priority::low:
            return *std::ranges::min_element(held);
        case Priority::high:
            return *std::ranges::max_element(held);
        case Priority::last:
            break;
        }
        return held.back();
    }

    [[nodiscard]] GlideStart monoGlide(bool keyHeld) const noexcept
    {
        auto& v = voices[0];
        if (!(policy.glide.seconds > 0.0f) || !v.hasPlayed())
        {
            return {};
        }
        if (!keyHeld && policy.mode == PortamentoMode::normal)
        {
            return {};
        }
        const float from = policy.interrupt == InterruptFrom::currentPitch ? v.currentPitch() : v.targetPitch();
        return {.glide = true, .from = from, .law = policy.glide};
    }

    /// Plays `key` on the mono voice: a new note (from silence, or a
    /// retrigger in place), or a legato note while keys are held.
    void monoPlay(int key, float velocity, bool legato, const VoiceKnobs& knobs) noexcept
    {
        auto& v = voices[0];
        const GlideStart g = monoGlide(legato);
        counters.glides += g.glide ? 1 : 0;
        if (legato && records[0].busy && v.isActive())
        {
            ++counters.legatoNotes;
            v.legatoNoteOn(static_cast<float>(key), knobs, g, policy.legatoRestartsPhases, velocity, policy.velocity);
        }
        else
        {
            counters.retriggers += v.isActive() ? 1 : 0;
            v.noteOn(static_cast<float>(key), knobs, g, velocity);
        }
        records[0] = {.busy = true, .keyDown = true, .sustained = false, .note = key, .age = ++counter};
        monoKey = key;
    }

    void monoNoteOn(int key, float velocity, const VoiceKnobs& knobs) noexcept
    {
        const bool sustainedNote = policy.monoPedal == MonoPedal::sustainLegato && records[0].busy &&
                                   records[0].sustained && voices[0].isActive();
        const bool held = stackSize > 0 || sustainedNote;
        push(key);
        const int target = chosenKey();
        if (!held)
        {
            monoPlay(target, velocity, false, knobs);
            return;
        }
        if (target != monoKey || !records[0].busy)
        {
            monoPlay(target, velocity, true, knobs);
        }
    }

    void monoNoteOff(int key, const VoiceKnobs& knobs) noexcept
    {
        if (pedalDown && policy.monoPedal == MonoPedal::holdsKeys)
        {
            return; // stays in the stack until pedal-up
        }
        remove(key);
        monoAfterRelease(knobs);
    }

    /// After keys leave the stack: return to a held key, or release.
    void monoAfterRelease(const VoiceKnobs& knobs) noexcept
    {
        if (!records[0].busy)
        {
            return;
        }
        if (stackSize == 0)
        {
            if (records[0].keyDown && !pedalDown)
            {
                records[0].keyDown = false;
                voices[0].noteOff(knobs);
            }
            else if (records[0].keyDown)
            {
                records[0].keyDown = false;
                records[0].sustained = true;
            }
            return;
        }
        const int next = chosenKey();
        if (next == monoKey)
        {
            return;
        }
        const float velocity = voices[0].velocity();
        if (policy.returnTo == ReturnTo::legato)
        {
            monoPlay(next, velocity, true, knobs);
        }
        else
        {
            const GlideStart g = monoGlide(true);
            counters.glides += g.glide ? 1 : 0;
            ++counters.retriggers;
            voices[0].noteOn(static_cast<float>(next), knobs, g, velocity);
            records[0].note = next;
            monoKey = next;
        }
    }

    void monoPedalUp(const VoiceKnobs& knobs) noexcept
    {
        if (policy.monoPedal == MonoPedal::holdsKeys)
        {
            for (std::size_t k = 0; k < keyCount; ++k)
            {
                if (!keys[k])
                {
                    remove(static_cast<int>(k));
                }
            }
            if (stackSize == 0 && records[0].busy && records[0].keyDown)
            {
                records[0].keyDown = false;
                voices[0].noteOff(knobs);
                return;
            }
            monoAfterRelease(knobs);
            return;
        }
        if (records[0].busy && records[0].sustained)
        {
            records[0].sustained = false;
            voices[0].noteOff(knobs);
        }
    }

    void render(std::size_t i, const dsp::AudioBlock& out, std::size_t from, std::size_t to,
                const VoiceKnobs& knobs) noexcept
    {
        auto& r = records[i];
        while (from < to)
        {
            const auto length = std::min(to - from, checkSamples - r.sinceCheck);
            voices[i].process(out.subBlock(from, length), knobs.advanced(from));
            from += length;
            r.sinceCheck = (r.sinceCheck + length) % checkSamples;
            if (r.sinceCheck == 0 && !voices[i].isActive())
            {
                free(i);
                return;
            }
        }
    }

    std::array<Voice, Pool> voices;
    std::array<Record, Pool> records{};
    Policy policy{};
    double rate = 48000.0;
    std::size_t limit = Pool;
    std::uint64_t counter = 0;
    Counts counters{};
    BendRamp bend{};
    std::array<bool, keyCount> keys{};
    std::array<int, keyCount> stack{};
    std::size_t stackSize = 0;
    int monoKey = -1;
    int lastKey = -1;
    std::size_t newest = Pool;
    bool pedalDown = false;
};

} // namespace anoesis::test::legato
