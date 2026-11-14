// The D6 measurement report: every anti-aliasing prototype, measured with the
// test harness (measureAliasing, measureHarmonics, powerSpectrum) and printed
// as Markdown tables. Hidden from the default run; run it in an optimised
// build (about a minute):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[d6-report]"
//
// Results and the decision: docs/dsp/band-limited-oscillators.md

#include "prototype_support.hpp"

#include <anoesis/test/measure.hpp>
#include <anoesis/test/signals.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;

namespace
{

constexpr std::size_t n = 65536;
constexpr double audible = 20000.0;
constexpr int topTwoOctaves = 104; // MIDI 104..127: 3.3 to 12.5 kHz

const char* shapeName(proto::Shape shape)
{
    switch (shape)
    {
    case proto::Shape::saw:
        return "saw";
    case proto::Shape::pulse:
        return "square";
    case proto::Shape::triangle:
        return "triangle";
    }
    return "";
}

std::string db(double value)
{
    if (std::isinf(value) && value < 0.0)
    {
        return "missing";
    }
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << value;
    return text.str();
}

// The odd cycle count in n samples nearest MIDI note `note` at `rate`.
std::uint64_t noteCycles(int note, double rate)
{
    const double hz = 440.0 * std::exp2((note - 69) / 12.0);
    const auto cycles = static_cast<std::uint64_t>(std::lround(coherentFrequency(hz, rate, n) * n / rate));
    return std::max<std::uint64_t>(cycles, 5); // at least 4 bins: measureAliasing must resolve it
}

template <typename Osc>
std::vector<float> renderNote(proto::Shape shape, std::uint64_t cycles, double rate, std::size_t ratio)
{
    auto osc = Candidate<Osc>::make();
    osc.prepare(
        {.sampleRate = static_cast<float>(rate * static_cast<double>(ratio)), .maxBlockSize = 512, .numChannels = 1});
    osc.setShape(shape);
    osc.setPulseWidth(0.5f);
    osc.setIncrement(coherentIncrement(cycles, n, ratio));
    return render(osc, rate, ratio, 4096, n);
}

struct AliasSweep
{
    double worstAll = -std::numeric_limits<double>::infinity();
    double worstTop = -std::numeric_limits<double>::infinity();
    double worstBelow = -std::numeric_limits<double>::infinity(); ///< MIDI 0-103
    int worstNote = 0;
};

template <typename Osc>
AliasSweep sweepAliasing(proto::Shape shape, double rate, std::size_t ratio)
{
    AliasSweep sweep;
    const auto window = rectangularWindow(n);
    for (int note = 0; note <= 127; ++note)
    {
        const auto cycles = noteCycles(note, rate);
        const auto f0 = static_cast<double>(cycles) * rate / n;
        const auto alias = measureAliasing(renderNote<Osc>(shape, cycles, rate, ratio), f0, rate, window, audible);
        if (alias.worstAliasDb > sweep.worstAll)
        {
            sweep.worstAll = alias.worstAliasDb;
            sweep.worstNote = note;
        }
        auto& band = note >= topTwoOctaves ? sweep.worstTop : sweep.worstBelow;
        band = std::max(band, alias.worstAliasDb);
    }
    return sweep;
}

struct Droop
{
    double worstTo10k = 0.0; ///< most negative harmonic error (dB) up to 10 kHz, over all notes
    double worstTo20k = 0.0; ///< ... up to 20 kHz (-inf: some harmonic missing)
    double at15k = 0.0;      ///< error of the harmonic nearest 15 kHz, for a 110 Hz saw
    double at19k = 0.0;      ///< ... nearest 19 kHz
};

template <typename Osc>
Droop measureDroop(double rate, std::size_t ratio)
{
    Droop droop;
    const auto window = rectangularWindow(n);
    const auto errorDb = [](double measured, std::size_t k)
    {
        const auto ratioToIdeal = measured / idealHarmonic(proto::Shape::saw, k);
        return ratioToIdeal < 1e-4 ? -std::numeric_limits<double>::infinity() : toDb(ratioToIdeal);
    };
    for (int note = 0; note <= 127; ++note)
    {
        const auto cycles = noteCycles(note, rate);
        const auto f0 = static_cast<double>(cycles) * rate / n;
        const auto harmonics =
            measureHarmonics(renderNote<Osc>(proto::Shape::saw, cycles, rate, ratio), f0, rate, window, audible);
        for (std::size_t k = 1; k <= harmonics.size(); ++k)
        {
            const auto e = errorDb(harmonics[k - 1], k);
            if (static_cast<double>(k) * f0 <= 10000.0)
            {
                droop.worstTo10k = std::min(droop.worstTo10k, e);
            }
            droop.worstTo20k = std::min(droop.worstTo20k, e);
        }
        if (note == 45) // A2, 110 Hz
        {
            const auto nearest = [&](double hz)
            {
                return std::min(harmonics.size(), static_cast<std::size_t>(std::lround(hz / f0)));
            };
            droop.at15k = errorDb(harmonics[nearest(15000.0) - 1], nearest(15000.0));
            droop.at19k = errorDb(harmonics[nearest(19000.0) - 1], nearest(19000.0));
        }
    }
    return droop;
}

// Worst alias (dB re a saw's fundamental) under a deep vibrato: linear FM,
// f = f0 (1 + 0.25 sin(2 pi g t)), g = 6.6 Hz, every true component on a
// multiple of g. See the vibrato test in antialiasing_prototypes_test.cpp.
template <typename Osc>
double vibratoAliasing(std::uint64_t carrierMultiple, double rate, std::size_t ratio)
{
    constexpr std::uint64_t vibratoCycles = 9;
    const auto inner = n * ratio;
    auto osc = Candidate<Osc>::make();
    osc.prepare(
        {.sampleRate = static_cast<float>(rate * static_cast<double>(ratio)), .maxBlockSize = 512, .numChannels = 1});
    const PerSample<Osc> vibrato = [&](Osc& o, std::size_t i)
    {
        const auto phase =
            2.0 * std::numbers::pi * static_cast<double>(vibratoCycles * (i % inner)) / static_cast<double>(inner);
        const auto cyclesPerSample = static_cast<double>(vibratoCycles * carrierMultiple) / static_cast<double>(inner) *
                                     (1.0 + (0.25 * std::sin(phase)));
        o.setIncrement(static_cast<std::uint32_t>(std::lround(std::ldexp(cyclesPerSample, 32))));
    };
    const auto out = render(osc, rate, ratio, n, n, vibrato);
    const auto g = static_cast<double>(vibratoCycles) * rate / n;
    const auto window = rectangularWindow(n);
    const auto alias = measureAliasing(out, g, rate, window, audible);
    return worstAliasReSawFundamental(alias.worstAliasDb, powerSpectrum(out, rate, window).powerAround(g, 1));
}

// The largest |mean| over saw and triangle renders from 1 kHz to Nyquist.
template <typename Osc>
double worstDc(double rate)
{
    double worst = 0.0;
    for (const auto shape : {proto::Shape::saw, proto::Shape::triangle})
    {
        for (int kHz = 1; kHz < static_cast<int>(rate / 2000.0); ++kHz)
        {
            const auto hz = 1000.0 * kHz;
            const auto cycles = static_cast<std::uint64_t>(std::lround(coherentFrequency(hz, rate, n) * n / rate));
            const auto out = renderNote<Osc>(shape, cycles, rate, 1);
            worst = std::max(worst, std::abs(std::accumulate(out.begin(), out.end(), 0.0) / n));
        }
    }
    return worst;
}

// "44.1 kHz, 2x" for a row label.
std::string rowLabel(double rate, std::size_t ratio)
{
    std::ostringstream label;
    label << std::fixed << std::setprecision(1) << rate / 1000.0 << " kHz, " << ratio << "x";
    return label.str();
}

template <typename... Oscs>
void printReport()
{
    auto& out = std::cout;
    const std::array<const char*, sizeof...(Oscs)> names{Candidate<Oscs>::name...};
    std::string header = "| |";
    std::string rule = "|---|";
    for (const auto* name : names)
    {
        header += std::string(" ") + name + " |";
        rule += "---:|";
    }

    out << "## Worst alias below 20 kHz, dB re fundamental\n\nTop two octaves (MIDI 104-127, 3.3-12.5 kHz) "
           "(the note of the worst case over the whole keyboard) / MIDI 0-103.\n\n";
    for (const auto shape : {proto::Shape::saw, proto::Shape::pulse, proto::Shape::triangle})
    {
        out << "### " << shapeName(shape) << "\n\n" << header << "\n" << rule << "\n";
        for (const double rate : {44100.0, 48000.0})
        {
            for (const std::size_t ratio : {std::size_t{1}, std::size_t{2}, std::size_t{4}})
            {
                out << "| " << rowLabel(rate, ratio) << " |";
                (
                    [&]
                    {
                        const auto s = sweepAliasing<Oscs>(shape, rate, ratio);
                        out << " " << db(s.worstTop) << " (MIDI " << s.worstNote << ") / " << db(s.worstBelow) << " |";
                    }(),
                    ...);
                out << '\n' << std::flush; // one row at a time: the report takes a while
            }
        }
        out << "\n";
    }

    out << "## Saw passband droop, dB re ideal harmonic amplitude\n\n"
           "Worst harmonic up to 10 kHz / up to 20 kHz over MIDI 0-127; then a 110 Hz saw's harmonics nearest "
           "15 kHz / 19 kHz.\n\n"
        << header << "\n"
        << rule << "\n";
    for (const double rate : {44100.0, 48000.0})
    {
        for (const std::size_t ratio : {std::size_t{1}, std::size_t{2}})
        {
            out << "| " << rowLabel(rate, ratio) << " |";
            (
                [&]
                {
                    const auto d = measureDroop<Oscs>(rate, ratio);
                    out << " " << db(d.worstTo10k) << " / " << db(d.worstTo20k) << "; " << db(d.at15k) << " / "
                        << db(d.at19k) << " |";
                }(),
                ...);
            out << '\n' << std::flush; // one row at a time: the report takes a while
        }
    }

    out << "\n## Saw under a deep vibrato (+-25%, 6.6 Hz), worst alias below 20 kHz, dB re fundamental\n\n"
           "Carrier 1978 Hz (peaks at 2472 Hz) / 4945 Hz (peaks at 6181 Hz).\n\n"
        << header << "\n"
        << rule << "\n";
    for (const double rate : {44100.0, 48000.0})
    {
        for (const std::size_t ratio : {std::size_t{1}, std::size_t{2}})
        {
            out << "| " << rowLabel(rate, ratio) << " |";
            (
                [&]
                {
                    out << " " << db(vibratoAliasing<Oscs>(300, rate, ratio)) << " / "
                        << db(vibratoAliasing<Oscs>(750, rate, ratio)) << " |";
                }(),
                ...);
            out << '\n' << std::flush; // one row at a time: the report takes a while
        }
    }

    out << "\n## DC and memory\n\n" << header << "\n" << rule << "\n";
    out << "| largest saw/triangle mean, 1 kHz to Nyquist, 48 kHz |" << std::scientific << std::setprecision(1);
    (
        [&]
        {
            out << " " << worstDc<Oscs>(48000.0) << " |";
        }(),
        ...);
    out << std::defaultfloat << "\n| bytes per oscillator |";
    (
        [&]
        {
            out << " " << sizeof(Oscs) << " |";
        }(),
        ...);
    out << "\n\nShared tables: minBLEP 32-tap " << minBlepTable32().bytes() << " bytes, 64-tap "
        << minBlepTable64().bytes() << " bytes; wavetable bank " << wavetableBank().bytes() << " bytes.\n";
    out << std::fixed << std::setprecision(3) << "minBLEP group delay: 32-tap " << minBlepTable32().delay()
        << " samples, 64-tap " << minBlepTable64().delay() << " samples.\n";

    // A glide that crosses a mipmap boundary switches tables mid-cycle: the
    // output jumps by the difference of the two levels at that phase.
    double worstSwitch = 0.0;
    std::size_t worstLevel = 0;
    for (std::size_t level = 0; level + 1 < proto::WavetableBank::levels; ++level)
    {
        const auto* richer = wavetableBank().table(proto::Shape::saw, level);
        const auto* poorer = wavetableBank().table(proto::Shape::saw, level + 1);
        for (std::size_t i = 0; i < proto::WavetableBank::tableSize; ++i)
        {
            const auto jump = std::abs(static_cast<double>(richer[i]) - static_cast<double>(poorer[i]));
            if (jump > worstSwitch)
            {
                worstSwitch = jump;
                worstLevel = level;
            }
        }
    }
    out << "Wavetable saw: largest jump when the level changes: " << worstSwitch << " (levels " << worstLevel << " -> "
        << worstLevel + 1 << ", " << proto::WavetableBank::harmonicsAt(worstLevel) << " -> "
        << proto::WavetableBank::harmonicsAt(worstLevel + 1) << " harmonics).\n";
}

} // namespace

TEST_CASE("D6 report: measure every anti-aliasing prototype", "[.][d6-report]")
{
    printReport<PolyBlep2, PolyBlep4, MinBlep32, MinBlep64, Wavetable>();
    SUCCEED();
}
