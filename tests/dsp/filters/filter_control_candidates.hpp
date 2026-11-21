// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Candidate combining laws for Sylenth1's Filter Control (M1.3 task 6,
// inventory F12; docs/dsp/filter-control.md), measured by the hidden
// [filter-control-report] and the [filter-control] benchmarks on the real
// LadderFilter. The chosen model, (a2) with each control clamped and the
// resonances added (the project owner's decision, stage 2), now calls the
// library's filter_control.hpp; the others stay here as stage 1 built them,
// for comparison. Nothing else here is library code.
//
// The Filter Control's Cutoff and Resonance act on filters A and B at once;
// the developer: the knob values "are added to the knob values of both
// filter A and B", "the cutoff values are added on a logarithmic scale"
// (2008), and the base frequency is what you get if you "just add those
// values" (2016). What is open is the law the sum goes through, its anchor,
// where the clamps sit, how keytrack enters, and how the resonances combine.
//
// Everything is in knob units (0-10) in, and the filter's own units out: the
// cutoff as octaves above 1 Hz (log2 Hz, so LadderFilter takes 2^octaves as
// its cutoff, or octaves into its modulation input), and the loop gain k.
// Floats, as a voice would compute them; the report checks them in double.

#pragma once

#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace anoesis::test::filter_control
{

namespace dsp = anoesis::dsp;

/// One cutoff knob's exponential law: hzAtZero x 2^(octavesPerUnit x units).
struct CutoffLaw
{
    float log2HzAtZero = 0.0f;
    float octavesPerUnit = 1.0f;

    [[nodiscard]] constexpr float octaves(float units) const noexcept
    {
        return log2HzAtZero + (octavesPerUnit * units);
    }
    [[nodiscard]] float hz(float units) const noexcept
    {
        return std::exp2(octaves(units));
    }
    /// The knob position that this law reads as `hz`.
    [[nodiscard]] float unitsFor(float hz) const noexcept
    {
        return (std::log2(hz) - log2HzAtZero) / octavesPerUnit;
    }
};

/// F3's placeholder: 10 Hz x 2^(11 x / 10), 10 Hz to 20.5 kHz over one knob.
inline constexpr CutoffLaw f3Law{.log2HzAtZero = 3.3219281f, .octavesPerUnit = 1.1f}; // log2(10)

/// One octave per knob unit through the old LCD's 146.16 Hz at the knob's
/// centre (a user's reading, 2013): 4.5675 Hz x 2^units, the library's
/// dsp::filterControlLaw (whose anchor is in Hz; this is its log2).
inline constexpr CutoffLaw lcdLaw{.log2HzAtZero = 2.1914047f, .octavesPerUnit = 1.0f}; // log2(146.16) - 5

/// The chosen model as the library computes it: knob units in, octaves
/// above the anchor out (dsp::filterCutoffOctaves), keytrack as
/// keytrackOctaves().
[[nodiscard]] inline float builtOctavesAboveAnchor(float filter, float filterMod, float control, float controlMod,
                                                   float keytrack, float pitch) noexcept
{
    return dsp::filterCutoffOctaves({
        .cutoff = filter,
        .cutoffModulation = filterMod,
        .controlCutoff = control,
        .controlModulation = controlMod,
        .keytrack = dsp::keytrackOctaves(pitch, keytrack / 10.0f),
    });
}

enum class CutoffModel : std::uint8_t
{
    knobSumF3,     ///< (a1) knob units added, then F3's law over the sum (F12's hypothesis)
    knobSumOctave, ///< (a2) knob units added, then one octave per unit through 146.16 Hz at 5
    centredOffset, ///< (b) filter X through F3's law, the Filter Control an offset in octaves centred on 5
    hzSum,         ///< (c) each knob through F3's law, the two added in Hz
};

/// Where a control plus its modulation is held to a knob's range.
enum class Clamp : std::uint8_t
{
    none,       ///< nowhere but the DSP's [1 Hz, 0.495 fs]
    perControl, ///< each control plus its own modulation to [0, 10], then added (F24's hypothesis)
    sum20,      ///< the sum of the two to [0, 20]
    sum10,      ///< the sum of the two to [0, 10]: "the knob's range" applied to the sum
};

/// One filter's cutoff terms, in knob units.
struct CutoffInputs
{
    float filter = 5.0f;     ///< Cutoff A (or B)
    float control = 5.0f;    ///< the Filter Control's Cutoff
    float filterMod = 0.0f;  ///< modulation to Cutoff A (B): sum of amount x source
    float controlMod = 0.0f; ///< modulation to Cutoff AB, which moves both filters as the Filter Control does
    float keytrack = 0.0f;   ///< the Keytrack knob, 0-10
    float pitch = 9.0f;      ///< the voice's sounding pitch (keytracking.md)
};

/// Keytrack in knob units, as the developer's misc source adds them: 0 at
/// A-1, Keytrack x (pitch - 9) / 120, so 10 units over 10 octaves at 10.
[[nodiscard]] inline float keytrackUnits(float keytrack, float pitch) noexcept
{
    return keytrack * ((pitch - 9.0f) / 120.0f);
}

/// Keytrack in octaves, as keytrackOctaves() builds it: 1:1 at 10 whatever
/// the cutoff law.
[[nodiscard]] inline float keytrackOctavesDirect(float keytrack, float pitch) noexcept
{
    return (keytrack / 10.0f) * ((pitch - 9.0f) / 12.0f);
}

/// The two controls plus their modulation, held as `clamp` says; returns the
/// sum in knob units.
[[nodiscard]] inline float clampedSum(const CutoffInputs& in, Clamp clamp) noexcept
{
    const float a = in.filter + in.filterMod;
    const float c = in.control + in.controlMod;
    switch (clamp)
    {
    case Clamp::none:
        return a + c;
    case Clamp::perControl:
        return std::clamp(a, 0.0f, 10.0f) + std::clamp(c, 0.0f, 10.0f);
    case Clamp::sum20:
        return std::clamp(a + c, 0.0f, 20.0f);
    case Clamp::sum10:
        return std::clamp(a + c, 0.0f, 10.0f);
    }
    return a + c;
}

/// The cutoff in octaves above 1 Hz (log2 Hz), before the DSP's own clamp.
[[nodiscard]] inline float cutoffOctaves(CutoffModel model, Clamp clamp, const CutoffInputs& in) noexcept
{
    switch (model)
    {
    case CutoffModel::knobSumF3:
        return f3Law.octaves(clampedSum(in, clamp) + keytrackUnits(in.keytrack, in.pitch));
    case CutoffModel::knobSumOctave:
        if (clamp == Clamp::perControl)
        {
            // The decision: the library's law.
            return lcdLaw.log2HzAtZero +
                   builtOctavesAboveAnchor(in.filter, in.filterMod, in.control, in.controlMod, in.keytrack, in.pitch);
        }
        return lcdLaw.octaves(clampedSum(in, clamp) + keytrackUnits(in.keytrack, in.pitch));
    case CutoffModel::centredOffset:
    {
        // Each control is clamped on its own; the sum variants don't apply.
        const bool held = clamp != Clamp::none;
        const float a = held ? std::clamp(in.filter + in.filterMod, 0.0f, 10.0f) : in.filter + in.filterMod;
        const float c = held ? std::clamp(in.control + in.controlMod, 0.0f, 10.0f) : in.control + in.controlMod;
        return f3Law.octaves(a) + (f3Law.octavesPerUnit * (c - 5.0f)) + keytrackOctavesDirect(in.keytrack, in.pitch);
    }
    case CutoffModel::hzSum:
    {
        const bool held = clamp != Clamp::none;
        const float a = held ? std::clamp(in.filter + in.filterMod, 0.0f, 10.0f) : in.filter + in.filterMod;
        const float c = held ? std::clamp(in.control + in.controlMod, 0.0f, 10.0f) : in.control + in.controlMod;
        return std::log2(f3Law.hz(a) + f3Law.hz(c)) + keytrackOctavesDirect(in.keytrack, in.pitch);
    }
    }
    return 0.0f;
}

/// The cutoff in Hz after the DSP's clamp to [1 Hz, 0.495 fs], as
/// LadderFilter applies it.
[[nodiscard]] inline double dspCutoffHz(double octaves, double sampleRate) noexcept
{
    return std::clamp(std::exp2(octaves), 1.0, 0.495 * sampleRate);
}

// --- Resonance --------------------------------------------------------------------------

/// F5's placeholder: k = 6 x knob / 10.
inline constexpr float kPerUnit = 0.6f;

enum class ResonanceModel : std::uint8_t
{
    knobSum,     ///< knob units added (= k added, under F5's linear law), each control clamped to [0, 10]
    knobSumTo10, ///< knob units added, the sum clamped to [0, 10]
    multiplied,  ///< k = k_X x (Filter Control / 5): the Filter Control a gain, 5 unity
    maximum,     ///< k = 0.6 x max(X, Filter Control)
};

struct ResonanceInputs
{
    float filter = 0.0f;
    float control = 0.0f;
    float filterMod = 0.0f;
    float controlMod = 0.0f;
};

[[nodiscard]] inline float resonanceK(ResonanceModel model, const ResonanceInputs& in) noexcept
{
    const float a = std::clamp(in.filter + in.filterMod, 0.0f, 10.0f);
    const float c = std::clamp(in.control + in.controlMod, 0.0f, 10.0f);
    switch (model)
    {
    case ResonanceModel::knobSum:
        // The decision: the library's law.
        return dsp::filterResonanceK({
            .resonance = in.filter,
            .resonanceModulation = in.filterMod,
            .controlResonance = in.control,
            .controlModulation = in.controlMod,
        });
    case ResonanceModel::knobSumTo10:
        return kPerUnit * std::clamp(in.filter + in.filterMod + in.control + in.controlMod, 0.0f, 10.0f);
    case ResonanceModel::multiplied:
        return kPerUnit * a * (c / 5.0f);
    case ResonanceModel::maximum:
        return kPerUnit * std::max(a, c);
    }
    return 0.0f;
}

/// The same with no knob-space clamp at all: the DSP's [0, 64] only.
[[nodiscard]] inline float resonanceKUnclamped(const ResonanceInputs& in) noexcept
{
    return kPerUnit * (in.filter + in.filterMod + in.control + in.controlMod);
}

} // namespace anoesis::test::filter_control
