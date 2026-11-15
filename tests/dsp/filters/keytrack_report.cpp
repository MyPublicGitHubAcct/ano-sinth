// The keytracking report (M1.3 task 5, docs/dsp/keytracking.md): measures
// the candidate models in keytrack_candidates.hpp on LadderFilter as built:
//
// - the law and the reference: the self-oscillation pitch at keytrack 0, 5,
//   and 10 across the keyboard, with the reference at A-1 and at A4;
// - intervals at keytrack 10: the self-oscillation pitch from C1 to C7 at
//   44.1 and 192 kHz, Warm on and off, at the threshold and at k 6 and 8,
//   for each model, and where the cutoff clamps below 0.495 fs;
// - the master tune: how far the filter lands from the oscillators;
// - a portamento glide and a pitch-bend sweep: how closely a self-oscillating
//   filter follows the oscillators' pitch, and the zipper of each feeding
//   rate (once per host block, or every 32-sample grid point);
// - a legato note step at high resonance: the click against a unit step;
// - bit-identity across block sizes 1, 37, 4096; subnormals; allocation.
//
// Hidden from the default run; run it in an optimised build (about a minute):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[keytrack-report]"
//
// CPU: the "[keytrack]" benchmarks (tools/bench.sh "[keytrack]").

#include "../support/allocation_counter.hpp"
#include "keytrack_candidates.hpp"

#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/math/pitch.hpp>

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
#include <vector>

using namespace anoesis::test;
namespace dsp = anoesis::dsp;
namespace kt = anoesis::test::keytrack;
using dsp::LadderFilter;

namespace
{

constexpr double pi = std::numbers::pi;
constexpr std::size_t grid = LadderFilter::gridSamples;

std::string fixed(double x, int digits)
{
    if (std::isnan(x))
    {
        return "-";
    }
    if (std::isinf(x))
    {
        return x < 0 ? "-inf" : "inf";
    }
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

double noteHz(double note, double masterTuneHz = 440.0)
{
    return masterTuneHz * std::exp2((note - 69.0) / 12.0);
}

double centsBetween(double hz, double referenceHz)
{
    return 1200.0 * std::log2(hz / referenceHz);
}

// --- The models and how the voice feeds them -----------------------------------

enum class Model : std::uint8_t
{
    noteNumber,    ///< (a)
    soundingPitch, ///< (b)
    linearHz,      ///< (c)
    corrected,     ///< (d)
    noteRamped,    ///< (e)
};

enum class Feed : std::uint8_t
{
    perBlock,  ///< once at the start of each host block
    gridStart, ///< at each grid point, the pitch there (applied by the segment's end)
    gridEnd,   ///< at each grid point, the pitch at the segment's end (aligned)
};

const char* modelName(Model m)
{
    switch (m)
    {
    case Model::noteNumber:
        return "(a) note number";
    case Model::soundingPitch:
        return "(b) sounding pitch";
    case Model::linearHz:
        return "(c) linear in Hz";
    case Model::corrected:
        return "(d) sounding pitch + c(k)";
    case Model::noteRamped:
        return "(e) note number, steps ramped 10 ms";
    }
    return "?";
}

std::string feedName(Feed f, std::size_t hostBlock)
{
    switch (f)
    {
    case Feed::perBlock:
        return "per block of " + std::to_string(hostBlock);
    case Feed::gridStart:
        return "per grid point (its start)";
    case Feed::gridEnd:
        return "per grid point (aligned)";
    }
    return "?";
}

/// The detune table for (d), one per saturator; filled by calibrate().
constexpr std::array<float, 15> tableK{
    4.0f, 4.05f, 4.1f, 4.2f, 4.3f, 4.4f, 4.6f, 4.8f, 5.0f, 5.5f, 6.0f, 6.5f, 7.0f, 7.5f, 8.0f,
};
using Table = kt::DetuneTable<tableK.size()>;

struct Setup
{
    double rate = 48000.0;
    bool warm = true;
    float k = 4.4f;
    float drive = 1.0f;
    double cutoffHz = 440.0;            ///< the cutoff knob (Hz): the cutoff at the reference note
    double reference = kt::referenceA4; ///< the note where keytrack changes nothing
    double amount = 1.0;                ///< keytrack / 10
    Model model = Model::soundingPitch;
    Feed feed = Feed::gridEnd;
    std::size_t hostBlock = 512;
    const Table* table = nullptr;
};

using PitchTrack = std::function<kt::VoicePitch(std::size_t)>;

PitchTrack fixedKey(double key, double masterTuneHz = 440.0)
{
    return [=](std::size_t)
    {
        return kt::VoicePitch{.key = key, .masterTuneHz = masterTuneHz};
    };
}

float octavesFor(const Setup& s, const kt::VoicePitch& p)
{
    switch (s.model)
    {
    case Model::noteNumber:
    case Model::noteRamped:
        return kt::noteNumberOctaves(p, s.amount, s.reference);
    case Model::soundingPitch:
        return kt::soundingPitchOctaves(p, s.amount, s.reference);
    case Model::linearHz:
        return kt::linearHzOctaves(p, s.amount, s.cutoffHz);
    case Model::corrected:
        return kt::correctedOctaves(p, s.amount, s.reference, *s.table, s.k);
    }
    return 0.0f;
}

void apply(LadderFilter& f, const Setup& s, const kt::VoicePitch& p)
{
    const float octaves = octavesFor(s, p);
    if (s.model == Model::noteRamped)
    {
        f.setCutoff(static_cast<float>(s.cutoffHz * std::exp2(static_cast<double>(octaves))));
    }
    else
    {
        f.setCutoffModulation(octaves);
    }
}

struct Run
{
    std::vector<float> y;
    std::size_t allocations = 0;
};

/// Filters `x` in host blocks of s.hostBlock, split at the filter's grid,
/// feeding the keytrack as s.feed says. The filter is set up (and reset) with
/// the pitch at sample 0 applied, as a voice starting a note would.
Run run(const Setup& s, std::span<const float> x, const PitchTrack& pitch)
{
    LadderFilter f;
    f.setWarm(s.warm);
    f.setResonance(s.k);
    f.setDrive(s.drive);
    f.setCutoff(static_cast<float>(s.cutoffHz));
    apply(f, s, pitch(0));
    f.prepare({.sampleRate = static_cast<float>(s.rate), .maxBlockSize = 4096, .numChannels = 1});
    Buffer buffer{1, x.size()};
    std::ranges::copy(x, buffer.channel(0).begin());
    Run result;
    {
        const AllocationCounter counter;
        for (std::size_t block = 0; block < x.size(); block += s.hostBlock)
        {
            const auto blockEnd = std::min(x.size(), block + s.hostBlock);
            if (s.feed == Feed::perBlock)
            {
                apply(f, s, pitch(block));
            }
            for (std::size_t start = block; start < blockEnd;)
            {
                const auto end = std::min(blockEnd, ((start / grid) + 1) * grid);
                if (s.feed != Feed::perBlock && start % grid == 0)
                {
                    apply(f, s, pitch(s.feed == Feed::gridStart ? start : start + grid));
                }
                f.process(buffer.block().subBlock(start, end - start));
                start = end;
            }
        }
        result.allocations = counter.count();
    }
    result.y.assign(buffer.channel(0).begin(), buffer.channel(0).end());
    return result;
}

/// Long enough for the oscillation to grow from the impulse and settle: the
/// linear loop grows by e every 1/((k^(1/4) cos 45 deg - 1) 2 pi f) seconds
/// (filter-theory.md), about 51/f s at k 4.05 and 6.6/f s at k 4.4, and it
/// takes about ten of those.
double secondsFor(float k, double hz)
{
    const double growth = (std::pow(static_cast<double>(k), 0.25) * std::numbers::sqrt2 / 2.0) - 1.0;
    return std::max(2.5, 0.5 + (12.0 / (growth * 2.0 * pi * hz)));
}

/// The strongest spectral peak in Hz (Hann, parabolic on the log power), for
/// tones near Nyquist where zero crossings can't resolve the period.
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

std::vector<float> kick(std::size_t n)
{
    std::vector<float> x(n, 0.0f);
    x[0] = 0.5f;
    return x;
}

/// The self-oscillation pitch in Hz over the last 0.5 s of a render started
/// by one impulse, or NaN when it doesn't oscillate.
double selfOscillationHz(const Setup& s, const PitchTrack& pitch, double seconds, bool spectral = false)
{
    const auto r = run(s, kick(static_cast<std::size_t>(seconds * s.rate)), pitch);
    const auto tail = std::span{r.y}.subspan(r.y.size() - static_cast<std::size_t>(0.5 * s.rate));
    double sum = 0.0;
    for (const float v : tail)
    {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    if (!(sum > 1e-12) || !std::isfinite(sum))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return spectral ? peakHz(r.y, s.rate) : measureFrequency(tail, s.rate);
}

// --- (d)'s calibration ------------------------------------------------------------

Table calibrate(bool warm)
{
    Table table;
    table.k = tableK;
    for (std::size_t i = 0; i < tableK.size(); ++i)
    {
        if (i == 0)
        {
            table.cents[i] = 0.0f; // at the threshold the oscillation is vanishingly small: no detune
            continue;
        }
        const Setup s{.rate = 48000.0, .warm = warm, .k = tableK[i], .cutoffHz = 880.0, .amount = 0.0};
        table.cents[i] =
            static_cast<float>(centsBetween(selfOscillationHz(s, fixedKey(69), secondsFor(tableK[i], 880.0)), 880.0));
    }
    return table;
}

void reportCalibration(const Table& tanhTable, const Table& cubicTable)
{
    std::cout << "\n### (d)'s table: the self-oscillation detune against k (48 kHz, cutoff 880 Hz, cents)\n\n"
                 "Measured here, then interpolated linearly by (d); k 4 is set to 0.\n\n| Saturator";
    for (const float k : tableK)
    {
        std::cout << " | " << fixed(static_cast<double>(k), 2);
    }
    std::cout << " |\n|---";
    for (std::size_t i = 0; i < tableK.size(); ++i)
    {
        std::cout << "|---";
    }
    std::cout << "|\n";
    for (const auto& [name, table] :
         {std::pair{"tanh (Warm on)", &tanhTable}, std::pair{"cubic (Warm off)", &cubicTable}})
    {
        std::cout << "| " << name;
        for (const float c : table->cents)
        {
            std::cout << " | " << fixed(static_cast<double>(c), 1);
        }
        std::cout << " |\n";
    }
}

// --- The law and the reference ------------------------------------------------------

void reportLaw()
{
    constexpr std::array<double, 6> notes{9.0, 24.0, 48.0, 69.0, 72.0, 96.0};
    std::cout << "\n### Keytrack 0, 5, 10 against the reference ((a), 48 kHz, k 4.4, Warm on)\n\n"
                 "The self-oscillation pitch in Hz at each note. Reference A-1 (MIDI 9) with the cutoff at 13.75 Hz, "
                 "and reference A4 (MIDI 69) with the cutoff at 440 Hz: either way the cutoff is the note's own "
                 "frequency at keytrack 10. Expected: cutoff x 2^(keytrack/10 x (note - reference)/12), 35.8 cents "
                 "flat (k 4.4).\n\n"
                 "| Reference | Keytrack | A-1 | C1 | C3 | A4 | C5 | C7 |\n|---|---|---|---|---|---|---|---|\n";
    for (const double reference : {kt::referenceAMinus1, kt::referenceA4})
    {
        for (const double amount : {0.0, 0.5, 1.0})
        {
            std::cout << "| " << (reference < 20.0 ? "A-1" : "A4") << " | " << fixed(10.0 * amount, 0);
            for (const double note : notes)
            {
                const Setup s{
                    .cutoffHz = noteHz(reference),
                    .reference = reference,
                    .amount = amount,
                    .model = Model::noteNumber,
                };
                const double expected = noteHz(reference) * std::exp2(amount * (note - reference) / 12.0);
                const double hz = selfOscillationHz(s, fixedKey(note), secondsFor(s.k, expected));
                std::cout << " | " << fixed(hz, hz < 100.0 ? 2 : 1);
                CHECK(std::fabs(centsBetween(hz, expected) + 35.8) < 2.0);
            }
            std::cout << " |\n";
        }
    }
}

// --- Intervals at keytrack 10 --------------------------------------------------------

struct IntervalRow
{
    Model model = Model::soundingPitch;
    double amount = 1.0;
    double cutoffHz = 440.0; ///< (c): the cutoff it adds to
};

void reportIntervals(const Table& tanhTable, const Table& cubicTable)
{
    constexpr std::array<double, 7> notes{24.0, 36.0, 48.0, 60.0, 72.0, 84.0, 96.0};
    std::cout << "\n### Intervals at keytrack 10: the self-oscillation pitch, cents from each note's 12-TET "
                 "frequency\n\n"
                 "Reference A4 at 440 Hz, so at keytrack 10 the cutoff is the note's frequency. (a) and (b) are the "
                 "same filter on a held note with no bend, glide, or master tune. Spread: the largest minus the "
                 "smallest, the error in any interval from C1 to C7. (c) adds to a 10 Hz cutoff (the knob's "
                 "bottom, F3) at keytrack 10 and 5.\n\n"
                 "| Model | Saturator | Rate | k | C1 | C2 | C3 | C4 | C5 | C6 | C7 | Spread |\n"
                 "|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    const auto row = [&](const IntervalRow& r, bool warm, double rate, float k)
    {
        std::cout << "| " << modelName(r.model)
                  << (r.model == Model::linearHz ? ", keytrack " + fixed(10.0 * r.amount, 0) : std::string{}) << " | "
                  << (warm ? "tanh" : "cubic") << " | " << fixed(rate / 1000.0, 1) << " | "
                  << fixed(static_cast<double>(k), 2);
        double lo = std::numeric_limits<double>::infinity();
        double hi = -lo;
        for (const double note : notes)
        {
            const Setup s{
                .rate = rate,
                .warm = warm,
                .k = k,
                .cutoffHz = r.cutoffHz,
                .amount = r.amount,
                .model = r.model,
                .table = warm ? &tanhTable : &cubicTable,
            };
            const double cents =
                centsBetween(selfOscillationHz(s, fixedKey(note), secondsFor(k, noteHz(note))), noteHz(note));
            lo = std::min(lo, cents);
            hi = std::max(hi, cents);
            std::cout << " | " << fixed(cents, 1);
        }
        std::cout << " | " << fixed(hi - lo, 2) << " |\n";
    };
    for (const auto model : {Model::soundingPitch, Model::corrected})
    {
        for (const bool warm : {true, false})
        {
            for (const double rate : {44100.0, 192000.0})
            {
                for (const float k : {4.05f, 6.0f, 8.0f})
                {
                    row({.model = model}, warm, rate, k);
                }
            }
        }
    }
    for (const double amount : {1.0, 0.5})
    {
        row({.model = Model::linearHz, .amount = amount, .cutoffHz = 10.0}, true, 44100.0, 6.0f);
    }

    std::cout << "\n### Near the clamp (44.1 kHz, k 6, Warm on, keytrack 10, the cutoff two octaves above the "
                 "note)\n\n"
                 "Cents from 4 x the note's frequency (the expected cutoff, in the header) and the spectral peak; "
                 "the cutoff clamps at 0.495 fs = 21,830 Hz. Locked: the output stuck at a large level (its peak "
                 "over the last 0.5 s), the solver failure in the next table.\n\n"
                 "| Model | C5 (2093) | C6 (4186) | C7 (8372) | C8 (16744) | C9 (33488) | G9 (50175) |\n"
                 "|---|---|---|---|---|---|---|\n";
    for (const auto model : {Model::soundingPitch, Model::corrected})
    {
        std::cout << "| " << modelName(model);
        for (const double note : {72.0, 84.0, 96.0, 108.0, 120.0, 127.0})
        {
            const Setup s{.rate = 44100.0, .k = 6.0f, .cutoffHz = 1760.0, .model = model, .table = &tanhTable};
            const auto r = run(s, kick(static_cast<std::size_t>(2.5 * s.rate)), fixedKey(note));
            double peak = 0.0;
            for (std::size_t i = r.y.size() - 22050; i < r.y.size(); ++i)
            {
                peak = std::max(peak, static_cast<double>(std::fabs(r.y[i])));
            }
            if (peak > 2.0)
            {
                std::cout << " | locked, peak " << fixed(peak, 1);
                continue;
            }
            const double hz = peakHz(r.y, s.rate);
            std::cout << " | " << fixed(centsBetween(hz, 4.0 * noteHz(note)), 1) << " (" << fixed(hz, 0) << " Hz)";
        }
        std::cout << " |\n";
    }
}

/// The lowest cutoff, as a fraction of the sample rate, at which a 0.5
/// impulse leaves the filter stuck above 2 in the second half of a 1 s
/// render, scanned in steps of 0.0025 from 0.30 to 0.495 fs.
void reportNearNyquist()
{
    std::cout << "\n### Where the solver fails near Nyquist (LadderFilter as built; scanned 0.30 - 0.495 fs in "
                 "steps of 0.0025)\n\n"
                 "The lowest cutoff, as a fraction of fs, where an impulse of 0.5 leaves the output stuck above 2 "
                 "(it should self-oscillate under 0 dBFS, or decay). Newton stops converging there: its steps cycle "
                 "at 30-65 in y, the same with 64 iterations as with 8. - : bounded up to the clamp.\n\n"
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
                    const auto r = run(s, kick(static_cast<std::size_t>(rate)), fixedKey(69));
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
    std::cout << "\n### The master tune (48 kHz, k 4.4, Warm on, keytrack 10, reference A4, cutoff 440 Hz)\n\n"
                 "The self-oscillation pitch minus the oscillators' (the note at the master tune), cents. At 440 Hz "
                 "both models give the k 4.4 detune alone.\n\n"
                 "| Model | Master tune | A2 | A4 | A6 |\n|---|---|---|---|---|\n";
    for (const auto model : {Model::noteNumber, Model::soundingPitch})
    {
        for (const double tune : {440.0, 432.0, 444.0})
        {
            std::cout << "| " << modelName(model) << " | " << fixed(tune, 0) << " Hz";
            for (const double note : {45.0, 69.0, 93.0})
            {
                const Setup s{.model = model};
                const double hz = selfOscillationHz(s, fixedKey(note, tune), 2.5);
                std::cout << " | " << fixed(centsBetween(hz, noteHz(note, tune)), 1);
            }
            std::cout << " |\n";
        }
    }
}

// --- Glide and bend ------------------------------------------------------------------

/// The pitch of a self-oscillation period by period: upward zero crossings
/// interpolated linearly; each period's frequency at its midpoint (samples).
struct Period
{
    double at = 0.0;
    double hz = 0.0;
};

std::vector<Period> periods(std::span<const float> y, double rate)
{
    std::vector<Period> out;
    double last = -1.0;
    for (std::size_t i = 1; i < y.size(); ++i)
    {
        if (y[i - 1] < 0.0f && y[i] >= 0.0f)
        {
            const auto a = static_cast<double>(y[i - 1]);
            const auto b = static_cast<double>(y[i]);
            const double t = static_cast<double>(i - 1) + (a / (a - b));
            if (last >= 0.0)
            {
                out.push_back({.at = 0.5 * (last + t), .hz = rate / (t - last)});
            }
            last = t;
        }
    }
    return out;
}

struct Tracking
{
    double maxCents = 0.0;
    double rmsCents = 0.0;
};

/// How far a self-oscillating filter strays from the oscillators' pitch over
/// [from, to), after taking out its steady detune (read over the 0.5 s before
/// `from`). The oscillators' pitch is averaged over each period, as the
/// measurement is.
Tracking tracking(const Setup& s, const PitchTrack& pitch, std::size_t from, std::size_t to)
{
    const auto r = run(s, kick(to), pitch);
    const auto all = periods(r.y, s.rate);
    const auto expectedHz = [&](const Period& p, double width)
    {
        // The mean of the oscillators' frequency over the period, by Simpson's rule.
        const auto at = [&](double n)
        {
            return noteHz(pitch(static_cast<std::size_t>(std::max(0.0, n))).soundingNote());
        };
        return (at(p.at - (0.5 * width)) + (4.0 * at(p.at)) + at(p.at + (0.5 * width))) / 6.0;
    };
    double steady = 0.0;
    std::size_t count = 0;
    const auto steadyFrom = static_cast<double>(from) - (0.5 * s.rate);
    for (const auto& p : all)
    {
        if (p.at > steadyFrom && p.at < static_cast<double>(from))
        {
            steady += centsBetween(p.hz, expectedHz(p, s.rate / p.hz));
            ++count;
        }
    }
    steady /= static_cast<double>(count);
    Tracking t;
    double sum = 0.0;
    count = 0;
    for (const auto& p : all)
    {
        if (p.at >= static_cast<double>(from) && p.at < static_cast<double>(to))
        {
            const double e = centsBetween(p.hz, expectedHz(p, s.rate / p.hz)) - steady;
            t.maxCents = std::max(t.maxCents, std::fabs(e));
            sum += e * e;
            ++count;
        }
    }
    t.rmsCents = std::sqrt(sum / static_cast<double>(std::max<std::size_t>(count, 1)));
    return t;
}

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

void reportGlideAndBend()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t eventAt = 72000; // 1.5 s
    // A legato C3 -> C4 with a 100 ms portamento (Normal mode, constant time).
    const kt::Glide glide{.from = 48.0, .to = 60.0, .start = eventAt, .samples = 4800};
    const PitchTrack glidePitch = [=](std::size_t n)
    {
        return kt::VoicePitch{.key = n < eventAt ? 48.0 : 60.0, .glide = glide.glideAt(n)};
    };
    // A3 held, the wheel swept +-2 semitones at 2 Hz for 1 s.
    const PitchTrack bendPitch = [=](std::size_t n)
    {
        const double t = (static_cast<double>(n) - static_cast<double>(eventAt)) / rate;
        return kt::VoicePitch{.key = 57.0, .bend = (t >= 0.0 && t < 1.0) ? 2.0 * std::sin(2.0 * pi * 2.0 * t) : 0.0};
    };

    struct Method
    {
        Model model;
        Feed feed;
        std::size_t hostBlock;
    };
    constexpr std::array<Method, 7> methods{
        Method{.model = Model::noteNumber, .feed = Feed::gridEnd, .hostBlock = 512},
        Method{.model = Model::noteRamped, .feed = Feed::gridEnd, .hostBlock = 512},
        Method{.model = Model::soundingPitch, .feed = Feed::perBlock, .hostBlock = 512},
        Method{.model = Model::soundingPitch, .feed = Feed::perBlock, .hostBlock = 128},
        Method{.model = Model::soundingPitch, .feed = Feed::perBlock, .hostBlock = 64},
        Method{.model = Model::soundingPitch, .feed = Feed::gridStart, .hostBlock = 512},
        Method{.model = Model::soundingPitch, .feed = Feed::gridEnd, .hostBlock = 512},
    };

    std::cout << "\n### Following a glide and a bend: a self-oscillating filter against the oscillators (48 kHz, k "
                 "4.4, Warm on, keytrack 10)\n\n"
                 "Glide: a legato C3 -> C4 with a 100 ms portamento, linear in semitones. Bend: A3 held, the wheel "
                 "+-2 semitones at 2 Hz for 1 s. The pitch period by period (zero crossings) against the "
                 "oscillators' over the same period, after taking out the steady detune; over the glide plus 50 "
                 "ms, and over the bend's second. Largest / RMS, cents.\n\n"
                 "| Model | Fed | Glide | Bend |\n|---|---|---|---|\n";
    for (const auto& m : methods)
    {
        const Setup s{.rate = rate, .k = 4.4f, .model = m.model, .feed = m.feed, .hostBlock = m.hostBlock};
        const auto g = tracking(s, glidePitch, eventAt, eventAt + 4800 + 2400);
        const auto b = tracking(s, bendPitch, eventAt, eventAt + 48000);
        std::cout << "| " << modelName(m.model) << " | " << feedName(m.feed, m.hostBlock) << " | "
                  << fixed(g.maxCents, 1) << " / " << fixed(g.rmsCents, 1) << " | " << fixed(b.maxCents, 1) << " / "
                  << fixed(b.rmsCents, 1) << " |\n";
    }

    std::cout
        << "\n### Zipper: each way of feeding against the aligned grid (48 kHz, LP24, k 3.6, a 110 Hz saw at "
           "-6 dBFS, keytrack 10)\n\n"
           "The same glide and bend, keys an octave up (C4 -> C5, A4) so the cutoff sits above the saw's "
           "fundamental. The error's power against the aligned grid's output over the event, dB. The aligned "
           "grid is itself within -77 to -86 dB of a cutoff set every sample (filters.md, the staircase "
           "table). (a) and (e) are left out: they don't follow the glide or the bend, so they differ by the whole "
           "sweep (-1 and -3 dB), which is a different sound, not a zipper.\n\n| Model | Fed | Glide | Bend "
           "|\n|---|---|---|---|\n";
    const PitchTrack glideUp = [&](std::size_t n)
    {
        auto p = glidePitch(n);
        p.key += 12.0;
        return p;
    };
    const PitchTrack bendUp = [&](std::size_t n)
    {
        auto p = bendPitch(n);
        p.key += 12.0;
        return p;
    };
    const auto x = saw(110.0, rate, eventAt + 48000, 0.5);
    const Setup aligned{.rate = rate, .k = 3.6f, .model = Model::soundingPitch};
    const auto glideReference = run(aligned, x, glideUp).y;
    const auto bendReference = run(aligned, x, bendUp).y;
    for (const auto& m : methods)
    {
        if (m.model != Model::soundingPitch || m.feed == Feed::gridEnd)
        {
            continue; // (a) and (e) don't follow the glide at all: a different sound, not a zipper
        }
        Setup s = aligned;
        s.model = m.model;
        s.feed = m.feed;
        s.hostBlock = m.hostBlock;
        const double g = errorDb(run(s, x, glideUp).y, glideReference, eventAt, eventAt + 7200);
        const double b = errorDb(run(s, x, bendUp).y, bendReference, eventAt, eventAt + 48000);
        std::cout << "| " << modelName(m.model) << " | " << feedName(m.feed, m.hostBlock) << " | " << fixed(g, 1)
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
                 "| Model | Fed | A5 -> E6 | A5 -> A6 |\n|---|---|---|---|\n";
    struct Method
    {
        Model model;
        Feed feed;
        std::size_t glideSamples; ///< 0: no portamento
        const char* how;
    };
    for (const auto& m : {
             Method{
                 .model = Model::noteNumber,
                 .feed = Feed::gridEnd,
                 .glideSamples = 0,
                 .how = "per grid point, a step",
             },
             Method{
                 .model = Model::noteRamped,
                 .feed = Feed::gridEnd,
                 .glideSamples = 0,
                 .how = "into setCutoff(), ramped 10 ms",
             },
             Method{
                 .model = Model::soundingPitch,
                 .feed = Feed::gridEnd,
                 .glideSamples = 1440,
                 .how = "per grid point, a 30 ms portamento",
             },
             Method{
                 .model = Model::soundingPitch,
                 .feed = Feed::gridEnd,
                 .glideSamples = 4800,
                 .how = "per grid point, a 100 ms portamento",
             },
         })
    {
        std::cout << "| " << modelName(m.model) << " | " << m.how;
        for (const double interval : {7.0, 12.0})
        {
            const kt::Glide g{
                .from = 81.0,
                .to = 81.0 + interval,
                .start = at,
                .samples = std::max<std::size_t>(m.glideSamples, 1),
            };
            const PitchTrack pitch = [=](std::size_t i)
            {
                return kt::VoicePitch{
                    .key = i < at ? 81.0 : 81.0 + interval,
                    .glide = m.glideSamples == 0 ? 0.0 : g.glideAt(i),
                };
            };
            const Setup s{.rate = rate, .k = 3.6f, .model = m.model, .feed = m.feed};
            const auto y = run(s, x, pitch).y;
            std::cout << " | " << fixed(10.0 * std::log10(energyAbove1k(y, at + 32, rate) / unitStep), 1);
        }
        std::cout << " |\n";
    }
    std::cout << "| steady | the cutoff at the new note throughout";
    for (const double interval : {7.0, 12.0})
    {
        const Setup s{.rate = rate, .k = 3.6f, .model = Model::noteNumber};
        const auto y = run(s, x, fixedKey(81.0 + interval)).y;
        std::cout << " | " << fixed(10.0 * std::log10(energyAbove1k(y, at + 32, rate) / unitStep), 1);
    }
    std::cout << " |\n";
}

// --- Block sizes, subnormals, allocation -----------------------------------------------

void reportBlockSizes(const Table& tanhTable)
{
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 48000;
    constexpr std::size_t silenceFrom = 32000;
    // Legato keys every 4000 samples with a 60 ms portamento, and the wheel
    // moving throughout; then silence to the end (16000 samples).
    constexpr std::array<double, 8> keys{48.0, 55.0, 67.0, 43.0, 72.0, 60.0, 64.0, 52.0};
    const PitchTrack pitch = [=](std::size_t i)
    {
        const std::size_t step = std::min<std::size_t>(i / 4000, keys.size() - 1);
        const double from = step == 0 ? keys[0] : keys[step - 1];
        const kt::Glide g{.from = from, .to = keys[step], .start = step * 4000, .samples = 2880};
        const double t = static_cast<double>(i) / rate;
        return kt::VoicePitch{
            .key = keys[step],
            .glide = g.glideAt(i),
            .bend = 0.7 * std::sin(2.0 * pi * 3.1 * t),
            .masterTuneHz = 438.0,
        };
    };
    auto x = saw(110.0, rate, n, 0.5);
    std::fill(x.begin() + silenceFrom, x.end(), 0.0f);

    std::cout << "\n### Block sizes, subnormals, allocation (48 kHz, k 3.9, drive 2, a saw then silence, legato "
                 "keys with a 60 ms portamento, the wheel moving, master tune 438 Hz, keytrack 10)\n\n"
                 "Blocks of 1 and 37 against 4096, bit for bit (the per-block feed is fed at each host block, so "
                 "its block is the host's). Subnormal outputs and heap "
                 "allocations while processing.\n\n"
                 "| Model | Fed | 1 | 37 | Subnormals | Allocations |\n|---|---|---|---|---|---|\n";
    struct Method
    {
        Model model;
        Feed feed;
    };
    for (const auto& m : {
             Method{.model = Model::noteNumber, .feed = Feed::gridEnd},
             Method{.model = Model::soundingPitch, .feed = Feed::perBlock},
             Method{.model = Model::soundingPitch, .feed = Feed::gridStart},
             Method{.model = Model::soundingPitch, .feed = Feed::gridEnd},
             Method{.model = Model::linearHz, .feed = Feed::gridEnd},
             Method{.model = Model::corrected, .feed = Feed::gridEnd},
             Method{.model = Model::noteRamped, .feed = Feed::gridEnd},
         })
    {
        const auto at = [&](std::size_t hostBlock)
        {
            const Setup s{
                .rate = rate,
                .k = 3.9f,
                .drive = 2.0f,
                .cutoffHz = 440.0,
                .model = m.model,
                .feed = m.feed,
                .hostBlock = hostBlock,
                .table = &tanhTable,
            };
            return run(s, x, pitch);
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
        std::cout << "| " << modelName(m.model) << " | " << feedName(m.feed, 4096) << " | "
                  << (one ? "identical" : "DIFFER") << " | " << (odd ? "identical" : "DIFFER") << " | " << subnormals
                  << " | " << reference.allocations << " |\n";
    }
}

} // namespace

TEST_CASE("Keytrack report: candidate models on LadderFilter", "[.][keytrack-report]")
{
    std::cout << "\n## Keytrack report\n";
    const auto tanhTable = calibrate(true);
    const auto cubicTable = calibrate(false);
    reportCalibration(tanhTable, cubicTable);
    reportLaw();
    reportIntervals(tanhTable, cubicTable);
    reportNearNyquist();
    reportMasterTune();
    reportGlideAndBend();
    reportLegatoStep();
    reportBlockSizes(tanhTable);
}
