// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/stereo_gain.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/oscillators/waveform.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace anoesis::dsp
{

/// One of a voice's oscillators, with its controls: tuning (octave, note,
/// fine), the master tune, volume, pan, invert, Phase, and Retrig. It renders a mono
/// band-limited oscillator and adds it, panned, into the voice's stereo bus
/// (pan comes before the filter, so the bus is stereo).
///
/// The controls live here rather than in MinBlepOscillator, which stays a
/// mono, single-voice source: unison (a later M1.1 task) turns this slot into
/// N detuned oscillators plus a WhiteNoise for the Noise waveform, each voice
/// with its own stereo gains, and applying volume and pan per oscillator
/// would repeat that work in every unison voice.
///
/// Pitch is one fractional note: the caller's note (key, bend, glide, pitch
/// modulation), plus tuning.offset(), converted once by noteToFrequency()
/// at the master tune. process(out, note) converts once per block;
/// process(out, notes) converts every sample, exactly.
///
/// Volume, pan, and invert are smoothed by a StereoGain (10 ms linear ramps).
/// Pitch is not smoothed: the oscillator's phase is continuous, so a pitch
/// step makes no discontinuity in the waveform.
///
/// Phase and Retrig act at noteOn(): a retriggered note starts at Phase, as
/// an onset from silence or as a band-limited jump while sounding; a
/// free-running one is left alone. Phase is degrees here and fixed point in
/// the oscillator.
///
/// Satisfies Preparable. No allocation: the table is shared, and the mono
/// signal goes through a small buffer on the stack.
///
/// Docs: docs/dsp/oscillator-controls.md, docs/dsp/oscillator-phase.md
template <std::size_t Taps>
class OscillatorSlot
{
public:
    /// The table must outlive the slot (see MinBlepOscillator).
    explicit OscillatorSlot(const MinBlepTable& residuals) noexcept : oscillator(residuals) {}

    void prepare(const ProcessSpec& spec) noexcept
    {
        oscillator.prepare(spec);
        gain.prepare(spec);
        random.reset();
    }

    /// stop(), and restarts the random sequence of free-running start phases
    /// from its seed: the slot is as freshly prepared.
    void reset() noexcept
    {
        stop();
        random.reset();
    }

    /// For a voice that has finished: silences the oscillator, so the next
    /// noteOn() starts it from silence, and finishes any gain ramp. Unlike
    /// reset(), the random sequence carries on, so each new free-running note
    /// draws a new start phase. Without a noteOn(), process() starts the
    /// oscillator at phase 0.
    void stop() noexcept
    {
        oscillator.reset();
        gain.reset();
    }

    /// A note-on, between two process() calls (a voice splits its block at
    /// the event, so the note-on is sample-accurate).
    ///
    /// - Retrig on: the oscillator restarts at Phase. From silence, that is
    ///   the band-limited onset at Phase; while it sounds (a stolen voice, or
    ///   a retriggered mono note), a band-limited jump to Phase.
    /// - Retrig off: a sounding oscillator runs on untouched. From silence,
    ///   it starts at a random phase from this slot's Random (a working
    ///   hypothesis, inventory O10: a free-running oscillator has been
    ///   running unheard, so its phase is arbitrary).
    void noteOn() noexcept
    {
        if (retrigger)
        {
            oscillator.jumpTo(startPhase);
        }
        else if (!oscillator.isRunning())
        {
            oscillator.restart(random.nextUint32());
        }
    }

    /// The start phase for a retriggered note-on, in degrees (any value,
    /// wrapped to one cycle). It takes effect at the next note-on, not on a
    /// held note.
    void setPhaseDegrees(float degrees) noexcept
    {
        startPhase = phaseFromDegrees(degrees);
    }

    /// Retrig: whether a note-on restarts the oscillator at Phase (on, the
    /// default) or leaves it free-running (off).
    void setRetrigger(bool shouldRetrigger) noexcept
    {
        retrigger = shouldRetrigger;
    }

    /// The seed of the free-running start phases. Give each slot its own, or
    /// they start in step. Restarts the sequence.
    void setRandomSeed(std::uint64_t seed) noexcept
    {
        random.setSeed(seed);
    }

    void setWaveform(Waveform waveform) noexcept
    {
        oscillator.setWaveform(waveform);
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

    /// Linear gain; Sylenth1's Volume knob maps to it linearly (inventory O4).
    void setVolume(float linearGain) noexcept
    {
        gain.setGain(linearGain);
    }

    /// -1 (hard left) to +1 (hard right), equal-power (inventory O6).
    void setPan(float position) noexcept
    {
        gain.setPan(position);
    }

    void setInverted(bool shouldInvert) noexcept
    {
        gain.setInverted(shouldInvert);
    }

    /// The frequency this slot plays for `note`: note + tuning at the master
    /// tune.
    [[nodiscard]] float frequencyFor(float note) const noexcept
    {
        return noteToFrequency(note + tuning.offset(), referenceHz);
    }

    /// The oscillator's current phase step (2^32 per cycle): the pitch it is
    /// actually playing, for measurement.
    [[nodiscard]] std::uint32_t phaseIncrement() const noexcept
    {
        return oscillator.phaseIncrement();
    }

    [[nodiscard]] float latencySamples() const noexcept
    {
        return oscillator.latencySamples();
    }

    /// Adds this slot's output at `note` (constant for the block) into
    /// channels 0 and 1 of `out`.
    void process(const AudioBlock& out, float note) noexcept
    {
        oscillator.setFrequency(frequencyFor(note));
        render(out, out.numSamples(), [this](std::size_t) noexcept { return oscillator.next(); });
    }

    /// As above, with a note per sample (`notes[i]` for sample i), for the
    /// shorter of `notes` and the block.
    void process(const AudioBlock& out, std::span<const float> notes) noexcept
    {
        render(out, std::min(out.numSamples(), notes.size()),
               [this, notes](std::size_t i) noexcept
               {
                   oscillator.setFrequency(frequencyFor(notes[i]));
                   return oscillator.next();
               });
    }

private:
    // The mono signal is staged in chunks of this many samples on the
    // stack, then panned into the bus.
    static constexpr std::size_t chunkSize = 64;

    template <typename NextSample>
    void render(const AudioBlock& out, std::size_t numSamples, NextSample nextSample) noexcept
    {
        std::array<float, chunkSize> mono; // NOLINT(cppcoreguidelines-pro-type-member-init): written before read
        for (std::size_t start = 0; start < numSamples; start += chunkSize)
        {
            const auto length = std::min(chunkSize, numSamples - start);
            for (std::size_t i = 0; i < length; ++i)
            {
                mono[i] = nextSample(start + i);
            }
            gain.process(std::span{mono.data(), length}, out.subBlock(start, length));
        }
    }

    MinBlepOscillator<Taps> oscillator;
    StereoGain gain;
    Random random;
    Tuning tuning;
    float referenceHz = concertPitchHz;
    std::uint32_t startPhase = 0;
    bool retrigger = true;
};

} // namespace anoesis::dsp
