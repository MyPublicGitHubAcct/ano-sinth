// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/denormals.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

/// Polyphase IIR halfband filters: the building blocks of Oversampler.
///
/// A halfband low-pass H(z) = 1/2 [A0(z^2) + z^-1 A1(z^2)] made of two chains
/// of first-order allpass sections A(z^2) = prod (a + z^-2) / (1 + a z^-2).
/// Running each chain at the *low* rate (where z^2 becomes z) is the
/// polyphase trick: a 2x interpolator or decimator costs one allpass section
/// per coefficient per low-rate sample. The coefficients are elliptic
/// (Valenzuela and Constantinides), designed and verified by
/// tools/design/halfband_iir.py. Doc: docs/dsp/oversampling.md
namespace anoesis::dsp
{

namespace halfband
{

/// Stage 1 (1x <-> 2x). Passes 0..0.2268 of the 2x rate (0..20 kHz at a
/// 44.1 kHz base) with 1.7e-10 dB ripple; rejects 0.2732..0.5 (24.1 kHz up)
/// by 104.0 dB with these float coefficients.
inline constexpr std::array<float, 8> stage1{
    3.736511671e-02f, 1.393618248e-01f, 2.813460114e-01f, 4.362618268e-01f,
    5.844611708e-01f, 7.170654297e-01f, 8.345820435e-01f, 9.443040689e-01f,
};

/// Stage 2 (2x <-> 4x). Passes 0..0.1134 of the 4x rate (0..20 kHz at an
/// 88.2 kHz rate); rejects 0.3866..0.5 (68.2 kHz up) by 125.2 dB.
inline constexpr std::array<float, 4> stage2{
    4.002939818e-02f,
    1.627607247e-01f,
    3.812495627e-01f,
    7.376963808e-01f,
};

/// Group delay at DC of H, in samples of the high rate: the average of the
/// two paths' delays (equal magnitudes at DC), each section contributing
/// 2 (1 - a) / (1 + a), and path 1 one extra sample for its z^-1.
template <std::size_t N>
[[nodiscard]] constexpr float groupDelayAtDc(const std::array<float, N>& coefficients) noexcept
{
    float total = 1.0f;
    for (const auto a : coefficients)
    {
        total += 2.0f * (1.0f - a) / (1.0f + a);
    }
    return 0.5f * total;
}

} // namespace halfband

/// A cascade of first-order allpass sections y = a (x - y1) + x1, run at the
/// low rate. Coefficients are taken from every second element of an array,
/// starting at `first`, so one sorted coefficient list feeds both paths.
/// flushStates() zeroes any state below FLT_MIN (flushDenormal); the
/// Oversampler calls it once per block, so the recursive states decay to
/// exact zero after the input stops, with or without FTZ
/// (docs/dsp/denormals.md). Flushing inside the per-sample chain would put
/// a compare in its serial dependency (+50% on the round trip, measured).
template <std::size_t Sections>
class AllpassChain
{
public:
    template <std::size_t N>
    constexpr AllpassChain(const std::array<float, N>& coefficients, std::size_t first) noexcept
    {
        for (std::size_t k = 0; k < Sections; ++k)
        {
            a[k] = coefficients[first + (2 * k)];
        }
    }

    [[nodiscard]] float process(float x) noexcept
    {
        for (std::size_t k = 0; k < Sections; ++k)
        {
            const auto y = (a[k] * (x - y1[k])) + x1[k];
            x1[k] = x;
            y1[k] = y;
            x = y;
        }
        return x;
    }

    void reset() noexcept
    {
        x1.fill(0.0f);
        y1.fill(0.0f);
    }

    /// Zeroes every state below FLT_MIN in size.
    void flushStates() noexcept
    {
        for (std::size_t k = 0; k < Sections; ++k)
        {
            x1[k] = flushDenormal(x1[k]);
            y1[k] = flushDenormal(y1[k]);
        }
    }

    /// True when every state is exactly zero.
    [[nodiscard]] bool isAtRest() const noexcept
    {
        const auto zero = [](float v)
        {
            return std::fpclassify(v) == FP_ZERO;
        };
        return std::ranges::all_of(x1, zero) && std::ranges::all_of(y1, zero);
    }

private:
    std::array<float, Sections> a{};
    std::array<float, Sections> x1{};
    std::array<float, Sections> y1{};
};

/// One channel of 2x interpolation: each input sample becomes two output
/// samples, with images above the passband removed.
template <std::size_t N>
class HalfbandUpsampler
{
public:
    explicit constexpr HalfbandUpsampler(const std::array<float, N>& coefficients) noexcept
        : path0(coefficients, 0), path1(coefficients, 1)
    {
    }

    /// Y(z) = 2 H(z) X(z^2) = [A0(z^2) + z^-1 A1(z^2)] X(z^2): the even
    /// output is path 0, the odd output path 1. (The factor 2 restores the
    /// level lost by inserting zeros.)
    /// The outputs are flushed below FLT_MIN (outside the recursion).
    void process(float input, float& even, float& odd) noexcept
    {
        even = flushDenormal(path0.process(input));
        odd = flushDenormal(path1.process(input));
    }

    void reset() noexcept
    {
        path0.reset();
        path1.reset();
    }

    void flushStates() noexcept
    {
        path0.flushStates();
        path1.flushStates();
    }

    [[nodiscard]] bool isAtRest() const noexcept
    {
        return path0.isAtRest() && path1.isAtRest();
    }

private:
    AllpassChain<(N + 1) / 2> path0;
    AllpassChain<N / 2> path1;
};

/// One channel of 2x decimation: each pair of input samples becomes one
/// output sample, with everything above the passband removed first.
template <std::size_t N>
class HalfbandDownsampler
{
public:
    explicit constexpr HalfbandDownsampler(const std::array<float, N>& coefficients) noexcept
        : path0(coefficients, 0), path1(coefficients, 1)
    {
    }

    /// The odd-phase output of H: y[n] = 1/2 [A0(v[2n+1]) + A1(v[2n])].
    /// Both paths see a sample from the same pair, so neither needs a delay.
    /// Flushed: half the sum of two tiny paths can fall below FLT_MIN.
    [[nodiscard]] float process(float first, float second) noexcept
    {
        return flushDenormal(0.5f * (path0.process(second) + path1.process(first)));
    }

    void reset() noexcept
    {
        path0.reset();
        path1.reset();
    }

    void flushStates() noexcept
    {
        path0.flushStates();
        path1.flushStates();
    }

    [[nodiscard]] bool isAtRest() const noexcept
    {
        return path0.isAtRest() && path1.isAtRest();
    }

private:
    AllpassChain<(N + 1) / 2> path0;
    AllpassChain<N / 2> path1;
};

} // namespace anoesis::dsp
