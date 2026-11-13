#include <anoesis/test/window.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>

namespace anoesis::test
{

namespace
{

// Cosine-sum window: sum over k of (-1)^k a_k cos(2 pi k n / N), periodic.
Window cosineSum(std::size_t n, std::initializer_list<double> coefficients, std::size_t mainLobeBins)
{
    Window window{.samples = std::vector<double>(n), .mainLobeBins = mainLobeBins};
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto x = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n);
        double value = 0.0;
        double sign = 1.0;
        double k = 0.0;
        for (const auto a : coefficients)
        {
            value += sign * a * std::cos(k * x);
            sign = -sign;
            k += 1.0;
        }
        window.samples[i] = value;
    }
    return window;
}

// Modified Bessel function of the first kind, order 0, by its power series
// sum ((x/2)^k / k!)^2. Converges for all x; ~40 terms at x = 20.
double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    const auto halfX = x / 2.0;
    for (int k = 1; k < 500; ++k)
    {
        term *= halfX / static_cast<double>(k);
        const auto squared = term * term;
        sum += squared;
        if (squared < sum * 1e-17)
        {
            break;
        }
    }
    return sum;
}

} // namespace

Window rectangularWindow(std::size_t n)
{
    return {.samples = std::vector<double>(n, 1.0), .mainLobeBins = 1};
}

Window hannWindow(std::size_t n)
{
    return cosineSum(n, {0.5, 0.5}, 2);
}

Window blackmanHarrisWindow(std::size_t n)
{
    return cosineSum(n, {0.35875, 0.48829, 0.14128, 0.01168}, 4);
}

Window kaiserWindow(std::size_t n, double beta)
{
    const auto lobe = std::sqrt(1.0 + ((beta / std::numbers::pi) * (beta / std::numbers::pi)));
    Window window{.samples = std::vector<double>(n), .mainLobeBins = static_cast<std::size_t>(std::ceil(lobe))};
    const auto norm = besselI0(beta);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto t = (2.0 * static_cast<double>(i) / static_cast<double>(n)) - 1.0; // periodic: -1 .. 1 - 2/N
        window.samples[i] = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - (t * t)))) / norm;
    }
    return window;
}

} // namespace anoesis::test
