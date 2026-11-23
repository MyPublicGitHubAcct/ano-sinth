// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// DestSlot (M1.5 task 4, stage 1; docs/dsp/modulation-destinations.md): a
// copy of dsp::OscillatorSlot as built (M1.1, M1.4 task 5) with what the
// decided rates (M1.5 task 2) need to modulate its Volume, Pan and Phase,
// and the candidates task 4 measures. Prototypes for measurement, not
// library code. It grew from task 2's ModOscillatorSlot. With no modulation
// it is OscillatorSlot to the bit: process(out, note[s]) is the library's
// (the default run checks it through DestVoice).
//
// process(out, note[s], modulation) takes the call's modulation sums in knob
// units (SlotModulation), added to the slot's own knobs before their laws:
//
// - Volume per sample: the knob plus its sum, clamped to [0, 10] (F24, M9),
//   then linear (O4), the invert folded in.
// - Pan ramped on the grid: the knob plus its sum at the segment's two grid
//   points (the previous grid point's sum to this one's, a segment late),
//   clamped to [0, 10], each unison voice's equal-power gains computed there
//   at unisonPosition() (O8) and interpolated linearly in between.
// - Phase per sample: a running offset added to every unison voice's phase.
//   Each sample's change of the offset is added to the phase step (phase
//   modulation as frequency modulation); where the step would leave [0,
//   Nyquist) it is a band-limited jump (jumpTo), counted in phaseJumps().
//   A retrigger lands on Phase plus the offset in force; a note from silence
//   starts at Phase plus setPhaseOffset()'s value. skip() carries the
//   offset's change into the phase it adds up, so a skipped slot comes back
//   where a rendered one would be.
//
// While Volume or Pan is modulated, each unison voice's gains are computed
// outside its gain ramp, from the knobs (KnobSource, question 1) plus the
// sums, times the voice's presence (a 0-1 ramp over the gain ramp's 10 ms
// that fades voices in and out as setVoices() does). The gain ramps,
// presence, the slot's own knob ramps and the noise mix's ramps are all
// advanced in step on both paths (SkippableGain::skip()), so the slot can
// switch between them at any grid point.
//
// Noise (NoiseModulation, question 3): ignored (task 2's prototype), or the
// noise mix at unit volume from the pan positions at the grid points,
// interpolated, times the volume per sample.

#pragma once

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/control/stereo_gain.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/generators/white_noise.hpp>
#include <anoesis/dsp/math/fast_math.hpp>
#include <anoesis/dsp/math/pan.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/oscillators/unison.hpp>
#include <anoesis/dsp/oscillators/waveform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace anoesis::test::destinations
{

namespace dsp = anoesis::dsp;

using dsp::AudioBlock;
using dsp::concertPitchHz;
using dsp::halfCycle;
using dsp::incrementFor;
using dsp::isPeriodic;
using dsp::LinearSmoother;
using dsp::maxUnisonVoices;
using dsp::MinBlepOscillator;
using dsp::MinBlepTable;
using dsp::noteToFrequency;
using dsp::panGains;
using dsp::phaseFromDegrees;
using dsp::ProcessSpec;
using dsp::Random;
using dsp::StereoGain;
using dsp::StereoGains;
using dsp::Tuning;
using dsp::unisonLayout;
using dsp::unisonNoiseMix;
using dsp::unisonPosition;
using dsp::Waveform;
using dsp::WhiteNoise;

/// Where a modulated slot reads its own Volume and Pan knobs (question 1).
enum class KnobSource : std::uint8_t
{
    targets,     ///< the knobs' targets, unramped (task 2's prototype)
    slotRamps,   ///< the slot's own 10 ms ramps in knob units, started where its gain ramps start
    globalRamps, ///< the engine's ramps in knob units, read at absolute samples (as Mix's)
};

/// What a Volume or Pan route does to a Noise slot (question 3).
enum class NoiseModulation : std::uint8_t
{
    ignored, ///< nothing (task 2's prototype)
    mixed,   ///< the mix at unit volume at the grid points, interpolated, times the volume per sample
};

/// A slot's Volume, Pan and Phase modulation for one process() or skip()
/// call, in knob units.
struct SlotModulation
{
    std::span<const float> volume; ///< Volume's sum per sample; empty: no Volume route
    bool pan = false;              ///< Pan modulated: from `panBefore` to `panNow` over the segment
    float panBefore = 0.0f;        ///< Pan's sum at the segment's first grid point
    float panNow = 0.0f;           ///< ... and at its last
    std::size_t position = 0;      ///< the call's first sample's position in the voice's segment
    std::size_t segment = 32;      ///< the segment's length
    std::span<const float> phase;  ///< Phase's sum per sample; empty: the offset in force is kept
    /// globalRamps: the engine's Volume and Pan ramps (knob units, 0-10),
    /// standing `at` samples before the call's first sample.
    const LinearSmoother* volumeKnob = nullptr;
    const LinearSmoother* panKnob = nullptr;
    std::size_t at = 0;

    [[nodiscard]] bool gainsModulated() const noexcept
    {
        return !volume.empty() || pan;
    }
    [[nodiscard]] bool active() const noexcept
    {
        return gainsModulated() || !phase.empty();
    }
};

/// dsp::StereoGain with skip(): its two ramps advanced without rendering, as
/// process() would advance them. Otherwise the library's, to the bit.
class SkippableGain
{
public:
    static constexpr float defaultRampSeconds = StereoGain::defaultRampSeconds;

    SkippableGain() noexcept
    {
        setRampTime(defaultRampSeconds);
        updateTargets();
    }
    void prepare(const ProcessSpec& spec) noexcept
    {
        left.prepare(spec);
        right.prepare(spec);
    }
    void reset() noexcept
    {
        left.reset();
        right.reset();
    }
    void setRampTime(float seconds) noexcept
    {
        left.setRampTime(seconds);
        right.setRampTime(seconds);
    }
    void setGain(float newGain) noexcept
    {
        volume = std::isnan(newGain) ? 0.0f : newGain;
        updateTargets();
    }
    void setPan(float position) noexcept
    {
        pan = position;
        updateTargets();
    }
    void setInverted(bool shouldInvert) noexcept
    {
        inverted = shouldInvert;
        updateTargets();
    }
    [[nodiscard]] StereoGains targetGains() const noexcept
    {
        return {.left = left.targetValue(), .right = right.targetValue()};
    }
    [[nodiscard]] StereoGains currentGains() const noexcept
    {
        return {.left = left.currentValue(), .right = right.currentValue()};
    }
    [[nodiscard]] bool isSmoothing() const noexcept
    {
        return left.isSmoothing() || right.isSmoothing();
    }
    void skip(std::size_t numSamples) noexcept
    {
        left.skip(numSamples);
        right.skip(numSamples);
    }
    void process(std::span<const float> in, const AudioBlock& out) noexcept
    {
        if (out.numChannels() < 2)
        {
            return;
        }
        const auto numSamples = std::min(in.size(), out.numSamples());
        float* const leftOut = out.channel(0).data();
        float* const rightOut = out.channel(1).data();
        const float* const source = in.data();
        if (!isSmoothing())
        {
            const float gainLeft = left.currentValue();
            const float gainRight = right.currentValue();
            for (std::size_t i = 0; i < numSamples; ++i)
            {
                leftOut[i] += gainLeft * source[i];
                rightOut[i] += gainRight * source[i];
            }
            return;
        }
        for (std::size_t i = 0; i < numSamples; ++i)
        {
            leftOut[i] += left.next() * source[i];
            rightOut[i] += right.next() * source[i];
        }
    }

private:
    void updateTargets() noexcept
    {
        const auto gains = panGains(pan);
        const float signedVolume = inverted ? -volume : volume;
        left.setTarget(signedVolume * gains.left);
        right.setTarget(signedVolume * gains.right);
    }

    LinearSmoother left;
    LinearSmoother right;
    float volume = 1.0f;
    float pan = 0.0f;
    bool inverted = false;
};

/// One of a voice's oscillators, with its controls: 0 to 8 unison voices with
/// their detune and stereo spread, tuning (octave, note, fine), the master
/// tune, volume, pan, invert, Phase, and Retrig. It renders each unison voice
/// as a mono band-limited oscillator and adds it, with its own pan, into the
/// voice's stereo bus (pan comes before the filter, so the bus is stereo).
/// For the Noise waveform it runs one noise source instead.
///
/// The controls live here rather than in MinBlepOscillator, which stays a
/// mono, single-voice source: each unison voice is one MinBlepOscillator and
/// one StereoGain, and volume, pan, and invert fold into that gain.
///
/// Pitch is one fractional note: the caller's note (key, bend, glide, pitch
/// modulation), plus tuning.offset(), converted once by noteToFrequency()
/// at the master tune. Each unison voice plays that frequency times its
/// detune ratio, 2^(detune / 12), computed when the detune or the voice
/// count changes. process(out, note) converts once per block;
/// process(out, notes) converts every sample, exactly.
///
/// Unison (docs/dsp/oscillator-unison.md; the laws are in unison.hpp):
///
/// - Voices 0 to 8. 0 renders nothing. Voices are listed from the lowest
///   pitch to the highest.
/// - Detune: the outermost voices' offset in semitones; the others are evenly
///   spaced between (linear in semitones).
/// - Stereo: a width in [0, 1]. Each voice sits at Pan + width x its side,
///   clipped to the hard sides (O8), with the equal-power law per voice.
/// - Every voice has the same level, with no normalisation for the count.
/// - Changing the voice count while the slot sounds fades the voices that go
///   in or out over the gain ramp (10 ms), so it does not click. The voices
///   that stay keep running, at their new detune and position.
/// - Noise: one WhiteNoise per slot. Two independent noise draws per sample
///   are mixed (unisonNoiseMix()) to the power and the left/right
///   correlation that N noise voices at the voices' gains would have.
///   Detune, Phase, and Retrig do not apply.
///
/// Volume, pan, and invert are smoothed per voice by a StereoGain (10 ms
/// linear ramps), and the noise mix by three LinearSmoothers. Pitch is not
/// smoothed: the oscillator's phase is continuous, so a pitch step makes no
/// discontinuity in the waveform.
///
/// Phase and Retrig act at noteOn(), on every unison voice: a retriggered
/// note starts each voice at Phase, as an onset from silence or as a
/// band-limited jump while sounding; a free-running one is left alone, and a
/// voice starting from silence draws its own random phase. Phase is degrees
/// here and fixed point in the oscillator.
///
/// For a voice that skips what can't be heard (M1.4 task 5, docs/dsp/
/// part-solo-and-skipping.md): isSilent() (process() would add exact
/// zeros), isCentred() (left is right to the bit), canSkip() (no gain ramp
/// moves), and skip(), which renders nothing but adds up each oscillator's
/// phase steps; at the next process(), noteOn() or setVoices(), each one
/// jumps there with MinBlepOscillator::jumpTo(), band-limited, so a
/// free-running phase carries on unheard.
///
/// Satisfies Preparable. No allocation: fixed capacity for 8 voices, the
/// table is shared, and the mono signal goes through a small buffer on the
/// stack. About 5 KB at 64 taps.
///
/// Docs: docs/dsp/oscillator-controls.md, docs/dsp/oscillator-phase.md,
/// docs/dsp/oscillator-unison.md, docs/dsp/part-solo-and-skipping.md
template <std::size_t Taps>
class DestSlot
{
public:
    /// The noise source's seed is the slot's seed XOR this (the fractional
    /// part of sqrt(2)), so it is not the phase sequence.
    static constexpr std::uint64_t noiseSeedSalt = 0x6A09E667F3BCC909U;

    /// The table must outlive the slot (see MinBlepOscillator).
    explicit DestSlot(const MinBlepTable& residuals) noexcept
        : oscillators(makeOscillators(residuals, std::make_index_sequence<maxUnisonVoices>{}))
    {
        for (auto& ramp : mixRamps)
        {
            ramp.setRampTime(StereoGain::defaultRampSeconds);
        }
        for (std::size_t v = count; v < maxUnisonVoices; ++v)
        {
            gains[v].setGain(0.0f);
            gains[v].reset();
        }
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            presence[v].setRampTime(StereoGain::defaultRampSeconds);
            presence[v].setCurrentAndTarget(v < count ? 1.0f : 0.0f);
        }
        volumeKnob.setRampTime(StereoGain::defaultRampSeconds);
        panKnob.setRampTime(StereoGain::defaultRampSeconds);
        volumeKnob.setCurrentAndTarget(10.0f);
        panKnob.setCurrentAndTarget(5.0f);
        noise.setSeed(noiseSeedSalt);
        updateNoiseMix();
    }

    void prepare(const ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        for (auto& oscillator : oscillators)
        {
            oscillator.prepare(spec);
        }
        for (auto& gain : gains)
        {
            gain.prepare(spec);
        }
        for (auto& ramp : mixRamps)
        {
            ramp.prepare(spec);
        }
        for (auto& p : presence)
        {
            p.prepare(spec);
        }
        volumeKnob.prepare(spec);
        panKnob.prepare(spec);
        reset();
    }

    /// stop(), and restarts the random sequences (the free-running start
    /// phases and the noise) from their seed: the slot is as freshly
    /// prepared.
    void reset() noexcept
    {
        stop();
        random.reset();
        noise.reset();
    }

    /// For a voice that has finished: silences every oscillator, so the next
    /// noteOn() starts them from silence, and finishes every gain ramp (so
    /// voices that were fading out are at 0, and are dropped before the next
    /// sample). Unlike reset(), the random sequences carry on, so each new
    /// free-running note draws new start phases. Without a noteOn(),
    /// process() starts the oscillators at phase 0.
    void stop() noexcept
    {
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            oscillators[v].reset();
            gains[v].reset();
            presence[v].reset();
        }
        for (auto& ramp : mixRamps)
        {
            ramp.reset();
        }
        volumeKnob.reset();
        panKnob.reset();
        panCached = false;
        noteHeld = false;
        running = false;
        pending = {};
        skipped = false;
        appliedPhase = 0;
    }

    /// A note-on, between two process() calls (a voice splits its block at
    /// the event, so the note-on is sample-accurate). `since`, in [0, 1),
    /// places it that many samples before the next sample, for events finer
    /// than a sample. For every unison voice:
    ///
    /// - Retrig on: the oscillator restarts at Phase, with
    ///   MinBlepOscillator::jumpTo(). From silence, that is the band-limited
    ///   onset at Phase, moved on by `since` x the first sample's increment;
    ///   while it sounds (a stolen voice, or a retriggered mono note), a
    ///   band-limited jump to Phase at that time.
    /// - Retrig off: a sounding oscillator runs on untouched. From silence,
    ///   it starts at a random phase from this slot's Random, drawn in voice
    ///   order (a working hypothesis, inventory O10: a free-running
    ///   oscillator has been running unheard, so its phase is arbitrary).
    void noteOn(float since = 0.0f) noexcept
    {
        resume(); // a retrigger jumps from where the phase would be
        noteHeld = true;
        for (std::size_t v = 0; v < count; ++v)
        {
            startVoice(v, since);
        }
    }

    /// The number of unison voices, 0 (off) to 8; others are clamped. While
    /// the slot sounds, voices that are added start as at a note-on (if a
    /// note is held) and fade in, and voices that are removed fade out, both
    /// over the gain ramp. Before the first sample after a note-on, prepare(),
    /// reset(), or stop(), nothing has been heard, and the change is
    /// immediate.
    void setVoices(int voices) noexcept
    {
        const auto newCount = static_cast<std::size_t>(std::clamp(voices, 0, static_cast<int>(maxUnisonVoices)));
        if (newCount == count)
        {
            return;
        }
        resume();
        count = newCount;
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            if (v < count && states[v] != VoiceState::on)
            {
                // A voice still fading out is caught and brought back; one
                // that has finished, or never ran, starts afresh.
                if (states[v] == VoiceState::off || !gains[v].isSmoothing())
                {
                    oscillators[v].reset();
                    if (noteHeld)
                    {
                        startVoice(v);
                    }
                }
                states[v] = VoiceState::on;
                gains[v].setGain(volume);
                gains[v].setInverted(inverted);
                presence[v].setTarget(1.0f);
            }
            else if (v >= count && states[v] == VoiceState::on)
            {
                gains[v].setGain(0.0f);
                presence[v].setTarget(0.0f);
                states[v] = running ? VoiceState::fading : VoiceState::off;
                if (!running)
                {
                    oscillators[v].reset();
                    gains[v].reset();
                    presence[v].reset();
                }
            }
        }
        updateDetune();
        updatePositions();
        updateNoiseMix();
    }

    [[nodiscard]] std::size_t voices() const noexcept
    {
        return count;
    }

    /// The voices being rendered: the current ones, plus any still fading
    /// out.
    [[nodiscard]] std::size_t soundingVoices() const noexcept
    {
        std::size_t sounding = 0;
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            if (states[v] == VoiceState::on || (states[v] == VoiceState::fading && gains[v].isSmoothing()))
            {
                ++sounding;
            }
        }
        return sounding;
    }

    /// Whether a voice's oscillator has produced output since it last started
    /// from silence. A voice that is off, or has faded out, is not rendered
    /// at all, so this is false for it.
    [[nodiscard]] bool isVoiceRunning(std::size_t voice) const noexcept
    {
        return oscillators[std::min(voice, maxUnisonVoices - 1)].isRunning();
    }

    /// The detune spread: the outermost voices' offset from the note, in
    /// semitones (unison.hpp spaces the others evenly between). NaN counts as
    /// 0, and the spread is clamped to +-1200 semitones. Sylenth1's Detune
    /// knob, and its dependence on the voice count, map to it in the
    /// parameter table (inventory O13).
    void setDetune(float semitones) noexcept
    {
        spread = std::isnan(semitones) ? 0.0f : std::clamp(semitones, -maxSpread, maxSpread);
        updateDetune();
    }

    /// The stereo width, 0 (every voice at Pan) to 1 (the side voices at
    /// Pan +-1); clamped, NaN is 0. See unisonPosition().
    void setStereo(float width) noexcept
    {
        stereoWidth = width;
        updatePositions();
        updateNoiseMix();
    }

    /// The start phase for a retriggered note-on, in degrees (any value,
    /// wrapped to one cycle). It takes effect at the next note-on, not on a
    /// held note.
    void setPhaseDegrees(float degrees) noexcept
    {
        startPhase = phaseFromDegrees(degrees);
    }

    /// Retrig: whether a note-on restarts the oscillators at Phase (on, the
    /// default) or leaves them free-running (off).
    void setRetrigger(bool shouldRetrigger) noexcept
    {
        retrigger = shouldRetrigger;
    }

    /// The seed of the free-running start phases and of the noise. Give each
    /// slot its own, or they start in step. Restarts both sequences.
    void setRandomSeed(std::uint64_t seed) noexcept
    {
        random.setSeed(seed);
        noise.setSeed(seed ^ noiseSeedSalt);
    }

    /// Any of the 8 waveforms. Noise switches the slot to its noise source;
    /// the others play on the oscillators. A switch mid-note is immediate.
    void setWaveform(Waveform waveform) noexcept
    {
        noiseSelected = !isPeriodic(waveform);
        for (auto& oscillator : oscillators)
        {
            oscillator.setWaveform(waveform);
        }
    }

    void setTuning(const Tuning& newTuning) noexcept
    {
        tuning = newTuning;
    }

    /// The master tune: the frequency of A4 in Hz (see noteToFrequency()).
    void setReferenceHz(float hz) noexcept
    {
        referenceHz = hz;
    }

    /// Linear gain per voice; Sylenth1's Volume knob maps to it linearly
    /// (inventory O4).
    void setVolume(float linearGain) noexcept
    {
        volume = linearGain;
        volumeKnob.setTarget(linearGain * 10.0f);
        for (std::size_t v = 0; v < count; ++v)
        {
            gains[v].setGain(volume);
        }
        settleIfSilent();
        updateNoiseMix();
    }

    /// -1 (hard left) to +1 (hard right), equal-power (inventory O6). With
    /// unison, every voice's position moves by it (O8).
    void setPan(float position) noexcept
    {
        pan = position;
        panKnob.setTarget((position + 1.0f) * 5.0f);
        updatePositions();
        updateNoiseMix();
    }

    void setInverted(bool shouldInvert) noexcept
    {
        inverted = shouldInvert;
        for (std::size_t v = 0; v < count; ++v)
        {
            gains[v].setInverted(inverted);
        }
        settleIfSilent();
        updateNoiseMix();
    }

    /// The frequency the centre of the unison plays for `note`: note +
    /// tuning at the master tune.
    [[nodiscard]] float frequencyFor(float note) const noexcept
    {
        return noteToFrequency(note + tuning.offset(), referenceHz);
    }

    /// A voice's oscillator, for measurement (its phase, its residuals).
    [[nodiscard]] const MinBlepOscillator<Taps>& oscillator(std::size_t voice) const noexcept
    {
        return oscillators[std::min(voice, maxUnisonVoices - 1)];
    }

    /// A voice's current phase step (2^32 per cycle): the pitch it is
    /// actually playing, for measurement.
    [[nodiscard]] std::uint32_t phaseIncrement(std::size_t voice = 0) const noexcept
    {
        return oscillators[std::min(voice, maxUnisonVoices - 1)].phaseIncrement();
    }

    /// The gains a voice is heading for (volume, invert, and its pan
    /// position folded in).
    [[nodiscard]] StereoGains voiceGains(std::size_t voice) const noexcept
    {
        return gains[std::min(voice, maxUnisonVoices - 1)].targetGains();
    }

    [[nodiscard]] float latencySamples() const noexcept
    {
        return oscillators[0].latencySamples();
    }

    /// Adds this slot's output at `note` (constant for the block) into
    /// channels 0 and 1 of `out`.
    void process(const AudioBlock& out, float note) noexcept
    {
        resume();
        const float centre = frequencyFor(note);
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            if (states[v] != VoiceState::off)
            {
                oscillators[v].setFrequency(centre * ratios[v]);
            }
        }
        render(out, out.numSamples(), {});
    }

    /// As above, with a note per sample (`notes[i]` for sample i), for the
    /// shorter of `notes` and the block.
    void process(const AudioBlock& out, std::span<const float> notes) noexcept
    {
        resume();
        const auto numSamples = std::min(out.numSamples(), notes.size());
        render(out, numSamples, notes.first(numSamples));
    }

    /// As process(out, note), with Volume, Pan and Phase modulated (see the
    /// file's comment). With nothing active it is process(out, note).
    void process(const AudioBlock& out, float note, const SlotModulation& modulation) noexcept
    {
        if (!modulated(modulation))
        {
            process(out, note);
            return;
        }
        resume();
        const float centre = frequencyFor(note);
        // With Phase per sample, each sample sets its own step; setting the
        // base here too would add a step change per call, so the output
        // would depend on how the caller splits its blocks.
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            if (states[v] != VoiceState::off)
            {
                baseIncrements[v] = incrementFor(centre * ratios[v], sampleRate);
                if (modulation.phase.empty())
                {
                    oscillators[v].setIncrement(baseIncrements[v]);
                }
            }
        }
        renderModulated(out, out.numSamples(), {}, modulation);
    }

    /// As process(out, notes), modulated.
    void process(const AudioBlock& out, std::span<const float> notes, const SlotModulation& modulation) noexcept
    {
        if (!modulated(modulation))
        {
            process(out, notes);
            return;
        }
        resume();
        const auto numSamples = std::min(out.numSamples(), notes.size());
        renderModulated(out, numSamples, notes.first(numSamples), modulation);
    }

    /// As skip(numSamples, note), with the call's modulation: the Phase
    /// offset's change is added to each oscillator's skipped steps, as
    /// rendering would have added it.
    void skip(std::size_t numSamples, float note, const SlotModulation& modulation) noexcept
    {
        if (noiseSelected && modulated(modulation))
        {
            skipNoiseModulated(numSamples);
            return;
        }
        skip(numSamples, note);
        skipModulation(numSamples, modulation);
    }
    void skip(std::span<const float> notes, const SlotModulation& modulation) noexcept
    {
        if (noiseSelected && modulated(modulation))
        {
            skipNoiseModulated(notes.size());
            return;
        }
        skip(notes);
        skipModulation(notes.size(), modulation);
    }

    /// Question 1: where a modulated slot reads its Volume and Pan knobs.
    void setKnobSource(KnobSource source) noexcept
    {
        knobSource = source;
    }
    /// Question 3: what Volume and Pan routes do to Noise.
    void setNoiseModulation(NoiseModulation n) noexcept
    {
        noiseModulation = n;
    }

    /// While no voice sounds (the voice doesn't call process() or skip()):
    /// the offset in force follows the call's Phase sum, so a voice that
    /// unison brings back starts at Phase plus the offset as it stands.
    void trackPhase(const SlotModulation& modulation) noexcept
    {
        if (!modulation.phase.empty())
        {
            appliedPhase = phaseOf(modulation.phase.back());
        }
    }

    /// The Phase offset for a note from silence, in knob units (10 per
    /// cycle): where a retriggered onset lands, with Phase.
    void setPhaseOffset(float units) noexcept
    {
        appliedPhase = phaseOf(units);
    }

    /// Whether a call with this modulation renders exact zeros for the next
    /// `length` samples, given an upper bound on Volume's sum there: the
    /// Volume knob's larger end there plus the bound can't lift the clamp
    /// above 0. Unmodulated, isSilent().
    [[nodiscard]] bool isSilentUnder(const SlotModulation& modulation, float volumeBound,
                                     std::size_t length) const noexcept
    {
        if (!modulated(modulation) || !modulation.gainsModulated())
        {
            return isSilent();
        }
        return volumeKnobMax(modulation, length) + volumeBound <= 0.0f;
    }

    /// Whether skip() with this modulation keeps the slot exact. Modulated,
    /// the slot's knob ramps and the voices' presence must be still too.
    [[nodiscard]] bool canSkipUnder(const SlotModulation& modulation) const noexcept
    {
        if (!canSkip())
        {
            return false;
        }
        if (!modulated(modulation))
        {
            return true;
        }
        return !volumeKnob.isSmoothing() && !panKnob.isSmoothing() &&
               std::ranges::none_of(presence, [](const LinearSmoother& p) { return p.isSmoothing(); });
    }

    /// Phase changes made as band-limited jumps because the phase step
    /// would have left [0, Nyquist), since prepare().
    [[nodiscard]] std::uint64_t phaseJumps() const noexcept
    {
        return phaseJumpCount;
    }

    /// The phase offset in force, 2^32 per cycle.
    [[nodiscard]] std::uint32_t phaseOffset() const noexcept
    {
        return appliedPhase;
    }

    /// Whether process() would add exact zeros: every voice it renders has
    /// its gains settled at exactly 0 (Volume 0, or none sounding), or, for
    /// Noise, the noise mix has.
    [[nodiscard]] bool isSilent() const noexcept
    {
        if (noiseSelected)
        {
            return noiseSettledAtZero();
        }
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            if (states[v] == VoiceState::off || (states[v] == VoiceState::fading && !gains[v].isSmoothing()))
            {
                continue;
            }
            const auto g = gains[v].targetGains();
            if (gains[v].isSmoothing() || !isZero(g.left) || !isZero(g.right))
            {
                return false;
            }
        }
        return true;
    }

    /// Whether the left channel it adds is its right, to the bit, for as long
    /// as no control changes: every voice it renders has its gains, now and
    /// at their target, equal left and right (pan and stereo spread at the
    /// centre). Never for Noise, which mixes two independent draws into the
    /// two channels.
    [[nodiscard]] bool isCentred() const noexcept
    {
        const auto same = [](float a, float b)
        {
            return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
        };
        if (noiseSelected)
        {
            const float a = mixRamps[0].currentValue();
            const float c = mixRamps[1].currentValue();
            const float b = mixRamps[2].currentValue();
            return !mixRamps[0].isSmoothing() && !mixRamps[1].isSmoothing() && !mixRamps[2].isSmoothing() &&
                   same(a, c) && same(c, b);
        }
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            if (states[v] == VoiceState::off)
            {
                continue;
            }
            const auto now = gains[v].currentGains();
            const auto target = gains[v].targetGains();
            if (!same(now.left, now.right) || !same(target.left, target.right))
            {
                return false;
            }
        }
        return true;
    }

    /// Whether skip() keeps the slot exact: no ramp it renders with moves,
    /// since skip() doesn't advance them: the voices' gains (one fading out
    /// included) or, for Noise, the noise mix's. (The noise mix follows every
    /// Volume and Pan change but only Noise advances it, so a saw slot
    /// doesn't wait on it.)
    [[nodiscard]] bool canSkip() const noexcept
    {
        if (noiseSelected)
        {
            return !mixRamps[0].isSmoothing() && !mixRamps[1].isSmoothing() && !mixRamps[2].isSmoothing();
        }
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            if (states[v] != VoiceState::off && gains[v].isSmoothing())
            {
                return false;
            }
        }
        return true;
    }

    /// `numSamples` samples at `note` without rendering (call it only when
    /// canSkip()): each running oscillator's phase step is added up (2^32 per
    /// cycle, so the sum is exact in uint32), a voice that has faded out is
    /// dropped as process() drops it, and the noise draws its numbers. At
    /// the next process(), noteOn() or setVoices(), each oscillator jumps to
    /// the phase it would have had: the output differs from rendering only
    /// by the band-limiting residual of the last Taps samples, for Taps
    /// samples.
    void skip(std::size_t numSamples, float note) noexcept
    {
        const float centre = frequencyFor(note);
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            if (!dropIfFaded(v))
            {
                oscillators[v].setFrequency(centre * ratios[v]);
                pending[v] += static_cast<std::uint32_t>(numSamples) * oscillators[v].phaseIncrement();
            }
        }
        skipNoise(numSamples);
        skipRamps(numSamples);
        skipped = skipped || numSamples > 0;
        running = running || numSamples > 0;
    }

    /// As above, with a note per sample (`notes[i]` for sample i).
    void skip(std::span<const float> notes) noexcept
    {
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            if (dropIfFaded(v))
            {
                continue;
            }
            for (const float note : notes)
            {
                oscillators[v].setFrequency(frequencyFor(note) * ratios[v]);
                pending[v] += oscillators[v].phaseIncrement();
            }
        }
        skipNoise(notes.size());
        skipRamps(notes.size());
        skipped = skipped || !notes.empty();
        running = running || !notes.empty();
    }

private:
    // The mono signal is staged in chunks of this many samples on the
    // stack, then panned into the bus.
    static constexpr std::size_t chunkSize = 64;
    static constexpr float maxSpread = 1200.0f;

    enum class VoiceState : std::uint8_t
    {
        off,    ///< silent; its gain is settled at 0
        on,     ///< one of the current voices
        fading, ///< removed while sounding; rendered until its gain reaches 0
    };

    template <std::size_t... Index>
    static std::array<MinBlepOscillator<Taps>, maxUnisonVoices>
    makeOscillators(const MinBlepTable& residuals, std::index_sequence<Index...> /*indices*/) noexcept
    {
        return {((void)Index, MinBlepOscillator<Taps>{residuals})...};
    }

    void startVoice(std::size_t v, float since = 0.0f) noexcept
    {
        if (retrigger)
        {
            oscillators[v].jumpTo(startPhase + appliedPhase, since);
        }
        else if (!oscillators[v].isRunning())
        {
            oscillators[v].restart(random.nextUint32());
        }
    }

    // Before anything has been heard, a control change takes effect at once.
    void settleIfSilent() noexcept
    {
        if (running)
        {
            return;
        }
        for (std::size_t v = 0; v < count; ++v)
        {
            gains[v].reset();
        }
        for (auto& p : presence)
        {
            p.reset();
        }
        volumeKnob.reset();
        panKnob.reset();
    }

    void updateDetune() noexcept
    {
        const auto& layout = unisonLayout(count);
        for (std::size_t v = 0; v < count; ++v)
        {
            ratios[v] = std::exp2(spread * layout.detune[v] / 12.0f);
        }
    }

    void updatePositions() noexcept
    {
        const auto& layout = unisonLayout(count);
        for (std::size_t v = 0; v < count; ++v)
        {
            gains[v].setPan(unisonPosition(pan, stereoWidth, layout.side[v]));
        }
        settleIfSilent();
    }

    void updateNoiseMix() noexcept
    {
        std::array<StereoGains, maxUnisonVoices> voiceTargets{};
        for (std::size_t v = 0; v < count; ++v)
        {
            voiceTargets[v] = gains[v].targetGains();
        }
        const auto mix = unisonNoiseMix(std::span{voiceTargets.data(), count}, inverted);
        mixRamps[0].setTarget(mix.left);
        mixRamps[1].setTarget(mix.cross);
        mixRamps[2].setTarget(mix.right);
        if (!running)
        {
            for (auto& ramp : mixRamps)
            {
                ramp.reset();
            }
        }
    }

    void render(const AudioBlock& out, std::size_t numSamples, std::span<const float> notes) noexcept
    {
        if (noiseSelected)
        {
            renderNoise(out, numSamples);
        }
        else
        {
            renderVoices(out, numSamples, notes);
        }
        advanceRamps(numSamples, false);
        for (auto& p : presence)
        {
            p.skip(numSamples);
        }
        panCached = false;
        running = running || numSamples > 0;
    }

    void renderVoices(const AudioBlock& out, std::size_t numSamples, std::span<const float> notes) noexcept
    {
        std::array<float, chunkSize> mono;   // NOLINT(cppcoreguidelines-pro-type-member-init): written before read
        std::array<float, chunkSize> centre; // NOLINT(cppcoreguidelines-pro-type-member-init): written before read
        for (std::size_t start = 0; start < numSamples; start += chunkSize)
        {
            const auto length = std::min(chunkSize, numSamples - start);
            if (!notes.empty())
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    centre[i] = frequencyFor(notes[start + i]);
                }
            }
            for (std::size_t v = 0; v < maxUnisonVoices; ++v)
            {
                if (states[v] == VoiceState::fading && !gains[v].isSmoothing())
                {
                    // Faded out: silent from here on.
                    states[v] = VoiceState::off;
                    oscillators[v].reset();
                }
                if (states[v] == VoiceState::off)
                {
                    continue;
                }
                auto& oscillator = oscillators[v];
                if (notes.empty())
                {
                    for (std::size_t i = 0; i < length; ++i)
                    {
                        mono[i] = oscillator.next();
                    }
                }
                else
                {
                    const float ratio = ratios[v];
                    for (std::size_t i = 0; i < length; ++i)
                    {
                        oscillator.setFrequency(centre[i] * ratio);
                        mono[i] = oscillator.next();
                    }
                }
                gains[v].process(std::span{mono.data(), length}, out.subBlock(start, length));
            }
        }
    }

    /// Phase knob units (10 per cycle) to a phase, wrapped.
    [[nodiscard]] static std::uint32_t phaseOf(float units) noexcept
    {
        const double cycles = static_cast<double>(units) / 10.0;
        const double wrapped = cycles - std::floor(cycles);
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(std::ldexp(wrapped, 32)) & 0xFFFFFFFFU);
    }

    /// Whether this call takes the modulated path: Noise only for its gains,
    /// and only when it mixes them.
    [[nodiscard]] bool modulated(const SlotModulation& m) const noexcept
    {
        if (noiseSelected)
        {
            return noiseModulation == NoiseModulation::mixed && m.gainsModulated();
        }
        return m.active();
    }

    /// The Volume knob in knob units, as the modulated path reads it at the
    /// call's sample i (the slot's own ramp is read by advancing it).
    [[nodiscard]] float globalVolumeAt(const SlotModulation& m, std::size_t i) const noexcept
    {
        return m.volumeKnob != nullptr ? m.volumeKnob->valueAfter(m.at + i) : volume * 10.0f;
    }

    /// The Pan knob in knob units `ahead` samples from the call's start.
    [[nodiscard]] float panKnobAfter(const SlotModulation& m, std::size_t ahead) const noexcept
    {
        switch (knobSource)
        {
        case KnobSource::targets:
            break;
        case KnobSource::slotRamps:
            return panKnob.valueAfter(ahead);
        case KnobSource::globalRamps:
            if (m.panKnob != nullptr)
            {
                return m.panKnob->valueAfter(m.at + ahead);
            }
            break;
        }
        return (pan + 1.0f) * 5.0f;
    }

    /// The Volume knob's largest value over the call's next `length`
    /// samples (a linear ramp: its ends).
    [[nodiscard]] float volumeKnobMax(const SlotModulation& m, std::size_t length) const noexcept
    {
        const auto last = std::max<std::size_t>(length, 1);
        switch (knobSource)
        {
        case KnobSource::targets:
            break;
        case KnobSource::slotRamps:
            return std::max(volumeKnob.valueAfter(1), volumeKnob.valueAfter(last));
        case KnobSource::globalRamps:
            if (m.volumeKnob != nullptr)
            {
                return std::max(m.volumeKnob->valueAfter(m.at), m.volumeKnob->valueAfter(m.at + last - 1));
            }
            break;
        }
        return volume * 10.0f;
    }

    /// Voice v's equal-power gains at Pan `units` (knob plus modulation),
    /// clamped to [0, 10], at its unison position.
    [[nodiscard]] StereoGains panGainsAt(std::size_t v, float units) const noexcept
    {
        const float clamped = std::clamp(units, 0.0f, 10.0f);
        const auto& layout = unisonLayout(count);
        return panGains(unisonPosition((clamped / 5.0f) - 1.0f, stereoWidth, layout.side[v]));
    }

    /// The pan gains at the segment's two grid points: the first taken when
    /// the segment starts (cached), the last for this call (the knob at the
    /// segment's end, in closed form).
    void panEnds(const SlotModulation& m) noexcept
    {
        const auto toEnd = m.segment > m.position ? m.segment - m.position : 0;
        const float startUnits = panKnobAfter(m, 0) + m.panBefore;
        const float endUnits = panKnobAfter(m, toEnd) + m.panNow;
        if (m.position == 0 || !panCached)
        {
            for (std::size_t v = 0; v < maxUnisonVoices; ++v)
            {
                panStart[v] = panGainsAt(v, startUnits);
            }
            panCached = true;
        }
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            panEnd[v] = panGainsAt(v, endUnits);
        }
    }

    /// Volume per sample over a chunk: the knob plus its sum, clamped, the
    /// law linear, signed by Inv (unsigned for Noise, whose mix holds Inv).
    void volumeChunk(const SlotModulation& m, std::size_t start, std::size_t length, bool sign,
                     std::array<float, chunkSize>& out) noexcept
    {
        for (std::size_t i = 0; i < length; ++i)
        {
            float knob = volume * 10.0f;
            if (knobSource == KnobSource::slotRamps)
            {
                knob = volumeKnob.next();
            }
            else if (knobSource == KnobSource::globalRamps)
            {
                knob = globalVolumeAt(m, start + i);
            }
            const float sum = m.volume.empty() ? 0.0f : m.volume[start + i];
            const float units = std::clamp(knob + sum, 0.0f, 10.0f);
            out[i] = (sign && inverted ? -units : units) / 10.0f;
        }
    }

    /// The slot's own knob ramps and the voices' presence, advanced where
    /// nothing read them.
    void advanceRamps(std::size_t numSamples, bool volumeRead) noexcept
    {
        if (!volumeRead)
        {
            volumeKnob.skip(numSamples);
        }
        panKnob.skip(numSamples);
    }

    void renderModulated(const AudioBlock& out, std::size_t numSamples, std::span<const float> notes,
                         const SlotModulation& m) noexcept
    {
        if (noiseSelected)
        {
            renderNoiseModulated(out, numSamples, m);
            running = running || numSamples > 0;
            return;
        }
        const bool phaseMoves = !m.phase.empty();
        const bool gainsModulated = m.gainsModulated();
        if (gainsModulated)
        {
            panEnds(m);
        }
        else
        {
            panCached = false;
        }
        std::array<float, chunkSize> mono;               // NOLINT(cppcoreguidelines-pro-type-member-init)
        std::array<float, chunkSize> centre;             // NOLINT(cppcoreguidelines-pro-type-member-init)
        std::array<float, chunkSize> gain;               // NOLINT(cppcoreguidelines-pro-type-member-init)
        std::array<std::int64_t, chunkSize> phaseChange; // NOLINT(cppcoreguidelines-pro-type-member-init)
        const auto segment = static_cast<float>(m.segment);
        for (std::size_t start = 0; start < numSamples; start += chunkSize)
        {
            const auto length = std::min(chunkSize, numSamples - start);
            if (!notes.empty())
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    centre[i] = frequencyFor(notes[start + i]);
                }
            }
            if (phaseMoves)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    const std::uint32_t target = phaseOf(m.phase[start + i]);
                    phaseChange[i] = static_cast<std::int32_t>(target - appliedPhase);
                    appliedPhase = target;
                }
            }
            if (gainsModulated)
            {
                volumeChunk(m, start, length, true, gain);
            }
            advanceRamps(length, gainsModulated && knobSource == KnobSource::slotRamps);
            for (std::size_t v = 0; v < maxUnisonVoices; ++v)
            {
                if (states[v] == VoiceState::fading && !gains[v].isSmoothing())
                {
                    states[v] = VoiceState::off;
                    oscillators[v].reset();
                }
                if (states[v] == VoiceState::off)
                {
                    presence[v].skip(length);
                    continue;
                }
                auto& oscillator = oscillators[v];
                const float ratio = ratios[v];
                for (std::size_t i = 0; i < length; ++i)
                {
                    const std::uint32_t base =
                        notes.empty() ? baseIncrements[v] : incrementFor(centre[i] * ratio, sampleRate);
                    if (phaseMoves)
                    {
                        const std::int64_t step = static_cast<std::int64_t>(base) + phaseChange[i];
                        if (step >= 0 && std::cmp_less(step, halfCycle))
                        {
                            oscillator.setIncrement(static_cast<std::uint32_t>(step));
                        }
                        else
                        {
                            oscillator.setIncrement(base);
                            oscillator.jumpTo(oscillator.nextPhase() + static_cast<std::uint32_t>(phaseChange[i]));
                            ++phaseJumpCount;
                        }
                    }
                    else if (!notes.empty())
                    {
                        oscillator.setIncrement(base);
                    }
                    mono[i] = oscillator.next();
                }
                const auto piece = out.subBlock(start, length);
                if (!gainsModulated)
                {
                    gains[v].process(std::span{mono.data(), length}, piece);
                    presence[v].skip(length);
                    continue;
                }
                float* const left = piece.channel(0).data();
                float* const right = piece.channel(1).data();
                const auto a = panStart[v];
                const auto b = panEnd[v];
                for (std::size_t i = 0; i < length; ++i)
                {
                    const float t = static_cast<float>(m.position + start + i) / segment;
                    const float pl = a.left + ((b.left - a.left) * t);
                    const float pr = a.right + ((b.right - a.right) * t);
                    const float g = gain[i] * presence[v].next();
                    left[i] += (g * pl) * mono[i];
                    right[i] += (g * pr) * mono[i];
                }
                gains[v].skip(length);
            }
        }
        running = running || numSamples > 0;
    }

    /// Noise with its gains modulated: the mix at unit volume from the pan
    /// positions at the segment's grid points, interpolated, times the
    /// volume per sample. Two draws per sample while the mix sounds.
    void renderNoiseModulated(const AudioBlock& out, std::size_t numSamples, const SlotModulation& m) noexcept
    {
        const auto toEnd = m.segment > m.position ? m.segment - m.position : 0;
        const auto mixAt = [&](float units)
        {
            std::array<StereoGains, maxUnisonVoices> g{};
            for (std::size_t v = 0; v < count; ++v)
            {
                g[v] = panGainsAt(v, units);
            }
            return unisonNoiseMix(std::span{g.data(), count}, inverted);
        };
        if (m.position == 0 || !panCached)
        {
            noiseStart = mixAt(panKnobAfter(m, 0) + m.panBefore);
            panCached = true;
        }
        const auto end = mixAt(panKnobAfter(m, toEnd) + m.panNow);
        const auto& s = noiseStart;
        float* const left = out.channel(0).data();
        float* const right = out.channel(1).data();
        std::array<float, chunkSize> gain; // NOLINT(cppcoreguidelines-pro-type-member-init)
        const auto segment = static_cast<float>(m.segment);
        // Two draws per sample while a voice sounds (any voice's presence
        // above 0, checked per sample, so where a count of 0 stops the draws
        // doesn't depend on the caller's blocks). The Volume knob at 0 doesn't
        // stop them: a route can lift it.
        for (std::size_t start = 0; start < numSamples; start += chunkSize)
        {
            const auto length = std::min(chunkSize, numSamples - start);
            volumeChunk(m, start, length, false, gain);
            advanceRamps(length, knobSource == KnobSource::slotRamps);
            for (std::size_t i = 0; i < length; ++i)
            {
                float sounding = 0.0f;
                for (auto& p : presence)
                {
                    sounding = std::max(sounding, p.next());
                }
                if (!(sounding > 0.0f))
                {
                    continue;
                }
                const float t = static_cast<float>(m.position + start + i) / segment;
                const float a = s.left + ((end.left - s.left) * t);
                const float c = s.cross + ((end.cross - s.cross) * t);
                const float b = s.right + ((end.right - s.right) * t);
                const float n1 = noise.next();
                const float n2 = noise.next();
                left[start + i] += gain[i] * ((a * n1) + (c * n2));
                right[start + i] += gain[i] * ((c * n1) + (b * n2));
            }
        }
        for (auto& ramp : mixRamps)
        {
            ramp.skip(numSamples);
        }
        for (auto& g : gains)
        {
            g.skip(numSamples);
        }
    }

    /// The ramps a skip advances: the knobs and the voices' presence (the
    /// gain ramps can't be moving: canSkip()).
    void skipRamps(std::size_t numSamples) noexcept
    {
        advanceRamps(numSamples, false);
        for (auto& p : presence)
        {
            p.skip(numSamples);
        }
    }

    /// A skip of Noise on its modulated path: the draws rendering would make
    /// (canSkipUnder() holds the presence still, so a voice sounds
    /// throughout or not at all), and the ramps it advances.
    void skipNoiseModulated(std::size_t numSamples) noexcept
    {
        const bool sounding =
            std::ranges::any_of(presence, [](const LinearSmoother& p) { return p.currentValue() > 0.0f; });
        for (std::size_t i = 0; sounding && i < 2 * numSamples; ++i)
        {
            static_cast<void>(noise.next());
        }
        skipRamps(numSamples);
        panCached = false;
        skipped = skipped || numSamples > 0;
        running = running || numSamples > 0;
    }

    /// After skip(): Phase's change into the skipped steps.
    void skipModulation(std::size_t numSamples, const SlotModulation& m) noexcept
    {
        if (!m.phase.empty() && !noiseSelected && numSamples > 0)
        {
            const std::uint32_t target = phaseOf(m.phase[numSamples - 1]);
            const std::uint32_t change = target - appliedPhase;
            for (std::size_t v = 0; v < maxUnisonVoices; ++v)
            {
                if (states[v] != VoiceState::off)
                {
                    pending[v] += change;
                }
            }
            appliedPhase = target;
            skipped = skipped || change != 0;
        }
        if (!m.gainsModulated())
        {
            panCached = false;
        }
    }

    // Two noise draws per sample, mixed into the two channels. Once the mix
    // has settled at zero (0 voices), nothing is drawn: the sample at which
    // that happens does not depend on how the block is split.
    void renderNoise(const AudioBlock& out, std::size_t numSamples) noexcept
    {
        if (out.numChannels() < 2)
        {
            return;
        }
        float* const left = out.channel(0).data();
        float* const right = out.channel(1).data();
        const auto silent = [this] noexcept
        {
            return !mixRamps[0].isSmoothing() && !mixRamps[1].isSmoothing() && !mixRamps[2].isSmoothing() &&
                   isZero(mixRamps[0].targetValue()) && isZero(mixRamps[1].targetValue()) &&
                   isZero(mixRamps[2].targetValue());
        };
        if (!mixRamps[0].isSmoothing() && !mixRamps[1].isSmoothing() && !mixRamps[2].isSmoothing())
        {
            if (silent())
            {
                return;
            }
            const float a = mixRamps[0].currentValue();
            const float c = mixRamps[1].currentValue();
            const float b = mixRamps[2].currentValue();
            for (std::size_t i = 0; i < numSamples; ++i)
            {
                const float n1 = noise.next();
                const float n2 = noise.next();
                left[i] += (a * n1) + (c * n2);
                right[i] += (c * n1) + (b * n2);
            }
            return;
        }
        for (std::size_t i = 0; i < numSamples; ++i)
        {
            const float a = mixRamps[0].next();
            const float c = mixRamps[1].next();
            const float b = mixRamps[2].next();
            if (silent())
            {
                return;
            }
            const float n1 = noise.next();
            const float n2 = noise.next();
            left[i] += (a * n1) + (c * n2);
            right[i] += (c * n1) + (b * n2);
        }
    }

    [[nodiscard]] bool noiseSettledAtZero() const noexcept
    {
        return !mixRamps[0].isSmoothing() && !mixRamps[1].isSmoothing() && !mixRamps[2].isSmoothing() &&
               isZero(mixRamps[0].targetValue()) && isZero(mixRamps[1].targetValue()) &&
               isZero(mixRamps[2].targetValue());
    }

    /// As renderVoices() drops a voice that has faded out; true if it is off.
    bool dropIfFaded(std::size_t v) noexcept
    {
        if (states[v] == VoiceState::fading && !gains[v].isSmoothing())
        {
            states[v] = VoiceState::off;
            oscillators[v].reset();
            pending[v] = 0;
        }
        return states[v] == VoiceState::off;
    }

    /// The noise's draws for a skip, as renderNoise() would make them (none
    /// once the mix has settled at zero).
    void skipNoise(std::size_t numSamples) noexcept
    {
        if (!noiseSelected || noiseSettledAtZero())
        {
            return;
        }
        for (std::size_t i = 0; i < 2 * numSamples; ++i)
        {
            static_cast<void>(noise.next());
        }
    }

    /// After a skip: each oscillator jumps to the phase it would have had.
    void resume() noexcept
    {
        if (!skipped)
        {
            return;
        }
        skipped = false;
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            if (states[v] != VoiceState::off && pending[v] != 0)
            {
                oscillators[v].jumpTo(oscillators[v].nextPhase() + pending[v]);
            }
            pending[v] = 0;
        }
    }

    [[nodiscard]] static bool isZero(float x) noexcept
    {
        return x >= 0.0f && x <= 0.0f; // == 0 without -Wfloat-equal
    }

    std::array<MinBlepOscillator<Taps>, maxUnisonVoices> oscillators;
    std::array<SkippableGain, maxUnisonVoices> gains{};
    std::array<float, maxUnisonVoices> ratios{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    std::array<VoiceState, maxUnisonVoices> states{VoiceState::on};
    std::array<LinearSmoother, 3> mixRamps{}; // the noise mix: left, cross, right
    WhiteNoise noise;
    Random random;
    Tuning tuning;
    std::size_t count = 1;
    float referenceHz = concertPitchHz;
    float spread = 0.0f;
    float stereoWidth = 0.0f;
    float pan = 0.0f;
    float volume = 1.0f;
    std::uint32_t startPhase = 0;
    bool inverted = false;
    bool retrigger = true;
    bool noiseSelected = false;
    bool noteHeld = false;                                // a note-on since the last stop()
    bool running = false;                                 // output rendered since the last stop()
    std::array<std::uint32_t, maxUnisonVoices> pending{}; // phase steps skipped, not yet jumped to
    bool skipped = false;                                 // skip() since the last jump
    float sampleRate = 48000.0f;
    std::array<std::uint32_t, maxUnisonVoices> baseIncrements{}; // each voice's step at the call's note, unmodulated
    std::uint32_t appliedPhase = 0;                              // the phase offset in force
    std::uint64_t phaseJumpCount = 0;
    // Task 4's modulated path.
    KnobSource knobSource = KnobSource::slotRamps;
    NoiseModulation noiseModulation = NoiseModulation::mixed;
    LinearSmoother volumeKnob;                              // Volume in knob units, ramped (KnobSource::slotRamps)
    LinearSmoother panKnob;                                 // Pan in knob units, 5 the centre
    std::array<LinearSmoother, maxUnisonVoices> presence{}; // each voice faded in (1) or out (0)
    std::array<StereoGains, maxUnisonVoices> panStart{};    // each voice's pan gains at the segment's first grid point
    std::array<StereoGains, maxUnisonVoices> panEnd{};      // ... and at its last, for this call
    dsp::NoiseMix noiseStart{};                             // the noise mix at the first grid point
    bool panCached = false;                                 // panStart (noiseStart) is this segment's
};

} // namespace anoesis::test::destinations
