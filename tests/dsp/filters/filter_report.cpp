// The filter report: measures the candidate models in filter_candidates.hpp
// to choose one (docs/dsp/filters.md). It measures:
//
// - the linear response against the analog prototype, for every response
//   and slope, from 100 Hz to 19 kHz at 44.1 kHz: the gain and phase at the
//   cutoff, the largest error where the prototype is above -30 dB, and where
//   a resonant peak lands (cutoff accuracy near Nyquist);
// - self-oscillation: the pitch error in cents from 55 Hz to 14 kHz at
//   44.1-192 kHz, the threshold (the smallest k that grows), and the level;
// - the passband loss against resonance, quiet and loud;
// - THD and the harmonic profile against drive, per saturator (the Warm
//   Drive pairing);
// - aliasing against drive at 1x, 2x, and 4x;
// - each solver's distance from the converged Newton solution at the same
//   oversampling factor, and the Newton iterations it took;
// - stability at maximum resonance and drive, under an audio-rate cutoff
//   sweep, with DC, and in silence (subnormals, the time to exact zero);
// - bit-identity across block sizes 1, 37, and 4096;
// - clicks and zipper noise under a stepped and a staircase cutoff.
//
// Hidden from the default run; run it in an optimised build (about 30 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[filter-report]"
//
// CPU: the "[filter]" benchmarks (tools/bench.sh "[filter]").

#include "../support/allocation_counter.hpp"
#include "filter_candidates.hpp"

#include <anoesis/test/fft.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/signals.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/resampling/oversampler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
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
#include <utility>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::filters;
namespace dsp = anoesis::dsp;

namespace
{

constexpr double pi = std::numbers::pi;

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

double cents(double ratio)
{
    return 1200.0 * std::log2(ratio);
}

/// A model that passes its input through, so Oversampled<Identity> measures
/// the Oversampler alone.
struct Identity
{
    void prepare(double /*rate*/) noexcept {}
    void reset() noexcept {}
    void setCutoff(double /*hz*/) noexcept {}
    void setResonance(float /*k*/) noexcept {}
    void setDrive(float /*gain*/) noexcept {}
    void setResponse(Response /*r*/) noexcept {}
    [[nodiscard]] static float process(float x) noexcept
    {
        return x;
    }
};

using dsp::OversamplingFactor;

// The candidates, each with the default saturator (tanh) where it has one.
using NewtonTanh = NewtonZdf<TanhSat>;
using NewtonOneStep = NewtonZdf<TanhSat, 1>;

/// The Newton ladder with the cubic saturator, labelled as such.
struct NewtonCubic : NewtonZdf<CubicSat>
{
    static constexpr const char* name = "Newton ZDF (cubic)";
};
using LinearisedTanh = LinearisedZdf<TanhSat>;
using FeedbackTanh = FeedbackOnlyZdf<TanhSat>;
using UnitDelayTanh = UnitDelayLadder<TanhSat>;
using HuovNaive = Huovilainen<TanhSat, HuovilainenTuning::naive>;
using HuovExact = Huovilainen<TanhSat, HuovilainenTuning::exact>;

template <typename Model>
std::string labelOf(OversamplingFactor factor)
{
    switch (factor)
    {
    case OversamplingFactor::two:
        return std::string{Model::name} + ", 2x";
    case OversamplingFactor::four:
        return std::string{Model::name} + ", 4x";
    default:
        return std::string{Model::name};
    }
}

/// Calls visit(label, make) for each candidate, make(rate) returning a
/// prepared model (Model or Oversampled<Model>) with LP24, k 0, drive 1.
template <typename Model, typename Visit>
void visitAt(const Visit& visit, OversamplingFactor factor)
{
    const auto label = labelOf<Model>(factor);
    if (factor == OversamplingFactor::none)
    {
        visit(label,
              [](double rate)
              {
                  Model m{};
                  m.prepare(rate);
                  return m;
              });
    }
    else
    {
        visit(label,
              [factor](double rate)
              {
                  Oversampled<Model> m{factor};
                  m.prepare(rate);
                  return m;
              });
    }
}

/// The main candidate set, used by most sections.
template <typename Visit>
void forEachNonlinear(const Visit& visit)
{
    visitAt<NewtonTanh>(visit, OversamplingFactor::none);
    visitAt<NewtonCubic>(visit, OversamplingFactor::none);
    visitAt<LinearisedTanh>(visit, OversamplingFactor::none);
    visitAt<FeedbackTanh>(visit, OversamplingFactor::none);
    visitAt<UnitDelayTanh>(visit, OversamplingFactor::none);
    visitAt<HuovNaive>(visit, OversamplingFactor::two);
    visitAt<HuovExact>(visit, OversamplingFactor::two);
    visitAt<HuovExact>(visit, OversamplingFactor::four);
    visitAt<NewtonTanh>(visit, OversamplingFactor::two);
}

template <typename Visit>
void forEachCandidate(const Visit& visit)
{
    visitAt<LinearZdf>(visit, OversamplingFactor::none);
    forEachNonlinear(visit);
}

template <typename M>
std::vector<float> run(M& model, std::vector<float> x)
{
    processBlock(model, std::span{x});
    return x;
}

template <typename M>
std::vector<float> runModulated(M& model, std::vector<float> x, std::span<const float> cutoff)
{
    processModulated(model, std::span{x}, cutoff);
    return x;
}

/// A naive sawtooth (the filter's input, so its aliasing doesn't matter
/// here), amplitude 1.
std::vector<float> saw(double hz, double rate, std::size_t n)
{
    std::vector<float> x(n);
    double phase = 0.0;
    for (auto& v : x)
    {
        v = static_cast<float>((2.0 * phase) - 1.0);
        phase += hz / rate;
        phase -= std::floor(phase);
    }
    return x;
}

// --- 1. Linear response ---------------------------------------------------

struct ResponseError
{
    double gainAtCutoffDb = 0.0;
    double phaseAtCutoffDeg = 0.0;
    double worstBelowDb = 0.0; ///< 20 Hz to the cutoff
    double worstAboveDb = 0.0; ///< the cutoff to 20 kHz, where the prototype is within 30 dB of its peak
    [[nodiscard]] double worstDb() const noexcept
    {
        return std::max(worstBelowDb, worstAboveDb);
    }
};

/// The model's response to a 1e-4 impulse, scaled back up, divided by the
/// Oversampler's own response when it is oversampled (its latency and the
/// halfbands' phase are not the filter's).
template <typename Make>
std::vector<Complex> smallSignalSpectrum(const Make& make, double rate, double cutoff, float k, Response response,
                                         std::size_t fftSize, const std::vector<Complex>* divideBy)
{
    auto model = make(rate);
    model.setResponse(response);
    model.setResonance(k);
    model.setCutoff(cutoff);
    model.reset();
    constexpr float level = 1e-4f;
    const MonoProcess process = [&](std::span<const float> in)
    {
        std::vector<float> x(in.begin(), in.end());
        for (auto& v : x)
        {
            v *= level;
        }
        auto y = run(model, std::move(x));
        for (auto& v : y)
        {
            v /= level;
        }
        return y;
    };
    auto spectrum = impulseResponseSpectrum(process, fftSize);
    if (divideBy != nullptr)
    {
        for (std::size_t i = 0; i < spectrum.size(); ++i)
        {
            spectrum[i] /= (*divideBy)[i];
        }
    }
    return spectrum;
}

ResponseError compareWithAnalog(const std::vector<Complex>& spectrum, double rate, double cutoff, float k,
                                Response response, bool svf)
{
    const auto fftSize = (spectrum.size() - 1) * 2;
    const double binHz = rate / static_cast<double>(fftSize);
    const auto analog = [&](double hz)
    {
        return svf ? analogSvf(response, k, hz / cutoff) : analogLadder(response, k, hz / cutoff);
    };
    ResponseError e;
    const auto bin = static_cast<std::size_t>(std::lround(cutoff / binHz));
    const double binFrequency = static_cast<double>(bin) * binHz;
    e.gainAtCutoffDb = dbOf(std::abs(spectrum[bin])) - dbOf(std::abs(analog(binFrequency)));
    e.phaseAtCutoffDeg = std::arg(spectrum[bin] / analog(binFrequency)) * 180.0 / pi;
    for (std::size_t i = 1; i < spectrum.size(); ++i)
    {
        const double hz = static_cast<double>(i) * binHz;
        if (hz < 20.0 || hz > 20000.0)
        {
            continue;
        }
        const double reference = dbOf(std::abs(analog(hz)));
        const double peak = dbOf(std::abs(analog(cutoff)));
        if (reference < std::max(peak, 0.0) - 30.0)
        {
            continue;
        }
        const double error = std::fabs(dbOf(std::abs(spectrum[i])) - reference);
        (hz <= cutoff ? e.worstBelowDb : e.worstAboveDb) =
            std::max(hz <= cutoff ? e.worstBelowDb : e.worstAboveDb, error);
    }
    return e;
}

/// The frequency of the largest magnitude between lo and hi, parabolically
/// interpolated on the dB values.
double peakFrequency(const std::function<double(double)>& magnitudeDb, double lo, double hi, double step)
{
    double best = lo;
    double bestDb = -1e300;
    const auto count = static_cast<std::size_t>((hi - lo) / step);
    for (std::size_t i = 0; i <= count; ++i)
    {
        const double f = lo + (static_cast<double>(i) * step);
        const double v = magnitudeDb(f);
        if (v > bestDb)
        {
            bestDb = v;
            best = f;
        }
    }
    const double a = magnitudeDb(best - step);
    const double c = magnitudeDb(best + step);
    const double denominator = a - (2.0 * bestDb) + c;
    return std::fabs(denominator) < 1e-30 ? best : best + (0.5 * step * (a - c) / denominator);
}

void reportLinearResponse()
{
    constexpr double rate = 44100.0;
    constexpr std::size_t fftSize = 65536;
    const std::array<double, 6> cutoffs{100.0, 1000.0, 5000.0, 10000.0, 15000.0, 19000.0};

    Oversampled<Identity> os2{OversamplingFactor::two};
    os2.prepare(rate);
    Oversampled<Identity> os4{OversamplingFactor::four};
    os4.prepare(rate);
    const auto identitySpectrum = [&](Oversampled<Identity>& os)
    {
        os.reset();
        return impulseResponseSpectrum([&](std::span<const float> in)
                                       { return run(os, std::vector<float>(in.begin(), in.end())); }, fftSize);
    };
    const auto id2 = identitySpectrum(os2);
    const auto id4 = identitySpectrum(os4);
    const auto divisorFor = [&](const std::string& label) -> const std::vector<Complex>*
    {
        if (label.ends_with(", 2x"))
        {
            return &id2;
        }
        if (label.ends_with(", 4x"))
        {
            return &id4;
        }
        return nullptr;
    };

    std::cout << "\n### Linear response against the analog prototype (LP24, 44.1 kHz, -80 dBFS)\n\n"
                 "Gain error at the cutoff (dB) / phase error at the cutoff (degrees) / the largest magnitude error "
                 "(dB) from 20 Hz to the cutoff / the largest from the cutoff to 20 kHz where the prototype is within "
                 "30 dB of its peak. Oversampled models are divided by the Oversampler's own response.\n\n"
                 "| Model | k | 100 Hz | 1 kHz | 5 kHz | 10 kHz | 15 kHz | 19 kHz |\n"
                 "|---|---|---|---|---|---|---|---|\n";
    const auto rowFor = [&](const std::string& label, const auto& make, bool svf)
    {
        for (const float k : {0.0f, 3.0f})
        {
            std::cout << "| " << label << " | " << fixed(k, 0);
            for (const double fc : cutoffs)
            {
                const auto response = svf ? Response::lp12 : Response::lp24;
                const auto spectrum = smallSignalSpectrum(make, rate, fc, k, response, fftSize, divisorFor(label));
                const auto e = compareWithAnalog(spectrum, rate, fc, k, response, svf);
                std::cout << " | " << fixed(e.gainAtCutoffDb, 2) << " / " << fixed(e.phaseAtCutoffDeg, 1) << " / "
                          << fixed(e.worstBelowDb, 2) << " / " << fixed(e.worstAboveDb, 1);
            }
            std::cout << " |\n";
        }
    };
    forEachCandidate([&](const std::string& label, const auto& make) { rowFor(label, make, false); });
    visitAt<HuovNaive>([&](const std::string& label, const auto& make) { rowFor(label, make, false); },
                       OversamplingFactor::four);
    visitAt<LinearSvf>([&](const std::string& label, const auto& make) { rowFor(label + " (LP12)", make, true); },
                       OversamplingFactor::none);

    std::cout
        << "\n**Every response from the taps** (Linear ZDF, Newton ZDF, Huovilainen exact 2x; 44.1 kHz; the "
           "largest magnitude error in dB from 20 Hz to 20 kHz where the prototype is within 30 dB of its peak, at "
           "1 kHz and 10 kHz)\n\n"
           "| Response | k | Linear ZDF 1k / 10k | Newton ZDF 1k / 10k | Huovilainen exact 2x 1k / 10k |\n"
           "|---|---|---|---|---|\n";
    for (const auto response : allResponses)
    {
        for (const float k : {0.0f, 3.0f})
        {
            std::cout << "| " << responseName(response) << " | " << fixed(k, 0);
            const auto cell = [&](const std::string& label, const auto& make)
            {
                std::cout << " | ";
                for (const double fc : {1000.0, 10000.0})
                {
                    const auto spectrum = smallSignalSpectrum(make, rate, fc, k, response, fftSize, divisorFor(label));
                    std::cout << fixed(compareWithAnalog(spectrum, rate, fc, k, response, false).worstDb(), 2)
                              << (fc < 5000.0 ? " / " : "");
                }
            };
            visitAt<LinearZdf>(cell, OversamplingFactor::none);
            visitAt<NewtonTanh>(cell, OversamplingFactor::none);
            visitAt<HuovExact>(cell, OversamplingFactor::two);
            std::cout << " |\n";
        }
    }

    std::cout << "\n**Where the resonant peak lands** (LP24, k = 3.5, 44.1 kHz): the digital peak against the "
                 "prototype's, in cents\n\n"
                 "| Model | 1 kHz | 5 kHz | 10 kHz | 15 kHz | 18 kHz | 20 kHz |\n|---|---|---|---|---|---|---|\n";
    const auto peakRow = [&](const std::string& label, const auto& make)
    {
        std::cout << "| " << label;
        for (const double fc : {1000.0, 5000.0, 10000.0, 15000.0, 18000.0, 20000.0})
        {
            const auto spectrum = smallSignalSpectrum(make, rate, fc, 3.5f, Response::lp24, fftSize, divisorFor(label));
            const double binHz = rate / static_cast<double>(fftSize);
            const auto digitalDb = [&](double hz)
            {
                const auto i =
                    static_cast<std::size_t>(std::clamp(hz / binHz, 1.0, (static_cast<double>(fftSize) / 2.0) - 1.0));
                const double t = (hz / binHz) - static_cast<double>(i);
                return dbOf(((1.0 - t) * std::abs(spectrum[i])) + (t * std::abs(spectrum[i + 1])));
            };
            const auto analogDb = [&](double hz)
            {
                return dbOf(std::abs(analogLadder(Response::lp24, 3.5, hz / fc)));
            };
            const double hi = std::min(1.6 * fc, 0.4999 * rate);
            const double digital = peakFrequency(digitalDb, 0.5 * fc, hi, binHz);
            const double analog = peakFrequency(analogDb, 0.5 * fc, 1.6 * fc, binHz);
            std::cout << " | " << fixed(cents(digital / analog), 1);
        }
        std::cout << " |\n";
    };
    forEachCandidate(peakRow);
}

// --- 2. Self-oscillation --------------------------------------------------

struct Oscillation
{
    double centsError = std::numeric_limits<double>::quiet_NaN();
    double levelDb = -std::numeric_limits<double>::infinity();
};

template <typename Make>
Oscillation selfOscillate(const Make& make, double rate, double cutoff, float k, double seconds = 2.5)
{
    auto model = make(rate);
    model.setResonance(k);
    model.setCutoff(cutoff);
    const auto n = static_cast<std::size_t>(seconds * rate);
    std::vector<float> x(n, 0.0f);
    x[0] = 0.5f;
    const auto y = run(model, std::move(x));
    const auto tail = std::span{y}.subspan(n - static_cast<std::size_t>(0.5 * rate));
    double sumSquares = 0.0;
    for (const float v : tail)
    {
        sumSquares += static_cast<double>(v) * static_cast<double>(v);
    }
    const double rms = std::sqrt(sumSquares / static_cast<double>(tail.size()));
    Oscillation o;
    o.levelDb = dbOf(rms * std::numbers::sqrt2);
    if (rms > 1e-6 && std::isfinite(rms))
    {
        o.centsError = cents(measureFrequency(tail, rate) / cutoff);
    }
    return o;
}

/// The smallest k at which a 1e-15 impulse grows: bisection on the RMS of
/// cycles 10-15 against cycles 35-40 (linear regime throughout).
template <typename Make>
double threshold(const Make& make, double rate, double cutoff)
{
    const auto grows = [&](double k)
    {
        auto model = make(rate);
        model.setResonance(static_cast<float>(k));
        model.setCutoff(cutoff);
        const double period = rate / cutoff;
        const auto n = static_cast<std::size_t>(40.0 * period) + 1;
        std::vector<float> x(n, 0.0f);
        x[0] = 1e-15f;
        const auto y = run(model, std::move(x));
        const auto rms = [&](double fromCycle, double toCycle)
        {
            const auto a = static_cast<std::size_t>(fromCycle * period);
            const auto b = static_cast<std::size_t>(toCycle * period);
            double sum = 0.0;
            for (std::size_t i = a; i < b; ++i)
            {
                sum += static_cast<double>(y[i]) * static_cast<double>(y[i]);
            }
            return sum / static_cast<double>(b - a);
        };
        return rms(35.0, 40.0) > rms(10.0, 15.0);
    };
    double lo = 2.0;
    double hi = 8.0;
    if (!grows(hi))
    {
        return std::numeric_limits<double>::infinity();
    }
    for (int i = 0; i < 18; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (grows(mid) ? hi : lo) = mid;
    }
    return 0.5 * (lo + hi);
}

constexpr std::array<double, 9> oscillationCutoffs{55.0, 110.0, 220.0, 440.0, 880.0, 1760.0, 3520.0, 7040.0, 14080.0};

void reportSelfOscillation(const std::vector<double>& rates)
{
    for (const double rate : rates)
    {
        std::cout << "\n### Self-oscillation pitch at k = 4.4, " << fixed(rate / 1000.0, 1)
                  << " kHz (cents from the cutoff; - means no oscillation)\n\n| Model";
        for (const double fc : oscillationCutoffs)
        {
            if (fc < 0.45 * rate)
            {
                std::cout << " | " << fc;
            }
        }
        std::cout << " |\n|---";
        for (const double fc : oscillationCutoffs)
        {
            if (fc < 0.45 * rate)
            {
                std::cout << "|---";
            }
        }
        std::cout << "|\n";
        forEachNonlinear(
            [&](const std::string& label, const auto& make)
            {
                std::cout << "| " << label;
                for (const double fc : oscillationCutoffs)
                {
                    if (fc >= 0.45 * rate)
                    {
                        continue;
                    }
                    const auto o = selfOscillate(make, rate, fc, 4.4f);
                    std::cout << " | " << (std::isnan(o.centsError) ? std::string{"-"} : fixed(o.centsError, 2));
                }
                std::cout << " |\n";
            });
    }
}

void reportOscillationLevel()
{
    constexpr double rate = 48000.0;
    const std::array<float, 7> ks{4.05f, 4.1f, 4.2f, 4.4f, 5.0f, 6.0f, 8.0f};
    for (const double fc : {440.0, 7040.0})
    {
        std::cout << "\n### Self-oscillation level and pitch against k (48 kHz, cutoff " << fc
                  << " Hz, no input after one impulse)\n\n"
                     "Peak-equivalent level (dBFS, from the RMS of the last 0.5 s) / pitch error (cents).\n\n"
                     "| Model | k 4.05 | 4.1 | 4.2 | 4.4 | 5 | 6 | 8 |\n|---|---|---|---|---|---|---|---|\n";
        forEachNonlinear(
            [&](const std::string& label, const auto& make)
            {
                std::cout << "| " << label;
                for (const float k : ks)
                {
                    const auto o = selfOscillate(make, rate, fc, k, 4.0);
                    std::cout << " | " << fixed(o.levelDb, 1) << " / "
                              << (std::isnan(o.centsError) ? std::string{"-"} : fixed(o.centsError, 1));
                }
                std::cout << " |\n";
            });
    }

    std::cout << "\n**Silence stays silent**: with no input, every model's output stays exactly 0 at k = 8 (a "
                 "self-oscillating filter needs something to start it): ";
    bool allSilent = true;
    forEachNonlinear(
        [&](const std::string& /*label*/, const auto& make)
        {
            auto model = make(rate);
            model.setResonance(8.0f);
            const auto y = run(model, std::vector<float>(48000, 0.0f));
            allSilent = allSilent && std::ranges::all_of(y, [](float v) { return v == 0.0f; });
        });
    std::cout << (allSilent ? "yes" : "NO") << ".\n";
    CHECK(allSilent);
}

void reportThreshold(const std::vector<double>& rates)
{
    std::cout << "\n### Self-oscillation threshold (the smallest k that grows from a 1e-15 impulse)\n\n| Model | Rate";
    const std::array<double, 5> cutoffs{110.0, 880.0, 3520.0, 7040.0, 14080.0};
    for (const double fc : cutoffs)
    {
        std::cout << " | " << fc;
    }
    std::cout << " |\n|---|---|---|---|---|---|---|\n";
    forEachNonlinear(
        [&](const std::string& label, const auto& make)
        {
            for (const double rate : rates)
            {
                std::cout << "| " << label << " | " << fixed(rate / 1000.0, 1);
                for (const double fc : cutoffs)
                {
                    std::cout << " | " << fixed(threshold(make, rate, fc), 3);
                }
                std::cout << " |\n";
            }
        });
}

// --- 3. Passband loss ---------------------------------------------------

void reportPassbandLoss()
{
    constexpr double rate = 48000.0;
    const std::array<float, 8> ks{0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};
    std::cout << "\n### Passband gain against resonance (48 kHz, cutoff 2 kHz, a 100 Hz probe; dB)\n\n"
                 "The component at the probe's frequency (measureResponseAt), so it reads the passband even while the "
                 "filter self-oscillates at 2 kHz. Theory for the linear ladder: LP24 and LP12 1/(1 + k) at DC.\n\n"
                 "| Model | Response | Probe | k 0 | 1 | 2 | 3 | 4 | 5 | 6 | 8 |\n"
                 "|---|---|---|---|---|---|---|---|---|---|---|\n";
    const auto rows = [&](const std::string& label, const auto& make)
    {
        for (const auto response : {Response::lp24, Response::lp12})
        {
            for (const double amplitude : {0.01, 0.5})
            {
                std::cout << "| " << label << " | " << responseName(response) << " | "
                          << (amplitude < 0.1 ? "-40 dBFS" : "-6 dBFS");
                for (const float k : ks)
                {
                    const MonoProcess process = [&](std::span<const float> in)
                    {
                        auto model = make(rate);
                        model.setResponse(response);
                        model.setResonance(k);
                        model.setCutoff(2000.0);
                        return run(model, std::vector<float>(in.begin(), in.end()));
                    };
                    const auto point = measureResponseAt(process, 100.0, rate, amplitude);
                    std::cout << " | " << fixed(point.magnitudeDb, 1);
                }
                std::cout << " |\n";
            }
        }
    };
    visitAt<LinearZdf>(rows, OversamplingFactor::none);
    forEachNonlinear(rows);
}

// --- 4. THD and the harmonic profile ------------------------------------

template <typename Make>
void harmonicRow(const std::string& label, const Make& make)
{
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 65536;
    const double f0 = coherentFrequency(220.0, rate, n);
    for (const float drive : {1.0f, 2.0f, 4.0f, 8.0f, 16.0f})
    {
        auto model = make(rate);
        model.setCutoff(10000.0);
        model.setDrive(drive);
        const auto settle = static_cast<std::size_t>(8192);
        auto y = run(model, sine(f0, rate, settle + n, 0.5));
        const std::span tail{y.data() + settle, n};
        const auto window = rectangularWindow(n);
        const auto h = measureHarmonics(tail, f0, rate, window, 20000.0);
        const double thd = measureThd(tail, f0, rate, window, 10);
        std::cout << "| " << label << " | " << fixed(drive, 0) << " | " << fixed(dbOf(h[0]), 1) << " | "
                  << fixed(100.0 * thd, 2);
        for (const std::size_t k : {2U, 3U, 5U, 7U, 9U})
        {
            std::cout << " | " << fixed(dbOf(h[k - 1] / h[0]), 1);
        }
        std::cout << " |\n";
    }
}

void reportHarmonics()
{
    std::cout
        << "\n### THD and harmonics against drive (LP24, cutoff 10 kHz, k 0, a 220 Hz sine at -6 dBFS, 48 kHz)\n\n"
           "Drive is a gain on the input, with no make-up. Harmonics in dB re the fundamental at the output.\n\n"
           "| Model | Drive | Fundamental (dBFS) | THD (%) | H2 | H3 | H5 | H7 | H9 |\n"
           "|---|---|---|---|---|---|---|---|---|\n";
    const auto rows = [](const std::string& label, const auto& make)
    {
        harmonicRow(label, make);
    };
    visitAt<NewtonTanh>(rows, OversamplingFactor::none);
    visitAt<NewtonZdf<CubicSat>>([&](const std::string& l, const auto& m) { harmonicRow(l + " (cubic)", m); },
                                 OversamplingFactor::none);
    visitAt<NewtonZdf<AlgebraicSat>>([&](const std::string& l, const auto& m) { harmonicRow(l + " (algebraic)", m); },
                                     OversamplingFactor::none);
    visitAt<NewtonZdf<AsymmetricSat>>([&](const std::string& l, const auto& m)
                                      { harmonicRow(l + " (asymmetric tanh)", m); }, OversamplingFactor::none);
    visitAt<LinearisedTanh>(rows, OversamplingFactor::none);
    visitAt<LinearisedZdf<CubicSat>>([&](const std::string& l, const auto& m) { harmonicRow(l + " (cubic)", m); },
                                     OversamplingFactor::none);
    visitAt<FeedbackTanh>(rows, OversamplingFactor::none);
    visitAt<HuovExact>(rows, OversamplingFactor::two);
}

// --- 5. Aliasing --------------------------------------------------------

template <typename Make>
void aliasingRow(const std::string& label, const Make& make, float k, double cutoff)
{
    constexpr double rate = 44100.0;
    constexpr std::size_t n = 65536;
    const double f0 = coherentFrequency(2500.0, rate, n);
    std::cout << "| " << label;
    for (const float drive : {1.0f, 2.0f, 4.0f, 16.0f})
    {
        auto model = make(rate);
        model.setCutoff(cutoff);
        model.setResonance(k);
        model.setDrive(drive);
        constexpr std::size_t settle = 16384;
        auto y = run(model, sine(f0, rate, settle + n, 0.5));
        const std::span tail{y.data() + settle, n};
        const auto a = measureAliasing(tail, f0, rate, rectangularWindow(n), 20000.0);
        std::cout << " | " << fixed(a.worstAliasDb, 1) << " / " << fixed(a.aliasToSignalDb, 1);
    }
    std::cout << " |\n";
}

void reportAliasing()
{
    for (const auto& [k, cutoff, title] :
         {std::tuple{0.0f, 15000.0, "LP24, cutoff 15 kHz, k 0"}, std::tuple{3.0f, 5000.0, "LP24, cutoff 5 kHz, k 3"}})
    {
        std::cout << "\n### Aliasing against drive and oversampling (" << title
                  << ", a 2.5 kHz sine at -6 dBFS, 44.1 kHz)\n\n"
                     "The strongest single alias below 20 kHz, dB re the fundamental / all aliasing below 20 kHz, dB "
                     "re the harmonics.\n\n"
                     "| Model | Drive 1 | Drive 2 | Drive 4 | Drive 16 |\n|---|---|---|---|---|\n";
        const auto row = [&, kk = k, fc = cutoff](const std::string& label, const auto& make)
        {
            aliasingRow(label, make, kk, fc);
        };
        for (const auto factor : {OversamplingFactor::none, OversamplingFactor::two, OversamplingFactor::four})
        {
            visitAt<NewtonTanh>(row, factor);
            visitAt<LinearisedTanh>(row, factor);
            visitAt<FeedbackTanh>(row, factor);
            visitAt<NewtonZdf<CubicSat>>([&, kk = k, fc = cutoff](const std::string& l, const auto& m)
                                         { aliasingRow(l + " (cubic)", m, kk, fc); }, factor);
        }
        visitAt<UnitDelayTanh>(row, OversamplingFactor::none);
        visitAt<HuovExact>(row, OversamplingFactor::two);
        visitAt<HuovExact>(row, OversamplingFactor::four);
    }
}

// --- 6. Distance from the converged solution ----------------------------

/// A driven, resonant sweep: a 110 Hz saw at -6 dBFS, drive 4, k 3.8, the
/// cutoff from 100 Hz to 8 kHz and back over 2 s, at 44.1 kHz.
struct Sweep
{
    std::vector<float> input;
    std::vector<float> cutoff;
};

Sweep sweepScenario(double rate, double seconds = 2.0)
{
    const auto n = static_cast<std::size_t>(seconds * rate);
    Sweep s{.input = saw(110.0, rate, n), .cutoff = std::vector<float>(n)};
    for (std::size_t i = 0; i < n; ++i)
    {
        s.input[i] *= 0.5f;
        const double t = static_cast<double>(i) / static_cast<double>(n);
        const double octaves = 6.32 * (1.0 - std::fabs((2.0 * t) - 1.0));
        s.cutoff[i] = static_cast<float>(100.0 * std::exp2(octaves));
    }
    return s;
}

template <typename Make>
std::vector<float> renderSweep(const Make& make, double rate, const Sweep& s, float k = 3.8f, float drive = 4.0f)
{
    auto model = make(rate);
    model.setResonance(k);
    model.setDrive(drive);
    return runModulated(model, s.input, s.cutoff);
}

template <typename Model>
SolverStats statsOf(Model& model)
{
    if constexpr (requires { model.stats; })
    {
        return model.stats;
    }
    else if constexpr (requires { model.model().stats; })
    {
        return model.model().stats;
    }
    else
    {
        return {};
    }
}

void reportSolverDistance()
{
    constexpr double rate = 44100.0;
    const auto scenario = sweepScenario(rate);
    std::cout << "\n### Distance from the converged Newton solution (a 110 Hz saw at -6 dBFS, the cutoff swept "
                 "100 Hz - 8 kHz - 100 Hz over 2 s, 44.1 kHz)\n\n"
                 "The error's power against the reference's, in dB, at the same oversampling factor (so aliasing and "
                 "the Oversampler's delay are common to both), below 20 kHz after a 4096-sample Hann-windowed Welch "
                 "average. Hard: k 3.8, drive 4. Gentle: k 2, drive 2.\n\n"
                 "| Factor | Model | Hard (dB re signal) | Gentle |\n|---|---|---|---|\n";
    for (const auto factor : {OversamplingFactor::none, OversamplingFactor::two, OversamplingFactor::four})
    {
        std::vector<float> hard;
        std::vector<float> gentle;
        visitAt<NewtonTanh>(
            [&](const std::string& /*l*/, const auto& make)
            {
                hard = renderSweep(make, rate, scenario);
                gentle = renderSweep(make, rate, scenario, 2.0f, 2.0f);
            },
            factor);
        const auto distance = [&](const std::string& label, const auto& make)
        {
            const auto window = hannWindow(4096);
            const auto band = [&](const std::vector<float>& x)
            {
                const auto spectrum = averagedPowerSpectrum(x, rate, window);
                double sum = 0.0;
                for (std::size_t i = 0; i < spectrum.power.size(); ++i)
                {
                    sum += spectrum.frequencyOf(i) <= 20000.0 ? spectrum.power[i] : 0.0;
                }
                return sum;
            };
            const auto errorDb = [&](const std::vector<float>& y, const std::vector<float>& reference)
            {
                std::vector<float> error(y.size());
                for (std::size_t i = 0; i < y.size(); ++i)
                {
                    error[i] = y[i] - reference[i];
                }
                return 10.0 * std::log10((band(error) / band(reference)) + 1e-300);
            };
            std::cout << "| " << static_cast<int>(factor) << "x | " << label << " | "
                      << fixed(errorDb(renderSweep(make, rate, scenario), hard), 1) << " | "
                      << fixed(errorDb(renderSweep(make, rate, scenario, 2.0f, 2.0f), gentle), 1) << " |\n";
        };
        visitAt<NewtonOneStep>(distance, factor);
        visitAt<LinearisedTanh>(distance, factor);
        visitAt<FeedbackTanh>(distance, factor);
        visitAt<UnitDelayTanh>(distance, factor);
        if (factor != OversamplingFactor::none)
        {
            visitAt<HuovExact>(distance, factor);
            visitAt<HuovNaive>(distance, factor);
        }
    }

    std::cout << "\n**Newton iterations** (tolerance 1e-5, at most 8; the first guess is the linearised solve)\n\n"
                 "| Scenario | Mean | Most | Unconverged samples |\n|---|---|---|---|\n";
    const auto statsRow = [&](const char* name, float k, float drive, bool sweep)
    {
        NewtonTanh model;
        model.prepare(rate);
        model.setResonance(k);
        model.setDrive(drive);
        model.setCutoff(1000.0);
        if (sweep)
        {
            static_cast<void>(runModulated(model, scenario.input, scenario.cutoff));
        }
        else
        {
            static_cast<void>(run(model, scenario.input));
        }
        const auto s = statsOf(model);
        std::cout << "| " << name << " | "
                  << fixed(static_cast<double>(s.iterations) / static_cast<double>(s.samples), 2) << " | "
                  << s.mostIterations << " | " << s.unconverged << " |\n";
    };
    statsRow("Saw -6 dBFS, cutoff 1 kHz, k 0, drive 1", 0.0f, 1.0f, false);
    statsRow("The same, k 3.8, drive 4", 3.8f, 4.0f, false);
    statsRow("The sweep above (k 3.8, drive 4)", 3.8f, 4.0f, true);
    statsRow("The sweep, k 8, drive 32", 8.0f, 32.0f, true);
}

// --- 7. Stability, block sizes, allocation -------------------------------

bool isSubnormal(float v)
{
    return std::fpclassify(v) == FP_SUBNORMAL;
}

struct Stress
{
    bool finite = true;
    double peak = 0.0;
    std::size_t subnormals = 0;
};

Stress stressOf(std::span<const float> y)
{
    Stress s;
    for (const float v : y)
    {
        s.finite = s.finite && std::isfinite(v);
        s.peak = std::max(s.peak, static_cast<double>(std::fabs(v)));
        s.subnormals += isSubnormal(v) ? 1U : 0U;
    }
    return s;
}

/// The audio-rate sweep: the cutoff 20 Hz - 20 kHz (log) at 500 Hz, per
/// sample, k 3.9, drive 4, on a 110 Hz saw at full scale.
Sweep audioRateSweep(double rate, std::size_t n)
{
    Sweep s{.input = saw(110.0, rate, n), .cutoff = std::vector<float>(n)};
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / rate;
        s.cutoff[i] = static_cast<float>(632.5 * std::exp2(4.98 * std::sin(2.0 * pi * 500.0 * t)));
    }
    return s;
}

void reportStability()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 96000;
    const auto sweep = audioRateSweep(rate, n);
    std::cout << "\n### Stability (48 kHz, 2 s each, no FTZ)\n\n"
                 "Peak |output| (or NaN/inf). A: k 8, drive 32, a full-scale 110 Hz saw, the cutoff swept by A's "
                 "audio-rate sweep. B: the audio-rate sweep (20 Hz - 20 kHz at 500 Hz) at k 3.9, drive 4. C: DC "
                 "steps 0 -> 1 -> 10 -> -10, k 4.5, cutoff 1 kHz. D: an impulse, then 20 s of silence at k 3 "
                 "(cutoff 100 Hz): subnormal outputs, and when the output reaches exactly 0.\n\n"
                 "| Model | A | B | C | D: subnormals | D: exact 0 after |\n|---|---|---|---|---|---|\n";
    forEachCandidate(
        [&](const std::string& label, const auto& make)
        {
            auto a = make(rate);
            a.setResonance(8.0f);
            a.setDrive(32.0f);
            const auto ya = stressOf(runModulated(a, sweep.input, sweep.cutoff));
            auto b = make(rate);
            b.setResonance(3.9f);
            b.setDrive(4.0f);
            const auto yb = stressOf(runModulated(b, sweep.input, sweep.cutoff));
            auto c = make(rate);
            c.setResonance(4.5f);
            c.setCutoff(1000.0);
            std::vector<float> dc(n, 0.0f);
            std::fill(dc.begin() + (n / 4), dc.begin() + (n / 2), 1.0f);
            std::fill(dc.begin() + (n / 2), dc.begin() + (3 * n / 4), 10.0f);
            std::fill(dc.begin() + (3 * n / 4), dc.end(), -10.0f);
            const auto yc = stressOf(run(c, dc));
            auto d = make(rate);
            d.setResonance(3.0f);
            d.setCutoff(100.0);
            std::vector<float> silence(static_cast<std::size_t>(20.0 * rate), 0.0f);
            silence[0] = 1.0f;
            const auto yd = run(d, silence);
            const auto sd = stressOf(yd);
            std::size_t lastNonZero = 0;
            for (std::size_t i = 0; i < yd.size(); ++i)
            {
                lastNonZero = yd[i] != 0.0f ? i : lastNonZero;
            }
            const auto cell = [](const Stress& s)
            {
                return s.finite ? fixed(s.peak, 2) : std::string{"**NaN/inf**"};
            };
            std::cout << "| " << label << " | " << cell(ya) << " | " << cell(yb) << " | " << cell(yc) << " | "
                      << sd.subnormals << " | "
                      << (lastNonZero + 1 >= yd.size() ? std::string{"> 20 s"}
                                                       : fixed(static_cast<double>(lastNonZero + 1) / rate, 2) + " s")
                      << " |\n";
        });
}

void reportBlockSizes()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 48000;
    const auto sweep = audioRateSweep(rate, n);
    std::cout << "\n### Block sizes and allocation (the audio-rate sweep, k 3.9, drive 4, 48 kHz)\n\n"
                 "Blocks of 1 and 37 against 4096, bit for bit; heap allocations while processing.\n\n"
                 "| Model | 1 | 37 | Allocations |\n|---|---|---|---|\n";
    forEachCandidate(
        [&](const std::string& label, const auto& make)
        {
            const auto render = [&](std::size_t blockSize, std::size_t* allocations)
            {
                auto model = make(rate);
                model.setResonance(3.9f);
                model.setDrive(4.0f);
                auto y = sweep.input;
                const AllocationCounter counter;
                for (std::size_t start = 0; start < n; start += blockSize)
                {
                    const auto length = std::min(blockSize, n - start);
                    processModulated(model, std::span{y}.subspan(start, length),
                                     std::span<const float>{sweep.cutoff}.subspan(start, length));
                }
                if (allocations != nullptr)
                {
                    *allocations = counter.count();
                }
                return y;
            };
            std::size_t allocations = 0;
            const auto reference = render(4096, &allocations);
            const auto same = [&](std::size_t blockSize)
            {
                const auto y = render(blockSize, nullptr);
                return std::ranges::equal(
                    y, reference, [](float p, float q)
                    { return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q); });
            };
            const bool one = same(1);
            const bool odd = same(37);
            CHECK(one);
            CHECK(odd);
            CHECK(allocations == 0);
            std::cout << "| " << label << " | " << (one ? "identical" : "DIFFER") << " | "
                      << (odd ? "identical" : "DIFFER") << " | " << allocations << " |\n";
        });
}

// --- 8. Clicks and zipper noise -------------------------------------------

/// Energy above 1 kHz in a 4096-sample Hann window centred on `at`.
double energyAbove(std::span<const float> x, std::size_t at, double rate)
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

void reportZipper()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 48000;
    constexpr std::size_t at = 24000;
    std::vector<float> step(n, 0.0f);
    std::fill(step.begin() + at, step.end(), 1.0f);
    const double unitStep = energyAbove(step, at, rate);
    const auto input = sine(150.0, rate, n, 0.25);

    std::cout << "\n### A stepped cutoff (48 kHz, LP24, k 3.6, a 150 Hz sine at -12 dBFS; the cutoff 500 Hz -> 4 kHz "
                 "at 0.5 s)\n\n"
                 "Energy above 1 kHz in a Hann 4096 centred on the step, dB re an instant unit step: the resonant "
                 "ring the step excites. Smoothed: the cutoff's log2 ramped linearly over 10 ms "
                 "(dsp::LinearSmoother). Steady: the same window with the cutoff already at 4 kHz.\n\n"
                 "| Model | Stepped | Smoothed 10 ms | Steady |\n|---|---|---|---|\n";
    forEachCandidate(
        [&](const std::string& label, const auto& make)
        {
            const auto render = [&](int how)
            {
                std::vector<float> cutoff(n, how == 2 ? 4000.0f : 500.0f);
                if (how == 0)
                {
                    std::fill(cutoff.begin() + at, cutoff.end(), 4000.0f);
                }
                else if (how == 1)
                {
                    dsp::LinearSmoother smoother;
                    smoother.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = 1, .numChannels = 1});
                    smoother.setRampTime(0.01f);
                    smoother.setCurrentAndTarget(std::log2(500.0f));
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        if (i == at)
                        {
                            smoother.setTarget(std::log2(4000.0f));
                        }
                        cutoff[i] = std::exp2(smoother.next());
                    }
                }
                auto model = make(rate);
                model.setResonance(3.6f);
                return runModulated(model, input, cutoff);
            };
            std::cout << "| " << label;
            for (const int how : {0, 1, 2})
            {
                std::cout << " | " << fixed(10.0 * std::log10(energyAbove(render(how), at, rate) / unitStep), 1);
            }
            std::cout << " |\n";
        });

    std::cout << "\n### Zipper noise from a staircase cutoff (44.1 kHz, the driven sweep of the solver section: a "
                 "110 Hz saw, k 3.8, drive 4, 100 Hz - 8 kHz - 100 Hz over 2 s)\n\n"
                 "The error's power against the per-sample sweep's, dB: the cutoff held for 32 or 64 samples; "
                 "interpolated linearly in log2 between points every 32 samples (so a tan per sample remains); or "
                 "the integrator gain g = tan(pi fc / fs) interpolated linearly between points every 32 samples "
                 "(no tan between them; ZDF models at 1x only).\n\n"
                 "| Model | Held 32 | Held 64 | log2 interpolated 32 | g interpolated 32 |\n|---|---|---|---|---|\n";
    constexpr double sweepRate = 44100.0;
    const auto scenario = sweepScenario(sweepRate);
    enum class Path : std::uint8_t
    {
        held,
        logLinear,
        gainLinear,
    };
    const auto staircase = [&](std::size_t every, Path path)
    {
        auto s = scenario;
        const auto gainOf = [&](float hz)
        {
            return std::tan(pi * static_cast<double>(hz) / sweepRate);
        };
        for (std::size_t i = 0; i < s.cutoff.size(); ++i)
        {
            const std::size_t a = (i / every) * every;
            const std::size_t b = std::min(a + every, s.cutoff.size() - 1);
            const double t = static_cast<double>(i - a) / static_cast<double>(every);
            if (path == Path::logLinear)
            {
                const double la = std::log2(static_cast<double>(scenario.cutoff[a]));
                const double lb = std::log2(static_cast<double>(scenario.cutoff[b]));
                s.cutoff[i] = static_cast<float>(std::exp2(la + (t * (lb - la))));
            }
            else if (path == Path::gainLinear)
            {
                const double ga = gainOf(scenario.cutoff[a]);
                const double gb = gainOf(scenario.cutoff[b]);
                s.cutoff[i] = static_cast<float>(std::atan(ga + (t * (gb - ga))) * sweepRate / pi);
            }
            else
            {
                s.cutoff[i] = scenario.cutoff[a];
            }
        }
        return s;
    };
    const std::array<Sweep, 4> variants{
        staircase(32, Path::held),
        staircase(64, Path::held),
        staircase(32, Path::logLinear),
        staircase(32, Path::gainLinear),
    };
    const auto row = [&](const std::string& label, const auto& make)
    {
        const auto reference = renderSweep(make, sweepRate, scenario);
        double referencePower = 0.0;
        for (const float v : reference)
        {
            referencePower += static_cast<double>(v) * static_cast<double>(v);
        }
        std::cout << "| " << label;
        for (const auto& v : variants)
        {
            const auto y = renderSweep(make, sweepRate, v);
            double errorPower = 0.0;
            for (std::size_t i = 0; i < y.size(); ++i)
            {
                const double e = static_cast<double>(y[i]) - static_cast<double>(reference[i]);
                errorPower += e * e;
            }
            std::cout << " | " << fixed(10.0 * std::log10(errorPower / referencePower), 1);
        }
        std::cout << " |\n";
    };
    visitAt<LinearZdf>(row, OversamplingFactor::none);
    visitAt<NewtonTanh>(row, OversamplingFactor::none);
    visitAt<LinearisedTanh>(row, OversamplingFactor::none);
    visitAt<HuovExact>(row, OversamplingFactor::two);
}

} // namespace

TEST_CASE("Filter report: candidate models", "[.][filter-report]")
{
    std::cout << "\n## Filter report (filter_candidates.hpp)\n";
    reportLinearResponse();
    reportSelfOscillation({44100.0, 48000.0, 96000.0, 192000.0});
    reportThreshold({44100.0, 96000.0});
    reportOscillationLevel();
    reportPassbandLoss();
    reportHarmonics();
    reportAliasing();
    reportSolverDistance();
    reportStability();
    reportBlockSizes();
    reportZipper();
}
