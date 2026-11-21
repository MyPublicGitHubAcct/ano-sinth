// The Filter Control report (docs/dsp/filter-control.md, M1.3 task 6):
// measures the built combining law (filter_control.hpp, the project owner's
// choice of model (a2)) against stage 1's other candidates
// (filter_control_candidates.hpp) on the real LadderFilter, the chosen model
// fed as the voice will feed it (filter_control_support.hpp):
//
// - what each model predicts for the settings that tutorials and users
//   report (the old LCD's Hz readings, the defaults, "fully open");
// - the effective cutoff (the -12 dB point of LP24 at k 0, which the
//   prewarped ladder puts exactly at the cutoff) over a grid of filter X x
//   Filter Control positions, at 44.1 and 96 kHz, against each model's
//   formula;
// - the reachable range, where the DSP's ceiling (0.495 fs) takes over, and
//   how the knob feels (octaves per knob unit across the range);
// - keytrack 0, 5, 10 from C1 to C7: the self-oscillation pitch's slope;
// - full +/- modulation with and without each clamp;
// - resonance: where self-oscillation starts, and what the Filter Control's
//   Resonance at 0 and 10 does to a filter at 0, 5, and 10;
// - how the voice feeds the sum (the filter's own 10 ms ramp, a ramp in the
//   voice, both, neither): a knob step, a 7-bit MIDI CC sweep, a clamp
//   transient, and block sizes;
// - feeding k: setResonance() at each grid point against the knob's own ramp
//   and the filter's resonance modulation input (a step, a CC sweep, an LFO).
//
// Hidden from the default run; run it in an optimised build (about 30 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[filter-control-report]"
//
// CPU: the "[filter-control]" benchmarks (tools/bench.sh "[filter-control]").

#include "filter_control_candidates.hpp"
#include "filter_control_support.hpp"
#include "ladder_reference.hpp"

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/signals.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::filter_control;
namespace dsp = anoesis::dsp;

namespace
{

constexpr double pi = std::numbers::pi;
constexpr std::size_t grid = dsp::LadderFilter::gridSamples;

std::string fixed(double x, int digits)
{
    if (std::isnan(x))
    {
        return "-";
    }
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

/// Hz with sensible digits: 4.57, 146.2, 4,677.
std::string hzText(double hz)
{
    if (std::isnan(hz))
    {
        return "-";
    }
    if (hz >= 1e6)
    {
        return "2^" + fixed(std::log2(hz), 1);
    }
    if (hz < 100.0)
    {
        return fixed(hz, 2);
    }
    if (hz < 1000.0)
    {
        return fixed(hz, 1);
    }
    std::ostringstream s;
    const auto whole = std::llround(hz);
    s << whole / 1000 << ',' << std::setw(3) << std::setfill('0') << whole % 1000;
    return s.str();
}

std::string_view modelName(CutoffModel m)
{
    switch (m)
    {
    case CutoffModel::knobSumF3:
        return "(a1) knob sum, F3's law";
    case CutoffModel::knobSumOctave:
        return "(a2) knob sum, 1 oct/unit, 146.16 Hz at 5";
    case CutoffModel::centredOffset:
        return "(b) Filter Control an offset centred on 5";
    case CutoffModel::hzSum:
        return "(c) Hz added";
    }
    return "?";
}

std::string_view clampName(Clamp c)
{
    switch (c)
    {
    case Clamp::none:
        return "none (the DSP's only)";
    case Clamp::perControl:
        return "each control + its modulation to [0, 10]";
    case Clamp::sum20:
        return "the sum to [0, 20]";
    case Clamp::sum10:
        return "the sum to [0, 10]";
    }
    return "?";
}

std::string_view resonanceName(ResonanceModel m)
{
    switch (m)
    {
    case ResonanceModel::knobSum:
        return "knob units added, each clamped";
    case ResonanceModel::knobSumTo10:
        return "knob units added, the sum to [0, 10]";
    case ResonanceModel::multiplied:
        return "multiplied (Filter Control / 5)";
    case ResonanceModel::maximum:
        return "the larger of the two";
    }
    return "?";
}

constexpr std::array<CutoffModel, 4> cutoffModels{
    CutoffModel::knobSumF3,
    CutoffModel::knobSumOctave,
    CutoffModel::centredOffset,
    CutoffModel::hzSum,
};
constexpr std::array<ResonanceModel, 4> resonanceModels{
    ResonanceModel::knobSum,
    ResonanceModel::knobSumTo10,
    ResonanceModel::multiplied,
    ResonanceModel::maximum,
};

/// The law a model's LCD would read each knob by (all three cutoff knobs read
/// 146.16 Hz at their centre on the old display, so one law for all three).
CutoffLaw displayLaw(CutoffModel m)
{
    return m == CutoffModel::knobSumOctave ? lcdLaw : f3Law;
}

double effectiveHz(CutoffModel m, Clamp c, const CutoffInputs& in, double rate)
{
    return dspCutoffHz(static_cast<double>(cutoffOctaves(m, c, in)), rate);
}

double centsBetween(double hz, double referenceHz)
{
    return 1200.0 * std::log2(hz / referenceHz);
}

dsp::LadderFilter settledFilter(double rate, double hz, float k, bool warm = true)
{
    dsp::LadderFilter f;
    f.setWarm(warm);
    f.setResonance(k);
    f.setCutoff(static_cast<float>(hz));
    f.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = 4096, .numChannels = 1});
    return f;
}

std::vector<float> render(dsp::LadderFilter& f, std::vector<float> x)
{
    Buffer buffer{1, x.size()};
    std::ranges::copy(x, buffer.channel(0).begin());
    for (std::size_t start = 0; start < x.size(); start += 4096)
    {
        f.process(buffer.block().subBlock(start, std::min<std::size_t>(4096, x.size() - start)));
    }
    return {buffer.channel(0).begin(), buffer.channel(0).end()};
}

// --- 1. What each model predicts for the reported settings ----------------------------

void reportEvidence()
{
    constexpr double rate = 44100.0;
    std::cout << "\n### What each model predicts for the reported settings (44.1 kHz)\n\n"
                 "Each model reads every cutoff knob by its own law (the old LCD showed each knob's own Hz: "
                 "146.16 Hz at the centre for all three, a user in 2013). A Hz reading from a tutorial is turned into "
                 "a knob position by that law; then the effective cutoff is the model's sum, clamped to [1 Hz, "
                 "0.495 fs] = [1, 21,830] Hz. Clamps: each control plus its modulation to [0, 10].\n\n"
                 "| Setting | Source | (a1) knob sum, F3 | (a2) knob sum, 1 oct/unit | (b) centred offset | "
                 "(c) Hz added |\n|---|---|---|---|---|---|\n";
    const auto row =
        [&](const std::string& setting, const std::string& source, const std::function<std::string(CutoffModel)>& cell)
    {
        std::cout << "| " << setting << " | " << source;
        for (const auto m : cutoffModels)
        {
            std::cout << " | " << cell(m);
        }
        std::cout << " |\n";
    };
    const auto effective = [&](CutoffModel m, CutoffInputs in)
    {
        return effectiveHz(m, Clamp::perControl, in, rate);
    };
    row("A knob at 5 reads (LCD)", "146.16 Hz (titchbit, 2013)",
        [](CutoffModel m) { return hzText(static_cast<double>(displayLaw(m).hz(5.0f))) + " Hz"; });
    row("A knob at 7 reads (LCD)", "\"7 (around the 800Hz mark)\" (Attack, 2015)",
        [](CutoffModel m) { return hzText(static_cast<double>(displayLaw(m).hz(7.0f))) + " Hz"; });
    row("Knob position that reads 800 Hz", "",
        [](CutoffModel m) { return fixed(static_cast<double>(displayLaw(m).unitsFor(800.0f)), 2); });
    row("Defaults: A 5, Control 5", "centre defaults (titchbit; fluffy, 2018)",
        [&](CutoffModel m) { return hzText(effective(m, {.filter = 5.0f, .control = 5.0f})) + " Hz"; });
    row("Detuned pad: A \"7 (~800 Hz)\" (read as 800 Hz), Control \"~60 Hz\", Keytrack 3, at C3", "Attack, 2015",
        [&](CutoffModel m)
        {
            const auto law = displayLaw(m);
            return hzText(effective(m,
                                    {
                                        .filter = law.unitsFor(800.0f),
                                        .control = law.unitsFor(60.0f),
                                        .keytrack = 3.0f,
                                        .pitch = 48.0f,
                                    })) +
                   " Hz";
        });
    row("Vintage arp: A 40 Hz, Control 20 Hz", "Attack, 2015",
        [&](CutoffModel m)
        {
            const auto law = displayLaw(m);
            return hzText(effective(m, {.filter = law.unitsFor(40.0f), .control = law.unitsFor(20.0f)})) + " Hz";
        });
    row("... with Mod Env 1 \"all the way to the right\" (+10) on Cutoff A, at its peak", "Attack, 2015",
        [&](CutoffModel m)
        {
            const auto law = displayLaw(m);
            return hzText(effective(
                       m, {.filter = law.unitsFor(40.0f), .control = law.unitsFor(20.0f), .filterMod = 10.0f})) +
                   " Hz";
        });
    row("A 0, Control 5 / 4.5 (\"closes completely\" at 4.5)", "fluffy_little_something, 2018 (A unstated)",
        [&](CutoffModel m)
        {
            return hzText(effective(m, {.filter = 0.0f, .control = 5.0f})) + " / " +
                   hzText(effective(m, {.filter = 0.0f, .control = 4.5f}));
        });
    row("A 10, Control 5 (\"at 10.000 the filter is fully open\")", "7up, 2017 (v3.0.2)",
        [&](CutoffModel m) { return hzText(effective(m, {.filter = 10.0f, .control = 5.0f})) + " Hz"; });
    row("A 10, Control 0 (\"you may need to adjust both cutoff knobs\" to open it fully)", "the developer, 2008",
        [&](CutoffModel m) { return hzText(effective(m, {.filter = 10.0f, .control = 0.0f})) + " Hz"; });
    row("A 0, Control 0: the bottom", "",
        [&](CutoffModel m) { return hzText(effective(m, {.filter = 0.0f, .control = 0.0f})) + " Hz"; });
    row("Keytrack 10, one octave of the keyboard", "1:1 (manual, the developer, Chris-S)",
        [&](CutoffModel m)
        {
            const double lo = effective(m, {.filter = 3.0f, .control = 0.0f, .keytrack = 10.0f, .pitch = 48.0f});
            const double hi = effective(m, {.filter = 3.0f, .control = 0.0f, .keytrack = 10.0f, .pitch = 60.0f});
            return fixed(centsBetween(hi, lo), 0) + " cents";
        });
}

// --- 2. The effective cutoff over filter X x Filter Control -----------------------------

void reportGrid()
{
    constexpr std::array<float, 5> positions{0.0f, 2.5f, 5.0f, 7.5f, 10.0f};
    // The filter's small-signal response is pinned to 4e-5 of its peak
    // (ladder_filter_test.cpp). At the cutoff |H| = 1/4, so that is 1.6e-4
    // relative, 0.0014 dB, and LP24 falls 12.04 dB per octave there
    // (d/dlog2 w of -40 log10(1 + w^2) at w = 1): 0.14 cents.
    const double toleranceCents = 1200.0 * (20.0 * std::log10(1.0 + (4e-5 / 0.25))) / 12.0412;
    std::cout << "\n### The effective cutoff over filter X x Filter Control (LP24, k 0, -80 dBFS)\n\n"
                 "The -12.04 dB point of LadderFilter's response (bisection on the impulse response's DTFT), in Hz. "
                 "Rows: Cutoff A; columns: the Filter Control's Cutoff. Each control clamped to [0, 10] (no "
                 "modulation here, so it doesn't bite). Below each table: the largest distance from the model's "
                 "formula after the DSP's clamp, at 44.1 and 96 kHz (tolerance "
              << fixed(toleranceCents, 2) << " cents, from the filter tests' small-signal bound).\n";
    for (const auto m : cutoffModels)
    {
        std::cout << "\n**" << modelName(m) << "**, 44.1 kHz (96 kHz where it differs)\n\n"
                  << "| A \\ Control | 0 | 2.5 | 5 | 7.5 | 10 |\n|---|---|---|---|---|---|\n";
        double worst = 0.0;
        for (const float a : positions)
        {
            std::cout << "| " << fixed(static_cast<double>(a), 1);
            for (const float c : positions)
            {
                const CutoffInputs in{.filter = a, .control = c};
                std::array<double, 2> measured{};
                for (std::size_t r = 0; r < 2; ++r)
                {
                    const double rate = r == 0 ? 44100.0 : 96000.0;
                    const double expected = effectiveHz(m, Clamp::perControl, in, rate);
                    // The chosen model fed as the voice feeds it (the anchor in
                    // setCutoff, the law's octaves as modulation); the others
                    // through setCutoff in Hz.
                    measured[r] =
                        m == CutoffModel::knobSumOctave
                            ? minus12DbPoint(fedFilter(builtOctavesAboveAnchor(a, 0.0f, c, 0.0f, 0.0f, 9.0f), rate),
                                             expected, rate)
                            : minus12DbPoint(settledFilter(rate, expected, 0.0f), expected, rate);
                    const double cents = centsBetween(measured[r], expected);
                    worst = std::max(worst, std::fabs(cents));
                    CHECK(std::fabs(cents) <= toleranceCents);
                }
                std::string cell = hzText(measured[0]);
                if (std::fabs(centsBetween(measured[1], measured[0])) > 1.0)
                {
                    cell += " (" + hzText(measured[1]) + ")";
                }
                std::cout << " | " << cell;
            }
            std::cout << " |\n";
        }
        std::cout << "\nLargest distance from the formula: " << fixed(worst, 4) << " cents.\n";
    }
}

// --- 3. Range, the DSP's ceiling, and the knob's feel ------------------------------------

void reportRange()
{
    std::cout << "\n### The reachable range, where the ceiling takes over, and the knob's feel\n\n"
                 "Knobs only (A and Control over [0, 10] on a 0.1 grid, no modulation, keytrack 0), after the DSP's "
                 "[1 Hz, 0.495 fs]. Pinned: the share of the A x Control square at the ceiling. Span: octaves from A = "
                 "Control = 0 to A = Control = 10 before the clamp; one knob: octaves over one knob's travel with the "
                 "other held.\n\n"
                 "| Model | Rate | Lowest | Default (5, 5) | Highest | Pinned at the ceiling | Span of the sum | One "
                 "knob |\n|---|---|---|---|---|---|---|---|\n";
    for (const auto m : cutoffModels)
    {
        for (const double rate : {44100.0, 96000.0})
        {
            double lo = std::numeric_limits<double>::infinity();
            double hi = 0.0;
            std::size_t pinned = 0;
            std::size_t count = 0;
            for (int i = 0; i <= 100; ++i)
            {
                for (int j = 0; j <= 100; ++j)
                {
                    const CutoffInputs in{
                        .filter = 0.1f * static_cast<float>(i),
                        .control = 0.1f * static_cast<float>(j),
                    };
                    const double hz = effectiveHz(m, Clamp::perControl, in, rate);
                    lo = std::min(lo, hz);
                    hi = std::max(hi, hz);
                    pinned += hz >= 0.495 * rate * (1.0 - 1e-6) ? 1U : 0U;
                    ++count;
                }
            }
            const CutoffInputs defaults; // both knobs at 5
            const auto octaves = [&](float a, float c)
            {
                return static_cast<double>(cutoffOctaves(m, Clamp::perControl, {.filter = a, .control = c}));
            };
            std::cout << "| " << modelName(m) << " | " << fixed(rate / 1000.0, 1) << " | " << hzText(lo) << " | "
                      << hzText(effectiveHz(m, Clamp::perControl, defaults, rate)) << " | " << hzText(hi) << " | "
                      << fixed(100.0 * static_cast<double>(pinned) / static_cast<double>(count), 1) << "% | "
                      << fixed(octaves(10.0f, 10.0f) - octaves(0.0f, 0.0f), 2) << " | "
                      << fixed(octaves(10.0f, 5.0f) - octaves(0.0f, 5.0f), 2) << " |\n";
        }
    }

    std::cout << "\n**Where the ceiling takes over** (the sum in knob units at which the cutoff reaches 0.495 fs; "
                 "(a) models only, since the others aren't a sum):\n\n"
                 "| Model | 44.1 kHz | 48 kHz | 96 kHz | 192 kHz |\n|---|---|---|---|---|\n";
    for (const auto m : {CutoffModel::knobSumF3, CutoffModel::knobSumOctave})
    {
        const auto law = m == CutoffModel::knobSumF3 ? f3Law : lcdLaw;
        std::cout << "| " << modelName(m);
        for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0})
        {
            std::cout << " | " << fixed(static_cast<double>(law.unitsFor(static_cast<float>(0.495 * rate))), 2);
        }
        std::cout << " |\n";
    }

    std::cout << "\n**How the knob feels**: octaves per unit of Cutoff A (a central difference of 0.01, after the "
                 "DSP's clamp, 44.1 kHz), at five positions of A, with the Filter Control at 0 / 5 / 10. 0.00 is a "
                 "dead zone: the clamp, or (c)'s other knob dominating.\n\n"
                 "| Model | A 0 | A 2.5 | A 5 | A 7.5 | A 10 |\n|---|---|---|---|---|---|\n";
    for (const auto m : cutoffModels)
    {
        std::cout << "| " << modelName(m);
        for (const float a : {0.0f, 2.5f, 5.0f, 7.5f, 10.0f})
        {
            std::string cell;
            for (const float c : {0.0f, 5.0f, 10.0f})
            {
                const float lo = std::max(0.0f, a - 0.01f);
                const float hi = std::min(10.0f, a + 0.01f);
                const double d = std::log2(effectiveHz(m, Clamp::perControl, {.filter = hi, .control = c}, 44100.0) /
                                           effectiveHz(m, Clamp::perControl, {.filter = lo, .control = c}, 44100.0)) /
                                 static_cast<double>(hi - lo);
                cell += (cell.empty() ? "" : " / ") + fixed(d, 2);
            }
            std::cout << " | " << cell;
        }
        std::cout << " |\n";
    }

    std::cout << "\n**With keytrack and modulation** (before the DSP's clamp, so what the voice asks for): keytrack 10 "
                 "over MIDI 0-127; then 16 slots at +10 or -10 on Cutoff A and Cutoff AB, each control clamped to "
                 "[0, 10] or not clamped.\n\n"
                 "| Model | Knobs + keytrack 10, MIDI 0-127 | Full +/- modulation, clamped | Full +/- modulation, "
                 "not clamped |\n|---|---|---|---|\n";
    for (const auto m : cutoffModels)
    {
        const auto hzAt = [&](Clamp c, CutoffInputs in)
        {
            return std::exp2(static_cast<double>(cutoffOctaves(m, c, in)));
        };
        const double ktLo =
            hzAt(Clamp::perControl, {.filter = 0.0f, .control = 0.0f, .keytrack = 10.0f, .pitch = 0.0f});
        const double ktHi =
            hzAt(Clamp::perControl, {.filter = 10.0f, .control = 10.0f, .keytrack = 10.0f, .pitch = 127.0f});
        const CutoffInputs down{.filterMod = -160.0f, .controlMod = -160.0f};
        const CutoffInputs up{.filterMod = 160.0f, .controlMod = 160.0f};
        std::cout << "| " << modelName(m) << " | " << hzText(ktLo) << " to " << hzText(ktHi) << " Hz | "
                  << hzText(hzAt(Clamp::perControl, down)) << " to " << hzText(hzAt(Clamp::perControl, up)) << " Hz | "
                  << fixed(std::log2(hzAt(Clamp::none, down)), 0) << " to "
                  << fixed(std::log2(hzAt(Clamp::none, up)), 0)
                  << " octaves above 1 Hz (the filter clamps its modulation to +/-40 octaves, then to [1 Hz, 0.495 "
                     "fs]) |\n";
    }
}

// --- 4. Keytrack 0, 5, 10 from C1 to C7 ---------------------------------------------------

/// Long enough for a self-oscillation to grow from an impulse and settle
/// (keytrack_report.cpp): about ten e-folding times of the linear loop.
double secondsFor(float k, double hz)
{
    const double growth = (std::pow(static_cast<double>(k), 0.25) * std::numbers::sqrt2 / 2.0) - 1.0;
    return std::max(2.5, 0.5 + (12.0 / (growth * 2.0 * pi * hz)));
}

/// The self-oscillation pitch over the last 0.5 s of an impulse response, or NaN.
double selfOscillationHz(double rate, double cutoffHz, float k, bool warm, double* levelDb = nullptr)
{
    auto f = settledFilter(rate, cutoffHz, k, warm);
    std::vector<float> x(static_cast<std::size_t>(secondsFor(k, cutoffHz) * rate), 0.0f);
    x[0] = 0.5f;
    const auto y = render(f, x);
    const auto tail = std::span{y}.last(static_cast<std::size_t>(0.5 * rate));
    double sum = 0.0;
    for (const float v : tail)
    {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    const double rms = std::sqrt(sum / static_cast<double>(tail.size()));
    if (levelDb != nullptr)
    {
        *levelDb = 20.0 * std::log10(rms * std::numbers::sqrt2); // peak-equivalent dBFS, as filters.md
    }
    if (!(rms > 1e-6) || !std::isfinite(rms))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return measureFrequency(tail, rate);
}

void reportKeytrack()
{
    constexpr double rate = 44100.0;
    constexpr float k = 4.4f;
    constexpr std::array<float, 7> notes{24.0f, 36.0f, 48.0f, 60.0f, 72.0f, 84.0f, 96.0f};
    std::cout << "\n### Keytrack 0, 5, 10 from C1 to C7 (44.1 kHz, k 4.4, Warm on)\n\n"
                 "The filter self-oscillating, Cutoff A set so that C4's cutoff is 261.6 Hz at that keytrack (the "
                 "Filter Control at 0, so that (c)'s other knob adds only 10 Hz; no clamp, so A may leave [0, 10]). "
                 "Slope: the least-squares fit of the "
                 "self-oscillation pitch against the note, in cents per octave of the keyboard (1:1 is 1200). "
                 "Spread: the largest distance of any note from that line, in cents. (a1) adds keytrack in knob "
                 "units through F3's 1.1 octaves per unit; (b) and (c) add the built keytrackOctaves().\n\n"
                 "| Model | Keytrack | Expected slope | Measured slope | Spread | A for C4 at 261.6 Hz |\n"
                 "|---|---|---|---|---|---|\n";
    for (const auto m : cutoffModels)
    {
        for (const float keytrack : {0.0f, 5.0f, 10.0f})
        {
            // Solve for A by bisection: the cutoff rises with A in every model.
            float lo = -40.0f;
            float hi = 40.0f;
            for (int i = 0; i < 60; ++i)
            {
                const float mid = 0.5f * (lo + hi);
                const double hz = std::exp2(static_cast<double>(cutoffOctaves(
                    m, Clamp::none, {.filter = mid, .control = 0.0f, .keytrack = keytrack, .pitch = 60.0f})));
                (hz < 261.63 ? lo : hi) = mid;
            }
            const float a = 0.5f * (lo + hi);
            std::vector<double> x;
            std::vector<double> y;
            for (const float note : notes)
            {
                const double cutoff = std::exp2(static_cast<double>(cutoffOctaves(
                    m, Clamp::none, {.filter = a, .control = 0.0f, .keytrack = keytrack, .pitch = note})));
                x.push_back(static_cast<double>(note) / 12.0);
                y.push_back(1200.0 * std::log2(selfOscillationHz(rate, cutoff, k, true)));
            }
            const auto n = static_cast<double>(x.size());
            double sx = 0.0;
            double sy = 0.0;
            double sxx = 0.0;
            double sxy = 0.0;
            for (std::size_t i = 0; i < x.size(); ++i)
            {
                sx += x[i];
                sy += y[i];
                sxx += x[i] * x[i];
                sxy += x[i] * y[i];
            }
            const double slope = ((n * sxy) - (sx * sy)) / ((n * sxx) - (sx * sx));
            const double intercept = (sy - (slope * sx)) / n;
            double spread = 0.0;
            for (std::size_t i = 0; i < x.size(); ++i)
            {
                spread = std::max(spread, std::fabs(y[i] - (intercept + (slope * x[i]))));
            }
            const double perUnit = m == CutoffModel::knobSumF3 ? 1.1 : 1.0;
            const double expected = m == CutoffModel::knobSumF3 || m == CutoffModel::knobSumOctave
                                        ? 1200.0 * perUnit * static_cast<double>(keytrack) / 10.0
                                        : 1200.0 * static_cast<double>(keytrack) / 10.0;
            std::cout << "| " << modelName(m) << " | " << fixed(static_cast<double>(keytrack), 0) << " | "
                      << fixed(expected, 0) << " | " << fixed(slope, 1) << " | " << fixed(spread, 2) << " | "
                      << fixed(static_cast<double>(a), 2) << " |\n";
        }
    }
}

// --- 5. Full +/- modulation: the clamps -------------------------------------------------

void reportClamps()
{
    constexpr double rate = 44100.0;
    std::cout << "\n### Full +/- modulation, with and without each clamp (44.1 kHz)\n\n"
                 "An LFO at +/-10 knob units on Cutoff A (one slot at full depth), Cutoff A at 0, 5, or 10, the "
                 "Filter Control at 5, over one cycle (1,000 points). Lowest and highest effective cutoff after the "
                 "DSP's clamp; held: the share of the cycle where a knob-space clamp changes the cutoff; ceiling: the "
                 "share at 0.495 fs.\n\n"
                 "| Model | Clamp | A | Lowest | Highest | Held by the knob clamp | At the ceiling |\n"
                 "|---|---|---|---|---|---|---|\n";
    for (const auto m : {CutoffModel::knobSumF3, CutoffModel::knobSumOctave})
    {
        for (const auto clamp : {Clamp::none, Clamp::perControl, Clamp::sum20, Clamp::sum10})
        {
            for (const float a : {0.0f, 5.0f, 10.0f})
            {
                double lo = std::numeric_limits<double>::infinity();
                double hi = 0.0;
                std::size_t held = 0;
                std::size_t ceiling = 0;
                constexpr std::size_t points = 1000;
                for (std::size_t i = 0; i < points; ++i)
                {
                    const auto lfo = static_cast<float>(10.0 * std::sin(2.0 * pi * static_cast<double>(i) / points));
                    const CutoffInputs in{.filter = a, .control = 5.0f, .filterMod = lfo};
                    const double hz = effectiveHz(m, clamp, in, rate);
                    lo = std::min(lo, hz);
                    hi = std::max(hi, hz);
                    held +=
                        std::fabs(cutoffOctaves(m, clamp, in) - cutoffOctaves(m, Clamp::none, in)) > 1e-6f ? 1U : 0U;
                    ceiling += hz >= 0.495 * rate * (1.0 - 1e-6) ? 1U : 0U;
                }
                std::cout << "| " << modelName(m) << " | " << clampName(clamp) << " | "
                          << fixed(static_cast<double>(a), 0) << " | " << hzText(lo) << " | " << hzText(hi) << " | "
                          << fixed(100.0 * static_cast<double>(held) / points, 1) << "% | "
                          << fixed(100.0 * static_cast<double>(ceiling) / points, 1) << "% |\n";
            }
        }
    }

    std::cout << "\n**Every slot at full depth** (2 mod envelopes, 2 LFOs, 4 misc sources, two destinations each: "
                 "16 x 10 = 160 knob units) on Cutoff A, or on Resonance A, with every knob at 10. The cutoff in "
                 "octaves above 1 Hz before the DSP; k before the DSP's ceiling of 64:\n\n"
                 "| Clamp | (a2) cutoff, all +10 | (a2) cutoff, all -10 | k (knob units added), all +10 |\n"
                 "|---|---|---|---|\n";
    for (const auto clamp : {Clamp::none, Clamp::perControl, Clamp::sum20, Clamp::sum10})
    {
        const CutoffInputs up{.filter = 10.0f, .control = 10.0f, .filterMod = 160.0f};
        const CutoffInputs down{.filter = 10.0f, .control = 10.0f, .filterMod = -160.0f};
        const ResonanceInputs r{.filter = 10.0f, .control = 10.0f, .filterMod = 160.0f};
        double k = 0.0;
        switch (clamp)
        {
        case Clamp::none:
            k = static_cast<double>(resonanceKUnclamped(r));
            break;
        case Clamp::perControl:
            k = static_cast<double>(resonanceK(ResonanceModel::knobSum, r));
            break;
        case Clamp::sum20:
            k = 0.6 * 20.0;
            break;
        case Clamp::sum10:
            k = static_cast<double>(resonanceK(ResonanceModel::knobSumTo10, r));
            break;
        }
        std::cout << "| " << clampName(clamp) << " | "
                  << fixed(static_cast<double>(cutoffOctaves(CutoffModel::knobSumOctave, clamp, up)), 1) << " | "
                  << fixed(static_cast<double>(cutoffOctaves(CutoffModel::knobSumOctave, clamp, down)), 1) << " | "
                  << fixed(k, 1) << " |\n";
    }

    // The filter itself under that modulation: a full-scale saw, the cutoff
    // swept by an LFO at +/-10 units around A 5 + Control 5, k from both
    // resonances at 10 plus a +10 mod envelope, fed at each grid point; and
    // the double-precision reference (ladder_reference.hpp) fed the same g,
    // interpolated across each segment as the filter does.
    std::cout << "\n**The filter under it**: a full-scale 110 Hz saw, 2 s at 44.1 kHz, (a2), A 5 + Control 5 with an "
                 "LFO at +/-10 on Cutoff A, both Resonances at 10 with a +10 envelope on Resonance A (k: none 18, "
                 "each clamped 12, the sum to [0, 10] 6; the cutoff's clamp in the first column), Warm on, fed at each "
                 "grid point. Peak output of LadderFilter, "
                 "whether every sample is finite, the peak of the double-precision reference fed the same g "
                 "sequence, and the reference's peak with g computed every sample instead of interpolated; then the "
                 "same with the cutoff held at the LFO's extremes:\n\n"
                 "| Clamp | k | LFO | Peak | Finite | Reference's peak | Reference, g every sample |\n"
                 "|---|---|---|---|---|---|---|\n";
    // (cutoff clamp, resonance model): k 18 unclamped, 12 each clamped, 6
    // with the resonances' sum clamped (with the cutoff clamped per control
    // too, to separate the resonance from the sweep), 6 with both sums to 10.
    struct Case
    {
        Clamp clamp;
        float k;
    };
    const ResonanceInputs r{.filter = 10.0f, .control = 10.0f, .filterMod = 10.0f};
    for (const auto [clamp, k] : {
             Case{.clamp = Clamp::none, .k = resonanceKUnclamped(r)},
             Case{.clamp = Clamp::perControl, .k = resonanceK(ResonanceModel::knobSum, r)},
             Case{.clamp = Clamp::perControl, .k = resonanceK(ResonanceModel::knobSumTo10, r)},
             Case{.clamp = Clamp::sum10, .k = resonanceK(ResonanceModel::knobSumTo10, r)},
         })
    {
        for (const double lfoHz : {0.0, 0.5, 5.0})
        {
            for (const double held : lfoHz > 0.0 ? std::vector<double>{0.0} : std::vector<double>{-1.0, 1.0})
            {
                dsp::LadderFilter f;
                f.setResonance(k);
                f.setCutoff(lcdLaw.hz(0.0f));
                f.prepare({.sampleRate = 44100.0f, .maxBlockSize = grid, .numChannels = 1});
                ladder::ReferenceLadder<ladder::ReferenceTanh> reference;
                ladder::ReferenceLadder<ladder::ReferenceTanh> perSample;
                double perSamplePeak = 0.0;
                constexpr std::size_t n = 88200;
                Buffer buffer{1, grid};
                double phase = 0.0;
                double peak = 0.0;
                double referencePeak = 0.0;
                bool finite = true;
                const auto gainAt = [&](std::size_t sample)
                {
                    const double t = static_cast<double>(sample) / rate;
                    const auto lfo =
                        static_cast<float>(lfoHz > 0.0 ? 10.0 * std::sin(2.0 * pi * lfoHz * t) : 10.0 * held);
                    return cutoffOctaves(CutoffModel::knobSumOctave, clamp,
                                         {.filter = 5.0f, .control = 5.0f, .filterMod = lfo});
                };
                const auto tanOf = [&](float octaves)
                {
                    const double hz = std::clamp(static_cast<double>(std::exp2(std::clamp(octaves, -40.0f, 40.0f))),
                                                 1.0, 0.495 * rate);
                    return std::tan(pi * hz / rate);
                };
                f.setCutoffModulation(gainAt(0) - lcdLaw.log2HzAtZero);
                f.prepare({.sampleRate = 44100.0f, .maxBlockSize = grid, .numChannels = 1});
                double gStart = tanOf(gainAt(0));
                for (std::size_t start = 0; start < n; start += grid)
                {
                    const float octaves = gainAt(start + grid);
                    f.setCutoffModulation(octaves - lcdLaw.log2HzAtZero);
                    const double gEnd = tanOf(octaves);
                    for (std::size_t i = 0; i < grid; ++i)
                    {
                        const auto v = static_cast<float>((2.0 * phase) - 1.0);
                        buffer.channel(0)[i] = v;
                        phase += 110.0 / rate;
                        phase -= std::floor(phase);
                        const double g =
                            gStart + ((gEnd - gStart) * static_cast<double>(i) / static_cast<double>(grid));
                        referencePeak =
                            std::max(referencePeak,
                                     std::fabs(reference.process(static_cast<double>(v), g, static_cast<double>(k))));
                        perSamplePeak = std::max(
                            perSamplePeak, std::fabs(perSample.process(static_cast<double>(v), tanOf(gainAt(start + i)),
                                                                       static_cast<double>(k))));
                    }
                    gStart = gEnd;
                    f.process(buffer.block());
                    for (const float v : buffer.channel(0))
                    {
                        finite = finite && std::isfinite(v);
                        peak = std::max(peak, std::fabs(static_cast<double>(v)));
                    }
                }
                CHECK(finite);
                std::string lfoText = held < 0.0 ? "held at its bottom" : "held at its top";
                if (lfoHz > 0.0)
                {
                    lfoText = fixed(lfoHz, 1) + " Hz";
                }
                std::cout << "| " << clampName(clamp) << " | " << fixed(static_cast<double>(k), 0) << " | " << lfoText
                          << " | " << fixed(peak, 2) << " | " << (finite ? "yes" : "**no**") << " | "
                          << fixed(referencePeak, 2) << " | " << fixed(perSamplePeak, 2) << " |\n";
            }
        }
    }
}

// --- 6. Resonance ----------------------------------------------------------------------

/// True if a 1e-15 impulse grows at loop gain k (1 kHz, 48 kHz, Warm on):
/// the RMS of the last 10 cycles of 400 against cycles 10 to 20.
bool grows(float k)
{
    constexpr double rate = 48000.0;
    auto f = settledFilter(rate, 1000.0, k);
    std::vector<float> x(19200, 0.0f);
    x[0] = 1e-15f;
    const auto y = render(f, x);
    const auto rms = [&](std::size_t from, std::size_t to)
    {
        double s = 0.0;
        for (std::size_t i = from; i < to; ++i)
        {
            s += static_cast<double>(y[i]) * static_cast<double>(y[i]);
        }
        return std::sqrt(s / static_cast<double>(to - from));
    };
    return rms(18720, 19200) > rms(480, 960);
}

void reportResonance()
{
    std::cout << "\n### Resonance: where self-oscillation starts\n\n"
                 "F5's law k = 0.6 per knob unit on whatever each model combines; the ladder self-oscillates from "
                 "k = 4 exactly (filters.md). The Resonance A position where it starts, with the Filter Control's "
                 "Resonance at 0 / 5 / 10 (- : never; 0: already at A = 0). Checked on LadderFilter: a 1e-15 "
                 "impulse at 1 kHz decays 0.05 units below the position and grows 0.05 above (k +/- 0.03: e^-1.2 and "
                 "e^+1.2 over 400 cycles).\n\n"
                 "| Model | Control 0 | Control 5 | Control 10 |\n|---|---|---|---|\n";
    for (const auto m : resonanceModels)
    {
        std::cout << "| " << resonanceName(m);
        for (const float c : {0.0f, 5.0f, 10.0f})
        {
            float lo = 0.0f;
            float hi = 10.0f;
            const auto kAt = [&](float a)
            {
                return resonanceK(m, {.filter = a, .control = c});
            };
            std::string cell;
            if (kAt(0.0f) >= 4.0f)
            {
                cell = "0";
                CHECK(grows(kAt(0.0f) + 0.03f));
            }
            else if (kAt(10.0f) < 4.0f)
            {
                cell = "-";
                CHECK_FALSE(grows(kAt(10.0f)));
            }
            else
            {
                for (int i = 0; i < 40; ++i)
                {
                    const float mid = 0.5f * (lo + hi);
                    (kAt(mid) < 4.0f ? lo : hi) = mid;
                }
                const float at = 0.5f * (lo + hi);
                cell = fixed(static_cast<double>(at), 2);
                CHECK_FALSE(grows(kAt(at - 0.05f)));
                CHECK(grows(kAt(at + 0.05f)));
            }
            std::cout << " | " << cell;
        }
        std::cout << " |\n";
    }

    std::cout << "\n### Resonance: the Filter Control's Resonance at 0 and at 10\n\n"
                 "A filter at Resonance 0, 5, 10; each cell k with the Filter Control at 0 -> at 10. Then the "
                 "self-oscillation at that k (440 Hz, 48 kHz, Warm on): level in peak-equivalent dBFS / pitch in "
                 "cents from the cutoff; decays below k 4.\n\n"
                 "| Model | Filter 0 | Filter 5 | Filter 10 |\n|---|---|---|---|\n";
    for (const auto m : resonanceModels)
    {
        std::cout << "| " << resonanceName(m);
        for (const float a : {0.0f, 5.0f, 10.0f})
        {
            const float k0 = resonanceK(m, {.filter = a, .control = 0.0f});
            const float k10 = resonanceK(m, {.filter = a, .control = 10.0f});
            std::cout << " | k " << fixed(static_cast<double>(k0), 1) << " -> " << fixed(static_cast<double>(k10), 1);
        }
        std::cout << " |\n";
    }
    std::cout << "\n| k | Warm on: level / cents | Warm off: level / cents |\n|---|---|---|\n";
    for (const float k : {4.2f, 5.4f, 6.0f, 7.2f, 9.0f, 10.8f, 12.0f, 18.0f})
    {
        std::cout << "| " << fixed(static_cast<double>(k), 1);
        for (const bool warm : {true, false})
        {
            double level = 0.0;
            const double hz = selfOscillationHz(48000.0, 440.0, k, warm, &level);
            std::cout << " | " << fixed(level, 1) << " dBFS / " << fixed(centsBetween(hz, 440.0), 1);
        }
        std::cout << " |\n";
    }
}

// --- 7. Feeding the cutoff: the 10 ms ramp, a voice ramp, both, neither -------------------

std::string_view feedName(Feed f)
{
    switch (f)
    {
    case Feed::filterRamp:
        return "W1: setCutoff(knob sum), the filter's ramp";
    case Feed::voiceRamp:
        return "W2 (built): the knobs' global ramps in the voice's sum, all into the modulation input";
    case Feed::bothRamps:
        return "W3: the voice's sum into setCutoff (ramped twice)";
    case Feed::noRamp:
        return "W4: no ramp, into the modulation input";
    }
    return "?";
}

constexpr std::array<Feed, 4> feeds{Feed::filterRamp, Feed::voiceRamp, Feed::bothRamps, Feed::noRamp};

std::vector<float> saw(double hz, double rate, std::size_t n, double amplitude)
{
    std::vector<float> x(n);
    double phase = 0.0;
    for (auto& v : x)
    {
        v = static_cast<float>(amplitude * ((2.0 * phase) - 1.0));
        phase += hz / rate;
        phase -= std::floor(phase);
    }
    return x;
}

/// Stage 1's resonance for the feeding measurements, k 3.6, as knob units
/// through the law: Resonance A at 6.
Knobs feedingKnobs(Signal cutoff, Signal controlCutoff)
{
    Knobs knobs;
    knobs.cutoff = std::move(cutoff);
    knobs.controlCutoff = std::move(controlCutoff);
    knobs.resonance = constant(6.0f);
    knobs.controlResonance = constant(0.0f);
    return knobs;
}

void reportFeeding()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t at = 24576; // a grid point and a host-block boundary
    constexpr std::size_t n = 49152;

    // A knob step: the Filter Control from 3 to 6 with A at 5, (a2): 1,169 Hz
    // to 9,354 Hz, three octaves (filters.md's step was 500 Hz to 4 kHz).
    std::vector<float> step(n, 0.0f);
    std::fill(step.begin() + at, step.end(), 1.0f);
    const double unitStep = energyAbove1k(step, at + 32, rate);
    const auto sineIn = sine(150.0, rate, n, 0.25);
    const Signal stepC = [&](std::size_t i)
    {
        return i < at ? 3.0f : 6.0f;
    };
    const Voice still{};
    std::cout << "\n### Feeding the sum: a Filter Control step (48 kHz, (a2), A 5, Control 3 -> 6: 1,169 -> 9,354 "
                 "Hz)\n\n"
                 "To 90%: the time from the step until log2 of the cutoff has moved 90% of the way. From W1: the "
                 "largest distance of the cutoff from W1's at any grid point. Click: LP24 at k 3.6, a 150 Hz sine at "
                 "-12 dBFS, energy above 1 kHz in a Hann 4096 centred 32 samples after the step, dB re an instant unit "
                 "step (filters.md's measure; steady: no step).\n\n"
                 "| Feed | To 90% | From W1 | Click |\n|---|---|---|---|\n";
    const auto stepKnobs = feedingKnobs(constant(5.0f), stepC);
    const auto reference = runOne({.rate = rate, .feed = Feed::filterRamp}, stepKnobs, still, sineIn);
    for (const auto feed : feeds)
    {
        const auto r = runOne({.rate = rate, .feed = feed}, stepKnobs, still, sineIn);
        const double from = r.log2Cutoff[(at / grid) - 1];
        const double to = r.log2Cutoff.back();
        std::size_t reach = r.log2Cutoff.size();
        double fromW1 = 0.0;
        for (std::size_t i = at / grid; i < r.log2Cutoff.size(); ++i)
        {
            if (reach == r.log2Cutoff.size() && r.log2Cutoff[i] >= from + (0.9 * (to - from)))
            {
                reach = i;
            }
            fromW1 = std::max(fromW1, 1200.0 * std::fabs(r.log2Cutoff[i] - reference.log2Cutoff[i]));
        }
        const double ms = 1000.0 * static_cast<double>(((reach + 1) * grid) - at) / rate;
        std::cout << "| " << feedName(feed) << " | " << fixed(ms, 2) << " ms | " << fixed(fromW1, 3) << " cents | "
                  << fixed(10.0 * std::log10(energyAbove1k(r.y, at + 32, rate) / unitStep), 1) << " dB |\n";
    }
    {
        const auto r = runOne({.rate = rate}, feedingKnobs(constant(5.0f), constant(6.0f)), still, sineIn);
        std::cout << "| steady (Control at 6 throughout) | | | "
                  << fixed(10.0 * std::log10(energyAbove1k(r.y, at + 32, rate) / unitStep), 1) << " dB |\n";
    }

    // A 7-bit MIDI CC sweep of the Filter Control, 0 -> 10 over 2 s, events at
    // host blocks of 512.
    constexpr std::size_t sweepN = 96000;
    const auto sawIn = saw(110.0, rate, sweepN, 0.5);
    const Signal cc = [&](std::size_t i)
    {
        // The value sent at the start of each host block of 512.
        const std::size_t blockStart = (i / 512) * 512;
        const double t = std::min(1.0, static_cast<double>(blockStart) / static_cast<double>(sweepN));
        return static_cast<float>(std::round(127.0 * t) / 127.0 * 10.0);
    };
    // The ideal: the unquantised knob at each grid point's segment end.
    const auto idealLog2 = [&](std::size_t gridIndex)
    {
        const double t = std::min(1.0, static_cast<double>((gridIndex + 1) * grid) / static_cast<double>(sweepN));
        return static_cast<double>(cutoffOctaves(CutoffModel::knobSumOctave, Clamp::perControl,
                                                 {.filter = 3.0f, .control = static_cast<float>(10.0 * t)}));
    };
    std::cout << "\n### Feeding the sum: a 7-bit MIDI CC sweep of the Filter Control (0 -> 10 in 2 s, 128 steps of "
                 "0.079 units = 94 cents under (a2), events at host blocks of 512; A 3, so 36.5 Hz to the ceiling)\n\n"
                 "The cutoff at each grid point, over the part of the sweep below the ceiling. Largest step: between "
                 "neighbouring grid points (the unquantised knob moves 3.2 cents per grid point). Roughness: the RMS "
                 "of the second difference, which a smooth sweep keeps near 0 and a staircase does not. Lag: the "
                 "mean distance behind the unquantised knob, in cents and in ms at the sweep's 6,000 cents per "
                 "second.\n\n"
                 "| Feed | Largest step | Roughness | Lag |\n|---|---|---|---|\n";
    for (const auto feed : feeds)
    {
        const auto r = runOne({.rate = rate, .feed = feed}, feedingKnobs(constant(3.0f), cc), still, sawIn);
        const double top = std::log2(0.495 * rate) - 0.01;
        double largest = 0.0;
        double rough = 0.0;
        double lag = 0.0;
        std::size_t count = 0;
        for (std::size_t i = 2; i < r.log2Cutoff.size(); ++i)
        {
            if (r.log2Cutoff[i] >= top || idealLog2(i) >= top)
            {
                break;
            }
            largest = std::max(largest, 1200.0 * std::fabs(r.log2Cutoff[i] - r.log2Cutoff[i - 1]));
            const double second = 1200.0 * (r.log2Cutoff[i] - (2.0 * r.log2Cutoff[i - 1]) + r.log2Cutoff[i - 2]);
            rough += second * second;
            lag += 1200.0 * (idealLog2(i) - r.log2Cutoff[i]);
            ++count;
        }
        const auto c = static_cast<double>(count);
        std::cout << "| " << feedName(feed) << " | " << fixed(largest, 1) << " cents | "
                  << fixed(std::sqrt(rough / c), 2) << " cents | " << fixed(lag / c, 1) << " cents ("
                  << fixed(lag / c / 6.0, 1) << " ms) |\n";
    }

    // The clamp across a ramp: A 2 -> 9 with +3 of modulation on Cutoff A and
    // the Filter Control at 2. Each control clamped: the sum goes 7 -> 12.
    // W1 can only clamp against the knob's target, so while the filter ramps
    // the control part, the modulation part is already the target's.
    const Signal stepA = [&](std::size_t i)
    {
        return i < at ? 2.0f : 9.0f;
    };
    const Voice modulated{.cutoffModulation = constant(3.0f)};
    std::cout << "\n### Feeding the sum: the clamp across a ramp (48 kHz, (a2), A 2 -> 9 with +3 of modulation on "
                 "Cutoff A, Control 2: the clamped sum 7 -> 12)\n\n"
                 "Exact: the clamp applied to the ramped knob, clamp(A(t) + 3) + 2. W1 clamps against the target, so "
                 "its modulation part steps from +3 to +1 at once while the knob ramps from 2. The largest distance "
                 "from exact during the ramp, and the click (as above).\n\n"
                 "| Feed | From exact | Click |\n|---|---|---|\n";
    const auto exact = [&]
    {
        // The knob's 10 ms ramp from the step, in closed form, at each segment end.
        std::vector<double> e;
        for (std::size_t g = 0; g < n / grid; ++g)
        {
            const auto end = static_cast<double>((g + 1) * grid);
            const double t = std::clamp((end - static_cast<double>(at)) / 480.0, 0.0, 1.0);
            const double a = 2.0 + (7.0 * t);
            e.push_back(std::log2(static_cast<double>(dsp::filterControlLaw.anchorHz)) + std::min(10.0, a + 3.0) + 2.0);
        }
        return e;
    }();
    for (const auto feed : {Feed::filterRamp, Feed::voiceRamp})
    {
        const auto r = runOne({.rate = rate, .feed = feed}, feedingKnobs(stepA, constant(2.0f)), modulated, sineIn);
        double worst = 0.0;
        for (std::size_t i = 0; i < r.log2Cutoff.size(); ++i)
        {
            worst = std::max(worst, 1200.0 * std::fabs(r.log2Cutoff[i] - std::min(exact[i], std::log2(0.495 * rate))));
        }
        std::cout << "| " << feedName(feed) << " | " << fixed(worst, 1) << " cents | "
                  << fixed(10.0 * std::log10(energyAbove1k(r.y, at + 32, rate) / unitStep), 1) << " dB |\n";
    }

    // Block sizes: the step above with its event one sample after a grid
    // point, in host blocks of 1, 37, 4096.
    std::cout << "\n**Block sizes** (the Filter Control step with its event one sample after a grid point; W1 applies "
                 "it at the next grid point, W2's global ramp starts at the event's sample; host blocks of 1 and 37 "
                 "against 4096):\n\n| Feed | 1 | 37 |\n|---|---|---|\n";
    for (const auto feed : {Feed::filterRamp, Feed::voiceRamp})
    {
        const Signal event = [&](std::size_t i)
        {
            return i <= at ? 3.0f : 6.0f;
        };
        const auto knobs = feedingKnobs(constant(5.0f), event);
        const auto big = runOne({.rate = rate, .feed = feed, .hostBlock = 4096}, knobs, still, sineIn);
        std::cout << "| " << feedName(feed);
        for (const std::size_t block : {std::size_t{1}, std::size_t{37}})
        {
            const auto small = runOne({.rate = rate, .feed = feed, .hostBlock = block}, knobs, still, sineIn);
            const bool same =
                std::ranges::equal(small.y, big.y, [](float p, float q)
                                   { return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q); });
            CHECK(same);
            std::cout << " | " << (same ? "identical" : "**differs**");
        }
        std::cout << " |\n";
    }
}

// --- 8. Feeding k: the filter's resonance ramp at each grid point, or a modulation input ---

/// The k the filter uses at the end of each of the voice's segments when the
/// voice calls setResonance() at each grid point: LadderFilter's own
/// LinearSmoother (10 ms, restarted by each new target, advanced a sample at
/// a time), replayed on the k the voice fed.
std::vector<double> rampedK(const VoiceRun& r, double rate)
{
    dsp::LinearSmoother s;
    s.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = 4096, .numChannels = 1});
    s.setRampTime(dsp::LadderFilter::rampSeconds);
    s.setCurrentAndTarget(r.noteOnK);
    std::vector<double> out;
    for (const float k : r.k)
    {
        s.setTarget(k);
        s.skip(grid);
        out.push_back(static_cast<double>(s.currentValue()));
    }
    return out;
}

void reportResonanceFeeding()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 49152;
    constexpr std::size_t at = 24576; // a grid point and a host-block boundary
    const auto sineIn = sine(150.0, rate, n, 0.25);
    std::vector<float> step(n, 0.0f);
    std::fill(step.begin() + at, step.end(), 1.0f);
    const double unitStep = energyAbove1k(step, at + 32, rate);
    std::cout << "\n### Feeding k: setResonance() at each grid point, or the resonance modulation input (48 kHz)\n\n"
                 "The voice computes k at each grid point for the segment's end (the knobs' ramps and its own "
                 "modulation, clamped and added). Fed through setResonance(), each call restarts the filter's 10 ms "
                 "ramp from where it is (k replayed from the filter's own smoother). Fed through "
                 "setResonanceModulation() (built), the filter reads it at the next grid point and runs k linearly "
                 "to it: at the grid points, the voice's k itself. 'The filter's ramp' is setResonance() once at the "
                 "event, the knob's own 10 ms ramp (no per-voice modulation possible). Compared against the knob's "
                 "ramp in closed form.\n\n";

    // A step of the Filter Control's Resonance, 1 -> 4 with Resonance A at 2
    // (k 1.8 -> 3.6), the cutoff at A 5 + Control 3 (1,169 Hz).
    const Knobs stepKnobs{
        .cutoff = constant(5.0f),
        .controlCutoff = constant(3.0f),
        .resonance = constant(2.0f),
        .controlResonance = [](std::size_t i) { return i < at ? 1.0f : 4.0f; },
    };
    std::cout << "**A step of the Filter Control's Resonance, 1 -> 4** (Resonance A 2: k 1.8 -> 3.6; Cutoff A 5 + "
                 "Control 3, 1,169 Hz; a 150 Hz sine at -12 dBFS, LP24). To 90%: from the step until k has moved "
                 "90%. From the knob's ramp: the largest distance in k at any grid point. Click: energy above 1 kHz "
                 "around the step, dB re an instant unit step.\n\n"
                 "| Feed | To 90% | From the knob's ramp | Click |\n|---|---|---|---|\n";
    const Voice still{};
    const auto idealStepK = [&](std::size_t gridIndex)
    {
        const auto end = static_cast<double>((gridIndex + 1) * grid);
        return 0.6 * (3.0 + (3.0 * std::clamp((end - static_cast<double>(at)) / 480.0, 0.0, 1.0)));
    };
    const auto stepRow = [&](const std::string& name, const VoiceRun& r, const std::vector<double>& k)
    {
        std::size_t reach = k.size();
        double from = 0.0;
        for (std::size_t i = 0; i < k.size(); ++i)
        {
            if (i >= at / grid && reach == k.size() && k[i] >= 1.8 + (0.9 * 1.8))
            {
                reach = i;
            }
            from = std::max(from, std::fabs(k[i] - idealStepK(i)));
        }
        const double ms = 1000.0 * static_cast<double>(((reach + 1) * grid) - at) / rate;
        std::cout << "| " << name << " | " << fixed(ms, 2) << " ms | " << fixed(from, 4) << " | "
                  << fixed(10.0 * std::log10(energyAbove1k(r.y, at + 32, rate) / unitStep), 1) << " dB |\n";
    };
    {
        const auto r = runOne({.rate = rate, .resonanceFeed = ResonanceFeed::filterRamp}, stepKnobs, still, sineIn);
        stepRow("the filter's ramp: setResonance() at the event", r, rampedK(r, rate));
    }
    {
        const auto r = runOne({.rate = rate, .resonanceFeed = ResonanceFeed::perGridPoint}, stepKnobs, still, sineIn);
        stepRow("setResonance() at each grid point", r, rampedK(r, rate));
    }
    {
        const auto r = runOne({.rate = rate, .resonanceFeed = ResonanceFeed::modulation}, stepKnobs, still, sineIn);
        stepRow("setResonanceModulation() at each grid point (built)", r, std::vector<double>(r.k.begin(), r.k.end()));
    }
    {
        const auto r = runOne({.rate = rate},
                              {
                                  .cutoff = constant(5.0f),
                                  .controlCutoff = constant(3.0f),
                                  .resonance = constant(2.0f),
                                  .controlResonance = constant(4.0f),
                              },
                              still, sineIn);
        std::cout << "| steady (Control at 4 throughout) | | | "
                  << fixed(10.0 * std::log10(energyAbove1k(r.y, at + 32, rate) / unitStep), 1) << " dB |\n";
    }

    // A 7-bit MIDI CC sweep of the Filter Control's Resonance, 0 -> 10 in 2 s
    // (k 0 -> 6), events at host blocks of 512.
    constexpr std::size_t sweepN = 96000;
    const auto sawIn = saw(110.0, rate, sweepN, 0.5);
    const Knobs sweepKnobs{
        .cutoff = constant(5.0f),
        .controlCutoff = constant(3.0f),
        .resonance = constant(0.0f),
        .controlResonance =
            [](std::size_t i)
        {
            const std::size_t blockStart = (i / 512) * 512;
            const double t = std::min(1.0, static_cast<double>(blockStart) / static_cast<double>(sweepN));
            return static_cast<float>(std::round(127.0 * t) / 127.0 * 10.0);
        },
    };
    const auto idealK = [&](std::size_t gridIndex)
    {
        const double t = std::min(1.0, static_cast<double>((gridIndex + 1) * grid) / static_cast<double>(sweepN));
        return 0.6 * 10.0 * t;
    };
    std::cout << "\n**A 7-bit MIDI CC sweep of the Filter Control's Resonance**, 0 -> 10 in 2 s (k 0 -> 6, 128 steps "
                 "of 0.047 in k), events at host blocks of 512; Resonance A 0. The k at each grid point: the largest "
                 "step between grid points (the unquantised knob moves 0.0020 per grid point), roughness (RMS second "
                 "difference), and the mean lag behind the unquantised knob, in k and in ms (3 k per second).\n\n"
                 "| Feed | Largest step | Roughness | Lag |\n|---|---|---|---|\n";
    const auto sweepRow = [&](const std::string& name, const std::vector<double>& k)
    {
        double largest = 0.0;
        double rough = 0.0;
        double lag = 0.0;
        std::size_t count = 0;
        for (std::size_t i = 2; i < k.size(); ++i)
        {
            largest = std::max(largest, std::fabs(k[i] - k[i - 1]));
            const double second = k[i] - (2.0 * k[i - 1]) + k[i - 2];
            rough += second * second;
            lag += idealK(i) - k[i];
            ++count;
        }
        const auto c = static_cast<double>(count);
        std::cout << "| " << name << " | " << fixed(largest, 4) << " | " << fixed(std::sqrt(rough / c), 5) << " | "
                  << fixed(lag / c, 4) << " (" << fixed(1000.0 * lag / c / 3.0, 1) << " ms) |\n";
    };
    {
        const auto r = runOne({.rate = rate, .resonanceFeed = ResonanceFeed::perGridPoint}, sweepKnobs, still, sawIn);
        sweepRow("setResonance() at each grid point", rampedK(r, rate));
        const auto m = runOne({.rate = rate, .resonanceFeed = ResonanceFeed::modulation}, sweepKnobs, still, sawIn);
        sweepRow("setResonanceModulation() at each grid point (built)", std::vector<double>(m.k.begin(), m.k.end()));
    }

    // An LFO on Resonance A: +/-2 units around 3 (k 0.6 to 3.0), per voice.
    std::cout << "\n**An LFO on Resonance A**, +/-2 units around Resonance A 3 (k 1.8 +/- 1.2), a sine at 2, 5, and "
                 "20 Hz, over 1 s after 0.2 s: the k swing at the LFO's frequency (least squares) against the "
                 "voice's k, and its lag in ms.\n\n"
                 "| Feed | 2 Hz: swing / lag | 5 Hz | 20 Hz |\n|---|---|---|---|\n";
    const auto fit = [&](const std::vector<double>& k, double hz)
    {
        // Amplitude and phase of k at hz over grid points 0.2 s to 1.2 s.
        double sc = 0.0;
        double ss = 0.0;
        double count = 0.0;
        for (std::size_t i = 0; i < k.size(); ++i)
        {
            const double t = static_cast<double>((i + 1) * grid) / rate;
            if (t < 0.2 || t >= 1.2)
            {
                continue;
            }
            sc += k[i] * std::cos(2.0 * pi * hz * t);
            ss += k[i] * std::sin(2.0 * pi * hz * t);
            count += 1.0;
        }
        return std::pair{2.0 * std::hypot(sc, ss) / count, std::atan2(sc, ss)};
    };
    const std::vector<float> lfoIn = saw(110.0, rate, 60000, 0.5);
    std::string ramped = "| setResonance() at each grid point";
    std::string knobRow = "| setResonanceModulation() at each grid point (built): the voice's k";
    for (const double hz : {2.0, 5.0, 20.0})
    {
        const Voice lfo{
            .resonanceModulation = [hz](std::size_t i)
            { return static_cast<float>(2.0 * std::sin(2.0 * pi * hz * static_cast<double>(i) / rate)); },
        };
        const Knobs knobs{.cutoff = constant(5.0f), .controlCutoff = constant(3.0f), .resonance = constant(3.0f)};
        const auto r = runOne({.rate = rate, .resonanceFeed = ResonanceFeed::perGridPoint}, knobs, lfo, lfoIn);
        const auto [a0, p0] = fit(std::vector<double>(r.k.begin(), r.k.end()), hz);
        const auto [a1, p1] = fit(rampedK(r, rate), hz);
        const double lag0 = 1000.0 * (-p0) / (2.0 * pi * hz);
        knobRow += " | " + fixed(a0, 3) + " (" + fixed(a0 / 1.2, 3) + "x) / " +
                   fixed(std::fabs(lag0) < 0.05 ? 0.0 : lag0, 1) + " ms";
        ramped += " | " + fixed(a1 / a0, 3) + "x / " + fixed(1000.0 * (p0 - p1) / (2.0 * pi * hz), 1) + " ms";
    }
    std::cout << knobRow << " |\n" << ramped << " |\n";
}

} // namespace

TEST_CASE("Filter Control report: the built law and the candidates on LadderFilter", "[.][filter-control-report]")
{
    std::cout << "\n## Filter Control report\n";
    reportEvidence();
    reportGrid();
    reportRange();
    reportKeytrack();
    reportClamps();
    reportResonance();
    reportFeeding();
    reportResonanceFeeding();
}

TEST_CASE("Filter Control report: feeding k only", "[.][filter-control-resonance-feeding]")
{
    reportResonanceFeeding();
}
