// The oscillator measurement report: the minBLEP oscillator (32 and 64 taps)
// and the PolyBLEP reference, for each of the 7 periodic waveforms, measured
// with the test harness (measureAliasing, measureHarmonics, powerSpectrum)
// and printed as Markdown tables. Hidden from the default run; run it in an
// optimised build (a minute or two):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[d6-report]"
//
// Results: docs/dsp/band-limited-oscillators.md (the D6 decision) and
// docs/dsp/oscillator-waveforms.md (the 8 waveforms). The mipmapped-wavetable
// candidate was measured too, then removed; to rerun it, check out commit
// 984d868.

#include "oscillator_support.hpp"

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
constexpr double minusInfinity = -std::numeric_limits<double>::infinity();

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
std::vector<float> renderNote(Waveform shape, std::uint64_t cycles, double rate, std::size_t ratio)
{
    auto osc = Candidate<Osc>::make();
    osc.prepare(
        {.sampleRate = static_cast<float>(rate * static_cast<double>(ratio)), .maxBlockSize = 512, .numChannels = 1});
    osc.setWaveform(shape);
    osc.setIncrement(coherentIncrement(cycles, n, ratio));
    return render(osc, rate, ratio, 4096, n);
}

double peakOf(const std::vector<float>& signal)
{
    double peak = 0.0;
    for (const float x : signal)
    {
        peak = std::isfinite(x) ? std::max(peak, static_cast<double>(std::abs(x)))
                                : std::numeric_limits<double>::infinity();
    }
    return peak;
}

// One sweep over MIDI 0-127: the worst alias, the passband error against the
// waveform's Fourier series, and the peak level, from the same renders.
struct Sweep
{
    double worstAll = minusInfinity;
    double worstTop = minusInfinity;   ///< MIDI 104-127
    double worstBelow = minusInfinity; ///< MIDI 0-103
    int worstNote = 0;
    double droopTo10k = 0.0; ///< most negative harmonic error (dB) up to 10 kHz
    double droopTo20k = 0.0; ///< ... up to 20 kHz (-inf: some harmonic missing)
    double at15k = 0.0;      ///< error of the harmonic nearest 15 kHz, at A2 (110 Hz)
    double at19k = 0.0;      ///< ... nearest 19 kHz
    double peak = 0.0;
};

template <typename Osc>
Sweep sweep(Waveform shape, double rate, std::size_t ratio, bool withDroop)
{
    Sweep result;
    const auto window = rectangularWindow(n);
    // A harmonic whose ideal amplitude is this small sits on a null of the
    // series (a pulse's k w = integer, a square's even harmonics); its
    // relative error says nothing about droop.
    constexpr double negligible = 1e-3;
    const auto errorDb = [&](double measured, std::size_t k)
    {
        const auto ratioToIdeal = measured / idealHarmonic(shape, k);
        return ratioToIdeal < 1e-4 ? minusInfinity : toDb(ratioToIdeal);
    };
    for (int note = 0; note <= 127; ++note)
    {
        const auto cycles = noteCycles(note, rate);
        const auto f0 = static_cast<double>(cycles) * rate / n;
        const auto out = renderNote<Osc>(shape, cycles, rate, ratio);
        result.peak = std::max(result.peak, peakOf(out));

        const auto alias = measureAliasing(out, f0, rate, window, audible);
        if (alias.worstAliasDb > result.worstAll)
        {
            result.worstAll = alias.worstAliasDb;
            result.worstNote = note;
        }
        auto& band = note >= topTwoOctaves ? result.worstTop : result.worstBelow;
        band = std::max(band, alias.worstAliasDb);

        if (!withDroop)
        {
            continue;
        }
        const auto harmonics = measureHarmonics(out, f0, rate, window, audible);
        for (std::size_t k = 1; k <= harmonics.size(); ++k)
        {
            if (idealHarmonic(shape, k) < negligible)
            {
                continue;
            }
            const auto e = errorDb(harmonics[k - 1], k);
            if (static_cast<double>(k) * f0 <= 10000.0)
            {
                result.droopTo10k = std::min(result.droopTo10k, e);
            }
            result.droopTo20k = std::min(result.droopTo20k, e);
        }
        if (note == 45 && shape == Waveform::saw) // A2, 110 Hz
        {
            const auto nearest = [&](double hz)
            {
                return std::min(harmonics.size(), static_cast<std::size_t>(std::lround(hz / f0)));
            };
            result.at15k = errorDb(harmonics[nearest(15000.0) - 1], nearest(15000.0));
            result.at19k = errorDb(harmonics[nearest(19000.0) - 1], nearest(19000.0));
        }
    }
    return result;
}

// Worst alias (dB re a saw's fundamental) under a deep vibrato: linear FM,
// f = f0 (1 + 0.25 sin(2 pi g t)), g = 6.6 Hz, every true component on a
// multiple of g. See the vibrato test in band_limited_oscillator_test.cpp.
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
    for (const auto shape : {Waveform::saw, Waveform::triangle})
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

struct Level
{
    double worstDc = 0.0; ///< largest |mean - ideal mean|, 1 kHz steps to Nyquist
    double peak = 0.0;    ///< over those renders and 0.01 Hz
};

// DC against the ideal mean, and the peak, from 0.01 Hz to Nyquist at `rate`.
template <typename Osc>
Level measureLevel(Waveform shape, double rate)
{
    Level level;
    for (int kHz = 1; kHz < static_cast<int>(rate / 2000.0); ++kHz)
    {
        const auto hz = 1000.0 * kHz;
        const auto cycles = static_cast<std::uint64_t>(std::lround(coherentFrequency(hz, rate, n) * n / rate));
        const auto out = renderNote<Osc>(shape, cycles, rate, 1);
        const auto mean = std::accumulate(out.begin(), out.end(), 0.0) / n;
        level.worstDc = std::max(level.worstDc, std::abs(mean - idealMean(shape)));
        level.peak = std::max(level.peak, peakOf(out));
    }
    for (const double hz : {0.01, (rate / 2.0) - 1.0, rate / 2.0})
    {
        auto osc = Candidate<Osc>::make();
        osc.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = 512, .numChannels = 1});
        osc.setWaveform(shape);
        osc.setFrequency(static_cast<float>(hz));
        level.peak = std::max(level.peak, peakOf(render(osc, rate, 1, 0, 16384)));
    }
    return level;
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
    const auto row = [&](const std::string& label, const auto& cell)
    {
        out << "| " << label << " |";
        (
            [&]
            {
                out << " " << cell.template operator()<Oscs>() << " |";
            }(),
            ...);
        out << '\n' << std::flush; // one row at a time: the report takes a while
    };

    for (const double rate : {44100.0, 48000.0})
    {
        out << "## " << rowLabel(rate, 1) << ": worst alias below 20 kHz, dB re fundamental\n\n"
            << "Top two octaves (MIDI 104-127, 3.3-12.5 kHz) (the note of the worst case over the whole keyboard) / "
               "MIDI 0-103. Then the worst harmonic error against the waveform's Fourier series, dB, up to 10 kHz / "
               "up to 20 kHz, over MIDI 0-127 (harmonics on a null of the series skipped). Then the peak level.\n\n"
            << header << "\n"
            << rule << "\n";
        for (const auto shape : periodicShapes)
        {
            row(shapeName(shape),
                [&]<typename Osc>
                {
                    const auto s = sweep<Osc>(shape, rate, 1, true);
                    std::ostringstream cell;
                    cell << db(s.worstTop) << " (MIDI " << s.worstNote << ") / " << db(s.worstBelow) << "; droop "
                         << db(s.droopTo10k) << " / " << db(s.droopTo20k) << "; peak " << std::fixed
                         << std::setprecision(2) << s.peak;
                    return cell.str();
                });
        }
        out << "\n";
    }

    out << "## Saw, oversampled: worst alias below 20 kHz, dB re fundamental\n\n"
           "Top two octaves (the worst note) / MIDI 0-103.\n\n"
        << header << "\n"
        << rule << "\n";
    for (const double rate : {44100.0, 48000.0})
    {
        for (const std::size_t ratio : {std::size_t{2}, std::size_t{4}})
        {
            row(rowLabel(rate, ratio),
                [&]<typename Osc>
                {
                    const auto s = sweep<Osc>(Waveform::saw, rate, ratio, false);
                    return db(s.worstTop) + " (MIDI " + std::to_string(s.worstNote) + ") / " + db(s.worstBelow);
                });
        }
    }

    out << "\n## Saw passband droop, dB re ideal harmonic amplitude\n\n"
           "Worst harmonic up to 10 kHz / up to 20 kHz over MIDI 0-127; then a 110 Hz saw's harmonics nearest "
           "15 kHz / 19 kHz.\n\n"
        << header << "\n"
        << rule << "\n";
    for (const double rate : {44100.0, 48000.0})
    {
        for (const std::size_t ratio : {std::size_t{1}, std::size_t{2}})
        {
            row(rowLabel(rate, ratio),
                [&]<typename Osc>
                {
                    const auto d = sweep<Osc>(Waveform::saw, rate, ratio, true);
                    return db(d.droopTo10k) + " / " + db(d.droopTo20k) + "; " + db(d.at15k) + " / " + db(d.at19k);
                });
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
            row(rowLabel(rate, ratio),
                [&]<typename Osc>
                {
                    return db(vibratoAliasing<Osc>(300, rate, ratio)) + " / " +
                           db(vibratoAliasing<Osc>(750, rate, ratio));
                });
        }
    }

    out << "\n## DC and peak, 48 kHz, 0.01 Hz to Nyquist\n\n"
           "Largest |mean - ideal mean| over 1 kHz steps to Nyquist / largest |sample| over those, 0.01 Hz, and "
           "Nyquist.\n\n"
        << header << "\n"
        << rule << "\n";
    for (const auto shape : periodicShapes)
    {
        row(shapeName(shape),
            [&]<typename Osc>
            {
                const auto level = measureLevel<Osc>(shape, 48000.0);
                std::ostringstream cell;
                cell << std::scientific << std::setprecision(1) << level.worstDc << " / " << std::fixed
                     << std::setprecision(2) << level.peak;
                return cell.str();
            });
    }

    out << "\n## Memory\n\n" << header << "\n" << rule << "\n";
    row("bytes per oscillator", []<typename Osc> { return std::to_string(sizeof(Osc)); });
    out << "\nShared tables: minBLEP 32-tap " << minBlepTable32().bytes() << " bytes, 64-tap "
        << minBlepTable64().bytes() << " bytes.\n";
    out << std::fixed << std::setprecision(3) << "minBLEP group delay: 32-tap " << minBlepTable32().delay()
        << " samples, 64-tap " << minBlepTable64().delay() << " samples.\n";
}

} // namespace

TEST_CASE("Oscillator report: every waveform, minBLEP against the PolyBLEP reference", "[.][d6-report]")
{
    printReport<PolyBlep2, PolyBlep4, MinBlep32, MinBlep64>();
    SUCCEED();
}
