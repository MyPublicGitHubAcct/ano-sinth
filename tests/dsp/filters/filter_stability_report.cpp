// The stability report (M1.3 task 7, docs/dsp/filter-stability.md), as built:
// LadderSolver (Newton as before; where it does not converge, the nested
// bracketing solve from the linearised guess; a non-finite input taken as 0)
// and LadderFilter around it, against the double-precision reference and
// today's Newton solve (ladder_reference.hpp):
//
// - the equations: the solution is unique (checked over random states), and
//   what the reference does with the cutoff held near Nyquist, against the
//   built filter;
// - where the fallback runs, which is where Newton alone fails: the lowest
//   fc/fs, k 3.5 to 64, an impulse and a full-scale saw at drive 16 and 32;
// - the grid of stage 1 (fc/fs 0.30-0.495, k 0-64, drive 1-32, Warm on and
//   off, 44.1-192 kHz): unconverged samples, the work, the distance from the
//   reference solved from the same state (absolute, relative to the
//   equations' scale, and against the bound derived in the tests), and
//   bit-identity with today's Newton wherever that converges;
// - the held cutoff: level and pitch against the reference at 0.44-0.495 fs;
// - the ordinary case: bit-identity with today's Newton on the filter
//   report's material, and the distance from the reference;
// - extreme settings: audio-rate modulation across the clamp (against the
//   reference fed the same g), 1 Hz, DC of +-10 and drive 1000 at k 64, NaN
//   and inf, the setters, blocks of 1 at the corner, responses and bypass
//   switched while self-oscillating, and five minutes of self-oscillation
//   (checked: the suite runs 30 s of it);
// - aliasing at the developer's corner and of the self-oscillation, at 1x,
//   2x, and 4x.
//
// The nine solvers it was chosen from, and ADAA on the loop input, are at
// commit 26cd4ec (filter_stability_candidates.hpp).
//
// Hidden from the default run; run it in an optimised build (a few minutes):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[filter-stability-report]"
//
// CPU: the "[filter-stability]" benchmarks (tools/bench.sh "[filter-stability]").

#include "ladder_reference.hpp"

#include <anoesis/dsp/core/processor.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/filters/ladder_solver.hpp>
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
using namespace anoesis::test::ladder;
using anoesis::dsp::CubicSaturator;
using anoesis::dsp::FilterResponse;
using anoesis::dsp::LadderFilter;
using anoesis::dsp::LadderSolver;
using anoesis::dsp::LadderSolveStats;
using anoesis::dsp::LadderState;
using anoesis::dsp::OversamplingFactor;
using anoesis::dsp::TanhSaturator;

namespace
{

constexpr double pi = std::numbers::pi;
constexpr std::size_t blockSize = 512;
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double epsilon = std::numeric_limits<float>::epsilon();

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
    f.template operator()<TanhSaturator>("Warm on (tanh)");
    f.template operator()<CubicSaturator>("Warm off (cubic)");
}

template <typename Sat>
constexpr bool isTanh = std::is_same_v<Sat, TanhSaturator>;

/// g as LadderFilter computes it at a settled grid point.
float gainFor(double cutoff, double rate)
{
    const auto fs = static_cast<float>(rate);
    const float hz = std::exp2(std::log2(static_cast<float>(cutoff)));
    return std::tan(static_cast<float>(pi) * std::clamp(hz, 1.0f, LadderFilter::maxCutoffRatio * fs) / fs);
}

template <typename Sat>
LadderFilter makeBuilt(double rate, double cutoff, float k, float drive)
{
    LadderFilter f;
    f.setWarm(isTanh<Sat>);
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
    anoesis::dsp::processInChunks(f, buffer.block(), block);
    return {buffer.channel(0).begin(), buffer.channel(0).end()};
}

/// LP24 from a solver at a fixed g and k: the filter's per-sample path with
/// settled controls (drive on the input, y4 out, flushed).
struct SolverRun
{
    std::vector<float> y;
    std::uint64_t fellBack = 0;
    std::uint64_t unconverged = 0;
};

template <typename Sat, bool Today>
SolverRun renderSolver(std::span<const float> x, float drive, std::span<const float> g, float k)
{
    SolverRun run;
    run.y.resize(x.size());
    LadderState state;
    for (std::size_t n = 0; n < x.size(); ++n)
    {
        const float gn = g.size() == 1 ? g[0] : g[n];
        if constexpr (Today)
        {
            const auto r = todaysNewton<Sat>(state, x[n] * drive, gn, k);
            run.y[n] = anoesis::dsp::flushDenormal(r.solution.y[3]);
            run.unconverged += r.converged ? 0U : 1U;
        }
        else
        {
            LadderSolveStats stats;
            const auto r = LadderSolver::solve<Sat>(state, x[n] * drive, gn, k, &stats);
            run.y[n] = anoesis::dsp::flushDenormal(r.y[3]);
            run.fellBack += stats.fellBack ? 1U : 0U;
            run.unconverged += stats.converged ? 0U : 1U;
        }
    }
    return run;
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

bool bitIdentical(std::span<const float> a, std::span<const float> b)
{
    return std::ranges::equal(a, b, [](float p, float q)
                              { return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q); });
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
    return spectrum.frequencyOf(top) + (offset * spectrum.binWidth());
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

/// test (1)'s criterion: level within 0.1 dB, pitch within 1 cent, peak under 1.
bool matches(const Behaviour& b, const Behaviour& reference)
{
    return std::fabs(b.levelDb - reference.levelDb) < 0.1 && std::fabs(b.cents - reference.cents) < 1.0 && b.peak < 1.0;
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

// --- 1. The equations ---------------------------------------------------------------

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
                }
                previous = phi;
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
                         "the RMS) / pitch (cents from the cutoff). Below, LadderFilter's at 0.495 fs.\n\n"
                         "| fc/fs | g | k 4.05 | 4.4 | 5 | 6 | 8 | 16 | 64 |\n|---|---|---|---|---|---|---|---|---|\n";
            for (const double ratio : ratios)
            {
                const float g = gainFor(ratio * rate, rate);
                std::cout << "| " << fixed(ratio, 3) << " | " << fixed(g, 2);
                for (const float k : ks)
                {
                    std::cout << " | "
                              << describe(behaviourOf(renderReference<Sat>(x, 1.0f, g, k), rate, ratio * rate));
                }
                std::cout << " |\n";
            }
            std::cout << "| **LadderFilter, 0.495** | |";
            for (const float k : ks)
            {
                auto f = makeBuilt<Sat>(rate, 0.495 * rate, k, 1.0f);
                std::cout << " " << describe(behaviourOf(renderBuilt(f, x), rate, 0.495 * rate)) << " |";
            }
            std::cout << "\n";
        });
}

// --- 2. Where the fallback runs ---------------------------------------------------------

/// The lowest fc/fs (0.0025 to 0.495 in steps of 0.0025) at which the built
/// solve falls back (Newton alone leaves the sample unconverged), or 0 if
/// none; and whether any sample there ended unconverged.
template <typename Sat>
std::pair<double, std::uint64_t> lowestFallback(double rate, float k, float drive, std::span<const float> x)
{
    for (int step = 1; step <= 198; ++step)
    {
        const double ratio = 0.0025 * step;
        const std::array<float, 1> g{gainFor(ratio * rate, rate)};
        const auto run = renderSolver<Sat, false>(x, drive, g, k);
        if (run.fellBack > 0)
        {
            return {ratio, run.unconverged};
        }
    }
    return {0.0, 0};
}

void reportFallbackBoundary()
{
    constexpr double rate = 48000.0;
    constexpr std::array<float, 12> ks{3.5f, 4.05f, 4.4f, 5.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f, 48.0f, 64.0f};
    const auto impulse = kick(12000);
    const auto loud = saw(110.0, rate, 12000, 1.0f);
    std::cout << "\n### Where the fallback runs\n\n"
                 "The lowest fc/fs (from 0.0025 in steps of 0.0025) with a sample where Newton does not converge in "
                 "8 iterations, so the nested solve runs: the as-built solver's failure boundary (stage 1), now "
                 "where the fallback takes over. 48 kHz, 0.25 s (- : none up to 0.495). Impulse: 0.5 at drive 1. "
                 "Saw: a full-scale 110 Hz saw at drive 16 (F8's +24 dB) and 32.\n\n"
                 "| Saturator | Input | k 3.5 | 4.05 | 4.4 | 5 | 6 | 8 | 12 | 16 | 24 | 32 | 48 | 64 |\n"
                 "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    std::uint64_t unconverged = 0;
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
                    const auto [ratio, bad] =
                        lowestFallback<Sat>(rate, k, isSaw ? drive : 1.0f,
                                            isSaw ? std::span<const float>{loud} : std::span<const float>{impulse});
                    unconverged += bad;
                    std::cout << " | " << (ratio > 0.0 ? fixed(ratio, 4) : "-");
                }
                std::cout << " |\n";
            }
        });
    CHECK(unconverged == 0);
    std::cout << "\nUnconverged samples after the fallback, at those boundaries: " << unconverged << ".\n";
}

// --- 3. The grid ---------------------------------------------------------------------------------

constexpr std::array<double, 11> gridRatios{0.30, 0.35, 0.40, 0.42, 0.44, 0.45, 0.46, 0.47, 0.48, 0.49, 0.495};
constexpr std::array<float, 11> gridKs{0.0f, 2.0f, 3.9f, 4.05f, 4.4f, 5.0f, 6.0f, 8.0f, 16.0f, 32.0f, 64.0f};
constexpr std::array<float, 4> gridDrives{1.0f, 4.0f, 16.0f, 32.0f};
constexpr std::array<double, 4> gridRates{44100.0, 48000.0, 96000.0, 192000.0};
constexpr std::size_t gridLength = 2048;

struct GridTotals
{
    std::uint64_t samples = 0;
    std::uint64_t unconverged = 0;
    std::uint64_t fellBack = 0;
    std::uint64_t newtonIterations = 0;
    std::uint64_t rows = 0;
    std::uint32_t maxRows = 0;
    std::uint64_t stageEvaluations = 0;
    std::uint32_t maxStageEvaluations = 0;
    std::uint64_t newtonConverged = 0; ///< samples where today's Newton converges from the same state
    std::uint64_t differ = 0;          ///< ... and the built solve differs from it
    std::size_t cells = 0;
    std::size_t cellsOff = 0; ///< some sample more than 1e-4 of its equations' scale from the reference
    double worstDistance = 0.0;
    double worstRelative = 0.0;
    double worstOfBound = 0.0; ///< the largest error as a fraction of the derived bound
    std::string worstCell;
};

bool sameSolution(const anoesis::dsp::LadderSolution& a, const LadderState& sa, const anoesis::dsp::LadderSolution& b,
                  const LadderState& sb)
{
    const auto same = [](float p, float q)
    {
        return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q);
    };
    bool identical = same(a.u, b.u) && same(sa.previousU, sb.previousU);
    for (std::size_t i = 0; i < 4; ++i)
    {
        identical = identical && same(a.y[i], b.y[i]) && same(sa.s[i], sb.s[i]);
    }
    return identical;
}

template <typename Sat>
GridTotals runGrid()
{
    using R = ReferenceOf<Sat>::Type;
    GridTotals t;
    for (const double rate : gridRates)
    {
        const auto x = saw(110.0, rate, gridLength);
        for (const double ratio : gridRatios)
        {
            const float g = gainFor(ratio * rate, rate);
            const double gd = g;
            for (const float k : gridKs)
            {
                const double kd = k;
                for (const float drive : gridDrives)
                {
                    LadderState state;
                    double worstRelative = 0.0;
                    for (const float v : x)
                    {
                        const std::array<double, 4> s{state.s[0], state.s[1], state.s[2], state.s[3]};
                        auto today = state;
                        const auto newton = todaysNewton<Sat>(today, v * drive, g, k);
                        LadderSolveStats stats;
                        const auto solution = LadderSolver::solve<Sat>(state, v * drive, g, k, &stats);
                        ++t.samples;
                        t.unconverged += stats.converged ? 0U : 1U;
                        t.fellBack += stats.fellBack ? 1U : 0U;
                        t.newtonIterations += stats.newtonIterations;
                        t.rows += stats.rows;
                        t.maxRows = std::max(t.maxRows, stats.rows);
                        t.stageEvaluations += stats.stageEvaluations;
                        t.maxStageEvaluations = std::max(t.maxStageEvaluations, stats.stageEvaluations);
                        if (newton.converged)
                        {
                            ++t.newtonConverged;
                            t.differ += sameSolution(solution, state, newton.solution, today) ? 0U : 1U;
                        }
                        const std::array<double, 4> guess{solution.y[0], solution.y[1], solution.y[2], solution.y[3]};
                        const auto r = referenceSolve<R>(s, static_cast<double>(v * drive), gd, kd, guess);
                        double scale = 1.0 + (2.0 * gd);
                        for (const double si : s)
                        {
                            scale = std::max(scale, 1.0 + std::fabs(si) + (2.0 * gd));
                        }
                        // The bound of the suite's grid test (ladder_stability_test.cpp).
                        const double residual = 8.0 * epsilon * scale;
                        const double tau = stats.fellBack ? static_cast<double>(LadderSolver::newtonTolerance) : 0.0;
                        std::array<double, 4> q{};
                        for (std::size_t i = 0; i < 4; ++i)
                        {
                            const double d = 1.0 + (gd * R::slope(r.y[i]));
                            q[i] = i == 0 ? gd * kd * R::slope(r.u) / d : q[i - 1] * gd * R::slope(r.y[i - 1]) / d;
                        }
                        for (std::size_t i = 0; i < 4; ++i)
                        {
                            const double error = std::fabs(static_cast<double>(solution.y[i]) - r.y[i]);
                            const double bound = (static_cast<double>(i + 1) * residual) +
                                                 (q[i] * ((4.0 * residual / (1.0 + q[3])) + tau));
                            t.worstDistance = std::max(t.worstDistance, error);
                            worstRelative = std::max(worstRelative, error / scale);
                            t.worstOfBound = std::max(t.worstOfBound, error / bound);
                        }
                    }
                    ++t.cells;
                    t.cellsOff += worstRelative > 1e-4 ? 1U : 0U;
                    if (worstRelative > t.worstRelative)
                    {
                        t.worstRelative = worstRelative;
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
        << "\n### The grid\n\n"
           "Stage 1's grid: fc/fs 0.30, 0.35, 0.40, 0.42, 0.44-0.49 by 0.01, 0.495; k 0, 2, 3.9, 4.05, 4.4, 5, 6, 8, "
           "16, 32, 64; drive 1, 4, 16, 32; 44.1, 48, 96, 192 kHz: 1,936 cells per saturator, each a 110 Hz saw at "
           "-6 dBFS (times the drive) for 2,048 samples from rest. Every sample solved by LadderSolver, by today's "
           "Newton, and by the reference, from the same state. Unconverged: the solve's own report. Rows, stage "
           "evaluations: the nested solve's work, per sample over the whole grid (mean / max). Distance: the "
           "largest |y_i - y_ref,i|, absolute and relative to the equations' scale 1 + max|s_i| + 2g; cells off: "
           "a sample more than 1e-4 of that scale away; of the bound: the largest error as a fraction of the "
           "per-sample bound derived in the suite's grid test. Identical: of the samples where today's Newton "
           "converges, those where the built solve is the same to the bit.\n\n"
           "| Saturator | Unconverged | Fell back | Newton iterations (mean) | Rows (mean / max) | Stage evaluations "
           "(mean / max) | Cells off | Distance: absolute / relative | Of the bound | Where (relative) | Today's "
           "Newton converges / identical |\n"
           "|---|---|---|---|---|---|---|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            const auto t = runGrid<Sat>();
            const auto samples = static_cast<double>(t.samples);
            CHECK(t.unconverged == 0);
            CHECK(t.differ == 0);
            CHECK(t.worstOfBound <= 1.0);
            std::cout << "| " << name << " | " << t.unconverged << " | "
                      << fixed(100.0 * static_cast<double>(t.fellBack) / samples, 2) << "% | "
                      << fixed(static_cast<double>(t.newtonIterations) / samples, 2) << " | "
                      << fixed(static_cast<double>(t.rows) / samples, 2) << " / " << t.maxRows << " | "
                      << fixed(static_cast<double>(t.stageEvaluations) / samples, 2) << " / " << t.maxStageEvaluations
                      << " | " << t.cellsOff << " | " << scientific(t.worstDistance) << " / "
                      << scientific(t.worstRelative) << " | " << fixed(t.worstOfBound, 3) << " | " << t.worstCell
                      << " | " << t.newtonConverged << " / " << (t.newtonConverged - t.differ) << " |\n";
        });
}

// --- 4. The held cutoff -----------------------------------------------------------------------------

void reportHeldCutoff()
{
    constexpr std::array<float, 7> ks{4.05f, 4.4f, 5.0f, 6.0f, 8.0f, 16.0f, 64.0f};
    int total = 0;
    int matched = 0;
    const auto x48 = kick(static_cast<std::size_t>(1.5 * 48000.0));
    for (const double rate : {48000.0, 44100.0})
    {
        const auto x = kick(static_cast<std::size_t>(1.5 * rate));
        forBothSaturators(
            [&]<typename Sat>(const char* name)
            {
                const double cutoff = 0.495 * rate;
                std::cout << "\n### The held cutoff at 0.495 fs, " << name << ", " << fixed(rate / 1000.0, 1)
                          << " kHz\n\nAn impulse of 0.5; level (dBFS) / pitch (cents from the cutoff) over the last "
                             "0.5 s of 1.5 s, the peak when above 2.\n\n"
                             "| Model | k 4.05 | 4.4 | 5 | 6 | 8 | 16 | 64 | Matches "
                             "|\n|---|---|---|---|---|---|---|---|---|\n";
                const float g = gainFor(cutoff, rate);
                std::array<Behaviour, ks.size()> reference{};
                std::cout << "| Reference (double)";
                for (std::size_t i = 0; i < ks.size(); ++i)
                {
                    reference[i] = behaviourOf(renderReference<Sat>(x, 1.0f, g, ks[i]), rate, cutoff);
                    std::cout << " | " << describe(reference[i]);
                }
                std::cout << " | |\n| **LadderFilter**";
                int m = 0;
                for (std::size_t i = 0; i < ks.size(); ++i)
                {
                    auto f = makeBuilt<Sat>(rate, cutoff, ks[i], 1.0f);
                    const auto b = behaviourOf(renderBuilt(f, x), rate, cutoff);
                    m += matches(b, reference[i]) ? 1 : 0;
                    std::cout << " | " << describe(b);
                }
                total += static_cast<int>(ks.size());
                matched += m;
                std::cout << " | " << m << " of " << ks.size() << " |\n";
            });
    }
    std::cout << "\n**Lower held cutoffs** (48 kHz): the number of k (of 4.05, 4.4, 5, 6, 8, 16, 64) where "
                 "LadderFilter matches the reference (level within 0.1 dB, pitch within 1 cent, peak under 1).\n\n"
                 "| Saturator | 0.44 | 0.46 | 0.48 |\n|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            std::cout << "| " << name;
            for (const double ratio : {0.44, 0.46, 0.48})
            {
                int m = 0;
                for (const float k : ks)
                {
                    const auto reference =
                        behaviourOf(renderReference<Sat>(x48, 1.0f, gainFor(ratio * 48000.0, 48000.0), k), 48000.0,
                                    ratio * 48000.0);
                    auto f = makeBuilt<Sat>(48000.0, ratio * 48000.0, k, 1.0f);
                    m += matches(behaviourOf(renderBuilt(f, x48), 48000.0, ratio * 48000.0), reference) ? 1 : 0;
                }
                total += static_cast<int>(ks.size());
                matched += m;
                std::cout << " | " << m;
            }
            std::cout << " |\n";
        });
    CHECK(matched == total);
    std::cout << "\nMatched " << matched << " of " << total << ".\n";
}

// --- 5. The ordinary case ---------------------------------------------------------------------------

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

/// The filter report's solver-distance sweep's g per sample: the cutoff
/// 100 Hz -> 8 kHz -> 100 Hz (in octaves) over 2 s, 44.1 kHz.
std::vector<float> sweepGains()
{
    constexpr double rate = 44100.0;
    const auto n = static_cast<std::size_t>(2.0 * rate);
    std::vector<float> g(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double phase = static_cast<double>(i) / static_cast<double>(n);
        const double octaves = std::log2(80.0) * (1.0 - std::fabs((2.0 * phase) - 1.0));
        g[i] = gainFor(100.0 * std::exp2(octaves), rate);
    }
    return g;
}

void reportOrdinary()
{
    std::cout << "\n### The ordinary case\n\n"
                 "The filter report's material through the built solve and through today's Newton: the THD table's "
                 "220 Hz sine at -6 dBFS through LP24 at 10 kHz, k 0, drive 1, 2, 4, 8, 16 (48 kHz), and the "
                 "solver-distance sweeps (a 110 Hz saw at -6 dBFS, the cutoff 100 Hz -> 8 kHz -> 100 Hz over 2 s, "
                 "44.1 kHz), hard (k 3.8, drive 4) and gentle (k 2, drive 2). Identical: bit for bit. Against the "
                 "reference: the error against the double-precision ladder on its own trajectory.\n\n"
                 "| Saturator | Harmonics, 5 drives | Hard sweep | Gentle sweep | Fell back | Against the reference, "
                 "hard / gentle (dB) |\n|---|---|---|---|---|---|\n";
    const auto gains = sweepGains();
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            int identical = 0;
            std::uint64_t fellBack = 0;
            const std::array<float, 1> harmonicsGain{gainFor(10000.0, 48000.0)};
            const auto sine220 = sine(coherentFrequency(220.0, 48000.0, 65536), 48000.0, 8192 + 65536, 0.5);
            for (const float drive : {1.0f, 2.0f, 4.0f, 8.0f, 16.0f})
            {
                const auto built = renderSolver<Sat, false>(sine220, drive, harmonicsGain, 0.0f);
                fellBack += built.fellBack;
                identical +=
                    bitIdentical(built.y, renderSolver<Sat, true>(sine220, drive, harmonicsGain, 0.0f).y) ? 1 : 0;
            }
            const auto x = saw(110.0, 44100.0, gains.size());
            const auto hard = renderSolver<Sat, false>(x, 4.0f, gains, 3.8f);
            const auto gentle = renderSolver<Sat, false>(x, 2.0f, gains, 2.0f);
            fellBack += hard.fellBack + gentle.fellBack;
            const bool hardSame = bitIdentical(hard.y, renderSolver<Sat, true>(x, 4.0f, gains, 3.8f).y);
            const bool gentleSame = bitIdentical(gentle.y, renderSolver<Sat, true>(x, 2.0f, gains, 2.0f).y);
            CHECK(identical == 5);
            CHECK(hardSame);
            CHECK(gentleSame);
            CHECK(fellBack == 0);
            using R = ReferenceOf<Sat>::Type;
            const auto referenceSweep = [&](float k, float drive)
            {
                ReferenceLadder<R> ladder;
                std::vector<float> y(x.size());
                for (std::size_t i = 0; i < x.size(); ++i)
                {
                    y[i] = static_cast<float>(ladder.process(static_cast<double>(x[i] * drive), gains[i], k));
                }
                return y;
            };
            std::cout << "| " << name << " | " << (identical == 5 ? "identical" : std::to_string(identical) + " of 5")
                      << " | " << (hardSame ? "identical" : "DIFFER") << " | " << (gentleSame ? "identical" : "DIFFER")
                      << " | " << fellBack << " | " << fixed(errorDb(hard.y, referenceSweep(3.8f, 4.0f), 4410), 1)
                      << " / " << fixed(errorDb(gentle.y, referenceSweep(2.0f, 2.0f), 4410), 1) << " |\n";
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

void reportModulation()
{
    constexpr double rate = 48000.0;
    constexpr std::size_t grid = LadderFilter::gridSamples;
    const auto n = static_cast<std::size_t>(rate);
    const auto x = saw(110.0, rate, n, 1.0f);
    const auto modulation = [](std::size_t sample)
    {
        return static_cast<float>(1.5 * std::sin(2.0 * pi * 300.0 * static_cast<double>(sample) / rate));
    };
    const auto gainAt = [](float octaves)
    {
        const auto fs = static_cast<float>(rate);
        const float hz = std::exp2(std::clamp(std::log2(12000.0f) + octaves, -40.0f, 40.0f));
        return std::tan(static_cast<float>(pi) * std::clamp(hz, 1.0f, LadderFilter::maxCutoffRatio * fs) / fs);
    };
    std::cout << "\n### Extreme settings\n\nLadderFilter as built, 48 kHz.\n\n"
                 "**Cutoff modulation across the clamp**: a full-scale 110 Hz saw; the cutoff 12 kHz plus "
                 "1.5 sin(2 pi 300 t) octaves through the modulation input every 32 samples (to 34 kHz, clamped at "
                 "0.495 fs), 1 s. The peak; the reference's, fed the same g (the filter's grid replayed into the "
                 "solver, bit for bit); unconverged samples; and the fixed-cutoff bound drive max|x| / (1 + k) + 1, "
                 "which does not hold for a cutoff moving at audio rate.\n\n"
                 "| Saturator | k | Drive | Peak | Reference's peak | Unconverged | Fixed-cutoff bound |\n"
                 "|---|---|---|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            using R = ReferenceOf<Sat>::Type;
            for (const float k : {0.0f, 4.4f, 8.0f, 16.0f, 64.0f})
            {
                for (const float drive : {1.0f, 32.0f})
                {
                    auto f = makeBuilt<Sat>(rate, 12000.0, k, drive);
                    Buffer buffer{1, n};
                    std::ranges::copy(x, buffer.channel(0).begin());
                    for (std::size_t start = 0; start < n; start += grid)
                    {
                        f.setCutoffModulation(modulation(start));
                        f.process(buffer.block().subBlock(start, grid));
                    }
                    LadderState state;
                    ReferenceLadder<R> reference;
                    std::vector<float> replay(n);
                    double referencePeak = 0.0;
                    std::uint64_t unconverged = 0;
                    float segmentStart = gainAt(0.0f);
                    for (std::size_t start = 0; start < n; start += grid)
                    {
                        const float segmentEnd = gainAt(modulation(start));
                        const float step = (segmentEnd - segmentStart) * (1.0f / static_cast<float>(grid));
                        for (std::size_t i = 0; i < grid; ++i)
                        {
                            const float g = segmentStart + (step * static_cast<float>(i));
                            LadderSolveStats stats;
                            const auto r = LadderSolver::solve<Sat>(state, x[start + i] * drive, g, k, &stats);
                            unconverged += stats.converged ? 0U : 1U;
                            replay[start + i] = anoesis::dsp::flushDenormal(r.y[3]);
                            referencePeak =
                                std::max(referencePeak,
                                         std::fabs(reference.process(static_cast<double>(x[start + i] * drive), g, k)));
                        }
                        segmentStart = segmentEnd;
                    }
                    const auto o = outcomeOf(buffer.channel(0));
                    CHECK(bitIdentical(replay, buffer.channel(0)));
                    CHECK(unconverged == 0);
                    CHECK(std::fabs(o.peak - referencePeak) <= 0.01 * referencePeak);
                    std::cout << "| " << name << " | " << fixed(k, 1) << " | " << fixed(drive, 0) << " | " << show(o)
                              << " | " << fixed(referencePeak, 3) << " | " << unconverged << " | "
                              << fixed((drive / (1.0 + k)) + 1.0, 2) << " |\n";
                }
            }
        });
}

void reportCeilings()
{
    constexpr double rate = 48000.0;
    std::cout << "\n**Low cutoff, DC, and the ceilings**: 2 s each. DC: 0 -> +10 -> -10 in thirds. Saw: full scale, "
                 "110 Hz. The bound, with the cutoff fixed: the loop's DC gain plus a self-oscillation under "
                 "0 dBFS, drive x max|x| / (1 + k) + 1.\n\n"
                 "| Case | Saturator | Peak | Bound |\n|---|---|---|---|\n";
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
                auto f = makeBuilt<Sat>(rate, c.cutoff, c.k, c.drive);
                const double bound = (static_cast<double>(c.drive) * c.maxInput / (1.0 + c.k)) + 1.0;
                const auto o = outcomeOf(renderBuilt(f, *c.input));
                CHECK(o.nonFinite == 0);
                CHECK(o.peak < bound);
                std::cout << "| " << c.label << " | " << name << " | " << show(o) << " | " << fixed(bound, 2) << " |\n";
            }
            auto oneHz = makeBuilt<Sat>(rate, 1.0, 64.0f, 1.0f);
            const auto b =
                behaviourOf(renderBuilt(oneHz, kick(static_cast<std::size_t>(20.0 * rate))), rate, 1.0, 10.0);
            std::cout << "| 1 Hz, k 64, an impulse: the last 10 s of 20 | " << name << " | level "
                      << fixed(b.levelDb, 1) << " dBFS, peak " << fixed(b.peak, 3) << " | |\n";
        });
}

void reportNonFinite()
{
    constexpr double rate = 48000.0;
    std::cout << "\n**NaN and inf.** A 220 Hz saw at -6 dBFS, k 4.4, 1 kHz and 0.495 fs, Warm on, with one bad "
                 "sample at 1000 of 1 s: non-finite outputs, and whether the output is the same input's with 0 "
                 "there, bit for bit. Setters: each set to the bad value mid-run.\n\n"
                 "| Bad value | Where | Non-finite outputs | Same as 0 there |\n|---|---|---|---|\n";
    for (const float bad : badValues)
    {
        for (const double cutoff : {1000.0, 0.495 * rate})
        {
            const auto n = static_cast<std::size_t>(rate);
            auto clean = saw(220.0, rate, n);
            clean[1000] = 0.0f;
            auto x = clean;
            x[1000] = bad;
            auto f = makeBuilt<TanhSaturator>(rate, cutoff, 4.4f, 1.0f);
            auto g = makeBuilt<TanhSaturator>(rate, cutoff, 4.4f, 1.0f);
            const auto y = renderBuilt(f, x);
            const bool same = bitIdentical(y, renderBuilt(g, clean));
            const auto o = outcomeOf(y);
            CHECK(o.nonFinite == 0);
            CHECK(same);
            std::cout << "| " << fixed(bad, 0) << " | input, " << (cutoff < 2000.0 ? "1 kHz" : "0.495 fs") << " | "
                      << o.nonFinite << " | " << (same ? "yes" : "NO") << " |\n";
        }
    }
    for (const float bad : badValues)
    {
        constexpr std::array<const char*, 4> setters{"setCutoff", "setCutoffModulation", "setResonance", "setDrive"};
        for (std::size_t which = 0; which < setters.size(); ++which)
        {
            const auto n = static_cast<std::size_t>(rate / 2.0);
            auto f = makeBuilt<TanhSaturator>(rate, 1000.0, 4.4f, 1.0f);
            Buffer buffer{1, n};
            std::ranges::copy(saw(220.0, rate, n), buffer.channel(0).begin());
            f.process(buffer.block().subBlock(0, n / 2));
            switch (which)
            {
            case 0:
                f.setCutoff(bad);
                break;
            case 1:
                f.setCutoffModulation(bad);
                break;
            case 2:
                f.setResonance(bad);
                break;
            default:
                f.setDrive(bad);
                break;
            }
            f.process(buffer.block().subBlock(n / 2, n - (n / 2)));
            const auto o = outcomeOf(buffer.channel(0));
            CHECK(o.nonFinite == 0);
            std::cout << "| " << fixed(bad, 0) << " | " << setters[which] << " | " << o.nonFinite << " | |\n";
        }
    }
}

void reportBlocksAndSwitching()
{
    constexpr double rate = 48000.0;
    std::cout << "\n**Block sizes at the corner** (0.495 fs, k 8, an impulse then a saw, 1 s): blocks of 1 and 37 "
                 "against 4096, bit for bit.\n\n| Saturator | 1 / 37 |\n|---|---|\n";
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
            const bool one = bitIdentical(render(1), reference);
            const bool odd = bitIdentical(render(37), reference);
            CHECK(one);
            CHECK(odd);
            std::cout << "| " << name << " | " << (one ? "identical" : "DIFFER") << " / "
                      << (odd ? "identical" : "DIFFER") << " |\n";
        });

    std::cout << "\n**Responses and bypass switched while self-oscillating** (k 8, an impulse then silence, the "
                 "response changed every 2,000 samples through all six, bypass toggled every 3,100 with an impulse "
                 "on leaving it, 5 s). Bound: the largest tap-mix sum (16) times the self-oscillation's peak "
                 "(under 0.5).\n\n| Saturator | Cutoff | Peak | Non-finite |\n|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            for (const double cutoff : {440.0, 5000.0, 0.44 * rate, 0.495 * rate})
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
                            buffer.channel(0)[start] = 0.5f;
                        }
                    }
                    f.process(buffer.block().subBlock(start, std::min<std::size_t>(100, n - start)));
                }
                const auto o = outcomeOf(buffer.channel(0));
                CHECK(o.nonFinite == 0);
                CHECK(o.peak < 8.0);
                std::cout << "| " << name << " | " << fixed(cutoff, 0) << " Hz | " << fixed(o.peak, 2) << " | "
                          << o.nonFinite << " |\n";
            }
        });
}

void reportFiveMinutes()
{
    constexpr double rate = 48000.0;
    std::cout << "\n**Five minutes of self-oscillation** (an impulse of 0.5, then silence, 48 kHz): level and pitch "
                 "of the last 0.5 s of each minute; the spread across the five (decided: under 0.05 dB and 0.01 "
                 "cent at the corner).\n\n"
                 "| Saturator | Cutoff, k | Minute 1 | Minute 5 | Spread (dB / cents) |\n|---|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            for (const auto& [cutoff, k] :
                 {std::pair{440.0, 4.4f}, std::pair{440.0, 8.0f}, std::pair{0.495 * rate, 8.0f}})
            {
                auto f = makeBuilt<Sat>(rate, cutoff, k, 1.0f);
                const auto perMinute = static_cast<std::size_t>(60.0 * rate);
                std::vector<float> minute(perMinute);
                std::vector<Behaviour> b;
                for (int m = 0; m < 5; ++m)
                {
                    std::ranges::fill(minute, 0.0f);
                    minute[0] = m == 0 ? 0.5f : 0.0f;
                    minute = renderBuilt(f, minute);
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
                if (cutoff > 1000.0)
                {
                    CHECK(hi - lo < 0.05);
                    CHECK(chi - clo < 0.01);
                }
                std::cout << "| " << name << " | " << fixed(cutoff, 0) << " Hz, " << fixed(k, 1) << " | "
                          << describe(b.front()) << " | " << describe(b.back()) << " | " << fixed(hi - lo, 3) << " / "
                          << fixed(chi - clo, 3) << " |\n";
            }
        });
}

// --- 7. Aliasing ------------------------------------------------------------------------------------------

std::vector<float> renderOversampled(LadderFilter& inner, OversamplingFactor factor, double rate,
                                     std::span<const float> x)
{
    anoesis::dsp::Oversampler oversampler{factor};
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
                 "the harmonics.\n\n| Filter | Drive 1 | 4 | 16 | 32 |\n|---|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            for (const auto factor : {OversamplingFactor::none, OversamplingFactor::two, OversamplingFactor::four})
            {
                std::cout << "| " << name << ", " << factorName(factor);
                for (const float drive : {1.0f, 4.0f, 16.0f, 32.0f})
                {
                    LadderFilter f;
                    f.setWarm(isTanh<Sat>);
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
                 "44.1 kHz)\n\nThe strongest non-harmonic component below 20 kHz, dB re the oscillation / all of "
                 "them, dB re its harmonics (Blackman-Harris, 65,536 samples after 1 s), at the oscillation's "
                 "measured pitch. 20.5 kHz is F3's top (0.465 fs).\n\n"
                 "| Filter | Cutoff 5 kHz | 10 kHz | 15 kHz | 20.5 kHz |\n|---|---|---|---|---|\n";
    forBothSaturators(
        [&]<typename Sat>(const char* name)
        {
            for (const auto factor : {OversamplingFactor::none, OversamplingFactor::two, OversamplingFactor::four})
            {
                std::cout << "| " << name << ", " << factorName(factor);
                for (const double cutoff : {5000.0, 10000.0, 15000.0, 20500.0})
                {
                    LadderFilter f;
                    f.setWarm(isTanh<Sat>);
                    f.setResonance(6.0f);
                    f.setCutoff(static_cast<float>(cutoff));
                    const auto y = renderOversampled(f, factor, rate, kick(44100 + n));
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

} // namespace

TEST_CASE("Filter stability report: the solve near Nyquist and at extreme settings, as built",
          "[.][filter-stability-report]")
{
    std::cout << "\n## Filter stability report (as built)\n";
    reportUniqueness();
    reportReferenceNearNyquist();
    reportFallbackBoundary();
    reportHeldCutoff();
    reportOrdinary();
    reportGrid();
    reportModulation();
    reportCeilings();
    reportNonFinite();
    reportBlocksAndSwitching();
    reportFiveMinutes();
    reportAliasing();
}
