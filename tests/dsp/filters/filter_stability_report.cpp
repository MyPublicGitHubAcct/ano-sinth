// The stability report (M1.3 task 7, docs/dsp/filter-stability.md): what the
// filter's equations should do near Nyquist and at extreme settings, where
// LadderFilter's Newton solve fails, and the candidate fixes measured against
// a double-precision reference:
//
// - the reference: the solution is unique (checked over random states), and
//   what the filter does with the cutoff held from 0.40 to 0.495 fs, k 4.05
//   to 64 (level, pitch), against the built filter;
// - where the as-built solver fails: the lowest fc/fs with an unconverged
//   sample, k 3.5 to 64, an impulse and a full-scale saw at drive 32, and
//   the cutoff clamp that would avoid it (the do-nothing baseline);
// - every candidate over a grid of fc/fs 0.30-0.495, k 0-64, drive 1-32,
//   Warm on and off, 44.1-192 kHz: unconverged samples, iterations, and the
//   distance from the reference solved from the same state;
// - the held-cutoff case (an impulse of 0.5, the cutoff held at 0.46, 0.48,
//   0.495 fs): each candidate's level and pitch against the reference's;
// - no regression in the ordinary case: bit-identity with the as-built
//   solver on the filter report's harmonics and solver-distance material;
// - extreme settings: audio-rate modulation across the clamp, 1 Hz, DC of
//   +-10 and drive 32 at k 64, drive 1000, NaN and inf on the input and the
//   setters, blocks of 1 at the corner, responses and bypass switched while
//   self-oscillating, and five minutes of self-oscillation;
// - aliasing at the developer's corner (high resonance, high drive) and
//   self-oscillating near the clamp, at 1x, 2x, 4x; ADAA on the loop input
//   as an option.
//
// The prototypes are in filter_stability_candidates.hpp. CPU: the
// "[filter-stability]" benchmarks (tools/bench.sh "[filter-stability]").
//
// Hidden from the default run; run it in an optimised build (a few minutes):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[filter-stability-report]"

#include "filter_stability_candidates.hpp"

#include <anoesis/dsp/core/processor.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/resampling/oversampler.hpp>

#include <anoesis/test/buffer.hpp>
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
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::stability;
using anoesis::dsp::FilterResponse;
using anoesis::dsp::LadderFilter;
using anoesis::dsp::OversamplingFactor;

namespace
{

constexpr double pi = std::numbers::pi;
constexpr std::size_t blockSize = 512;
constexpr double nan = std::numeric_limits<double>::quiet_NaN();

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

std::string scientific(double x)
{
    if (!std::isfinite(x))
    {
        return fixed(x, 0);
    }
    std::ostringstream s;
    s << std::scientific << std::setprecision(1) << x;
    return s.str();
}

double dbOf(double amplitude)
{
    return 20.0 * std::log10(std::max(amplitude, 1e-300));
}

template <typename F>
void forBothSaturators(const F& f)
{
    f.template operator()<dsp::TanhSaturator>("Warm on (tanh)");
    f.template operator()<dsp::CubicSaturator>("Warm off (cubic)");
}

/// Every solver candidate, in the order of the doc's table.
template <typename F>
void forEachCandidate(const F& f)
{
    f.template operator()<NewtonAsBuilt<>>("As built: Newton, 8 iterations");
    f.template operator()<NewtonDamped<8>>("Damped Newton (line search), 8");
    f.template operator()<NewtonDamped<32>>("Damped Newton (line search), 32");
    f.template operator()<NewtonStepLimited<>>("Step-limited Newton (|dy| <= 1)");
    f.template operator()<NewtonPreviousGuess<>>("Newton from the previous solution");
    f.template operator()<NewtonOutputVariable<>>("Newton in the saturators' outputs");
    f.template operator()<NewtonBracketFallback<false>>("Newton + nested fallback, from Newton's iterate");
    f.template operator()<NewtonBracketFallback<true>>("Newton + nested fallback, from the guess");
    f.template operator()<NestedOnly>("Nested scalar solve alone");
}

/// The recommended candidate (docs/dsp/filter-stability.md).
using Chosen = NewtonBracketFallback<true>;

template <typename Sat, typename Solver>
CandidateFilter<Sat, Solver> makeCandidate(double rate, double cutoff, float k, float drive,
                                           float clampRatio = LadderFilter::maxCutoffRatio)
{
    CandidateFilter<Sat, Solver> f{clampRatio};
    f.setResonance(k);
    f.setDrive(drive);
    f.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = blockSize, .numChannels = 1});
    f.setCutoff(static_cast<float>(cutoff));
    return f;
}

template <typename Sat>
LadderFilter makeBuilt(double rate, double cutoff, float k, float drive)
{
    LadderFilter f;
    f.setWarm(std::is_same_v<Sat, dsp::TanhSaturator>);
    f.setCutoff(static_cast<float>(cutoff));
    f.setResonance(k);
    f.setDrive(drive);
    f.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = 4096, .numChannels = 1});
    return f;
}

std::vector<float> renderBuilt(LadderFilter& f, std::span<const float> x, std::size_t block = blockSize)
{
    Buffer buffer{1, x.size()};
    std::ranges::copy(x, buffer.channel(0).begin());
    dsp::processInChunks(f, buffer.block(), block);
    return {buffer.channel(0).begin(), buffer.channel(0).end()};
}

template <typename Filter>
std::vector<float> renderSamples(Filter& f, std::span<const float> x)
{
    std::vector<float> y(x.size());
    for (std::size_t n = 0; n < x.size(); ++n)
    {
        y[n] = f.processSample(0, x[n]);
    }
    return y;
}

std::vector<float> kick(std::size_t n, float amplitude = 0.5f)
{
    std::vector<float> x(n, 0.0f);
    x[0] = amplitude;
    return x;
}

std::vector<float> saw(double hz, double rate, std::size_t n, float amplitude = 0.5f)
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

double peakOf(std::span<const float> y)
{
    double peak = 0.0;
    for (const float v : y)
    {
        if (!std::isfinite(v))
        {
            return std::numeric_limits<double>::infinity();
        }
        peak = std::max(peak, static_cast<double>(std::fabs(v)));
    }
    return peak;
}

/// The strongest spectral peak in Hz (Hann 16384, parabolic on log power):
/// near Nyquist zero crossings can't resolve the period.
double peakHz(std::span<const float> x, double rate)
{
    static const Window window = hannWindow(16384);
    const auto spectrum = powerSpectrum(x.last(16384), rate, window);
    const auto& p = spectrum.power;
    const auto top = static_cast<std::size_t>(std::ranges::max_element(p) - p.begin());
    if (top == 0 || top + 1 >= p.size())
    {
        return spectrum.frequencyOf(top);
    }
    const double a = std::log(p[top - 1]);
    const double b = std::log(p[top]);
    const double c = std::log(p[top + 1]);
    const double offset = 0.5 * (a - c) / (a - (2.0 * b) + c);
    return spectrum.frequencyOf(top) + (offset * (spectrum.frequencyOf(1) - spectrum.frequencyOf(0)));
}

struct Behaviour
{
    double levelDb = -std::numeric_limits<double>::infinity(); ///< peak-equivalent, from the RMS
    double cents = nan;                                        ///< pitch from the cutoff
    double peak = 0.0;
};

/// Level, pitch, and peak over the last `seconds` of y.
Behaviour behaviourOf(std::span<const float> y, double rate, double cutoff, double seconds = 0.5)
{
    const auto tail = y.last(static_cast<std::size_t>(seconds * rate));
    Behaviour b;
    b.peak = peakOf(tail);
    double sum = 0.0;
    for (const float v : tail)
    {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    const double rms = std::sqrt(sum / static_cast<double>(tail.size()));
    if (!std::isfinite(rms))
    {
        b.levelDb = std::numeric_limits<double>::infinity();
        return b;
    }
    b.levelDb = dbOf(rms * std::numbers::sqrt2);
    if (rms > 1e-6)
    {
        b.cents = 1200.0 * std::log2(peakHz(tail, rate) / cutoff);
    }
    return b;
}

std::string describe(const Behaviour& b)
{
    if (b.levelDb < -200.0)
    {
        return "silent";
    }
    return fixed(b.levelDb, 1) + " / " + fixed(b.cents, 1) + (b.peak > 2.0 ? " (peak " + fixed(b.peak, 0) + ")" : "");
}

/// The reference ladder along its own trajectory, as floats for measuring.
template <typename Sat>
std::vector<float> renderReference(std::span<const float> x, float drive, float g, float k)
{
    ReferenceLadder<typename ReferenceOf<Sat>::Type> ladder;
    std::vector<float> y(x.size());
    for (std::size_t n = 0; n < x.size(); ++n)
    {
        y[n] = static_cast<float>(ladder.process(static_cast<double>(x[n] * drive), g, k));
    }
    return y;
}

float gainFor(double cutoff, double rate)
{
    return makeCandidate<dsp::TanhSaturator, NewtonAsBuilt<>>(rate, cutoff, 0.0f, 1.0f).gain();
}

// --- 1. The reference -----------------------------------------------------------

void reportUniqueness()
{
    std::cout << "\n### The solution is unique\n\n"
                 "The last row phi(y4) = y4 - H(y4) evaluated at 2,000 points across [s4 - 2g, s4 + 2g] for 4,000 "
                 "random states (s in +-50, x in +-32, g 0.1-64, k 0-64), in double: the number of sign changes "
                 "(1 means one root) and of points where phi decreases (0 means increasing).\n\n"
                 "| Saturator | Most sign changes | Decreasing steps | Smallest slope of phi |\n|---|---|---|---|\n";
    const auto check = [&]<typename R>(const char* name)
    {
        std::uint32_t state = 7;
        const auto next = [&]
        {
            state = (state * 1664525U) + 1013904223U;
            return static_cast<double>(state >> 8U) / 16777216.0;
        };
        int mostChanges = 0;
        int decreasing = 0;
        double smallestSlope = std::numeric_limits<double>::infinity();
        for (int trial = 0; trial < 4000; ++trial)
        {
            std::array<double, 4> s{};
            for (auto& v : s)
            {
                v = (next() * 100.0) - 50.0;
            }
            const double x = (next() * 64.0) - 32.0;
            const double g = 0.1 * std::pow(640.0, next());
            const double k = next() < 0.2 ? 64.0 : next() * 16.0;
            std::array<double, 4> y{};
            double previous = 0.0;
            double previousY4 = 0.0;
            int changes = 0;
            for (int i = 0; i <= 2000; ++i)
            {
                const double y4 = s[3] - (2.0 * g) + (4.0 * g * i / 2000.0);
                double derivative = 0.0;
                const double phi = y4 - referenceChain<R>(s, x, g, k, y4, y, derivative);
                smallestSlope = std::min(smallestSlope, 1.0 - derivative);
                if (i > 0)
                {
                    changes += (phi > 0.0) != (previous > 0.0) ? 1 : 0;
                    decreasing += phi < previous - (1e-9 * (1.0 + std::fabs(previous))) ? 1 : 0;
                    CHECK(y4 > previousY4);
                }
                previous = phi;
                previousY4 = y4;
            }
            mostChanges = std::max(mostChanges, changes);
        }
        CHECK(mostChanges == 1);
        CHECK(decreasing == 0);
        std::cout << "| " << name << " | " << mostChanges << " | " << decreasing << " | " << fixed(smallestSlope, 3)
                  << " |\n";
    };
    check.operator()<ReferenceTanh>("tanh");
    check.operator()<ReferenceCubic>("cubic");
}

void reportReferenceNearNyquist()
{
    constexpr double rate = 48000.0;
    constexpr std::array<double, 7> ratios{0.30, 0.40, 0.44, 0.46, 0.48, 0.49, 0.495};
    constexpr std::array<float, 7> ks{4.05f, 4.4f, 5.0f, 6.0f, 8.0f, 16.0f, 64.0f};
    const auto x = kick(static_cast<std::size_t>(1.5 * rate));
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            std::cout << "\n### What the equations do with the cutoff held near Nyquist: the reference, " << name
                      << "\n\nAn impulse of 0.5, 48 kHz, 1.5 s; the last 0.5 s: level (dBFS, peak-equivalent from "
                         "the RMS) / pitch (cents from the cutoff). Below, the built filter's peak there.\n\n"
                         "| fc/fs | g | k 4.05 | 4.4 | 5 | 6 | 8 | 16 | 64 |\n|---|---|---|---|---|---|---|---|---|\n";
            for (const double ratio : ratios)
            {
                const float g = gainFor(ratio * rate, rate);
                std::cout << "| " << fixed(ratio, 3) << " | " << fixed(g, 2);
                for (const float k : ks)
                {
                    const auto y = renderReference<Sat>(x, 1.0f, g, k);
                    std::cout << " | " << describe(behaviourOf(y, rate, ratio * rate));
                }
                std::cout << " |\n";
            }
            std::cout << "| built, peak | |";
            for (const float k : ks)
            {
                auto f = makeBuilt<Sat>(rate, 0.495 * rate, k, 1.0f);
                std::cout << " " << fixed(behaviourOf(renderBuilt(f, x), rate, 0.495 * rate).peak, 1) << " |";
            }
            std::cout << "\n";
            std::cout << "\nFor comparison, at 440 Hz (filters.md): k 4.05 " << name << ", "
                      << describe(behaviourOf(renderReference<Sat>(kick(static_cast<std::size_t>(3.0 * rate)), 1.0f,
                                                                   gainFor(440.0, rate), 4.05f),
                                              rate, 440.0))
                      << "; k 8, "
                      << describe(behaviourOf(renderReference<Sat>(kick(static_cast<std::size_t>(3.0 * rate)), 1.0f,
                                                                   gainFor(440.0, rate), 8.0f),
                                              rate, 440.0))
                      << ".\n";
        });
}

// --- 2. Where the as-built solver fails ----------------------------------------------

/// The lowest fc/fs (0.0025 to 0.495 in steps of 0.0025) at which the solver
/// leaves a sample unconverged, or 0 if none.
template <typename Sat, typename Solver>
double lowestFailure(double rate, float k, float drive, std::span<const float> x)
{
    for (int step = 1; step <= 198; ++step)
    {
        const double ratio = 0.0025 * step;
        auto f = makeCandidate<Sat, Solver>(rate, ratio * rate, k, drive);
        static_cast<void>(renderSamples(f, x));
        if (f.stats.unconverged > 0)
        {
            return ratio;
        }
    }
    return 0.0;
}

void reportBoundary()
{
    constexpr double rate = 48000.0;
    constexpr std::array<float, 12> ks{3.5f, 4.05f, 4.4f, 5.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f, 48.0f, 64.0f};
    const auto impulse = kick(12000);
    const auto loud = saw(110.0, rate, 12000, 1.0f);
    std::cout << "\n### Where the as-built solver fails\n\n"
                 "The lowest fc/fs (from 0.0025 in steps of 0.0025) with a sample Newton leaves unconverged at 8 "
                 "iterations, 48 kHz, 0.25 s (- : none up to 0.495). Impulse: 0.5 at drive 1. Saw: a full-scale "
                 "110 Hz saw at drive 16 (F8's +24 dB) and 32.\n\n"
                 "| Saturator | Input | k 3.5 | 4.05 | 4.4 | 5 | 6 | 8 | 12 | 16 | 24 | 32 | 48 | 64 |\n"
                 "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    double failureBoundary = 0.495; // the lowest failing fc/fs anywhere, for the clamp
    double lowestUpToSix = 0.495;
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            for (const float drive : {0.0f, 16.0f, 32.0f})
            {
                const bool isSaw = drive > 0.0f;
                std::cout << "| " << name << " | "
                          << (isSaw ? "saw, drive " + fixed(drive, 0) : std::string{"impulse"});
                for (const float k : ks)
                {
                    const double r = lowestFailure<Sat, NewtonAsBuilt<>>(rate, k, isSaw ? drive : 1.0f,
                                                                         isSaw ? std::span<const float>{loud}
                                                                               : std::span<const float>{impulse});
                    std::cout << " | " << (r > 0.0 ? fixed(r, 4) : "-");
                    if (r > 0.0)
                    {
                        failureBoundary = std::min(failureBoundary, r);
                        if (k <= 6.0f && drive <= 16.0f)
                        {
                            lowestUpToSix = std::min(lowestUpToSix, r);
                        }
                    }
                }
                std::cout << " |\n";
            }
        });
    const auto semitones = [](double from, double to)
    {
        return 12.0 * std::log2(from / to);
    };
    const double clamp = failureBoundary - 0.0025;
    const double clampSix = lowestUpToSix - 0.0025;
    std::cout
        << "\n**The clamp as the fix (the do-nothing baseline).** The highest safe fc/fs for k up to 64 and drive "
           "up to 32: "
        << fixed(clamp, 4)
        << "; for k up to 6 and drive up to 16 (Resonance and Drive at 10 under F5 and F8): " << fixed(clampSix, 4)
        << ". The top cutoff it leaves, against today's 0.495 fs and F3's 20.5 kHz:\n\n"
           "| Rate | 0.495 fs | Any k, drive: clamp | lost | The knobs' ranges: clamp | lost | F3's top (20.5 kHz) |\n"
           "|---|---|---|---|---|---|---|\n";
    for (const double r : {44100.0, 48000.0, 96000.0})
    {
        const double top = std::min(20500.0, 0.495 * r);
        std::cout << "| " << fixed(r / 1000.0, 1) << " kHz | " << fixed(0.495 * r, 0) << " Hz | " << fixed(clamp * r, 0)
                  << " Hz | " << fixed(semitones(0.495 * r, clamp * r), 2) << " st | " << fixed(clampSix * r, 0)
                  << " Hz | " << fixed(semitones(0.495 * r, clampSix * r), 2) << " st | "
                  << (20500.0 <= clampSix * r ? std::string{"reachable"}
                                              : "cut by " + fixed(semitones(top, clampSix * r), 2) + " st")
                  << " |\n";
    }
}

// --- 3. The convergence grid -----------------------------------------------------------------

struct GridTotals
{
    std::uint64_t samples = 0;
    std::uint64_t unconverged = 0;
    std::uint64_t iterations = 0;
    std::uint32_t maxIterations = 0;
    std::uint64_t fallbacks = 0;
    std::uint64_t stageEvaluations = 0;
    std::size_t cells = 0;
    std::size_t cellsUnconverged = 0;
    std::size_t cellsOff = 0; ///< some sample more than 1e-4 of its equation's scale from the reference
    double worstDistance = 0.0;
    double worstRelative = 0.0;
    std::string worstCell;
};

constexpr std::array<double, 11> gridRatios{0.30, 0.35, 0.40, 0.42, 0.44, 0.45, 0.46, 0.47, 0.48, 0.49, 0.495};
constexpr std::array<float, 11> gridKs{0.0f, 2.0f, 3.9f, 4.05f, 4.4f, 5.0f, 6.0f, 8.0f, 16.0f, 32.0f, 64.0f};
constexpr std::array<float, 4> gridDrives{1.0f, 4.0f, 16.0f, 32.0f};
constexpr std::array<double, 4> gridRates{44100.0, 48000.0, 96000.0, 192000.0};
constexpr std::size_t gridSamples = 2048;

/// For the as-built map: cells off the reference, by fc/fs and k.
using FailureMap = std::array<std::array<int, gridKs.size()>, gridRatios.size()>;

template <typename Sat, typename Solver>
GridTotals runGrid(FailureMap* map)
{
    using R = ReferenceOf<Sat>::Type;
    GridTotals t;
    for (const double rate : gridRates)
    {
        const auto x = saw(110.0, rate, gridSamples);
        for (std::size_t ri = 0; ri < gridRatios.size(); ++ri)
        {
            for (std::size_t ki = 0; ki < gridKs.size(); ++ki)
            {
                for (const float drive : gridDrives)
                {
                    const double ratio = gridRatios[ri];
                    const float k = gridKs[ki];
                    auto f = makeCandidate<Sat, Solver>(rate, ratio * rate, k, drive);
                    double worst = 0.0;
                    double worstRelative = 0.0;
                    for (const float v : x)
                    {
                        const auto before = f.state();
                        static_cast<void>(f.processSample(0, v));
                        const auto& after = f.state();
                        const std::array<double, 4> s{before.s[0], before.s[1], before.s[2], before.s[3]};
                        const std::array<double, 4> guess{after.y[0], after.y[1], after.y[2], after.y[3]};
                        const auto r = referenceSolve<R>(s, static_cast<double>(v * drive), f.gain(), k, guess);
                        // The size of the terms in the stage equations, which sets float's rounding there.
                        double scale = 1.0 + (2.0 * static_cast<double>(f.gain()));
                        for (const double si : s)
                        {
                            scale = std::max(scale, 1.0 + std::fabs(si) + (2.0 * static_cast<double>(f.gain())));
                        }
                        for (std::size_t i = 0; i < 4; ++i)
                        {
                            const double d = std::fabs(static_cast<double>(after.y[i]) - r.y[i]);
                            worst = std::isfinite(d) ? std::max(worst, d) : std::numeric_limits<double>::infinity();
                            worstRelative = std::isfinite(d) ? std::max(worstRelative, d / scale)
                                                             : std::numeric_limits<double>::infinity();
                        }
                    }
                    const auto& st = f.stats;
                    t.samples += st.samples;
                    t.unconverged += st.unconverged;
                    t.iterations += st.iterations;
                    t.maxIterations = std::max(t.maxIterations, st.maxIterations);
                    t.fallbacks += st.fallbacks;
                    t.stageEvaluations += st.stageEvaluations;
                    ++t.cells;
                    t.cellsUnconverged += st.unconverged > 0 ? 1U : 0U;
                    t.cellsOff += worstRelative > 1e-4 ? 1U : 0U;
                    t.worstRelative = std::max(t.worstRelative, worstRelative);
                    if (map != nullptr && worstRelative > 1e-4)
                    {
                        ++(*map)[ri][ki];
                    }
                    t.worstDistance = std::max(t.worstDistance, worst);
                    if (worstRelative >= t.worstRelative)
                    {
                        t.worstCell = fixed(ratio, 3) + " fs, k " + fixed(k, 2) + ", drive " + fixed(drive, 0) + ", " +
                                      fixed(rate / 1000.0, 1) + " kHz";
                    }
                }
            }
        }
    }
    return t;
}

void reportGrid()
{
    std::cout
        << "\n### Every candidate over the grid\n\n"
           "fc/fs 0.30, 0.35, 0.40, 0.42, 0.44-0.49 by 0.01, 0.495; k 0, 2, 3.9, 4.05, 4.4, 5, 6, 8, 16, 32, 64; "
           "drive 1, 4, 16, 32; 44.1, 48, 96, 192 kHz: 1,936 cells per saturator, each a 110 Hz saw at "
           "-6 dBFS (times the drive) for 2,048 samples from rest. Unconverged: samples the solver ended "
           "without a step under 1e-5. Iterations: residual evaluations, plus the nested solve's rows (its "
           "stage evaluations are the next column, per sample). Distance: the largest |y_i - y_ref,i| per "
           "sample, the reference solved in double from the candidate's own state, and the same relative to "
           "the size of the terms in the stage equations (1 + max|s_i| + 2g), which sets float's rounding "
           "there (about 1e-7 of it); off: cells with a sample more than 1e-4 of that scale away.\n\n"
           "| Candidate | Saturator | Unconverged samples | Cells unconverged | Cells off | Mean / max "
           "iterations | Nested solves / stage evaluations per sample | Largest distance: absolute / relative | Where "
           "(the largest relative) |\n"
           "|---|---|---|---|---|---|---|---|---|\n";
    FailureMap tanhMap{};
    FailureMap cubicMap{};
    forEachCandidate(
        [&]<typename Solver>(const char* candidate)
        {
            forBothSaturators(
                [&]<typename Sat>(const char* name)
                {
                    FailureMap* map = nullptr;
                    if constexpr (std::is_same_v<Solver, NewtonAsBuilt<>>)
                    {
                        map = std::is_same_v<Sat, dsp::TanhSaturator> ? &tanhMap : &cubicMap;
                    }
                    const auto t = runGrid<Sat, Solver>(map);
                    const auto samples = static_cast<double>(t.samples);
                    std::cout << "| " << candidate << " | " << name << " | " << t.unconverged << " ("
                              << fixed(100.0 * static_cast<double>(t.unconverged) / samples, 2) << "%) | "
                              << t.cellsUnconverged << " | " << t.cellsOff << " | "
                              << fixed(static_cast<double>(t.iterations) / samples, 2) << " / " << t.maxIterations
                              << " | " << fixed(100.0 * static_cast<double>(t.fallbacks) / samples, 2) << "% / "
                              << fixed(static_cast<double>(t.stageEvaluations) / samples, 2) << " | "
                              << scientific(t.worstDistance) << " / " << scientific(t.worstRelative) << " | "
                              << t.worstCell << " |\n";
                });
        });

    for (const auto& [map, name] : {std::pair{&tanhMap, "tanh"}, std::pair{&cubicMap, "cubic"}})
    {
        std::cout
            << "\n**Where the as-built solver is off the reference** (" << name
            << "): cells off (more than 1e-4 of the equations' scale), of 16 (4 drives x 4 rates), by fc/fs and k.\n\n"
               "| fc/fs | k 0 | 2 | 3.9 | 4.05 | 4.4 | 5 | 6 | 8 | 16 | 32 | 64 |\n"
               "|---|---|---|---|---|---|---|---|---|---|---|---|\n";
        for (std::size_t ri = 0; ri < gridRatios.size(); ++ri)
        {
            std::cout << "| " << fixed(gridRatios[ri], 3);
            for (std::size_t ki = 0; ki < gridKs.size(); ++ki)
            {
                const int n = (*map)[ri][ki];
                std::cout << " | " << (n == 0 ? "." : std::to_string(n));
            }
            std::cout << " |\n";
        }
    }
}

// --- 4. The held-cutoff case -----------------------------------------------------------------------

void reportHeldCutoff()
{
    constexpr std::array<float, 7> ks{4.05f, 4.4f, 5.0f, 6.0f, 8.0f, 16.0f, 64.0f};
    for (const double rate : {48000.0, 44100.0})
    {
        forBothSaturators(
            [&]<typename Sat>(const char* name)
            {
                const double cutoff = 0.495 * rate;
                const auto x = kick(static_cast<std::size_t>(1.5 * rate));
                std::cout << "\n### The held cutoff: an impulse of 0.5, the cutoff at 0.495 fs, " << name << ", "
                          << fixed(rate / 1000.0, 1)
                          << " kHz\n\nLevel (dBFS) / pitch (cents from the cutoff) over the last 0.5 s of 1.5 s, with "
                             "the peak there when above 2.\n\n"
                             "| Model | k 4.05 | 4.4 | 5 | 6 | 8 | 16 | 64 | Matches |\n"
                             "|---|---|---|---|---|---|---|---|---|\n";
                const float g = gainFor(cutoff, rate);
                std::array<Behaviour, ks.size()> reference{};
                std::cout << "| **Reference (double)**";
                for (std::size_t i = 0; i < ks.size(); ++i)
                {
                    reference[i] = behaviourOf(renderReference<Sat>(x, 1.0f, g, ks[i]), rate, cutoff);
                    std::cout << " | " << describe(reference[i]);
                }
                std::cout << " | |\n";
                forEachCandidate(
                    [&]<typename Solver>(const char* candidate)
                    {
                        std::cout << "| " << candidate;
                        int matches = 0;
                        for (std::size_t i = 0; i < ks.size(); ++i)
                        {
                            auto f = makeCandidate<Sat, Solver>(rate, cutoff, ks[i], 1.0f);
                            const auto b = behaviourOf(renderSamples(f, x), rate, cutoff);
                            const bool match = std::fabs(b.levelDb - reference[i].levelDb) < 0.1 &&
                                               std::fabs(b.cents - reference[i].cents) < 1.0 &&
                                               b.peak < (1.1 * reference[i].peak) + 1e-3;
                            matches += match ? 1 : 0;
                            std::cout << " | " << describe(b);
                        }
                        std::cout << " | " << matches << " of " << ks.size() << " |\n";
                    });
            });
    }

    std::cout << "\n**Lower held cutoffs** (48 kHz): the number of k (of 4.05, 4.4, 5, 6, 8, 16, 64) where the "
                 "candidate matches the reference within 0.1 dB and 1 cent, peak within 10%.\n\n"
                 "| Candidate | tanh 0.44 | 0.46 | 0.48 | cubic 0.44 | 0.46 | 0.48 |\n|---|---|---|---|---|---|---|\n";
    constexpr double rate = 48000.0;
    const auto x = kick(static_cast<std::size_t>(1.5 * rate));
    std::vector<std::vector<Behaviour>> references;
    forBothSaturators(
        [&]<typename Sat>(const char*)
        {
            for (const double ratio : {0.44, 0.46, 0.48})
            {
                std::vector<Behaviour> row;
                row.reserve(ks.size());
                for (const float k : ks)
                {
                    row.push_back(
                        behaviourOf(renderReference<Sat>(x, 1.0f, gainFor(ratio * rate, rate), k), rate, ratio * rate));
                }
                references.push_back(row);
            }
        });
    forEachCandidate(
        [&]<typename Solver>(const char* candidate)
        {
            std::cout << "| " << candidate;
            std::size_t column = 0;
            forBothSaturators(
                [&]<typename Sat>(const char*)
                {
                    for (const double ratio : {0.44, 0.46, 0.48})
                    {
                        int matches = 0;
                        for (std::size_t i = 0; i < ks.size(); ++i)
                        {
                            auto f = makeCandidate<Sat, Solver>(rate, ratio * rate, ks[i], 1.0f);
                            const auto b = behaviourOf(renderSamples(f, x), rate, ratio * rate);
                            const auto& r = references[column][i];
                            matches += std::fabs(b.levelDb - r.levelDb) < 0.1 && std::fabs(b.cents - r.cents) < 1.0 &&
                                               b.peak < (1.1 * r.peak) + 1e-3
                                           ? 1
                                           : 0;
                        }
                        std::cout << " | " << matches;
                        ++column;
                    }
                });
            std::cout << " |\n";
        });
}

// --- 5. No regression in the ordinary case ---------------------------------------------------------

bool bitIdentical(std::span<const float> a, std::span<const float> b)
{
    return std::ranges::equal(a, b, [](float p, float q)
                              { return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q); });
}

double errorDb(std::span<const float> y, std::span<const float> reference, std::size_t from)
{
    double e = 0.0;
    double s = 0.0;
    for (std::size_t n = from; n < y.size(); ++n)
    {
        const double d = static_cast<double>(y[n]) - static_cast<double>(reference[n]);
        e += d * d;
        s += static_cast<double>(reference[n]) * static_cast<double>(reference[n]);
    }
    return e > 0.0 ? 10.0 * std::log10(e / s) : -std::numeric_limits<double>::infinity();
}

/// The filter report's solver-distance material: a 110 Hz saw at -6 dBFS, the
/// cutoff swept 100 Hz -> 8 kHz -> 100 Hz (in octaves) over 2 s, 44.1 kHz,
/// g per sample as the candidates computed it.
template <typename Sat, typename Solver>
std::vector<float> sweep(float k, float drive, bool reference)
{
    constexpr double rate = 44100.0;
    const auto n = static_cast<std::size_t>(2.0 * rate);
    const auto x = saw(110.0, rate, n);
    auto f = makeCandidate<Sat, Solver>(rate, 100.0, k, drive);
    ReferenceLadder<typename ReferenceOf<Sat>::Type> ladder;
    std::vector<float> y(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double phase = static_cast<double>(i) / static_cast<double>(n);
        const double octaves = std::log2(80.0) * (1.0 - std::fabs((2.0 * phase) - 1.0));
        f.setCutoff(static_cast<float>(100.0 * std::exp2(octaves)));
        y[i] = reference ? static_cast<float>(ladder.process(static_cast<double>(x[i] * drive), f.gain(), k))
                         : f.processSample(0, x[i]);
    }
    return y;
}

template <typename Sat, typename Solver>
std::vector<float> harmonicsMaterial(float drive)
{
    constexpr double rate = 48000.0;
    auto f = makeCandidate<Sat, Solver>(rate, 10000.0, 0.0f, drive);
    return renderSamples(f, sine(coherentFrequency(220.0, rate, 65536), rate, 8192 + 65536, 0.5));
}

void reportOrdinary()
{
    // The as-built candidate against LadderFilter itself.
    bool sameAsFilter = true;
    forBothSaturators(
        [&]<typename Sat>(const char*)
        {
            for (const float drive : {1.0f, 4.0f, 16.0f})
            {
                auto built = makeBuilt<Sat>(48000.0, 10000.0, 0.0f, drive);
                const auto x = sine(coherentFrequency(220.0, 48000.0, 65536), 48000.0, 8192 + 65536, 0.5);
                sameAsFilter =
                    sameAsFilter && bitIdentical(renderBuilt(built, x), harmonicsMaterial<Sat, NewtonAsBuilt<>>(drive));
            }
            for (const float k : {3.8f, 8.0f})
            {
                auto built = makeBuilt<Sat>(48000.0, 1000.0, k, 4.0f);
                auto copy = makeCandidate<Sat, NewtonAsBuilt<>>(48000.0, 1000.0, k, 4.0f);
                const auto x = saw(110.0, 48000.0, 48000);
                sameAsFilter = sameAsFilter && bitIdentical(renderBuilt(built, x), renderSamples(copy, x));
            }
        });
    CHECK(sameAsFilter);
    std::cout << "\n### No regression in the ordinary case\n\n"
              << "The as-built candidate reproduces LadderFilter bit for bit (LP24, settled controls: the harmonics "
                 "material at drive 1, 4, 16, and a saw at k 3.8 and 8 at 1 kHz): "
              << (sameAsFilter ? "yes" : "NO") << ".\n\n"
              << "Each candidate against the as-built solver. Harmonics: LP24 at 10 kHz, k 0, a 220 Hz sine at "
                 "-6 dBFS, drive 1, 2, 4, 8, 16, 48 kHz (filters.md's THD table). Sweeps: filters.md's solver-distance "
                 "material (a 110 Hz saw at -6 dBFS, the cutoff 100 Hz -> 8 kHz -> 100 Hz over 2 s, 44.1 kHz), hard "
                 "(k 3.8, drive 4) and gentle (k 2, drive 2). Identical: bit for bit with the as-built solver; "
                 "otherwise the error against it (dB re the signal). Against the reference: the error against the "
                 "double-precision ladder on its own trajectory, hard / gentle.\n\n"
                 "| Candidate | Saturator | Harmonics, 5 drives | Hard sweep | Gentle sweep | Against the reference, "
                 "hard / gentle (dB) |\n"
                 "|---|---|---|---|---|---|\n";
    forEachCandidate(
        [&]<typename Solver>(const char* candidate)
        {
            forBothSaturators(
                [&]<typename Sat>(const char* name)
                {
                    int identical = 0;
                    double worstHarmonics = -std::numeric_limits<double>::infinity();
                    for (const float drive : {1.0f, 2.0f, 4.0f, 8.0f, 16.0f})
                    {
                        const auto a = harmonicsMaterial<Sat, NewtonAsBuilt<>>(drive);
                        const auto b = harmonicsMaterial<Sat, Solver>(drive);
                        identical += bitIdentical(a, b) ? 1 : 0;
                        worstHarmonics = std::max(worstHarmonics, errorDb(b, a, 8192));
                    }
                    const auto cell = [](std::span<const float> a, std::span<const float> b)
                    {
                        return bitIdentical(a, b) ? std::string{"identical"} : fixed(errorDb(b, a, 4410), 1) + " dB";
                    };
                    const auto hard = sweep<Sat, Solver>(3.8f, 4.0f, false);
                    const auto gentle = sweep<Sat, Solver>(2.0f, 2.0f, false);
                    std::cout << "| " << candidate << " | " << name << " | "
                              << (identical == 5 ? std::string{"identical"}
                                                 : std::to_string(identical) + " of 5 identical; " +
                                                       fixed(worstHarmonics, 1) + " dB")
                              << " | " << cell(sweep<Sat, NewtonAsBuilt<>>(3.8f, 4.0f, false), hard) << " | "
                              << cell(sweep<Sat, NewtonAsBuilt<>>(2.0f, 2.0f, false), gentle) << " | "
                              << fixed(errorDb(hard, sweep<Sat, Solver>(3.8f, 4.0f, true), 4410), 1) << " / "
                              << fixed(errorDb(gentle, sweep<Sat, Solver>(2.0f, 2.0f, true), 4410), 1) << " |\n";
                });
        });
}

// --- 6. Extreme settings ---------------------------------------------------------------------------------

struct Outcome
{
    double peak = 0.0;
    std::size_t nonFinite = 0;
};

Outcome outcomeOf(std::span<const float> y)
{
    Outcome o;
    for (const float v : y)
    {
        if (!std::isfinite(v))
        {
            ++o.nonFinite;
        }
        else
        {
            o.peak = std::max(o.peak, static_cast<double>(std::fabs(v)));
        }
    }
    return o;
}

std::string show(const Outcome& o)
{
    if (o.nonFinite > 0)
    {
        return std::to_string(o.nonFinite) + " non-finite";
    }
    return "peak " + (o.peak < 0.01 ? scientific(o.peak) : fixed(o.peak, 2));
}

constexpr std::array<float, 3> badValues{
    std::numeric_limits<float>::quiet_NaN(),
    std::numeric_limits<float>::infinity(),
    -std::numeric_limits<float>::infinity(),
};

void reportExtremes()
{
    constexpr double rate = 48000.0;
    std::cout << "\n### Extreme settings\n\nAs built: LadderFilter. Chosen: the recommended candidate (Newton, nested "
                 "fallback from the guess) in the candidate wrapper (LP24, g set directly). 48 kHz.\n";

    // Audio-rate modulation across the clamp.
    std::cout << "\n**Cutoff modulation across the clamp**: a full-scale 110 Hz saw; the cutoff 12 kHz plus "
                 "1.5 sin(2 pi 300 t) octaves (to 34 kHz, clamped at 0.495 fs), set every 32 samples (as built: "
                 "the modulation input; chosen: g per sample), 1 s. Peak; for the chosen solver, unconverged "
                 "samples.\n\n"
                 "| Saturator | k | Drive | As built | Chosen | Chosen, unconverged |\n|---|---|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            for (const float k : {0.0f, 4.4f, 8.0f, 64.0f})
            {
                for (const float drive : {1.0f, 32.0f})
                {
                    const auto n = static_cast<std::size_t>(rate);
                    const auto x = saw(110.0, rate, n, 1.0f);
                    auto built = makeBuilt<Sat>(rate, 12000.0, k, drive);
                    Buffer buffer{1, n};
                    std::ranges::copy(x, buffer.channel(0).begin());
                    for (std::size_t start = 0; start < n; start += 32)
                    {
                        const double t = static_cast<double>(start) / rate;
                        built.setCutoffModulation(static_cast<float>(1.5 * std::sin(2.0 * pi * 300.0 * t)));
                        built.process(buffer.block().subBlock(start, 32));
                    }
                    auto chosen = makeCandidate<Sat, Chosen>(rate, 12000.0, k, drive);
                    std::vector<float> y(n);
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        const double t = static_cast<double>(i) / rate;
                        chosen.setCutoff(static_cast<float>(12000.0 * std::exp2(1.5 * std::sin(2.0 * pi * 300.0 * t))));
                        y[i] = chosen.processSample(0, x[i]);
                    }
                    std::cout << "| " << name << " | " << fixed(k, 1) << " | " << fixed(drive, 0) << " | "
                              << show(outcomeOf(buffer.channel(0))) << " | " << show(outcomeOf(y)) << " | "
                              << chosen.stats.unconverged << " |\n";
                }
            }
        });

    // 1 Hz, DC, drive 32 and 1000, k 64.
    std::cout << "\n**Low cutoff, DC, and the ceilings**: 2 s each. DC: 0 -> +10 -> -10 in thirds. Saw: full scale, "
                 "110 Hz. The bound is the loop's DC gain plus a self-oscillation under 0 dBFS, drive x max|x| / "
                 "(1 + k) + 1.\n\n"
                 "| Case | Saturator | As built | Chosen | Bound |\n|---|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            const auto n = static_cast<std::size_t>(2.0 * rate);
            std::vector<float> dc(n, 0.0f);
            std::fill(dc.begin() + static_cast<std::ptrdiff_t>(n / 3),
                      dc.begin() + static_cast<std::ptrdiff_t>(2 * n / 3), 10.0f);
            std::fill(dc.begin() + static_cast<std::ptrdiff_t>(2 * n / 3), dc.end(), -10.0f);
            const auto loud = saw(110.0, rate, n, 1.0f);
            struct Case
            {
                const char* label;
                double cutoff;
                float k;
                float drive;
                const std::vector<float>* input;
                double maxInput;
            };
            const std::array<Case, 9> cases{
                Case{"1 Hz, k 0, saw, drive 32", 1.0, 0.0f, 32.0f, &loud, 1.0},
                Case{"1 Hz, k 4.4, saw, drive 32", 1.0, 4.4f, 32.0f, &loud, 1.0},
                Case{"1 Hz, k 64, saw, drive 32", 1.0, 64.0f, 32.0f, &loud, 1.0},
                Case{"1 Hz, k 64, DC +-10, drive 32", 1.0, 64.0f, 32.0f, &dc, 10.0},
                Case{"1 kHz, k 64, DC +-10, drive 32", 1000.0, 64.0f, 32.0f, &dc, 10.0},
                Case{"1 kHz, k 64, saw, drive 32", 1000.0, 64.0f, 32.0f, &loud, 1.0},
                Case{"1 kHz, k 64, saw, drive 1000", 1000.0, 64.0f, 1000.0f, &loud, 1.0},
                Case{"0.495 fs, k 64, DC +-10, drive 32", 0.495 * rate, 64.0f, 32.0f, &dc, 10.0},
                Case{"0.495 fs, k 64, saw, drive 1000", 0.495 * rate, 64.0f, 1000.0f, &loud, 1.0},
            };
            for (const auto& c : cases)
            {
                auto built = makeBuilt<Sat>(rate, c.cutoff, c.k, c.drive);
                auto chosen = makeCandidate<Sat, Chosen>(rate, c.cutoff, c.k, c.drive);
                const double bound = (static_cast<double>(c.drive) * c.maxInput / (1.0 + c.k)) + 1.0;
                std::cout << "| " << c.label << " | " << name << " | " << show(outcomeOf(renderBuilt(built, *c.input)))
                          << " | " << show(outcomeOf(renderSamples(chosen, *c.input))) << " | " << fixed(bound, 2)
                          << " |\n";
            }
            auto oneHz = makeCandidate<Sat, Chosen>(rate, 1.0, 64.0f, 1.0f);
            const auto b =
                behaviourOf(renderSamples(oneHz, kick(static_cast<std::size_t>(20.0 * rate))), rate, 1.0, 10.0);
            std::cout << "| 1 Hz, k 64, an impulse: the last 10 s of 20 | " << name << " | | level "
                      << fixed(b.levelDb, 1) << " dBFS, peak " << fixed(b.peak, 3) << " | |\n";
        });

    // NaN and inf.
    std::cout << "\n**NaN and inf.** Input: a 220 Hz saw at -6 dBFS, k 4.4, 1 kHz, with one bad sample at 1000; "
                 "non-finite outputs after it, and whether the filter is back to finite output by the end (1 s). "
                 "Then reset(). Setters: each set to the bad value mid-run.\n\n"
                 "| Bad value | Where | As built: non-finite outputs | Recovers | After reset() | Input sanitised "
                 "| State checked per block |\n|---|---|---|---|---|---|---|\n";
    for (const float bad : badValues)
    {
        const auto n = static_cast<std::size_t>(rate);
        auto x = saw(220.0, rate, n);
        x[1000] = bad;
        auto built = makeBuilt<dsp::TanhSaturator>(rate, 1000.0, 4.4f, 1.0f);
        const auto y = renderBuilt(built, x);
        const auto after = std::span{y}.subspan(1001);
        const auto nonFinite = std::ranges::count_if(after, [](float v) { return !std::isfinite(v); });
        built.reset();
        const auto clean = renderBuilt(built, saw(220.0, rate, 4800));
        auto sanitized = makeCandidate<dsp::TanhSaturator, SanitizedInput<NewtonAsBuilt<>>>(rate, 1000.0, 4.4f, 1.0f);
        const auto ys = renderSamples(sanitized, x);
        auto checked = makeCandidate<dsp::TanhSaturator, NewtonAsBuilt<>>(rate, 1000.0, 4.4f, 1.0f);
        std::vector<float> yc(n);
        for (std::size_t start = 0; start < n; start += blockSize)
        {
            for (std::size_t i = start; i < std::min(n, start + blockSize); ++i)
            {
                yc[i] = checked.processSample(0, x[i]);
            }
            checked.recoverNonFinite();
        }
        const auto count = [](std::span<const float> v)
        {
            return std::ranges::count_if(v, [](float s) { return !std::isfinite(s); });
        };
        std::cout << "| " << fixed(bad, 0) << " | input | " << nonFinite << " | "
                  << (std::isfinite(y.back()) ? "yes" : "no") << " | "
                  << (std::ranges::all_of(clean, [](float v) { return std::isfinite(v); }) ? "finite" : "NOT finite")
                  << " | " << count(ys) << " non-finite | " << count(yc) << " non-finite (to the block's end) |\n";
    }
    for (const float bad : badValues)
    {
        constexpr std::array<const char*, 4> setters{"setCutoff", "setCutoffModulation", "setResonance", "setDrive"};
        for (std::size_t which = 0; which < setters.size(); ++which)
        {
            const auto n = static_cast<std::size_t>(rate / 2.0);
            const auto x = saw(220.0, rate, n);
            auto built = makeBuilt<dsp::TanhSaturator>(rate, 1000.0, 4.4f, 1.0f);
            Buffer buffer{1, n};
            std::ranges::copy(x, buffer.channel(0).begin());
            built.process(buffer.block().subBlock(0, n / 2));
            switch (which)
            {
            case 0:
                built.setCutoff(bad);
                break;
            case 1:
                built.setCutoffModulation(bad);
                break;
            case 2:
                built.setResonance(bad);
                break;
            default:
                built.setDrive(bad);
                break;
            }
            built.process(buffer.block().subBlock(n / 2, n - (n / 2)));
            const auto o = outcomeOf(buffer.channel(0));
            std::cout << "| " << fixed(bad, 0) << " | " << setters[which] << " | " << o.nonFinite << " | "
                      << (o.nonFinite == 0 ? "-" : "no") << " | | | |\n";
        }
    }

    // Blocks of 1 at the corner.
    std::cout << "\n**Block sizes at the corner** (0.495 fs, k 8, an impulse then a saw, 1 s): blocks of 1 and 37 "
                 "against 4096, bit for bit.\n\n| Saturator | As built 1 / 37 |\n|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            auto x = saw(110.0, rate, static_cast<std::size_t>(rate));
            x[0] = 0.5f;
            const auto render = [&](std::size_t block)
            {
                auto f = makeBuilt<Sat>(rate, 0.495 * rate, 8.0f, 1.0f);
                return renderBuilt(f, x, block);
            };
            const auto reference = render(4096);
            std::cout << "| " << name << " | " << (bitIdentical(render(1), reference) ? "identical" : "DIFFER") << " / "
                      << (bitIdentical(render(37), reference) ? "identical" : "DIFFER") << " |\n";
        });

    // Responses and bypass while self-oscillating.
    std::cout << "\n**Responses and bypass switched while self-oscillating** (as built, k 8, an impulse then "
                 "silence, the response changed every 2,000 samples through all six, bypass toggled every 3,100, "
                 "5 s). Bound: the largest tap-mix sum (16) times the self-oscillation's peak plus the passband.\n\n"
                 "| Saturator | Cutoff | Peak | Non-finite |\n|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            for (const double cutoff : {440.0, 5000.0, 0.44 * rate})
            {
                auto f = makeBuilt<Sat>(rate, cutoff, 8.0f, 1.0f);
                const auto n = static_cast<std::size_t>(5.0 * rate);
                Buffer buffer{1, n};
                buffer.channel(0)[0] = 0.5f;
                constexpr std::array<FilterResponse, 6> responses{
                    FilterResponse::lp24, FilterResponse::hp24, FilterResponse::bp12,
                    FilterResponse::lp12, FilterResponse::hp12, FilterResponse::bp24,
                };
                std::size_t r = 0;
                for (std::size_t start = 0; start < n; start += 100)
                {
                    if (start % 2000 == 0)
                    {
                        f.setResponse(responses[r++ % responses.size()]);
                    }
                    if (start % 3100 == 0 && start > 0)
                    {
                        f.setBypassed(!f.isBypassed());
                        if (!f.isBypassed())
                        {
                            buffer.channel(0)[start] = 0.5f; // restart it
                        }
                    }
                    f.process(buffer.block().subBlock(start, std::min<std::size_t>(100, n - start)));
                }
                const auto o = outcomeOf(buffer.channel(0));
                std::cout << "| " << name << " | " << fixed(cutoff, 0) << " Hz | " << fixed(o.peak, 2) << " | "
                          << o.nonFinite << " |\n";
            }
        });

    // Five minutes.
    std::cout << "\n**Five minutes of self-oscillation** (an impulse of 0.5, then silence, 48 kHz): level and pitch "
                 "in the last 0.5 s of each minute; the spread across the five minutes.\n\n"
                 "| Filter | Saturator | Cutoff, k | Minute 1 | Minute 5 | Spread (dB / cents) |\n"
                 "|---|---|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            const auto minutes = [&](auto&& process, double cutoff)
            {
                const auto perMinute = static_cast<std::size_t>(60.0 * rate);
                std::vector<float> minute(perMinute);
                std::vector<Behaviour> b;
                for (int m = 0; m < 5; ++m)
                {
                    std::ranges::fill(minute, 0.0f);
                    if (m == 0)
                    {
                        minute[0] = 0.5f;
                    }
                    process(minute);
                    b.push_back(behaviourOf(minute, rate, cutoff));
                }
                double lo = b[0].levelDb;
                double hi = lo;
                double clo = b[0].cents;
                double chi = clo;
                for (const auto& v : b)
                {
                    lo = std::min(lo, v.levelDb);
                    hi = std::max(hi, v.levelDb);
                    clo = std::min(clo, v.cents);
                    chi = std::max(chi, v.cents);
                }
                return std::tuple{b.front(), b.back(), hi - lo, chi - clo};
            };
            for (const float k : {4.4f, 8.0f})
            {
                auto built = makeBuilt<Sat>(rate, 440.0, k, 1.0f);
                const auto [first, last, db, cents] = minutes(
                    [&](std::vector<float>& v)
                    {
                        Buffer buffer{1, v.size()};
                        std::ranges::copy(v, buffer.channel(0).begin());
                        dsp::processInChunks(built, buffer.block(), blockSize);
                        std::ranges::copy(buffer.channel(0), v.begin());
                    },
                    440.0);
                std::cout << "| As built | " << name << " | 440 Hz, " << fixed(k, 1) << " | " << describe(first)
                          << " | " << describe(last) << " | " << fixed(db, 3) << " / " << fixed(cents, 3) << " |\n";
            }
            auto chosen = makeCandidate<Sat, Chosen>(rate, 0.495 * rate, 8.0f, 1.0f);
            const auto [first, last, db, cents] = minutes(
                [&](std::vector<float>& v)
                {
                    for (auto& s : v)
                    {
                        s = chosen.processSample(0, s);
                    }
                },
                0.495 * rate);
            std::cout << "| Chosen | " << name << " | 0.495 fs, 8.0 | " << describe(first) << " | " << describe(last)
                      << " | " << fixed(db, 3) << " / " << fixed(cents, 3) << " |\n";
        });
}

// --- 7. Aliasing ------------------------------------------------------------------------------------------

/// A filter inside the Oversampler (or not), for any BlockProcessor.
template <typename Inner>
std::vector<float> renderOversampled(Inner& inner, OversamplingFactor factor, double rate, std::span<const float> x)
{
    dsp::Oversampler oversampler{factor};
    oversampler.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = blockSize, .numChannels = 1});
    inner.prepare(oversampler.oversampledSpec());
    Buffer buffer{1, x.size()};
    std::ranges::copy(x, buffer.channel(0).begin());
    for (std::size_t start = 0; start < x.size(); start += blockSize)
    {
        const auto block = buffer.block().subBlock(start, std::min(blockSize, x.size() - start));
        if (factor == OversamplingFactor::none)
        {
            inner.process(block);
        }
        else
        {
            inner.process(oversampler.upsample(block));
            oversampler.downsample(block);
        }
    }
    return {buffer.channel(0).begin(), buffer.channel(0).end()};
}

std::string_view factorName(OversamplingFactor f)
{
    switch (f)
    {
    case OversamplingFactor::two:
        return "2x";
    case OversamplingFactor::four:
        return "4x";
    default:
        return "1x";
    }
}

void reportAliasing()
{
    constexpr double rate = 44100.0;
    constexpr std::size_t n = 65536;
    constexpr std::size_t settle = 16384;
    const double f0 = coherentFrequency(2500.0, rate, n);
    std::cout << "\n### Aliasing at the developer's corner (LP24 at 5 kHz, k 3.9, a 2.5 kHz sine at -6 dBFS, "
                 "44.1 kHz)\n\nThe strongest single alias below 20 kHz, dB re the fundamental / all aliasing, dB re "
                 "the harmonics. As built (the solver converges here).\n\n"
                 "| Filter | Drive 1 | 4 | 16 | 32 |\n|---|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            for (const auto factor : {OversamplingFactor::none, OversamplingFactor::two, OversamplingFactor::four})
            {
                std::cout << "| " << name << ", " << factorName(factor);
                for (const float drive : {1.0f, 4.0f, 16.0f, 32.0f})
                {
                    LadderFilter f;
                    f.setWarm(std::is_same_v<Sat, dsp::TanhSaturator>);
                    f.setCutoff(5000.0f);
                    f.setResonance(3.9f);
                    f.setDrive(drive);
                    const auto y = renderOversampled(f, factor, rate, sine(f0, rate, settle + n, 0.5));
                    const auto a =
                        measureAliasing(std::span{y}.subspan(settle, n), f0, rate, rectangularWindow(n), 20000.0);
                    std::cout << " | " << fixed(a.worstAliasDb, 1) << " / " << fixed(a.aliasToSignalDb, 1);
                }
                std::cout << " |\n";
            }
        });

    std::cout << "\n### Aliasing of the self-oscillation (k 6, Resonance 10 under F5; an impulse, no input, "
                 "44.1 kHz)\n\nThe oscillation's own harmonics folded back: the strongest non-harmonic component "
                 "below 20 kHz, dB re the oscillation / all of them, dB re its harmonics (Blackman-Harris, 65,536 "
                 "samples after 1 s), at the oscillation's measured pitch. The chosen solver (20.5 kHz is F3's top: "
                 "0.465 fs, where the as-built solver "
                 "fails at k 6).\n\n| Filter | Cutoff 5 kHz | 10 kHz | 15 kHz | 20.5 kHz |\n|---|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            for (const auto factor : {OversamplingFactor::none, OversamplingFactor::two, OversamplingFactor::four})
            {
                std::cout << "| " << name << ", " << factorName(factor);
                for (const double cutoff : {5000.0, 10000.0, 15000.0, 20500.0})
                {
                    CandidateFilter<Sat, Chosen> f;
                    f.setResonance(6.0f);
                    f.setCutoff(static_cast<float>(cutoff));
                    const auto x = kick(44100 + n);
                    const auto y = renderOversampled(f, factor, rate, x); // prepare() sets g at the inner rate
                    const auto tail = std::span{y}.last(n);
                    const double fosc = peakHz(tail, rate);
                    const auto a = measureAliasing(tail, fosc, rate, blackmanHarrisWindow(n), 20000.0);
                    std::cout << " | " << fixed(a.worstAliasDb, 1) << " / " << fixed(a.aliasToSignalDb, 1) << " at "
                              << fixed(fosc, 0) << " Hz";
                }
                std::cout << " |\n";
            }
        });
}

/// ADAA on the loop's input saturator, as an option: aliasing, and what it
/// does to the response at the cutoff, the threshold, and the pitch.
void reportAdaa()
{
    constexpr double rate = 44100.0;
    constexpr std::size_t n = 65536;
    constexpr std::size_t settle = 16384;
    const double f0 = coherentFrequency(2500.0, rate, n);
    std::cout << "\n### First-order ADAA on the loop's input saturator (an option)\n\n"
                 "1x, 44.1 kHz, a 2.5 kHz sine at -6 dBFS: the strongest alias (dB re the fundamental). Then the "
                 "small-signal gain at the cutoff (LP24, k 0, against the as-built 1/4), the threshold (the "
                 "smallest k that grows a 1e-15 impulse), and the self-oscillation pitch at k 4.4.\n\n"
                 "| Solver | Saturator | k 0, 15 kHz: drive 4 / 16 | k 3.9, 5 kHz: drive 4 / 16 | Gain at a 1 / 10 kHz "
                 "cutoff (dB re as built) | Threshold at 1 / 7 kHz | Pitch at k 4.4, 440 Hz / 7 kHz (cents) |\n"
                 "|---|---|---|---|---|---|---|\n";
    const auto row = [&]<typename Sat, typename Solver>(const char* label, const char* name)
    {
        std::cout << "| " << label << " | " << name;
        for (const auto& [k, cutoff] : {std::pair{0.0f, 15000.0}, std::pair{3.9f, 5000.0}})
        {
            std::cout << " | ";
            for (const float drive : {4.0f, 16.0f})
            {
                auto f = makeCandidate<Sat, Solver>(rate, cutoff, k, drive);
                const auto y = renderSamples(f, sine(f0, rate, settle + n, 0.5));
                const auto a =
                    measureAliasing(std::span{y}.subspan(settle, n), f0, rate, rectangularWindow(n), 20000.0);
                std::cout << fixed(a.worstAliasDb, 1) << (drive < 8.0f ? " / " : "");
            }
        }
        std::cout << " | ";
        for (const double cutoff : {1000.0, 10000.0})
        {
            const MonoProcess candidate = [&](std::span<const float> in)
            {
                auto f = makeCandidate<Sat, Solver>(rate, cutoff, 0.0f, 1.0f);
                return renderSamples(f, in);
            };
            std::cout << fixed(measureResponseAt(candidate, cutoff, rate, 1e-4).magnitudeDb + 12.0412, 2)
                      << (cutoff < 5000.0 ? " / " : "");
        }
        std::cout << " | ";
        for (const double cutoff : {1000.0, 7000.0})
        {
            const double period = rate / cutoff;
            const auto grows = [&](double k)
            {
                auto f = makeCandidate<Sat, Solver>(rate, cutoff, static_cast<float>(k), 1.0f);
                const auto y = renderSamples(f, kick(static_cast<std::size_t>(40.0 * period) + 1, 1e-15f));
                double early = 0.0;
                double late = 0.0;
                for (auto i = static_cast<std::size_t>(10.0 * period); i < static_cast<std::size_t>(15.0 * period); ++i)
                {
                    early += static_cast<double>(y[i]) * static_cast<double>(y[i]);
                }
                for (auto i = static_cast<std::size_t>(35.0 * period); i < static_cast<std::size_t>(40.0 * period); ++i)
                {
                    late += static_cast<double>(y[i]) * static_cast<double>(y[i]);
                }
                return late > early;
            };
            double lo = 2.0;
            double hi = 8.0;
            for (int i = 0; i < 18; ++i)
            {
                const double mid = 0.5 * (lo + hi);
                (grows(mid) ? hi : lo) = mid;
            }
            std::cout << fixed(0.5 * (lo + hi), 3) << (cutoff < 5000.0 ? " / " : "");
        }
        std::cout << " | ";
        for (const double cutoff : {440.0, 7000.0})
        {
            auto f = makeCandidate<Sat, Solver>(rate, cutoff, 4.4f, 1.0f);
            const auto y = renderSamples(f, kick(static_cast<std::size_t>(2.5 * rate)));
            const auto tail = std::span{y}.last(static_cast<std::size_t>(0.5 * rate));
            std::cout << fixed(1200.0 * std::log2(measureFrequency(tail, rate) / cutoff), 1)
                      << (cutoff < 5000.0 ? " / " : "");
        }
        std::cout << " |\n";
    };
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            row.template operator()<Sat, NewtonAsBuilt<>>("As built", name);
            row.template operator()<Sat, NewtonInputAdaa<>>("ADAA on the input", name);
        });
}

} // namespace

TEST_CASE("Filter stability report: the solver near Nyquist and at extreme settings", "[.][filter-stability-report]")
{
    std::cout << "\n## Filter stability report\n";
    reportUniqueness();
    reportReferenceNearNyquist();
    reportBoundary();
    reportHeldCutoff();
    reportOrdinary();
    reportGrid();
    reportExtremes();
    reportAliasing();
    reportAdaa();
}
