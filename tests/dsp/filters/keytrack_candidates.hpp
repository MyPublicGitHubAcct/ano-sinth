// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Keytracking candidates (M1.3 task 5, docs/dsp/keytracking.md): test-only
// prototypes, measured by the hidden [keytrack-report] and the [keytrack]
// benchmarks. Each model turns a voice's pitch and the Keytrack amount into
// an offset in octaves for LadderFilter::setCutoffModulation():
//
// - (a) note number: amount x (key - reference) / 12, from the MIDI key only;
// - (b) sounding pitch: the same from the voice's pitch (key, portamento
//   glide, pitch bend) with the reference moved by the master tune;
// - (c) linear in Hz: the cutoff plus amount x the note's frequency, as an
//   octave offset log2(1 + amount f(note) / cutoff), for contrast;
// - (d) sounding pitch plus a resonance-dependent cutoff scale c(k): the
//   octaves that cancel the self-oscillation detune measured at k (a table,
//   calibrated by the report), so keytrack 10 plays the note's pitch;
// - (e) note number with each note step ramped over 10 ms: the offset goes
//   into the cutoff's own control ramp (setCutoff) instead of the modulation
//   input.
//
// The reference note, the amount, and the master tune are inputs, so the
// open questions (inventory F13, F17-F21) change a value, not a model. How
// often the voice feeds the value (once per host block, or at each of the
// filter's 32-sample grid points, at the segment's start or its end) is the
// report's other axis.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace anoesis::test::keytrack
{

/// A-1, MIDI 9: where the developer's keytrack source is 0 (inventory F17).
inline constexpr double referenceAMinus1 = 9.0;
/// A4, MIDI 69: the developer's "hinge point" and the source's +5.
inline constexpr double referenceA4 = 69.0;

/// What a voice knows about its pitch at one moment.
struct VoicePitch
{
    double key = 69.0;           ///< the MIDI key (the arpeggiator's note when it plays)
    double glide = 0.0;          ///< portamento: the glided note minus the key, in semitones
    double bend = 0.0;           ///< pitch bend in semitones (wheel x bend range)
    double masterTuneHz = 440.0; ///< the A4 reference

    /// The pitch the oscillators play before their own offsets, in notes at
    /// A4 = 440 Hz (the master tune folded in as a note offset).
    [[nodiscard]] double soundingNote() const noexcept
    {
        return key + glide + bend + (12.0 * std::log2(masterTuneHz / 440.0));
    }
};

/// (a) The key alone.
[[nodiscard]] inline float noteNumberOctaves(const VoicePitch& p, double amount, double reference) noexcept
{
    return static_cast<float>(amount * (p.key - reference) / 12.0);
}

/// (b) The sounding pitch.
[[nodiscard]] inline float soundingPitchOctaves(const VoicePitch& p, double amount, double reference) noexcept
{
    return static_cast<float>(amount * (p.soundingNote() - reference) / 12.0);
}

/// (c) Linear in Hz: fc = cutoff + amount x f(note), as octaves on `cutoffHz`.
[[nodiscard]] inline float linearHzOctaves(const VoicePitch& p, double amount, double cutoffHz) noexcept
{
    const double noteHz = p.masterTuneHz * std::exp2((p.key + p.glide + p.bend - 69.0) / 12.0);
    return static_cast<float>(std::log2(1.0 + (amount * noteHz / cutoffHz)));
}

/// (d)'s correction: the self-oscillation detune against k, in cents,
/// interpolated linearly; correction() is the octaves that cancel it.
template <std::size_t N>
struct DetuneTable
{
    std::array<float, N> k{};
    std::array<float, N> cents{};

    [[nodiscard]] float correction(float loopGain) const noexcept
    {
        if (loopGain <= k.front())
        {
            return -cents.front() / 1200.0f;
        }
        if (loopGain >= k.back())
        {
            return -cents.back() / 1200.0f;
        }
        const auto upper = static_cast<std::size_t>(std::ranges::upper_bound(k, loopGain) - k.begin());
        const float t = (loopGain - k[upper - 1]) / (k[upper] - k[upper - 1]);
        return -(cents[upper - 1] + (t * (cents[upper] - cents[upper - 1]))) / 1200.0f;
    }
};

/// (d) The sounding pitch plus the correction at k.
template <std::size_t N>
[[nodiscard]] float correctedOctaves(const VoicePitch& p, double amount, double reference, const DetuneTable<N>& table,
                                     float loopGain) noexcept
{
    return soundingPitchOctaves(p, amount, reference) + table.correction(loopGain);
}

/// A portamento glide: constant time, linear in semitones, from `from` to
/// the key over `samples`, starting at `start`. glideAt() is VoicePitch::glide.
struct Glide
{
    double from = 0.0;
    double to = 0.0;
    std::size_t start = 0;
    std::size_t samples = 1;

    [[nodiscard]] double glideAt(std::size_t n) const noexcept
    {
        if (n < start)
        {
            return 0.0;
        }
        const double t = std::min(1.0, static_cast<double>(n - start) / static_cast<double>(samples));
        return (from - to) * (1.0 - t);
    }
};

} // namespace anoesis::test::keytrack
