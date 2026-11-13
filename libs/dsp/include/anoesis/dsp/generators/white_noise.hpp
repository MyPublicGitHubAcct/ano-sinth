// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/random.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace anoesis::dsp
{

/// White noise: independent samples, uniform on (-1, 1), so a flat spectrum
/// and an RMS level of 1/sqrt(3) (-4.77 dBFS). Deterministic for a given seed
/// (see Random), so every render of a patch is identical.
///
/// A BlockProcessor: process() *overwrites* every channel with noise. Samples
/// are drawn sample-major (channel 0, 1, ... of sample n, then sample n + 1),
/// so the output does not depend on how a block is split into chunks, and the
/// channels are independent of each other.
///
/// Doc: docs/dsp/white-noise.md
class WhiteNoise
{
public:
    explicit WhiteNoise(std::uint64_t seed = 0) noexcept : random(seed) {}

    /// Nothing depends on the sample rate; prepare() just restarts the sequence.
    void prepare(const ProcessSpec& /*spec*/) noexcept
    {
        reset();
    }

    /// Restarts the sequence from the seed.
    void reset() noexcept
    {
        random.reset();
    }

    /// Changes the seed and restarts the sequence.
    void setSeed(std::uint64_t seed) noexcept
    {
        random.setSeed(seed);
    }

    /// The next noise sample.
    [[nodiscard]] float next() noexcept
    {
        return random.nextBipolar();
    }

    /// Writes `out.size()` consecutive noise samples.
    void fill(std::span<float> out) noexcept
    {
        for (auto& sample : out)
        {
            sample = next();
        }
    }

    /// Overwrites every channel of `block` with independent noise.
    void process(const AudioBlock& block) noexcept
    {
        for (std::size_t n = 0; n < block.numSamples(); ++n)
        {
            for (std::size_t ch = 0; ch < block.numChannels(); ++ch)
            {
                block.channel(ch)[n] = next();
            }
        }
    }

private:
    Random random;
};

} // namespace anoesis::dsp
