// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/control/stereo_gain.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/generators/white_noise.hpp>
#include <anoesis/dsp/math/pan.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/oscillators/unison.hpp>
#include <anoesis/dsp/oscillators/waveform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace anoesis::dsp
{

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
/// Satisfies Preparable. No allocation: fixed capacity for 8 voices, the
/// table is shared, and the mono signal goes through a small buffer on the
/// stack. About 5 KB at 64 taps.
///
/// Docs: docs/dsp/oscillator-controls.md, docs/dsp/oscillator-phase.md,
/// docs/dsp/oscillator-unison.md
template <std::size_t Taps>
class OscillatorSlot
{
public:
    /// The noise source's seed is the slot's seed XOR this (the fractional
    /// part of sqrt(2)), so it is not the phase sequence.
    static constexpr std::uint64_t noiseSeedSalt = 0x6A09E667F3BCC909U;

    /// The table must outlive the slot (see MinBlepOscillator).
    explicit OscillatorSlot(const MinBlepTable& residuals) noexcept
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
        noise.setSeed(noiseSeedSalt);
        updateNoiseMix();
    }

    void prepare(const ProcessSpec& spec) noexcept
    {
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
        }
        for (auto& ramp : mixRamps)
        {
            ramp.reset();
        }
        noteHeld = false;
        running = false;
    }

    /// A note-on, between two process() calls (a voice splits its block at
    /// the event, so the note-on is sample-accurate). For every unison voice:
    ///
    /// - Retrig on: the oscillator restarts at Phase. From silence, that is
    ///   the band-limited onset at Phase; while it sounds (a stolen voice, or
    ///   a retriggered mono note), a band-limited jump to Phase.
    /// - Retrig off: a sounding oscillator runs on untouched. From silence,
    ///   it starts at a random phase from this slot's Random, drawn in voice
    ///   order (a working hypothesis, inventory O10: a free-running
    ///   oscillator has been running unheard, so its phase is arbitrary).
    void noteOn() noexcept
    {
        noteHeld = true;
        for (std::size_t v = 0; v < count; ++v)
        {
            startVoice(v);
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
            }
            else if (v >= count && states[v] == VoiceState::on)
            {
                gains[v].setGain(0.0f);
                states[v] = running ? VoiceState::fading : VoiceState::off;
                if (!running)
                {
                    oscillators[v].reset();
                    gains[v].reset();
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
        const auto numSamples = std::min(out.numSamples(), notes.size());
        render(out, numSamples, notes.first(numSamples));
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

    void startVoice(std::size_t v) noexcept
    {
        if (retrigger)
        {
            oscillators[v].jumpTo(startPhase);
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

    [[nodiscard]] static bool isZero(float x) noexcept
    {
        return x >= 0.0f && x <= 0.0f; // == 0 without -Wfloat-equal
    }

    std::array<MinBlepOscillator<Taps>, maxUnisonVoices> oscillators;
    std::array<StereoGain, maxUnisonVoices> gains{};
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
    bool noteHeld = false; // a note-on since the last stop()
    bool running = false;  // output rendered since the last stop()
};

} // namespace anoesis::dsp
