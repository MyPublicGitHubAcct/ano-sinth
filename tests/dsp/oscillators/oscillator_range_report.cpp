// The range report, above Nyquist: what a voice whose pitch is above Nyquist
// plays now that it is clamped to a Nyquist increment, the level of every
// waveform as its fundamental approaches Nyquist, and prototypes of the
// alternatives to the clamp (muting, at once or ramped; fading near
// Nyquist; fading to the waveform's mean), each under a sweep through
// Nyquist, a fast vibrato across it, and, for the clamp, a driven saturator
// at 4x oversampling downstream. Hidden from the default run; run it in an
// optimised build (a few seconds):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[range-report]"
//
// Results, and the option chosen: docs/dsp/oscillator-range.md

#include "oscillator_support.hpp"

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/resampling/oversampler.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/measure.hpp>
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
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;
using anoesis::dsp::halfCycle;

namespace
{

constexpr double rate = 48000.0;

MinBlep64 preparedOscillator(Waveform shape, double sampleRate = rate)
{
    auto osc = Candidate<MinBlep64>::make();
    osc.prepare({.sampleRate = static_cast<float>(sampleRate), .maxBlockSize = 512, .numChannels = 1});
    osc.setWaveform(shape);
    return osc;
}

double mean(std::span<const float> x)
{
    double sum = 0.0;
    for (const float v : x)
    {
        sum += v;
    }
    return sum / static_cast<double>(x.size());
}

double rmsAbout(std::span<const float> x, double centre)
{
    double sum = 0.0;
    for (const float v : x)
    {
        sum += (v - centre) * (v - centre);
    }
    return std::sqrt(sum / static_cast<double>(x.size()));
}

double peakOf(std::span<const float> x)
{
    double peak = 0.0;
    for (const float v : x)
    {
        peak = std::max(peak, static_cast<double>(std::abs(v)));
    }
    return peak;
}

// The amplitude of the Nyquist component, y = dc + a (-1)^n.
double nyquistAmplitude(std::span<const float> x)
{
    double sum = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        sum += (i % 2 == 0 ? 1.0 : -1.0) * x[i];
    }
    return std::abs(sum / static_cast<double>(x.size()));
}

std::string db(double ratio)
{
    std::ostringstream s;
    if (ratio <= 0.0)
    {
        s << "-inf";
    }
    else
    {
        s << std::fixed << std::setprecision(1) << 20.0 * std::log10(ratio);
    }
    return s.str();
}

std::string fixed(double x, int digits = 3)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

// --- The clamp, measured ------------------------------------------------------

void reportClamp(std::ostream& out)
{
    out << "\n## A voice above Nyquist, as clamped now\n\n"
           "Each waveform at a Nyquist increment (phase step 2^31), started at 256 phases (k/256 of a cycle) and, "
           "separately, arrived at by a per-sample sweep from 0.45 fs to 0.55 fs over 0.1 s from 256 random phases. "
           "Measured after 128 samples, over 4096. The output is exactly dc + a (-1)^n. "
           "Amplitudes in dBFS (1.0 = 0 dB). The rate does not matter: in cycles, every rate is the same.\n\n"
           "| Waveform | Ideal (band-limited) | DC | Nyquist amplitude, min / median / max (start phase) | Max (sweep) "
           "| "
           "Peak |\n|---|---|---:|---:|---:|---:|\n";
    for (const auto shape : periodicShapes)
    {
        std::vector<double> amplitudes;
        double dcMin = 1e9;
        double dcMax = -1e9;
        double peak = 0.0;
        for (std::uint32_t k = 0; k < 256; ++k)
        {
            auto osc = preparedOscillator(shape);
            osc.restart(k << 24U);
            osc.setIncrement(halfCycle);
            std::vector<float> y(128 + 4096);
            for (auto& v : y)
            {
                v = osc.next();
            }
            const std::span tail{y.data() + 128, 4096};
            amplitudes.push_back(nyquistAmplitude(tail));
            dcMin = std::min(dcMin, mean(tail));
            dcMax = std::max(dcMax, mean(tail));
            peak = std::max(peak, peakOf(y));
        }
        double sweepMax = 0.0;
        anoesis::dsp::Random random{7};
        for (int k = 0; k < 256; ++k)
        {
            auto osc = preparedOscillator(shape);
            osc.restart(random.nextUint32());
            std::vector<float> y(4800 + 4096);
            for (std::size_t i = 0; i < y.size(); ++i)
            {
                const double f = i < 4800 ? 0.45 + (0.1 * static_cast<double>(i) / 4800.0) : 0.55;
                osc.setFrequency(static_cast<float>(f * rate));
                y[i] = osc.next();
            }
            sweepMax = std::max(sweepMax, nyquistAmplitude(std::span{y.data() + 4800, 4096}));
            peak = std::max(peak, peakOf(y));
        }
        std::ranges::sort(amplitudes);
        out << "| " << shapeName(shape) << " | " << (std::abs(idealMean(shape)) > 0 ? "DC only" : "silence") << " | "
            << fixed(dcMin) << (dcMax - dcMin > 1e-3 ? " to " + fixed(dcMax) : std::string{}) << " | "
            << db(amplitudes.front()) << " / " << db(amplitudes[128]) << " / " << db(amplitudes.back()) << " | "
            << db(sweepMax) << " | " << fixed(peak, 2) << " |\n";
    }
}

// --- Level against pitch near Nyquist ----------------------------------------

void reportLevelNearNyquist(std::ostream& out)
{
    const std::array fractions{0.30, 0.40, 0.45, 0.47, 0.48, 0.49, 0.495, 0.499};
    out << "\n## Level as the fundamental approaches Nyquist\n\n"
           "RMS about the mean, relative to the ideal fundamental's RMS (the band-limited waveform above fs/4 is "
           "its fundamental only), 48000 samples. The kernel's roll-off near Nyquist shows here; the sine is not "
           "filtered by it.\n\n| Waveform |";
    for (const double f : fractions)
    {
        out << " " << f << " fs |";
    }
    out << "\n|---|";
    for (std::size_t i = 0; i < fractions.size(); ++i)
    {
        out << "---:|";
    }
    out << "\n";
    for (const auto shape : periodicShapes)
    {
        out << "| " << shapeName(shape) << " |";
        const double ideal = idealHarmonic(shape, 1) / std::numbers::sqrt2; // the fundamental's RMS
        for (const double f : fractions)
        {
            auto osc = preparedOscillator(shape);
            osc.setFrequency(static_cast<float>(f * rate));
            std::vector<float> y(256 + 48000);
            for (auto& v : y)
            {
                v = osc.next();
            }
            const std::span tail{y.data() + 256, 48000};
            out << " " << db(rmsAbout(tail, mean(tail)) / ideal) << " |";
        }
        out << "\n";
    }
}

// --- The options, prototyped --------------------------------------------------

enum class Option : std::uint8_t
{
    clamp,
    hardMute,
    rampedMute,
    fade,
    fadeToMean,
};

const char* optionName(Option option)
{
    switch (option)
    {
    case Option::clamp:
        return "Clamp (now)";
    case Option::hardMute:
        return "Mute above Nyquist, at once";
    case Option::rampedMute:
        return "Mute above Nyquist, 10 ms ramp";
    case Option::fade:
        return "Fade 0.45 fs to 0.5 fs";
    case Option::fadeToMean:
        return "Fade to the waveform's mean";
    }
    return "";
}

constexpr std::array allOptions{Option::clamp, Option::hardMute, Option::rampedMute, Option::fade, Option::fadeToMean};

// A raised cosine from 1 at `lo` to 0 at 0.5 cycles per sample.
double fadeGain(double cycles, double lo = 0.45)
{
    if (cycles <= lo)
    {
        return 1.0;
    }
    if (cycles >= 0.5)
    {
        return 0.0;
    }
    return 0.5 * (1.0 + std::cos(std::numbers::pi * (cycles - lo) / (0.5 - lo)));
}

// One voice with the option applied to its output; `cycles` is the target
// pitch in cycles per sample, per sample.
std::vector<float> renderOption(Option option, Waveform shape, std::span<const double> cycles, std::uint32_t startPhase,
                                double sampleRate = rate)
{
    auto osc = preparedOscillator(shape, sampleRate);
    osc.restart(startPhase);
    anoesis::dsp::LinearSmoother ramp;
    ramp.setRampTime(0.01f);
    ramp.prepare({.sampleRate = static_cast<float>(sampleRate), .maxBlockSize = 512, .numChannels = 1});
    ramp.setTarget(cycles.front() < 0.5 ? 1.0f : 0.0f);
    ramp.reset();
    const auto meanValue = static_cast<float>(idealMean(shape));
    std::vector<float> y(cycles.size());
    for (std::size_t i = 0; i < cycles.size(); ++i)
    {
        osc.setFrequency(static_cast<float>(cycles[i] * sampleRate));
        const float x = osc.next();
        switch (option)
        {
        case Option::clamp:
            y[i] = x;
            break;
        case Option::hardMute:
            y[i] = cycles[i] < 0.5 ? x : 0.0f;
            break;
        case Option::rampedMute:
            ramp.setTarget(cycles[i] < 0.5 ? 1.0f : 0.0f);
            y[i] = ramp.next() * x;
            break;
        case Option::fade:
            y[i] = static_cast<float>(fadeGain(cycles[i])) * x;
            break;
        case Option::fadeToMean:
            y[i] = meanValue + (static_cast<float>(fadeGain(cycles[i])) * (x - meanValue));
            break;
        }
    }
    return y;
}

// Power from 10 Hz to `edgeHz`, about the mean, in dB relative to a
// full-scale sine (0.5). 32768-point segments: 1.5 Hz bins.
double powerBelowDb(std::span<const float> y, double edgeHz, double sampleRate = rate)
{
    const double centre = mean(y);
    std::vector<float> ac(y.size());
    std::ranges::transform(y, ac.begin(), [&](float v) { return static_cast<float>(v - centre); });
    const auto spectrum = averagedPowerSpectrum(ac, sampleRate, blackmanHarrisWindow(32768));
    double sum = 0.0;
    for (std::size_t k = spectrum.nearestBin(10.0); k < spectrum.power.size() && spectrum.frequencyOf(k) < edgeHz; ++k)
    {
        sum += spectrum.power[k];
    }
    return powerToDb(sum / 0.5);
}

// Each option on one pitch trajectory (cycles per sample, per sample).
void reportTrajectory(std::ostream& out, std::span<const double> cycles)
{
    out << "| Option | Saw: above Nyquist | Saw: 10 Hz-18 kHz | HPulse: above Nyquist | HPulse: 10 Hz-18 kHz | "
           "Sine: above Nyquist | Sine: 10 Hz-18 kHz |\n|---|---:|---:|---:|---:|---:|---:|\n";
    for (const auto option : allOptions)
    {
        out << "| " << optionName(option) << " |";
        for (const auto shape : {Waveform::saw, Waveform::hpulse, Waveform::sine})
        {
            double above = 0.0;
            double splatter = -400.0;
            for (std::uint32_t k = 0; k < 16; ++k)
            {
                const auto y = renderOption(option, shape, cycles, k << 28U);
                // Every sample whose target has been above Nyquist for the
                // last 10 ms (the ramp), about the ideal: the mean.
                double sum = 0.0;
                std::size_t count = 0;
                std::size_t aboveFor = 0;
                for (std::size_t i = 0; i < y.size(); ++i)
                {
                    aboveFor = cycles[i] >= 0.5 ? aboveFor + 1 : 0;
                    if (aboveFor > 480)
                    {
                        const double d = y[i] - idealMean(shape);
                        sum += d * d;
                        ++count;
                    }
                }
                if (count > 0)
                {
                    above = std::max(above, std::sqrt(sum / static_cast<double>(count)));
                }
                splatter = std::max(splatter, powerBelowDb(y, 18000.0));
            }
            out << " " << db(above) << " | " << fixed(splatter, 1) << " dB |";
        }
        out << "\n";
    }
}

void reportSweep(std::ostream& out)
{
    constexpr std::size_t length = std::size_t{3} * 32768;
    std::vector<double> cycles(length);
    // 0.40 fs -> 0.60 fs -> 0.40 fs, linear in pitch, about 1 s each way.
    const double lo = std::log2(0.40);
    const double hi = std::log2(0.60);
    for (std::size_t i = 0; i < length / 2; ++i)
    {
        const double t = 2.0 * static_cast<double>(i) / static_cast<double>(length);
        cycles[i] = std::exp2(lo + ((hi - lo) * t));
        cycles[length - 1 - i] = cycles[i];
    }
    out << "\n## The options under a sweep through Nyquist\n\n"
           "One voice swept from 0.40 fs to 0.60 fs and back, linear in pitch, about 1 s each way, at 48 kHz "
           "(19.2 to 28.8 kHz), started at 16 phases; the worst of the 16. *Above Nyquist*: the RMS about the "
           "waveform's mean once the target has been above fs/2 for 10 ms (the band-limited ideal is the mean "
           "alone, so 0 here). *10 Hz-18 kHz*: the power in that band about the mean, relative to a full-scale "
           "sine: what the transitions put into the audible band, where the tone itself never is. Saw, HPulse "
           "(a DC of -0.5), and sine.\n\n";
    reportTrajectory(out, cycles);

    // A 13 Hz vibrato of +-1 semitone around 0.485 fs: 0.458 to 0.514 fs.
    for (std::size_t i = 0; i < length; ++i)
    {
        const double t = static_cast<double>(i) / rate;
        cycles[i] = 0.485 * std::exp2(std::sin(2.0 * std::numbers::pi * 13.0 * t) / 12.0);
    }
    out << "\n## The options under a fast vibrato across Nyquist\n\n"
           "As above, for a 13 Hz vibrato of +-1 semitone around 0.485 fs (0.458 to 0.514 fs: in and out of "
           "the fade band and across Nyquist 13 times a second), 2 s.\n\n";
    reportTrajectory(out, cycles);
}

// A 220 Hz saw at full scale plus one voice whose target is above Nyquist,
// through tanh(drive x) at 4x oversampling: the power that lands in the
// audible band and is not a harmonic of 220 Hz.
void reportSaturator(std::ostream& out)
{
    constexpr std::size_t n = 65536;
    constexpr double noteHz = 220.0;
    anoesis::dsp::Oversampler oversampler;
    oversampler.setFactor(anoesis::dsp::OversamplingFactor::four);
    oversampler.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = 512, .numChannels = 1});
    const auto saturate = [&](std::vector<float> x, float drive)
    {
        oversampler.reset();
        for (std::size_t start = 0; start < x.size(); start += 512)
        {
            Buffer block{1, 512};
            std::copy_n(x.begin() + static_cast<std::ptrdiff_t>(start), 512, block.channel(0).begin());
            const auto inner = oversampler.upsample(block.block());
            for (std::size_t i = 0; i < inner.numSamples(); ++i)
            {
                auto& s = inner.channel(0)[i];
                s = std::tanh(drive * s);
            }
            oversampler.downsample(block.block());
            std::copy_n(block.channel(0).begin(), 512, x.begin() + static_cast<std::ptrdiff_t>(start));
        }
        return x;
    };
    // In-band, non-harmonic power relative to the 220 Hz fundamental.
    const auto inharmonicDb = [&](std::span<const float> y)
    {
        const std::span tail{y.data() + 4096, n - 8192};
        const auto spectrum = averagedPowerSpectrum(tail, rate, blackmanHarrisWindow(8192));
        const double fundamental = spectrum.powerAround(noteHz, spectrum.mainLobeBins);
        double other = 0.0;
        for (std::size_t k = 1; k < spectrum.power.size() && spectrum.frequencyOf(k) < 20000.0; ++k)
        {
            const double f = spectrum.frequencyOf(k);
            const double harmonic = std::round(f / noteHz) * noteHz;
            if (std::abs(f - harmonic) > static_cast<double>(spectrum.mainLobeBins + 1) * spectrum.binWidth())
            {
                other += spectrum.power[k];
            }
        }
        return powerToDb(other / fundamental);
    };
    const auto note = [&]
    {
        auto osc = preparedOscillator(Waveform::saw);
        osc.setFrequency(static_cast<float>(noteHz));
        std::vector<float> y(n);
        for (auto& v : y)
        {
            v = osc.next();
        }
        return y;
    }();

    out << "\n## Downstream: a driven saturator at 4x oversampling\n\n"
           "A 220 Hz saw at full scale plus one voice of the same waveform whose target is 1.1 fs/2 (above "
           "Nyquist), summed and driven through tanh(drive x) at 4x oversampling (the library's Oversampler), "
           "48 kHz. The figure is the power below 20 kHz that is not a harmonic of 220 Hz (intermodulation), "
           "relative to the 220 Hz fundamental; the clamped voice's own level is its median and its loudest "
           "start phase from the table above. *Muted* is the note alone, which the mute and fade options give.\n\n"
           "| Upper voice | Drive 1 | Drive 4 | Drive 16 |\n|---|---:|---:|---:|\n";
    const std::vector<double> cycles(n, 0.55);
    struct Row
    {
        const char* name;
        Waveform shape;
        std::uint32_t phase;
    };
    for (const auto& row : {
             Row{.name = "Muted (note alone)", .shape = Waveform::saw, .phase = 0},
             Row{.name = "Saw, clamped, phase 1/4", .shape = Waveform::saw, .phase = 0x40000000U},
             Row{.name = "Saw, clamped, phase 0", .shape = Waveform::saw, .phase = 0},
             Row{.name = "HPulse, clamped, phase 0", .shape = Waveform::hpulse, .phase = 0},
             Row{.name = "Sine, clamped, phase 1/4", .shape = Waveform::sine, .phase = 0x40000000U},
         })
    {
        out << "| " << row.name << " |";
        std::vector<float> mix = note;
        if (row.name[0] != 'M')
        {
            const auto upper = renderOption(Option::clamp, row.shape, cycles, row.phase);
            for (std::size_t i = 0; i < n; ++i)
            {
                mix[i] += upper[i];
            }
        }
        for (const float drive : {1.0f, 4.0f, 16.0f})
        {
            out << " " << fixed(inharmonicDb(saturate(mix, drive)), 1) << " dB |";
        }
        out << "\n";
    }
}

} // namespace

TEST_CASE("Range report: above Nyquist and the alternatives to the clamp", "[.][range-report]")
{
    std::ostringstream out;
    reportClamp(out);
    reportLevelNearNyquist(out);
    reportSweep(out);
    reportSaturator(out);
    std::cout << out.str() << std::flush;
    SUCCEED();
}
