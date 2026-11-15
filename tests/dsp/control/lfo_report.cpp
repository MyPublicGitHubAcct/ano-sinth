// The LFO report: Lfo as built (docs/dsp/lfos.md), against a naive
// reference (the same rows read at the same 64-bit phase every sample, with
// no slew and no grid). The candidate models it was chosen from were
// measured by this report at commit dae814e (lfo_candidates.hpp there). It
// measures:
//
// - the waveform table: each row's start value, range, and mean;
// - discontinuities: the high-frequency energy of saw and pulse LFOs at
//   1 and 20 Hz, on their own, on a sine's amplitude, and on a filter's
//   cutoff; one edge as a click; and the shape and aliasing at 100 Hz and
//   1 kHz;
// - retriggers, and the first samples after a restart;
// - the curved rows' interpolation: the error and the spurs it adds to a
//   modulated amplitude and pitch;
// - the random rows: distribution, independence, determinism, and the
//   Lorenz row against its rate;
// - a rate change mid-cycle.
//
// Hidden from the default run; run it in an optimised build (about 10 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[lfo-report]"
//
// CPU: the "[lfo]" benchmarks (tools/bench.sh "[lfo]").

#include <anoesis/dsp/control/lfo.hpp>

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
#include <iomanip>
#include <iostream>
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace anoesis::test;
using anoesis::dsp::Lfo;
using anoesis::dsp::LfoKind;
using anoesis::dsp::lfoShape;
using anoesis::dsp::LfoWaveform;
namespace dsp = anoesis::dsp;

namespace
{

constexpr float rate = 48000.0f;
constexpr double rateD = 48000.0;

// The carrier for the amplitude and cutoff measurements. Not a whole number
// of cycles per second, so an LFO's edges meet it at varying phases (a step
// at a zero crossing of the carrier clicks least).
constexpr double carrierHz = 443.7;

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

/// The naive reference: the periodic rows read at a 64-bit phase every
/// sample, with no slew and no grid; a retrigger jumps.
class Naive
{
public:
    void setWaveform(LfoWaveform waveform) noexcept
    {
        row = &lfoShape(waveform);
    }

    void setFrequency(float hz) noexcept
    {
        frequency = hz;
        if (sampleRate > 0.0)
        {
            const double cycles = std::min(static_cast<double>(frequency) / sampleRate, 0.5);
            increment = static_cast<std::uint64_t>(std::round(std::ldexp(cycles, 64)));
        }
    }

    void prepare(const dsp::ProcessSpec& spec) noexcept
    {
        sampleRate = static_cast<double>(spec.sampleRate);
        setFrequency(frequency);
        restart();
    }

    void restart() noexcept
    {
        phase = static_cast<std::uint64_t>(row->start) << 32U;
    }

    void retrigger() noexcept
    {
        restart();
    }

    void process(std::span<float> out) noexcept
    {
        for (auto& y : out)
        {
            y = row->wave.valueAt(static_cast<std::uint32_t>(phase >> 32U));
            phase += increment;
        }
    }

private:
    const dsp::LfoShape* row = &lfoShape(LfoWaveform::sine);
    float frequency = 1.0f;
    double sampleRate = 0.0;
    std::uint64_t increment = 0;
    std::uint64_t phase = 0;
};

template <typename Visit>
void forEachModel(const Visit& visit)
{
    visit("Naive (reference)", [] { return Naive{}; });
    visit("Lfo, as built", [] { return Lfo{1}; });
}

template <typename Make>
auto preparedModel(Make make, LfoWaveform waveform, float hz)
{
    auto lfo = make();
    lfo.setWaveform(waveform);
    lfo.setFrequency(hz);
    lfo.prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 1});
    return lfo;
}

template <typename L>
std::vector<float> render(L& lfo, std::size_t n)
{
    std::vector<float> y(n);
    lfo.process(y);
    return y;
}

// Power of `x` between lowHz and highHz, Welch-averaged under a 4096-sample
// Hann window.
double bandPower(std::span<const float> x, double lowHz, double highHz)
{
    static const Window window = hannWindow(4096);
    const auto spectrum = averagedPowerSpectrum(x, rateD, window);
    double sum = 0.0;
    for (std::size_t k = 0; k < spectrum.power.size(); ++k)
    {
        const double f = spectrum.frequencyOf(k);
        if (f >= lowHz && f < highHz)
        {
            sum += spectrum.power[k];
        }
    }
    return sum;
}

// The envelope report's click metric: the energy above `cutHz` in a
// 4096-sample Hann window centred on `at`.
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

// In dB re an instant step from 0 to 1 at `at`.
double clickDb(std::span<const float> x, std::size_t at)
{
    std::vector<float> step(8192, 0.0f);
    std::fill(step.begin() + 4096, step.end(), 1.0f);
    return powerToDb(windowedEnergyAbove(x, at, 1000.0) / windowedEnergyAbove(step, 4096, 1000.0));
}

std::vector<float> onAmplitude(std::span<const float> lfo, double hz)
{
    std::vector<float> y(lfo.size());
    for (std::size_t n = 0; n < y.size(); ++n)
    {
        const double carrier = std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / rateD);
        y[n] = static_cast<float>(carrier * (0.5 + (0.5 * static_cast<double>(lfo[n]))));
    }
    return y;
}

// A sine through a TPT one-pole low-pass at 1 kHz x 2^(2 lfo).
std::vector<float> onCutoff(std::span<const float> lfo, double hz)
{
    std::vector<float> y(lfo.size());
    double s = 0.0;
    for (std::size_t n = 0; n < y.size(); ++n)
    {
        const double x = std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / rateD);
        const double g = std::tan(std::numbers::pi * 1000.0 * std::exp2(2.0 * static_cast<double>(lfo[n])) / rateD);
        const double v = (x - s) * g / (1.0 + g);
        const double out = v + s;
        s = out + v;
        y[n] = static_cast<float>(out);
    }
    return y;
}

const char* nameOf(LfoWaveform waveform)
{
    constexpr std::array<const char*, dsp::lfoWaveformCount> names{
        "Sine", "Saw", "Ramp", "Triangle", "Pulse", "HPulse", "QPulse", "TriSaw", "Lorenz", "S&H", "Smooth",
    };
    return names[static_cast<std::size_t>(waveform)];
}

constexpr std::array allWaveforms{
    LfoWaveform::sine,   LfoWaveform::saw,        LfoWaveform::ramp,   LfoWaveform::triangle,
    LfoWaveform::pulse,  LfoWaveform::hpulse,     LfoWaveform::qpulse, LfoWaveform::trisaw,
    LfoWaveform::lorenz, LfoWaveform::sampleHold, LfoWaveform::smooth,
};

// ---------------------------------------------------------------------------

void reportTable()
{
    std::cout << "\n### The waveform table (48 kHz)\n\n"
              << "Periodic rows: one cycle at 1 Hz. Random rows: 1000 cycles at 100 Hz, seed 1.\n\n"
              << "| Row | Start value | Min | Max | Mean |\n|---|---:|---:|---:|---:|\n";
    for (const auto waveform : allWaveforms)
    {
        const bool periodic = lfoShape(waveform).kind == LfoKind::periodic;
        auto lfo = preparedModel([] { return Lfo{1}; }, waveform, periodic ? 1.0f : 100.0f);
        const auto y = render(lfo, std::size_t{48000} * (periodic ? 1U : 10U));
        double sum = 0.0;
        for (const float v : y)
        {
            sum += static_cast<double>(v);
        }
        const auto [lo, hi] = std::ranges::minmax(y);
        std::cout << "| " << nameOf(waveform) << " | " << fixed(y[0], 3) << " | " << fixed(lo, 3) << " | "
                  << fixed(hi, 3) << " | " << fixed(sum / static_cast<double>(y.size()), 4) << " |\n";
    }
}

// ---------------------------------------------------------------------------

void reportDiscontinuities()
{
    constexpr std::size_t length = std::size_t{1} << 19U;
    std::cout << "\n### Discontinuities\n\n"
              << "48 kHz, about 11 s of each, Welch (Hann 4096). LFO alone: power above 1 kHz. On a 443.7 Hz sine's "
                 "amplitude (0.5 + 0.5 lfo): power above 2 kHz. On a one-pole low-pass at 1 kHz x 2^(2 lfo): power "
                 "above 4 kHz. The naive reference's level against its whole signal, then the built LFO's against "
                 "the naive's.\n\n"
              << "| LFO | Naive, dB re its signal: LFO / amplitude / cutoff | Lfo against naive: LFO / amplitude / "
                 "cutoff |\n|---|---|---|\n";
    for (const auto waveform : {LfoWaveform::saw, LfoWaveform::pulse})
    {
        for (const float hz : {1.0f, 20.0f})
        {
            const auto measure = [](std::span<const float> y)
            {
                return std::array{
                    bandPower(y, 1000.0, 24000.0),
                    bandPower(onAmplitude(y, carrierHz), 2000.0, 24000.0),
                    bandPower(onCutoff(y, carrierHz), 4000.0, 24000.0),
                };
            };
            auto naive = preparedModel([] { return Naive{}; }, waveform, hz);
            const auto reference = render(naive, length);
            const auto p0 = measure(reference);
            const std::array total{
                bandPower(reference, 0.0, 24001.0),
                bandPower(onAmplitude(reference, carrierHz), 0.0, 24001.0),
                bandPower(onCutoff(reference, carrierHz), 0.0, 24001.0),
            };
            auto built = preparedModel([] { return Lfo{1}; }, waveform, hz);
            const auto p1 = measure(render(built, length));
            std::cout << "| " << nameOf(waveform) << " " << hz << " Hz | " << db(powerToDb(p0[0] / total[0])) << " / "
                      << db(powerToDb(p0[1] / total[1])) << " / " << db(powerToDb(p0[2] / total[2])) << " | "
                      << db(powerToDb(p1[0] / p0[0])) << " / " << db(powerToDb(p1[1] / p0[1])) << " / "
                      << db(powerToDb(p1[2] / p0[2])) << " |\n";
        }
    }

    std::cout << "\n**One edge as a click** (energy above 1 kHz in a Hann 4096 centred on the edge, dB re an instant "
                 "step from 0 to 1; the edges are steps of 2)\n\n| Model | Saw edge (1 Hz) | Pulse edge (1 Hz, high "
                 "to low) |\n|---|---:|---:|\n";
    forEachModel(
        [](const char* name, auto make)
        {
            auto saw = preparedModel(make, LfoWaveform::saw, 1.0f);
            const auto s = render(saw, 96000 + 4096);
            auto pulse = preparedModel(make, LfoWaveform::pulse, 1.0f);
            const auto p = render(pulse, 96000 + 4096);
            std::cout << "| " << name << " | " << db(clickDb(s, 48000)) << " | " << db(clickDb(p, 24000)) << " |\n";
        });
}

void reportSmear()
{
    constexpr std::size_t length = 65536;
    constexpr std::size_t warmup = 4096;
    static const Window window = blackmanHarrisWindow(length);
    std::cout << "\n### Shape and aliasing at the top of the range\n\n"
              << "48 kHz, coherent frequencies near 100 Hz and 1 kHz, Blackman-Harris. Harmonics 1 and 5 against the "
                 "naive reference's (dB), and the worst non-harmonic tone below 20 kHz (dB re the fundamental).\n\n"
              << "| Model | Saw 100 Hz: h1 / h5 / alias | Saw 1 kHz | Pulse 1 kHz |\n|---|---|---|---|\n";
    std::array<std::vector<double>, 3> reference;
    forEachModel(
        [&](const char* name, auto make)
        {
            std::cout << "| " << name;
            std::size_t column = 0;
            for (const auto& [waveform, target] : {
                     std::pair{LfoWaveform::saw, 100.0},
                     std::pair{LfoWaveform::saw, 1000.0},
                     std::pair{LfoWaveform::pulse, 1000.0},
                 })
            {
                const double hz = coherentFrequency(target, rateD, length);
                auto lfo = preparedModel(make, waveform, static_cast<float>(hz));
                const auto all = render(lfo, length + warmup);
                const std::span<const float> y{all.data() + warmup, length};
                const auto harmonics = measureHarmonics(y, hz, rateD, window, 20000.0);
                if (reference[column].empty())
                {
                    reference[column] = harmonics;
                }
                const auto aliasing = measureAliasing(y, hz, rateD, window, 20000.0);
                std::cout << " | " << db(toDb(harmonics[0] / reference[column][0])) << " / "
                          << db(toDb(harmonics[4] / reference[column][4])) << " / " << db(aliasing.worstAliasDb);
                ++column;
            }
            std::cout << " |\n";
        });
}

// ---------------------------------------------------------------------------

template <typename L>
std::vector<float> renderRetrigger(L lfo, std::size_t at, std::size_t length)
{
    std::vector<float> y(length);
    lfo.process(std::span{y}.first(at));
    lfo.retrigger();
    lfo.process(std::span{y}.subspan(at));
    return y;
}

double largestStep(std::span<const float> y, std::size_t at)
{
    double worst = 0.0;
    for (std::size_t n = at; n < at + 64; ++n)
    {
        worst = std::max(worst, static_cast<double>(std::abs(y[n] - y[n - 1])));
    }
    return worst;
}

void reportRetrigger()
{
    constexpr std::size_t at = 12000;
    constexpr std::size_t length = 24000;
    std::cout << "\n### Retrigger\n\n"
              << "48 kHz, 2 Hz. A sine retriggered at its trough (it restarts at its peak: a jump of +2), and a saw "
                 "half-way (from 0 to -1). Step: the largest change between adjacent samples after it. Click: as "
                 "above. On amplitude: a 443.7 Hz sine at 0.5 + 0.5 lfo, power above 2 kHz in the same window, dB re "
                 "the same sine gated on by a hard step.\n\n"
              << "| Model | Sine: step, click, on amplitude | Saw: step, click |\n|---|---|---|\n";
    std::vector<float> gated(length, 0.0f);
    for (std::size_t n = at; n < length; ++n)
    {
        gated[n] = static_cast<float>(std::sin(2.0 * std::numbers::pi * carrierHz * static_cast<double>(n) / rateD));
    }
    const double gateEnergy = windowedEnergyAbove(gated, at, 2000.0);
    forEachModel(
        [&](const char* name, auto make)
        {
            const auto sine = renderRetrigger(preparedModel(make, LfoWaveform::sine, 2.0f), at, length);
            const auto saw = renderRetrigger(preparedModel(make, LfoWaveform::saw, 2.0f), at, length);
            const auto amp = onAmplitude(sine, carrierHz);
            std::cout << "| " << name << " | " << fixed(largestStep(sine, at), 3) << ", " << db(clickDb(sine, at))
                      << ", " << db(powerToDb(windowedEnergyAbove(amp, at, 2000.0) / gateEnergy)) << " | "
                      << fixed(largestStep(saw, at), 3) << ", " << db(clickDb(saw, at)) << " |\n";
        });

    std::cout << "\n**The first samples after a restart** (a voice from silence; every row at its start value):\n\n"
              << "| Row | Samples 0, 1, 2, 3 |\n|---|---|\n";
    for (const auto waveform : {LfoWaveform::sine, LfoWaveform::saw, LfoWaveform::triangle, LfoWaveform::pulse})
    {
        auto lfo = preparedModel([] { return Lfo{1}; }, waveform, 2.0f);
        static_cast<void>(render(lfo, 7777));
        lfo.restart();
        const auto y = render(lfo, 4);
        std::cout << "| " << nameOf(waveform) << " | " << fixed(y[0], 4) << ", " << fixed(y[1], 4) << ", "
                  << fixed(y[2], 4) << ", " << fixed(y[3], 4) << " |\n";
    }
}

// ---------------------------------------------------------------------------

std::vector<float> vibrato(std::span<const float> lfo, double hz)
{
    std::vector<float> y(lfo.size());
    double phase = 0.0;
    for (std::size_t n = 0; n < y.size(); ++n)
    {
        y[n] = static_cast<float>(std::sin(2.0 * std::numbers::pi * phase));
        phase += hz * std::exp2(static_cast<double>(lfo[n]) / 12.0) / rateD;
        phase -= std::floor(phase);
    }
    return y;
}

// The strongest bin more than 400 Hz from `carrier`, dB re the signal.
double worstSpur(std::span<const float> y, double carrier)
{
    static const Window window = blackmanHarrisWindow(16384);
    const auto spectrum = averagedPowerSpectrum(y, rateD, window);
    double total = 0.0;
    double worst = 0.0;
    for (std::size_t k = 0; k < spectrum.power.size(); ++k)
    {
        total += spectrum.power[k];
        if (std::abs(spectrum.frequencyOf(k) - carrier) > 400.0)
        {
            worst = std::max(worst, spectrum.power[k]);
        }
    }
    return powerToDb(worst / total);
}

void reportInterpolation()
{
    constexpr std::size_t length = std::size_t{1} << 18U;
    std::cout << "\n### The curved rows' interpolation (every 32 samples)\n\n"
              << "48 kHz. Control: the largest difference from the ideal sine at every sample. Spurs: the strongest "
                 "tone more than 400 Hz from a 1 kHz carrier, dB re the signal, with the LFO on the carrier's "
                 "amplitude (0.5 + 0.5 lfo) and on its pitch (+-1 semitone); the ideal sine's row is the floor.\n\n"
              << "| LFO | Control error | Amplitude spur | Pitch spur | Ideal: amplitude, pitch |\n"
              << "|---|---:|---:|---:|---|\n";
    for (const float hz : {0.5f, 5.0f, 40.0f, 200.0f})
    {
        auto lfo = preparedModel([] { return Lfo{1}; }, LfoWaveform::sine, hz);
        const auto y = render(lfo, length);
        std::vector<float> ideal(length);
        const double cycles = std::ldexp(static_cast<double>(lfo.phaseIncrement()), -64);
        double error = 0.0;
        for (std::size_t n = 0; n < length; ++n)
        {
            const double t = 0.25 + (cycles * static_cast<double>(n));
            ideal[n] = static_cast<float>(std::sin(2.0 * std::numbers::pi * t));
            error = std::max(error, std::abs(static_cast<double>(y[n]) - static_cast<double>(ideal[n])));
        }
        std::cout << "| Sine " << hz << " Hz | " << sci(error) << " | " << db(worstSpur(onAmplitude(y, 1000.0), 1000.0))
                  << " | " << db(worstSpur(vibrato(y, 1000.0), 1000.0)) << " | "
                  << db(worstSpur(onAmplitude(ideal, 1000.0), 1000.0)) << ", "
                  << db(worstSpur(vibrato(ideal, 1000.0), 1000.0)) << " |\n";
    }
}

// ---------------------------------------------------------------------------

struct Stats
{
    double mean = 0.0;
    double sigma = 0.0;
    double lo = 0.0;
    double hi = 0.0;
};

Stats statsOf(std::span<const float> x)
{
    double sum = 0.0;
    double sum2 = 0.0;
    for (const float v : x)
    {
        sum += static_cast<double>(v);
        sum2 += static_cast<double>(v) * static_cast<double>(v);
    }
    const auto n = static_cast<double>(x.size());
    const auto [lo, hi] = std::ranges::minmax(x);
    return {
        .mean = sum / n,
        .sigma = std::sqrt(std::max(0.0, (sum2 / n) - ((sum / n) * (sum / n)))),
        .lo = static_cast<double>(lo),
        .hi = static_cast<double>(hi),
    };
}

double correlation(std::span<const float> a, std::span<const float> b)
{
    const auto sa = statsOf(a);
    const auto sb = statsOf(b);
    double sum = 0.0;
    for (std::size_t n = 0; n < a.size(); ++n)
    {
        sum += (static_cast<double>(a[n]) - sa.mean) * (static_cast<double>(b[n]) - sb.mean);
    }
    return sum / static_cast<double>(a.size()) / (sa.sigma * sb.sigma);
}

bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

void reportRandom()
{
    constexpr float hz = 10.0f;
    constexpr std::size_t length = std::size_t{48000} * 40;
    constexpr std::size_t voices = 16;
    std::cout
        << "\n### The random rows\n\n"
        << "48 kHz, 10 Hz, 40 s per voice, 16 voices seeded 1-16. Cycle-to-cycle r: between values half a "
           "cycle into consecutive cycles. Adjacent seeds: the mean |r| between voices seeded n and n + 1.\n\n"
        << "| Row | Mean | Sigma | Min / max | Cycle-to-cycle r | Adjacent seeds, mean abs r | Same seed twice |\n"
        << "|---|---:|---:|---|---:|---:|---|\n";
    for (const auto waveform : {LfoWaveform::sampleHold, LfoWaveform::smooth, LfoWaveform::lorenz})
    {
        std::vector<std::vector<float>> outputs;
        for (std::size_t v = 0; v < voices; ++v)
        {
            Lfo lfo{v + 1};
            lfo.setWaveform(waveform);
            lfo.setFrequency(hz);
            lfo.prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 1});
            outputs.push_back(render(lfo, length));
        }
        std::vector<float> all;
        std::vector<float> perCycle;
        for (const auto& y : outputs)
        {
            all.insert(all.end(), y.begin(), y.end());
            for (std::size_t n = 0; n + 2400 < y.size(); n += 4800)
            {
                perCycle.push_back(y[n + 2400]);
            }
        }
        const auto s = statsOf(all);
        const std::span<const float> c{perCycle};
        const double lag = correlation(c.first(c.size() - 1), c.subspan(1));
        double seedR = 0.0;
        for (std::size_t v = 0; v + 1 < voices; ++v)
        {
            seedR += std::abs(correlation(outputs[v], outputs[v + 1]));
        }
        seedR /= static_cast<double>(voices - 1);
        Lfo again{1};
        again.setWaveform(waveform);
        again.setFrequency(hz);
        again.prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 1});
        const bool same = std::ranges::equal(render(again, length), outputs[0], sameBits);
        CHECK(same);
        std::cout << "| " << nameOf(waveform) << " | " << fixed(s.mean, 4) << " | " << fixed(s.sigma, 3) << " | "
                  << fixed(s.lo, 3) << " / " << fixed(s.hi, 3) << " | " << fixed(lag, 3) << " | " << fixed(seedR, 3)
                  << " | " << (same ? "bit-identical" : "DIFFERS") << " |\n";
    }

    std::cout << "\n**The Lorenz row against its rate** (48 kHz, 200 cycles, seed 1): sigma, and zero crossings per "
                 "cycle (the same at every rate if the rate only scales time).\n\n"
              << "| Rate | Sigma | Zero crossings per cycle |\n|---|---:|---:|\n";
    for (const float r : {0.05f, 0.5f, 5.0f, 50.0f, 500.0f})
    {
        Lfo lfo{1};
        lfo.setWaveform(LfoWaveform::lorenz);
        lfo.setFrequency(r);
        lfo.prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 1});
        const auto y = render(lfo, static_cast<std::size_t>(200.0f * rate / r));
        std::size_t crossings = 0;
        for (std::size_t i = 1; i < y.size(); ++i)
        {
            crossings += (y[i] < 0.0f) != (y[i - 1] < 0.0f) ? 1U : 0U;
        }
        std::cout << "| " << r << " Hz | " << fixed(statsOf(y).sigma, 3) << " | "
                  << fixed(static_cast<double>(crossings) / 200.0, 2) << " |\n";
    }
}

// ---------------------------------------------------------------------------

void reportRateChange()
{
    std::cout << "\n### A rate change mid-cycle\n\n"
              << "48 kHz, 2 Hz to 7 Hz at sample 7000. The largest step between adjacent samples near the change, "
                 "against the waveform's steepest slope per sample at 7 Hz. 1.0 or below: no jump.\n\n"
              << "| Model | Sine | Triangle | Saw (between edges) |\n|---|---:|---:|---:|\n";
    forEachModel(
        [](const char* name, auto make)
        {
            std::cout << "| " << name;
            for (const auto& [waveform, slope] : {
                     std::pair{LfoWaveform::sine, 2.0 * std::numbers::pi * 7.0 / rateD},
                     std::pair{LfoWaveform::triangle, 28.0 / rateD},
                     std::pair{LfoWaveform::saw, 14.0 / rateD},
                 })
            {
                auto lfo = preparedModel(make, waveform, 2.0f);
                std::vector<float> y(7100);
                lfo.process(std::span{y}.first(7000));
                lfo.setFrequency(7.0f);
                lfo.process(std::span{y}.subspan(7000));
                double worst = 0.0;
                for (std::size_t n = 6993; n < 7090; ++n)
                {
                    worst = std::max(worst, static_cast<double>(std::abs(y[n] - y[n - 1])));
                }
                std::cout << " | " << fixed(worst / slope, 3);
            }
            std::cout << " |\n";
        });
}

} // namespace

TEST_CASE("LFO report: as built", "[.][lfo-report]")
{
    std::cout << "\n## LFO report (Lfo, as built)\n";
    reportTable();
    reportDiscontinuities();
    reportSmear();
    reportRetrigger();
    reportInterpolation();
    reportRandom();
    reportRateChange();
}
