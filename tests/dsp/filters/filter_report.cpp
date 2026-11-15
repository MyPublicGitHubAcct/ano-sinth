// The filter report: measures LadderFilter as built (docs/dsp/filters.md),
// with Warm Drive on (tanh) and off (cubic), at the host rate and inside the
// library's Oversampler at 2x and 4x:
//
// - the small-signal response against the analog prototype (the gain and
//   phase at the cutoff, the largest error below and above it), for LP24
//   and for every response;
// - self-oscillation: the pitch error in cents from 55 Hz to 14 kHz at
//   44.1-192 kHz, the threshold, and the level and pitch against k;
// - the passband against resonance;
// - THD and the harmonic profile against drive;
// - aliasing against drive at 1x, 2x, and 4x;
// - stability at maximum resonance and drive, under an audio-rate cutoff
//   sweep, with DC, and in silence (subnormals, the time to exact zero);
// - bit-identity across block sizes 1, 37, and 4096, and allocation;
// - clicks: a resonant cutoff step through setCutoff() (ramped over 10 ms)
//   and through the grid-rate modulation input.
//
// The candidate models it was chosen from, and the measurements that chose
// it (solver error, the g grid against per-sample g, Newton's iterations),
// are at commit a0e9899 (filter_candidates.hpp).
//
// Hidden from the default run; run it in an optimised build (about 30 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[filter-report]"
//
// CPU: the "[filter]" benchmarks (tools/bench.sh "[filter]").

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/core/processor.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/resampling/oversampler.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/fft.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/signals.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
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
namespace dsp = anoesis::dsp;
using dsp::FilterResponse;
using dsp::LadderFilter;
using dsp::OversamplingFactor;

namespace
{

constexpr double pi = std::numbers::pi;
constexpr std::size_t blockSize = 512;

constexpr std::array<FilterResponse, 6> allResponses{
    FilterResponse::lp24, FilterResponse::lp12, FilterResponse::bp24,
    FilterResponse::bp12, FilterResponse::hp24, FilterResponse::hp12,
};
constexpr std::array<const char*, 6> responseNames{"LP24", "LP12", "BP24", "BP12", "HP24", "HP12"};

std::string fixed(double x, int digits)
{
    if (std::isnan(x))
    {
        return "nan";
    }
    if (std::isinf(x))
    {
        return x < 0 ? "-inf" : "inf";
    }
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

double dbOf(double amplitude)
{
    return 20.0 * std::log10(std::max(amplitude, 1e-300));
}

/// The filter at the host rate or inside the Oversampler.
class Filter
{
public:
    Filter(OversamplingFactor factor, bool warm) : oversampler(factor)
    {
        filter.setWarm(warm);
    }

    void prepare(double rate)
    {
        const dsp::ProcessSpec spec{
            .sampleRate = static_cast<float>(rate),
            .maxBlockSize = blockSize,
            .numChannels = 1,
        };
        oversampler.prepare(spec);
        filter.prepare(oversampler.oversampledSpec());
    }

    LadderFilter& operator*() noexcept
    {
        return filter;
    }
    LadderFilter* operator->() noexcept
    {
        return &filter;
    }

    void process(const dsp::AudioBlock& block) noexcept
    {
        if (oversampler.factor() == OversamplingFactor::none)
        {
            filter.process(block);
            return;
        }
        filter.process(oversampler.upsample(block));
        oversampler.downsample(block);
    }

private:
    LadderFilter filter;
    dsp::Oversampler oversampler;
};

struct Settings
{
    double cutoff = 1000.0;
    float k = 0.0f;
    float drive = 1.0f;
    FilterResponse response = FilterResponse::lp24;
    bool warm = true;
    OversamplingFactor factor = OversamplingFactor::none;
};

Filter makeFilter(double rate, const Settings& s)
{
    Filter f{s.factor, s.warm};
    f->setCutoff(static_cast<float>(s.cutoff));
    f->setResonance(s.k);
    f->setDrive(s.drive);
    f->setResponse(s.response);
    f.prepare(rate);
    return f;
}

std::vector<float> render(Filter& f, std::span<const float> x)
{
    Buffer buffer{1, x.size()};
    std::ranges::copy(x, buffer.channel(0).begin());
    for (std::size_t start = 0; start < x.size(); start += blockSize)
    {
        f.process(buffer.block().subBlock(start, blockSize));
    }
    return {buffer.channel(0).begin(), buffer.channel(0).end()};
}

const char* factorName(OversamplingFactor factor)
{
    switch (factor)
    {
    case OversamplingFactor::two:
        return "2x";
    case OversamplingFactor::four:
        return "4x";
    default:
        return "1x";
    }
}

std::string label(bool warm, OversamplingFactor factor)
{
    return std::string{warm ? "Warm on (tanh)" : "Warm off (cubic)"} + ", " + factorName(factor);
}

std::vector<float> saw(double hz, double rate, std::size_t n, float amplitude = 1.0f)
{
    std::vector<float> x(n);
    double phase = 0.0;
    for (auto& v : x)
    {
        v = amplitude * static_cast<float>((2.0 * phase) - 1.0);
        phase += hz / rate;
        phase -= std::floor(phase);
    }
    return x;
}

// --- The small-signal response ----------------------------------------------

std::complex<double> analogPrototype(FilterResponse r, double k, double frequencyOverCutoff)
{
    const auto& c = dsp::filterTapMixes[static_cast<std::size_t>(r)];
    const std::complex<double> s{0.0, frequencyOverCutoff};
    const auto p = 1.0 + s;
    const std::array<std::complex<double>, 5> powers{p * p * p * p, p * p * p, p * p, p, 1.0};
    std::complex<double> numerator{};
    for (std::size_t i = 0; i < 5; ++i)
    {
        numerator += static_cast<double>(c[i]) * powers[i];
    }
    return numerator / (powers[0] + k);
}

std::vector<Complex> spectrumOf(double rate, const Settings& s, std::size_t fftSize)
{
    constexpr float level = 1e-4f;
    auto spectrum = impulseResponseSpectrum(
        [&](std::span<const float> in)
        {
            auto f = makeFilter(rate, s);
            std::vector<float> x(in.begin(), in.end());
            for (auto& v : x)
            {
                v *= level;
            }
            auto y = render(f, x);
            for (auto& v : y)
            {
                v /= level;
            }
            return y;
        },
        fftSize);
    if (s.factor != OversamplingFactor::none)
    {
        // Divide out the Oversampler's own response (its delay and phase).
        dsp::Oversampler identity{s.factor};
        identity.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = blockSize, .numChannels = 1});
        const auto own = impulseResponseSpectrum(
            [&](std::span<const float> in)
            {
                Buffer buffer{1, in.size()};
                std::ranges::copy(in, buffer.channel(0).begin());
                for (std::size_t start = 0; start < in.size(); start += blockSize)
                {
                    static_cast<void>(identity.upsample(buffer.block().subBlock(start, blockSize)));
                    identity.downsample(buffer.block().subBlock(start, blockSize));
                }
                return std::vector<float>(buffer.channel(0).begin(), buffer.channel(0).end());
            },
            fftSize);
        for (std::size_t i = 0; i < spectrum.size(); ++i)
        {
            spectrum[i] /= own[i];
        }
    }
    return spectrum;
}

struct ResponseError
{
    double gainAtCutoffDb = 0.0;
    double phaseAtCutoffDeg = 0.0;
    double worstBelowDb = 0.0;
    double worstAboveDb = 0.0;
};

ResponseError compare(const std::vector<Complex>& spectrum, double rate, const Settings& s)
{
    const auto fftSize = (spectrum.size() - 1) * 2;
    const double binHz = rate / static_cast<double>(fftSize);
    const auto analog = [&](double hz)
    {
        return analogPrototype(s.response, s.k, hz / s.cutoff);
    };
    ResponseError e;
    const auto bin = static_cast<std::size_t>(std::lround(s.cutoff / binHz));
    const double at = static_cast<double>(bin) * binHz;
    e.gainAtCutoffDb = dbOf(std::abs(spectrum[bin])) - dbOf(std::abs(analog(at)));
    e.phaseAtCutoffDeg = std::arg(spectrum[bin] / analog(at)) * 180.0 / pi;
    const double peak = std::max(0.0, dbOf(std::abs(analog(s.cutoff))));
    for (std::size_t i = 1; i < spectrum.size(); ++i)
    {
        const double hz = static_cast<double>(i) * binHz;
        const double reference = dbOf(std::abs(analog(hz)));
        if (hz < 20.0 || hz > 20000.0 || reference < peak - 30.0)
        {
            continue;
        }
        auto& worst = hz <= s.cutoff ? e.worstBelowDb : e.worstAboveDb;
        worst = std::max(worst, std::fabs(dbOf(std::abs(spectrum[i])) - reference));
    }
    return e;
}

void reportResponse()
{
    constexpr double rate = 44100.0;
    constexpr std::size_t fftSize = 65536;
    std::cout << "\n### Small-signal response against the analog prototype (LP24, 44.1 kHz, -80 dBFS)\n\n"
                 "Gain error at the cutoff (dB) / phase error there (degrees) / the largest magnitude error (dB) from "
                 "20 Hz to the cutoff / from the cutoff to 20 kHz where the prototype is within 30 dB of its peak. "
                 "Oversampled rows are divided by the Oversampler's own response.\n\n"
                 "| Filter | k | 100 Hz | 1 kHz | 5 kHz | 10 kHz | 15 kHz | 19 kHz |\n"
                 "|---|---|---|---|---|---|---|---|\n";
    for (const bool warm : {true, false})
    {
        for (const auto factor : {OversamplingFactor::none, OversamplingFactor::two, OversamplingFactor::four})
        {
            for (const float k : {0.0f, 3.0f})
            {
                std::cout << "| " << label(warm, factor) << " | " << fixed(k, 0);
                for (const double cutoff : {100.0, 1000.0, 5000.0, 10000.0, 15000.0, 19000.0})
                {
                    const Settings s{.cutoff = cutoff, .k = k, .warm = warm, .factor = factor};
                    const auto e = compare(spectrumOf(rate, s, fftSize), rate, s);
                    std::cout << " | " << fixed(e.gainAtCutoffDb, 2) << " / " << fixed(e.phaseAtCutoffDeg, 1) << " / "
                              << fixed(e.worstBelowDb, 2) << " / " << fixed(e.worstAboveDb, 1);
                }
                std::cout << " |\n";
            }
        }
    }

    std::cout << "\n**Every response** (Warm on, 44.1 kHz; the largest error in dB from 20 Hz to 20 kHz where the "
                 "prototype is within 30 dB of its peak, at cutoffs of 1 kHz and 10 kHz)\n\n"
                 "| Response | k | 1x: 1k / 10k | 2x: 1k / 10k |\n|---|---|---|---|\n";
    for (std::size_t r = 0; r < allResponses.size(); ++r)
    {
        for (const float k : {0.0f, 3.0f})
        {
            std::cout << "| " << responseNames[r] << " | " << fixed(k, 0);
            for (const auto factor : {OversamplingFactor::none, OversamplingFactor::two})
            {
                std::cout << " | ";
                for (const double cutoff : {1000.0, 10000.0})
                {
                    const Settings s{.cutoff = cutoff, .k = k, .response = allResponses[r], .factor = factor};
                    const auto e = compare(spectrumOf(rate, s, fftSize), rate, s);
                    std::cout << fixed(std::max(e.worstBelowDb, e.worstAboveDb), 2) << (cutoff < 5000.0 ? " / " : "");
                }
            }
            std::cout << " |\n";
        }
    }
}

// --- Self-oscillation -------------------------------------------------------

struct Oscillation
{
    double cents = std::numeric_limits<double>::quiet_NaN();
    double levelDb = -std::numeric_limits<double>::infinity();
};

Oscillation selfOscillate(double rate, const Settings& s, double seconds = 2.5)
{
    auto f = makeFilter(rate, s);
    std::vector<float> x(static_cast<std::size_t>(seconds * rate), 0.0f);
    x[0] = 0.5f;
    const auto y = render(f, x);
    const auto tail = std::span{y}.subspan(y.size() - static_cast<std::size_t>(0.5 * rate));
    double sum = 0.0;
    for (const float v : tail)
    {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    const double rms = std::sqrt(sum / static_cast<double>(tail.size()));
    Oscillation o;
    o.levelDb = dbOf(rms * std::numbers::sqrt2);
    if (rms > 1e-6 && std::isfinite(rms))
    {
        o.cents = 1200.0 * std::log2(measureFrequency(tail, rate) / s.cutoff);
    }
    return o;
}

double threshold(double rate, const Settings& base)
{
    const double period = rate / base.cutoff;
    const auto grows = [&](double k)
    {
        auto s = base;
        s.k = static_cast<float>(k);
        auto f = makeFilter(rate, s);
        std::vector<float> x(static_cast<std::size_t>(40.0 * period) + 1, 0.0f);
        x[0] = 1e-15f;
        const auto y = render(f, x);
        const auto rms = [&](double from, double to)
        {
            double sum = 0.0;
            for (auto i = static_cast<std::size_t>(from * period); i < static_cast<std::size_t>(to * period); ++i)
            {
                sum += static_cast<double>(y[i]) * static_cast<double>(y[i]);
            }
            return sum;
        };
        return rms(35.0, 40.0) > rms(10.0, 15.0);
    };
    double lo = 2.0;
    double hi = 8.0;
    for (int i = 0; i < 18; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (grows(mid) ? hi : lo) = mid;
    }
    return 0.5 * (lo + hi);
}

void reportSelfOscillation()
{
    constexpr std::array<double, 9> cutoffs{55.0, 110.0, 220.0, 440.0, 880.0, 1760.0, 3520.0, 7040.0, 14080.0};
    std::cout
        << "\n### Self-oscillation pitch at k = 4.4 (cents from the cutoff; - where the cutoff is above 0.45 fs)\n\n"
           "| Filter | Rate | 55 | 110 | 220 | 440 | 880 | 1760 | 3520 | 7040 | 14080 |\n"
           "|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const bool warm : {true, false})
    {
        for (const auto factor : {OversamplingFactor::none, OversamplingFactor::two})
        {
            for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0})
            {
                std::cout << "| " << label(warm, factor) << " | " << fixed(rate / 1000.0, 1);
                for (const double cutoff : cutoffs)
                {
                    if (cutoff >= 0.45 * rate)
                    {
                        std::cout << " | -";
                        continue;
                    }
                    const auto o = selfOscillate(rate, {.cutoff = cutoff, .k = 4.4f, .warm = warm, .factor = factor});
                    std::cout << " | " << fixed(o.cents, 2);
                }
                std::cout << " |\n";
            }
        }
    }

    std::cout << "\n### Self-oscillation threshold (the smallest k that grows from a 1e-15 impulse)\n\n"
                 "| Filter | Rate | 110 | 880 | 3520 | 7040 | 14080 |\n|---|---|---|---|---|---|---|\n";
    for (const bool warm : {true, false})
    {
        for (const double rate : {44100.0, 96000.0})
        {
            std::cout << "| " << label(warm, OversamplingFactor::none) << " | " << fixed(rate / 1000.0, 1);
            for (const double cutoff : {110.0, 880.0, 3520.0, 7040.0, 14080.0})
            {
                std::cout << " | " << fixed(threshold(rate, {.cutoff = cutoff, .warm = warm}), 3);
            }
            std::cout << " |\n";
        }
    }

    for (const double cutoff : {440.0, 7040.0})
    {
        std::cout
            << "\n### Self-oscillation level and pitch against k (48 kHz, cutoff " << cutoff
            << " Hz)\n\nPeak-equivalent level (dBFS, from the RMS of the last 0.5 s of 4 s) / pitch error "
               "(cents).\n\n| Filter | k 4.05 | 4.1 | 4.2 | 4.4 | 5 | 6 | 8 |\n|---|---|---|---|---|---|---|---|\n";
        for (const bool warm : {true, false})
        {
            std::cout << "| " << label(warm, OversamplingFactor::none);
            for (const float k : {4.05f, 4.1f, 4.2f, 4.4f, 5.0f, 6.0f, 8.0f})
            {
                const auto o = selfOscillate(48000.0, {.cutoff = cutoff, .k = k, .warm = warm}, 4.0);
                std::cout << " | " << fixed(o.levelDb, 1) << " / " << fixed(o.cents, 1);
            }
            std::cout << " |\n";
        }
    }
}

// --- The passband -----------------------------------------------------------

void reportPassband()
{
    constexpr double rate = 48000.0;
    std::cout << "\n### Passband against resonance (48 kHz, cutoff 2 kHz, a 20 Hz probe; dB)\n\n"
                 "The component at the probe's frequency (measureResponseAt), which reads the passband even while the "
                 "filter self-oscillates. Theory for the linear ladder: 1/(1 + k).\n\n"
                 "| Filter | Response | Probe | k 0 | 1 | 2 | 3 | 4 | 5 | 6 | 8 |\n"
                 "|---|---|---|---|---|---|---|---|---|---|---|\n";
    std::cout << "| 1/(1 + k) | | |";
    for (const double k : {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0})
    {
        std::cout << " " << fixed(-20.0 * std::log10(1.0 + k), 1) << " |";
    }
    std::cout << "\n";
    for (const bool warm : {true, false})
    {
        for (const auto response : {FilterResponse::lp24, FilterResponse::lp12})
        {
            for (const double amplitude : {0.01, 0.5})
            {
                std::cout << "| " << label(warm, OversamplingFactor::none) << " | "
                          << responseNames[static_cast<std::size_t>(response)] << " | "
                          << (amplitude < 0.1 ? "-40 dBFS" : "-6 dBFS");
                for (const float k : {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f})
                {
                    const MonoProcess process = [&](std::span<const float> in)
                    {
                        auto f = makeFilter(rate, {.cutoff = 2000.0, .k = k, .response = response, .warm = warm});
                        return render(f, in);
                    };
                    std::cout << " | " << fixed(measureResponseAt(process, 20.0, rate, amplitude).magnitudeDb, 1);
                }
                std::cout << " |\n";
            }
        }
    }
}

// --- Harmonics and aliasing --------------------------------------------------

void reportHarmonics()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 65536;
    constexpr std::size_t settle = 8192;
    const double f0 = coherentFrequency(220.0, rate, n);
    std::cout
        << "\n### THD and harmonics against drive (LP24, cutoff 10 kHz, k 0, a 220 Hz sine at -6 dBFS, 48 kHz)\n\n"
           "Drive is a gain on the input, with no make-up. Harmonics in dB re the fundamental.\n\n"
           "| Filter | Drive | Fundamental (dBFS) | THD (%) | H2 | H3 | H5 | H7 | H9 |\n"
           "|---|---|---|---|---|---|---|---|---|\n";
    for (const bool warm : {true, false})
    {
        for (const float drive : {1.0f, 2.0f, 4.0f, 8.0f, 16.0f})
        {
            auto f = makeFilter(rate, {.cutoff = 10000.0, .drive = drive, .warm = warm});
            const auto y = render(f, sine(f0, rate, settle + n, 0.5));
            const auto tail = std::span{y}.subspan(settle, n);
            const auto window = rectangularWindow(n);
            const auto h = measureHarmonics(tail, f0, rate, window, 20000.0);
            std::cout << "| " << label(warm, OversamplingFactor::none) << " | " << fixed(drive, 0) << " | "
                      << fixed(dbOf(h[0]), 1) << " | " << fixed(100.0 * measureThd(tail, f0, rate, window, 10), 2);
            for (const std::size_t k : {2U, 3U, 5U, 7U, 9U})
            {
                std::cout << " | " << fixed(dbOf(h[k - 1] / h[0]), 1);
            }
            std::cout << " |\n";
        }
    }
}

void reportAliasing()
{
    constexpr double rate = 44100.0;
    constexpr std::size_t n = 65536;
    constexpr std::size_t settle = 16384;
    const double f0 = coherentFrequency(2500.0, rate, n);
    for (const auto& [k, cutoff, title] :
         {std::tuple{0.0f, 15000.0, "LP24, cutoff 15 kHz, k 0"}, std::tuple{3.0f, 5000.0, "LP24, cutoff 5 kHz, k 3"}})
    {
        std::cout
            << "\n### Aliasing against drive and oversampling (" << title
            << ", a 2.5 kHz sine at -6 dBFS, 44.1 kHz)\n\n"
               "The strongest single alias below 20 kHz, dB re the fundamental / all aliasing below 20 kHz, dB "
               "re the harmonics.\n\n| Filter | Drive 1 | Drive 2 | Drive 4 | Drive 16 |\n|---|---|---|---|---|\n";
        for (const bool warm : {true, false})
        {
            for (const auto factor : {OversamplingFactor::none, OversamplingFactor::two, OversamplingFactor::four})
            {
                std::cout << "| " << label(warm, factor);
                for (const float drive : {1.0f, 2.0f, 4.0f, 16.0f})
                {
                    auto f =
                        makeFilter(rate, {.cutoff = cutoff, .k = k, .drive = drive, .warm = warm, .factor = factor});
                    const auto y = render(f, sine(f0, rate, settle + n, 0.5));
                    const auto a =
                        measureAliasing(std::span{y}.subspan(settle, n), f0, rate, rectangularWindow(n), 20000.0);
                    std::cout << " | " << fixed(a.worstAliasDb, 1) << " / " << fixed(a.aliasToSignalDb, 1);
                }
                std::cout << " |\n";
            }
        }
    }
}

// --- Stability, block sizes, allocation --------------------------------------

void reportStability()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 96000;
    std::cout << "\n### Stability (48 kHz, 2 s each, no FTZ)\n\n"
                 "Peak |output|. A: k 8, drive 32, a full-scale 110 Hz saw, the cutoff swept 20 Hz - 20 kHz at 500 Hz "
                 "through the modulation input every 32 samples. B: the same sweep at k 3.9, drive 4. C: DC steps "
                 "0 -> 1 -> 10 -> -10, k 4.5, cutoff 1 kHz. D: an impulse, then 20 s of silence at k 3, cutoff "
                 "100 Hz: subnormal outputs, and when the output is exactly 0 for good.\n\n"
                 "| Filter | A | B | C | D: subnormals | D: exactly 0 after |\n|---|---|---|---|---|---|\n";
    for (const bool warm : {true, false})
    {
        for (const auto factor : {OversamplingFactor::none, OversamplingFactor::two, OversamplingFactor::four})
        {
            const auto peakOf = [](std::span<const float> y)
            {
                double peak = 0.0;
                for (const float v : y)
                {
                    peak = std::isfinite(v) ? std::max(peak, static_cast<double>(std::fabs(v)))
                                            : std::numeric_limits<double>::infinity();
                }
                return peak;
            };
            const auto sweep = [&](float k, float drive)
            {
                Filter f{factor, warm};
                f->setCutoff(632.5f);
                f->setResonance(k);
                f->setDrive(drive);
                f.prepare(rate);
                Buffer buffer{1, n};
                std::ranges::copy(saw(110.0, rate, n), buffer.channel(0).begin());
                for (std::size_t start = 0; start < n; start += 32)
                {
                    const double t = static_cast<double>(start) / rate;
                    f->setCutoffModulation(static_cast<float>(4.98 * std::sin(2.0 * pi * 500.0 * t)));
                    f.process(buffer.block().subBlock(start, 32));
                }
                return peakOf(buffer.channel(0));
            };
            const double a = sweep(8.0f, 32.0f);
            const double b = sweep(3.9f, 4.0f);
            auto c = makeFilter(rate, {.cutoff = 1000.0, .k = 4.5f, .warm = warm, .factor = factor});
            std::vector<float> dc(n, 0.0f);
            std::fill(dc.begin() + (n / 4), dc.begin() + (n / 2), 1.0f);
            std::fill(dc.begin() + (n / 2), dc.begin() + (3 * n / 4), 10.0f);
            std::fill(dc.begin() + (3 * n / 4), dc.end(), -10.0f);
            const double cPeak = peakOf(render(c, dc));
            auto d = makeFilter(rate, {.cutoff = 100.0, .k = 3.0f, .warm = warm, .factor = factor});
            std::vector<float> silence(static_cast<std::size_t>(20.0 * rate), 0.0f);
            silence[0] = 1.0f;
            const auto yd = render(d, silence);
            const auto subnormals =
                std::ranges::count_if(yd, [](float v) { return std::fpclassify(v) == FP_SUBNORMAL; });
            const auto nonZero = std::ranges::find_last_if(yd, [](float v) { return v != 0.0f; });
            const auto silentFrom =
                static_cast<double>(nonZero.empty() ? 0 : std::distance(yd.begin(), nonZero.begin()) + 1) / rate;
            std::cout << "| " << label(warm, factor) << " | " << fixed(a, 2) << " | " << fixed(b, 2) << " | "
                      << fixed(cPeak, 2) << " | " << subnormals << " | " << fixed(silentFrom, 2) << " s |\n";
        }
    }
}

void reportBlockSizes()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 48000;
    std::cout << "\n### Block sizes and allocation (48 kHz, k 3.9, drive 4, a saw; the cutoff, its modulation, and the "
                 "response changed every 1000 samples)\n\n"
                 "Blocks of 1 and 37 against 4096 (split at the control changes), bit for bit; heap allocations while "
                 "processing.\n\n| Filter | 1 | 37 | Allocations |\n|---|---|---|---|\n";
    for (const bool warm : {true, false})
    {
        const auto renderInBlocks = [&](std::size_t size, std::size_t* allocations)
        {
            LadderFilter f;
            f.setWarm(warm);
            f.setCutoff(632.5f);
            f.setResonance(3.9f);
            f.setDrive(4.0f);
            f.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = 4096, .numChannels = 1});
            Buffer buffer{1, n};
            std::ranges::copy(saw(110.0, rate, n), buffer.channel(0).begin());
            const AllocationCounter counter;
            for (std::size_t start = 0; start < n; start += 1000)
            {
                dsp::processInChunks(f, buffer.block().subBlock(start, 1000), size);
                f.setCutoffModulation(static_cast<float>(4.98 * std::sin(0.7 * static_cast<double>(start))));
                f.setResponse(allResponses[(start / 1000) % 6]);
                f.setCutoff(static_cast<float>(200.0 + static_cast<double>(start % 7000)));
            }
            if (allocations != nullptr)
            {
                *allocations = counter.count();
            }
            return std::vector<float>(buffer.channel(0).begin(), buffer.channel(0).end());
        };
        std::size_t allocations = 0;
        const auto reference = renderInBlocks(4096, &allocations);
        const auto same = [&](std::size_t size)
        {
            const auto y = renderInBlocks(size, nullptr);
            return std::ranges::equal(y, reference, [](float p, float q)
                                      { return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q); });
        };
        const bool one = same(1);
        const bool odd = same(37);
        CHECK(one);
        CHECK(odd);
        CHECK(allocations == 0);
        std::cout << "| " << label(warm, OversamplingFactor::none) << " | " << (one ? "identical" : "DIFFER") << " | "
                  << (odd ? "identical" : "DIFFER") << " | " << allocations << " |\n";
    }
}

// --- Clicks -------------------------------------------------------------------

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

void reportClicks()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 48000;
    constexpr std::size_t at = 24000;
    std::vector<float> step(n, 0.0f);
    std::fill(step.begin() + at, step.end(), 1.0f);
    const double unitStep = energyAbove1k(step, at + 32, rate);
    const auto x = sine(150.0, rate, n, 0.25);
    std::cout << "\n### A resonant cutoff step (48 kHz, LP24, k 3.6, a 150 Hz sine at -12 dBFS, 500 Hz -> 4 kHz at a "
                 "grid point)\n\n"
                 "Energy above 1 kHz in a Hann 4096 centred 32 samples after the step, dB re an instant unit step. "
                 "setCutoff(): ramped in log2 over 10 ms. Modulation: setCutoffModulation(3), one 32-sample ramp of "
                 "g. Steady: the cutoff at 4 kHz throughout.\n\n"
                 "| Filter | setCutoff() | Modulation input | Steady |\n|---|---|---|---|\n";
    for (const bool warm : {true, false})
    {
        const auto run = [&](int how)
        {
            Filter f{OversamplingFactor::none, warm};
            f->setCutoff(how == 2 ? 4000.0f : 500.0f);
            f->setResonance(3.6f);
            f.prepare(rate);
            Buffer buffer{1, n};
            std::ranges::copy(x, buffer.channel(0).begin());
            f.process(buffer.block().subBlock(0, at));
            if (how == 0)
            {
                f->setCutoff(4000.0f);
            }
            else if (how == 1)
            {
                f->setCutoffModulation(3.0f);
            }
            f.process(buffer.block().subBlock(at, n - at));
            return 10.0 * std::log10(energyAbove1k(buffer.channel(0), at + 32, rate) / unitStep);
        };
        std::cout << "| " << label(warm, OversamplingFactor::none) << " | " << fixed(run(0), 1) << " | "
                  << fixed(run(1), 1) << " | " << fixed(run(2), 1) << " |\n";
    }
}

} // namespace

TEST_CASE("Filter report: LadderFilter as built", "[.][filter-report]")
{
    std::cout << "\n## Filter report (LadderFilter)\n";
    reportResponse();
    reportSelfOscillation();
    reportPassband();
    reportHarmonics();
    reportAliasing();
    reportStability();
    reportBlockSizes();
    reportClicks();
}
