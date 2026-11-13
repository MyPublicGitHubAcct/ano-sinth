#include <anoesis/test/signals.hpp>

#include <cmath>
#include <numbers>

namespace anoesis::test
{

std::vector<float> sine(double frequencyHz, double sampleRate, std::size_t numSamples, double amplitude,
                        double phaseRadians)
{
    std::vector<float> x(numSamples);
    const auto omega = 2.0 * std::numbers::pi * frequencyHz / sampleRate;
    for (std::size_t n = 0; n < numSamples; ++n)
    {
        x[n] = static_cast<float>(amplitude * std::sin((omega * static_cast<double>(n)) + phaseRadians));
    }
    return x;
}

std::vector<float> impulse(std::size_t numSamples, std::size_t position, float amplitude)
{
    std::vector<float> x(numSamples, 0.0f);
    if (position < numSamples)
    {
        x[position] = amplitude;
    }
    return x;
}

std::vector<float> constant(std::size_t numSamples, float value)
{
    // Parentheses, not braces: {numSamples, value} would be a two-element list.
    std::vector<float> x(numSamples, value);
    return x;
}

double coherentFrequency(double targetHz, double sampleRate, std::size_t numSamples)
{
    const auto n = static_cast<double>(numSamples);
    const auto cycles = targetHz * n / sampleRate;
    auto odd = (2.0 * std::floor(cycles / 2.0)) + 1.0; // the odd number at or just below...
    if (cycles - odd > 1.0)
    {
        odd += 2.0; // ...or just above, whichever is nearer
    }
    return odd * sampleRate / n;
}

} // namespace anoesis::test
