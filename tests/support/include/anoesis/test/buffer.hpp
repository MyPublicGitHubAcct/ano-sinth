// Test support: an owning multichannel buffer.

#pragma once

#include <anoesis/dsp/core/audio_block.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace anoesis::test
{

/// Owns `numChannels` channels of `numSamples` floats, all zero to start, and
/// hands them out as an anoesis::dsp::AudioBlock. Copyable: a copy owns its
/// own samples.
class Buffer
{
public:
    Buffer(std::size_t numChannels, std::size_t numSamples);

    [[nodiscard]] std::size_t numChannels() const noexcept
    {
        return channels.size();
    }

    [[nodiscard]] std::size_t numSamples() const noexcept
    {
        return length;
    }

    [[nodiscard]] std::span<float> channel(std::size_t index) noexcept
    {
        return channels[index];
    }

    [[nodiscard]] std::span<const float> channel(std::size_t index) const noexcept
    {
        return channels[index];
    }

    /// A view of the whole buffer. Valid until the buffer is destroyed.
    [[nodiscard]] dsp::AudioBlock block() noexcept;

private:
    std::vector<std::vector<float>> channels;
    std::vector<float*> pointers;
    std::size_t length;
};

/// True when both buffers have the same shape and every sample has the same
/// bit pattern (so NaN equals NaN, and +0 differs from -0). Use it to check
/// determinism, where "close" is not good enough.
[[nodiscard]] bool bitIdentical(const Buffer& a, const Buffer& b) noexcept;

} // namespace anoesis::test
