// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/modulation/controller_ramp.hpp>
#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/modulation/route_compiler.hpp>
#include <anoesis/dsp/modulation/source_bound.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

/// What a voice's modulation sources read that isn't the voice's own (M1.5
/// task 3, decided 2026-11-22; docs/dsp/modulation-sources.md): the engine's
/// state, passed to every voice through VoiceKnobs::sources.
///
/// - The patch: the 16 slots and their version (route_compiler.hpp).
/// - The two mod envelopes' settings (seconds and a sustain level, as the
///   amp envelopes take them) and the two LFOs' (their waveform and their
///   Rate, Gain and Offset knobs, and Free). The knob laws are M1.9's; the
///   LFO Rate's is a placeholder here (lfoRateHz()), since the voice adds
///   LFO Rate's modulation in knob units before it.
/// - The mod wheel and channel pressure (ControllerRamp), read at absolute
///   samples.
/// - The free LFOs' global instances. The engine moves them on with each
///   piece it renders, so they stand where the knob ramps stand: `offset`
///   samples before the first sample of the block the voice is given. A
///   voice that copies one moves its copy on by its own offset into the
///   call.
/// - Step Velocity: a stand-in input (0-1) until M1.6's step sequencer, set
///   by the engine at its events' samples, constant between them.
///
/// A null context, or a null patch, is the init patch: no routes. A null
/// controller reads 0; null free LFOs leave a free LFO where it stands.
///
/// SourceObserver is for measurement and tests only: a voice given one
/// reports each chunk's source values and the bounds it took.
/// Doc: docs/dsp/modulation-sources.md
namespace anoesis::dsp::modulation
{

/// The LFO Rate knob's law (inventory L6; a placeholder until M1.9 measures
/// it, stage 1's): 0.1 Hz x 1000^(x / 10), x the knob plus its modulation
/// held to [0, 10]. NaN gives NaN, which Lfo::setFrequency() ignores.
[[nodiscard]] inline float lfoRateHz(float units) noexcept
{
    const float x = std::clamp(units, 0.0f, 10.0f);
    return 0.1f * std::exp2(x * (9.965784285f / 10.0f));
}

/// A mod envelope's settings, as an Adsr takes them (adsr.hpp): times in
/// seconds, the sustain a level 0-1. The knob laws are the amp envelopes'
/// (M1.9).
struct ModEnvelopeSettings
{
    float attack = 0.0f;
    float decay = 0.0f;
    float sustain = 1.0f;
    float release = 0.0f;
};

/// An LFO's settings: its waveform, its knobs, and Free. Rate is the knob,
/// 0-10 (lfoRateHz()); Gain 0-10; Offset -10 to +10. A free LFO is copied
/// from its global instance at a note-on from silence.
struct LfoSettings
{
    LfoWaveform waveform = LfoWaveform::sine;
    float rate = 5.0f;
    float gain = 10.0f;
    float offset = 0.0f;
    bool free = false;
};

inline constexpr std::size_t modEnvelopeCount = 2;
inline constexpr std::size_t lfoCount = 2;

/// The engine's modulation state, as a voice reads it.
struct SourceContext
{
    const Patch* patch = nullptr; ///< null: no routes
    std::array<ModEnvelopeSettings, modEnvelopeCount> modEnvelopes{};
    std::array<LfoSettings, lfoCount> lfos{};
    const ControllerRamp* modWheel = nullptr;   ///< CC 1; null reads 0
    const ControllerRamp* aftertouch = nullptr; ///< channel pressure; null reads 0
    /// The free LFOs' global instances, standing `offset` samples before the
    /// voice's block (VoiceKnobs::offset).
    const std::array<Lfo, lfoCount>* freeLfos = nullptr;
    float stepVelocity = 0.0f; ///< Step Velocity's stand-in, 0-1 (M1.6)
};

/// The most samples one chunk of a voice holds (its 32-sample grid).
inline constexpr std::size_t observedChunkSamples = 32;

/// For measurement: what a voice's sources did over one chunk.
struct SourceObservation
{
    std::size_t length = 0; ///< the chunk's samples (at most observedChunkSamples)
    std::uint64_t time = 0; ///< the absolute sample of its first
    /// The bounds were taken at this chunk's start: a grid point, or the
    /// start of a process() call (where the skip test may test again).
    bool rebound = false;
    /// Each source's bound from where it was taken to the segment's end, as
    /// mixModulationBound() takes it.
    SourceBounds bounds{};
    /// Each source's value at each sample (0 for a source that doesn't run).
    std::array<std::array<float, observedChunkSamples>, sourceCount> values{};
};

/// For measurement and tests: told about every chunk a voice renders.
class SourceObserver
{
public:
    virtual void observe(const SourceObservation& chunk) noexcept = 0;

protected:
    SourceObserver() = default;
    ~SourceObserver() = default;
    SourceObserver(const SourceObserver&) = default;
    SourceObserver& operator=(const SourceObserver&) = default;
    SourceObserver(SourceObserver&&) = default;
    SourceObserver& operator=(SourceObserver&&) = default;
};

} // namespace anoesis::dsp::modulation
