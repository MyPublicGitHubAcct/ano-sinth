#include <anoesis/test/lifecycle.hpp>

#include <cmath>
#include <numbers>

namespace anoesis::test
{

Buffer makeProbeSignal(std::size_t numChannels, std::size_t numSamples)
{
    Buffer buffer{numChannels, numSamples};
    for (std::size_t ch = 0; ch < numChannels; ++ch)
    {
        const auto samples = buffer.channel(ch);
        const auto detune = 1.0 + (0.1 * static_cast<double>(ch));
        for (std::size_t n = 0; n < numSamples; ++n)
        {
            const auto t = static_cast<double>(n);
            const auto value = (0.5 * std::sin(0.05 * std::numbers::pi * detune * t)) +
                               (0.25 * std::sin((0.31 * std::numbers::pi * detune * t) + 1.0));
            samples[n] = static_cast<float>(value);
        }
        if (numSamples > 0)
        {
            samples[0] = 1.0f;
        }
    }
    return buffer;
}

Buffer makeDisturbanceSignal(std::size_t numChannels, std::size_t numSamples)
{
    Buffer buffer{numChannels, numSamples};
    for (std::size_t ch = 0; ch < numChannels; ++ch)
    {
        const auto samples = buffer.channel(ch);
        for (std::size_t n = 0; n < numSamples; ++n)
        {
            samples[n] = 4.0f - (8.0f * static_cast<float>(n) / static_cast<float>(numSamples));
        }
    }
    return buffer;
}

} // namespace anoesis::test
