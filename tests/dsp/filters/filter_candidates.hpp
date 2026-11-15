// Test-only prototypes of filter models, measured by the [filter-report] and
// the [filter] benchmarks to choose one (docs/dsp/filters.md).
//
// Every ladder here is four one-pole lowpass stages in a loop with negative
// feedback k from the last stage, the structure the manual describes ("The
// filter contains four filter stages and each stage incorporates a non-linear
// saturation function"). The six responses are the same for every ladder:
// fixed mixes of five taps, the first stage's input u and the four stage
// outputs y1..y4 (the Oberheim Xpander's method), from a table of data
// (tapTable). The linear prototype of every response is
//
//   H(s) = sum_i c_i (1 + s)^(4 - i) / ((1 + s)^4 + k),   s = j f / fc,
//
// so the loop resonates at the cutoff in every mode and self-oscillates at
// k = 4. The ladders differ in how they discretise it and where they
// saturate:
//
// - LinearZdf: zero-delay feedback (Zavalishin's topology-preserving
//   transform): trapezoidal one-poles, cutoff prewarped, the feedback loop
//   solved exactly. No saturation. The linear reference.
// - NewtonZdf<Sat>: the nonlinear ladder dy_i/dt = wc (t(y_(i-1)) - t(y_i)),
//   y_0 = x - k y4 (five saturators: the input and the four stages), the
//   trapezoidal rule, and the implicit equations solved by Newton-Raphson to
//   a tolerance. The Jacobian is lower bidiagonal plus the feedback corner,
//   so each iteration costs O(4). Started from the linearised solution below.
// - LinearisedZdf<Sat>: the same system with each t(v) replaced by v times
//   t(v0)/v0 at an estimate v0 from the state (Teemu Voipio's "cheap
//   non-linear zero-delay filters"): five saturator calls and one linear solve
//   per sample, no iteration.
// - FeedbackOnlyZdf<Sat>: linear trapezoidal stages, one saturator on the
//   loop's input t(x - k y4), solved exactly by a scalar Newton iteration.
// - Huovilainen<Sat>: Huovilainen's explicit nonlinear ladder (DAFx 2004):
//   each stage y += g (t(in) - t(y)), the feedback from the last stage
//   delayed by one sample and averaged over two (a half-sample correction).
//   Meant to run oversampled (Oversampled<> below). Two tunings: naive
//   (g = 1 - e^-w, k as given) and exact (g and k solved in closed form so
//   the linearised loop oscillates at the cutoff when k = 4; derived in the
//   doc).
// - UnitDelayLadder<Sat>: the non-ZDF baseline: trapezoidal stages with the
//   cutoff prewarped, the feedback taken from the previous sample's output,
//   one saturator on the input. Uncompensated.
// - LinearSvf: Zavalishin's trapezoidal state-variable filter, 12 dB only,
//   R = 1 - k/4. A reference for the SVF question, not a ladder.
//
// Saturators (Sat) are a separate axis, for the Warm Drive question: tanh
// (dsp::fastTanh), a cubic soft clip (3rd harmonic only below its knee), the
// algebraic sigmoid x / sqrt(1 + x^2), an asymmetric tanh (even harmonics),
// and the identity. Each has unit slope at 0 and a value() and a slope().
//
// Shared rules: prepare(rate) at the rate the model runs at; setters apply
// from the next sample; process(float) is one sample; drive is a gain on the
// input with no make-up; recursive state is flushed with flushDenormal.
// Nothing allocates after prepare().

#pragma once

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/fast_math.hpp>
#include <anoesis/dsp/resampling/oversampler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace anoesis::test::filters
{

// --- Responses as data ----------------------------------------------------

/// The six filter responses. Bypass is not a response: it skips the filter.
enum class Response : std::uint8_t
{
    lp24,
    lp12,
    bp24,
    bp12,
    hp24,
    hp12,
};

inline constexpr std::array<Response, 6> allResponses{
    Response::lp24, Response::lp12, Response::bp24, Response::bp12, Response::hp24, Response::hp12,
};

using TapMix = std::array<float, 5>;

/// The weights of u, y1, y2, y3, y4 for each response. The bandpasses are
/// scaled for a unit peak at k = 0 (BP12: 2 s/(1+s)^2; BP24: 4 s^2/(1+s)^4).
inline constexpr std::array<TapMix, 6> tapTable{
    TapMix{0.0f, 0.0f, 0.0f, 0.0f, 1.0f},   // LP24: 1/(1+s)^4
    TapMix{0.0f, 0.0f, 1.0f, 0.0f, 0.0f},   // LP12: 1/(1+s)^2 (in the 4-pole loop)
    TapMix{0.0f, 0.0f, 4.0f, -8.0f, 4.0f},  // BP24: 4 s^2/(1+s)^4
    TapMix{0.0f, 2.0f, -2.0f, 0.0f, 0.0f},  // BP12: 2 s/(1+s)^2
    TapMix{1.0f, -4.0f, 6.0f, -4.0f, 1.0f}, // HP24: s^4/(1+s)^4
    TapMix{1.0f, -2.0f, 1.0f, 0.0f, 0.0f},  // HP12: s^2/(1+s)^2
};

[[nodiscard]] constexpr const TapMix& tapsOf(Response response) noexcept
{
    return tapTable[static_cast<std::size_t>(response)];
}

[[nodiscard]] constexpr const char* responseName(Response response) noexcept
{
    constexpr std::array<const char*, 6> names{"LP24", "LP12", "BP24", "BP12", "HP24", "HP12"};
    return names[static_cast<std::size_t>(response)];
}

[[nodiscard]] constexpr bool isTwelve(Response response) noexcept
{
    return response == Response::lp12 || response == Response::bp12 || response == Response::hp12;
}

[[nodiscard]] inline float mixTaps(const TapMix& c, float u, float y1, float y2, float y3, float y4) noexcept
{
    return (c[0] * u) + (c[1] * y1) + (c[2] * y2) + (c[3] * y3) + (c[4] * y4);
}

/// The analog prototype of a ladder response at f / fc, in double.
[[nodiscard]] inline std::complex<double> analogLadder(Response response, double k, double frequencyOverCutoff)
{
    const std::complex<double> s{0.0, frequencyOverCutoff};
    const auto p = 1.0 + s;
    const std::array<std::complex<double>, 5> powers{p * p * p * p, p * p * p, p * p, p, 1.0};
    const auto& c = tapsOf(response);
    std::complex<double> numerator{0.0, 0.0};
    for (std::size_t i = 0; i < 5; ++i)
    {
        numerator += static_cast<double>(c[i]) * powers[i];
    }
    return numerator / (powers[0] + k);
}

/// The analog prototype of the state-variable filter (12 dB responses only).
[[nodiscard]] inline std::complex<double> analogSvf(Response response, double k, double frequencyOverCutoff)
{
    const std::complex<double> s{0.0, frequencyOverCutoff};
    const double r = std::max(0.0, 1.0 - (k / 4.0));
    const auto denominator = (s * s) + (2.0 * r * s) + 1.0;
    switch (response)
    {
    case Response::bp12:
        return s / denominator;
    case Response::hp12:
        return (s * s) / denominator;
    default:
        return 1.0 / denominator;
    }
}

// --- Saturators ---------------------------------------------------------

struct LinearSat
{
    static constexpr const char* name = "linear";
    [[nodiscard]] static float value(float x) noexcept
    {
        return x;
    }
    [[nodiscard]] static float slope(float /*x*/, float /*value*/) noexcept
    {
        return 1.0f;
    }
};

/// tanh (dsp::fastTanh): every odd harmonic, a smooth knee.
struct TanhSat
{
    static constexpr const char* name = "tanh";
    [[nodiscard]] static float value(float x) noexcept
    {
        return dsp::fastTanh(x);
    }
    [[nodiscard]] static float slope(float /*x*/, float value) noexcept
    {
        return 1.0f - (value * value);
    }
};

/// x - (4/27) x^3 up to |x| = 1.5, then +-1: a pure 3rd harmonic below the
/// knee, slope 0 at it. The cheapest saturator here (no division, no exp).
struct CubicSat
{
    static constexpr const char* name = "cubic";
    [[nodiscard]] static float value(float x) noexcept
    {
        const float c = std::clamp(x, -1.5f, 1.5f);
        return c - ((4.0f / 27.0f) * c * c * c);
    }
    [[nodiscard]] static float slope(float x, float /*value*/) noexcept
    {
        const float c = std::clamp(x, -1.5f, 1.5f);
        return 1.0f - ((4.0f / 9.0f) * c * c);
    }
};

/// x / sqrt(1 + x^2): every odd harmonic, a slower knee than tanh.
struct AlgebraicSat
{
    static constexpr const char* name = "algebraic";
    [[nodiscard]] static float value(float x) noexcept
    {
        return x / std::sqrt(1.0f + (x * x));
    }
    [[nodiscard]] static float slope(float x, float /*value*/) noexcept
    {
        const float r = 1.0f / std::sqrt(1.0f + (x * x));
        return r * r * r;
    }
};

/// (tanh(x + b) - tanh(b)) / (1 - tanh(b)^2), b = 0.25: tanh moved off its
/// centre, so it adds even harmonics ("tube nuance"), with t(0) = 0 and unit
/// slope at 0.
struct AsymmetricSat
{
    static constexpr const char* name = "asymmetric tanh";
    static constexpr float bias = 0.25f;
    [[nodiscard]] static float tanhBias() noexcept
    {
        static const float value = dsp::fastTanh(bias);
        return value;
    }
    [[nodiscard]] static float value(float x) noexcept
    {
        const float tb = tanhBias();
        return (dsp::fastTanh(x + bias) - tb) / (1.0f - (tb * tb));
    }
    [[nodiscard]] static float slope(float x, float /*value*/) noexcept
    {
        const float tb = tanhBias();
        const float t = dsp::fastTanh(x + bias);
        return (1.0f - (t * t)) / (1.0f - (tb * tb));
    }
};

/// t(v) / v, the saturator's secant gain, 1 at 0.
template <typename Sat>
[[nodiscard]] float secantGain(float v) noexcept
{
    return std::fabs(v) < 1e-4f ? 1.0f : Sat::value(v) / v;
}

// --- Shared controls ----------------------------------------------------

/// tan(pi fc / fs), the trapezoidal integrator's gain with the cutoff
/// prewarped, fc clamped to [1 Hz, 0.495 fs].
[[nodiscard]] inline float prewarpedGain(double cutoffHz, double sampleRate) noexcept
{
    const double fc = std::clamp(cutoffHz, 1.0, 0.495 * sampleRate);
    return static_cast<float>(std::tan(std::numbers::pi * fc / sampleRate));
}

/// The controls every model has, and what the trapezoidal ladders derive.
class LadderControls
{
public:
    void setCutoff(double hz) noexcept
    {
        cutoffHz = hz;
        update();
    }
    void setResonance(float k) noexcept
    {
        resonance = std::max(0.0f, k);
    }
    void setDrive(float gain) noexcept
    {
        drive = gain;
    }
    void setResponse(Response r) noexcept
    {
        response = r;
        taps = tapsOf(r);
    }

protected:
    void prepareControls(double rate) noexcept
    {
        sampleRate = rate;
        update();
    }

    double sampleRate = 48000.0;
    double cutoffHz = 1000.0;
    float g = 0.0f;
    float resonance = 0.0f;
    float drive = 1.0f;
    Response response = Response::lp24;
    TapMix taps = tapsOf(Response::lp24);

private:
    void update() noexcept
    {
        g = prewarpedGain(cutoffHz, sampleRate);
    }
};

// --- The models -----------------------------------------------------------

/// The linear ZDF ladder (TPT): the loop solved exactly,
/// u = (x - k S) / (1 + k G^4), G = g / (1 + g), S the stages' state seen at
/// the output.
class LinearZdf : public LadderControls
{
public:
    static constexpr const char* name = "Linear ZDF";

    void prepare(double rate) noexcept
    {
        prepareControls(rate);
        reset();
    }
    void reset() noexcept
    {
        s = {};
    }
    [[nodiscard]] float process(float x) noexcept
    {
        x *= drive;
        const float big = g / (1.0f + g);
        const float inv = 1.0f / (1.0f + g);
        const float sum = (big * big * big * s[0] * inv) + (big * big * s[1] * inv) + (big * s[2] * inv) + (s[3] * inv);
        const float big4 = big * big * big * big;
        const float u = (x - (resonance * sum)) / (1.0f + (resonance * big4));
        std::array<float, 4> y{};
        float in = u;
        for (std::size_t i = 0; i < 4; ++i)
        {
            y[i] = (big * in) + (s[i] * inv);
            s[i] = dsp::flushDenormal((2.0f * y[i]) - s[i]);
            in = y[i];
        }
        return mixTaps(taps, u, y[0], y[1], y[2], y[3]);
    }
    [[nodiscard]] std::span<const float> state() const noexcept
    {
        return s;
    }

private:
    std::array<float, 4> s{};
};

/// Statistics of an iterative solver, for the report.
struct SolverStats
{
    std::uint64_t samples = 0;
    std::uint64_t iterations = 0;
    std::uint32_t mostIterations = 0;
    std::uint64_t unconverged = 0;
};

/// The trapezoidal nonlinear ladder, linearised at the state (one solve, no
/// iteration). Used on its own (LinearisedZdf) and as Newton's first guess.
template <typename Sat>
struct LinearisedSolve
{
    /// Solves for y1..y4 and u given the state s, the previous loop input
    /// (the estimate for the input saturator), g, k, and x.
    static void solve(const std::array<float, 4>& s, float previousU, float g, float k, float x,
                      std::array<float, 4>& y, float& u, float& inputGain) noexcept
    {
        inputGain = secantGain<Sat>(previousU);
        std::array<float, 4> sigma{};
        for (std::size_t i = 0; i < 4; ++i)
        {
            sigma[i] = secantGain<Sat>(s[i]);
        }
        // y_i = a_i y_(i-1) + b_i, y_0 = u: compose to y4 = A u + B.
        float a = 1.0f;
        float b = 0.0f;
        std::array<float, 4> ai{};
        std::array<float, 4> bi{};
        for (std::size_t i = 0; i < 4; ++i)
        {
            const float inGain = i == 0 ? inputGain : sigma[i - 1];
            const float denominator = 1.0f / (1.0f + (g * sigma[i]));
            ai[i] = g * inGain * denominator;
            bi[i] = s[i] * denominator;
            a *= ai[i];
            b = (ai[i] * b) + bi[i];
        }
        u = (x - (k * b)) / (1.0f + (k * a));
        float in = u;
        for (std::size_t i = 0; i < 4; ++i)
        {
            y[i] = (ai[i] * in) + bi[i];
            in = y[i];
        }
    }
};

/// The linearised ZDF ladder: five secant gains from the state, one linear
/// solve.
template <typename Sat>
class LinearisedZdf : public LadderControls
{
public:
    static constexpr const char* name = "Linearised ZDF";

    void prepare(double rate) noexcept
    {
        prepareControls(rate);
        reset();
    }
    void reset() noexcept
    {
        s = {};
        previousU = 0.0f;
    }
    [[nodiscard]] float process(float x) noexcept
    {
        x *= drive;
        std::array<float, 4> y{};
        float u = 0.0f;
        float inputGain = 1.0f;
        LinearisedSolve<Sat>::solve(s, previousU, g, resonance, x, y, u, inputGain);
        for (std::size_t i = 0; i < 4; ++i)
        {
            s[i] = dsp::flushDenormal((2.0f * y[i]) - s[i]);
        }
        previousU = dsp::flushDenormal(u);
        return mixTaps(taps, inputGain * u, y[0], y[1], y[2], y[3]);
    }
    [[nodiscard]] std::span<const float> state() const noexcept
    {
        return s;
    }

private:
    std::array<float, 4> s{};
    float previousU = 0.0f;
};

/// The ZDF ladder with a saturator at the input and in every stage, solved by
/// Newton-Raphson. maxIterations 1 is "the linearised guess plus one Newton
/// step".
template <typename Sat, std::uint32_t MaxIterations = 8>
class NewtonZdf : public LadderControls
{
public:
    static constexpr const char* name = MaxIterations == 1 ? "Newton ZDF, 1 step" : "Newton ZDF";
    static constexpr float tolerance = 1e-5f;

    void prepare(double rate) noexcept
    {
        prepareControls(rate);
        reset();
    }
    void reset() noexcept
    {
        s = {};
        previousU = 0.0f;
    }
    [[nodiscard]] float process(float x) noexcept
    {
        x *= drive;
        std::array<float, 4> y{};
        float u = 0.0f;
        float inputGain = 1.0f;
        LinearisedSolve<Sat>::solve(s, previousU, g, resonance, x, y, u, inputGain);

        const float k = resonance;
        std::uint32_t iteration = 0;
        bool converged = false;
        float tu = 0.0f;
        while (iteration < MaxIterations)
        {
            ++iteration;
            u = x - (k * y[3]);
            tu = Sat::value(u);
            const float du = Sat::slope(u, tu);
            std::array<float, 4> t{};
            std::array<float, 4> d{};
            for (std::size_t i = 0; i < 4; ++i)
            {
                t[i] = Sat::value(y[i]);
                d[i] = Sat::slope(y[i], t[i]);
            }
            // F_i = y_i - s_i - g (t(y_(i-1)) - t(y_i)), y_0 = u = x - k y4.
            // Delta_i = p_i + q_i Delta_4, then Delta_4 from the last row.
            std::array<float, 4> p{};
            std::array<float, 4> q{};
            float previousT = tu;
            for (std::size_t i = 0; i < 4; ++i)
            {
                const float residual = y[i] - s[i] - (g * (previousT - t[i]));
                const float diagonal = 1.0f + (g * d[i]);
                if (i == 0)
                {
                    p[0] = -residual / diagonal;
                    q[0] = -(g * k * du) / diagonal;
                }
                else
                {
                    p[i] = (-residual + (g * d[i - 1] * p[i - 1])) / diagonal;
                    q[i] = (g * d[i - 1] * q[i - 1]) / diagonal;
                }
                previousT = t[i];
            }
            const float delta4 = p[3] / (1.0f - q[3]);
            float largest = 0.0f;
            for (std::size_t i = 0; i < 4; ++i)
            {
                const float delta = i == 3 ? delta4 : p[i] + (q[i] * delta4);
                y[i] += delta;
                largest = std::max(largest, std::fabs(delta));
            }
            if (largest < tolerance)
            {
                converged = true;
                break;
            }
        }
        u = x - (k * y[3]);
        tu = Sat::value(u);
        for (std::size_t i = 0; i < 4; ++i)
        {
            s[i] = dsp::flushDenormal((2.0f * y[i]) - s[i]);
        }
        previousU = dsp::flushDenormal(u);
        ++stats.samples;
        stats.iterations += iteration;
        stats.mostIterations = std::max(stats.mostIterations, iteration);
        stats.unconverged += converged ? 0U : 1U;
        return mixTaps(taps, tu, y[0], y[1], y[2], y[3]);
    }
    [[nodiscard]] std::span<const float> state() const noexcept
    {
        return s;
    }

    SolverStats stats{};

private:
    std::array<float, 4> s{};
    float previousU = 0.0f;
};

/// Linear trapezoidal stages, one saturator on the loop input: y4 = G^4 t(x -
/// k y4) + S, solved for y4 by a scalar Newton iteration (monotonic, so it
/// converges from the linear solution).
template <typename Sat>
class FeedbackOnlyZdf : public LadderControls
{
public:
    static constexpr const char* name = "Feedback-only ZDF";
    static constexpr float tolerance = 1e-6f;
    static constexpr std::uint32_t maxIterations = 8;

    void prepare(double rate) noexcept
    {
        prepareControls(rate);
        reset();
    }
    void reset() noexcept
    {
        s = {};
    }
    [[nodiscard]] float process(float x) noexcept
    {
        x *= drive;
        const float big = g / (1.0f + g);
        const float inv = 1.0f / (1.0f + g);
        const float sum = (big * big * big * s[0] * inv) + (big * big * s[1] * inv) + (big * s[2] * inv) + (s[3] * inv);
        const float big4 = big * big * big * big;
        const float k = resonance;
        float y4 = ((big4 * x) + sum) / (1.0f + (k * big4));
        std::uint32_t iteration = 0;
        bool converged = false;
        while (iteration < maxIterations)
        {
            ++iteration;
            const float u = x - (k * y4);
            const float tu = Sat::value(u);
            const float f = y4 - (big4 * tu) - sum;
            const float derivative = 1.0f + (big4 * k * Sat::slope(u, tu));
            const float delta = f / derivative;
            y4 -= delta;
            if (std::fabs(delta) < tolerance)
            {
                converged = true;
                break;
            }
        }
        const float v = Sat::value(x - (k * y4));
        std::array<float, 4> y{};
        float in = v;
        for (std::size_t i = 0; i < 4; ++i)
        {
            y[i] = (big * in) + (s[i] * inv);
            s[i] = dsp::flushDenormal((2.0f * y[i]) - s[i]);
            in = y[i];
        }
        ++stats.samples;
        stats.iterations += iteration;
        stats.mostIterations = std::max(stats.mostIterations, iteration);
        stats.unconverged += converged ? 0U : 1U;
        return mixTaps(taps, v, y[0], y[1], y[2], y[3]);
    }
    [[nodiscard]] std::span<const float> state() const noexcept
    {
        return s;
    }

    SolverStats stats{};

private:
    std::array<float, 4> s{};
};

/// The non-ZDF baseline: trapezoidal stages with the cutoff prewarped, the
/// feedback from the previous sample's y4, a saturator on the input. No
/// tuning or resonance compensation, so the unit delay detunes the
/// resonance and moves the self-oscillation threshold.
template <typename Sat>
class UnitDelayLadder : public LadderControls
{
public:
    static constexpr const char* name = "Unit-delay ladder";

    void prepare(double rate) noexcept
    {
        prepareControls(rate);
        reset();
    }
    void reset() noexcept
    {
        s = {};
        previousY4 = 0.0f;
    }
    [[nodiscard]] float process(float x) noexcept
    {
        x *= drive;
        const float big = g / (1.0f + g);
        const float inv = 1.0f / (1.0f + g);
        const float v = Sat::value(x - (resonance * previousY4));
        std::array<float, 4> y{};
        float in = v;
        for (std::size_t i = 0; i < 4; ++i)
        {
            y[i] = (big * in) + (s[i] * inv);
            s[i] = dsp::flushDenormal((2.0f * y[i]) - s[i]);
            in = y[i];
        }
        previousY4 = y[3];
        return mixTaps(taps, v, y[0], y[1], y[2], y[3]);
    }
    [[nodiscard]] std::span<const float> state() const noexcept
    {
        return s;
    }

private:
    std::array<float, 4> s{};
    float previousY4 = 0.0f;
};

/// How Huovilainen's ladder maps the cutoff and resonance.
enum class HuovilainenTuning : std::uint8_t
{
    naive, ///< g = 1 - e^(-2 pi fc / fs), k as given
    exact, ///< g and k from the linearised loop's phase and gain at fc (doc)
};

/// Huovilainen's explicit nonlinear ladder. Each stage
/// y_i += g (t(y_(i-1)) - t(y_i)), with t(y_i) kept from the previous
/// sample; the loop input u = x - k (y4[n-1] + y4[n-2]) / 2.
template <typename Sat, HuovilainenTuning Tuning>
class Huovilainen
{
public:
    static constexpr const char* name =
        Tuning == HuovilainenTuning::exact ? "Huovilainen, exact tuning" : "Huovilainen, naive tuning";

    void prepare(double rate) noexcept
    {
        sampleRate = rate;
        update();
        reset();
    }
    void reset() noexcept
    {
        y = {};
        w = {};
        previousY4 = 0.0f;
        feedback = 0.0f;
    }
    void setCutoff(double hz) noexcept
    {
        cutoffHz = hz;
        update();
    }
    void setResonance(float k) noexcept
    {
        resonance = std::max(0.0f, k);
    }
    void setDrive(float gain) noexcept
    {
        drive = gain;
    }
    void setResponse(Response r) noexcept
    {
        taps = tapsOf(r);
    }

    [[nodiscard]] float process(float x) noexcept
    {
        x *= drive;
        const float u = x - (resonance * kScale * feedback);
        const float tu = Sat::value(u);
        float in = tu;
        for (std::size_t i = 0; i < 4; ++i)
        {
            y[i] = dsp::flushDenormal(y[i] + (g * (in - w[i])));
            w[i] = Sat::value(y[i]);
            in = w[i];
        }
        feedback = 0.5f * (y[3] + previousY4);
        previousY4 = y[3];
        return mixTaps(taps, tu, y[0], y[1], y[2], y[3]);
    }
    [[nodiscard]] std::span<const float> state() const noexcept
    {
        return y;
    }

    /// The exact tuning's k at which the linearised loop oscillates, over 4
    /// (1 for the naive tuning), at the current cutoff.
    [[nodiscard]] float thresholdScale() const noexcept
    {
        return kScale;
    }

private:
    void update() noexcept
    {
        constexpr double pi = std::numbers::pi;
        if constexpr (Tuning == HuovilainenTuning::naive)
        {
            const double fc = std::clamp(cutoffHz, 1.0, 0.5 * sampleRate);
            g = static_cast<float>(1.0 - std::exp(-2.0 * pi * fc / sampleRate));
            kScale = 1.0f;
        }
        else
        {
            // The linearised loop k z^-1 (1 + z^-1)/2 H1(z)^4, H1 = g/(1 - a z^-1),
            // a = 1 - g, has phase -pi at w when each stage contributes
            // atan(a sin w / (1 - a cos w)) = pi/4 - 3w/8; solve for a. Valid for
            // w < 2 pi / 3 (fc < fs / 3).
            const double fc = std::clamp(cutoffHz, 1.0, sampleRate / 3.2);
            const double omega = 2.0 * pi * fc / sampleRate;
            const double tau = std::tan((pi / 4.0) - (3.0 * omega / 8.0));
            const double a = tau / (std::sin(omega) + (tau * std::cos(omega)));
            const double gain = 1.0 - a;
            const double stageSquared = (gain * gain) / (1.0 - (2.0 * a * std::cos(omega)) + (a * a));
            const double threshold = 1.0 / (std::cos(omega / 2.0) * stageSquared * stageSquared);
            g = static_cast<float>(gain);
            kScale = static_cast<float>(threshold / 4.0);
        }
    }

    double sampleRate = 48000.0;
    double cutoffHz = 1000.0;
    float g = 0.0f;
    float kScale = 1.0f;
    float resonance = 0.0f;
    float drive = 1.0f;
    TapMix taps = tapsOf(Response::lp24);
    std::array<float, 4> y{};
    std::array<float, 4> w{};
    float previousY4 = 0.0f;
    float feedback = 0.0f;
};

/// Zavalishin's trapezoidal state-variable filter: LP12, BP12 (unnormalised,
/// s / (s^2 + 2Rs + 1)), HP12; the other responses give the LP12. R = 1 - k/4.
class LinearSvf : public LadderControls
{
public:
    static constexpr const char* name = "Linear SVF (12 dB)";

    void prepare(double rate) noexcept
    {
        prepareControls(rate);
        reset();
    }
    void reset() noexcept
    {
        s1 = 0.0f;
        s2 = 0.0f;
    }
    [[nodiscard]] float process(float x) noexcept
    {
        x *= drive;
        const float r = std::max(0.0f, 1.0f - (resonance / 4.0f));
        const float hp = (x - (((2.0f * r) + g) * s1) - s2) / (1.0f + (2.0f * r * g) + (g * g));
        const float bp = (g * hp) + s1;
        s1 = dsp::flushDenormal((g * hp) + bp);
        const float lp = (g * bp) + s2;
        s2 = dsp::flushDenormal((g * bp) + lp);
        switch (response)
        {
        case Response::bp12:
            return bp;
        case Response::hp12:
            return hp;
        default:
            return lp;
        }
    }

private:
    float s1 = 0.0f;
    float s2 = 0.0f;
};

// --- Blocks and oversampling --------------------------------------------

inline constexpr std::size_t maxBlock = 4096;

/// Processes `io` in place, one sample at a time.
template <typename Model>
void processBlock(Model& model, std::span<float> io) noexcept
{
    for (auto& x : io)
    {
        x = model.process(x);
    }
}

/// Processes `io` in place with a cutoff per sample (set before the sample).
template <typename Model>
void processModulated(Model& model, std::span<float> io, std::span<const float> cutoffHz) noexcept
{
    for (std::size_t i = 0; i < io.size(); ++i)
    {
        model.setCutoff(static_cast<double>(cutoffHz[i]));
        io[i] = model.process(io[i]);
    }
}

/// Runs a model inside the library's Oversampler (2x or 4x), the model
/// prepared at the oversampled rate. A cutoff set between base-rate samples
/// applies to all of the next sample's inner samples.
template <typename Model>
class Oversampled
{
public:
    explicit Oversampled(dsp::OversamplingFactor factor, Model model = {}) noexcept : inner(model), oversampler(factor)
    {
    }

    void prepare(double rate)
    {
        oversampler.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = maxBlock, .numChannels = 1});
        inner.prepare(rate * static_cast<double>(oversampler.ratio()));
        reset();
    }
    void reset() noexcept
    {
        oversampler.reset();
        inner.reset();
    }
    void setCutoff(double hz) noexcept
    {
        inner.setCutoff(hz);
    }
    void setResonance(float k) noexcept
    {
        inner.setResonance(k);
    }
    void setDrive(float gain) noexcept
    {
        inner.setDrive(gain);
    }
    void setResponse(Response r) noexcept
    {
        inner.setResponse(r);
    }

    [[nodiscard]] float process(float x) noexcept
    {
        float sample = x;
        const std::array<float*, 1> channels{&sample};
        const dsp::AudioBlock block{channels.data(), 1, 1};
        for (auto& v : oversampler.upsample(block).channel(0))
        {
            v = inner.process(v);
        }
        oversampler.downsample(block);
        return sample;
    }

    /// The whole of `io` at once, in chunks of up to maxBlock.
    void processSpan(std::span<float> io) noexcept
    {
        for (std::size_t start = 0; start < io.size(); start += maxBlock)
        {
            const std::array<float*, 1> channels{io.data() + start};
            const dsp::AudioBlock block{channels.data(), 1, std::min(maxBlock, io.size() - start)};
            for (auto& v : oversampler.upsample(block).channel(0))
            {
                v = inner.process(v);
            }
            oversampler.downsample(block);
        }
    }

    void processSpanModulated(std::span<float> io, std::span<const float> cutoffHz) noexcept
    {
        const auto ratio = oversampler.ratio();
        for (std::size_t start = 0; start < io.size(); start += maxBlock)
        {
            const std::array<float*, 1> channels{io.data() + start};
            const dsp::AudioBlock block{channels.data(), 1, std::min(maxBlock, io.size() - start)};
            const auto up = oversampler.upsample(block).channel(0);
            for (std::size_t i = 0; i < up.size(); ++i)
            {
                if (i % ratio == 0)
                {
                    inner.setCutoff(static_cast<double>(cutoffHz[start + (i / ratio)]));
                }
                up[i] = inner.process(up[i]);
            }
            oversampler.downsample(block);
        }
    }

    [[nodiscard]] Model& model() noexcept
    {
        return inner;
    }
    [[nodiscard]] float latencySamples() const noexcept
    {
        return oversampler.latencySamples();
    }

private:
    Model inner;
    dsp::Oversampler oversampler;
};

template <typename Model>
void processBlock(Oversampled<Model>& model, std::span<float> io) noexcept
{
    model.processSpan(io);
}

template <typename Model>
void processModulated(Oversampled<Model>& model, std::span<float> io, std::span<const float> cutoffHz) noexcept
{
    model.processSpanModulated(io, cutoffHz);
}

} // namespace anoesis::test::filters
