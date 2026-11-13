#include <anoesis/test/measure.hpp>
#include <anoesis/test/signals.hpp>
#include <anoesis/test/spectrum.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace anoesis::test
{

double toDb(double amplitudeRatio) noexcept
{
    return 20.0 * std::log10(amplitudeRatio);
}

double powerToDb(double powerRatio) noexcept
{
    return 10.0 * std::log10(powerRatio);
}

namespace
{

// Harmonic lobes overlap (and are double-counted) if the fundamental is
// closer than two main lobes to DC or to the next harmonic.
void requireResolvable(const Spectrum& spectrum, double fundamentalHz)
{
    if (fundamentalHz < 2.0 * static_cast<double>(spectrum.mainLobeBins + 1) * spectrum.binWidth())
    {
        throw std::invalid_argument(
            "the fundamental is too low to resolve its harmonics: use a longer FFT or a narrower window");
    }
}

} // namespace

double measureThd(std::span<const float> signal, double fundamentalHz, double sampleRate, const Window& window,
                  std::size_t maxHarmonic)
{
    const auto spectrum = powerSpectrum(signal, sampleRate, window);
    requireResolvable(spectrum, fundamentalHz);
    const auto lobe = spectrum.mainLobeBins;
    const auto fundamental = spectrum.powerAround(fundamentalHz, lobe);

    double harmonics = 0.0;
    for (std::size_t h = 2; h <= maxHarmonic; ++h)
    {
        const auto f = fundamentalHz * static_cast<double>(h);
        if (f >= sampleRate / 2.0)
        {
            break;
        }
        harmonics += spectrum.powerAround(f, lobe);
    }
    return std::sqrt(harmonics / fundamental);
}

AliasingResult measureAliasing(std::span<const float> signal, double fundamentalHz, double sampleRate,
                               const Window& window, double maxFrequencyHz)
{
    const auto spectrum = powerSpectrum(signal, sampleRate, window);
    requireResolvable(spectrum, fundamentalHz);
    const auto lobe = spectrum.mainLobeBins;
    const auto lastBin = spectrum.nearestBin(std::min(maxFrequencyHz, sampleRate / 2.0));

    // Mark DC and every harmonic's main lobe; what is left is non-harmonic.
    std::vector<bool> isHarmonic(spectrum.power.size(), false);
    const auto mark = [&](std::size_t centre)
    {
        const auto first = centre > lobe ? centre - lobe : 0;
        const auto last = std::min(centre + lobe, spectrum.power.size() - 1);
        for (auto k = first; k <= last; ++k)
        {
            isHarmonic[k] = true;
        }
    };
    mark(0);
    AliasingResult result;
    for (std::size_t h = 1; static_cast<double>(h) * fundamentalHz <= sampleRate / 2.0; ++h)
    {
        const auto f = static_cast<double>(h) * fundamentalHz;
        mark(spectrum.nearestBin(f));
        result.harmonicPower += spectrum.powerAround(f, lobe);
    }

    std::size_t worstBin = 0;
    double worstBinPower = -1.0;
    for (std::size_t k = 0; k <= lastBin; ++k)
    {
        if (!isHarmonic[k])
        {
            result.aliasPower += spectrum.power[k];
            if (spectrum.power[k] > worstBinPower)
            {
                worstBinPower = spectrum.power[k];
                worstBin = k;
            }
        }
    }

    // The strongest alias's power is its whole main lobe (minus any part that
    // overlaps a harmonic), so off-bin aliases are not under-reported.
    double worstPower = 0.0;
    if (worstBinPower >= 0.0)
    {
        const auto first = worstBin > lobe ? worstBin - lobe : 0;
        const auto last = std::min(worstBin + lobe, lastBin);
        for (auto k = first; k <= last; ++k)
        {
            worstPower += isHarmonic[k] ? 0.0 : spectrum.power[k];
        }
    }

    const auto fundamentalPower = spectrum.powerAround(fundamentalHz, lobe);
    result.aliasToSignalDb = powerToDb(result.aliasPower / result.harmonicPower);
    result.worstAliasDb = powerToDb(worstPower / fundamentalPower);
    result.worstAliasHz = spectrum.frequencyOf(worstBin);
    return result;
}

ResponsePoint measureResponseAt(const MonoProcess& process, double frequencyHz, double sampleRate, double amplitude,
                                std::size_t settleSamples, std::size_t measureSamples)
{
    const auto input = sine(frequencyHz, sampleRate, settleSamples + measureSamples, amplitude);
    const auto output = process(input);
    if (output.size() != input.size())
    {
        throw std::invalid_argument("measureResponseAt: the process must return as many samples as it is given");
    }

    // Correlate input and output with e^(-i w n) under the same window. The
    // ratio of the two correlations is the response at w; any window bias
    // cancels, and the Kaiser(20) window's -190 dB sidelobes keep the sine's
    // negative-frequency image out.
    const auto window = kaiserWindow(measureSamples, 20.0);
    const auto omega = 2.0 * std::numbers::pi * frequencyHz / sampleRate;
    Complex in{};
    Complex out{};
    for (std::size_t i = 0; i < measureSamples; ++i)
    {
        const auto n = settleSamples + i;
        const auto rotation = std::polar(window.samples[i], -omega * static_cast<double>(n));
        in += static_cast<double>(input[n]) * rotation;
        out += static_cast<double>(output[n]) * rotation;
    }
    const auto response = out / in;
    return {
        .frequencyHz = frequencyHz,
        .magnitude = std::abs(response),
        .magnitudeDb = toDb(std::abs(response)),
        .phaseRadians = std::arg(response),
    };
}

std::vector<Complex> impulseResponseSpectrum(const MonoProcess& process, std::size_t fftSize)
{
    const auto output = process(impulse(fftSize));
    if (output.size() != fftSize)
    {
        throw std::invalid_argument("impulseResponseSpectrum: the process must return as many samples as it is given");
    }
    const std::vector<double> response(output.begin(), output.end());
    return realFft(response);
}

} // namespace anoesis::test
