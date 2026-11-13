// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <cmath>
#include <cstddef>

namespace anoesis::dsp
{

/// Everything a processor needs to know before it runs. Passed to prepare().
///
/// The sample rate is a float: every standard rate (44.1k to 768k) is an
/// integer below 2^24, so float holds it exactly, and a Cortex-M4F has no
/// double-precision FPU. Hosts that report a double rate convert it once.
///
/// Lifecycle rules: docs/dsp/processor-lifecycle.md
struct ProcessSpec
{
    float sampleRate = 0.0f;      ///< Hz.
    std::size_t maxBlockSize = 0; ///< No later process() call passes more samples than this.
    std::size_t numChannels = 0;  ///< No later process() call passes more channels than this.

    /// True when every field is usable: a finite, positive rate and non-zero sizes.
    [[nodiscard]] bool isValid() const noexcept
    {
        return std::isfinite(sampleRate) && sampleRate > 0.0f && maxBlockSize > 0 && numChannels > 0;
    }
};

} // namespace anoesis::dsp
