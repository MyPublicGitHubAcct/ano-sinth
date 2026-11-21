// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

/// The Filter Control's combining law: how Sylenth1's shared Cutoff and
/// Resonance (inventory §3, F12, F26-F30) combine with each filter's own,
/// with modulation and keytrack. Chosen by the project owner from measured
/// candidates (docs/dsp/filter-control.md):
///
/// - Knob units add, then one exponential law at one octave per unit:
///     S = [Cutoff X + its modulation]  + [Filter Control Cutoff + Cutoff AB
///         modulation] + keytrack,  cutoff = 4.5675 Hz x 2^S,
///   each bracket held to the knob's range [0, 10]. One knob at 5 and the
///   other at 0 is 146.16 Hz (the old LCD's reading at a knob's centre); both
///   at 5, the defaults, 4,677 Hz.
/// - Each control plus its own modulation is clamped, then the two are added;
///   the sum is not clamped (it spans 0-20). Keytrack goes in after the
///   clamps, unclamped (its source is, F20). Then the DSP's own [1 Hz,
///   0.495 fs] applies, from S = 12.22 at 44.1 kHz to 14.34 at 192 kHz.
/// - Keytrack's term in knob units is the developer's misc source times the
///   knob, Keytrack x (pitch - 9) / 120; at one octave per unit that is
///   keytrackOctaves(pitch, Keytrack / 10) exactly, which the voice passes.
/// - Resonance the same way: k = 0.6 x ([Res X + mod] + [Res Ctrl + mod]),
///   each control clamped, 0 to 12, self-oscillating from a sum of 6.67.
///
/// The output is the filter's units: octaves above the law's anchor, for
/// LadderFilter::setCutoffModulation() with setCutoff(anchorHz) held, and the
/// loop gain k. The knob range, the anchor, the octaves per unit, and k per
/// unit are parameters (FilterControlLaw), so a demo measurement (F3, F5,
/// F27-F29) changes a default, not the function.
///
/// The voice (M1.4) calls these per voice at each of its filter's 32-sample
/// grid points, with the knobs' global 10 ms ramps read at the end of the
/// coming segment (LinearSmoother::valueAfter) and its own modulation there.
///
/// Pure constexpr functions: no state, no allocation, real-time safe. NaN in
/// gives NaN out.
/// Doc: docs/dsp/filter-control.md
namespace anoesis::dsp
{

/// The law's numbers. The defaults are the decision (model (a2)).
struct FilterControlLaw
{
    /// A knob's range: each control plus its own modulation is held to it.
    float knobMin = 0.0f;
    float knobMax = 10.0f;
    /// The cutoff at S = 0: 146.16 Hz (a knob's centre on the old LCD) five
    /// octaves down.
    float anchorHz = 4.5675f;
    float octavesPerUnit = 1.0f;
    /// F5's resonance law: k per knob unit of the sum.
    float kPerUnit = 0.6f;
};

inline constexpr FilterControlLaw filterControlLaw{};

/// One filter's cutoff terms, in knob units.
struct FilterCutoffTerms
{
    float cutoff = 0.0f;            ///< filter X's Cutoff knob (ramped)
    float cutoffModulation = 0.0f;  ///< the voice's modulation to Cutoff X: the sum of amount x source
    float controlCutoff = 0.0f;     ///< the Filter Control's Cutoff knob (ramped)
    float controlModulation = 0.0f; ///< the voice's modulation to Cutoff AB
    float keytrack = 0.0f;          ///< keytrackOctaves(pitch, Keytrack / 10): knob units at 1 octave per unit
};

/// One filter's resonance terms, in knob units.
struct FilterResonanceTerms
{
    float resonance = 0.0f;           ///< filter X's Resonance knob (ramped)
    float resonanceModulation = 0.0f; ///< the voice's modulation to Reso X
    float controlResonance = 0.0f;    ///< the Filter Control's Resonance knob (ramped)
    float controlModulation = 0.0f;   ///< the voice's modulation to Reso AB
};

/// A control plus its modulation, held to the knob's range. Written out
/// rather than std::clamp to need no include; NaN passes through.
[[nodiscard]] constexpr float clampToKnob(float units, const FilterControlLaw& law = filterControlLaw) noexcept
{
    if (units < law.knobMin)
    {
        return law.knobMin;
    }
    return units > law.knobMax ? law.knobMax : units;
}

/// S: the cutoff sum in knob units. Each control plus its own modulation is
/// clamped, the two are added, and keytrack goes in after, unclamped; the sum
/// is not clamped.
[[nodiscard]] constexpr float filterCutoffUnits(const FilterCutoffTerms& terms,
                                                const FilterControlLaw& law = filterControlLaw) noexcept
{
    return clampToKnob(terms.cutoff + terms.cutoffModulation, law) +
           clampToKnob(terms.controlCutoff + terms.controlModulation, law) + terms.keytrack;
}

/// The cutoff in octaves above law.anchorHz: octavesPerUnit x S. The voice
/// feeds it to LadderFilter::setCutoffModulation() with setCutoff(anchorHz)
/// held, so the cutoff is anchorHz x 2^octaves, clamped by the DSP.
[[nodiscard]] constexpr float filterCutoffOctaves(const FilterCutoffTerms& terms,
                                                  const FilterControlLaw& law = filterControlLaw) noexcept
{
    return law.octavesPerUnit * filterCutoffUnits(terms, law);
}

/// The resonance sum in knob units: each control plus its own modulation
/// clamped, then added (0 to 20).
[[nodiscard]] constexpr float filterResonanceUnits(const FilterResonanceTerms& terms,
                                                   const FilterControlLaw& law = filterControlLaw) noexcept
{
    return clampToKnob(terms.resonance + terms.resonanceModulation, law) +
           clampToKnob(terms.controlResonance + terms.controlModulation, law);
}

/// The loop gain k: kPerUnit x the resonance sum, 0 to 12 by default (4,
/// the self-oscillation threshold, at a sum of 6.67).
[[nodiscard]] constexpr float filterResonanceK(const FilterResonanceTerms& terms,
                                               const FilterControlLaw& law = filterControlLaw) noexcept
{
    return law.kPerUnit * filterResonanceUnits(terms, law);
}

} // namespace anoesis::dsp
