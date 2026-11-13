#include <anoesis/test/buffer.hpp>

#include <algorithm>
#include <cstring>

namespace anoesis::test
{

Buffer::Buffer(std::size_t numChannels, std::size_t numSamples)
    : channels(numChannels, std::vector<float>(numSamples, 0.0f)), pointers(numChannels, nullptr), length(numSamples)
{
}

dsp::AudioBlock Buffer::block() noexcept
{
    // Refreshed on every call, so a copied Buffer never hands out pointers
    // into the buffer it was copied from.
    std::ranges::transform(channels, pointers.begin(), [](auto& ch) { return ch.data(); });
    return {pointers.data(), pointers.size(), length};
}

bool bitIdentical(const Buffer& a, const Buffer& b) noexcept
{
    if (a.numChannels() != b.numChannels() || a.numSamples() != b.numSamples())
    {
        return false;
    }
    for (std::size_t ch = 0; ch < a.numChannels(); ++ch)
    {
        if (std::memcmp(a.channel(ch).data(), b.channel(ch).data(), a.numSamples() * sizeof(float)) != 0)
        {
            return false;
        }
    }
    return true;
}

} // namespace anoesis::test
