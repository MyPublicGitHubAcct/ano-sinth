// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

/// An oscillator's Volume law (inventory O4, M8, M9; decided in M1.5 task 4,
/// docs/dsp/modulation-destinations.md):
///
/// - Volume works in knob units, 0 to 10. Modulation adds in those units.
/// - The knob plus its modulation is clamped to [0, 10] before the law
///   (F24): a negative sum is silent, as the developer says of v3 ("in v3 it
///   will mute negative volume"), not inverted as v2 did.
/// - The law is linear: gain = units / 10, so 10 is unity (O4's working
///   hypothesis, a placeholder until the demo is measured).
///
/// The slot (oscillator_slot.hpp) calls it per sample while a route reaches
/// its Volume; unmodulated, the caller's gain goes to the slot's gain ramps.
///
/// Pure constexpr functions: no state, no allocation, real-time safe. NaN in
/// gives silence.
/// Doc: docs/dsp/modulation-destinations.md
namespace anoesis::dsp
{

/// The law's numbers. The defaults are the decision.
struct VolumeLaw
{
    /// The knob's range: the knob plus its modulation is held to it.
    float knobMin = 0.0f;
    float knobMax = 10.0f;
    /// The knob value whose gain is 1.
    float unity = 10.0f;
};

inline constexpr VolumeLaw volumeLaw{};

/// One oscillator's Volume terms, in knob units.
struct VolumeTerms
{
    float volume = 10.0f;    ///< the Volume knob (ramped)
    float modulation = 0.0f; ///< the voice's modulation to Volume X / Volume AB
};

/// The knob plus its modulation held to the knob's range; NaN counts as the
/// bottom (silence).
[[nodiscard]] constexpr float volumeUnits(const VolumeTerms& terms, const VolumeLaw& law = volumeLaw) noexcept
{
    const float sum = terms.volume + terms.modulation;
    if (!(sum >= law.knobMin)) // below, or NaN
    {
        return law.knobMin;
    }
    return sum > law.knobMax ? law.knobMax : sum;
}

/// The gain: volumeUnits / unity, 0 to 1 by default. A true division, so
/// unity is exactly 1.
[[nodiscard]] constexpr float volumeGain(const VolumeTerms& terms, const VolumeLaw& law = volumeLaw) noexcept
{
    return volumeUnits(terms, law) / law.unity;
}

} // namespace anoesis::dsp
