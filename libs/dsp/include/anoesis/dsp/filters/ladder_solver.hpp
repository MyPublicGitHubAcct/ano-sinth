// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/math/fast_math.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

/// The ladder's per-sample solve (LadderFilter, docs/dsp/filters.md; theory
/// in docs/dsp/filter-theory.md and docs/dsp/filter-stability.md): the
/// trapezoidal equations of four saturating one-poles in a loop,
///
///   F_i(y) = y_i - s_i - g (t(y_(i-1)) - t(y_i)) = 0,   i = 1..4,
///   y_0 = x - k y4,
///
/// for one channel and one sample, then the integrators' update
/// s_i <- 2 y_i - s_i. Public so that the solve can be tested from a known
/// state against a reference; LadderFilter is its only user in the library.
namespace anoesis::dsp
{

/// tanh: every odd harmonic (Warm Drive on).
struct TanhSaturator
{
    [[nodiscard]] static float value(float x) noexcept
    {
        return fastTanh(x);
    }
    /// The slope at x, given value(x).
    [[nodiscard]] static float slope(float /*x*/, float value) noexcept
    {
        return 1.0f - (value * value);
    }
    /// value(x) / x, 1 at 0.
    [[nodiscard]] static float secant(float x) noexcept
    {
        return std::fabs(x) < 1e-4f ? 1.0f : fastTanh(x) / x;
    }
};

/// x - (4/27) x^3 up to |x| = 1.5, then +-1: only the third harmonic below
/// the knee, no division (Warm Drive off).
struct CubicSaturator
{
    static constexpr float knee = 1.5f;
    static constexpr float cubic = 4.0f / 27.0f;

    [[nodiscard]] static float value(float x) noexcept
    {
        const float c = std::clamp(x, -knee, knee);
        return c - (cubic * c * c * c);
    }
    [[nodiscard]] static float slope(float x, float /*value*/) noexcept
    {
        const float c = std::clamp(x, -knee, knee);
        return 1.0f - (3.0f * cubic * c * c);
    }
    [[nodiscard]] static float secant(float x) noexcept
    {
        if (std::fabs(x) <= knee)
        {
            return 1.0f - (cubic * x * x);
        }
        return 1.0f / std::fabs(x);
    }
};

/// One channel's state between samples.
struct LadderState
{
    std::array<float, 4> s{}; ///< the integrators' states
    float previousU = 0.0f;   ///< the loop input, the input saturator's estimate
};

/// The loop's solution for one sample.
struct LadderSolution
{
    float u = 0.0f; ///< t(x - k y4), the first stage's input
    std::array<float, 4> y{};
};

/// What one solve() or solveNested() did. Filled only when asked for (tests
/// and reports).
struct LadderSolveStats
{
    std::uint32_t newtonIterations = 0;
    bool fellBack = false;              ///< Newton did not converge, so the nested solve ran
    std::uint32_t rows = 0;             ///< the nested solve's evaluations of the last row
    std::uint32_t stageEvaluations = 0; ///< the nested solve's stage evaluations, the final chain included
    /// Newton's last step was under the tolerance; or the nested solve's last
    /// row step was, with every stage solve converged.
    bool converged = false;
};

class LadderSolver
{
public:
    /// Newton's outcome: the solution, and the iterations it took (0: it did not converge).
    struct NewtonOutcome
    {
        LadderSolution solution;
        std::uint32_t iterations = 0;
    };

    static constexpr float newtonTolerance = 1e-5f;
    static constexpr std::uint32_t maxNewtonIterations = 8;

    /// The nested solve's caps: evaluations of the last row, and of each
    /// stage per solve. Measured worst: 19 rows (docs/dsp/filter-stability.md).
    static constexpr std::uint32_t maxRows = 64;
    static constexpr std::uint32_t maxStageEvaluations = 64;

    /// One sample, updating the state. A non-finite x is taken as 0, so a
    /// NaN or inf on the input never reaches the state.
    ///
    /// Newton-Raphson from the loop linearised at the state, to
    /// newtonTolerance, at most maxNewtonIterations (the Jacobian is lower
    /// bidiagonal plus the feedback corner, so each step writes
    /// Delta_i = p_i + q_i Delta_4 and solves the last row). Where Newton
    /// converges this is the whole solve. Where it doesn't (near Nyquist, at
    /// high drive and resonance), the nested bracketing solve, restarted from
    /// the linearised guess: it cannot fail, because each stage and the last
    /// row are monotone scalar equations with known brackets
    /// (docs/dsp/filter-stability.md).
    template <typename Sat>
    [[nodiscard]] static LadderSolution solve(LadderState& state, float x, float g, float k,
                                              LadderSolveStats* stats = nullptr) noexcept
    {
        const auto newtonOutcome = solveByNewton<Sat>(state, x, g, k);
        if (newtonOutcome.iterations > 0)
        {
            if (stats != nullptr)
            {
                *stats = {.newtonIterations = newtonOutcome.iterations, .converged = true};
            }
            return newtonOutcome.solution;
        }
        auto result = solveNested<Sat>(state, x, g, k, stats);
        if (stats != nullptr)
        {
            stats->newtonIterations = maxNewtonIterations;
        }
        return result;
    }

    /// The nested solve alone, from the linearised guess, whether or not
    /// Newton would converge: what solve() falls back to. LadderFilter calls
    /// it where solveByNewton() did not converge; tests call it from any
    /// state, including those Newton solves.
    template <typename Sat>
    [[nodiscard]] static LadderSolution solveNested(LadderState& state, float x, float g, float k,
                                                    LadderSolveStats* stats = nullptr) noexcept
    {
        if (!std::isfinite(x))
        {
            x = 0.0f;
        }
        LadderSolution result = newton<Sat, 0>(state, x, g, k).solution; // the linearised guess
        if (stats != nullptr)
        {
            *stats = {};
        }
        nestedSolve<Sat>(state, x, g, k, result.y, stats);
        finish<Sat>(state, x, k, result);
        return result;
    }

    /// Newton alone, the first half of solve(): with iterations > 0 the
    /// sample is solved and the state updated; with 0, Newton did not
    /// converge, the state is untouched, and the caller runs solveNested().
    /// LadderFilter calls the two halves itself (see newton()).
    template <typename Sat>
    [[nodiscard]] static NewtonOutcome solveByNewton(LadderState& state, float x, float g, float k) noexcept
    {
        return newton<Sat, maxNewtonIterations>(state, std::isfinite(x) ? x : 0.0f, g, k);
    }

private:
    /// The loop linearised at the state, then up to Iterations Newton steps.
    /// If one converges, finishes the sample (the state updated) and returns
    /// the iterations taken; otherwise returns 0 with the state untouched and
    /// the last iterate in the solution (with Iterations 0, the linearised
    /// guess, which is how solveNested() restarts from it).
    ///
    /// In the ordinary case this is the whole per-sample cost, so it calls
    /// nothing and returns by value. A call anywhere in it, even one never
    /// taken, makes every sample save and restore the callee-saved registers
    /// (measured: +10% on the filter with tanh); so does returning through an
    /// out-parameter (+2-5%). The fallback's call sits one level up, in the
    /// filter's run of up to 32 samples.
    template <typename Sat, std::uint32_t Iterations>
    [[nodiscard]] static NewtonOutcome newton(LadderState& state, float x, float g, float k) noexcept
    {
        auto& s = state.s;
        NewtonOutcome outcome;
        auto& result = outcome.solution;
        auto& y = result.y;

        // The first guess: each t(v) replaced by v t(v0)/v0 at an estimate v0
        // from the state; the loop is then linear and solved in closed form.
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

        std::uint32_t iteration = 0;
        if constexpr (Iterations > 0)
        {
            for (; iteration < Iterations; ++iteration)
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
                // 1 - q4 >= 1: every q_i <= 0 because the slopes are >= 0.
                const float delta4 = p[3] / (1.0f - q[3]);
                float largest = 0.0f;
                for (std::size_t i = 0; i < 4; ++i)
                {
                    const float delta = i == 3 ? delta4 : p[i] + (q[i] * delta4);
                    y[i] += delta;
                    largest = std::max(largest, std::fabs(delta));
                }
                if (largest < newtonTolerance)
                {
                    break;
                }
            }
        }
        if (iteration == Iterations) // no step under the tolerance
        {
            return outcome;
        }
        outcome.iterations = iteration + 1;
        const float u = x - (k * y[3]);
        result.u = Sat::value(u);
        for (std::size_t i = 0; i < 4; ++i)
        {
            s[i] = flushDenormal((2.0f * y[i]) - s[i]);
        }
        state.previousU = flushDenormal(u);
        return outcome;
    }

    /// The loop's input saturator at the solution, and the integrators' update.
    template <typename Sat>
    static void finish(LadderState& state, float x, float k, LadderSolution& result) noexcept
    {
        const float u = x - (k * result.y[3]);
        result.u = Sat::value(u);
        for (std::size_t i = 0; i < 4; ++i)
        {
            state.s[i] = flushDenormal((2.0f * result.y[i]) - state.s[i]);
        }
        state.previousU = flushDenormal(u);
    }

    /// A stage's root, with the saturator and its slope there.
    struct StageRoot
    {
        float y = 0.0f;
        float t = 0.0f;
        float slope = 0.0f;
        bool converged = false;
    };

    /// One rtsafe step on an increasing f (Numerical Recipes, 2nd ed., 9.4),
    /// given f and f' > 0 at y and the bracket [lo, hi] already narrowed by
    /// f's sign: Newton's step, or a bisection when Newton's would leave the
    /// bracket or would not be at most half the step before last. Without
    /// the second rule Newton can ping-pong between the bracket's ends (a
    /// tanh stage at g = 64: a residual of -118 after 48 steps). Divides
    /// only when it takes Newton's step.
    struct Safeguard
    {
        float lo = 0.0f;
        float hi = 0.0f;
        float stepBeforeLast = 0.0f;
        float lastStep = 0.0f;

        Safeguard(float low, float high) noexcept : lo(low), hi(high), stepBeforeLast(high - low), lastStep(high - low)
        {
        }

        /// The next y, and lastStep set to the step taken.
        [[nodiscard]] float next(float y, float f, float slope) noexcept
        {
            // y - f / slope in (lo, hi), with slope > 0, without dividing.
            const bool inside = (y - lo) * slope > f && (hi - y) * slope > -f;
            const bool fast = std::fabs(2.0f * f) <= std::fabs(stepBeforeLast * slope);
            stepBeforeLast = lastStep;
            const float to = inside && fast ? y - (f / slope) : lo + (0.5f * (hi - lo));
            lastStep = to - y;
            return to;
        }
    };

    /// y + g t(y) = c, increasing with slope 1 + g t'(y) >= 1: its root is
    /// unique and in [c - g, c + g]. rtsafe from the root of the stage with t
    /// replaced by a hard clip (slope 1 to |y| = 1, then +-1): c / (1 + g)
    /// inside, c -+ g outside, in the bracket and close for large g whichever
    /// side the root is on. Stops when the residual is at the rounding level
    /// of its terms (about |c| + g, each rounded to 2^-24 of itself), or a
    /// step is under 1e-7 |y|.
    template <typename Sat>
    [[nodiscard]] static StageRoot stageSolve(float c, float g, std::uint32_t& evaluations) noexcept
    {
        Safeguard bracket{c - g, c + g};
        const float inside = c / (1.0f + g);
        float y = std::clamp(std::fabs(inside) <= 1.0f ? inside : c - std::copysign(g, c), bracket.lo, bracket.hi);
        const float rounding = 4.0f * std::numeric_limits<float>::epsilon() * (std::fabs(c) + g);
        for (std::uint32_t n = 0; n < maxStageEvaluations; ++n)
        {
            const float t = Sat::value(y);
            const float d = Sat::slope(y, t);
            const float f = y + (g * t) - c;
            ++evaluations;
            if (std::fabs(f) <= rounding)
            {
                return {.y = y, .t = t, .slope = d, .converged = true};
            }
            (f > 0.0f ? bracket.hi : bracket.lo) = y;
            const float before = y;
            y = bracket.next(y, f, 1.0f + (g * d));
            if (std::fabs(bracket.lastStep) <= 1e-7f * std::fabs(before))
            {
                const float tn = Sat::value(y);
                return {.y = y, .t = tn, .slope = Sat::slope(y, tn), .converged = true};
            }
        }
        const float t = Sat::value(y);
        return {.y = y, .t = t, .slope = Sat::slope(y, t), .converged = false};
    }

    /// The chain for a trial y4: u = x - k y4, then each stage from its input
    /// s_i + g t(y_(i-1)). Fills y with the stages' roots and returns
    /// dH/dy4, H being the y4 the chain returns (y[3]).
    template <typename Sat>
    static float chain(const LadderState& state, float x, float g, float k, float y4, std::array<float, 4>& y,
                       std::uint32_t& evaluations, bool& stagesConverged) noexcept
    {
        const float u = x - (k * y4);
        float c = Sat::value(u);
        float derivative = -k * Sat::slope(u, c);
        for (std::size_t i = 0; i < 4; ++i)
        {
            const auto root = stageSolve<Sat>(state.s[i] + (g * c), g, evaluations);
            stagesConverged = stagesConverged && root.converged;
            y[i] = root.y;
            derivative *= g / (1.0f + (g * root.slope));
            if (i < 3)
            {
                derivative *= root.slope;
            }
            c = root.t;
        }
        return derivative;
    }

    /// The nested bracketing solve from y (the linearised guess): rtsafe on
    /// the last row phi(y4) = y4 - H(y4), increasing with slope >= 1, over
    /// [s4 - 2g, s4 + 2g], to newtonTolerance; then the chain at the root.
    template <typename Sat>
    static void nestedSolve(const LadderState& state, float x, float g, float k, std::array<float, 4>& y,
                            LadderSolveStats* stats) noexcept
    {
        const float s4 = state.s[3];
        Safeguard bracket{s4 - (2.0f * g), s4 + (2.0f * g)};
        float y4 = std::clamp(y[3], bracket.lo, bracket.hi);
        std::uint32_t evaluations = 0;
        bool stagesConverged = true;
        bool rowConverged = false;
        std::uint32_t rows = 0;
        while (rows < maxRows)
        {
            const float derivative = chain<Sat>(state, x, g, k, y4, y, evaluations, stagesConverged);
            ++rows;
            const float phi = y4 - y[3];
            if (std::fabs(phi) <= 0.0f) // exactly on the root
            {
                rowConverged = true;
                break;
            }
            (phi > 0.0f ? bracket.hi : bracket.lo) = y4;
            y4 = bracket.next(y4, phi, 1.0f - derivative);
            if (std::fabs(bracket.lastStep) <= newtonTolerance)
            {
                rowConverged = true;
                // The stages at the root. A step that rounded to nothing left
                // y4 where the chain was just solved, and solving it again
                // would repeat it exactly.
                if (std::fabs(bracket.lastStep) > 0.0f)
                {
                    static_cast<void>(chain<Sat>(state, x, g, k, y4, y, evaluations, stagesConverged));
                }
                break;
            }
        }
        y[3] = y4;
        if (stats != nullptr)
        {
            stats->fellBack = true;
            stats->rows = rows;
            stats->stageEvaluations = evaluations;
            stats->converged = rowConverged && stagesConverged;
        }
    }
};

} // namespace anoesis::dsp
