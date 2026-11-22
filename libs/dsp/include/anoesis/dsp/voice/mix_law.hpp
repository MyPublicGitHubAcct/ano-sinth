// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

/// Mix A and Mix B's law: each part's level after its filter and amp
/// envelope (inventory §5, X1-X3). Chosen by the project owner from measured
/// candidates (docs/dsp/mixer.md):
///
/// - Mix works in Sylenth1's internal units, 0 to 15, shown as 0 to 10 (the
///   developer: 6.667 shown is "a real 10 internally"). Modulation adds in
///   those units.
/// - The knob plus its modulation is clamped to the knob's range, [0, 15],
///   before the law (F24): a negative sum is silent, as the v3 engine "will
///   mute negative volume", not inverted as v2 did.
/// - The law is linear: gain = units / 10, so internal 10 (6.67 shown) is
///   unity and the top is x1.5 (+3.52 dB).
///
/// The voice (M1.4) calls it per sample with Mix's global 10 ms ramp read at
/// that sample and its own modulation (0 until M1.5).
///
/// Pure constexpr functions: no state, no allocation, real-time safe. NaN in
/// gives silence.
/// Doc: docs/dsp/mixer.md
namespace anoesis::dsp
{

/// The law's numbers. The defaults are the decision.
struct MixLaw
{
    /// The knob's range in internal units: the knob plus its modulation is
    /// held to it.
    float knobMin = 0.0f;
    float knobMax = 15.0f;
    /// The internal value whose gain is 1.
    float unity = 10.0f;
};

inline constexpr MixLaw mixLaw{};

/// Internal units per unit shown: Mix's knob shows 0-10 for 0-15.
inline constexpr float mixInternalPerShown = 1.5f;

/// One part's Mix terms, in internal units.
struct MixTerms
{
    float mix = 10.0f;       ///< the Mix knob (ramped)
    float modulation = 0.0f; ///< the voice's modulation to Mix X / Mix AB: the sum of amount x source
};

/// The knob plus its modulation held to the knob's range; NaN counts as the
/// bottom (silence). Written out rather than std::clamp to need no include.
[[nodiscard]] constexpr float mixUnits(const MixTerms& terms, const MixLaw& law = mixLaw) noexcept
{
    const float sum = terms.mix + terms.modulation;
    if (!(sum >= law.knobMin)) // below, or NaN
    {
        return law.knobMin;
    }
    return sum > law.knobMax ? law.knobMax : sum;
}

/// The gain: mixUnits / unity, 0 to 1.5 by default. A true division, so the
/// gain at every quarter unit is exact to one rounding and unity is 1.
[[nodiscard]] constexpr float mixGain(const MixTerms& terms, const MixLaw& law = mixLaw) noexcept
{
    return mixUnits(terms, law) / law.unity;
}

} // namespace anoesis::dsp
