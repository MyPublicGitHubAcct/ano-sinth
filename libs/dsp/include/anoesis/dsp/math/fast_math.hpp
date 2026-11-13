// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>

/// Fast approximations of exp2 and tanh for per-sample use (pitch and level
/// modulation, saturating filter stages). Branch-free apart from selects, no
/// tables, float only. Measured accuracy (tests/dsp/math/fast_math_test.cpp):
///
///   fastExp2  relative error < 5e-7 over [-126, 127]; exact at integers
///   fastTanh  absolute error < 2e-7, relative error < 1e-6 everywhere
///
/// Design and derivation: docs/dsp/fast-math.md
namespace anoesis::dsp
{

namespace detail
{

/// q(f) ~= 2^f - 1 for f in [0, 1], with q(0) = 0 and q(1) = 1 built in.
/// Degree-5 minimax for relative error (5.6e-7), designed by
/// tools/design/exp2_polynomial.py.
[[nodiscard]] inline float exp2MinusOneUnit(float f) noexcept
{
    // c1 is close to ln 2 but deliberately not equal: it is the minimax value.
    constexpr float c1 = 6.931475673e-01f; // NOLINT(modernize-use-std-numbers)
    constexpr float c2 = 2.402067909e-01f;
    constexpr float c3 = 5.566123860e-02f;
    constexpr float c4 = 9.189177681e-03f;
    constexpr float c5 = 1.795225531e-03f;
    return f * (c1 + (f * (c2 + (f * (c3 + (f * (c4 + (f * c5))))))));
}

/// 2^n as a float, built directly from its exponent bits. n in [-126, 127].
[[nodiscard]] inline float powerOfTwo(std::int32_t n) noexcept
{
    return std::bit_cast<float>(static_cast<std::uint32_t>(n + 127) << 23U);
}

/// floor(x) as an integer, for x well inside the int32 range. Avoids
/// std::floor, which is a library call on x86-64 without SSE4.1.
[[nodiscard]] inline std::int32_t floorToInt(float x) noexcept
{
    const auto truncated = static_cast<std::int32_t>(x);
    return truncated - (x < static_cast<float>(truncated) ? 1 : 0);
}

} // namespace detail

/// 2^x. Inputs are clamped to [-126, 127], so the result is always a normal,
/// finite float (never a denormal or inf). NaN gives 2^-126.
[[nodiscard]] inline float fastExp2(float x) noexcept
{
    // std::max(lo, x) returns lo for NaN, so the integer conversion below never
    // sees NaN (converting NaN to int is undefined behaviour).
    x = std::min(127.0f, std::max(-126.0f, x));
    const auto i = detail::floorToInt(x);
    const auto scale = detail::powerOfTwo(i);
    return scale + (scale * detail::exp2MinusOneUnit(x - static_cast<float>(i)));
}

/// tanh(x), with the exact symmetry tanh(-x) = -tanh(x). Saturates to exactly
/// +-1 for |x| >= 8.6 (tanh(8.6) is within 7e-8 of 1). NaN gives +-1.
[[nodiscard]] inline float fastTanh(float x) noexcept
{
    // tanh|x| = (e^2|x| - 1) / (e^2|x| + 1) = m / (m + 2), with m = e^2|x| - 1
    // computed as (2^i - 1) + 2^i * q(f). For small x, i = 0 and m = q(f),
    // which keeps full relative precision (a naive 1 - 2 / (e + 1) cancels).
    //
    // Saturation point: below 8.6, m < 2^25, so m + 2 is exact in float and
    // the quotient is monotonic. Above about 8.66, m + 2 would round
    // alternately up and down, and the result would wobble by one ulp.
    constexpr float saturationPoint = 8.6f;
    constexpr float twoLog2e = 2.0f * std::numbers::log2e_v<float>;

    const auto magnitude = std::min(saturationPoint, std::fabs(x)); // NaN -> saturationPoint
    const auto y = twoLog2e * magnitude;
    const auto i = static_cast<std::int32_t>(y); // y >= 0, so truncation is floor
    const auto scale = detail::powerOfTwo(i);
    const auto m = (scale - 1.0f) + (scale * detail::exp2MinusOneUnit(y - static_cast<float>(i)));
    const auto t = magnitude < saturationPoint ? m / (m + 2.0f) : 1.0f;
    return std::copysign(t, x);
}

} // namespace anoesis::dsp
