// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#include <anoesis/dsp/resampling/oversampler.hpp>

#include <algorithm>
#include <array>

namespace anoesis::dsp
{

Oversampler::Oversampler(OversamplingFactor factor) noexcept : requested(factor), active(factor) {}

void Oversampler::setFactor(OversamplingFactor factor) noexcept
{
    requested = factor;
}

void Oversampler::prepare(const ProcessSpec& spec)
{
    active = requested;
    baseSpec = spec;
    const auto channelLength = spec.maxBlockSize * ratio();
    filters.assign(spec.numChannels, ChannelFilters{});
    storage.assign(spec.numChannels * channelLength, 0.0f);
    channelPointers.resize(spec.numChannels);
    for (std::size_t ch = 0; ch < spec.numChannels; ++ch)
    {
        channelPointers[ch] = storage.data() + (ch * channelLength);
    }
    reset();
}

void Oversampler::ChannelFilters::reset() noexcept
{
    up1.reset();
    up2.reset();
    down2.reset();
    down1.reset();
}

void Oversampler::reset() noexcept
{
    for (auto& channel : filters)
    {
        channel.reset();
    }
}

OversamplingFactor Oversampler::factor() const noexcept
{
    return active;
}

std::size_t Oversampler::ratio() const noexcept
{
    return static_cast<std::size_t>(active);
}

ProcessSpec Oversampler::oversampledSpec() const noexcept
{
    return {
        .sampleRate = baseSpec.sampleRate * static_cast<float>(ratio()),
        .maxBlockSize = baseSpec.maxBlockSize * ratio(),
        .numChannels = baseSpec.numChannels,
    };
}

AudioBlock Oversampler::upsample(const AudioBlock& input) noexcept
{
    const auto numSamples = std::min(input.numSamples(), baseSpec.maxBlockSize);
    const auto numChannels = std::min(input.numChannels(), filters.size());

    for (std::size_t ch = 0; ch < numChannels; ++ch)
    {
        const auto in = input.channel(ch);
        float* out = channelPointers[ch];
        auto& f = filters[ch];
        switch (active)
        {
        case OversamplingFactor::none:
            std::copy_n(in.begin(), numSamples, out);
            break;
        case OversamplingFactor::two:
            for (std::size_t n = 0; n < numSamples; ++n)
            {
                f.up1.process(in[n], out[2 * n], out[(2 * n) + 1]);
            }
            break;
        case OversamplingFactor::four:
            // Per sample through both stages, so no intermediate 2x buffer.
            for (std::size_t n = 0; n < numSamples; ++n)
            {
                std::array<float, 2> twice{};
                f.up1.process(in[n], twice[0], twice[1]);
                f.up2.process(twice[0], out[4 * n], out[(4 * n) + 1]);
                f.up2.process(twice[1], out[(4 * n) + 2], out[(4 * n) + 3]);
            }
            break;
        }
    }
    return {channelPointers.data(), numChannels, numSamples * ratio()};
}

void Oversampler::downsample(const AudioBlock& output) noexcept
{
    const auto numSamples = std::min(output.numSamples(), baseSpec.maxBlockSize);
    const auto numChannels = std::min(output.numChannels(), filters.size());

    for (std::size_t ch = 0; ch < numChannels; ++ch)
    {
        const float* in = channelPointers[ch];
        const auto out = output.channel(ch);
        auto& f = filters[ch];
        switch (active)
        {
        case OversamplingFactor::none:
            std::copy_n(in, numSamples, out.begin());
            break;
        case OversamplingFactor::two:
            for (std::size_t n = 0; n < numSamples; ++n)
            {
                out[n] = f.down1.process(in[2 * n], in[(2 * n) + 1]);
            }
            break;
        case OversamplingFactor::four:
            for (std::size_t n = 0; n < numSamples; ++n)
            {
                const auto first = f.down2.process(in[4 * n], in[(4 * n) + 1]);
                const auto second = f.down2.process(in[(4 * n) + 2], in[(4 * n) + 3]);
                out[n] = f.down1.process(first, second);
            }
            break;
        }
    }
}

float Oversampler::latencySamples() const noexcept
{
    // Upsampling delays by the halfband's group delay tau; decimating at the
    // odd phase delays by tau - 1 (both in high-rate samples). A round trip
    // through one stage is therefore 2 tau - 1 high-rate samples.
    const auto roundTrip1 = (2.0f * halfband::groupDelayAtDc(halfband::stage1)) - 1.0f; // 2x samples
    const auto roundTrip2 = (2.0f * halfband::groupDelayAtDc(halfband::stage2)) - 1.0f; // 4x samples
    switch (active)
    {
    case OversamplingFactor::none:
        return 0.0f;
    case OversamplingFactor::two:
        return roundTrip1 / 2.0f;
    case OversamplingFactor::four:
        return (roundTrip1 + (roundTrip2 / 2.0f)) / 2.0f;
    }
    return 0.0f;
}

} // namespace anoesis::dsp
