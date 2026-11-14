// Test-only prototypes of oscillator drift models, measured by the
// [drift-report] to choose one (docs/dsp/oscillator-drift.md). Each model
// produces a pitch deviation in cents, one value per update, at an update
// rate it is given: every sample, or at a control rate whose values are then
// held or interpolated per sample (ControlRate below).
//
// Every model is normalised to the same standard deviation (sigma, in
// cents) and given one characteristic rate (rateHz). What the rate means
// differs by model, so the report measures each model's spectrum and quotes
// the frequencies below which half and 99% of its variance lie.
//
// Each model starts from its stationary distribution (start()), as a
// process that has been running unheard would be.

#pragma once

#include <anoesis/dsp/math/random.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace anoesis::test::drift
{

struct Settings
{
    double sigmaCents = 1.5;
    double rateHz = 0.5;
    double updateHz = 750.0;
};

/// Unit-variance noise: a uniform draw scaled by sqrt(3).
inline float unitUniform(dsp::Random& random) noexcept
{
    return random.nextBipolar() * std::numbers::sqrt3_v<float>;
}

/// Approximately Gaussian, unit variance: the sum of four uniform draws
/// (Irwin-Hall), bounded at +-2 sqrt(3).
inline float unitGaussian(dsp::Random& random) noexcept
{
    const float sum = random.nextBipolar() + random.nextBipolar() + random.nextBipolar() + random.nextBipolar();
    return sum * 1.5f; // var(sum) = 4/3, so scale by sqrt(3/4) * sqrt(3) = 1.5
}

/// One-pole lowpass-filtered white noise (an Ornstein-Uhlenbeck process, a
/// "leaky random walk"): x = a x + g w. Corner at rateHz, -20 dB/decade
/// above it.
class OnePole
{
public:
    static constexpr const char* name = "One-pole noise (OU)";

    OnePole(const Settings& s, std::uint64_t seed) noexcept : random(seed), sigma(static_cast<float>(s.sigmaCents))
    {
        const double a = std::exp(-2.0 * std::numbers::pi * s.rateHz / s.updateHz);
        pole = static_cast<float>(a);
        gain = static_cast<float>(s.sigmaCents * std::sqrt(1.0 - (a * a)));

        start();
    }

    void start() noexcept
    {
        x = sigma * unitGaussian(random);
    }

    [[nodiscard]] float next() noexcept
    {
        x = (pole * x) + (gain * unitUniform(random));
        return x;
    }

private:
    dsp::Random random;
    float pole = 0.0f;
    float gain = 0.0f;
    float sigma = 0.0f;
    float x = 0.0f;
};

/// Two one-poles in cascade, both at rateHz: -40 dB/decade above it, so the
/// path is smooth (its slope is continuous) rather than rough like the OU's.
/// Starts from the exact stationary covariance of the two states.
class TwoPole
{
public:
    static constexpr const char* name = "Two-pole noise";

    TwoPole(const Settings& s, std::uint64_t seed) noexcept : random(seed)
    {
        const double a = std::exp(-2.0 * std::numbers::pi * s.rateHz / s.updateHz);
        // Unit-variance white input w, y1 = a y1 + (1 - a) w, y2 = a y2 + (1 - a) y1.
        // Stationary moments (Lyapunov equation of the 2x2 system):
        // Stationary moments of y1' = a y1 + b w, y2' = a y2 + b y1' (y1
        // first, as next() updates them), the fixed point of
        //   m11 = a^2 m11 + b^2
        //   m12 = a^2 m12 + b m11
        //   m22 = a^2 m22 + 2 a^2 b m12 + b^2 m11
        const double b = 1.0 - a;
        const double d = 1.0 - (a * a);
        const double m11 = b * b / d;
        const double m12 = b * m11 / d;
        const double m22 = ((2.0 * a * a * b * m12) + (b * b * m11)) / d;
        pole = static_cast<float>(a);
        inputGain = static_cast<float>(b * s.sigmaCents / std::sqrt(m22));
        scale = static_cast<float>(s.sigmaCents / std::sqrt(m22));
        // Cholesky factor of the stationary covariance, scaled to sigma.
        const double s11 = std::sqrt(m11);
        const double s21 = m12 / s11;
        const double s22 = std::sqrt(std::max(m22 - (s21 * s21), 0.0));
        chol = {static_cast<float>(s11), static_cast<float>(s21), static_cast<float>(s22)};
        start();
    }

    void start() noexcept
    {
        const float u = unitGaussian(random);
        const float v = unitGaussian(random);
        y1 = scale * chol[0] * u;
        y2 = scale * ((chol[1] * u) + (chol[2] * v));
    }

    [[nodiscard]] float next() noexcept
    {
        y1 = (pole * y1) + (inputGain * unitUniform(random));
        y2 = (pole * y2) + ((1.0f - pole) * y1);
        return y2;
    }

private:
    dsp::Random random;
    float pole = 0.0f;
    float inputGain = 0.0f;
    float scale = 0.0f;
    std::array<float, 3> chol{};
    float y1 = 0.0f;
    float y2 = 0.0f;
};

/// Interpolated sample and hold: a new uniform target every 1 / rateHz
/// seconds, approached linearly (Smooth = false) or along a raised cosine
/// (Smooth = true, so the slope is 0 at each target). Bounded exactly.
template <bool Smooth>
class SampleHold
{
public:
    static constexpr const char* name = Smooth ? "Sample and hold, cosine" : "Sample and hold, linear";

    SampleHold(const Settings& s, std::uint64_t seed) noexcept
        : random(seed), step(static_cast<float>(s.rateHz / s.updateHz))
    {
        // Mean variance across a segment between independent targets of
        // variance u^2: (2/3) u^2 linear, (3/4) u^2 raised cosine.
        const double meanFactor = Smooth ? 0.75 : 2.0 / 3.0;
        halfWidth = static_cast<float>(s.sigmaCents * std::sqrt(3.0 / meanFactor));

        start();
    }

    /// The largest |deviation|: the targets' half-width.
    [[nodiscard]] float bound() const noexcept
    {
        return halfWidth;
    }

    void start() noexcept
    {
        from = halfWidth * random.nextBipolar();
        to = halfWidth * random.nextBipolar();
        t = random.nextUnipolar(); // a random point in the segment: stationary
    }

    [[nodiscard]] float next() noexcept
    {
        t += step;
        if (t >= 1.0f)
        {
            t -= 1.0f;
            from = to;
            to = halfWidth * random.nextBipolar();
        }
        const float w = Smooth ? 0.5f - (0.5f * std::cos(std::numbers::pi_v<float> * t)) : t;
        return from + (w * (to - from));
    }

private:
    dsp::Random random;
    float halfWidth = 0.0f;
    float step = 0.0f;
    float from = 0.0f;
    float to = 0.0f;
    float t = 0.0f;
};

/// A sum of three slow sines at 0.618, 1, and 1.618 x rateHz (no common
/// period), each at the same amplitude, with random phases per start.
class SumOfSines
{
public:
    static constexpr const char* name = "Sum of 3 sines";

    SumOfSines(const Settings& s, std::uint64_t seed) noexcept
        : random(seed), amplitude(static_cast<float>(s.sigmaCents * std::sqrt(2.0 / 3.0)))
    {

        constexpr std::array<double, 3> ratios{0.6180339887, 1.0, std::numbers::phi};
        for (std::size_t k = 0; k < 3; ++k)
        {
            steps[k] = static_cast<float>(ratios[k] * s.rateHz / s.updateHz);
        }
        start();
    }

    [[nodiscard]] float bound() const noexcept
    {
        return 3.0f * amplitude;
    }

    void start() noexcept
    {
        for (auto& p : phases)
        {
            p = random.nextUnipolar();
        }
    }

    [[nodiscard]] float next() noexcept
    {
        float sum = 0.0f;
        for (std::size_t k = 0; k < 3; ++k)
        {
            phases[k] += steps[k];
            phases[k] -= std::floor(phases[k]);
            sum += std::sin(2.0f * std::numbers::pi_v<float> * phases[k]);
        }
        return amplitude * sum;
    }

private:
    dsp::Random random;
    float amplitude = 0.0f;
    std::array<float, 3> steps{};
    std::array<float, 3> phases{};
};

/// 1/f ("pink") noise by the Voss-McCartney algorithm: Rows uniform rows,
/// row k redrawn every 2^(k+1) ticks of a clock at 4 x rateHz, interpolated
/// linearly between ticks. -10 dB/decade over Rows octaves below the clock,
/// flat below that (so stationary and bounded).
template <int Rows>
class Pink
{
public:
    static constexpr const char* name = "1/f noise (Voss-McCartney)";

    Pink(const Settings& s, std::uint64_t seed) noexcept
        : random(seed), step(static_cast<float>(4.0 * s.rateHz / s.updateHz))
    {
        // The sum S of Rows rows of variance u^2/3 has variance Rows u^2/3.
        // Between ticks exactly one row changes, so cov(S_n, S_n+1) =
        // var S (1 - 1/Rows), and the interpolated mean variance is
        // var S (1 - 1/(3 Rows)).
        const double count = Rows;
        const double varPerUnit = (count / 3.0) * (1.0 - (1.0 / (3.0 * count)));
        halfWidth = static_cast<float>(s.sigmaCents / std::sqrt(varPerUnit));

        start();
    }

    [[nodiscard]] float bound() const noexcept
    {
        return static_cast<float>(Rows) * halfWidth;
    }

    void start() noexcept
    {
        sum = 0.0f;
        for (auto& r : rows)
        {
            r = halfWidth * random.nextBipolar();
            sum += r;
        }
        counter = random.nextUint32(); // a random point in the row schedule
        from = sum;
        advance();
        t = random.nextUnipolar();
    }

    [[nodiscard]] float next() noexcept
    {
        t += step;
        if (t >= 1.0f)
        {
            t -= 1.0f;
            from = to;
            advance();
        }
        return from + (t * (to - from));
    }

private:
    void advance() noexcept
    {
        ++counter;
        const auto k = static_cast<std::size_t>(std::countr_zero(counter));
        if (k < rows.size())
        {
            sum -= rows[k];
            rows[k] = halfWidth * random.nextBipolar();
            sum += rows[k];
        }
        to = sum;
    }

    dsp::Random random;
    std::array<float, Rows> rows{};
    float halfWidth = 0.0f;
    float step = 0.0f;
    float sum = 0.0f;
    float from = 0.0f;
    float to = 0.0f;
    float t = 0.0f;
    std::uint32_t counter = 0;
};

/// A static offset per note: one uniform draw at start(), constant after.
class Static
{
public:
    static constexpr const char* name = "Static per note";

    Static(const Settings& s, std::uint64_t seed) noexcept
        : random(seed), halfWidth(static_cast<float>(s.sigmaCents * std::numbers::sqrt3))
    {

        start();
    }

    void start() noexcept
    {
        x = halfWidth * random.nextBipolar();
    }

    [[nodiscard]] float next() const noexcept
    {
        return x;
    }

private:
    dsp::Random random;
    float halfWidth = 0.0f;
    float x = 0.0f;
};

enum class Interpolation : std::uint8_t
{
    hold,   ///< each control value held for its interval
    linear, ///< linear from the previous control value to the new one
};

/// A model updated every `interval` samples, its values held or
/// interpolated per sample. The interval is counted from the start, not per
/// block, so the output does not depend on how a render is split.
template <typename Model>
class ControlRate
{
public:
    ControlRate(Model m, std::size_t intervalSamples, Interpolation how) noexcept
        : model(m), interval(intervalSamples), mode(how), current(model.next())
    {

        previous = current;
    }

    [[nodiscard]] float next() noexcept
    {
        if (position == interval)
        {
            position = 0;
            previous = current;
            current = model.next();
        }
        const float t = static_cast<float>(position) / static_cast<float>(interval);
        ++position;
        return mode == Interpolation::hold ? current : previous + (t * (current - previous));
    }

private:
    Model model;
    std::size_t interval;
    Interpolation mode;
    std::size_t position = 0;
    float previous = 0.0f;
    float current = 0.0f;
};

} // namespace anoesis::test::drift
