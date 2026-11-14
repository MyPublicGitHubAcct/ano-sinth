// The drift report: candidate models of analog oscillator drift, prototyped
// in drift_candidates.hpp and measured to choose one
// (docs/dsp/oscillator-drift.md). For each model, at the same standard
// deviation:
//
// - the spectrum of the pitch deviation (the frequencies below which half
//   and 99% of its variance lie, its level at decades around its rate, and
//   what lies above 20 Hz, where pitch movement is heard as roughness rather
//   than drift) and its distribution (peak and kurtosis);
// - how independent two voices' streams are, from adjacent seeds;
// - how it meets the oscillator's band-limited pitch-step threshold
//   (bandLimitedPitchStep), updated every sample or at a control rate, held
//   or interpolated, at the top of the range where an increment is largest.
//
// Hidden from the default run; run it in an optimised build (about 10 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[drift-report]"
//
// Ten hours of each model, checking that nothing wanders, is a separate
// hidden case (about a minute):
//
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[drift-long-report]"
//
// CPU: the "[drift]" benchmarks (tools/bench.sh "[drift]").

#include "drift_candidates.hpp"
#include "oscillator_support.hpp"

#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>

#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::drift;

namespace
{

constexpr double sampleRate = 48000.0;
constexpr std::size_t controlInterval = 64;
constexpr double controlHz = sampleRate / static_cast<double>(controlInterval); // 750 Hz
constexpr double referenceRate = 0.5;                                           // Hz
constexpr std::uint64_t seed = 0x5EED;

std::string fixed(double x, int digits)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

// Streaming moments and peak.
struct Moments
{
    double n = 0.0;
    double sum = 0.0;
    double sum2 = 0.0;
    double sum4 = 0.0;
    double peak = 0.0;

    void add(double x) noexcept
    {
        n += 1.0;
        sum += x;
        sum2 += x * x;
        sum4 += x * x * x * x;
        peak = std::max(peak, std::abs(x));
    }

    [[nodiscard]] double mean() const noexcept
    {
        return sum / n;
    }

    [[nodiscard]] double variance() const noexcept
    {
        return (sum2 / n) - (mean() * mean());
    }

    [[nodiscard]] double kurtosis() const noexcept
    {
        // About zero: every model is zero-mean by construction.
        return (sum4 / n) / ((sum2 / n) * (sum2 / n));
    }
};

template <typename Model>
std::vector<float> renderControl(double seconds, double sigma, std::uint64_t modelSeed)
{
    Model model{{.sigmaCents = sigma, .rateHz = referenceRate, .updateHz = controlHz}, modelSeed};
    std::vector<float> x(static_cast<std::size_t>(seconds * controlHz));
    for (auto& v : x)
    {
        v = model.next();
    }
    return x;
}

// One model's row of the statistics table: one hour at the control rate.
template <typename Model>
void reportStatistics(std::ostringstream& out)
{
    constexpr double sigma = 1.0;
    const auto x = renderControl<Model>(3600.0, sigma, seed);
    Moments m;
    for (const float v : x)
    {
        m.add(v);
    }

    const auto spectrum = averagedPowerSpectrum(x, controlHz, hannWindow(1U << 16U));
    double total = 0.0;
    for (std::size_t k = 1; k < spectrum.power.size(); ++k)
    {
        total += spectrum.power[k];
    }
    double cumulative = 0.0;
    double f50 = 0.0;
    double f99 = 0.0;
    double above20 = 0.0;
    for (std::size_t k = 1; k < spectrum.power.size(); ++k)
    {
        cumulative += spectrum.power[k];
        if (f50 <= 0.0 && cumulative >= 0.5 * total)
        {
            f50 = spectrum.frequencyOf(k);
        }
        if (f99 <= 0.0 && cumulative >= 0.99 * total)
        {
            f99 = spectrum.frequencyOf(k);
        }
        if (spectrum.frequencyOf(k) > 20.0)
        {
            above20 += spectrum.power[k];
        }
    }
    // Power spectral density around f (+-10%), relative to that at rate / 10.
    const auto density = [&](double f)
    {
        const auto lo = spectrum.nearestBin(f * 0.9);
        const auto hi = std::max(spectrum.nearestBin(f * 1.1), lo + 1);
        double sum = 0.0;
        for (std::size_t k = lo; k < hi; ++k)
        {
            sum += spectrum.power[k];
        }
        return sum / static_cast<double>(hi - lo);
    };
    const double reference = density(referenceRate / 10.0);
    const auto relative = [&](double f)
    {
        return fixed(10.0 * std::log10(density(f) / reference), 1);
    };
    out << "| " << Model::name << " | " << fixed(std::sqrt(m.variance()), 3) << " | " << fixed(m.peak, 2) << " | "
        << fixed(m.kurtosis(), 2) << " | " << fixed(f50, 3) << " | " << fixed(f99, 2) << " | 0 / "
        << relative(referenceRate) << " / " << relative(referenceRate * 10.0) << " / "
        << relative(referenceRate * 100.0) << " | "
        << (above20 > 0.0 ? fixed(10.0 * std::log10(above20 / total), 1) : std::string{"none"}) << " |\n";
}

void reportStatisticsTable(std::ostringstream& out)
{
    out << "\n## Spectrum and distribution\n\n"
        << "sigma 1 cent, rate " << referenceRate << " Hz, updated at " << controlHz
        << " Hz (48 kHz / 64), one hour. f50 / f99: the frequencies below which 50% / 99% of the variance lies. "
           "Density: the PSD at rate x 0.1 / 1 / 10 / 100, in dB relative to rate x 0.1.\n\n"
        << "| Model | sigma (cents) | peak (cents) | kurtosis | f50 (Hz) | f99 (Hz) | density (dB) | above 20 Hz "
           "(dB re total) |\n"
        << "|---|---:|---:|---:|---:|---:|---|---:|\n";
    reportStatistics<OnePole>(out);
    reportStatistics<TwoPole>(out);
    reportStatistics<SampleHold<false>>(out);
    reportStatistics<SampleHold<true>>(out);
    reportStatistics<SumOfSines>(out);
    reportStatistics<Pink<8>>(out);

    // A static offset per note has no spectrum within a note; across notes
    // it is uniform.
    Static model{{.sigmaCents = 1.0, .rateHz = referenceRate, .updateHz = controlHz}, seed};
    Moments m;
    for (int note = 0; note < 100000; ++note)
    {
        model.start();
        m.add(model.next());
    }
    out << "| " << Static::name << " (100,000 notes) | " << fixed(std::sqrt(m.variance()), 3) << " | "
        << fixed(m.peak, 2) << " | " << fixed(m.kurtosis(), 2) << " | 0 | 0 | | none |\n";
}

// Two streams from adjacent seeds: their correlation, over 64 pairs of ten
// minutes each.
template <typename Model>
void reportIndependence(std::ostringstream& out)
{
    double sumAbs = 0.0;
    double worst = 0.0;
    for (std::uint64_t pair = 0; pair < 64; ++pair)
    {
        const auto a = renderControl<Model>(600.0, 1.0, seed + (2 * pair));
        const auto b = renderControl<Model>(600.0, 1.0, seed + (2 * pair) + 1);
        double ab = 0.0;
        double aa = 0.0;
        double bb = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            ab += static_cast<double>(a[i]) * b[i];
            aa += static_cast<double>(a[i]) * a[i];
            bb += static_cast<double>(b[i]) * b[i];
        }
        const double r = ab / std::sqrt(aa * bb);
        sumAbs += std::abs(r);
        worst = std::max(worst, std::abs(r));
    }
    out << "| " << Model::name << " | " << fixed(sumAbs / 64.0, 4) << " | " << fixed(worst, 4) << " |\n";
}

void reportIndependenceTable(std::ostringstream& out)
{
    out << "\n## Independence of voices\n\n"
        << "Streams from seeds n and n + 1 (Random seeds through SplitMix64), 64 pairs of 10 minutes at the control "
           "rate: |correlation|.\n\n"
        << "| Model | mean | worst |\n|---|---:|---:|\n";
    reportIndependence<OnePole>(out);
    reportIndependence<TwoPole>(out);
    reportIndependence<SampleHold<false>>(out);
    reportIndependence<SampleHold<true>>(out);
    reportIndependence<SumOfSines>(out);
    reportIndependence<Pink<8>>(out);
}

// The increments a voice at `note` would get, sample by sample, with the
// drift added to the note in float (as a voice would pass it), and how they
// meet bandLimitedPitchStep.
struct StepResult
{
    double worstRatio = 0.0; // largest |change of increment| / bandLimitedPitchStep
    double crossingsPerSecond = 0.0;
};

template <typename Next>
StepResult measureSteps(float note, double seconds, Next next)
{
    const auto samples = static_cast<std::size_t>(seconds * sampleRate);
    const auto rate = static_cast<float>(sampleRate);
    std::uint32_t previous = anoesis::dsp::incrementFor(anoesis::dsp::noteToFrequency(note + (next() * 0.01f)), rate);
    double worst = 0.0;
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < samples; ++i)
    {
        const auto increment = anoesis::dsp::incrementFor(anoesis::dsp::noteToFrequency(note + (next() * 0.01f)), rate);
        const auto change = increment > previous ? increment - previous : previous - increment;
        worst = std::max(worst, static_cast<double>(change));
        if (change >= anoesis::dsp::bandLimitedPitchStep)
        {
            ++crossings;
        }
        previous = increment;
    }
    return {
        .worstRatio = worst / static_cast<double>(anoesis::dsp::bandLimitedPitchStep),
        .crossingsPerSecond = static_cast<double>(crossings) / seconds,
    };
}

template <typename Model>
void reportStepsFor(std::ostringstream& out, double sigma, double rateHz)
{
    // 0.44 fs: just below the Nyquist fade, where a relative pitch change
    // makes the largest change of increment.
    const float note = anoesis::dsp::frequencyToNote(static_cast<float>(0.44 * sampleRate));
    constexpr double seconds = 60.0;
    std::string cells;
    const auto cell = [](const StepResult& r)
    {
        return fixed(r.worstRatio, 4) + " (" + fixed(r.crossingsPerSecond, 1) + "/s)";
    };
    {
        Model model{{.sigmaCents = sigma, .rateHz = rateHz, .updateHz = sampleRate}, seed};
        cells += cell(measureSteps(note, seconds, [&] { return model.next(); })) + " | ";
    }
    for (const auto how : {Interpolation::hold, Interpolation::linear})
    {
        ControlRate<Model> model{
            Model{{.sigmaCents = sigma, .rateHz = rateHz, .updateHz = controlHz}, seed},
            controlInterval,
            how,
        };
        cells += cell(measureSteps(note, seconds, [&] { return model.next(); })) + " | ";
    }
    out << "| " << Model::name << " | " << cells << "\n";
}

void reportStepsTable(std::ostringstream& out)
{
    for (const auto& [sigma, rateHz, label] : {
             std::tuple{1.5, 0.5, "1.5 cents, 0.5 Hz"},
             std::tuple{5.0, 2.0, "5 cents, 2 Hz"},
             std::tuple{20.0, 5.0, "20 cents, 5 Hz (stress)"},
         })
    {
        out << "\n## Against the pitch-step threshold: sigma " << label << "\n\n"
            << "A voice at 0.44 fs (21.1 kHz at 48 kHz), its drift added to its note in float, 60 s. The largest "
               "change of increment between samples, in units of bandLimitedPitchStep (2^18), and (in brackets) "
               "how many changes per second reach it, each of which adds a band-limited bend (about 15 ns).\n\n"
            << "| Model | every sample | control rate (64), held | control rate (64), linear |\n"
            << "|---|---:|---:|---:|\n";
        reportStepsFor<OnePole>(out, sigma, rateHz);
        reportStepsFor<TwoPole>(out, sigma, rateHz);
        reportStepsFor<SampleHold<false>>(out, sigma, rateHz);
        reportStepsFor<SampleHold<true>>(out, sigma, rateHz);
        reportStepsFor<SumOfSines>(out, sigma, rateHz);
        reportStepsFor<Pink<8>>(out, sigma, rateHz);
    }
}

// A drift held for each 64 samples is a staircase in pitch, which modulates
// every partial at the control rate (750 Hz): sidebands at f +- 750 k Hz.
// Measured on a sine, the sidebands' power (k = 1..4, both sides) relative
// to the carrier's, held against interpolated linearly.
template <typename Model>
double staircaseSidebands(double sigma, double rateHz, double hz, Interpolation how)
{
    constexpr std::size_t length = 1U << 18U;
    anoesis::dsp::MinBlepOscillator<anoesis::dsp::minBlepTaps> osc{d6::minBlepTable64()};
    osc.prepare({.sampleRate = static_cast<float>(sampleRate), .maxBlockSize = length, .numChannels = 1});
    osc.setWaveform(anoesis::dsp::Waveform::sine);
    ControlRate<Model> drift{
        Model{{.sigmaCents = sigma, .rateHz = rateHz, .updateHz = controlHz}, seed},
        controlInterval,
        how,
    };
    const float note = anoesis::dsp::frequencyToNote(static_cast<float>(hz));
    std::vector<float> x(length + 4096);
    for (auto& v : x)
    {
        osc.setFrequency(anoesis::dsp::noteToFrequency(note + (drift.next() * 0.01f)));
        v = osc.next();
    }
    const auto spectrum = powerSpectrum(std::span{x}.subspan(4096, length), sampleRate, blackmanHarrisWindow(length));
    // Wide enough to hold the carrier as the drift moves it (5 sigma).
    const auto halfWidth =
        static_cast<std::size_t>(std::ceil(((hz * 5.0 * sigma * 5.8e-4) + 20.0) / spectrum.binWidth()));
    const double carrier = spectrum.powerAround(hz, halfWidth);
    double sidebands = 0.0;
    for (int k = 1; k <= 4; ++k)
    {
        for (const double side : {-1.0, 1.0})
        {
            const double f = hz + (side * controlHz * k);
            if (f > 0.0 && f < sampleRate / 2.0)
            {
                sidebands += spectrum.powerAround(f, halfWidth);
            }
        }
    }
    return 10.0 * std::log10(sidebands / carrier);
}

template <typename Model>
void reportStaircaseFor(std::ostringstream& out, double sigma, double rateHz)
{
    out << "| " << Model::name;
    for (const double hz : {1000.0, 5000.0, 0.44 * sampleRate})
    {
        out << " | " << fixed(staircaseSidebands<Model>(sigma, rateHz, hz, Interpolation::hold), 1) << " / "
            << fixed(staircaseSidebands<Model>(sigma, rateHz, hz, Interpolation::linear), 1);
    }
    out << " |\n";
}

void reportStaircaseTable(std::ostringstream& out)
{
    for (const auto& [sigma, rateHz, label] :
         {std::tuple{1.5, 0.5, "1.5 cents, 0.5 Hz"}, std::tuple{5.0, 2.0, "5 cents, 2 Hz"}})
    {
        out << "\n## Holding the drift for 64 samples: sidebands at the control rate, sigma " << label << "\n\n"
            << "A sine with the drift updated every 64 samples (750 Hz), 2^18 samples at 48 kHz. The power at f +- "
               "750 k Hz (k = 1..4) relative to the carrier, in dB: held / interpolated linearly.\n\n"
            << "| Model | 1 kHz | 5 kHz | 21.1 kHz (0.44 fs) |\n|---|---:|---:|---:|\n";
        reportStaircaseFor<OnePole>(out, sigma, rateHz);
        reportStaircaseFor<TwoPole>(out, sigma, rateHz);
        reportStaircaseFor<SampleHold<false>>(out, sigma, rateHz);
        reportStaircaseFor<SampleHold<true>>(out, sigma, rateHz);
        reportStaircaseFor<SumOfSines>(out, sigma, rateHz);
        reportStaircaseFor<Pink<8>>(out, sigma, rateHz);
    }
}

// Ten hours at the control rate, streamed: the peak, and the spread of
// one-minute means and standard deviations, first hour against last.
template <typename Model>
void reportLong(std::ostringstream& out)
{
    constexpr double sigma = 1.0;
    constexpr double hours = 10.0;
    Model model{{.sigmaCents = sigma, .rateHz = referenceRate, .updateHz = controlHz}, seed};
    const auto perMinute = static_cast<std::size_t>(60.0 * controlHz);
    const auto minutes = static_cast<std::size_t>(hours * 60.0);
    Moments all;
    Moments firstHour;
    Moments lastHour;
    double worstMinuteMean = 0.0;
    double minMinuteSigma = 1e9;
    double maxMinuteSigma = 0.0;
    std::size_t beyond3 = 0;
    for (std::size_t minute = 0; minute < minutes; ++minute)
    {
        Moments m;
        for (std::size_t i = 0; i < perMinute; ++i)
        {
            const double x = model.next();
            m.add(x);
            all.add(x);
            if (minute < 60)
            {
                firstHour.add(x);
            }
            if (minute >= minutes - 60)
            {
                lastHour.add(x);
            }
            if (std::abs(x) > 3.0 * sigma)
            {
                ++beyond3;
            }
        }
        worstMinuteMean = std::max(worstMinuteMean, std::abs(m.mean()));
        minMinuteSigma = std::min(minMinuteSigma, std::sqrt(m.variance()));
        maxMinuteSigma = std::max(maxMinuteSigma, std::sqrt(m.variance()));
    }
    out << "| " << Model::name << " | " << fixed(all.peak, 2) << " | " << fixed(std::sqrt(firstHour.variance()), 3)
        << " / " << fixed(std::sqrt(lastHour.variance()), 3) << " | " << fixed(firstHour.mean(), 3) << " / "
        << fixed(lastHour.mean(), 3) << " | " << fixed(worstMinuteMean, 3) << " | " << fixed(minMinuteSigma, 2) << "-"
        << fixed(maxMinuteSigma, 2) << " | " << fixed(1e6 * static_cast<double>(beyond3) / all.n, 1) << " |\n";
}

} // namespace

TEST_CASE("Drift report: candidate models of oscillator drift", "[.][drift-report]")
{
    std::ostringstream out;
    out << "# Drift report\n";
    reportStatisticsTable(out);
    reportIndependenceTable(out);
    reportStepsTable(out);
    reportStaircaseTable(out);
    std::cout << out.str() << std::flush;
    SUCCEED();
}

TEST_CASE("Drift report: ten hours of each model", "[.][drift-long-report]")
{
    std::ostringstream out;
    out << "\n## Ten hours\n\n"
        << "sigma 1 cent, rate " << referenceRate << " Hz, at " << controlHz << " Hz. Per minute: the mean and the "
        << "standard deviation of each one-minute window.\n\n"
        << "| Model | peak (cents) | sigma, first / last hour | mean, first / last hour | worst minute mean | "
           "minute sigma range | beyond 3 sigma (ppm) |\n"
        << "|---|---:|---:|---:|---:|---:|---:|\n";
    reportLong<OnePole>(out);
    reportLong<TwoPole>(out);
    reportLong<SampleHold<false>>(out);
    reportLong<SampleHold<true>>(out);
    reportLong<SumOfSines>(out);
    reportLong<Pink<8>>(out);
    std::cout << out.str() << std::flush;
    SUCCEED();
}
