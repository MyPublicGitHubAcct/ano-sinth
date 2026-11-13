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

} // namespace anoesis::dsp
