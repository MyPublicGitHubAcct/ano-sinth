#include <anoesis/test/fft.hpp>
#include <anoesis/test/spectrum.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace anoesis::test
{

double Spectrum::binWidth() const noexcept
{
    return sampleRate / static_cast<double>(fftSize);
}

double Spectrum::frequencyOf(std::size_t bin) const noexcept
{
    return static_cast<double>(bin) * binWidth();
}

std::size_t Spectrum::nearestBin(double frequencyHz) const noexcept
{
    const auto last = power.empty() ? 0 : power.size() - 1;
    const auto bin = std::round(frequencyHz / binWidth());
    return bin <= 0.0 ? 0 : std::min(static_cast<std::size_t>(bin), last);
}

double Spectrum::powerAround(double frequencyHz, std::size_t halfWidth) const noexcept
{
    const auto centre = nearestBin(frequencyHz);
    const auto first = centre > halfWidth ? centre - halfWidth : 0;
    const auto last = std::min(centre + halfWidth, power.size() - 1);
    double sum = 0.0;
    for (auto k = first; k <= last; ++k)
    {
        sum += power[k];
    }
    return sum;
}

Spectrum powerSpectrum(std::span<const float> signal, double sampleRate, const Window& window)
{
    const auto n = signal.size();
    if (!isPowerOfTwo(n) || window.samples.size() != n)
    {
        throw std::invalid_argument("powerSpectrum: the signal and window must have the same power-of-two length");
    }

    std::vector<Complex> data(n);
    double windowEnergy = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        data[i] = static_cast<double>(signal[i]) * window.samples[i];
        windowEnergy += window.samples[i] * window.samples[i];
    }
    fftInPlace(data);

    Spectrum spectrum{
        .power = std::vector<double>((n / 2) + 1),
        .sampleRate = sampleRate,
        .fftSize = n,
        .mainLobeBins = window.mainLobeBins,
    };
    const auto scale = 1.0 / (static_cast<double>(n) * windowEnergy);
    for (std::size_t k = 0; k <= n / 2; ++k)
    {
        const auto oneSided = (k == 0 || k == n / 2) ? 1.0 : 2.0; // fold in the negative frequencies
        spectrum.power[k] = oneSided * std::norm(data[k]) * scale;
    }
    return spectrum;
}

Spectrum averagedPowerSpectrum(std::span<const float> signal, double sampleRate, const Window& window)
{
    const auto segment = window.samples.size();
    const auto count = segment == 0 ? 0 : signal.size() / segment;
    if (count == 0)
    {
        throw std::invalid_argument("averagedPowerSpectrum: the signal is shorter than one segment");
    }

    auto average = powerSpectrum(signal.first(segment), sampleRate, window);
    for (std::size_t s = 1; s < count; ++s)
    {
        const auto next = powerSpectrum(signal.subspan(s * segment, segment), sampleRate, window);
        std::ranges::transform(average.power, next.power, average.power.begin(), std::plus<>{});
    }
    for (auto& p : average.power)
    {
        p /= static_cast<double>(count);
    }
    return average;
}

} // namespace anoesis::test
