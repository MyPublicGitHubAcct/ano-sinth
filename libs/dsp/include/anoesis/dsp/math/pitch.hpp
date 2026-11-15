// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <cmath>

namespace anoesis::dsp
{

/// Concert pitch: A4 = 440 Hz (ISO 16).
inline constexpr float concertPitchHz = 440.0f;

/// The MIDI note number of A4.
inline constexpr float referenceNote = 69.0f;

/// Frequency in Hz of a (possibly fractional) MIDI note number in 12-tone
/// equal temperament: f = reference * 2^((note - 69) / 12).
///
/// `referenceHz` is the master tune: the frequency of A4. 440 is concert
/// pitch; 415 is baroque pitch; 432 and 444 are other common choices. A master
/// tune control in cents converts with referenceFromCents().
[[nodiscard]] inline float noteToFrequency(float note, float referenceHz = concertPitchHz) noexcept
{
    return referenceHz * std::exp2((note - referenceNote) / 12.0f);
}

/// The (fractional) MIDI note number of a frequency. `frequencyHz` must be
/// positive.
[[nodiscard]] inline float frequencyToNote(float frequencyHz, float referenceHz = concertPitchHz) noexcept
{
    return referenceNote + (12.0f * std::log2(frequencyHz / referenceHz));
}

/// Frequency ratio of an interval in cents (100 cents = 1 semitone).
[[nodiscard]] inline float centsToRatio(float cents) noexcept
{
    return std::exp2(cents / 1200.0f);
}

/// The A4 reference for a master tune offset in cents from concert pitch.
[[nodiscard]] inline float referenceFromCents(float cents) noexcept
{
    return concertPitchHz * centsToRatio(cents);
}

/// The master tune as an offset in semitones from concert pitch:
/// 12 log2(referenceHz / 440), so noteToFrequency(n + offset) equals
/// noteToFrequency(n, referenceHz). For anything that works in notes rather
/// than Hz, such as a keytracked filter (filters/keytrack.hpp). Computed once
/// per block, not per sample. `referenceHz` must be positive.
[[nodiscard]] inline float masterTuneSemitones(float referenceHz) noexcept
{
    return 12.0f * std::log2(referenceHz / concertPitchHz);
}

/// An oscillator's tuning: coarse steps of octaves and semitones, plus a fine
/// offset in (fractional) semitones. A pitch is one fractional note,
/// `note + offset()`, converted once by noteToFrequency(), so every pitch
/// source (key, bend, glide, modulation, detune) adds in the same unit.
///
/// No ranges are enforced here. Sylenth1's controls are octaves -3..3,
/// semitones -7..7, and fine -1..1 (inventory §2, O1-O3); the parameter
/// table maps the knobs.
struct Tuning
{
    int octaves = 0;
    int semitones = 0;
    float fine = 0.0f; ///< semitones; 0.01 is one cent

    /// The offset in semitones: 12 x octaves + semitones + fine. The coarse
    /// part is summed as integers, so the only rounding is adding `fine`.
    [[nodiscard]] constexpr float offset() const noexcept
    {
        return static_cast<float>((12 * octaves) + semitones) + fine;
    }
};

} // namespace anoesis::dsp
