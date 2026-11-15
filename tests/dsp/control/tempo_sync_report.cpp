// The tempo-sync report: tempo sync as built (docs/dsp/tempo-sync.md): the
// note values and conversions (note_value.hpp), HostTempo, and a synced Lfo
// driven once per block through setPhaseIncrement(). The candidate models it
// was chosen from were measured by this report at commit d181628
// (tempo_sync_candidates.hpp there). It measures:
//
// - the value list (syncNoteValues): every value's exact length, seconds,
//   hertz, and samples;
// - exactness: identities in exact, double, and float arithmetic, and an
//   arpeggiator's step positions over three hours;
// - drift of a synced Lfo against the exact grid over one minute (three
//   hours in the long report) at 8/1 and 1/64T, 44.1-192 kHz, through
//   setPhaseIncrement and, for comparison, through setFrequency(float);
// - a tempo step and a tempo ramp: the largest step, the click, and the
//   lag behind the note-on's grid by block size;
// - block sizes 1, 37, and 4096; subnormals; allocation.
//
// A loop, a locate, or the transport starting or stopping changes nothing:
// nothing here reads the host's position.
//
// Hidden from the default run; run it in an optimised build:
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[sync-report]"
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[sync-long-report]"
//
// CPU: the "[sync]" benchmarks (tools/bench.sh "[sync]").

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/host_tempo.hpp>
#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/control/note_value.hpp>

#include <anoesis/test/measure.hpp>
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
#include <tuple>
#include <vector>

using namespace anoesis::test;
using anoesis::dsp::HostTempo;
using anoesis::dsp::Lfo;
using anoesis::dsp::LfoWaveform;
using anoesis::dsp::noteHertz;
using anoesis::dsp::NoteKind;
using anoesis::dsp::notePhaseIncrement;
using anoesis::dsp::noteSamples;
using anoesis::dsp::noteSeconds;
using anoesis::dsp::NoteValue;
using anoesis::dsp::Ratio;
using anoesis::dsp::syncNoteValues;

namespace
{

constexpr double rateD = 48000.0;

std::string fixed(double x, int digits)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

std::string sci(double x)
{
    std::ostringstream s;
    s << std::scientific << std::setprecision(1) << x;
    return s.str();
}

std::string db(double x)
{
    if (!std::isfinite(x))
    {
        return x < 0 ? "-inf" : "inf";
    }
    return fixed(x, 1);
}

bool sameBits(double a, double b)
{
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

std::string fraction(Ratio r)
{
    return std::to_string(r.numerator()) + "/" + std::to_string(r.denominator());
}

std::string nameOf(NoteValue v)
{
    std::string name = fraction(v.base);
    if (v.kind == NoteKind::triplet)
    {
        name += 'T';
    }
    else if (v.kind == NoteKind::dotted)
    {
        name += 'D';
    }
    return name;
}

/// floor(a x 2^64 / b), for a < b < 2^63: long division, a bit at a time.
std::uint64_t fixedQuotient(std::uint64_t a, std::uint64_t b)
{
    std::uint64_t quotient = 0;
    std::uint64_t remainder = a;
    for (int bit = 0; bit < 64; ++bit)
    {
        remainder <<= 1U;
        quotient <<= 1U;
        if (remainder >= b)
        {
            remainder -= b;
            quotient |= 1U;
        }
    }
    return quotient;
}

double signedCycles(std::uint64_t difference)
{
    return std::ldexp(static_cast<double>(static_cast<std::int64_t>(difference)), -64);
}

// The LFO report's click metric: the energy above 1 kHz in a 4096-sample
// Hann window centred on `at`, in dB re an instant step from 0 to 1.
double windowedEnergyAbove(std::span<const float> x, std::size_t at, double cutHz)
{
    static const Window window = hannWindow(4096);
    const std::vector<float> segment(x.begin() + static_cast<std::ptrdiff_t>(at - 2048),
                                     x.begin() + static_cast<std::ptrdiff_t>(at + 2048));
    const auto spectrum = powerSpectrum(segment, rateD, window);
    double sum = 0.0;
    for (std::size_t k = 0; k < spectrum.power.size(); ++k)
    {
        if (spectrum.frequencyOf(k) >= cutHz)
        {
            sum += spectrum.power[k];
        }
    }
    return sum;
}

double clickDb(std::span<const float> x, std::size_t at)
{
    std::vector<float> step(8192, 0.0f);
    std::fill(step.begin() + 4096, step.end(), 1.0f);
    return powerToDb(windowedEnergyAbove(x, at, 1000.0) / windowedEnergyAbove(step, 4096, 1000.0));
}

Lfo preparedLfo(LfoWaveform waveform, double fs)
{
    Lfo lfo{1};
    lfo.setWaveform(waveform);
    lfo.prepare({.sampleRate = static_cast<float>(fs), .maxBlockSize = 4096, .numChannels = 1});
    return lfo;
}

// ---------------------------------------------------------------------------
// The value list and exactness

void reportValues()
{
    std::cout << "\n### The value list (syncNoteValues)\n\n"
                 "Lengths are exact fractions; seconds, hertz, and samples at 120 BPM.\n\n"
                 "| # | Value | Whole notes | Beats | Seconds | Hz | Samples at 44.1 kHz | Samples at 48 kHz |\n"
                 "|---|---|---|---|---|---|---|---|\n";
    for (std::size_t i = 0; i < syncNoteValues.size(); ++i)
    {
        const NoteValue v = syncNoteValues[i];
        std::cout << "| " << i << " | " << nameOf(v) << " | " << fraction(v.wholeNotes()) << " | "
                  << fraction(v.quarterNotes()) << " | " << fixed(noteSeconds(v, 120.0), 4) << " | "
                  << fixed(noteHertz(v, 120.0), 4) << " | " << fixed(noteSamples(v, 120.0, 44100.0), 2) << " | "
                  << fixed(noteSamples(v, 120.0, 48000.0), 2) << " |\n";
    }
    std::cout << "\nRange: 8/1D at 20 BPM is " << sci(noteHertz(NoteValue::dotted(8, 1), 20.0))
              << " Hz; 1/64T at 300 BPM is " << fixed(noteHertz(NoteValue::triplet(1, 64), 300.0), 1)
              << " Hz; at 999 BPM (HostTempo's top) " << fixed(noteHertz(NoteValue::triplet(1, 64), 999.0), 1)
              << " Hz.\n";
}

double multiplierOf(NoteKind kind)
{
    if (kind == NoteKind::triplet)
    {
        return 2.0 / 3.0;
    }
    if (kind == NoteKind::dotted)
    {
        return 1.5;
    }
    return 1.0;
}

float floatMultiplierOf(NoteKind kind)
{
    if (kind == NoteKind::triplet)
    {
        return 2.0f / 3.0f;
    }
    if (kind == NoteKind::dotted)
    {
        return 1.5f;
    }
    return 1.0f;
}

void reportExactness()
{
    std::cout << "\n### Triplets and dotted values: exact, double, float\n\n"
                 "Each identity in samples at 93 BPM and 44.1 kHz. Exact: Ratio. Double and float: a table of "
                 "multipliers (2/3 and 3/2 rounded to the type), a length as base x multiplier x (240 fs / bpm) "
                 "samples; \"sum\" adds the short value n times, \"product\" multiplies it by n.\n\n"
                 "| Identity | Exact | Double, product | Double, sum | Float, product | Float, sum |\n"
                 "|---|---|---|---|---|---|\n";
    const auto lengthD = [](NoteValue v)
    {
        return v.base.toDouble() * multiplierOf(v.kind) * (240.0 * 44100.0 / 93.0);
    };
    const auto lengthF = [](NoteValue v)
    {
        return static_cast<float>(v.base.toDouble()) * floatMultiplierOf(v.kind) * (240.0f * 44100.0f / 93.0f);
    };
    struct Identity
    {
        const char* name = "";
        NoteValue value;
        std::uint64_t times = 1;
        NoteValue equals;
    };
    const std::array identities{
        Identity{
            .name = "3 x 1/8T = 1/4",
            .value = NoteValue::triplet(1, 8),
            .times = 3,
            .equals = NoteValue::straight(1, 4),
        },
        Identity{
            .name = "3 x 1/2T = 1/1",
            .value = NoteValue::triplet(1, 2),
            .times = 3,
            .equals = NoteValue::straight(1, 1),
        },
        Identity{
            .name = "96 x 1/64T = 1/1",
            .value = NoteValue::triplet(1, 64),
            .times = 96,
            .equals = NoteValue::straight(1, 1),
        },
        Identity{
            .name = "2 x 1/8D = 1/4D",
            .value = NoteValue::dotted(1, 8),
            .times = 2,
            .equals = NoteValue::dotted(1, 4),
        },
        Identity{
            .name = "9 x 1/8T = 1/2D",
            .value = NoteValue::triplet(1, 8),
            .times = 9,
            .equals = NoteValue::dotted(1, 2),
        },
    };
    for (const auto& id : identities)
    {
        const bool exact = id.value.wholeNotes() * Ratio{id.times, 1} == id.equals.wholeNotes();
        double sumD = 0.0;
        float sumF = 0.0f;
        for (std::uint64_t i = 0; i < id.times; ++i)
        {
            sumD += lengthD(id.value);
            sumF += lengthF(id.value);
        }
        const auto word = [](bool b)
        {
            return b ? "equal" : "**differs**";
        };
        std::cout << "| " << id.name << " | " << word(exact) << " | "
                  << word(sameBits(static_cast<double>(id.times) * lengthD(id.value), lengthD(id.equals))) << " | "
                  << word(sameBits(sumD, lengthD(id.equals))) << " | "
                  << word(sameBits(static_cast<float>(id.times) * lengthF(id.value), lengthF(id.equals))) << " | "
                  << word(sameBits(sumF, lengthF(id.equals))) << " |\n";
        CHECK(exact);
    }

    // An arpeggiator's steps at 1/8T, 93 BPM, 44.1 kHz for three hours:
    // 294000/31 samples per step, a whole number every 31 steps.
    constexpr double fs = 44100.0;
    const NoteValue step = NoteValue::triplet(1, 8);
    const Ratio exactStep = Ratio{std::uint64_t{60} * 44100, 93} * step.quarterNotes();
    const auto steps = static_cast<std::uint64_t>((3.0 * 3600.0 * fs) / exactStep.toDouble());
    const double doubleStep = noteSamples(step, 93.0, fs);
    const float floatStep = 60.0f * 44100.0f * 4.0f * (1.0f / 8.0f) * (2.0f / 3.0f) / 93.0f;
    double worstDouble = 0.0;
    double worstFloat = 0.0;
    double worstAccumulated = 0.0;
    std::uint64_t offDouble = 0;
    std::uint64_t offFloat = 0;
    std::uint64_t offAccumulated = 0;
    float accumulated = 0.0f;
    for (std::uint64_t k = 0; k < steps; ++k)
    {
        const std::uint64_t exact =
            ((k * exactStep.numerator()) + exactStep.denominator() - 1) / exactStep.denominator();
        const auto viaDouble = static_cast<std::uint64_t>(std::ceil(static_cast<double>(k) * doubleStep));
        const auto viaFloat =
            static_cast<std::uint64_t>(std::ceil(static_cast<double>(k) * static_cast<double>(floatStep)));
        const auto viaAccumulated = static_cast<std::uint64_t>(std::ceil(static_cast<double>(accumulated)));
        accumulated += floatStep;
        const auto e = static_cast<double>(exact);
        worstDouble = std::max(worstDouble, std::abs(static_cast<double>(viaDouble) - e));
        worstFloat = std::max(worstFloat, std::abs(static_cast<double>(viaFloat) - e));
        worstAccumulated = std::max(worstAccumulated, std::abs(static_cast<double>(viaAccumulated) - e));
        offDouble += viaDouble != exact ? 1U : 0U;
        offFloat += viaFloat != exact ? 1U : 0U;
        offAccumulated += viaAccumulated != exact ? 1U : 0U;
    }
    std::cout << "\n**An arpeggiator at 1/8T, 93 BPM, 44.1 kHz for three hours** (" << steps
              << " steps of exactly 294000/31 samples; each step starts at the first sample at or after its time)\n\n"
                 "| Step positions from | Steps on the wrong sample | Worst, in samples |\n|---|---|---|\n"
              << "| exact Ratio, ceil(k x num / den) | 0 | 0 |\n"
              << "| k x noteSamples() (double) | " << offDouble << " | " << fixed(worstDouble, 0) << " |\n"
              << "| k x a float table's length | " << offFloat << " | " << fixed(worstFloat, 0) << " |\n"
              << "| a float position, the length added each time | " << offAccumulated << " | "
              << fixed(worstAccumulated, 0) << " |\n";
}

// ---------------------------------------------------------------------------
// Drift

const std::array<Ratio, 3> driftTempos{Ratio{120, 1}, Ratio{174, 1}, Ratio{375, 4}};
const std::array<std::uint64_t, 4> sampleRates{44100, 48000, 96000, 192000};

enum class RatePath : std::uint8_t
{
    increment, ///< setPhaseIncrement(notePhaseIncrement(...)), as built
    floatHz,   ///< setFrequency(float(noteHertz(...))), for comparison
};

// The largest phase error (cycles) of a synced saw Lfo against the exact
// phase, sampled at the end of every block of 4096 for `seconds`.
double worstDrift(NoteValue v, Ratio bpm, std::uint64_t fs, double seconds, RatePath path)
{
    constexpr std::size_t block = 4096;
    const auto total = static_cast<std::uint64_t>(seconds * static_cast<double>(fs));
    const Ratio q = v.quarterNotes();
    const Ratio perSample = bpm * Ratio{q.denominator(), 60 * fs * q.numerator()};
    REQUIRE(total < std::numeric_limits<std::uint64_t>::max() / perSample.numerator());

    auto lfo = preparedLfo(LfoWaveform::saw, static_cast<double>(fs));
    const std::uint64_t start = lfo.phase();
    if (path == RatePath::increment)
    {
        lfo.setPhaseIncrement(notePhaseIncrement(v, bpm.toDouble(), static_cast<double>(fs)));
    }
    else
    {
        lfo.setFrequency(static_cast<float>(noteHertz(v, bpm.toDouble())));
    }
    std::vector<float> out(block);
    double worst = 0.0;
    std::uint64_t n = 0;
    while (n < total)
    {
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(block, total - n));
        lfo.process(std::span{out}.first(length));
        n += length;
        const std::uint64_t exact =
            start + fixedQuotient((n * perSample.numerator()) % perSample.denominator(), perSample.denominator());
        worst = std::max(worst, std::abs(signedCycles(lfo.phase() - exact)));
    }
    return worst;
}

void reportDrift(double seconds, const char* title)
{
    const std::array values{NoteValue::straight(8, 1), NoteValue::triplet(1, 64)};
    std::cout << "\n### Drift against the exact grid over " << title
              << "\n\nA synced saw Lfo in blocks of 4096: the largest phase error in cycles at any block's end "
                 "against the exact phase, worst over 120, 174, and 93.75 BPM. One cycle is 16 s at 8/1 and 120 "
                 "BPM, and 20.8 ms at 1/64T.\n\n| Rate set through |";
    for (const auto v : values)
    {
        for (const auto fs : sampleRates)
        {
            std::cout << " " << nameOf(v) << ", " << fixed(static_cast<double>(fs) / 1000.0, 1) << " kHz |";
        }
    }
    std::cout << "\n|---|";
    for (std::size_t i = 0; i < values.size() * sampleRates.size(); ++i)
    {
        std::cout << "---|";
    }
    std::cout << "\n";
    for (const auto path : {RatePath::increment, RatePath::floatHz})
    {
        std::cout << "| "
                  << (path == RatePath::increment ? "setPhaseIncrement(notePhaseIncrement()), as built"
                                                  : "setFrequency(float hertz), for comparison")
                  << " |";
        for (const auto v : values)
        {
            for (const auto fs : sampleRates)
            {
                double worst = 0.0;
                for (const Ratio bpm : driftTempos)
                {
                    worst = std::max(worst, worstDrift(v, bpm, fs, seconds, path));
                }
                std::cout << " " << sci(worst) << " |";
                if (path == RatePath::increment)
                {
                    CHECK(worst < 1e-10);
                }
            }
        }
        std::cout << "\n";
    }
}

// ---------------------------------------------------------------------------
// Tempo changes

struct Scenario
{
    const char* name = "";
    std::size_t length = 0;
    std::size_t split = 0;                      ///< the host splits its blocks here (0: never)
    std::function<double(std::size_t)> bpm;     ///< reported at a block's first sample
    std::function<double(std::size_t)> elapsed; ///< quarter notes since sample 0
};

Scenario tempoStep()
{
    return Scenario{
        .name = "tempo step 120 -> 140 at 2 s",
        .length = 288000,
        .split = 96000,
        .bpm = [](std::size_t n) { return n < 96000 ? 120.0 : 140.0; },
        .elapsed =
            [](std::size_t n)
        {
            const auto x = static_cast<double>(n);
            return n < 96000 ? x / 24000.0 : 4.0 + ((x - 96000.0) * 140.0 / (60.0 * rateD));
        },
    };
}

Scenario tempoRamp()
{
    // 120 -> 160 BPM linearly from 1 s to 7 s.
    const auto quarters = [](double t)
    {
        if (t < 1.0)
        {
            return 2.0 * t;
        }
        const double u = std::min(t, 7.0) - 1.0;
        const double during = 2.0 + (((120.0 * u) + (40.0 * u * u / 12.0)) / 60.0);
        return t < 7.0 ? during : during + ((t - 7.0) * 160.0 / 60.0);
    };
    return Scenario{
        .name = "tempo ramp 120 -> 160 over 6 s",
        .length = 432000,
        .split = 0,
        .bpm =
            [](std::size_t n)
        {
            const double t = static_cast<double>(n) / rateD;
            if (t < 1.0)
            {
                return 120.0;
            }
            return t < 7.0 ? 120.0 + (40.0 * (t - 1.0) / 6.0) : 160.0;
        },
        .elapsed = [=](std::size_t n) { return quarters(static_cast<double>(n) / rateD); },
    };
}

struct Render
{
    std::vector<float> out;
    double worstLag = 0.0; ///< cycles from "start + quarters played", at any block start
};

// A synced Lfo as a voice drives it: once per block, the host's tempo into
// HostTempo, the increment into the LFO.
Render render(const Scenario& s, NoteValue v, std::size_t blockSize, LfoWaveform waveform = LfoWaveform::sine)
{
    auto lfo = preparedLfo(waveform, rateD);
    HostTempo tempo;
    Render r;
    r.out.assign(s.length, 0.0f);
    const std::uint64_t start = lfo.phase();
    const double cycleQuarters = v.quarterNotes().toDouble();
    std::size_t n = 0;
    while (n < s.length)
    {
        std::size_t end = std::min(n + blockSize, s.length);
        if (n < s.split && s.split < end)
        {
            end = s.split;
        }
        const double cycles = s.elapsed(n) / cycleQuarters;
        const double scaled = std::ldexp(cycles - std::floor(cycles), 64); // in [0, 2^64]
        const std::uint64_t reference = start + (scaled < 0x1p64 ? static_cast<std::uint64_t>(scaled) : 0U);
        r.worstLag = std::max(r.worstLag, std::abs(signedCycles(lfo.phase() - reference)));
        tempo.report(s.bpm(n));
        lfo.setPhaseIncrement(notePhaseIncrement(v, tempo.bpm(), rateD));
        lfo.process(std::span{r.out}.subspan(n, end - n));
        n = end;
    }
    return r;
}

double largestStep(std::span<const float> y, std::size_t from, std::size_t to)
{
    double worst = 0.0;
    for (std::size_t n = std::max<std::size_t>(from, 1); n < to; ++n)
    {
        worst = std::max(worst, static_cast<double>(std::abs(y[n] - y[n - 1])));
    }
    return worst;
}

void reportTempoChanges()
{
    std::cout
        << "\n### Tempo changes\n\n"
           "48 kHz, a sine LFO, note-on at sample 0, the tempo into HostTempo and the increment into the LFO "
           "once per block (512). Step: the largest sample-to-sample change from 64 samples before the step to "
           "4096 after, against the sine's own largest slope at 140 BPM. Click: energy above 1 kHz around the "
           "step, dB re an instant unit step, and the same measure 2 s later where nothing happens (the sine's "
           "own floor: its interpolation on the LFO's 32-sample grid). Lag: from \"note-on phase + quarter "
           "notes played\".\n\n"
           "| Value | Step (x slope) | Click at the step (dB) | The same, 2 s later (dB) | Lag at any block start "
           "(cycles) |\n|---|---|---|---|---|\n";
    const Scenario step = tempoStep();
    for (const NoteValue v : {NoteValue::straight(1, 4), NoteValue::triplet(1, 64)})
    {
        const Render r = render(step, v, 512);
        const double slope = 2.0 * std::numbers::pi * noteHertz(v, 140.0) / rateD;
        std::cout << "| " << nameOf(v) << " | "
                  << fixed(largestStep(r.out, step.split - 64, step.split + 4096) / slope, 3) << " | "
                  << db(clickDb(r.out, step.split)) << " | " << db(clickDb(r.out, step.split + 96000)) << " | "
                  << sci(r.worstLag) << " |\n";
    }

    std::cout << "\n**A tempo ramp** (120 -> 160 BPM over 6 s, reported at each block's start): the largest lag from "
                 "\"note-on phase + quarter notes played\", in cycles (and ms at 120 BPM), by block size\n\n"
                 "| Value | 32 | 128 | 512 | 2048 | 4096 |\n|---|---|---|---|---|---|\n";
    const Scenario ramp = tempoRamp();
    for (const NoteValue v : {NoteValue::straight(1, 4), NoteValue::triplet(1, 64)})
    {
        std::cout << "| " << nameOf(v) << " |";
        for (const std::size_t size : {32U, 128U, 512U, 2048U, 4096U})
        {
            const double lag = render(ramp, v, size).worstLag;
            std::cout << " " << sci(lag) << " (" << fixed(lag * noteSeconds(v, 120.0) * 1000.0, 2) << ") |";
        }
        std::cout << "\n";
    }
}

// ---------------------------------------------------------------------------
// Block sizes, subnormals, allocation

void reportBlockSizes()
{
    const Scenario steady{
        .name = "steady",
        .length = 480000,
        .split = 0,
        .bpm = [](std::size_t) { return 128.0; },
        .elapsed = [](std::size_t n) { return static_cast<double>(n) * 128.0 / (60.0 * rateD); },
    };
    const std::array scenarios{steady, tempoStep(), tempoRamp()};
    std::cout << "\n### Block sizes 1, 37, 4096\n\n"
                 "A sine at 1/16 and a saw at 1/4D, rendered in blocks of 1, 37, and 4096 (split at the tempo step). "
                 "\"identical\" if every sample matches bit for bit, else the largest difference.\n\n"
                 "| Row | Steady 128 BPM | Tempo step | Tempo ramp |\n|---|---|---|---|\n";
    std::size_t subnormals = 0;
    for (const auto& [value, waveform, label] : {
             std::tuple{NoteValue::straight(1, 16), LfoWaveform::sine, "sine 1/16"},
             std::tuple{NoteValue::dotted(1, 4), LfoWaveform::saw, "saw 1/4D"},
         })
    {
        std::cout << "| " << label << " |";
        for (const Scenario& s : scenarios)
        {
            const Render a = render(s, value, 1, waveform);
            const Render b = render(s, value, 37, waveform);
            const Render c = render(s, value, 4096, waveform);
            double worst = 0.0;
            bool identical = true;
            for (std::size_t n = 0; n < a.out.size(); ++n)
            {
                worst = std::max(worst, static_cast<double>(std::abs(a.out[n] - b.out[n])));
                worst = std::max(worst, static_cast<double>(std::abs(a.out[n] - c.out[n])));
                identical = identical && sameBits(a.out[n], b.out[n]) && sameBits(a.out[n], c.out[n]);
                subnormals += std::fpclassify(a.out[n]) == FP_SUBNORMAL ? 1U : 0U;
                subnormals += std::fpclassify(c.out[n]) == FP_SUBNORMAL ? 1U : 0U;
            }
            std::cout << " " << (identical ? std::string{"identical"} : sci(worst)) << " |";
        }
        std::cout << "\n";
    }
    std::cout << "\nSubnormal outputs: " << subnormals << ".\n";
    CHECK(subnormals == 0);

    auto lfo = preparedLfo(LfoWaveform::triangle, rateD);
    HostTempo tempo;
    std::array<float, 512> block{};
    std::size_t allocations = 0;
    {
        const AllocationCounter counter;
        for (std::size_t i = 0; i < 2000; ++i)
        {
            tempo.report(60.0 + static_cast<double>(i % 200));
            lfo.setPhaseIncrement(notePhaseIncrement(syncNoteValues[i % syncNoteValues.size()], tempo.bpm(), rateD));
            lfo.process(block);
        }
        allocations = counter.count();
    }
    std::cout << "Allocations while processing (2000 blocks of tempo reports, every list value): " << allocations
              << ".\n";
    CHECK(allocations == 0);
}

} // namespace

TEST_CASE("Tempo sync report: as built", "[.][sync-report]")
{
    std::cout << "\n## Tempo-sync report (as built)\n";
    reportValues();
    reportExactness();
    reportDrift(60.0, "one minute");
    reportTempoChanges();
    reportBlockSizes();
}

TEST_CASE("Tempo sync report: three hours", "[.][sync-long-report]")
{
    std::cout << "\n## Tempo-sync report: three hours\n";
    reportDrift(3.0 * 3600.0, "three hours");
}
