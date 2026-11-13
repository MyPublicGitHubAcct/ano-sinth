// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/resampling/halfband.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace anoesis::dsp
{

/// How many times the base rate to run at.
enum class OversamplingFactor : std::uint8_t
{
    none = 1,
    two = 2,
    four = 4,
};

/// 2x or 4x oversampling with polyphase IIR halfband filters (one stage per
/// doubling). Specification at a 44.1 kHz base rate: 0..20 kHz flat to
/// 1e-4 dB (designed: 3.4e-10 dB) through a round trip; images and aliases
/// from 24.1 kHz upwards rejected by at least 100 dB; a latency of a few
/// samples (latencySamples()).
///
/// Use, per block:
///
///   auto inner = oversampler.upsample(block);   // ratio() x the samples
///   distortion.process(inner);                  // at oversampledSpec() rate
///   oversampler.downsample(block);              // back, in place
///
/// prepare() allocates the oversampled buffer (channels x maxBlockSize x
/// ratio); nothing else allocates. Satisfies Preparable.
/// Doc: docs/dsp/oversampling.md
class Oversampler
{
public:
    explicit Oversampler(OversamplingFactor factor = OversamplingFactor::two) noexcept;

    /// Changes the factor. Takes effect at the next prepare(), which must
    /// follow (the buffer size changes).
    void setFactor(OversamplingFactor factor) noexcept;

    /// `spec` is the base-rate spec.
    void prepare(const ProcessSpec& spec);
    void reset() noexcept;

    [[nodiscard]] OversamplingFactor factor() const noexcept;
    [[nodiscard]] std::size_t ratio() const noexcept;

    /// The spec for processors that run inside the oversampled domain.
    [[nodiscard]] ProcessSpec oversampledSpec() const noexcept;

    /// Upsamples `input` into the internal buffer and returns a view of it,
    /// ratio() x input.numSamples() long. Process it in place, then call
    /// downsample(). Only the first maxBlockSize samples and numChannels
    /// channels of `input` are used.
    [[nodiscard]] AudioBlock upsample(const AudioBlock& input) noexcept;

    /// Downsamples the internal buffer (the view upsample() returned) into
    /// `output`, which should be the block that was upsampled.
    void downsample(const AudioBlock& output) noexcept;

    /// The delay of an upsample + downsample round trip at low frequencies,
    /// in base-rate samples: what a plugin reports for delay compensation.
    [[nodiscard]] float latencySamples() const noexcept;

private:
    struct ChannelFilters
    {
        HalfbandUpsampler<halfband::stage1.size()> up1{halfband::stage1};
        HalfbandUpsampler<halfband::stage2.size()> up2{halfband::stage2};
        HalfbandDownsampler<halfband::stage2.size()> down2{halfband::stage2};
        HalfbandDownsampler<halfband::stage1.size()> down1{halfband::stage1};

        void reset() noexcept;
    };

    OversamplingFactor requested;
    OversamplingFactor active;
    ProcessSpec baseSpec{};
    std::vector<ChannelFilters> filters;
    std::vector<float> storage;
    std::vector<float*> channelPointers;
};

} // namespace anoesis::dsp
