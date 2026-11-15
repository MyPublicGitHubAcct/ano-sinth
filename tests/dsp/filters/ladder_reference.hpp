// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Two oracles for the ladder's per-sample solve (LadderSolver,
// docs/dsp/filter-stability.md), test-only:
//
// - The double-precision reference: the same trapezoidal equations
//
//     F_i(y) = y_i - s_i - g (t(y_(i-1)) - t(y_i)) = 0,  i = 1..4,
//     y_0 = x - k y4,
//
//   solved in double with std::tanh by the nested bracketing method, to
//   1e-14 (stages to 1e-15), up to 400 evaluations per level. It cannot
//   fail: each stage y + g t(y) = c is increasing with slope >= 1, so its
//   root is unique and in [c - g, c + g]; the last row y4 - H(y4) is
//   increasing with slope >= 1 (H, the y4 the chain returns for a given
//   feedback, is non-increasing through -k), so its root is unique and
//   within 2g of s4.
// - Today's Newton solve, restated operation for operation from the solver
//   as built before the stability round (commit 26cd4ec): the linearised
//   guess, undamped Newton to 1e-5, at most 8 iterations. Wherever it
//   converges, LadderSolver must return exactly what it returns.

#pragma once

#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/filters/ladder_solver.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace anoesis::test::ladder
{

namespace dsp = anoesis::dsp;

// --- The double-precision reference -------------------------------------------

struct ReferenceTanh
{
    [[nodiscard]] static double value(double x) noexcept
    {
        return std::tanh(x);
    }
    [[nodiscard]] static double slope(double x) noexcept
    {
        const double t = std::tanh(x);
        return 1.0 - (t * t);
    }
};

struct ReferenceCubic
{
    [[nodiscard]] static double value(double x) noexcept
    {
        const double c = std::clamp(x, -1.5, 1.5);
        return c - ((4.0 / 27.0) * c * c * c);
    }
    [[nodiscard]] static double slope(double x) noexcept
    {
        return std::fabs(x) >= 1.5 ? 0.0 : 1.0 - ((4.0 / 9.0) * x * x);
    }
};

template <typename Sat>
struct ReferenceOf;
template <>
struct ReferenceOf<dsp::TanhSaturator>
{
    using Type = ReferenceTanh;
};
template <>
struct ReferenceOf<dsp::CubicSaturator>
{
    using Type = ReferenceCubic;
};

/// The root of an increasing f on [lo, hi] (f(lo) <= 0 <= f(hi)) by Newton
/// safeguarded as rtsafe is (Numerical Recipes, 2nd ed., 9.4): bisect when
/// Newton's step would leave the bracket or would not be at most half the
/// step before last. `evaluate` returns f and f' at y. Stops when a step is
/// under relTolerance |y|.
template <typename Evaluate>
[[nodiscard]] double referenceRoot(const Evaluate& evaluate, double lo, double hi, double guess,
                                   double relTolerance) noexcept
{
    double y = std::clamp(guess, lo, hi);
    double stepBeforeLast = hi - lo;
    double lastStep = stepBeforeLast;
    for (int n = 0; n < 400; ++n)
    {
        const auto [f, slope] = evaluate(y);
        if (f > 0.0)
        {
            hi = y;
        }
        else if (f < 0.0)
        {
            lo = y;
        }
        else
        {
            return y;
        }
        const double newton = y - (f / slope);
        const bool outside = newton <= lo || newton >= hi;
        const bool slow = std::fabs(2.0 * f) > std::fabs(stepBeforeLast * slope);
        stepBeforeLast = lastStep;
        const double next = outside || slow ? lo + (0.5 * (hi - lo)) : newton;
        lastStep = next - y;
        y = next;
        if (std::fabs(lastStep) <= relTolerance * std::fabs(y))
        {
            return y;
        }
    }
    return y;
}

template <typename R>
[[nodiscard]] double referenceStage(double c, double g, double guess) noexcept
{
    const auto evaluate = [&](double y)
    {
        return std::pair{y + (g * R::value(y)) - c, 1.0 + (g * R::slope(y))};
    };
    return referenceRoot(evaluate, c - g, c + g, guess, 1e-15);
}

/// The chain for a given y4: u = x - k y4, then each stage. Returns the y4
/// it produces (H) and dH/dy4 in `derivative`.
template <typename R>
double referenceChain(const std::array<double, 4>& s, double x, double g, double k, double y4, std::array<double, 4>& y,
                      double& derivative) noexcept
{
    const double u = x - (k * y4);
    double c = R::value(u);
    derivative = -k * R::slope(u);
    for (std::size_t i = 0; i < 4; ++i)
    {
        y[i] = referenceStage<R>(s[i] + (g * c), g, y[i]);
        const double slope = R::slope(y[i]);
        derivative *= g / (1.0 + (g * slope));
        if (i < 3)
        {
            derivative *= slope;
        }
        c = R::value(y[i]);
    }
    return y[3];
}

struct ReferenceSolution
{
    std::array<double, 4> y{};
    double u = 0.0; ///< x - k y4, before its saturator
};

/// The unique solution of one sample's equations from state s, to 1e-14,
/// starting from `guess` (any guess converges; a close one is faster).
template <typename R>
[[nodiscard]] ReferenceSolution referenceSolve(const std::array<double, 4>& s, double x, double g, double k,
                                               const std::array<double, 4>& guess) noexcept
{
    ReferenceSolution r;
    r.y = guess;
    const auto evaluate = [&](double y4)
    {
        double derivative = 0.0;
        const double phi = y4 - referenceChain<R>(s, x, g, k, y4, r.y, derivative);
        return std::pair{phi, 1.0 - derivative};
    };
    const double margin = 1e-12 * (1.0 + std::fabs(s[3]));
    const double y4 = referenceRoot(evaluate, s[3] - (2.0 * g) - margin, s[3] + (2.0 * g) + margin, guess[3], 1e-14);
    double derivative = 0.0;
    static_cast<void>(referenceChain<R>(s, x, g, k, y4, r.y, derivative));
    r.y[3] = y4;
    r.u = x - (k * y4);
    return r;
}

/// The reference ladder along its own trajectory (LP24 out), g and k fixed.
template <typename R>
class ReferenceLadder
{
public:
    double process(double x, double g, double k) noexcept
    {
        const auto r = referenceSolve<R>(s, x, g, k, y);
        y = r.y;
        for (std::size_t i = 0; i < 4; ++i)
        {
            s[i] = (2.0 * y[i]) - s[i];
        }
        return y[3];
    }

private:
    std::array<double, 4> s{};
    std::array<double, 4> y{};
};

// --- Today's Newton solve -------------------------------------------------------

struct NewtonResult
{
    dsp::LadderSolution solution;
    bool converged = false;
};

/// The solver as built before the stability round, operation for operation;
/// updates `state` as it did.
template <typename Sat>
[[nodiscard]] NewtonResult todaysNewton(dsp::LadderState& state, float x, float g, float k) noexcept
{
    auto& s = state.s;
    NewtonResult result;
    auto& y = result.solution.y;
    {
        const float inputSecant = Sat::secant(state.previousU);
        float a = 1.0f;
        float b = 0.0f;
        std::array<float, 4> ai{};
        std::array<float, 4> bi{};
        float inSecant = inputSecant;
        for (std::size_t i = 0; i < 4; ++i)
        {
            const float stageSecant = Sat::secant(s[i]);
            const float scale = 1.0f / (1.0f + (g * stageSecant));
            ai[i] = g * inSecant * scale;
            bi[i] = s[i] * scale;
            a *= ai[i];
            b = (ai[i] * b) + bi[i];
            inSecant = stageSecant;
        }
        float in = (x - (k * b)) / (1.0f + (k * a));
        for (std::size_t i = 0; i < 4; ++i)
        {
            y[i] = (ai[i] * in) + bi[i];
            in = y[i];
        }
    }
    for (std::uint32_t iteration = 0; iteration < 8; ++iteration)
    {
        const float u = x - (k * y[3]);
        const float tu = Sat::value(u);
        const float du = Sat::slope(u, tu);
        std::array<float, 4> p{};
        std::array<float, 4> q{};
        float previousT = tu;
        float previousSlope = du;
        for (std::size_t i = 0; i < 4; ++i)
        {
            const float t = Sat::value(y[i]);
            const float d = Sat::slope(y[i], t);
            const float residual = y[i] - s[i] - (g * (previousT - t));
            const float diagonal = 1.0f / (1.0f + (g * d));
            if (i == 0)
            {
                p[0] = -residual * diagonal;
                q[0] = -(g * k * du) * diagonal;
            }
            else
            {
                p[i] = (-residual + (g * previousSlope * p[i - 1])) * diagonal;
                q[i] = g * previousSlope * q[i - 1] * diagonal;
            }
            previousT = t;
            previousSlope = d;
        }
        const float delta4 = p[3] / (1.0f - q[3]);
        float largest = 0.0f;
        for (std::size_t i = 0; i < 4; ++i)
        {
            const float delta = i == 3 ? delta4 : p[i] + (q[i] * delta4);
            y[i] += delta;
            largest = std::max(largest, std::fabs(delta));
        }
        if (largest < 1e-5f)
        {
            result.converged = true;
            break;
        }
    }
    const float u = x - (k * y[3]);
    result.solution.u = Sat::value(u);
    for (std::size_t i = 0; i < 4; ++i)
    {
        s[i] = dsp::flushDenormal((2.0f * y[i]) - s[i]);
    }
    state.previousU = dsp::flushDenormal(u);
    return result;
}

} // namespace anoesis::test::ladder
