// The LFO report: measures the candidate models in lfo_candidates.hpp to
// choose one (docs/dsp/lfos.md). It measures:
//
// - the waveform table: each row's start value, range, and mean;
// - phase accuracy of float, double, 32-bit and 64-bit fixed-point
//   accumulation: frequency error and drift over ten minutes from 0.01 Hz
//   to 2 kHz at 44.1-192 kHz (hours: [lfo-long-report]);
// - discontinuities: the high-frequency energy of saw and pulse LFOs at
//   1 and 20 Hz, on their own, on a sine's amplitude, and on a filter's
//   cutoff; the click of one edge; and how much each treatment smears the
//   shape (droop and aliasing) at 100 Hz and 1 kHz;
// - retriggers: a hard jump, band-limited jumps, and crossfades, and the
//   first sample after a reset;
// - the evaluation rate: per sample against once per 16, 32, or 64
//   samples, held or interpolated, as the error it adds to a modulated
//   amplitude and pitch;
// - the random rows: distribution, independence across cycles and seeds,
//   determinism, spectrum, and the Lorenz system's sensitivity to rounding;
// - rate changes mid-cycle; block sizes 1, 37, and 4096; subnormals;
//   allocation; the lifecycle contract.
//
// Hidden from the default run; run it in an optimised build (about 30 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[lfo-report]"
//
// Three hours of each phase accumulator at every rate (a few minutes):
//
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[lfo-long-report]"
//
// CPU: the "[lfo]" benchmarks (tools/bench.sh "[lfo]").

#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"
#include "lfo_candidates.hpp"

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/lifecycle.hpp>
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
#include <limits>
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::lfo;
namespace dsp = anoesis::dsp;

namespace
{

constexpr float rate = 48000.0f;
constexpr double rateD = 48000.0;
constexpr std::array<double, 4> sampleRates{44100.0, 48000.0, 96000.0, 192000.0};

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

std::string sci(double x, int digits = 1)
{
    std::ostringstream s;
    s << std::scientific << std::setprecision(digits) << x;
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

dsp::ProcessSpec specAt(float sampleRate)
{
    return {.sampleRate = sampleRate, .maxBlockSize = 4096, .numChannels = 1};
}

using NaiveFloat = Lfo<PhaseKind::float32, Edges::naive>;
using NaiveFixed = Lfo<PhaseKind::fixed32, Edges::naive>;
using Slewed = Lfo<PhaseKind::fixed32, Edges::slew>;
using PolyBlep = Lfo<PhaseKind::fixed32, Edges::polyBlep>;
using MinBlepSteps = Lfo<PhaseKind::fixed32, Edges::minBlep>;

const dsp::MinBlepTable& table()
{
    return d6::minBlepTable64();
}

/// Calls visit(name, make) for each model, make() returning a new,
/// unprepared instance.
template <typename Visit>
void forEachModel(const Visit& visit)
{
    visit("Naive, float phase", [] { return NaiveFloat{}; });
    visit("Naive, fixed point", [] { return NaiveFixed{}; });
    visit("Slew 1 ms", [] { return Slewed{}; });
    visit("Slew 1 ms, at most 1/16 cycle",
          []
          {
              Slewed lfo;
              lfo.setSlewCycleFraction(1.0f / 16.0f);
              return lfo;
          });
    visit("PolyBLEP", [] { return PolyBlep{}; });
    visit("MinBLEP steps", [] { return MinBlepSteps{&table()}; });
    visit("Oscillator (minBLEP 64)", [] { return OscillatorLfo{table()}; });
}

template <typename Make>
auto preparedModel(Make make, Wave wave, float hz, float sampleRate = rate)
{
    auto lfo = make();
    lfo.setWave(wave);
    lfo.setFrequency(hz);
    lfo.prepare(specAt(sampleRate));
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
// Hann window (x is truncated to a whole number of segments).
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

// The click metric of the envelope report: the energy above 1 kHz in a
// 4096-sample Hann window centred on `at`, relative to an instant step from
// 0 to 1 there.
double windowedEnergyAbove(std::span<const float> x, std::size_t at, double cutHz)
{
    static const Window window = hannWindow(4096);
    std::vector<float> segment(4096);
    for (std::size_t i = 0; i < 4096; ++i)
    {
        segment[i] = x[at - 2048 + i];
    }
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

double unitStepEnergy(double cutHz)
{
    std::vector<float> step(8192, 0.0f);
    std::fill(step.begin() + 4096, step.end(), 1.0f);
    return windowedEnergyAbove(step, 4096, cutHz);
}

double clickDb(std::span<const float> x, std::size_t at)
{
    return powerToDb(windowedEnergyAbove(x, at, 1000.0) / unitStepEnergy(1000.0));
}

// A sine of `hz` with its amplitude scaled by 0.5 + 0.5 lfo.
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

// A sine of `hz` through a TPT one-pole low-pass whose cutoff is
// 1 kHz x 2^(2 lfo): the LFO on a filter's cutoff, two octaves each way.
std::vector<float> onCutoff(std::span<const float> lfo, double hz)
{
    std::vector<float> y(lfo.size());
    double s = 0.0;
    for (std::size_t n = 0; n < y.size(); ++n)
    {
        const double x = std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / rateD);
        const double cutoff = 1000.0 * std::exp2(2.0 * static_cast<double>(lfo[n]));
        const double g = std::tan(std::numbers::pi * cutoff / rateD);
        const double v = (x - s) * g / (1.0 + g);
        const double out = v + s;
        s = out + v;
        y[n] = static_cast<float>(out);
    }
    return y;
}

const char* kindName(Kind kind)
{
    switch (kind)
    {
    case Kind::periodic:
        return "periodic";
    case Kind::sampleHold:
        return "sample and hold";
    case Kind::smoothRandom:
        return "smoothed random";
    case Kind::lorenz:
        return "Lorenz";
    }
    return "?";
}

constexpr std::array allWaves{
    Wave::sine,   Wave::saw,    Wave::ramp,   Wave::triangle,   Wave::pulse,  Wave::hpulse,
    Wave::qpulse, Wave::trisaw, Wave::lorenz, Wave::sampleHold, Wave::smooth,
};

// ---------------------------------------------------------------------------

void reportTable()
{
    std::cout << "\n### The waveform table (naive, fixed point, 48 kHz)\n\n"
              << "Periodic rows: one cycle at 1 Hz. Random rows: 1000 cycles at 100 Hz, seed 0.\n\n"
              << "| Row | Kind | Start value | Min | Max | Mean |\n|---|---|---:|---:|---:|---:|\n";
    for (const auto wave : allWaves)
    {
        const bool periodic = shapeOf(wave).kind == Kind::periodic;
        auto lfo = preparedModel([] { return NaiveFixed{}; }, wave, periodic ? 1.0f : 100.0f);
        const auto y = render(lfo, std::size_t{48000} * (periodic ? 1U : 10U));
        double sum = 0.0;
        for (const float v : y)
        {
            sum += static_cast<double>(v);
        }
        const auto [lo, hi] = std::ranges::minmax(y);
        std::cout << "| " << shapeOf(wave).name << " | " << kindName(shapeOf(wave).kind) << " | " << fixed(y[0], 3)
                  << " | " << fixed(lo, 3) << " | " << fixed(hi, 3) << " | "
                  << fixed(sum / static_cast<double>(y.size()), 4) << " |\n";
    }
}

// ---------------------------------------------------------------------------

struct PhaseRun
{
    double frequencyError = 0.0; ///< relative: the phase error over the cycles run
    double phaseError = 0.0;     ///< cycles, after the run
};

template <PhaseKind P>
PhaseRun runAccumulator(double hz, double sampleRate, double seconds)
{
    Accumulator<P> acc;
    acc.setCycles(hz / sampleRate);
    acc.setPhase(0);
    const auto samples = static_cast<std::uint64_t>(seconds * sampleRate);
    std::uint64_t wraps = 0;
    std::uint32_t last = acc.phase();
    for (std::uint64_t n = 0; n < samples; ++n)
    {
        acc.advance();
        const std::uint32_t now = acc.phase();
        wraps += now < last ? 1U : 0U;
        last = now;
    }
    const double expected = hz * static_cast<double>(samples) / sampleRate;
    const double actual = static_cast<double>(wraps) + acc.cycles();
    return {.frequencyError = (actual - expected) / expected, .phaseError = actual - expected};
}

void reportPhase(double seconds)
{
    constexpr std::array<double, 4> rates{0.01, 1.0, 100.0, 2000.0};
    std::cout << "\n### Phase accuracy over " << fixed(seconds / 60.0, 0) << " minutes\n\n"
              << "Each accumulator from phase 0 at a constant rate. The effective frequency error (relative), and the "
                 "phase error at the end in cycles (counted wraps plus the final phase, against rate x time). "
                 "Worst over 44.1, 48, 96, and 192 kHz.\n\n"
              << "| Rate | float: frequency error / phase error | double | fixed 32 | fixed 64 (32.32) |\n"
              << "|---|---|---|---|---|\n";
    for (const double hz : rates)
    {
        std::array<PhaseRun, 4> worst{};
        const auto keep = [](PhaseRun& w, const PhaseRun& r)
        {
            if (std::abs(r.frequencyError) > std::abs(w.frequencyError))
            {
                w.frequencyError = r.frequencyError;
            }
            if (std::abs(r.phaseError) > std::abs(w.phaseError))
            {
                w.phaseError = r.phaseError;
            }
        };
        for (const double fs : sampleRates)
        {
            keep(worst[0], runAccumulator<PhaseKind::float32>(hz, fs, seconds));
            keep(worst[1], runAccumulator<PhaseKind::double64>(hz, fs, seconds));
            keep(worst[2], runAccumulator<PhaseKind::fixed32>(hz, fs, seconds));
            keep(worst[3], runAccumulator<PhaseKind::fixed64>(hz, fs, seconds));
        }
        std::cout << "| " << hz << " Hz";
        for (const auto& w : worst)
        {
            std::cout << " | " << sci(w.frequencyError) << " / " << sci(w.phaseError);
        }
        std::cout << " |\n";
    }
}

// ---------------------------------------------------------------------------

void reportDiscontinuities()
{
    constexpr std::size_t length = std::size_t{1} << 19U;
    std::cout << "\n### Discontinuities: high-frequency energy, relative to the naive fixed-point LFO\n\n"
              << "48 kHz, about 11 s of each, Welch (Hann 4096). LFO alone: power above 1 kHz. On a 443.7 Hz sine's "
                 "amplitude (0.5 + 0.5 lfo): power above 2 kHz. On a one-pole low-pass at 1 kHz x 2^(2 lfo) fed a "
                 "443.7 Hz sine: power above 4 kHz. dB against the naive fixed-point model's; its own absolute level "
                 "is in the first row, as dB relative to the whole signal's power.\n\n";
    for (const auto wave : {Wave::saw, Wave::pulse})
    {
        for (const float hz : {1.0f, 20.0f})
        {
            std::vector<float> reference;
            std::array<double, 3> referencePower{};
            std::cout << "**" << shapeOf(wave).name << " at " << hz << " Hz**\n\n"
                      << "| Model | LFO > 1 kHz | Amplitude > 2 kHz | Cutoff > 4 kHz |\n|---|---:|---:|---:|\n";
            const auto measure = [&](std::span<const float> y)
            {
                const auto amp = onAmplitude(y, carrierHz);
                const auto cut = onCutoff(y, carrierHz);
                return std::array{
                    bandPower(y, 1000.0, 24000.0),
                    bandPower(amp, 2000.0, 24000.0),
                    bandPower(cut, 4000.0, 24000.0),
                };
            };
            {
                auto lfo = preparedModel([] { return NaiveFixed{}; }, wave, hz);
                reference = render(lfo, length);
                referencePower = measure(reference);
                const auto total = [&](std::span<const float> y)
                {
                    return bandPower(y, 0.0, 24001.0);
                };
                const auto amp = onAmplitude(reference, carrierHz);
                const auto cut = onCutoff(reference, carrierHz);
                std::cout << "| (naive fixed point, dB re its signal) | "
                          << db(powerToDb(referencePower[0] / total(reference))) << " | "
                          << db(powerToDb(referencePower[1] / total(amp))) << " | "
                          << db(powerToDb(referencePower[2] / total(cut))) << " |\n";
            }
            forEachModel(
                [&](const char* name, auto make)
                {
                    auto lfo = preparedModel(make, wave, hz);
                    const auto y = render(lfo, length);
                    const auto p = measure(y);
                    std::cout << "| " << name << " | " << db(powerToDb(p[0] / referencePower[0])) << " | "
                              << db(powerToDb(p[1] / referencePower[1])) << " | "
                              << db(powerToDb(p[2] / referencePower[2])) << " |\n";
                });
            std::cout << "\n";
        }
    }

    // The spectrum of one saw edge, by octave band.
    std::cout << "**The spectrum of the saw's edges at 1 Hz, by band** (dB against the naive fixed-point model in the "
                 "same band)\n\n| Model | 1-2 kHz | 2-4 kHz | 4-8 kHz | 8-16 kHz | 16-24 kHz |\n"
                 "|---|---:|---:|---:|---:|---:|\n";
    constexpr std::array bands{
        std::pair{1000.0, 2000.0},  std::pair{2000.0, 4000.0},   std::pair{4000.0, 8000.0},
        std::pair{8000.0, 16000.0}, std::pair{16000.0, 24001.0},
    };
    std::array<double, 5> referenceBands{};
    {
        auto lfo = preparedModel([] { return NaiveFixed{}; }, Wave::saw, 1.0f);
        const auto y = render(lfo, length);
        for (std::size_t b = 0; b < bands.size(); ++b)
        {
            referenceBands[b] = bandPower(y, bands[b].first, bands[b].second);
        }
    }
    forEachModel(
        [&](const char* name, auto make)
        {
            auto lfo = preparedModel(make, Wave::saw, 1.0f);
            const auto y = render(lfo, length);
            std::cout << "| " << name;
            for (std::size_t b = 0; b < bands.size(); ++b)
            {
                std::cout << " | " << db(powerToDb(bandPower(y, bands[b].first, bands[b].second) / referenceBands[b]));
            }
            std::cout << " |\n";
        });

    // One edge, as the envelope report measured clicks.
    std::cout << "\n**One edge as a click** (energy above 1 kHz in a Hann 4096 centred on the edge, dB re an instant "
                 "step from 0 to 1; the saw's edge at 1 Hz is a step of -2, the pulse's +-2)\n\n"
                 "| Model | Saw edge | Pulse edge (high to low) |\n|---|---:|---:|\n";
    forEachModel(
        [&](const char* name, auto make)
        {
            auto saw = preparedModel(make, Wave::saw, 1.0f);
            const auto s = render(saw, 96000 + 4096);
            auto pulse = preparedModel(make, Wave::pulse, 1.0f);
            const auto p = render(pulse, 96000 + 4096);
            // The saw's edge is at sample 48000; the pulse falls at 24000.
            std::cout << "| " << name << " | " << db(clickDb(s, 48000)) << " | " << db(clickDb(p, 24000)) << " |\n";
        });
}

// ---------------------------------------------------------------------------

void reportSmear()
{
    constexpr std::size_t length = 65536;
    constexpr std::size_t warmup = 4096;
    static const Window window = blackmanHarrisWindow(length);
    std::cout << "\n### Smear and aliasing at the top of the range\n\n"
              << "48 kHz, coherent frequencies near 100 Hz and 1 kHz, 65536 samples after 4096 of warm-up, "
                 "Blackman-Harris. Harmonics 1 and 5 against the ideal waveform's (dB), and the worst non-harmonic "
                 "tone below 20 kHz (dB re the fundamental).\n\n"
              << "| Model | Saw 100 Hz: h1 / h5 / alias | Saw 1 kHz: h1 / h5 / alias | Pulse 1 kHz: h1 / h5 / alias "
                 "|\n|---|---|---|---|\n";
    forEachModel(
        [&](const char* name, auto make)
        {
            std::cout << "| " << name;
            for (const auto& [wave, target] :
                 {std::pair{Wave::saw, 100.0}, std::pair{Wave::saw, 1000.0}, std::pair{Wave::pulse, 1000.0}})
            {
                const double hz = coherentFrequency(target, rateD, length);
                auto lfo = preparedModel(make, wave, static_cast<float>(hz));
                const auto all = render(lfo, length + warmup);
                const std::span<const float> y{all.data() + warmup, length};
                const auto harmonics = measureHarmonics(y, hz, rateD, window, 20000.0);
                const auto ideal = wave == Wave::saw ? dsp::Waveform::saw : dsp::Waveform::pulse;
                const auto aliasing = measureAliasing(y, hz, rateD, window, 20000.0);
                std::cout << " | " << db(toDb(harmonics[0] / d6::idealHarmonic(ideal, 1))) << " / "
                          << db(toDb(harmonics[4] / std::max(d6::idealHarmonic(ideal, 5), 1e-12))) << " / "
                          << db(aliasing.worstAliasDb);
            }
            std::cout << " |\n";
        });
}

// ---------------------------------------------------------------------------

// The largest step between adjacent samples within 64 samples after `at`.
double largestStep(std::span<const float> y, std::size_t at)
{
    double worst = 0.0;
    for (std::size_t n = at; n < at + 64; ++n)
    {
        worst = std::max(worst, static_cast<double>(std::abs(y[n] - y[n - 1])));
    }
    return worst;
}

template <typename L>
std::vector<float> renderRetrigger(L lfo, std::size_t at, std::size_t length)
{
    std::vector<float> y(length);
    lfo.process(std::span{y}.first(at));
    lfo.retrigger();
    lfo.process(std::span{y}.subspan(at));
    return y;
}

void reportRetrigger()
{
    // A 2 Hz sine starts at its peak and is at its trough after a quarter
    // second; a retrigger there jumps by +2. A 2 Hz saw at half a cycle is at
    // 0, and a retrigger jumps to -1.
    constexpr std::size_t at = 12000;
    constexpr std::size_t length = 24000;
    std::cout << "\n### Retrigger\n\n"
              << "48 kHz, 2 Hz. A sine retriggered at its trough (it restarts at its peak: a jump of +2), and a saw "
                 "retriggered half-way (from 0 to -1). Step: the largest change between adjacent samples after it. "
                 "Click: energy above 1 kHz in a Hann 4096 centred on the "
                 "retrigger, dB re an instant step from 0 to 1. On a 443.7 Hz sine's amplitude: power above 2 kHz in "
                 "the same window, dB re the same sine gated on by a hard step.\n\n"
              << "| Model | Sine: step, click, on amplitude | Saw: step, click |\n|---|---|---|\n";
    std::vector<float> gate(length, 0.0f);
    std::fill(gate.begin() + static_cast<std::ptrdiff_t>(at), gate.end(), 1.0f);
    std::vector<float> gated(length);
    for (std::size_t n = 0; n < length; ++n)
    {
        gated[n] =
            gate[n] * static_cast<float>(std::sin(2.0 * std::numbers::pi * carrierHz * static_cast<double>(n) / rateD));
    }
    const double gateEnergy = windowedEnergyAbove(gated, at, 2000.0);
    const auto row = [&](const std::string& name, auto make)
    {
        const auto sine = renderRetrigger(preparedModel(make, Wave::sine, 2.0f), at, length);
        const auto saw = renderRetrigger(preparedModel(make, Wave::saw, 2.0f), at, length);
        const auto amp = onAmplitude(sine, carrierHz);
        std::cout << "| " << name << " | " << fixed(largestStep(sine, at), 3) << ", " << db(clickDb(sine, at)) << ", "
                  << db(powerToDb(windowedEnergyAbove(amp, at, 2000.0) / gateEnergy)) << " | "
                  << fixed(largestStep(saw, at), 3) << ", " << db(clickDb(saw, at)) << " |\n";
    };
    forEachModel([&](const char* name, auto make) { row(name, make); });
    for (const float fade : {0.001f, 0.005f})
    {
        row("Crossfade " + fixed(static_cast<double>(fade) * 1000.0, 0) + " ms (naive fixed point)",
            [fade]
            {
                NaiveFixed lfo;
                lfo.setRetriggerFade(fade);
                return lfo;
            });
    }
    row("Slew 5 ms",
        []
        {
            Slewed lfo;
            lfo.setSlewSeconds(0.005f);
            return lfo;
        });

    std::cout << "\n**The first samples after a reset** (the start of a voice's LFO: a sine starts at its peak, +1)\n\n"
              << "| Model | Samples 0, 1, 2, 3 |\n|---|---|\n";
    forEachModel(
        [&](const char* name, auto make)
        {
            auto lfo = preparedModel(make, Wave::sine, 2.0f);
            const auto y = render(lfo, 4);
            std::cout << "| " << name << " | " << fixed(y[0], 4) << ", " << fixed(y[1], 4) << ", " << fixed(y[2], 4)
                      << ", " << fixed(y[3], 4) << " |\n";
        });
}

// ---------------------------------------------------------------------------

struct RateErrors
{
    double control = 0.0;   ///< the largest |difference| from the per-sample LFO
    double ampSpur = 0.0;   ///< the strongest tone outside the carrier's own band, dB re the carrier
    double pitchSpur = 0.0; ///< the same, with the LFO on pitch
};

std::vector<float> vibrato(std::span<const float> lfo, double hz, double semitones)
{
    std::vector<float> y(lfo.size());
    double phase = 0.0;
    for (std::size_t n = 0; n < y.size(); ++n)
    {
        y[n] = static_cast<float>(std::sin(2.0 * std::numbers::pi * phase));
        phase += hz * std::exp2(semitones * static_cast<double>(lfo[n]) / 12.0) / rateD;
        phase -= std::floor(phase);
    }
    return y;
}

// The strongest bin more than 400 Hz from `carrier` (beyond the sidebands
// the modulation itself makes, at these depths and rates), in dB re the
// signal's total power.
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

template <typename L>
RateErrors measureRate(L lfo, std::span<const float> reference)
{
    std::vector<float> y(reference.size());
    lfo.process(y);
    RateErrors e;
    for (std::size_t n = 0; n < y.size(); ++n)
    {
        e.control = std::max(e.control, static_cast<double>(std::abs(y[n] - reference[n])));
    }
    e.ampSpur = worstSpur(onAmplitude(y, 1000.0), 1000.0);
    e.pitchSpur = worstSpur(vibrato(y, 1000.0, 1.0), 1000.0);
    return e;
}

void reportEvaluationRate()
{
    constexpr std::size_t length = std::size_t{1} << 18U;
    std::cout << "\n### Evaluation rate\n\n"
              << "48 kHz, against the same LFO per sample (naive, fixed point). Control: the largest difference in the "
                 "LFO's value (full scale +-1). Spurs: the strongest tone more than 400 Hz from a 1 kHz carrier "
                 "(beyond the modulation's own sidebands), dB re the signal, with the LFO on the carrier's "
                 "amplitude (0.5 + 0.5 lfo) and on its pitch (+-1 semitone). The per-sample row is the floor: the "
                 "window's leakage and the modulation's own far sidebands.\n\n"
              << "| LFO | Evaluation | Control error | Amplitude spur | Pitch spur |\n"
              << "|---|---|---:|---:|---:|\n";
    for (const auto& [wave, hz] :
         {std::pair{Wave::sine, 5.0f}, std::pair{Wave::sine, 40.0f}, std::pair{Wave::triangle, 5.0f}})
    {
        auto perSample = preparedModel([] { return NaiveFixed{}; }, wave, hz);
        const auto reference = render(perSample, length);
        const auto row = [&](const char* name, auto lfo)
        {
            lfo.setWave(wave);
            lfo.setFrequency(hz);
            lfo.prepare(specAt(rate));
            const auto e = measureRate(lfo, reference);
            std::cout << "| " << shapeOf(wave).name << " " << hz << " Hz | " << name << " | " << sci(e.control) << " | "
                      << db(e.ampSpur) << " | " << db(e.pitchSpur) << " |\n";
        };
        row("every sample", NaiveFixed{});
        row("every 16, held", ControlRateLfo<16, false>{});
        row("every 32, held", ControlRateLfo<32, false>{});
        row("every 64, held", ControlRateLfo<64, false>{});
        row("every 16, linear", ControlRateLfo<16, true>{});
        row("every 32, linear", ControlRateLfo<32, true>{});
        row("every 64, linear", ControlRateLfo<64, true>{});
    }
    std::cout << "\nA saw or pulse evaluated on a grid moves its edges to the grid: up to N - 1 samples late (0.67 ms "
                 "at N = 32, 48 kHz), so a pulse gating the volume in time with the host jitters by that much.\n";
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

// The frequency below which half the signal's (DC-free) power lies.
double medianFrequency(std::span<const float> x)
{
    static const Window window = hannWindow(65536);
    const auto spectrum = averagedPowerSpectrum(x, rateD, window);
    double total = 0.0;
    for (std::size_t k = 1; k < spectrum.power.size(); ++k)
    {
        total += spectrum.power[k];
    }
    double sum = 0.0;
    for (std::size_t k = 1; k < spectrum.power.size(); ++k)
    {
        sum += spectrum.power[k];
        if (sum >= 0.5 * total)
        {
            return spectrum.frequencyOf(k);
        }
    }
    return 0.0;
}

void reportRandom()
{
    constexpr float hz = 10.0f;
    constexpr std::size_t length = std::size_t{48000} * 40;
    constexpr std::size_t voices = 16;
    std::cout << "\n### The random rows\n\n"
              << "48 kHz, 10 Hz, 40 s per voice, 16 voices seeded 1-16 (naive, fixed point). Stats over every "
                 "sample of every voice; for S&H also over the held values. Seeds: the mean |r| between voices "
                 "with adjacent seeds. Median frequency: half the power lies below it, as a multiple of the rate.\n\n"
              << "| Row | Mean | Sigma | Min / max | Cycle-to-cycle r | Adjacent seeds, mean abs r | Median "
                 "frequency / rate | Same seed twice |\n|---|---:|---:|---|---:|---:|---:|---|\n";
    for (const auto wave : {Wave::sampleHold, Wave::smooth, Wave::lorenz})
    {
        std::vector<std::vector<float>> outputs;
        for (std::size_t v = 0; v < voices; ++v)
        {
            NaiveFixed lfo;
            lfo.setSeed(v + 1);
            lfo.setWave(wave);
            lfo.setFrequency(hz);
            lfo.prepare(specAt(rate));
            outputs.push_back(render(lfo, length));
        }
        std::vector<float> all;
        std::vector<float> perCycle;
        for (const auto& y : outputs)
        {
            all.insert(all.end(), y.begin(), y.end());
            for (std::size_t n = 0; n < y.size(); n += 4800)
            {
                perCycle.push_back(y[n + 2400]);
            }
        }
        const auto s = statsOf(all);
        double lag = 0.0;
        {
            const std::span<const float> c{perCycle};
            lag = correlation(c.first(c.size() - 1), c.subspan(1));
        }
        double seedR = 0.0;
        for (std::size_t v = 0; v + 1 < voices; ++v)
        {
            seedR += std::abs(correlation(outputs[v], outputs[v + 1]));
        }
        seedR /= static_cast<double>(voices - 1);
        NaiveFixed again;
        again.setSeed(1);
        again.setWave(wave);
        again.setFrequency(hz);
        again.prepare(specAt(rate));
        const bool same =
            std::ranges::equal(render(again, length), outputs[0], [](float a, float b)
                               { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); });
        CHECK(same);
        std::cout << "| " << shapeOf(wave).name << " | " << fixed(s.mean, 4) << " | " << fixed(s.sigma, 3) << " | "
                  << fixed(s.lo, 3) << " / " << fixed(s.hi, 3) << " | " << fixed(lag, 3) << " | " << fixed(seedR, 3)
                  << " | " << fixed(medianFrequency(outputs[0]) / static_cast<double>(hz), 2) << " | "
                  << (same ? "bit-identical" : "DIFFERS") << " |\n";
    }
    std::cout << "\n(A uniform distribution on (-1, 1) has sigma 0.577; independent values have r near 0, within "
                 "about 1 / sqrt(n).)\n";

    // The Lorenz row at other rates: its step is rate / fs time units, so at
    // slow rates each step is tiny against the state's float spacing.
    std::cout << "\n**The Lorenz row against its rate** (48 kHz, 200 cycles, seed 1): sigma, zero crossings of the "
                 "output per cycle (the same at every rate if the rate only scales time), and the share of samples "
                 "that repeat the previous one exactly (rounding swallowed the step).\n\n"
              << "| Rate | Sigma | Zero crossings per cycle | Samples equal to the previous |\n|---|---:|---:|---:|\n";
    for (const float r : {0.05f, 0.5f, 5.0f, 50.0f, 500.0f})
    {
        NaiveFixed lfo;
        lfo.setSeed(1);
        lfo.setWave(Wave::lorenz);
        lfo.setFrequency(r);
        lfo.prepare(specAt(rate));
        const auto n = static_cast<std::size_t>(200.0f * rate / r);
        const auto y = render(lfo, n);
        std::size_t stuck = 0;
        std::size_t crossings = 0;
        for (std::size_t i = 1; i < y.size(); ++i)
        {
            stuck += std::bit_cast<std::uint32_t>(y[i]) == std::bit_cast<std::uint32_t>(y[i - 1]) ? 1U : 0U;
            crossings += (y[i] < 0.0f) != (y[i - 1] < 0.0f) ? 1U : 0U;
        }
        std::cout << "| " << r << " Hz | " << fixed(statsOf(y).sigma, 3) << " | "
                  << fixed(static_cast<double>(crossings) / 200.0, 2) << " | "
                  << fixed(100.0 * static_cast<double>(stuck) / static_cast<double>(y.size()), 1) << "% |\n";
    }

    // The Lorenz system amplifies a rounding difference exponentially. The
    // same steps with every product rounded on its own (as a compiler that
    // does not contract a * b + c into a fused multiply-add computes them)
    // against the steps as this compiler built them, and against a start
    // one float step away.
    std::cout << "\n**The Lorenz row's sensitivity to rounding** (48 kHz, from the same settled start): the time "
                 "until two versions differ by 0.1 of full scale. Unfused: every product rounded before it is "
                 "added (volatile), as a compiler that does not contract a * b + c computes it; this build contracts "
                 "where it may. One ulp: the same arithmetic from x one float step away.\n\n"
              << "| Rate | This build against unfused | One ulp apart |\n|---|---:|---:|\n";
    for (const float r : {0.5f, 5.0f, 50.0f})
    {
        const float dt = std::min(r * lorenzTimePerCycle / rate, lorenzMaxStep);
        const auto unfused = [dt](Lorenz& s)
        {
            constexpr float sigma = 10.0f;
            constexpr float rho = 28.0f;
            constexpr float beta = 8.0f / 3.0f;
            const auto round = [](float v)
            {
                const volatile float stored = v;
                return static_cast<float>(stored);
            };
            const float h = 0.5f * dt;
            const float dx1 = round(sigma * (s.y - s.x));
            const float dy1 = round(round(s.x * (rho - s.z)) - s.y);
            const float dz1 = round(round(s.x * s.y) - round(beta * s.z));
            const float xm = s.x + round(h * dx1);
            const float ym = s.y + round(h * dy1);
            const float zm = s.z + round(h * dz1);
            s.x += round(dt * round(sigma * (ym - xm)));
            s.y += round(dt * round(round(xm * (rho - zm)) - ym));
            s.z += round(dt * round(round(xm * ym) - round(beta * zm)));
        };
        const auto limit = static_cast<std::size_t>(600.0f * rate);
        const auto timeApart = [&](Lorenz a, Lorenz b, bool useUnfused)
        {
            std::size_t n = 0;
            while (n < limit && std::abs(a.x - b.x) / lorenzScale < 0.1f)
            {
                a.step(dt);
                if (useUnfused)
                {
                    unfused(b);
                }
                else
                {
                    b.step(dt);
                }
                ++n;
            }
            return n < limit ? fixed(static_cast<double>(n) / rateD, 1) + " s" : std::string{"> 600 s"};
        };
        Lorenz start;
        dsp::Random random{1};
        start.start(random);
        Lorenz nudged = start;
        nudged.x = std::nextafter(nudged.x, 100.0f);
        std::cout << "| " << r << " Hz | " << timeApart(start, start, true) << " | " << timeApart(start, nudged, false)
                  << " |\n";
    }
}

// ---------------------------------------------------------------------------

void reportRateChange()
{
    std::cout << "\n### A rate change mid-cycle\n\n"
              << "48 kHz, 2 Hz to 7 Hz at sample 7000 (part-way through a cycle). The largest step between "
                 "adjacent samples within 8 samples of the change, against the largest the waveform makes at 7 Hz "
                 "(its slope per sample). Below 1.0: no jump.\n\n"
              << "| Model | Sine | Triangle | Saw (between edges) |\n|---|---:|---:|---:|\n";
    forEachModel(
        [&](const char* name, auto make)
        {
            std::cout << "| " << name;
            for (const auto& [wave, slope] : {
                     std::pair{Wave::sine, 2.0 * std::numbers::pi * 7.0 / rateD},
                     std::pair{Wave::triangle, 28.0 / rateD},
                     std::pair{Wave::saw, 14.0 / rateD},
                 })
            {
                auto lfo = preparedModel(make, wave, 2.0f);
                std::vector<float> y(7020);
                lfo.process(std::span{y}.first(7000));
                lfo.setFrequency(7.0f);
                lfo.process(std::span{y}.subspan(7000));
                double worst = 0.0;
                for (std::size_t n = 6993; n < 7009; ++n)
                {
                    worst = std::max(worst, static_cast<double>(std::abs(y[n] - y[n - 1])));
                }
                std::cout << " | " << fixed(worst / slope, 3);
            }
            std::cout << " |\n";
        });
}

// ---------------------------------------------------------------------------

// A script of retriggers and rate changes, rendered in blocks of `block`
// samples, split at rate changes as a host would at automation.
template <typename Make>
std::vector<float> renderScript(Make make, Wave wave, std::size_t block)
{
    constexpr std::size_t length = 96000;
    constexpr std::array<std::size_t, 5> retriggers{1000, 1001, 17777, 40000, 70001};
    constexpr std::array rates{
        std::pair<std::size_t, float>{5000, 7.3f},
        std::pair<std::size_t, float>{30001, 0.25f},
        std::pair<std::size_t, float>{50000, 333.0f},
        std::pair<std::size_t, float>{60123, 3.0f},
    };
    auto lfo = preparedModel(make, wave, 2.0f);
    std::vector<float> y(length);
    std::size_t nextRate = 0;
    for (std::size_t start = 0; start < length;)
    {
        std::size_t end = std::min(start + block, length);
        if (nextRate < rates.size() && rates[nextRate].first < end)
        {
            end = rates[nextRate].first;
        }
        std::vector<LfoEvent> events;
        for (const auto r : retriggers)
        {
            if (r >= start && r < end)
            {
                events.push_back({.offset = r - start});
            }
        }
        lfo.process(std::span{y}.subspan(start, end - start), events);
        start = end;
        if (nextRate < rates.size() && rates[nextRate].first == start)
        {
            lfo.setFrequency(rates[nextRate].second);
            ++nextRate;
        }
    }
    return y;
}

bool sameBits(std::span<const float> a, std::span<const float> b)
{
    return std::ranges::equal(a, b, [](float x, float y)
                              { return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y); });
}

template <typename Visit>
void forEachModelAndControlRate(const Visit& visit)
{
    forEachModel(visit);
    visit("Every 32, held", [] { return ControlRateLfo<32, false>{}; });
    visit("Every 32, linear", [] { return ControlRateLfo<32, true>{}; });
}

void reportContracts()
{
    std::cout
        << "\n### Block sizes, subnormals, allocation, lifecycle\n\n"
        << "Block sizes: a 2 s script of retriggers (including two on adjacent samples) and rate changes "
           "(0.25-333 Hz), rendered in blocks of 1, 37, and 4096: bit-identical? Subnormals: 60 s at 0.5 Hz "
           "with a retrigger every second, every row, outputs and internal state. Allocation: controls, "
           "retriggers, and process calls, counted. Lifecycle: checkLifecycleContract (reset and a second "
           "prepare give a fresh instance's output, bit for bit).\n\n"
        << "| Model | Block sizes | Subnormal outputs / states | Allocations | Lifecycle |\n|---|---|---|---:|---|\n";
    forEachModelAndControlRate(
        [&](const char* name, auto make)
        {
            bool blocksSame = true;
            for (const auto wave : {Wave::sine, Wave::saw, Wave::pulse, Wave::sampleHold, Wave::smooth, Wave::lorenz})
            {
                if (shapeOf(wave).kind != Kind::periodic && !requires { make().setSeed(std::uint64_t{}); })
                {
                    continue;
                }
                const auto one = renderScript(make, wave, 1);
                blocksSame = blocksSame && sameBits(one, renderScript(make, wave, 37)) &&
                             sameBits(one, renderScript(make, wave, 4096));
            }
            CHECK(blocksSame);

            std::size_t subnormalOut = 0;
            std::size_t subnormalState = 0;
            for (const auto wave : allWaves)
            {
                if (shapeOf(wave).kind != Kind::periodic && !requires { make().setSeed(std::uint64_t{}); })
                {
                    continue;
                }
                auto lfo = preparedModel(make, wave, 0.5f);
                std::array<float, 48000> y{};
                for (int s = 0; s < 60; ++s)
                {
                    for (auto& v : y)
                    {
                        v = lfo.next();
                        subnormalOut += std::fpclassify(v) == FP_SUBNORMAL ? 1U : 0U;
                        subnormalState += lfo.hasSubnormalState() ? 1U : 0U;
                    }
                    lfo.retrigger();
                }
            }

            auto lfo = preparedModel(make, Wave::saw, 3.0f);
            std::array<float, 512> out{};
            const std::array events{LfoEvent{.offset = 3}, LfoEvent{.offset = 300}};
            std::size_t allocations = 0;
            {
                const AllocationCounter counter;
                lfo.setFrequency(5.0f);
                lfo.setGain(0.5f);
                lfo.setOffset(0.25f);
                lfo.retrigger();
                lfo.process(out);
                lfo.process(out, events);
                lfo.setWave(Wave::sine);
                lfo.process(out, events);
                lfo.reset();
                lfo.process(out);
                allocations = counter.count();
            }
            CHECK(allocations == 0);

            const auto lifecycle = checkLifecycleContract(
                [&]
                {
                    auto model = make();
                    model.setWave(Wave::saw);
                    model.setFrequency(150.0f);
                    return AsProcessor{model};
                },
                {.sampleRate = rate, .maxBlockSize = 512, .numChannels = 1}, 3000);
            CHECK(lifecycle.passed());

            std::cout << "| " << name << " | " << (blocksSame ? "bit-identical" : "DIFFER") << " | " << subnormalOut
                      << " / " << subnormalState << " | " << allocations << " | "
                      << (lifecycle.passed() ? "passes" : "FAILS") << " |\n";
        });
}

} // namespace

TEST_CASE("LFO report: candidate models", "[.][lfo-report]")
{
    std::cout << "\n## LFO report (lfo_candidates.hpp)\n";
    reportTable();
    reportPhase(600.0);
    reportDiscontinuities();
    reportSmear();
    reportRetrigger();
    reportEvaluationRate();
    reportRandom();
    reportRateChange();
    reportContracts();
}

TEST_CASE("LFO report: phase accumulators over three hours", "[.][lfo-long-report]")
{
    reportPhase(3.0 * 3600.0);
}
