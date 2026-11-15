// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The stability round's prototypes (M1.3 task 7, docs/dsp/filter-stability.md):
// solvers for LadderFilter's trapezoidal ladder equations, test-only, measured
// by the hidden [filter-stability-report] and the [filter-stability]
// benchmarks.
//
// Every candidate solves the same equations as the built filter, per sample
// and per channel:
//
//   F_i(y) = y_i - s_i - g (t(y_(i-1)) - t(y_i)) = 0,  i = 1..4,
//   y_0 = x - k y4,
//
// then updates the integrators s_i <- 2 y_i - s_i. They differ only in how
// the root is found:
//
// - NewtonAsBuilt: LadderFilter::solve, copied operation for operation (the
//   linearised guess, undamped Newton to 1e-5, at most 8 iterations).
// - NewtonDamped: the same Newton direction, with a backtracking line search
//   on the residual (Armijo on |F|^2), so every accepted step reduces it.
// - NewtonStepLimited: the same direction, scaled so no stage moves by more
//   than a fixed amount per iteration.
// - NewtonPreviousGuess: as built, but starting from the previous sample's
//   solution instead of the loop linearised at the state.
// - NewtonBracketFallback: as built while each iteration reduces the
//   residual; when one doesn't (or the cap is reached), a globally convergent
//   nested scalar solve: the last row y4 - H(y4) is strictly increasing, so
//   a safeguarded Newton on y4 with a bisection bracket converges, each
//   stage itself a monotone scalar equation y + g t(y) = c.
// - NewtonOutputVariable: Newton in the saturators' outputs w_i = t(y_i)
//   where a stage is steep (the step is computed as built and mapped through
//   w = t(y), w_new = w + t'(y) dy, y_new = t^-1(w_new)), and in y where the
//   stage is saturated (|w_new| near 1).
// - NewtonInputAdaa: as built, with first-order antiderivative antialiasing
//   on the loop's input saturator (an aliasing option, not a solver fix).
//
// The cutoff clamp, the do-nothing baseline, is a parameter of the filter
// wrapper (maxCutoffRatio).
//
// The double-precision reference solves the same equations by the nested
// scalar method to 1e-14, with std::tanh. The solution is unique: each stage
// equation y + g t(y) = c has slope 1 + g t'(y) >= 1, so y_i is an increasing
// function of t(y_(i-1)), and H(y4), the y4 the chain returns for a given
// feedback, is non-increasing in y4 (through -k), so y4 - H(y4) is strictly
// increasing with slope >= 1 and has exactly one root, within 2g of s4.

#pragma once

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/math/fast_math.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>

namespace anoesis::test::stability
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

/// The root of an increasing f on [lo, hi] (f(lo) <= 0 <= f(hi)) by Newton,
/// safeguarded as rtsafe is (Numerical Recipes, 2nd ed., 9.4): a step bisects
/// the bracket instead when Newton's would leave it, or would not be at most
/// half the step before last. Without the second rule Newton can ping-pong
/// between the bracket's ends, shrinking it by a sliver each time (measured:
/// a tanh stage at g = 64 with a residual of -118 after 48 steps). `evaluate`
/// returns f and f' at y. Stops when a step is under abs + rel |y|, or when
/// |f| is at most fTolerance (the rounding level of f's terms: below it a
/// step is noise, and chasing it only spends evaluations).
template <typename T>
struct Root
{
    T y{};
    bool converged = false;
    std::uint32_t evaluations = 0;
};

template <typename T, typename Evaluate>
[[nodiscard]] Root<T> safeguardedRoot(const Evaluate& evaluate, T lo, T hi, T guess, T absTolerance, T relTolerance,
                                      std::uint32_t cap, T fTolerance = T{0}) noexcept
{
    Root<T> r;
    r.y = std::clamp(guess, lo, hi);
    T stepBeforeLast = hi - lo;
    T lastStep = stepBeforeLast;
    while (r.evaluations < cap)
    {
        const auto [f, slope] = evaluate(r.y);
        ++r.evaluations;
        if (std::fabs(f) <= fTolerance)
        {
            r.converged = true;
            return r;
        }
        if (f > T{0})
        {
            hi = r.y;
        }
        else if (f < T{0})
        {
            lo = r.y;
        }
        else
        {
            r.converged = true;
            return r;
        }
        const T newton = r.y - (f / slope);
        const bool outside = !(newton > lo && newton < hi);
        const bool slow = std::fabs(T{2} * f) > std::fabs(stepBeforeLast * slope);
        stepBeforeLast = lastStep;
        const T next = outside || slow ? lo + (T{0.5} * (hi - lo)) : newton;
        lastStep = next - r.y;
        const bool done = std::fabs(lastStep) <= absTolerance + (relTolerance * std::fabs(r.y));
        r.y = next;
        if (done)
        {
            r.converged = true;
            return r;
        }
    }
    return r;
}

/// y + g t(y) = c: increasing with slope >= 1, so the root is unique and lies
/// in [c - g, c + g].
template <typename R>
[[nodiscard]] double referenceStage(double c, double g, double guess) noexcept
{
    const auto evaluate = [&](double y)
    {
        return std::pair{y + (g * R::value(y)) - c, 1.0 + (g * R::slope(y))};
    };
    return safeguardedRoot(evaluate, c - g, c + g, guess, 0.0, 1e-15, 400).y;
}

struct ReferenceSolution
{
    std::array<double, 4> y{};
    double u = 0.0; ///< x - k y4, before its saturator
};

/// The chain for a given y4: u = x - k y4, then each stage from the last.
/// Returns the y4 it produces (H) and dH/dy4.
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

/// The unique solution of the sample's equations from state s, to 1e-14:
/// y4 - H(y4) is increasing with slope >= 1, and its root is within 2g of s4.
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
    const double y4 =
        safeguardedRoot(evaluate, s[3] - (2.0 * g) - margin, s[3] + (2.0 * g) + margin, guess[3], 0.0, 1e-14, 400).y;
    double derivative = 0.0;
    static_cast<void>(referenceChain<R>(s, x, g, k, y4, r.y, derivative));
    r.y[3] = y4;
    r.u = x - (k * y4);
    return r;
}

/// The reference ladder along its own trajectory (LP24 out).
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

    [[nodiscard]] const std::array<double, 4>& stages() const noexcept
    {
        return y;
    }

private:
    std::array<double, 4> s{};
    std::array<double, 4> y{};
};

// --- The float candidates ----------------------------------------------------------

struct ChannelState
{
    std::array<float, 4> s{}; ///< the integrators
    float previousU = 0.0f;   ///< the loop input before its saturator
    std::array<float, 4> y{}; ///< the last solution (NewtonPreviousGuess)
};

struct Solution
{
    float u = 0.0f; ///< t(x - k y4)
    std::array<float, 4> y{};
};

/// What a solve did, accumulated over samples.
struct SolveStats
{
    std::uint64_t samples = 0;
    std::uint64_t unconverged = 0; ///< ended without a step under the tolerance
    std::uint64_t iterations = 0;  ///< residual evaluations (Newton) plus fallback rows
    std::uint32_t maxIterations = 0;
    std::uint64_t fallbacks = 0;        ///< samples that left Newton for another method
    std::uint64_t stageEvaluations = 0; ///< scalar stage solves' evaluations (the nested solve)

    void add(std::uint32_t n, bool converged, bool fellBack, std::uint32_t stages = 0) noexcept
    {
        ++samples;
        iterations += n;
        stageEvaluations += stages;
        maxIterations = std::max(maxIterations, n);
        unconverged += converged ? 0U : 1U;
        fallbacks += fellBack ? 1U : 0U;
    }
};

constexpr float tolerance = 1e-5f;

/// The loop linearised at the state and solved in closed form (as built).
template <typename Sat>
void linearisedGuess(const ChannelState& state, float x, float g, float k, std::array<float, 4>& y) noexcept
{
    const auto& s = state.s;
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

/// The input saturator as built: t(u).
template <typename Sat>
struct PlainInput
{
    float previousU = 0.0f;
    [[nodiscard]] float value(float u) const noexcept
    {
        return Sat::value(u);
    }
    [[nodiscard]] float slope(float u, float t) const noexcept
    {
        return Sat::slope(u, t);
    }
};

/// One Newton evaluation at y, as built: the residuals, the saturators and
/// their slopes, and the step (delta). Returns |F|^2 (not used by the
/// as-built solver; computing it changes nothing in the step).
struct Step
{
    std::array<float, 4> delta{};
    std::array<float, 4> t{};     ///< t(y_i)
    std::array<float, 4> slope{}; ///< t'(y_i)
    float residual2 = 0.0f;
};

template <typename Sat, typename Input>
[[nodiscard]] Step newtonStep(const Input& input, const std::array<float, 4>& s, const std::array<float, 4>& y, float x,
                              float g, float k) noexcept
{
    Step step;
    const float u = x - (k * y[3]);
    const float tu = input.value(u);
    const float du = input.slope(u, tu);
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
        step.residual2 += residual * residual;
        step.t[i] = t;
        step.slope[i] = d;
        previousT = t;
        previousSlope = d;
    }
    const float delta4 = p[3] / (1.0f - q[3]);
    for (std::size_t i = 0; i < 4; ++i)
    {
        step.delta[i] = i == 3 ? delta4 : p[i] + (q[i] * delta4);
    }
    return step;
}

[[nodiscard]] inline float largest(const std::array<float, 4>& delta) noexcept
{
    float m = 0.0f;
    for (const float d : delta)
    {
        m = std::max(m, std::fabs(d));
    }
    return m;
}

/// The integrator update and the output, as built.
template <typename Sat, typename Input>
[[nodiscard]] Solution finish(ChannelState& state, const Input& input, float x, float k,
                              const std::array<float, 4>& y) noexcept
{
    Solution result;
    result.y = y;
    const float u = x - (k * y[3]);
    result.u = input.value(u);
    for (std::size_t i = 0; i < 4; ++i)
    {
        state.s[i] = dsp::flushDenormal((2.0f * y[i]) - state.s[i]);
    }
    state.previousU = dsp::flushDenormal(u);
    state.y = y;
    return result;
}

/// LadderFilter::solve, operation for operation.
template <std::uint32_t Cap = 8>
struct NewtonAsBuilt
{
    template <typename Sat>
    [[nodiscard]] static Solution solve(ChannelState& state, float x, float g, float k, SolveStats& stats) noexcept
    {
        const PlainInput<Sat> input;
        std::array<float, 4> y{};
        linearisedGuess<Sat>(state, x, g, k, y);
        bool converged = false;
        std::uint32_t n = 0;
        while (n < Cap)
        {
            const auto step = newtonStep<Sat>(input, state.s, y, x, g, k);
            ++n;
            for (std::size_t i = 0; i < 4; ++i)
            {
                y[i] += step.delta[i];
            }
            if (largest(step.delta) < tolerance)
            {
                converged = true;
                break;
            }
        }
        stats.add(n, converged, false);
        return finish<Sat>(state, input, x, k, y);
    }
};

/// Newton with a backtracking line search: a trial y + lambda dy is accepted
/// when |F|^2 falls by the Armijo fraction, else lambda halves (down to
/// 2^-MinHalvings, which is then taken). The trial's evaluation is the next
/// iteration's, so a full step costs what it does as built, and while every
/// full step is accepted the iterates are the as-built ones, bit for bit.
template <std::uint32_t Cap = 8, int MinHalvings = 10>
struct NewtonDamped
{
    template <typename Sat>
    [[nodiscard]] static Solution solve(ChannelState& state, float x, float g, float k, SolveStats& stats) noexcept
    {
        const PlainInput<Sat> input;
        std::array<float, 4> y{};
        linearisedGuess<Sat>(state, x, g, k, y);
        auto step = newtonStep<Sat>(input, state.s, y, x, g, k);
        std::uint32_t n = 1;
        bool converged = false;
        while (true)
        {
            if (largest(step.delta) < tolerance)
            {
                for (std::size_t i = 0; i < 4; ++i)
                {
                    y[i] += step.delta[i];
                }
                converged = true;
                break;
            }
            if (n >= Cap)
            {
                for (std::size_t i = 0; i < 4; ++i)
                {
                    y[i] += step.delta[i];
                }
                break;
            }
            float lambda = 1.0f;
            std::array<float, 4> trial{};
            Step next;
            for (int halving = 0;; ++halving)
            {
                for (std::size_t i = 0; i < 4; ++i)
                {
                    trial[i] = y[i] + (halving == 0 ? step.delta[i] : lambda * step.delta[i]);
                }
                next = newtonStep<Sat>(input, state.s, trial, x, g, k);
                ++n;
                const bool decreased = next.residual2 <= (1.0f - (2e-4f * lambda)) * step.residual2;
                if (decreased || halving >= MinHalvings || n >= Cap)
                {
                    break;
                }
                lambda *= 0.5f;
            }
            y = trial;
            step = next;
        }
        stats.add(n, converged, false);
        return finish<Sat>(state, input, x, k, y);
    }
};

/// Newton with each step scaled so no stage moves by more than Limit.
template <std::uint32_t Cap = 8>
struct NewtonStepLimited
{
    static constexpr float limit = 1.0f;

    template <typename Sat>
    [[nodiscard]] static Solution solve(ChannelState& state, float x, float g, float k, SolveStats& stats) noexcept
    {
        const PlainInput<Sat> input;
        std::array<float, 4> y{};
        linearisedGuess<Sat>(state, x, g, k, y);
        bool converged = false;
        std::uint32_t n = 0;
        while (n < Cap)
        {
            auto step = newtonStep<Sat>(input, state.s, y, x, g, k);
            ++n;
            const float m = largest(step.delta);
            if (m > limit)
            {
                const float scale = limit / m;
                for (auto& d : step.delta)
                {
                    d *= scale;
                }
            }
            for (std::size_t i = 0; i < 4; ++i)
            {
                y[i] += step.delta[i];
            }
            if (m < tolerance)
            {
                converged = true;
                break;
            }
        }
        stats.add(n, converged, false);
        return finish<Sat>(state, input, x, k, y);
    }
};

/// As built, from the previous sample's solution.
template <std::uint32_t Cap = 8>
struct NewtonPreviousGuess
{
    template <typename Sat>
    [[nodiscard]] static Solution solve(ChannelState& state, float x, float g, float k, SolveStats& stats) noexcept
    {
        const PlainInput<Sat> input;
        auto y = state.y;
        bool converged = false;
        std::uint32_t n = 0;
        while (n < Cap)
        {
            const auto step = newtonStep<Sat>(input, state.s, y, x, g, k);
            ++n;
            for (std::size_t i = 0; i < 4; ++i)
            {
                y[i] += step.delta[i];
            }
            if (largest(step.delta) < tolerance)
            {
                converged = true;
                break;
            }
        }
        stats.add(n, converged, false);
        return finish<Sat>(state, input, x, k, y);
    }
};

/// Whether stageSolve starts from the hard-clip model instead of the given
/// guess (measured both ways; see the report).
inline constexpr bool stageModelGuess = true;

/// y + g t(y) = c in float, by safeguardedRoot on [c - g, c + g].
template <typename Sat>
[[nodiscard]] float stageSolve(float c, float g, float guess, std::uint32_t& evaluations) noexcept
{
    const auto evaluate = [&](float y)
    {
        const float t = Sat::value(y);
        return std::pair{y + (g * t) - c, 1.0f + (g * Sat::slope(y, t))};
    };
    // Where to start: the root of the stage with t replaced by a hard clip
    // (slope 1 to |y| = 1, then +-1): c / (1 + g) inside, c -+ g outside. It
    // is in the bracket, and close for large g whichever side the root is on;
    // the previous row's y can be far off after a bisecting row.
    if (stageModelGuess)
    {
        const float inside = c / (1.0f + g);
        guess = std::fabs(inside) <= 1.0f ? inside : c - std::copysign(g, c);
    }
    // f's terms are about |c| + g, each rounded to 2^-24 of itself.
    const float rounding = 4.0f * std::numeric_limits<float>::epsilon() * (std::fabs(c) + g);
    const auto root = safeguardedRoot(evaluate, c - g, c + g, guess, 0.0f, 1e-7f, 64, rounding);
    evaluations += root.evaluations;
    return root.y;
}

/// The globally convergent nested solve, in float: safeguardedRoot on the
/// last row y4 - H(y4) over [s4 - 2g, s4 + 2g] to the Newton tolerance, each
/// stage by stageSolve. Counts rows in `rows` and stage evaluations in
/// `evaluations`; returns whether a row step fell under the tolerance.
template <typename Sat>
bool bracketSolve(const ChannelState& state, float x, float g, float k, std::array<float, 4>& y, std::uint32_t& rows,
                  std::uint32_t& evaluations) noexcept
{
    const auto& s = state.s;
    const auto chain = [&](float at, float& derivative)
    {
        const float u = x - (k * at);
        float c = Sat::value(u);
        derivative = -k * Sat::slope(u, c);
        for (std::size_t i = 0; i < 4; ++i)
        {
            y[i] = stageSolve<Sat>(s[i] + (g * c), g, y[i], evaluations);
            const float t = Sat::value(y[i]);
            const float slope = Sat::slope(y[i], t);
            derivative *= g / (1.0f + (g * slope));
            if (i < 3)
            {
                derivative *= slope;
            }
            c = t;
        }
        return y[3];
    };
    const auto evaluate = [&](float y4)
    {
        float derivative = 0.0f;
        const float phi = y4 - chain(y4, derivative);
        return std::pair{phi, 1.0f - derivative};
    };
    const auto root = safeguardedRoot(evaluate, s[3] - (2.0f * g), s[3] + (2.0f * g), y[3], tolerance, 0.0f, 64);
    rows += root.evaluations;
    float derivative = 0.0f;
    static_cast<void>(chain(root.y, derivative));
    y[3] = root.y;
    return root.converged;
}

/// As built while each Newton iteration reduces |F|^2; on the first that
/// doesn't, or at the cap, the nested solve, from where Newton got to
/// (FromGuess false) or from the linearised guess again (FromGuess true).
template <bool FromGuess = false, std::uint32_t Cap = 8>
struct NewtonBracketFallback
{
    template <typename Sat>
    [[nodiscard]] static Solution solve(ChannelState& state, float x, float g, float k, SolveStats& stats) noexcept
    {
        const PlainInput<Sat> input;
        std::array<float, 4> guess{};
        linearisedGuess<Sat>(state, x, g, k, guess);
        auto y = guess;
        bool converged = false;
        std::uint32_t n = 0;
        float previousResidual = std::numeric_limits<float>::infinity();
        while (n < Cap)
        {
            const auto step = newtonStep<Sat>(input, state.s, y, x, g, k);
            ++n;
            if (!(step.residual2 < previousResidual))
            {
                break;
            }
            previousResidual = step.residual2;
            for (std::size_t i = 0; i < 4; ++i)
            {
                y[i] += step.delta[i];
            }
            if (largest(step.delta) < tolerance)
            {
                converged = true;
                break;
            }
        }
        std::uint32_t stages = 0;
        const bool fellBack = !converged;
        if (fellBack)
        {
            if (FromGuess)
            {
                y = guess;
            }
            converged = bracketSolve<Sat>(state, x, g, k, y, n, stages);
        }
        stats.add(n, converged, fellBack, stages);
        return finish<Sat>(state, input, x, k, y);
    }
};

/// The nested solve alone, from the linearised guess: no Newton on the system.
struct NestedOnly
{
    template <typename Sat>
    [[nodiscard]] static Solution solve(ChannelState& state, float x, float g, float k, SolveStats& stats) noexcept
    {
        const PlainInput<Sat> input;
        std::array<float, 4> y{};
        linearisedGuess<Sat>(state, x, g, k, y);
        std::uint32_t rows = 0;
        std::uint32_t stages = 0;
        const bool converged = bracketSolve<Sat>(state, x, g, k, y, rows, stages);
        stats.add(rows, converged, false, stages);
        return finish<Sat>(state, input, x, k, y);
    }
};

/// The saturator's inverse on its steep part, for NewtonOutputVariable.
template <typename Sat>
struct Inverse;
template <>
struct Inverse<dsp::TanhSaturator>
{
    [[nodiscard]] static float of(float w) noexcept
    {
        return 0.5f * std::log((1.0f + w) / (1.0f - w));
    }
};
template <>
struct Inverse<dsp::CubicSaturator>
{
    /// t(3 sin a) = sin 3a, so t^-1(w) = 3 sin(asin(w) / 3) for |w| <= 1.
    [[nodiscard]] static float of(float w) noexcept
    {
        return 3.0f * std::sin(std::asin(w) * (1.0f / 3.0f));
    }
};

/// Newton in w = t(y) where the stage is steep: the as-built step dy, mapped
/// as w_new = t(y) + t'(y) dy, y_new = t^-1(w_new) while |w_new| < WLimit;
/// in y (y + dy) where it isn't.
template <std::uint32_t Cap = 8>
struct NewtonOutputVariable
{
    static constexpr float wLimit = 0.999f;

    template <typename Sat>
    [[nodiscard]] static Solution solve(ChannelState& state, float x, float g, float k, SolveStats& stats) noexcept
    {
        const PlainInput<Sat> input;
        std::array<float, 4> y{};
        linearisedGuess<Sat>(state, x, g, k, y);
        bool converged = false;
        std::uint32_t n = 0;
        while (n < Cap)
        {
            const auto step = newtonStep<Sat>(input, state.s, y, x, g, k);
            ++n;
            for (std::size_t i = 0; i < 4; ++i)
            {
                const float w = step.t[i] + (step.slope[i] * step.delta[i]);
                y[i] = std::fabs(w) < wLimit ? Inverse<Sat>::of(w) : y[i] + step.delta[i];
            }
            if (largest(step.delta) < tolerance)
            {
                converged = true;
                break;
            }
        }
        stats.add(n, converged, false);
        return finish<Sat>(state, input, x, k, y);
    }
};

/// The antiderivatives for first-order ADAA, in double: in float, T(u) -
/// T(u_prev) cancels (T is about |u|, so at |u| = 8 one ulp of T over a
/// difference of 1e-3 is an error of 1e-3, an aliasing floor near -60 dB).
template <typename Sat>
struct Antiderivative;
template <>
struct Antiderivative<dsp::TanhSaturator>
{
    /// log cosh x, stable for large |x|.
    [[nodiscard]] static double of(double x) noexcept
    {
        const double a = std::fabs(x);
        return a + std::log1p(std::exp(-2.0 * a)) - std::numbers::ln2;
    }
};
template <>
struct Antiderivative<dsp::CubicSaturator>
{
    [[nodiscard]] static double of(double x) noexcept
    {
        constexpr double knee = dsp::CubicSaturator::knee;
        const double c = std::clamp(x, -knee, knee);
        const double inside = (0.5 * c * c) - ((1.0 / 27.0) * c * c * c * c);
        return inside + (std::fabs(x) - knee); // beyond the knee t = +-1, so T grows as |x|
    }
};

/// The loop's input saturator with first-order ADAA against the previous
/// sample's input: (T(u) - T(u_prev)) / (u - u_prev), the midpoint value when
/// the two are within epsilon.
template <typename Sat>
struct AdaaInput
{
    float previousU = 0.0f;
    static constexpr float epsilon = 1e-3f;

    [[nodiscard]] float value(float u) const noexcept
    {
        const float d = u - previousU;
        if (std::fabs(d) < epsilon)
        {
            return Sat::value(0.5f * (u + previousU));
        }
        const double difference = Antiderivative<Sat>::of(u) - Antiderivative<Sat>::of(previousU);
        return static_cast<float>(difference / static_cast<double>(d));
    }
    [[nodiscard]] float slope(float u, float value) const noexcept
    {
        const float d = u - previousU;
        if (std::fabs(d) < epsilon)
        {
            const float m = 0.5f * (u + previousU);
            return 0.5f * Sat::slope(m, Sat::value(m));
        }
        return std::max(0.0f, (Sat::value(u) - value) / d);
    }
};

/// The as-built Newton with ADAA on the loop's input saturator.
template <std::uint32_t Cap = 8>
struct NewtonInputAdaa
{
    template <typename Sat>
    [[nodiscard]] static Solution solve(ChannelState& state, float x, float g, float k, SolveStats& stats) noexcept
    {
        const AdaaInput<Sat> input{state.previousU};
        std::array<float, 4> y{};
        linearisedGuess<Sat>(state, x, g, k, y);
        bool converged = false;
        std::uint32_t n = 0;
        while (n < Cap)
        {
            const auto step = newtonStep<Sat>(input, state.s, y, x, g, k);
            ++n;
            for (std::size_t i = 0; i < 4; ++i)
            {
                y[i] += step.delta[i];
            }
            if (largest(step.delta) < tolerance)
            {
                converged = true;
                break;
            }
        }
        stats.add(n, converged, false);
        return finish<Sat>(state, input, x, k, y);
    }
};

/// NaN handling, option 1: a non-finite input sample is replaced by 0 before
/// the solve, so the state never sees it.
template <typename Inner>
struct SanitizedInput
{
    template <typename Sat>
    [[nodiscard]] static Solution solve(ChannelState& state, float x, float g, float k, SolveStats& stats) noexcept
    {
        return Inner::template solve<Sat>(state, std::isfinite(x) ? x : 0.0f, g, k, stats);
    }
};

// --- A filter around a solver -----------------------------------------------------------

/// The built filter's per-sample path with a candidate solver: a drive gain
/// on the input, g from the cutoff as LadderFilter computes it at a settled
/// grid point (or set directly), k, and the LP24 output (y4), flushed. No
/// ramps, no grid, no other responses: with settled controls LadderFilter's
/// LP24 is exactly this, so NewtonAsBuilt reproduces it bit for bit. A
/// BlockProcessor, so it runs inside the Oversampler.
template <typename Sat, typename Solver>
class CandidateFilter
{
public:
    static constexpr std::size_t maxChannels = 2;

    explicit CandidateFilter(float maxCutoffRatio = dsp::LadderFilter::maxCutoffRatio) noexcept
        : cutoffRatio(maxCutoffRatio)
    {
    }

    void prepare(const dsp::ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        channelCount = std::min(spec.numChannels, maxChannels);
        setCutoff(cutoffHz);
        reset();
    }

    void reset() noexcept
    {
        channels = {};
    }

    /// As LadderFilter at a grid point: log2 and back, clamped, then tan.
    void setCutoff(float hz) noexcept
    {
        cutoffHz = hz;
        const float log2Hz = std::log2(std::max(dsp::LadderFilter::minCutoffHz, std::min(1e6f, hz)));
        const float settled = std::exp2(std::clamp(log2Hz, -40.0f, 40.0f));
        const float clamped = std::clamp(settled, dsp::LadderFilter::minCutoffHz, cutoffRatio * sampleRate);
        g = std::tan(std::numbers::pi_v<float> * clamped / sampleRate);
    }

    void setGain(float gain) noexcept
    {
        g = gain;
    }
    void setResonance(float loopGain) noexcept
    {
        k = loopGain;
    }
    void setDrive(float gain) noexcept
    {
        drive = gain;
    }

    [[nodiscard]] float gain() const noexcept
    {
        return g;
    }

    float processSample(std::size_t channel, float x) noexcept
    {
        const auto solution = Solver::template solve<Sat>(channels[channel], x * drive, g, k, stats);
        return dsp::flushDenormal(solution.y[3]);
    }

    void process(const dsp::AudioBlock& block) noexcept
    {
        const auto numChannels = std::min(block.numChannels(), channelCount);
        for (std::size_t ch = 0; ch < numChannels; ++ch)
        {
            for (auto& v : block.channel(ch))
            {
                v = processSample(ch, v);
            }
        }
    }

    /// NaN handling, option 2, once per block: a channel whose state is not
    /// finite is reset to rest. Returns whether any was.
    bool recoverNonFinite() noexcept
    {
        bool any = false;
        for (auto& c : channels)
        {
            const bool finite =
                std::ranges::all_of(c.s, [](float v) { return std::isfinite(v); }) && std::isfinite(c.previousU);
            if (!finite)
            {
                c = {};
                any = true;
            }
        }
        return any;
    }

    [[nodiscard]] const ChannelState& state(std::size_t channel = 0) const noexcept
    {
        return channels[channel];
    }

    SolveStats stats;

private:
    float cutoffRatio;
    float sampleRate = 48000.0f;
    std::size_t channelCount = 1;
    float cutoffHz = 1000.0f;
    float g = 0.0f;
    float k = 0.0f;
    float drive = 1.0f;
    std::array<ChannelState, maxChannels> channels{};
};

} // namespace anoesis::test::stability
