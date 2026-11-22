// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/voice/pitch_bend.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

/// What a voice reads that isn't its own: the global knob ramps that feed its
/// filters and set its parts' Mix, and the pitch bend; and how a note-on
/// moves its pitch.
/// Doc: docs/dsp/voice-architecture.md, docs/dsp/legato-portamento.md,
/// docs/dsp/mixer.md, docs/dsp/part-solo-and-skipping.md
namespace anoesis::dsp
{

/// The global knobs a voice reads (M1.4 tasks 1-3, until task 3 named
/// VoiceFilterKnobs):
///
/// - the parameter layer's ramps that feed the filters through the Filter
///   Control (filter_control.hpp), each positioned `offset` samples before
///   the first sample of the block the voice processes next (or of its
///   note-on), read ahead with LinearSmoother::valueAfter(); a null ramp
///   reads as 0;
/// - the pitch bend (pitch_bend.hpp), read at absolute samples: `time` is
///   the absolute sample of that same first sample. A null bend reads as 0;
/// - Mix A and Mix B (M1.4 task 4, mix_law.hpp), ramps in internal units
///   (0-15) read at each sample like the filter knobs. A null Mix ramp reads
///   as unity (internal 10), so a caller without a mixer hears the voice
///   unscaled;
/// - Solo's two gains (M1.4 task 5, part_solo.hpp), 0-1, read like Mix and
///   multiplied into its gain. A null one reads as 1, so a caller without
///   Solo hears the voice as before, to the bit.
struct VoiceKnobs
{
    std::array<const LinearSmoother*, 2> cutoff{};    ///< Cutoff A, Cutoff B (knob units)
    std::array<const LinearSmoother*, 2> resonance{}; ///< Resonance A, Resonance B
    const LinearSmoother* controlCutoff = nullptr;    ///< the Filter Control's Cutoff
    const LinearSmoother* controlResonance = nullptr; ///< the Filter Control's Resonance
    float keytrack = 0.0f;                            ///< the Keytrack knob, 0-10
    /// Samples from where the ramps stand to the first sample of the block
    /// the voice is given: 0 when the voice gets the whole host block, and
    /// the piece's start when an allocator renders it in pieces.
    std::size_t offset = 0;
    const PitchBend* bend = nullptr;            ///< the instance's bend; null: none
    std::uint64_t time = 0;                     ///< the absolute sample of the block's first sample
    std::array<const LinearSmoother*, 2> mix{}; ///< Mix A, Mix B (internal units); null: unity
    /// Solo's gains for parts A and B (0-1; PartSolo, part_solo.hpp), read
    /// like Mix and multiplied into its gain. Null reads as 1: no Solo.
    std::array<const LinearSmoother*, 2> solo{};

    /// The same knobs for a block starting `samples` later.
    [[nodiscard]] constexpr VoiceKnobs advanced(std::size_t samples) const noexcept
    {
        VoiceKnobs k = *this;
        k.offset += samples;
        k.time += samples;
        return k;
    }
};

/// How a note-on moves a voice's pitch, and its velocity (docs/dsp/
/// legato-portamento.md): a jump to the note, or a glide from `from` over
/// `glideSeconds` (linear in semitones); the velocity (0-1) taken at once,
/// or, while it glides, ramped from `fromVelocity` over the same time.
struct NoteStart
{
    float velocity = 1.0f;
    bool glide = false;
    float from = 0.0f;         ///< the pitch the glide starts from (a fractional note)
    float glideSeconds = 0.0f; ///< Portamento's time; 0 is a jump
    float fromVelocity = 1.0f; ///< where a gliding note-on's velocity ramps from
};

} // namespace anoesis::dsp
