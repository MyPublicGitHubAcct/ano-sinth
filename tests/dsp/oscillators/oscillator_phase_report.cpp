// The oscillator phase report: how much a retrigger aliases, as a band-limited
// jump against a naive phase jump and against restarting from silence, for
// every waveform, as hard sync (a retrigger every master period, which lets
// the aliasing be measured); and the peak output under dense random jumps.
// Printed as Markdown. Hidden from the default run; run it in an optimised
// build (a few seconds):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[phase-report]"
//
// Results: docs/dsp/oscillator-phase.md

#include "oscillator_support.hpp"

#include <anoesis/dsp/math/random.hpp>

#include <anoesis/test/measure.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;

namespace
{

constexpr std::size_t length = 65536;
constexpr std::size_t settle = 8192;

MinBlep64 preparedAt(Waveform shape, double sampleRate)
{
    auto osc = Candidate<MinBlep64>::make();
    osc.prepare({.sampleRate = static_cast<float>(sampleRate), .maxBlockSize = 512, .numChannels = 1});
    osc.setWaveform(shape);
    return osc;
}

// renderSynced()'s pattern, restarting from silence at the sample after each
// sync instead of jumping: what a retrigger did before jumpTo() existed.
std::vector<float> renderSyncedFromSilence(MinBlep64& osc, std::uint32_t target, std::uint64_t masterCycles)
{
    std::vector<float> out;
    out.reserve(length);
    std::uint64_t sync = 0;
    for (std::uint64_t i = 0; i < settle + length; ++i)
    {
        while (sync * length <= i * masterCycles)
        {
            const auto since =
                static_cast<double>((i * masterCycles) - (sync * length)) / static_cast<double>(masterCycles);
            osc.restart(target + static_cast<std::uint32_t>(since * static_cast<double>(osc.phaseIncrement())));
            ++sync;
        }
        const float y = osc.next();
        if (i >= settle)
        {
            out.push_back(y);
        }
    }
    return out;
}

// The worst alias below 20 kHz, in dB relative to an ideal saw's fundamental.
double worstAlias(const std::vector<float>& out, double f0, double sampleRate)
{
    const auto window = rectangularWindow(length);
    const auto alias = measureAliasing(out, f0, sampleRate, window, 20000.0);
    return worstAliasReSawFundamental(alias.worstAliasDb, powerSpectrum(out, sampleRate, window).powerAround(f0, 0));
}

void reportSync()
{
    std::cout << "\n### A retrigger every master period (hard sync): worst alias below 20 kHz\n\n"
              << "dB relative to an ideal saw's fundamental; 64 taps; the oscillator at 3.7 times the master rate, "
                 "retriggered at Phase 0, so each jump is from phase 0.7: on the triangle's falling slope, "
                 "where it bends as well as jumps.\n\n"
              << "| Rate | Master | Waveform | Band-limited jump | Naive jump | Restart from silence |\n"
              << "|---:|---:|---|---:|---:|---:|\n";
    std::cout << std::fixed << std::setprecision(1);
    for (const double sampleRate : {44100.0, 48000.0})
    {
        for (const double masterHz : {8.0, 110.0, 440.0, 1760.0})
        {
            // An odd number of master cycles in the analysis length.
            auto cycles = static_cast<std::uint64_t>(std::lround(masterHz * length / sampleRate));
            cycles |= 1U;
            const double f0 = static_cast<double>(cycles) * sampleRate / static_cast<double>(length);
            const auto increment = static_cast<std::uint32_t>(std::ldexp(f0 * 3.7 / sampleRate, 32));
            for (const auto shape : periodicShapes)
            {
                auto osc = preparedAt(shape, sampleRate);
                osc.setIncrement(increment);
                const auto banded = renderSynced(osc, 0, length, cycles, settle);
                const auto naive = renderSyncedNaive(shape, increment, 0, length, cycles, settle);
                auto fromSilence = preparedAt(shape, sampleRate);
                fromSilence.setIncrement(increment);
                const auto silent = renderSyncedFromSilence(fromSilence, 0, cycles);
                std::cout << "| " << sampleRate / 1000.0 << " kHz | " << f0 << " Hz | " << shapeName(shape) << " | "
                          << worstAlias(banded, f0, sampleRate) << " | " << worstAlias(naive, f0, sampleRate) << " | "
                          << worstAlias(silent, f0, sampleRate) << " |\n";
            }
        }
    }
}

void reportPeaks()
{
    std::cout << "\n### Peak output under dense random jumps\n\n"
              << "48 kHz, 64 taps: 300 jumps to random phases at random sub-sample times, 1 to 16 samples apart.\n\n"
              << "| Waveform | 1 kHz | 5 kHz | 12 kHz | 21 kHz | 24 kHz |\n|---|---:|---:|---:|---:|---:|\n";
    std::cout << std::setprecision(2);
    for (const auto shape : periodicShapes)
    {
        std::cout << "| " << shapeName(shape) << " |";
        for (const double hz : {1000.0, 5000.0, 12000.0, 21000.0, 24000.0})
        {
            anoesis::dsp::Random random{42};
            auto osc = preparedAt(shape, 48000.0);
            osc.setFrequency(static_cast<float>(hz));
            float peak = 0.0f;
            for (int jumps = 0; jumps < 300; ++jumps)
            {
                osc.jumpTo(random.nextUint32(), random.nextUnipolar());
                const auto samples = 1 + (random.nextUint32() % 16U);
                for (std::uint32_t i = 0; i < samples; ++i)
                {
                    peak = std::max(peak, std::abs(osc.next()));
                }
            }
            std::cout << " " << peak << " |";
        }
        std::cout << "\n";
    }
}

} // namespace

TEST_CASE("Oscillator phase report: retrigger aliasing and peaks", "[.][phase-report]")
{
    reportSync();
    reportPeaks();
    SUCCEED();
}
