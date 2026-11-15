// The keytracking report (docs/dsp/keytracking.md): measures keytracking as
// built (dsp::keytrackOctaves, the voice's sounding pitch, reference A-1),
// fed into LadderFilter as the voice will feed it (keytrack_support.hpp):
//
// - the law and the reference: the self-oscillation pitch at keytrack 0, 5,
//   and 10 across the keyboard;
// - intervals at keytrack 10: the self-oscillation pitch from C1 to C7 at
//   44.1 and 192 kHz, Warm on and off, at the threshold and at k 6 and 8;
// - near the clamp, and where LadderFilter's solver fails near Nyquist;
// - the master tune, a portamento glide, and a bend: how closely a
//   self-oscillating filter follows the oscillators, against the key alone
//   and against the other ways of feeding it (once per host block, or at
//   each grid point with the pitch there), and the zipper of each;
// - a legato note step at high resonance: the click against a unit step;
// - bit-identity across block sizes 1, 37, 4096; subnormals; allocation.
//
// The candidate models it was chosen from (note number, linear in Hz, a
// c(k) correction, ramped note steps) are at commit 3948d99
// (keytrack_candidates.hpp).
//
// Hidden from the default run; run it in an optimised build (about 20 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[keytrack-report]"
//
// CPU: the "[keytrack]" benchmarks (tools/bench.sh "[keytrack]").

#include "keytrack_support.hpp"

#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/math/pitch.hpp>

#include <anoesis/test/measure.hpp>
#include <anoesis/test/signals.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::keytrack;
namespace dsp = anoesis::dsp;

namespace
{

constexpr double pi = std::numbers::pi;

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

std::string feedName(Feed f, std::size_t hostBlock)
{
    switch (f)
    {
    case Feed::aligned:
        return "each grid point, the pitch at its segment's end (as built)";
    case Feed::gridStart:
        return "each grid point, the pitch there";
    case Feed::perBlock:
        return "once per block of " + std::to_string(hostBlock);
    }
    return "?";
}

PitchTrack held(double note)
{
    return [=](std::size_t)
    {
        return note;
    };
}

/// Long enough for the oscillation to grow from the impulse and settle: the
/// linear loop grows by e every 1/((k^(1/4) cos 45 deg - 1) 2 pi f) seconds
/// (filter-theory.md), and it takes about ten of those.
double secondsFor(float k, double hz)
{
    const double growth = (std::pow(static_cast<double>(k), 0.25) * std::numbers::sqrt2 / 2.0) - 1.0;
    return std::max(2.5, 0.5 + (12.0 / (growth * 2.0 * pi * hz)));
}

/// The strongest spectral peak in Hz (Hann, parabolic on the log power), for
/// tones near Nyquist, where zero crossings can't resolve the period.
double peakHz(std::span<const float> x, double rate)
{
    static const Window window = hannWindow(16384);
    const auto spectrum = powerSpectrum(x.subspan(x.size() - 16384), rate, window);
    const auto& p = spectrum.power;
    const auto top = static_cast<std::size_t>(std::ranges::max_element(p) - p.begin());
    if (top == 0 || top + 1 >= p.size())
    {
        return spectrum.frequencyOf(top);
    }
    const double a = std::log(p[top - 1]);
    const double b = std::log(p[top]);
    const double c = std::log(p[top + 1]);
    const double offset = 0.5 * (a - c) / (a - (2.0 * b) + c);
    return spectrum.frequencyOf(top) + (offset * (spectrum.frequencyOf(1) - spectrum.frequencyOf(0)));
}

/// The self-oscillation pitch in Hz over the last 0.5 s, or NaN.
double selfOscillationHz(const Setup& s, const PitchTrack& pitch, double seconds)
{
    const auto r = run(s, kick(static_cast<std::size_t>(seconds * s.rate)), pitch);
    const auto tail = std::span{r.y}.last(static_cast<std::size_t>(0.5 * s.rate));
    double sum = 0.0;
    for (const float v : tail)
    {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    if (!(sum > 1e-12) || !std::isfinite(sum))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return measureFrequency(tail, s.rate);
}

// --- The law and the reference ------------------------------------------------------

void reportLaw()
{
    std::cout << "\n### Keytrack 0, 5, 10 against the reference (48 kHz, k 4.4, Warm on)\n\n"
                 "The self-oscillation pitch in Hz at each note, the cutoff knob at 13.75 Hz (A-1's frequency), so "
                 "keytrack 10 puts the cutoff at the note's own frequency. Expected: 13.75 x 2^(keytrack/10 x "
                 "(note - 9)/12), 35.8 cents flat (k 4.4).\n\n"
                 "| Keytrack | A-1 | C1 | C3 | A4 | C5 | C7 |\n|---|---|---|---|---|---|---|\n";
    for (const double amount : {0.0, 0.5, 1.0})
    {
        std::cout << "| " << fixed(10.0 * amount, 0);
        for (const double note : {9.0, 24.0, 48.0, 69.0, 72.0, 96.0})
        {
            const Setup s{.amount = amount};
            const double expected = 13.75 * std::exp2(amount * (note - 9.0) / 12.0);
            const double hz = selfOscillationHz(s, held(note), secondsFor(s.k, expected));
            std::cout << " | " << fixed(hz, hz < 100.0 ? 2 : 1);
            CHECK(std::fabs(centsBetween(hz, expected) + 35.8) < 1.0);
        }
        std::cout << " |\n";
    }
}

// --- Intervals at keytrack 10 --------------------------------------------------------

void reportIntervals()
{
    constexpr std::array<double, 7> notes{24.0, 36.0, 48.0, 60.0, 72.0, 84.0, 96.0};
    std::cout << "\n### Intervals at keytrack 10: the self-oscillation pitch, cents from each note's 12-TET "
                 "frequency\n\n"
                 "The cutoff knob at 13.75 Hz, so the cutoff is the note's frequency. Spread: the largest minus the "
                 "smallest, the error in any interval from C1 to C7.\n\n"
                 "| Saturator | Rate | k | C1 | C2 | C3 | C4 | C5 | C6 | C7 | Spread |\n"
                 "|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const bool warm : {true, false})
    {
        for (const double rate : {44100.0, 192000.0})
        {
            for (const float k : {4.05f, 6.0f, 8.0f})
            {
                std::cout << "| " << (warm ? "tanh" : "cubic") << " | " << fixed(rate / 1000.0, 1) << " | "
                          << fixed(static_cast<double>(k), 2);
                double lo = std::numeric_limits<double>::infinity();
                double hi = -lo;
                for (const double note : notes)
                {
                    const Setup s{.rate = rate, .warm = warm, .k = k};
                    const double cents =
                        centsBetween(selfOscillationHz(s, held(note), secondsFor(k, noteHz(note))), noteHz(note));
                    lo = std::min(lo, cents);
                    hi = std::max(hi, cents);
                    std::cout << " | " << fixed(cents, 1);
                }
                std::cout << " | " << fixed(hi - lo, 2) << " |\n";
            }
        }
    }

    std::cout << "\n### Near the clamp (44.1 kHz, k 6, Warm on, keytrack 10, the cutoff two octaves above the "
                 "note)\n\n"
                 "Cents from 4 x the note's frequency (in the header) and the spectral peak; the cutoff clamps at "
                 "0.495 fs = 21,830 Hz. Locked: the output stuck at a large level (its peak over the last 0.5 s), "
                 "the solver failure in the next table.\n\n"
                 "| C5 (2093) | C6 (4186) | C7 (8372) | C8 (16744) | C9 (33488) | G9 (50175) |\n"
                 "|---|---|---|---|---|---|\n";
    for (const double note : {72.0, 84.0, 96.0, 108.0, 120.0, 127.0})
    {
        const Setup s{.rate = 44100.0, .k = 6.0f, .cutoffHz = 4.0 * 13.75};
        const auto r = run(s, kick(static_cast<std::size_t>(2.5 * s.rate)), held(note));
        double peak = 0.0;
        for (const float v : std::span{r.y}.last(22050))
        {
            peak = std::max(peak, static_cast<double>(std::fabs(v)));
        }
        if (peak > 2.0)
        {
            std::cout << "| locked, peak " << fixed(peak, 1) << " ";
            continue;
        }
        const double hz = peakHz(r.y, s.rate);
        std::cout << "| " << fixed(centsBetween(hz, 4.0 * noteHz(note)), 1) << " (" << fixed(hz, 0) << " Hz) ";
    }
    std::cout << "|\n";
}

/// The lowest cutoff, as a fraction of the sample rate, at which a 0.5
/// impulse leaves LadderFilter stuck above 2 in the second half of a 1 s
/// render, scanned in steps of 0.0025 from 0.30 to 0.495 fs.
void reportNearNyquist()
{
    std::cout << "\n### Where LadderFilter's solver fails near Nyquist (scanned 0.30 - 0.495 fs in steps of "
                 "0.0025)\n\n"
                 "The lowest cutoff, as a fraction of fs, where an impulse of 0.5 leaves the output stuck above 2 "
                 "(it should self-oscillate under 0 dBFS, or decay). Newton stops converging there: its steps cycle "
                 "at 30-65 in y, the same with 64 iterations as with 8. - : bounded up to the clamp. For M1.3's "
                 "stability task.\n\n"
                 "| Saturator | Rate | k 3.5 | 4.05 | 4.4 | 5 | 6 | 8 |\n|---|---|---|---|---|---|---|---|\n";
    for (const bool warm : {true, false})
    {
        for (const double rate : {44100.0, 96000.0})
        {
            std::cout << "| " << (warm ? "tanh" : "cubic") << " | " << fixed(rate / 1000.0, 1);
            for (const float k : {3.5f, 4.05f, 4.4f, 5.0f, 6.0f, 8.0f})
            {
                double first = std::numeric_limits<double>::quiet_NaN();
                for (int step = 0; step <= 78; ++step)
                {
                    const double ratio = 0.30 + (0.0025 * static_cast<double>(step));
                    const Setup s{.rate = rate, .warm = warm, .k = k, .cutoffHz = ratio * rate, .amount = 0.0};
                    const auto r = run(s, kick(static_cast<std::size_t>(rate)), held(69.0));
                    const auto stuck = std::ranges::any_of(std::span{r.y}.subspan(r.y.size() / 2),
                                                           [](float v) { return !(std::fabs(v) <= 2.0f); });
                    if (stuck)
                    {
                        first = ratio;
                        break;
                    }
                }
                std::cout << " | " << fixed(first, 4);
            }
            std::cout << " |\n";
        }
    }
}

// --- The master tune ------------------------------------------------------------------

void reportMasterTune()
{
    std::cout << "\n### The master tune (48 kHz, k 4.4, Warm on, keytrack 10)\n\n"
                 "The self-oscillation pitch minus the oscillators' (the note at the master tune), cents. As built "
                 "the voice adds masterTuneSemitones() to the pitch; the contrast leaves it out.\n\n"
                 "| Pitch | Master tune | A2 | A4 | A6 |\n|---|---|---|---|---|\n";
    for (const bool withTune : {true, false})
    {
        for (const float tune : {440.0f, 432.0f, 444.0f})
        {
            std::cout << "| " << (withTune ? "with the master tune (as built)" : "without it") << " | "
                      << fixed(static_cast<double>(tune), 0) << " Hz";
            for (const double note : {45.0, 69.0, 93.0})
            {
                const double offset = withTune ? static_cast<double>(dsp::masterTuneSemitones(tune)) : 0.0;
                const Setup defaults;
                const double hz = selfOscillationHz(defaults, held(note + offset), 2.5);
                const auto oscillators = static_cast<double>(dsp::noteToFrequency(static_cast<float>(note), tune));
                std::cout << " | " << fixed(centsBetween(hz, oscillators), 1);
            }
            std::cout << " |\n";
        }
    }
}

// --- Glide and bend ------------------------------------------------------------------

/// The error between two renders over [from, to), dB re the reference's power.
double errorDb(std::span<const float> y, std::span<const float> reference, std::size_t from, std::size_t to)
{
    double e = 0.0;
    double p = 0.0;
    for (std::size_t i = from; i < to; ++i)
    {
        const double d = static_cast<double>(y[i]) - static_cast<double>(reference[i]);
        e += d * d;
        p += static_cast<double>(reference[i]) * static_cast<double>(reference[i]);
    }
    return 10.0 * std::log10(std::max(e, 1e-300) / p);
}

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

/// How far the filter fed `fed` strays from the oscillators playing `played`,
/// period by period, after taking out the steady detune (as tracking() does,
/// with the filter and the oscillators fed different pitches).
Tracking trackingAgainst(const Setup& s, const PitchTrack& fed, const PitchTrack& played, std::size_t from,
                         std::size_t to)
{
    const auto r = run(s, kick(to), fed);
    const auto all = periods(r.y, s.rate);
    const auto expectedHz = [&](const Period& p)
    {
        const double width = s.rate / p.hz;
        const auto at = [&](double n)
        {
            return noteHz(played(static_cast<std::size_t>(std::max(0.0, n))));
        };
        return (at(p.at - (0.5 * width)) + (4.0 * at(p.at)) + at(p.at + (0.5 * width))) / 6.0;
    };
    double steady = 0.0;
    std::size_t count = 0;
    for (const auto& p : all)
    {
        if (p.at > static_cast<double>(from) - (0.5 * s.rate) && p.at < static_cast<double>(from))
        {
            steady += centsBetween(p.hz, expectedHz(p));
            ++count;
        }
    }
    steady /= static_cast<double>(std::max<std::size_t>(count, 1));
    Tracking t;
    double sum = 0.0;
    count = 0;
    for (const auto& p : all)
    {
        if (p.at >= static_cast<double>(from) && p.at < static_cast<double>(to))
        {
            const double e = centsBetween(p.hz, expectedHz(p)) - steady;
            t.maxCents = std::max(t.maxCents, std::fabs(e));
            sum += e * e;
            ++count;
        }
    }
    t.rmsCents = std::sqrt(sum / static_cast<double>(std::max<std::size_t>(count, 1)));
    return t;
}

void reportGlideAndBend()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t eventAt = 72000; // 1.5 s
    const Glide glide{.from = 48.0, .to = 60.0, .start = eventAt, .samples = 4800};
    const PitchTrack glidePitch = [=](std::size_t n)
    {
        return glide.noteAt(n);
    };
    const PitchTrack glideKey = [=](std::size_t n)
    {
        return n < eventAt ? 48.0 : 60.0;
    };
    const PitchTrack bendPitch = [=](std::size_t n)
    {
        const double t = (static_cast<double>(n) - static_cast<double>(eventAt)) / rate;
        return 57.0 + ((t >= 0.0 && t < 1.0) ? 2.0 * std::sin(2.0 * pi * 2.0 * t) : 0.0);
    };
    const PitchTrack bendKey = held(57.0);

    struct Method
    {
        bool keyOnly;
        Feed feed;
        std::size_t hostBlock;
    };
    constexpr std::array<Method, 6> methods{
        Method{.keyOnly = false, .feed = Feed::aligned, .hostBlock = 512},
        Method{.keyOnly = false, .feed = Feed::gridStart, .hostBlock = 512},
        Method{.keyOnly = false, .feed = Feed::perBlock, .hostBlock = 64},
        Method{.keyOnly = false, .feed = Feed::perBlock, .hostBlock = 128},
        Method{.keyOnly = false, .feed = Feed::perBlock, .hostBlock = 512},
        Method{.keyOnly = true, .feed = Feed::aligned, .hostBlock = 512},
    };
    const auto pitchName = [](bool keyOnly)
    {
        return keyOnly ? "the key alone" : "the voice's pitch (as built)";
    };

    std::cout << "\n### Following a glide and a bend: a self-oscillating filter against the oscillators (48 kHz, k "
                 "4.4, Warm on, keytrack 10)\n\n"
                 "Glide: a legato C3 -> C4 with a 100 ms portamento, linear in semitones. Bend: A3 held, the wheel "
                 "+-2 semitones at 2 Hz for 1 s. The oscillators play the voice's pitch; the filter is fed it, or "
                 "the key alone. Its pitch period by period (zero crossings) against the oscillators' over the same "
                 "period, after taking out the steady detune; over the glide plus 50 ms, and over the bend's "
                 "second. Largest / RMS, cents.\n\n"
                 "| Fed with | How | Glide | Bend |\n|---|---|---|---|\n";
    for (const auto& m : methods)
    {
        const Setup s{.rate = rate, .k = 4.4f, .feed = m.feed, .hostBlock = m.hostBlock};
        const auto g = trackingAgainst(s, m.keyOnly ? glideKey : glidePitch, glidePitch, eventAt, eventAt + 7200);
        const auto b = trackingAgainst(s, m.keyOnly ? bendKey : bendPitch, bendPitch, eventAt, eventAt + 48000);
        std::cout << "| " << pitchName(m.keyOnly) << " | " << feedName(m.feed, m.hostBlock) << " | "
                  << fixed(g.maxCents, 1) << " / " << fixed(g.rmsCents, 1) << " | " << fixed(b.maxCents, 1) << " / "
                  << fixed(b.rmsCents, 1) << " |\n";
    }

    std::cout << "\n### Zipper: each way of feeding against the aligned feed (48 kHz, LP24, k 3.6, a 110 Hz saw at "
                 "-6 dBFS, keytrack 10)\n\n"
                 "The same glide and bend, keys an octave up (C4 -> C5, A4) so the cutoff sits above the saw's "
                 "fundamental. The error's power against the aligned feed over the event, dB. The aligned feed is "
                 "itself within -77 to -86 dB of a cutoff set every sample (filters.md, the staircase table). The "
                 "key alone differs by the whole sweep: a different sound, not a zipper.\n\n"
                 "| Fed with | How | Glide | Bend |\n|---|---|---|---|\n";
    const PitchTrack glideUp = [&](std::size_t n)
    {
        return glidePitch(n) + 12.0;
    };
    const PitchTrack bendUp = [&](std::size_t n)
    {
        return bendPitch(n) + 12.0;
    };
    const PitchTrack glideKeyUp = [&](std::size_t n)
    {
        return glideKey(n) + 12.0;
    };
    const PitchTrack bendKeyUp = held(69.0);
    const auto x = saw(110.0, rate, eventAt + 48000, 0.5);
    const Setup aligned{.rate = rate, .k = 3.6f};
    const auto glideReference = run(aligned, x, glideUp).y;
    const auto bendReference = run(aligned, x, bendUp).y;
    for (const auto& m : methods)
    {
        if (!m.keyOnly && m.feed == Feed::aligned)
        {
            continue;
        }
        Setup s = aligned;
        s.feed = m.feed;
        s.hostBlock = m.hostBlock;
        const double g =
            errorDb(run(s, x, m.keyOnly ? glideKeyUp : glideUp).y, glideReference, eventAt, eventAt + 7200);
        const double b = errorDb(run(s, x, m.keyOnly ? bendKeyUp : bendUp).y, bendReference, eventAt, eventAt + 48000);
        std::cout << "| " << pitchName(m.keyOnly) << " | " << feedName(m.feed, m.hostBlock) << " | " << fixed(g, 1)
                  << " | " << fixed(b, 1) << " |\n";
    }
}

// --- A legato note step ------------------------------------------------------------------

double energyAbove1k(std::span<const float> x, std::size_t at, double rate)
{
    static const Window window = hannWindow(4096);
    const auto spectrum = powerSpectrum(x.subspan(at - 2048, 4096), rate, window);
    double sum = 0.0;
    for (std::size_t k = 0; k < spectrum.power.size(); ++k)
    {
        sum += spectrum.frequencyOf(k) >= 1000.0 ? spectrum.power[k] : 0.0;
    }
    return sum;
}

void reportLegatoStep()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 48000;
    constexpr std::size_t at = 24576; // a grid point and a host-block boundary
    std::vector<float> step(n, 0.0f);
    std::fill(step.begin() + at, step.end(), 1.0f);
    const double unitStep = energyAbove1k(step, at + 32, rate);
    const auto x = sine(150.0, rate, n, 0.25);
    std::cout << "\n### A legato note step at high resonance (48 kHz, LP24, k 3.6, a 150 Hz sine at -12 dBFS, "
                 "keytrack 10, the cutoff at the note)\n\n"
                 "The key steps from A5 (880 Hz) at a grid point, a fifth or an octave up. Energy above 1 kHz in a "
                 "Hann 4096 centred 32 samples after the step, dB re an instant unit step (filters.md's click "
                 "measure). Steady: the cutoff at the new note throughout.\n\n"
                 "| Portamento | A5 -> E6 | A5 -> A6 |\n|---|---|---|\n";
    for (const std::size_t glideSamples : {std::size_t{0}, std::size_t{1440}, std::size_t{4800}})
    {
        std::cout << "| "
                  << (glideSamples == 0 ? std::string{"none (a step)"}
                                        : fixed(1000.0 * static_cast<double>(glideSamples) / rate, 0) + " ms");
        for (const double interval : {7.0, 12.0})
        {
            const Glide g{
                .from = 81.0,
                .to = 81.0 + interval,
                .start = at,
                .samples = std::max<std::size_t>(glideSamples, 1),
            };
            const PitchTrack pitch = [=](std::size_t i)
            {
                if (glideSamples > 0)
                {
                    return g.noteAt(i);
                }
                return i < at ? 81.0 : 81.0 + interval;
            };
            const auto y = run({.rate = rate, .k = 3.6f}, x, pitch).y;
            std::cout << " | " << fixed(10.0 * std::log10(energyAbove1k(y, at + 32, rate) / unitStep), 1);
        }
        std::cout << " |\n";
    }
    std::cout << "| steady";
    for (const double interval : {7.0, 12.0})
    {
        const auto y = run({.rate = rate, .k = 3.6f}, x, held(81.0 + interval)).y;
        std::cout << " | " << fixed(10.0 * std::log10(energyAbove1k(y, at + 32, rate) / unitStep), 1);
    }
    std::cout << " |\n";
}

// --- Block sizes, subnormals, allocation -----------------------------------------------

void reportBlockSizes()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 48000;
    constexpr std::size_t silenceFrom = 32000;
    constexpr std::array<double, 8> keys{48.0, 55.0, 67.0, 43.0, 72.0, 60.0, 64.0, 52.0};
    const auto tune = static_cast<double>(dsp::masterTuneSemitones(438.0f));
    const PitchTrack pitch = [=](std::size_t i)
    {
        const std::size_t step = std::min<std::size_t>(i / 4000, keys.size() - 1);
        const Glide g{
            .from = step == 0 ? keys[0] : keys[step - 1],
            .to = keys[step],
            .start = step * 4000,
            .samples = 2880,
        };
        return g.noteAt(i) + (0.7 * std::sin(2.0 * pi * 3.1 * static_cast<double>(i) / rate)) + tune;
    };
    auto x = saw(110.0, rate, n, 0.5);
    std::fill(x.begin() + silenceFrom, x.end(), 0.0f);

    std::cout << "\n### Block sizes, subnormals, allocation (48 kHz, k 3.9, drive 2, a saw then silence, legato "
                 "keys with a 60 ms portamento, the wheel moving, master tune 438 Hz, keytrack 10)\n\n"
                 "Blocks of 1 and 37 against 4096, bit for bit (a per-block feed is fed at each host block, so its "
                 "block is the host's). Subnormal outputs and heap allocations while processing.\n\n"
                 "| Fed | 1 | 37 | Subnormals | Allocations |\n|---|---|---|---|---|\n";
    for (const auto feed : {Feed::aligned, Feed::gridStart, Feed::perBlock})
    {
        const auto at = [&](std::size_t hostBlock)
        {
            return run({.rate = rate, .k = 3.9f, .drive = 2.0f, .feed = feed, .hostBlock = hostBlock}, x, pitch);
        };
        const auto reference = at(4096);
        const auto same = [&](std::size_t hostBlock)
        {
            const auto y = at(hostBlock).y;
            return std::ranges::equal(y, reference.y, [](float p, float q)
                                      { return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q); });
        };
        const bool one = same(1);
        const bool odd = same(37);
        const auto subnormals =
            std::ranges::count_if(reference.y, [](float v) { return std::fpclassify(v) == FP_SUBNORMAL; });
        CHECK(subnormals == 0);
        CHECK(reference.allocations == 0);
        if (feed != Feed::perBlock)
        {
            CHECK(one);
            CHECK(odd);
        }
        std::cout << "| " << feedName(feed, 4096) << " | " << (one ? "identical" : "DIFFER") << " | "
                  << (odd ? "identical" : "DIFFER") << " | " << subnormals << " | " << reference.allocations << " |\n";
    }
}

} // namespace

TEST_CASE("Keytrack report: keytracking as built on LadderFilter", "[.][keytrack-report]")
{
    std::cout << "\n## Keytrack report\n";
    reportLaw();
    reportIntervals();
    reportNearNyquist();
    reportMasterTune();
    reportGlideAndBend();
    reportLegatoStep();
    reportBlockSizes();
}
