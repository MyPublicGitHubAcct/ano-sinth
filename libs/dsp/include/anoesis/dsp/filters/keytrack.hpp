// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

/// Filter keytracking: Sylenth1's Keytrack (inventory §3, F13, F17-F22),
/// chosen from measured candidates (docs/dsp/keytracking.md):
///
/// - Linear in octaves: the cutoff moves `amount` octaves per octave of
///   pitch, amount = Keytrack / 10, so 10 tracks 1:1 and 0 not at all. The
///   offset goes into LadderFilter::setCutoffModulation() (with the other
///   cutoff terms, by the Filter Control's combining law).
/// - The reference is A-1 (MIDI 9), where the developer's keytrack source is
///   0: there keytrack changes nothing, at any amount.
/// - The pitch is the voice's sounding pitch, as one fractional note: the key
///   (or the arpeggiator's note), plus the portamento glide and the pitch
///   bend in semitones, plus masterTuneSemitones() (math/pitch.hpp), so a
///   self-oscillating filter at keytrack 10 stays with the oscillators. Not
///   drift, and not an oscillator's own Octave, Note, or Fine.
/// - The voice computes it once per voice, feeds both filters, and does so
///   at each of the filter's 32-sample grid points with the pitch at the end
///   of the coming segment (the filter reaches a modulation one grid point
///   after reading it). Then it follows a glide to 3 cents and the output is
///   independent of the block size.
/// - In tune in its intervals, not absolutely: a saturating ladder goes flat
///   as it self-oscillates, by an amount set by k and the same at every
///   cutoff, and the player tunes it with the cutoff (no correction, F7).
/// - keytrackSource() is the misc modulation source (M1.5): the same
///   quantity, 0 at A-1, 0.5 at A4, 1 at A9 (MIDI 129), linear, unclamped.
///
/// Pure constexpr functions: no state, no allocation, real-time safe. NaN in
/// gives NaN out, which LadderFilter treats as no modulation.
/// Doc: docs/dsp/keytracking.md
namespace anoesis::dsp
{

/// A-1, MIDI 9 (13.75 Hz at A4 = 440 Hz): the note where keytrack changes
/// nothing, and where the keytrack source is 0.
inline constexpr float keytrackReferenceNote = 9.0f;

/// The cutoff offset in octaves for a voice at `pitch` (a fractional note),
/// with `amount` = Keytrack / 10: amount x (pitch - reference) / 12.
[[nodiscard]] constexpr float keytrackOctaves(float pitch, float amount,
                                              float referenceNote = keytrackReferenceNote) noexcept
{
    return amount * ((pitch - referenceNote) / 12.0f);
}

/// The keytrack modulation source for a voice at `pitch`: (pitch - 9) / 120,
/// 0 at A-1, 0.5 at A4, 1 at A9, negative below A-1. keytrackOctaves(p, 1)
/// is 10 x keytrackSource(p).
[[nodiscard]] constexpr float keytrackSource(float pitch) noexcept
{
    return (pitch - keytrackReferenceNote) / 120.0f;
}

} // namespace anoesis::dsp
