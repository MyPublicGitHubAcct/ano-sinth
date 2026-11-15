// Tests for the ladder's solve near Nyquist and at extreme settings
// (LadderSolver, LadderFilter; docs/dsp/filter-stability.md), the thresholds
// decided for M1.3 task 7:
//
// (1) the held cutoff: near Nyquist the filter self-oscillates as the
//     double-precision reference does;
// (2) the grid: the solve converges at every setting, within a bound derived
//     from float rounding and the step tolerance of the reference solved
//     from the same state;
// (3) the extremes: finite and bounded;
// (4) a NaN or inf input sample counts as 0;
// (5) a long run at the corner neither drifts nor grows;
// and, for the decision that the fallback must not touch the ordinary case:
// wherever today's Newton solve converges, the solve is today's, bit for bit.
// Two more pin what the trajectories alone don't reach: the nested solve from
// random states, whose roots lie anywhere in their brackets, and its work at
// the corner (the decided CPU ceiling, as a count of evaluations).
//
// The oracles (the double-precision reference and today's Newton solve) are
// in ladder_reference.hpp. Tolerances are derived in each test.

#include "ladder_reference.hpp"

#include <anoesis/dsp/core/processor.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/filters/ladder_solver.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <type_traits>
#include <vector>

using anoesis::dsp::CubicSaturator;
using anoesis::dsp::LadderFilter;
using anoesis::dsp::LadderSolver;
using anoesis::dsp::LadderSolveStats;
using anoesis::dsp::LadderState;
using anoesis::dsp::TanhSaturator;
using anoesis::test::Buffer;
using namespace anoesis::test::ladder;

namespace
{

constexpr double epsilon = std::numeric_limits<float>::epsilon();

template <typename Sat>
constexpr bool isTanh = std::is_same_v<Sat, TanhSaturator>;

/// g as LadderFilter computes it for a cutoff set before prepare(): log2 and
/// back, clamped to [1 Hz, 0.495 fs], then tan.
float gainOf(double cutoff, double rate)
{
    const auto fs = static_cast<float>(rate);
    const float hz = std::exp2(std::log2(static_cast<float>(cutoff)));
    return std::tan(std::numbers::pi_v<float> * std::clamp(hz, 1.0f, LadderFilter::maxCutoffRatio * fs) / fs);
}

template <typename Sat>
LadderFilter makeFilter(double rate, double cutoff, float k, float drive)
{
    LadderFilter f;
    f.setWarm(isTanh<Sat>);
    f.setCutoff(static_cast<float>(cutoff));
    f.setResonance(k);
    f.setDrive(drive);
    f.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = 4096, .numChannels = 1});
    return f;
}

std::vector<float> render(LadderFilter& f, std::span<const float> x, std::size_t blockSize = 4096)
{
    Buffer buffer{1, x.size()};
    std::ranges::copy(x, buffer.channel(0).begin());
    anoesis::dsp::processInChunks(f, buffer.block(), blockSize);
    return {buffer.channel(0).begin(), buffer.channel(0).end()};
}

std::vector<float> saw(double hz, double rate, std::size_t n, float amplitude)
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

std::vector<float> kick(std::size_t n)
{
    std::vector<float> x(n, 0.0f);
    x[0] = 0.5f;
    return x;
}

bool bitIdentical(std::span<const float> a, std::span<const float> b)
{
    return std::ranges::equal(a, b, [](float p, float q)
                              { return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q); });
}

bool same(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
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

/// A self-oscillation's level (dBFS, peak-equivalent from the RMS), pitch
/// (the strongest spectral peak: Hann, parabolic on log power, since near
/// Nyquist zero crossings can't resolve the period), and peak.
struct Oscillation
{
    double levelDb = 0.0;
    double hz = 0.0;
    double peak = 0.0;
};

constexpr std::size_t measureWindow = 16384;

template <typename T>
Oscillation oscillationOf(std::span<const T> y, double rate)
{
    static const auto window = anoesis::test::hannWindow(measureWindow);
    std::vector<float> tail(measureWindow);
    double sum = 0.0;
    double peak = 0.0;
    for (std::size_t i = 0; i < measureWindow; ++i)
    {
        const auto v = static_cast<double>(y[y.size() - measureWindow + i]);
        tail[i] = static_cast<float>(v);
        sum += v * v;
        peak = std::max(peak, std::fabs(v));
    }
    const auto spectrum = anoesis::test::powerSpectrum(tail, rate, window);
    const auto& p = spectrum.power;
    const auto top = static_cast<std::size_t>(std::ranges::max_element(p) - p.begin());
    double hz = spectrum.frequencyOf(top);
    if (top > 0 && top + 1 < p.size())
    {
        const double a = std::log(p[top - 1]);
        const double b = std::log(p[top]);
        const double c = std::log(p[top + 1]);
        hz += 0.5 * (a - c) / (a - (2.0 * b) + c) * spectrum.binWidth();
    }
    const double rms = std::sqrt(sum / static_cast<double>(measureWindow));
    return {.levelDb = 20.0 * std::log10(rms * std::numbers::sqrt2), .hz = hz, .peak = peak};
}

double cents(double hz, double reference)
{
    return 1200.0 * std::log2(hz / reference);
}

/// Rows bisection would need to shrink the last row's bracket, 4g wide, to
/// the step tolerance: the nested solve must never need more.
std::uint32_t rowBudget(double g)
{
    return static_cast<std::uint32_t>(
        std::ceil(std::log2(4.0 * g / static_cast<double>(LadderSolver::newtonTolerance))));
}

/// Whether the stage equations the solve settles hold to `bound`, evaluated
/// in double with the reference's saturator: all four, or the first three
/// (the nested solve's last row stops on a step, not a residual).
template <typename R>
bool stageResidualsAtRounding(const std::array<float, 4>& y, const std::array<double, 4>& s, double x, double g,
                              double k, bool all, double bound)
{
    double previousT = R::value(x - (k * static_cast<double>(y[3])));
    for (std::size_t i = 0; i < (all ? 4U : 3U); ++i)
    {
        const double t = R::value(static_cast<double>(y[i]));
        const double residual = static_cast<double>(y[i]) - s[i] - (g * (previousT - t));
        if (!(std::fabs(residual) <= bound))
        {
            return false;
        }
        previousT = t;
    }
    return true;
}

} // namespace

TEST_CASE("Ladder stability: near Nyquist the filter self-oscillates as the double-precision reference does",
          "[dsp][filter][stability]")
{
    // An impulse of 0.5, the cutoff held at 0.44-0.495 fs, k 4.05-64, Warm on
    // and off, 44.1 and 48 kHz: LadderFilter against the reference ladder
    // (double, std::tanh, along its own trajectory). Before the fallback the
    // filter locked here at a peak of 50-270.
    //
    // One reference per case serves both rates: the equations see the rate
    // only through g = tan(pi fc/fs), which is the same function of fc/fs at
    // either (in float the two differ by at most 2e-6 relative below the
    // clamp, which moves the pitch by under 0.001 cent), and the input is the
    // same in samples. So the pitch is compared in cycles per sample.
    //
    // Duration: the oscillation grows at a rate set by k - 4. Measured on the
    // reference, a window of 16,384 is within 0.01 dB and 0.05 cents of the
    // steady state from 53,000 samples at k 4.05 and 0.495 fs, from 10,000
    // at k 4.05 and 0.48 fs and 8,000 at k 4.4 and 0.495 fs, and from 4,000
    // or fewer everywhere else: settle 60,000, 12,000, and 6,000.
    //
    // Tolerances. Both are stable limit cycles of the same equations, and the
    // float solve is within (2)'s bound of the exact one each sample (a few
    // 1e-6 of the scale here), which moves the cycle's amplitude and period
    // by as little: far below 0.01 dB and 0.01 cent. What separates the two
    // measurements is the window. Near Nyquist the samples of the
    // oscillation beat against the sampling, and the two runs drift apart in
    // phase, so the same window covers a different part of the beat: in its
    // steady state the reference's own level moves by up to 0.045 dB from
    // one window to the next (k 4.05, 0.495 fs, cubic), its pitch by 0.001
    // cent. 0.1 dB is twice that; 1 cent is the decided figure and holds a
    // wide margin. A failing solve is tens of dB and hundreds of cents off.
    // The peak: every reference oscillation here is below -11 dBFS (a peak
    // near 0.4); under 1 says it is that oscillation and not a lock.
    constexpr std::array<double, 4> ratios{0.44, 0.46, 0.48, 0.495};
    constexpr std::array<float, 7> ks{4.05f, 4.4f, 5.0f, 6.0f, 8.0f, 16.0f, 64.0f};
    const auto check = [&]<typename Sat>
    {
        using R = ReferenceOf<Sat>::Type;
        for (const double ratio : ratios)
        {
            for (const float k : ks)
            {
                std::size_t settle = 6000;
                if (k < 4.1f && ratio > 0.49)
                {
                    settle = 60000;
                }
                else if ((k < 4.1f && ratio > 0.47) || (k < 4.5f && ratio > 0.49))
                {
                    settle = 12000;
                }
                const auto x = kick(settle + measureWindow);
                ReferenceLadder<R> reference;
                std::vector<double> yr(x.size());
                const double gr = gainOf(ratio * 48000.0, 48000.0);
                for (std::size_t n = 0; n < x.size(); ++n)
                {
                    yr[n] = reference.process(static_cast<double>(x[n]), gr, k);
                }
                const auto want = oscillationOf(std::span<const double>{yr}, 1.0);
                for (const double rate : {44100.0, 48000.0})
                {
                    CAPTURE(isTanh<Sat>, ratio, k, rate);
                    auto f = makeFilter<Sat>(rate, ratio * rate, k, 1.0f);
                    const auto got = oscillationOf(std::span<const float>{render(f, x)}, 1.0);
                    CAPTURE(want.levelDb, got.levelDb, want.hz, got.hz, got.peak);
                    CHECK(std::fabs(got.levelDb - want.levelDb) < 0.1);
                    CHECK(std::fabs(cents(got.hz, want.hz)) < 1.0);
                    CHECK(got.peak < 1.0);
                }
            }
        }
    };
    check.operator()<TanhSaturator>();
    check.operator()<CubicSaturator>();
}

TEST_CASE("Ladder stability: the solve converges at every setting, within its derived bound of the reference",
          "[dsp][filter][stability]")
{
    // Over a grid of fc/fs 0.30-0.495, k 0-64, drive 1-32, 44.1 and 192 kHz,
    // Warm on and off (a 110 Hz saw at -6 dBFS times the drive, from rest),
    // each sample solved by LadderSolver and by the reference from the same
    // state. Before the fallback a quarter of these samples were unconverged
    // and up to 31 times their equations' scale off.
    //
    // The bound. Let S = 1 + max|s_i| + 2g: every term of every equation is at
    // most S (|y_i - s_i| <= 2g, |g t| <= g), so float rounds them to about
    // eps S. The float solution y' is the exact solution of the equations
    // perturbed by its residual r, so to first order its error e solves
    // J e = r. With J's structure (the Newton step's algebra: diagonal
    // D_i = 1 + g t'_i, subdiagonal g t'_(i-1), the feedback corner
    // g k t'(u)), e_i = p_i + q_i e_4, where
    //   p_1 = r_1 / D_1,  p_i = (r_i + g t'_(i-1) p_(i-1)) / D_i,
    //   q_1 = g k t'(u) / D_1,  q_i = q_(i-1) g t'_(i-1) / D_i  (magnitudes),
    //   e_4 = p_4 / (1 + q_4).
    // Each link g t'_(i-1) / D_(i-1) is under 1, so |p_i| <= i R for
    // R = max |r_j|, and
    //   |e_i| <= i R + q_i (4 R / (1 + q_4) + tau_f).
    // R: evaluating a residual rounds its four terms (2 eps S); fastTanh is
    // within 2e-7 (1.7 eps) of tanh, in two terms of size g <= S/2
    // (1.7 eps S; the cubic is exact); a stage solve stops at a residual of
    // 4 eps (|c| + g) <= 4 eps S. So R = 8 eps S. A converged Newton step
    // leaves an error quadratic in a step under 1e-5: negligible. tau_f: the
    // nested solve stops its row on a step of at most tau = 1e-5, which may
    // leave y4 up to tau off, and the other stages follow y4 with
    // dy_i/dy4 = q_i; 0 where Newton converged. The q_i are the equations'
    // own sensitivities, computed from the reference's solution.
    //
    // Two checks follow from the same terms. The backward error: each stage
    // equation the solve settles (all four where Newton converged; the first
    // three after the nested solve, whose last row stops on its step) has a
    // residual, evaluated in double, of at most R = 8 eps S. And the work:
    // safeguarded Newton never needs more rows than bisection over the whole
    // bracket [s4 - 2g, s4 + 2g] would, ceil(log2(4g / tau)).
    //
    // 2e-4 of S as one number is not derivable: q_1 = g k / D_1 reaches 4,000
    // at k 64 near Nyquist, where y1 cannot be known in float to better than
    // q_1 eps S. The bound above is per sample: 32 eps (4e-6) of S at k 0,
    // and as wide as the equations' own conditioning where that is poor.
    constexpr std::array<double, 6> ratios{0.30, 0.40, 0.44, 0.46, 0.48, 0.495};
    constexpr std::array<float, 5> ks{0.0f, 4.05f, 6.0f, 16.0f, 64.0f};
    constexpr std::array<float, 3> drives{1.0f, 16.0f, 32.0f};
    constexpr std::size_t samples = 1024;
    const auto check = [&]<typename Sat>
    {
        using R = ReferenceOf<Sat>::Type;
        std::size_t unconverged = 0;
        std::size_t outside = 0;
        std::size_t fellBack = 0;
        std::size_t overRows = 0;
        std::size_t backward = 0;
        double worst = 0.0; // the largest error as a fraction of its bound
        for (const double rate : {44100.0, 192000.0})
        {
            const auto x = saw(110.0, rate, samples, 0.5f);
            for (const double ratio : ratios)
            {
                const float g = gainOf(ratio * rate, rate);
                for (const float k : ks)
                {
                    for (const float drive : drives)
                    {
                        LadderState state;
                        for (const float v : x)
                        {
                            const std::array<double, 4> s{state.s[0], state.s[1], state.s[2], state.s[3]};
                            LadderSolveStats stats;
                            const auto solution = LadderSolver::solve<Sat>(state, v * drive, g, k, &stats);
                            unconverged += stats.converged ? 0U : 1U;
                            fellBack += stats.fellBack ? 1U : 0U;
                            const std::array<double, 4> guess{
                                solution.y[0],
                                solution.y[1],
                                solution.y[2],
                                solution.y[3],
                            };
                            const double gd = g;
                            const double kd = k;
                            const auto r = referenceSolve<R>(s, static_cast<double>(v * drive), gd, kd, guess);
                            double scale = 1.0 + (2.0 * gd);
                            for (const double si : s)
                            {
                                scale = std::max(scale, 1.0 + std::fabs(si) + (2.0 * gd));
                            }
                            const double residual = 8.0 * epsilon * scale;
                            const double tau =
                                stats.fellBack ? static_cast<double>(LadderSolver::newtonTolerance) : 0.0;
                            std::array<double, 4> q{};
                            for (std::size_t i = 0; i < 4; ++i)
                            {
                                const double d = 1.0 + (gd * R::slope(r.y[i]));
                                q[i] = i == 0 ? gd * kd * R::slope(r.u) / d : q[i - 1] * gd * R::slope(r.y[i - 1]) / d;
                            }
                            for (std::size_t i = 0; i < 4; ++i)
                            {
                                const double bound = (static_cast<double>(i + 1) * residual) +
                                                     (q[i] * ((4.0 * residual / (1.0 + q[3])) + tau));
                                const double error = std::fabs(static_cast<double>(solution.y[i]) - r.y[i]);
                                const bool within = error <= bound;
                                outside += within ? 0U : 1U;
                                worst = std::max(worst, error / bound);
                            }
                            overRows += stats.rows > rowBudget(gd) ? 1U : 0U;
                            backward += stageResidualsAtRounding<R>(solution.y, s, static_cast<double>(v * drive), gd,
                                                                    kd, !stats.fellBack, residual)
                                            ? 0U
                                            : 1U;
                        }
                    }
                }
            }
        }
        CAPTURE(isTanh<Sat>, fellBack, worst);
        CHECK(fellBack > 0); // the grid reaches where Newton fails
        CHECK(unconverged == 0);
        CHECK(outside == 0);
        CHECK(backward == 0);
        CHECK(overRows == 0);
    };
    check.operator()<TanhSaturator>();
    check.operator()<CubicSaturator>();
}

TEST_CASE("Ladder stability: wherever today's Newton solve converges, the solve is today's, bit for bit",
          "[dsp][filter][stability]")
{
    // The decision: Newton as built, the nested solve only where Newton does
    // not converge, so nothing changes where it did. Today's solve is
    // restated in ladder_reference.hpp; from the same state, wherever it
    // converges, LadderSolver must return the same solution and leave the
    // same state, to the bit. Over the ordinary range (fc/fs 0.01-0.25) and
    // near Nyquist, k 0-64, drive 1-32.
    constexpr std::array<double, 7> ratios{0.01, 0.1, 0.25, 0.30, 0.40, 0.46, 0.495};
    constexpr std::array<float, 6> ks{0.0f, 3.8f, 4.4f, 8.0f, 16.0f, 64.0f};
    constexpr std::array<float, 3> drives{1.0f, 4.0f, 32.0f};
    const auto check = [&]<typename Sat>
    {
        std::size_t compared = 0;
        std::size_t differ = 0;
        std::size_t comparedNearNyquist = 0;
        for (const double rate : {44100.0, 96000.0})
        {
            const auto x = saw(110.0, rate, 1024, 0.5f);
            for (const double ratio : ratios)
            {
                const float g = gainOf(ratio * rate, rate);
                for (const float k : ks)
                {
                    for (const float drive : drives)
                    {
                        LadderState state;
                        for (const float v : x)
                        {
                            auto today = state;
                            const auto newton = todaysNewton<Sat>(today, v * drive, g, k);
                            const auto solution = LadderSolver::solve<Sat>(state, v * drive, g, k);
                            if (!newton.converged)
                            {
                                continue;
                            }
                            ++compared;
                            comparedNearNyquist += ratio > 0.29 ? 1U : 0U;
                            bool identical =
                                same(solution.u, newton.solution.u) && same(state.previousU, today.previousU);
                            for (std::size_t i = 0; i < 4; ++i)
                            {
                                identical = identical && same(solution.y[i], newton.solution.y[i]) &&
                                            same(state.s[i], today.s[i]);
                            }
                            differ += identical ? 0U : 1U;
                        }
                    }
                }
            }
        }
        CAPTURE(isTanh<Sat>, compared, comparedNearNyquist);
        CHECK(comparedNearNyquist > 10000);
        CHECK(differ == 0);
    };
    check.operator()<TanhSaturator>();
    check.operator()<CubicSaturator>();
}

TEST_CASE("Ladder stability: a NaN or inf input sample counts as 0", "[dsp][filter][stability]")
{
    // One bad sample at 1000 (NaN, +inf, -inf, or a finite sample the drive
    // takes past FLT_MAX) gives exactly the output of the same input with 0
    // there, every sample, bit for bit: the bad value never reaches the
    // state. At an ordinary setting and at the corner, Warm on and off.
    constexpr double rate = 48000.0;
    constexpr std::array<float, 4> bad{
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        1e36f, // finite, times drive 1000 is +inf
    };
    const auto check = [&]<typename Sat>
    {
        for (const double cutoff : {1000.0, 0.495 * rate})
        {
            auto clean = saw(220.0, rate, 4800, 0.5f);
            clean[1000] = 0.0f;
            auto reference = makeFilter<Sat>(rate, cutoff, 4.4f, 1000.0f);
            const auto expected = render(reference, clean, 512);
            for (const float badSample : bad)
            {
                CAPTURE(isTanh<Sat>, cutoff, badSample);
                auto x = clean;
                x[1000] = badSample;
                auto f = makeFilter<Sat>(rate, cutoff, 4.4f, 1000.0f);
                const auto y = render(f, x, 512);
                CHECK(bitIdentical(y, expected));
            }
        }
    };
    check.operator()<TanhSaturator>();
    check.operator()<CubicSaturator>();
}

TEST_CASE("Ladder stability: at the ceilings, with a fixed cutoff, bounded by the loop's DC gain",
          "[dsp][filter][stability]")
{
    // k 64 (the DSP's ceiling), 1 kHz and 0.495 fs, 48 kHz, 2 s: DC stepping
    // 0 -> +10 -> -10 at drive 32, and a full-scale 110 Hz saw at drive 1000
    // (the ceiling). The bound: with the cutoff fixed, LP24 passes a slow
    // input with the loop's DC gain, drive x / (1 + k) (at DC every stage
    // settles where t(y_i) = t(y_(i-1)), so y4 = u = x - k y4), and adds at
    // most a self-oscillation, which the reference puts below -6.7 dBFS at
    // every k and cutoff (filter-stability.md): drive max|x| / (1 + k) + 1.
    // Measured before the fallback: a peak of 257-344 at 0.495 fs.
    constexpr double rate = 48000.0;
    constexpr auto n = static_cast<std::size_t>(2.0 * rate);
    std::vector<float> dc(n, 0.0f);
    std::fill(dc.begin() + static_cast<std::ptrdiff_t>(n / 3), dc.begin() + static_cast<std::ptrdiff_t>(2 * n / 3),
              10.0f);
    std::fill(dc.begin() + static_cast<std::ptrdiff_t>(2 * n / 3), dc.end(), -10.0f);
    const auto loud = saw(110.0, rate, n, 1.0f);
    constexpr float k = 64.0f;
    const auto check = [&]<typename Sat>
    {
        for (const double cutoff : {1000.0, 0.495 * rate})
        {
            CAPTURE(isTanh<Sat>, cutoff);
            auto steps = makeFilter<Sat>(rate, cutoff, k, 32.0f);
            const double stepsPeak = peakOf(render(steps, dc));
            CAPTURE(stepsPeak);
            CHECK(stepsPeak < (32.0 * 10.0 / (1.0 + k)) + 1.0);
            auto driven = makeFilter<Sat>(rate, cutoff, k, 1000.0f);
            const double drivenPeak = peakOf(render(driven, loud));
            CAPTURE(drivenPeak);
            CHECK(drivenPeak < (1000.0 / (1.0 + k)) + 1.0);
        }
    };
    check.operator()<TanhSaturator>();
    check.operator()<CubicSaturator>();
}

TEST_CASE("Ladder stability: under audio-rate modulation across the clamp, the filter is the reference's",
          "[dsp][filter][stability]")
{
    // A full-scale 110 Hz saw, the cutoff 12 kHz plus 1.5 sin(2 pi 300 t)
    // octaves through the modulation input every 32 samples (to 34 kHz,
    // clamped at 0.495 fs), k 0-64, drive 1 and 32, 48 kHz, 0.25 s (the saw
    // and the modulation realign every 0.1 s, so this is 2.5 periods of the
    // pattern). No formula bounds this: with g changing every sample the
    // trapezoidal states carry energy across the change, and the equations'
    // own peak reaches 3.8 at k 64 and drive 32, above the fixed-cutoff
    // bound of 1.49. So the requirement is that the filter is correct here:
    // finite, every sample converged, and the peak that of the double
    // reference fed the same g. Before the fallback the peak was 15-350.
    //
    // The test replays the filter's g (each grid point's tan of the clamped
    // cutoff, interpolated linearly over the 32 samples to it) into
    // LadderSolver, checks the replay is the filter bit for bit (so its g is
    // the filter's), and reads the solve's convergence from it.
    //
    // Tolerance: the float solve is within (2)'s bound of the exact solution
    // each sample. Driven by the input, the filter forgets an error as its
    // poles decay, so the trajectories stay as close as one sample's error
    // (measured: the peaks agree to 3 decimals); 1% is the decided figure.
    constexpr double rate = 48000.0;
    constexpr std::size_t n = 12000;
    constexpr std::size_t grid = LadderFilter::gridSamples;
    const auto x = saw(110.0, rate, n, 1.0f);
    const auto modulation = [](std::size_t sample)
    {
        return static_cast<float>(1.5 * std::sin(2.0 * std::numbers::pi * 300.0 * static_cast<double>(sample) / rate));
    };
    const auto gainAt = [](float octaves)
    {
        const auto fs = static_cast<float>(rate);
        const float hz = std::exp2(std::clamp(std::log2(12000.0f) + octaves, -40.0f, 40.0f));
        return std::tan(std::numbers::pi_v<float> * std::clamp(hz, 1.0f, LadderFilter::maxCutoffRatio * fs) / fs);
    };
    const auto check = [&]<typename Sat>
    {
        using R = ReferenceOf<Sat>::Type;
        for (const float k : {0.0f, 4.4f, 8.0f, 64.0f})
        {
            for (const float drive : {1.0f, 32.0f})
            {
                CAPTURE(isTanh<Sat>, k, drive);
                auto f = makeFilter<Sat>(rate, 12000.0, k, drive);
                Buffer buffer{1, n};
                std::ranges::copy(x, buffer.channel(0).begin());
                for (std::size_t start = 0; start < n; start += grid)
                {
                    f.setCutoffModulation(modulation(start));
                    f.process(buffer.block().subBlock(start, grid));
                }
                const std::vector<float> y(buffer.channel(0).begin(), buffer.channel(0).end());

                LadderState state;
                ReferenceLadder<R> reference;
                std::vector<float> replay(n);
                double referencePeak = 0.0;
                std::size_t unconverged = 0;
                float segmentStart = gainAt(0.0f);
                for (std::size_t start = 0; start < n; start += grid)
                {
                    const float segmentEnd = gainAt(modulation(start));
                    const float step = (segmentEnd - segmentStart) * (1.0f / static_cast<float>(grid));
                    for (std::size_t i = 0; i < grid; ++i)
                    {
                        const float g = segmentStart + (step * static_cast<float>(i));
                        LadderSolveStats stats;
                        const auto solution = LadderSolver::solve<Sat>(state, x[start + i] * drive, g, k, &stats);
                        unconverged += stats.converged ? 0U : 1U;
                        replay[start + i] = anoesis::dsp::flushDenormal(solution.y[3]);
                        referencePeak =
                            std::max(referencePeak,
                                     std::fabs(reference.process(static_cast<double>(x[start + i] * drive), g, k)));
                    }
                    segmentStart = segmentEnd;
                }
                const double peak = peakOf(y);
                CAPTURE(peak, referencePeak);
                REQUIRE(bitIdentical(replay, y));
                CHECK(unconverged == 0);
                CHECK(std::isfinite(peak));
                CHECK(std::fabs(peak - referencePeak) <= 0.01 * referencePeak);
            }
        }
    };
    check.operator()<TanhSaturator>();
    check.operator()<CubicSaturator>();
}

TEST_CASE("Ladder stability: self-oscillating at the corner neither drifts nor grows", "[dsp][filter][stability]")
{
    // An impulse of 0.5, then silence, the cutoff held at 0.495 fs, k 8,
    // 48 kHz, 30 s; the level and pitch of the last 16,384 samples of each
    // 6 s, and their spread. (The hidden [filter-stability-report] runs the
    // full five minutes; this is the suite's share, every sample of it
    // through the nested solve.) A stable limit cycle has one level and one
    // pitch, so the spread is the measurement's own: the window catches a
    // different part of the beat between the oscillation and the sampling,
    // which moves the reference's level by up to 0.022 dB (k 8, cubic) and
    // its pitch by under 0.001 cent between windows. 0.05 dB and 0.01 cent
    // are the decided figures, above those; a drift or a growth would show
    // as a trend far past them (before the fallback the filter sat at a peak
    // of 82, cubic 198-272).
    constexpr double rate = 48000.0;
    constexpr auto chunk = static_cast<std::size_t>(6.0 * rate);
    const auto check = [&]<typename Sat>
    {
        CAPTURE(isTanh<Sat>);
        auto f = makeFilter<Sat>(rate, 0.495 * rate, 8.0f, 1.0f);
        std::vector<float> x(chunk, 0.0f);
        double lowDb = std::numeric_limits<double>::infinity();
        double highDb = -lowDb;
        double lowHz = lowDb;
        double highHz = -lowDb;
        double peak = 0.0;
        for (int i = 0; i < 5; ++i)
        {
            std::ranges::fill(x, 0.0f);
            x[0] = i == 0 ? 0.5f : 0.0f;
            const auto y = render(f, x);
            const auto o = oscillationOf(std::span<const float>{y}, rate);
            lowDb = std::min(lowDb, o.levelDb);
            highDb = std::max(highDb, o.levelDb);
            lowHz = std::min(lowHz, o.hz);
            highHz = std::max(highHz, o.hz);
            peak = std::max(peak, peakOf(y));
        }
        CAPTURE(lowDb, highDb, lowHz, highHz, peak);
        CHECK(highDb - lowDb < 0.05);
        CHECK(cents(highHz, lowHz) < 0.01);
        CHECK(peak < 1.0);
    };
    check.operator()<TanhSaturator>();
    check.operator()<CubicSaturator>();
}

TEST_CASE("Ladder stability: the nested solve alone reaches the root from any state", "[dsp][filter][stability]")
{
    // The fallback alone (LadderSolver::solveNested), from 4,000 random
    // states (s in +-50, x in +-32, g 0.1-64, k 0-64, the previous loop input
    // in +-32), against the reference from the same state: converged, within
    // the grid test's bound (with tau, the nested solve's row stop), its
    // stage equations at the rounding level, and within the row budget. The
    // states are wild on purpose: the trajectories of the other tests never
    // put the root more than g from s4, where the theory allows 2g (|y4 - s4|
    // = g |t(y3) - t(y4)|), and these do in about 1,600 cases of 4,000, so
    // the brackets are exercised to their ends.
    const auto check = [&]<typename Sat>
    {
        using R = ReferenceOf<Sat>::Type;
        std::uint32_t seed = 11;
        const auto next = [&]
        {
            seed = (seed * 1664525U) + 1013904223U;
            return static_cast<double>(seed >> 8U) / 16777216.0;
        };
        std::size_t unconverged = 0;
        std::size_t outside = 0;
        std::size_t backward = 0;
        std::size_t overRows = 0;
        std::size_t far = 0;
        for (int trial = 0; trial < 4000; ++trial)
        {
            LadderState state;
            for (auto& v : state.s)
            {
                v = static_cast<float>((next() * 100.0) - 50.0);
            }
            state.previousU = static_cast<float>((next() * 64.0) - 32.0);
            const auto x = static_cast<float>((next() * 64.0) - 32.0);
            const auto g = static_cast<float>(0.1 * std::pow(640.0, next()));
            const float k = next() < 0.2 ? 64.0f : static_cast<float>(next() * 16.0);
            const std::array<double, 4> s{state.s[0], state.s[1], state.s[2], state.s[3]};
            const double gd = g;
            const double kd = k;
            LadderSolveStats stats;
            const auto solution = LadderSolver::solveNested<Sat>(state, x, g, k, &stats);
            unconverged += stats.converged ? 0U : 1U;
            overRows += stats.rows > rowBudget(gd) ? 1U : 0U;
            const std::array<double, 4> guess{solution.y[0], solution.y[1], solution.y[2], solution.y[3]};
            const auto r = referenceSolve<R>(s, static_cast<double>(x), gd, kd, guess);
            far += std::fabs(r.y[3] - s[3]) > gd ? 1U : 0U;
            double scale = 1.0 + (2.0 * gd);
            for (const double si : s)
            {
                scale = std::max(scale, 1.0 + std::fabs(si) + (2.0 * gd));
            }
            const double residual = 8.0 * epsilon * scale;
            const double tau = LadderSolver::newtonTolerance;
            std::array<double, 4> q{};
            for (std::size_t i = 0; i < 4; ++i)
            {
                const double d = 1.0 + (gd * R::slope(r.y[i]));
                q[i] = i == 0 ? gd * kd * R::slope(r.u) / d : q[i - 1] * gd * R::slope(r.y[i - 1]) / d;
            }
            for (std::size_t i = 0; i < 4; ++i)
            {
                const double bound =
                    (static_cast<double>(i + 1) * residual) + (q[i] * ((4.0 * residual / (1.0 + q[3])) + tau));
                outside += std::fabs(static_cast<double>(solution.y[i]) - r.y[i]) <= bound ? 0U : 1U;
            }
            backward +=
                stageResidualsAtRounding<R>(solution.y, s, static_cast<double>(x), gd, kd, false, residual) ? 0U : 1U;
        }
        CAPTURE(isTanh<Sat>, far);
        CHECK(far > 1000);
        CHECK(unconverged == 0);
        CHECK(outside == 0);
        CHECK(backward == 0);
        CHECK(overRows == 0);
    };
    check.operator()<TanhSaturator>();
    check.operator()<CubicSaturator>();
}

TEST_CASE("Ladder stability: at the corner the fallback stays within its work budget", "[dsp][filter][stability]")
{
    // The cutoff held at 0.495 fs, k 8, self-oscillating after an impulse:
    // every sample falls back. Rows: never more than bisection over the whole
    // bracket would take (ceil(log2(4g / tau)) = 25 here). Stage
    // evaluations: the decided ceiling for the corner is 2 us per
    // channel-sample; a stage evaluation (a saturator, its slope, the
    // residual, the safeguarded step) costs about 28 ns and the 8 Newton
    // iterations before the fallback about 0.2 us (relwithdebinfo, Apple M1
    // Ultra), so 2 us is 64 evaluations per sample. Measured: 56 (tanh) and
    // 48 (cubic). A row started away from the linearised guess, or stages
    // started away from their hard-clip model, cost 100-170.
    constexpr double rate = 48000.0;
    const float g = gainOf(0.495 * rate, rate);
    const auto check = [&]<typename Sat>
    {
        LadderState state;
        static_cast<void>(LadderSolver::solve<Sat>(state, 0.5f, g, 8.0f));
        for (int i = 0; i < 4096; ++i)
        {
            static_cast<void>(LadderSolver::solve<Sat>(state, 0.0f, g, 8.0f));
        }
        constexpr int samples = 8192;
        std::size_t fellBack = 0;
        std::size_t overRows = 0;
        double evaluations = 0.0;
        for (int i = 0; i < samples; ++i)
        {
            LadderSolveStats stats;
            static_cast<void>(LadderSolver::solve<Sat>(state, 0.0f, g, 8.0f, &stats));
            fellBack += stats.fellBack ? 1U : 0U;
            overRows += stats.rows > rowBudget(g) ? 1U : 0U;
            evaluations += stats.stageEvaluations;
        }
        const double perSample = evaluations / samples;
        CAPTURE(isTanh<Sat>, perSample);
        CHECK(fellBack == static_cast<std::size_t>(samples));
        CHECK(overRows == 0);
        CHECK(perSample <= 64.0);
    };
    check.operator()<TanhSaturator>();
    check.operator()<CubicSaturator>();
}
