// The tempo-sync report: candidate models of a tempo-synced LFO
// (tempo_sync_candidates.hpp, docs/dsp/tempo-sync.md), measured against a
// simulated host. It measures:
//
// - the value list: every candidate value's length, exactly, its seconds,
//   hertz, and samples; the two candidate orders; the developer's worked
//   examples; triplet and dotted identities in exact, double, and float
//   arithmetic; and an arpeggiator's step positions over three hours;
// - drift against the host's grid over one minute (and three hours in the
//   long report) at 8/1 and 1/64T, 44.1-192 kHz, three tempos, with a host
//   that computes its position from its sample count and one that
//   accumulates it per block;
// - events: a tempo step, a tempo ramp, a loop, a locate, and the transport
//   starting and stopping: jumps, the largest step, the click, and where
//   each model ends against the note-on and the bar grid;
// - bit-identity across block sizes 1, 37, and 4096; subnormals; allocation.
//
// Hidden from the default run; run it in an optimised build:
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[sync-report]"
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[sync-long-report]"
//
// CPU: the "[sync]" benchmarks (tools/bench.sh "[sync]").

#include "../support/allocation_counter.hpp"
#include "tempo_sync_candidates.hpp"

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
using namespace anoesis::test::sync;

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

// Call f(model tag, name) for every model.
template <typename F>
void forEachModel(const F& f)
{
    f(RateOnly(), RateOnly::name);
    f(RateOnlyFloatHz(), RateOnlyFloatHz::name);
    f(Locked(), Locked::name);
    f(Hybrid(), Hybrid::name);
    f(NoteAnchored(), NoteAnchored::name);
}

// ---------------------------------------------------------------------------
// The value list

void reportValues()
{
    constexpr auto sorted = sortedValues();
    constexpr auto grouped = groupedValues();

    std::cout
        << "\n### The value list\n\n"
           "Candidate: every straight value from 8/1 to 1/64 with its dotted and triplet value, 30 in all, longest "
           "first. Lengths are exact fractions; seconds, hertz, and samples at 120 BPM.\n\n"
           "| # | Value | Whole notes | Beats | Seconds | Hz | Samples at 44.1 kHz | Samples at 48 kHz |\n"
           "|---|---|---|---|---|---|---|---|\n";
    for (std::size_t i = 0; i < sorted.size(); ++i)
    {
        const NoteValue v = sorted[i];
        std::cout << "| " << i << " | " << nameOf(v) << " | " << fraction(v.wholeNotes()) << " | "
                  << fraction(v.quarterNotes()) << " | " << fixed(seconds(v, 120.0), 4) << " | "
                  << fixed(hertz(v, 120.0), 4) << " | " << fixed(samples(v, 120.0, 44100.0), 2) << " | "
                  << fixed(samples(v, 120.0, 48000.0), 2) << " |\n";
    }

    std::size_t groupedReversals = 0;
    for (std::size_t i = 1; i < grouped.size(); ++i)
    {
        groupedReversals += grouped[i].wholeNotes() > grouped[i - 1].wholeNotes() ? 1U : 0U;
    }
    bool sortedStrict = true;
    for (std::size_t i = 1; i < sorted.size(); ++i)
    {
        sortedStrict = sortedStrict && sorted[i].wholeNotes() < sorted[i - 1].wholeNotes();
    }
    std::cout << "\nOrder: sorted by length, every step shorter than the last: " << (sortedStrict ? "yes" : "no")
              << ". Grouped by base (D, straight, T), steps that get longer: " << groupedReversals
              << " (each base's dotted value is longer than the previous base's triplet, e.g. 1/2T = 1/3 then 1/4D "
                 "= 3/8).\n";
    CHECK(sortedStrict);

    // The extremes against the LFO's range (inventory L6: 0.1-100 Hz free).
    std::cout << "\nRange: 8/1D at 20 BPM is " << sci(hertz(dotted(8, 1), 20.0)) << " Hz; 1/64T at 300 BPM is "
              << fixed(hertz(triplet(1, 64), 300.0), 1) << " Hz; at 999 BPM " << fixed(hertz(triplet(1, 64), 999.0), 1)
              << " Hz.\n";
}

void reportExactness()
{
    std::cout << "\n### Triplets and dotted values: exact, double, float\n\n"
                 "Each identity in samples at 93 BPM and 44.1 kHz (a tempo whose lengths are not dyadic). Exact: the "
                 "Ratio arithmetic. Double and float: a table of multipliers (2/3 and 3/2 rounded to the type), a "
                 "length as base x multiplier x (240 fs / bpm) samples; \"sum\" adds the short value n times, "
                 "\"product\" multiplies it by n.\n\n"
                 "| Identity | Exact | Double, product | Double, sum | Float, product | Float, sum |\n"
                 "|---|---|---|---|---|---|\n";

    const auto lengthD = [](NoteValue v)
    {
        const double base = v.base.toDouble();
        double multiplier = 1.0;
        if (v.kind == NoteKind::triplet)
        {
            multiplier = 2.0 / 3.0;
        }
        else if (v.kind == NoteKind::dotted)
        {
            multiplier = 1.5;
        }
        return base * multiplier * (240.0 * 44100.0 / 93.0);
    };
    const auto lengthF = [](NoteValue v)
    {
        const auto base = static_cast<float>(v.base.toDouble());
        float multiplier = 1.0f;
        if (v.kind == NoteKind::triplet)
        {
            multiplier = 2.0f / 3.0f;
        }
        else if (v.kind == NoteKind::dotted)
        {
            multiplier = 1.5f;
        }
        return base * multiplier * (240.0f * 44100.0f / 93.0f);
    };
    struct Identity
    {
        const char* name = "";
        NoteValue value;
        std::uint64_t times = 1;
        NoteValue equals;
    };
    const std::array identities{
        Identity{.name = "3 x 1/8T = 1/4", .value = triplet(1, 8), .times = 3, .equals = straight(1, 4)},
        Identity{.name = "3 x 1/16T = 1/8", .value = triplet(1, 16), .times = 3, .equals = straight(1, 8)},
        Identity{.name = "3 x 1/2T = 1/1", .value = triplet(1, 2), .times = 3, .equals = straight(1, 1)},
        Identity{.name = "96 x 1/64T = 1/1", .value = triplet(1, 64), .times = 96, .equals = straight(1, 1)},
        Identity{.name = "3 x 2/1T = 4/1", .value = triplet(2, 1), .times = 3, .equals = straight(4, 1)},
        Identity{.name = "2 x 1/4D = 1/2D", .value = dotted(1, 4), .times = 2, .equals = dotted(1, 2)},
        Identity{.name = "2 x 1/8D = 1/4D", .value = dotted(1, 8), .times = 2, .equals = dotted(1, 4)},
        Identity{.name = "2 x 1/8D = 3/8", .value = dotted(1, 8), .times = 2, .equals = straight(3, 8)},
        Identity{.name = "9 x 1/8T = 2 x 1/4D", .value = triplet(1, 8), .times = 9, .equals = dotted(1, 2)},
    };
    std::size_t doubleMisses = 0;
    std::size_t floatMisses = 0;
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
        const bool productD = sameBits(static_cast<double>(id.times) * lengthD(id.value), lengthD(id.equals));
        const bool productF = sameBits(static_cast<float>(id.times) * lengthF(id.value), lengthF(id.equals));
        const bool summedD = sameBits(sumD, lengthD(id.equals));
        const bool summedF = sameBits(sumF, lengthF(id.equals));
        doubleMisses += (productD ? 0U : 1U) + (summedD ? 0U : 1U);
        floatMisses += (productF ? 0U : 1U) + (summedF ? 0U : 1U);
        const auto word = [](bool b)
        {
            return b ? "equal" : "**differs**";
        };
        std::cout << "| " << id.name << " | " << word(exact) << " | " << word(productD) << " | " << word(summedD)
                  << " | " << word(productF) << " | " << word(summedF) << " |\n";
        CHECK(exact);
    }
    std::cout << "\nMisses: double " << doubleMisses << " of " << (2 * identities.size()) << ", float " << floatMisses
              << " of " << (2 * identities.size()) << ".\n";

    // The developer's worked numbers (KVR p=4006903): 8/1 = 32 beats,
    // 2/1 = 8, 2/1T = 16/3, 2/1D = 12.
    CHECK(straight(8, 1).quarterNotes() == Ratio{32, 1});
    CHECK(straight(2, 1).quarterNotes() == Ratio{8, 1});
    CHECK(triplet(2, 1).quarterNotes() == Ratio{16, 3});
    CHECK(dotted(2, 1).quarterNotes() == Ratio{12, 1});
    std::cout << "\nThe developer's examples (8/1 = 32 beats, 2/1 = 8, 2/1T = 16/3, 2/1D = 12): all exact.\n";

    // An arpeggiator's steps at 1/8T, 93 BPM, 44.1 kHz for three hours:
    // 294000/31 = 9483.87... samples per step, a whole number every 31.
    constexpr double fs = 44100.0;
    constexpr double bpm = 93.0;
    const NoteValue step = triplet(1, 8);
    const Ratio q = step.quarterNotes();
    // Samples per step = 60 fs q / bpm, exactly num/den.
    const Ratio exactStep = Ratio{std::uint64_t{60} * 44100, 93} * q;
    const auto steps = static_cast<std::uint64_t>((3.0 * 3600.0 * fs) / exactStep.toDouble());
    const double doubleStep = 60.0 * fs * 4.0 * (1.0 / 8.0) * (2.0 / 3.0) / bpm;
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
        // Each step starts at the first whole sample at or after its time.
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
              << " steps of exactly 294000/31 samples; each step starts at the first sample at or after its time; "
                 "against the exact rational position)\n\n"
                 "| Step positions from | Steps on the wrong sample | Worst, in samples |\n|---|---|---|\n"
              << "| exact Ratio | 0 | 0 |\n"
              << "| k x samples per step, double table | " << offDouble << " | " << fixed(worstDouble, 0) << " |\n"
              << "| k x samples per step, float table | " << offFloat << " | " << fixed(worstFloat, 0) << " |\n"
              << "| a float position, step added each time | " << offAccumulated << " | " << fixed(worstAccumulated, 0)
              << " |\n";
}

// ---------------------------------------------------------------------------
// Drift against the host's grid

struct Tempo
{
    Ratio bpm;
    const char* name = "";
};

const std::array tempos{
    Tempo{.bpm = Ratio{120, 1}, .name = "120"},
    Tempo{.bpm = Ratio{174, 1}, .name = "174"},
    Tempo{.bpm = Ratio{1999, 20}, .name = "99.95"},
};

const std::array<double, 4> sampleRates{44100.0, 48000.0, 96000.0, 192000.0};

/// How the simulated host reports its position.
enum class HostKind : std::uint8_t
{
    computed,    ///< ppq = samples x bpm / (60 fs), in double
    accumulated, ///< ppq += block x bpm / (60 fs), in double, every block
};

// The largest phase error (cycles) of a model against the exact phase of a
// note-on at sample 0 and PPQ 0, playing at a constant tempo, sampled every
// `every` blocks of 512 and at the end.
template <typename Model>
double worstDrift(NoteValue v, Ratio bpm, double fs, double secondsLong, HostKind host)
{
    constexpr std::size_t block = 512;
    constexpr std::size_t every = 64;
    const auto total = static_cast<std::uint64_t>(secondsLong * fs);
    const double bpmD = bpm.toDouble();
    // Cycles per sample = bpm.num q.den / (bpm.den 60 fs q.num) = a / b.
    const Ratio q = v.quarterNotes();
    const Ratio perSample = Ratio{bpm.numerator() * q.denominator(), 1} *
                            Ratio{1, bpm.denominator() * 60 * static_cast<std::uint64_t>(fs) * q.numerator()};
    const std::uint64_t a = perSample.numerator();
    const std::uint64_t b = perSample.denominator();
    REQUIRE(b < (std::uint64_t{1} << 63U));
    REQUIRE(total < std::numeric_limits<std::uint64_t>::max() / a);

    SyncedLfo<Model> lfo;
    lfo.prepare(fs);
    lfo.setNote(v);
    lfo.noteOn();
    double worst = 0.0;
    double ppq = 0.0;
    std::uint64_t n = 0;
    std::size_t blocks = 0;
    const auto measure = [&]
    {
        const std::uint64_t exact = fixedQuotient((n * a) % b, b).quotient;
        worst = std::max(worst, std::abs(signedCycles(lfo.phase() - exact)));
    };
    while (n < total)
    {
        const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(block, total - n));
        lfo.advance(length, HostClock{.bpm = bpmD, .ppq = ppq, .playing = true});
        n += length;
        if (host == HostKind::computed)
        {
            ppq = static_cast<double>(n) * bpmD / (60.0 * fs);
        }
        else
        {
            ppq += static_cast<double>(length) * bpmD / (60.0 * fs);
        }
        if (++blocks % every == 0)
        {
            measure();
        }
    }
    measure();
    return worst;
}

void reportDrift(double secondsLong, const char* title)
{
    const std::array values{straight(8, 1), triplet(1, 64)};
    std::cout << "\n### Drift against the host's grid over " << title
              << "\n\nThe largest phase error in cycles (sampled every 64 blocks of 512 and at the end) against the "
                 "exact phase, worst over 120, 174, and 99.95 BPM, for a note-on at PPQ 0. One cycle is 16 s at 8/1 "
                 "and 120 BPM, and 20.8 ms at 1/64T. A host that computes its position from its sample count, and "
                 "one that adds each block's length to it.\n\n"
                 "| Model | Host |";
    for (const auto v : values)
    {
        for (const double fs : sampleRates)
        {
            std::cout << " " << nameOf(v) << ", " << fixed(fs / 1000.0, 1) << " kHz |";
        }
    }
    std::cout << "\n|---|---|";
    for (std::size_t i = 0; i < values.size() * sampleRates.size(); ++i)
    {
        std::cout << "---|";
    }
    std::cout << "\n";
    forEachModel(
        [&](auto model, const char* name)
        {
            using Model = decltype(model);
            for (const HostKind host : {HostKind::computed, HostKind::accumulated})
            {
                std::cout << "| " << name << " | " << (host == HostKind::computed ? "computed" : "accumulated") << " |";
                for (const auto v : values)
                {
                    for (const double fs : sampleRates)
                    {
                        double worst = 0.0;
                        for (const auto& t : tempos)
                        {
                            worst = std::max(worst, worstDrift<Model>(v, t.bpm, fs, secondsLong, host));
                        }
                        std::cout << " " << sci(worst) << " |";
                    }
                }
                std::cout << "\n";
            }
        });
}

// ---------------------------------------------------------------------------
// Events

struct Scenario
{
    const char* name;
    std::size_t length;
    std::vector<std::size_t> splits; ///< the host splits its blocks here
    std::function<HostClock(std::size_t)> clock;
    std::function<double(std::size_t)> elapsed; ///< quarters since sample 0, by tempo
    std::size_t eventAt;                        ///< 0: no single event
};

constexpr double quartersPerSample120 = 120.0 / (60.0 * rateD);

std::vector<Scenario> scenarios()
{
    std::vector<Scenario> list;
    // A tempo step: 120 -> 140 BPM at 2 s.
    list.push_back(Scenario{
        .name = "tempo step 120 -> 140 at 2 s",
        .length = 288000,
        .splits = {96000},
        .clock =
            [](std::size_t n)
        {
            const auto x = static_cast<double>(n);
            if (n < 96000)
            {
                return HostClock{.bpm = 120.0, .ppq = x * quartersPerSample120, .playing = true};
            }
            return HostClock{.bpm = 140.0, .ppq = 4.0 + ((x - 96000.0) * 140.0 / (60.0 * rateD)), .playing = true};
        },
        .elapsed =
            [](std::size_t n)
        {
            const auto x = static_cast<double>(n);
            return n < 96000 ? x * quartersPerSample120 : 4.0 + ((x - 96000.0) * 140.0 / (60.0 * rateD));
        },
        .eventAt = 96000,
    });
    // A ramp: 120 -> 160 BPM linearly from 1 s to 7 s.
    const auto rampBpm = [](double t)
    {
        if (t < 1.0)
        {
            return 120.0;
        }
        return t < 7.0 ? 120.0 + (40.0 * (t - 1.0) / 6.0) : 160.0;
    };
    const auto rampQuarters = [](double t)
    {
        if (t < 1.0)
        {
            return 2.0 * t;
        }
        const double u = std::min(t, 7.0) - 1.0;
        const double during = 2.0 + (((120.0 * u) + (40.0 * u * u / 12.0)) / 60.0);
        return t < 7.0 ? during : during + ((t - 7.0) * 160.0 / 60.0);
    };
    list.push_back(Scenario{
        .name = "tempo ramp 120 -> 160 over 6 s",
        .length = 432000,
        .splits = {},
        .clock =
            [=](std::size_t n)
        {
            const double t = static_cast<double>(n) / rateD;
            return HostClock{.bpm = rampBpm(t), .ppq = rampQuarters(t), .playing = true};
        },
        .elapsed = [=](std::size_t n) { return rampQuarters(static_cast<double>(n) / rateD); },
        .eventAt = 0,
    });
    // A 4-bar loop at 120 BPM: back to PPQ 0 at 8 s.
    list.push_back(Scenario{
        .name = "4-bar loop at 8 s",
        .length = 480000,
        .splits = {384000},
        .clock =
            [](std::size_t n)
        {
            const auto x = static_cast<double>(n % 384000);
            return HostClock{.bpm = 120.0, .ppq = x * quartersPerSample120, .playing = true};
        },
        .elapsed = [](std::size_t n) { return static_cast<double>(n) * quartersPerSample120; },
        .eventAt = 384000,
    });
    // A locate while playing: PPQ 8 -> 33.37 at 4 s.
    list.push_back(Scenario{
        .name = "locate 8 -> 33.37 at 4 s",
        .length = 288000,
        .splits = {192000},
        .clock =
            [](std::size_t n)
        {
            const auto x = static_cast<double>(n);
            const double ppq = n < 192000 ? x * quartersPerSample120 : 33.37 + ((x - 192000.0) * quartersPerSample120);
            return HostClock{.bpm = 120.0, .ppq = ppq, .playing = true};
        },
        .elapsed = [](std::size_t n) { return static_cast<double>(n) * quartersPerSample120; },
        .eventAt = 192000,
    });
    // The transport starts at 1 s from PPQ 5.3, a note held from 0.
    list.push_back(Scenario{
        .name = "transport starts at 1 s (PPQ 5.3)",
        .length = 240000,
        .splits = {48000},
        .clock =
            [](std::size_t n)
        {
            if (n < 48000)
            {
                return HostClock{.bpm = 120.0, .ppq = 5.3, .playing = false};
            }
            const double ppq = 5.3 + (static_cast<double>(n - 48000) * quartersPerSample120);
            return HostClock{.bpm = 120.0, .ppq = ppq, .playing = true};
        },
        .elapsed = [](std::size_t n) { return static_cast<double>(n) * quartersPerSample120; },
        .eventAt = 48000,
    });
    // The transport stops at 2 s; the note goes on.
    list.push_back(Scenario{
        .name = "transport stops at 2 s",
        .length = 240000,
        .splits = {96000},
        .clock =
            [](std::size_t n)
        {
            const auto x = static_cast<double>(std::min<std::size_t>(n, 96000));
            return HostClock{.bpm = 120.0, .ppq = x * quartersPerSample120, .playing = n < 96000};
        },
        .elapsed = [](std::size_t n) { return static_cast<double>(n) * quartersPerSample120; },
        .eventAt = 96000,
    });
    return list;
}

struct Render
{
    std::vector<float> out;
    std::uint64_t phase = 0;     ///< at the end
    std::size_t jumps = 0;       ///< after the first block
    double worstNoteError = 0.0; ///< against the note-on reference, at every block start
};

template <typename Model>
Render render(const Scenario& s, NoteValue v, std::size_t blockSize, dsp::LfoWaveform waveform = dsp::LfoWaveform::sine)
{
    SyncedLfo<Model> lfo;
    lfo.prepare(rateD);
    lfo.setWaveform(waveform);
    lfo.setNote(v);
    lfo.noteOn();
    Render r;
    r.out.assign(s.length, 0.0f);
    const double cycleQuarters = v.quarterNotes().toDouble();
    std::size_t n = 0;
    std::size_t firstJumps = 0;
    auto split = s.splits.begin();
    while (n < s.length)
    {
        std::size_t end = std::min(n + blockSize, s.length);
        while (split != s.splits.end() && *split <= n)
        {
            ++split;
        }
        if (split != s.splits.end() && *split < end)
        {
            end = *split;
        }
        const double cycles = s.elapsed(n) / cycleQuarters;
        const std::uint64_t reference = toFixed(cycles - std::floor(cycles));
        r.worstNoteError = std::max(r.worstNoteError, std::abs(signedCycles(lfo.phase() - reference)));
        lfo.process(std::span{r.out}.subspan(n, end - n), s.clock(n));
        if (n == 0)
        {
            firstJumps = lfo.jumps();
        }
        n = end;
    }
    r.phase = lfo.phase();
    r.jumps = lfo.jumps() - firstJumps;
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

void reportEvents()
{
    const auto list = scenarios();
    struct Case
    {
        std::size_t scenario = 0;
        NoteValue value;
    };
    const std::array cases{
        Case{.scenario = 0, .value = straight(1, 4)}, Case{.scenario = 0, .value = triplet(1, 64)},
        Case{.scenario = 1, .value = straight(1, 4)}, Case{.scenario = 1, .value = triplet(1, 64)},
        Case{.scenario = 2, .value = straight(1, 4)}, Case{.scenario = 2, .value = dotted(1, 4)},
        Case{.scenario = 2, .value = straight(8, 1)}, Case{.scenario = 3, .value = straight(1, 4)},
        Case{.scenario = 4, .value = straight(1, 4)}, Case{.scenario = 4, .value = dotted(1, 4)},
        Case{.scenario = 5, .value = straight(1, 4)},
    };
    std::cout
        << "\n### Events\n\n"
           "48 kHz, blocks of 512 (the host splits a block at a tempo step, loop, locate, or start/stop), a sine "
           "LFO, note-on at sample 0. Jumps: phase sets after the first block. Step: the largest sample-to-sample "
           "change from 8 samples before the event to 4096 after, against the sine's own largest slope at the "
           "faster tempo. Click: the LFO's energy above 1 kHz around the event, dB re an instant unit step. "
           "Worst vs note: the largest phase error at any block start against \"note-on phase + quarters "
           "played\". End vs grid: the phase at the end against the bar grid (cycles counted from PPQ 0), when "
           "playing.\n\n"
           "| Event | Value | Model | Jumps | Step (x slope) | Click (dB) | Worst vs note (cycles) | End vs grid "
           "(cycles) |\n|---|---|---|---|---|---|---|---|\n";
    for (const auto& c : cases)
    {
        const Scenario& s = list[c.scenario];
        forEachModel(
            [&](auto model, const char* name)
            {
                using Model = decltype(model);
                const Render r = render<Model>(s, c.value, 512);
                const HostClock endClock = s.clock(s.length);
                std::string grid = "(stopped)";
                if (endClock.playing)
                {
                    grid = sci(std::abs(signedCycles(r.phase - gridPhase(endClock.ppq, c.value))));
                }
                std::string step = "-";
                std::string click = "-";
                if (s.eventAt > 0)
                {
                    const double fastest = std::max(s.clock(0).bpm, s.clock(s.length - 1).bpm);
                    const double slope = 2.0 * std::numbers::pi * hertz(c.value, fastest) / rateD;
                    step = fixed(largestStep(r.out, s.eventAt - 8, s.eventAt + 4096) / slope, 2);
                    click = db(clickDb(r.out, s.eventAt));
                }
                std::cout << "| " << s.name << " | " << nameOf(c.value) << " | " << name << " | " << r.jumps << " | "
                          << step << " | " << click << " | " << sci(r.worstNoteError) << " | " << grid << " |\n";
            });
    }
}

// ---------------------------------------------------------------------------
// Block sizes, subnormals, allocation

void reportBlockSizes()
{
    const auto list = scenarios();
    // Constant tempo, playing, for 10 s.
    const Scenario steady{
        .name = "steady 120 BPM",
        .length = 480000,
        .splits = {},
        .clock = [](std::size_t n)
        { return HostClock{.bpm = 120.0, .ppq = static_cast<double>(n) * quartersPerSample120, .playing = true}; },
        .elapsed = [](std::size_t n) { return static_cast<double>(n) * quartersPerSample120; },
        .eventAt = 0,
    };
    std::cout
        << "\n### Block sizes 1, 37, 4096\n\n"
           "A sine LFO at 1/16 (and a saw at 1/4D, which jumps), the same scenario rendered in blocks of 1, 37, "
           "and 4096 (split at the host's events). \"identical\" if every sample of both matches bit for bit; "
           "else the largest difference of the one that doesn't. Subnormal outputs are counted over every render.\n\n"
           "| Model | Steady | Tempo step | Ramp | Loop | Locate | Start | Stop |\n"
           "|---|---|---|---|---|---|---|---|\n";
    std::size_t subnormals = 0;
    forEachModel(
        [&](auto model, const char* name)
        {
            using Model = decltype(model);
            std::cout << "| " << name << " |";
            std::vector<const Scenario*> all{&steady};
            for (const auto& s : list)
            {
                all.push_back(&s);
            }
            for (const Scenario* s : all)
            {
                std::string cell;
                for (const auto& [value, waveform, label] : {
                         std::tuple{straight(1, 16), dsp::LfoWaveform::sine, "sine"},
                         std::tuple{dotted(1, 4), dsp::LfoWaveform::saw, "saw"},
                     })
                {
                    const Render a = render<Model>(*s, value, 1, waveform);
                    const Render b = render<Model>(*s, value, 37, waveform);
                    const Render c = render<Model>(*s, value, 4096, waveform);
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
                    if (!identical)
                    {
                        cell += std::string{cell.empty() ? "" : ", "} + label + " " + sci(worst);
                    }
                }
                std::cout << " " << (cell.empty() ? std::string{"identical"} : cell) << " |";
            }
            std::cout << "\n";
        });
    // The ramp's error against the note-on reference by block size.
    std::cout << "\n**A tempo ramp against the block size** (120 -> 160 BPM over 6 s, 1/64T, the largest phase error "
                 "in cycles at any block start; the host reports the tempo at each block's start)\n\n"
                 "| Model | 32 | 128 | 512 | 2048 | 4096 |\n|---|---|---|---|---|---|\n";
    forEachModel(
        [&](auto model, const char* name)
        {
            using Model = decltype(model);
            std::cout << "| " << name << " |";
            for (const std::size_t size : {32U, 128U, 512U, 2048U, 4096U})
            {
                std::cout << " " << sci(render<Model>(list[1], triplet(1, 64), size).worstNoteError) << " |";
            }
            std::cout << "\n";
        });
    std::cout << "\nSubnormal outputs: " << subnormals << ".\n";
    CHECK(subnormals == 0);

    // Allocation: every model, through tempo changes, a loop, start and stop.
    std::array<float, 512> block{};
    std::size_t allocations = 0;
    forEachModel(
        [&](auto model, const char*)
        {
            using Model = decltype(model);
            SyncedLfo<Model> lfo;
            lfo.prepare(rateD);
            lfo.setNote(dotted(1, 8));
            lfo.noteOn();
            const AllocationCounter counter;
            for (std::size_t i = 0; i < 2000; ++i)
            {
                const double bpm = 100.0 + static_cast<double>(i % 7);
                const double ppq = static_cast<double>(i % 300) * 0.0213;
                lfo.process(block, HostClock{.bpm = bpm, .ppq = ppq, .playing = (i / 500) % 2 == 0});
                lfo.advance(block.size(), HostClock{.bpm = bpm, .ppq = ppq + 0.01, .playing = true});
            }
            allocations += counter.count();
        });
    std::cout << "Allocations while processing (every model, 2000 blocks of tempo changes, jumps, starts and stops): "
              << allocations << ".\n";
    CHECK(allocations == 0);
}

} // namespace

TEST_CASE("Tempo sync report: candidates", "[.][sync-report]")
{
    std::cout << "\n## Tempo-sync report (candidates)\n";
    reportValues();
    reportExactness();
    reportDrift(60.0, "one minute");
    reportEvents();
    reportBlockSizes();
}

TEST_CASE("Tempo sync report: three hours", "[.][sync-long-report]")
{
    std::cout << "\n## Tempo-sync report: three hours\n";
    reportDrift(3.0 * 3600.0, "three hours");
}
