// The drift report: PitchDrift (docs/dsp/oscillator-drift.md), as built.
//
// - Its statistics at every sample rate: standard deviation, peak, kurtosis,
//   where its variance lies in frequency, and what lies above 20 Hz.
// - Note-ons from silence: the distribution of the starting values.
// - Independence of voices seeded from adjacent seeds.
// - The pitch-step threshold (bandLimitedPitchStep) at the top of the range.
// - The hold's sidebands on a sine.
// - Against a 4x-oversampled render of the same pitch path: what the 1x
//   oscillator adds with drift (the bend path's slow error) and without.
//
// Hidden from the default run; run it in an optimised build (about 10 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[drift-report]"
//
// Ten hours, checking that nothing wanders, is a separate hidden case:
//
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[drift-long-report]"
//
// The six candidate models this was chosen from are measured by this report
// at commit 264f362. CPU: tools/bench.sh "[drift]".

#include "range_support.hpp"

#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/oscillators/pitch_drift.hpp>
#include <anoesis/dsp/resampling/oversampler.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;
using namespace anoesis::test::range;
using anoesis::dsp::PitchDrift;

namespace
{

constexpr std::size_t interval = PitchDrift::updateInterval;
constexpr std::uint64_t seed = 0x5EED;

std::string fixed(double x, int digits)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

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

    [[nodiscard]] double sigma() const noexcept
    {
        return std::sqrt((sum2 / n) - (mean() * mean()));
    }

    [[nodiscard]] double kurtosis() const noexcept
    {
        return (sum4 / n) / ((sum2 / n) * (sum2 / n));
    }
};

PitchDrift preparedDrift(double sampleRate, float cents, float hz, std::uint64_t driftSeed = seed)
{
    PitchDrift drift{driftSeed};
    drift.prepare({.sampleRate = static_cast<float>(sampleRate), .maxBlockSize = 512, .numChannels = 1});
    drift.setDepthCents(cents);
    drift.setRateHz(hz);
    drift.noteOn();
    return drift;
}

// One value per update, in cents.
std::vector<float> updates(PitchDrift& drift, double seconds, double sampleRate)
{
    std::vector<float> cents;
    drift.process(static_cast<std::size_t>(seconds * sampleRate),
                  [&](std::size_t, std::size_t, float semitones) { cents.push_back(100.0f * semitones); });
    return cents;
}

void reportStatistics(std::ostringstream& out)
{
    out << "\n## Statistics at every sample rate\n\n"
        << "Default depth and rate (sigma 1.5 cents, corner 0.5 Hz), one hour of updates (every 64 samples). Half "
           "and 99% of the variance lie below f50 and f99 (theory: 0.221 and 1.69 Hz). Density: the PSD at "
           "0.05 / 0.5 / 5 / 50 Hz relative to 0.05 Hz (theory: 0 / -6.0 / -40.0 / -80.0 dB).\n\n"
        << "| Rate | Update rate | sigma (cents) | mean | peak | kurtosis | f50 (Hz) | f99 (Hz) | density (dB) | "
           "above 20 Hz |\n|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|\n";
    for (const double sampleRate : rates)
    {
        const double updateHz = sampleRate / static_cast<double>(interval);
        auto drift = preparedDrift(sampleRate, PitchDrift::defaultDepthCents, PitchDrift::defaultRateHz);
        const auto x = updates(drift, 3600.0, sampleRate);
        Moments m;
        for (const float v : x)
        {
            m.add(v);
        }
        const auto spectrum = averagedPowerSpectrum(x, updateHz, hannWindow(1U << 16U));
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
            const double f = spectrum.frequencyOf(k);
            f50 = (f50 <= 0.0 && cumulative >= 0.5 * total) ? f : f50;
            f99 = (f99 <= 0.0 && cumulative >= 0.99 * total) ? f : f99;
            above20 += f > 20.0 ? spectrum.power[k] : 0.0;
        }
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
        const double reference = density(0.05);
        const auto relative = [&](double f)
        {
            return fixed(10.0 * std::log10(density(f) / reference), 1);
        };
        out << "| " << sampleRate / 1000.0 << " kHz | " << fixed(updateHz, 1) << " Hz | " << fixed(m.sigma(), 3)
            << " | " << fixed(m.mean(), 3) << " | " << fixed(m.peak, 2) << " | " << fixed(m.kurtosis(), 2) << " | "
            << fixed(f50, 3) << " | " << fixed(f99, 2) << " | 0 / " << relative(0.5) << " / " << relative(5.0) << " / "
            << relative(50.0) << " | " << fixed(10.0 * std::log10(above20 / total), 1) << " dB |\n";
    }
}

void reportNoteOns(std::ostringstream& out)
{
    auto drift = preparedDrift(48000.0, PitchDrift::defaultDepthCents, PitchDrift::defaultRateHz);
    Moments m;
    for (int note = 0; note < 100000; ++note)
    {
        drift.stop();
        drift.noteOn();
        m.add(100.0 * drift.offset());
        drift.advance(interval * 7);
    }
    out << "\n## Note-ons from silence\n\n"
        << "100,000 notes, each started from silence: the starting offset (cents), sigma " << fixed(m.sigma(), 3)
        << ", mean " << fixed(m.mean(), 4) << ", peak " << fixed(m.peak, 2) << ", kurtosis " << fixed(m.kurtosis(), 2)
        << " (a Gaussian: 3; the draw is a sum of four uniforms, bounded at 3.46 sigma).\n";
}

void reportIndependence(std::ostringstream& out)
{
    double sumAbs = 0.0;
    double worst = 0.0;
    constexpr int pairs = 64;
    for (std::uint64_t voice = 0; voice < pairs; ++voice)
    {
        auto a = preparedDrift(48000.0, 1.0f, 0.5f, seed + voice);
        auto b = preparedDrift(48000.0, 1.0f, 0.5f, seed + voice + 1);
        const auto x = updates(a, 600.0, 48000.0);
        const auto y = updates(b, 600.0, 48000.0);
        double xy = 0.0;
        double xx = 0.0;
        double yy = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            xy += static_cast<double>(x[i]) * y[i];
            xx += static_cast<double>(x[i]) * x[i];
            yy += static_cast<double>(y[i]) * y[i];
        }
        const double r = std::abs(xy / std::sqrt(xx * yy));
        sumAbs += r;
        worst = std::max(worst, r);
    }
    out << "\n## Independence of voices\n\n"
        << pairs << " pairs of voices seeded n and n + 1, 10 minutes each: mean |r| " << fixed(sumAbs / pairs, 4)
        << ", worst " << fixed(worst, 4) << " (independent streams: about 0.029 and 0.11).\n";
}

void reportSteps(std::ostringstream& out)
{
    constexpr double sampleRate = 48000.0;
    const float note = anoesis::dsp::frequencyToNote(static_cast<float>(0.44 * sampleRate));
    out << "\n## Against the pitch-step threshold\n\n"
        << "A voice at 0.44 fs (21.1 kHz at 48 kHz) with the drift added to its note, held per 64 samples, 60 s: the "
           "largest change of increment in units of bandLimitedPitchStep, and how many changes per second reach "
           "it.\n\n| Depth, rate | Largest change | Crossings |\n|---|---:|---:|\n";
    for (const auto& [cents, hz] : {std::pair{1.5f, 0.5f}, std::pair{5.0f, 2.0f}, std::pair{20.0f, 5.0f}})
    {
        auto drift = preparedDrift(sampleRate, cents, hz);
        std::uint32_t previous = 0;
        bool first = true;
        double worst = 0.0;
        std::size_t crossings = 0;
        drift.process(static_cast<std::size_t>(60.0 * sampleRate),
                      [&](std::size_t, std::size_t, float semitones)
                      {
                          const auto increment = anoesis::dsp::incrementFor(
                              anoesis::dsp::noteToFrequency(note + semitones), static_cast<float>(sampleRate));
                          if (!first)
                          {
                              const auto change = increment > previous ? increment - previous : previous - increment;
                              worst = std::max(worst, static_cast<double>(change));
                              crossings += change >= anoesis::dsp::bandLimitedPitchStep ? 1U : 0U;
                          }
                          first = false;
                          previous = increment;
                      });
        out << "| " << cents << " cents, " << hz << " Hz | "
            << fixed(worst / static_cast<double>(anoesis::dsp::bandLimitedPitchStep), 3) << " | "
            << fixed(static_cast<double>(crossings) / 60.0, 1) << "/s |\n";
    }
}

void reportSidebands(std::ostringstream& out)
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t length = 1U << 18U;
    out << "\n## The hold's sidebands\n\n"
        << "A sine through a one-voice slot, its note plus the drift (held per 64 samples, 750 Hz), 2^18 samples at "
           "48 kHz: the power at f +- 750 k Hz (k = 1..4) relative to the carrier.\n\n"
        << "| Depth, rate | 1 kHz | 5 kHz | 21.1 kHz |\n|---|---:|---:|---:|\n";
    for (const auto& [cents, hz] : {std::pair{1.5f, 0.5f}, std::pair{5.0f, 2.0f}})
    {
        out << "| " << cents << " cents, " << hz << " Hz";
        for (const double carrierHz : {1000.0, 5000.0, 0.44 * sampleRate})
        {
            auto slot = preparedSlot(Waveform::sine, 1, sampleRate);
            slot.setPan(-1.0f);
            slot.noteOn();
            auto drift = preparedDrift(sampleRate, cents, hz);
            const auto note = static_cast<float>(noteOf(carrierHz));
            Buffer buffer{2, length + 4096};
            drift.process(length + 4096, [&](std::size_t start, std::size_t run, float semitones)
                          { slot.process(buffer.block().subBlock(start, run), note + semitones); });
            const auto spectrum = powerSpectrum(std::span{buffer.channel(0)}.subspan(4096, length), sampleRate,
                                                blackmanHarrisWindow(length));
            const auto halfWidth = static_cast<std::size_t>(
                std::ceil(((carrierHz * 5.0 * static_cast<double>(cents) * 5.8e-4) + 20.0) / spectrum.binWidth()));
            const double carrier = spectrum.powerAround(carrierHz, halfWidth);
            double sidebands = 0.0;
            for (int k = 1; k <= 4; ++k)
            {
                for (const double side : {-1.0, 1.0})
                {
                    const double f = carrierHz + (side * 750.0 * k);
                    sidebands += (f > 0.0 && f < sampleRate / 2.0) ? spectrum.powerAround(f, halfWidth) : 0.0;
                }
            }
            out << " | " << fixed(10.0 * std::log10(sidebands / carrier), 1) << " dB";
        }
        out << " |\n";
    }
}

// Power away from the harmonics of f0 (each masked by twice its peak
// deviation plus 20 Hz), relative to the fundamental's.
double powerAwayFromHarmonics(std::span<const float> x, double f0, double sampleRate, double peakCents)
{
    const auto spectrum = powerSpectrum(x, sampleRate, blackmanHarrisWindow(x.size()));
    const double lobeHz = static_cast<double>(spectrum.mainLobeBins + 1) * spectrum.binWidth();
    double outside = 0.0;
    double fundamental = 0.0;
    for (std::size_t k = 1; k < spectrum.power.size(); ++k)
    {
        const double f = spectrum.frequencyOf(k);
        if (f >= 20000.0)
        {
            break;
        }
        const double harmonic = std::max(std::round(f / f0), 1.0) * f0;
        const double width = (2.0 * harmonic * peakCents * std::numbers::ln2 / 1200.0) + 20.0 + lobeHz;
        outside += (std::abs(f - harmonic) > width && f > lobeHz) ? spectrum.power[k] : 0.0;
        fundamental += std::abs(f - f0) <= width ? spectrum.power[k] : 0.0;
    }
    return outside / fundamental;
}

void reportAgainstOversampled(std::ostringstream& out)
{
    constexpr double sampleRate = 44100.0;
    constexpr double f0 = sampleRate / 10.5;
    constexpr std::size_t block = 512;
    constexpr std::size_t warmup = 16 * block;
    constexpr std::size_t length = 1U << 16U;
    const auto note = static_cast<float>(noteOf(f0));
    out << "\n## Against a 4x-oversampled render of the same pitch path\n\n"
        << "A one-voice slot at fs / 10.5 (4.2 kHz at 44.1 kHz), and the same slot on the same held pitch path at "
           "4x, decimated by the library's Oversampler. Away from the harmonics (masked by twice their peak "
           "deviation plus 20 Hz), below 20 kHz, relative to the fundamental: the 1x render's power, and its "
           "excess over the 4x reference (what the 1x oscillator adds).\n\n"
        << "| Waveform | Drift | 1x | 4x reference | Excess |\n|---|---|---:|---:|---:|\n";
    for (const auto shape : {Waveform::saw, Waveform::triangle})
    {
        for (const auto& [cents, hz] : {std::pair{0.0f, 0.5f}, std::pair{1.5f, 0.5f}, std::pair{5.0f, 2.0f}})
        {
            auto base = preparedSlot(shape, 1, sampleRate);
            auto fast = preparedSlot(shape, 1, 4.0 * sampleRate);
            anoesis::dsp::Oversampler oversampler{anoesis::dsp::OversamplingFactor::four};
            oversampler.prepare(
                {.sampleRate = static_cast<float>(sampleRate), .maxBlockSize = block, .numChannels = 2});
            for (auto* slot : {&base, &fast})
            {
                slot->setPan(-1.0f);
                slot->noteOn();
            }
            auto drift = preparedDrift(sampleRate, cents, hz, 5);
            Buffer one{2, warmup + length};
            Buffer four{2, warmup + length};
            double peak = 0.0;
            for (std::size_t start = 0; start < warmup + length; start += block)
            {
                Buffer silent{2, block};
                const auto inner = oversampler.upsample(silent.block());
                drift.process(block,
                              [&](std::size_t from, std::size_t run, float semitones)
                              {
                                  peak = std::max(peak, std::abs(100.0 * static_cast<double>(semitones)));
                                  base.process(one.block().subBlock(start + from, run), note + semitones);
                                  fast.process(inner.subBlock(4 * from, 4 * run), note + semitones);
                              });
                oversampler.downsample(four.block().subBlock(start, block));
            }
            // Masks as for the drift at that depth, so with and without
            // compare like with like.
            const double maskCents = cents > 0.0f ? peak : 0.0;
            const double p1 =
                powerAwayFromHarmonics(std::span{one.channel(0)}.subspan(warmup), f0, sampleRate, maskCents);
            const double p4 =
                powerAwayFromHarmonics(std::span{four.channel(0)}.subspan(warmup), f0, sampleRate, maskCents);
            const double excess = p1 - p4;
            out << "| " << shapeName(shape) << " | "
                << (cents > 0.0f ? fixed(cents, 1) + " cents, " + fixed(hz, 1) + " Hz" : std::string{"none"}) << " | "
                << fixed(10.0 * std::log10(p1), 1) << " dB | " << fixed(10.0 * std::log10(p4), 1) << " dB | "
                << (excess > 0.0 ? fixed(10.0 * std::log10(excess), 1) + " dB" : std::string{"< 0"}) << " |\n";
        }
    }
}

} // namespace

TEST_CASE("Drift report: PitchDrift as built", "[.][drift-report]")
{
    std::ostringstream out;
    out << "# Drift report\n";
    reportStatistics(out);
    reportNoteOns(out);
    reportIndependence(out);
    reportSteps(out);
    reportSidebands(out);
    reportAgainstOversampled(out);
    std::cout << out.str() << std::flush;
    SUCCEED();
}

TEST_CASE("Drift report: ten hours", "[.][drift-long-report]")
{
    constexpr double sampleRate = 48000.0;
    auto drift = preparedDrift(sampleRate, 1.0f, PitchDrift::defaultRateHz);
    const auto perMinute = static_cast<std::size_t>(60.0 * sampleRate);
    constexpr std::size_t minutes = 600;
    Moments all;
    Moments firstHour;
    Moments lastHour;
    double worstMinuteMean = 0.0;
    double minSigma = 1e9;
    double maxSigma = 0.0;
    double beyond3 = 0.0;
    for (std::size_t minute = 0; minute < minutes; ++minute)
    {
        Moments m;
        drift.process(perMinute,
                      [&](std::size_t, std::size_t, float semitones)
                      {
                          const double x = 100.0 * static_cast<double>(semitones);
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
                          beyond3 += std::abs(x) > 3.0 ? 1.0 : 0.0;
                      });
        worstMinuteMean = std::max(worstMinuteMean, std::abs(m.mean()));
        minSigma = std::min(minSigma, m.sigma());
        maxSigma = std::max(maxSigma, m.sigma());
    }
    std::ostringstream out;
    out << "\n## Ten hours\n\n"
        << "sigma 1 cent, corner 0.5 Hz, 48 kHz, one value per update. Peak " << fixed(all.peak, 2)
        << "; sigma first / last hour " << fixed(firstHour.sigma(), 3) << " / " << fixed(lastHour.sigma(), 3)
        << "; mean first / last hour " << fixed(firstHour.mean(), 3) << " / " << fixed(lastHour.mean(), 3)
        << "; worst one-minute mean " << fixed(worstMinuteMean, 3) << "; one-minute sigma " << fixed(minSigma, 2)
        << " to " << fixed(maxSigma, 2) << "; beyond 3 sigma " << fixed(1e6 * beyond3 / all.n, 0)
        << " ppm (a Gaussian: 2,700).\n";
    std::cout << out.str() << std::flush;
    SUCCEED();
}
