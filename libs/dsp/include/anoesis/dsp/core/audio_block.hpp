// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>

namespace anoesis::dsp
{

/// A non-owning view of multichannel audio: an array of channel pointers, a
/// channel count, and a range of samples. Every channel holds the same number
/// of samples.
///
/// It is the one buffer type processors take, so the same processor runs
/// inside the JUCE plugin (wrapping juce::AudioBuffer's channel pointers), in
/// tests (anoesis::test::Buffer), or on an embedded target (static arrays).
/// Copying it is cheap and never copies audio. Writing through it writes the
/// caller's memory.
class AudioBlock
{
public:
    AudioBlock() noexcept = default;

    /// Views `numChannels` channels of `numSamples` samples each. The pointer
    /// array and the audio it points to must outlive the block.
    AudioBlock(float* const* channels, std::size_t numChannels, std::size_t numSamples) noexcept
        : channelPointers(channels), channelCount(numChannels), sampleCount(numSamples)
    {
    }

    [[nodiscard]] std::size_t numChannels() const noexcept
    {
        return channelCount;
    }

    [[nodiscard]] std::size_t numSamples() const noexcept
    {
        return sampleCount;
    }

    /// True when the block holds no samples (it may still have channels).
    [[nodiscard]] bool empty() const noexcept
    {
        return sampleCount == 0;
    }

    /// The samples of one channel. `index` must be below numChannels().
    [[nodiscard]] std::span<float> channel(std::size_t index) const noexcept
    {
        return {channelPointers[index] + startOffset, sampleCount};
    }

    /// A view of `length` samples starting `offset` samples into this block.
    /// Out-of-range requests are clamped to the samples that exist, so a
    /// sub-block can never reach past its parent.
    [[nodiscard]] AudioBlock subBlock(std::size_t offset, std::size_t length) const noexcept
    {
        const auto start = std::min(offset, sampleCount);
        const auto count = std::min(length, sampleCount - start);
        AudioBlock sub{channelPointers, channelCount, count};
        sub.startOffset = startOffset + start;
        return sub;
    }

    /// Writes zeros to every sample in the view.
    void clear() const noexcept
    {
        for (std::size_t ch = 0; ch < channelCount; ++ch)
        {
            std::ranges::fill(channel(ch), 0.0f);
        }
    }

private:
    float* const* channelPointers = nullptr;
    std::size_t channelCount = 0;
    std::size_t sampleCount = 0;
    std::size_t startOffset = 0;
};

} // namespace anoesis::dsp
