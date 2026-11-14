// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio
//
// Builds the minBLEP and compensated minBLAMP residual tables (decision D6).
// Runs once, at initialisation, in double precision. A port to a target
// without a double FPU would generate the tables offline instead.

#include <anoesis/dsp/oscillators/min_blep.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <vector>

namespace anoesis::dsp
{

namespace
{

using Complex = std::complex<double>;

// In-place iterative radix-2 FFT (inverse unscaled). data.size() is a power of two.
void fft(std::vector<Complex>& data, bool inverse)
{
    const auto n = data.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        auto bit = n >> 1U;
        for (; (j & bit) != 0; bit >>= 1U)
        {
            j ^= bit;
        }
        j ^= bit;
        if (i < j)
        {
            std::swap(data[i], data[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1U)
    {
        const double angle = (inverse ? 2.0 : -2.0) * std::numbers::pi / static_cast<double>(len);
        for (std::size_t start = 0; start < n; start += len)
        {
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto w = std::polar(1.0, angle * static_cast<double>(k));
                const auto a = data[start + k];
                const auto b = data[start + k + (len / 2)] * w;
                data[start + k] = a + b;
                data[start + k + (len / 2)] = a - b;
            }
        }
    }
}

// Zeroth-order modified Bessel function of the first kind (power series).
double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 50; ++k)
    {
        const double half = x / (2.0 * k);
        term *= half * half;
        sum += term;
    }
    return sum;
}

// Kaiser's beta for a stopband of `db` dB.
double kaiserBeta(double db)
{
    if (db > 50.0)
    {
        return 0.1102 * (db - 8.7);
    }
    return (0.5842 * std::pow(db - 21.0, 0.4)) + (0.07886 * (db - 21.0));
}

// The minimum-phase version of `kernel` with the same magnitude response,
// by the real cepstrum (fold the cepstrum onto positive quefrencies).
std::vector<double> minimumPhase(const std::vector<double>& kernel)
{
    const auto size = std::bit_ceil(kernel.size()) * 16;
    std::vector<Complex> spectrum(size);
    std::ranges::copy(kernel, spectrum.begin());
    fft(spectrum, false);

    double peak = 0.0;
    for (const auto& x : spectrum)
    {
        peak = std::max(peak, std::abs(x));
    }
    // Floor the stopband nulls, whose log is -infinity, at -200 dB.
    for (auto& x : spectrum)
    {
        x = std::log(std::max(std::abs(x), peak * 1e-10));
    }
    fft(spectrum, true);
    for (std::size_t i = 0; i < size; ++i)
    {
        // Fold: keep quefrency 0 and N/2, double the positive ones, drop the negative ones.
        const double c = spectrum[i].real() / static_cast<double>(size);
        double folded = 0.0;
        if (i == 0 || i == size / 2)
        {
            folded = c;
        }
        else if (i < size / 2)
        {
            folded = 2.0 * c;
        }
        spectrum[i] = folded;
    }
    fft(spectrum, false);
    for (auto& x : spectrum)
    {
        x = std::exp(x);
    }
    fft(spectrum, true);

    std::vector<double> result(kernel.size());
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        result[i] = spectrum[i].real() / static_cast<double>(size);
    }
    return result;
}

float flushTiny(double x)
{
    return std::abs(x) < 1e-30 ? 0.0f : static_cast<float>(x);
}

} // namespace

MinBlepTable::MinBlepTable(std::size_t taps, std::size_t phases) : tapCount(taps), phaseCount(phases)
{
    const auto length = taps * phases; // points 0..length, one every 1 / phases samples
    const auto perSample = static_cast<double>(phases);

    // A sinc cut off at Nyquist (zero crossings every sample), centred, under
    // a Kaiser window. Kaiser's formula sets the stopband for a full
    // transition width of 9.3% of the rate: 20 to 24.1 kHz at 44.1 kHz.
    const double stopbandDb = 7.95 + (14.36 * static_cast<double>(taps) * 0.093);
    const double beta = kaiserBeta(stopbandDb);
    std::vector<double> kernel(length + 1);
    for (std::size_t j = 0; j <= length; ++j)
    {
        const double x = (static_cast<double>(j) - (static_cast<double>(length) / 2.0)) / perSample;
        const double sinc = x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        const double r = (2.0 * static_cast<double>(j) / static_cast<double>(length)) - 1.0;
        kernel[j] = sinc * besselI0(beta * std::sqrt(std::max(0.0, 1.0 - (r * r)))) / besselI0(beta);
    }

    const auto h = minimumPhase(kernel);

    // Integrate (trapezoid rule) to the band-limited step, normalised to end
    // at exactly 1, then subtract the naive step: the step residual.
    std::vector<double> residual(length + 1);
    double integral = 0.0;
    residual[0] = 0.0;
    for (std::size_t j = 1; j <= length; ++j)
    {
        integral += (h[j - 1] + h[j]) / (2.0 * perSample);
        residual[j] = integral;
    }
    for (auto& r : residual)
    {
        r = (r / integral) - 1.0;
    }
    residual[length] = 0.0;

    // The ramp residual: the running integral of the step residual. It
    // settles at -(group delay); adding the delay brings it to 0 at the end.
    std::vector<double> ramp(length + 1);
    ramp[0] = 0.0;
    for (std::size_t j = 1; j <= length; ++j)
    {
        ramp[j] = ramp[j - 1] + ((residual[j - 1] + residual[j]) / (2.0 * perSample));
    }
    const double delay = -ramp[length];
    groupDelay = static_cast<float>(delay);
    for (auto& r : ramp)
    {
        r += delay;
    }
    ramp[length] = 0.0;

    steps.resize((phases + 1) * taps);
    ramps.resize((phases + 1) * taps);
    for (std::size_t p = 0; p <= phases; ++p)
    {
        for (std::size_t i = 0; i < taps; ++i)
        {
            const auto source = (i * phases) + p;
            steps[(p * taps) + i] = flushTiny(source <= length ? residual[source] : 0.0);
            ramps[(p * taps) + i] = flushTiny(source <= length ? ramp[source] : 0.0);
        }
    }
}

} // namespace anoesis::dsp
