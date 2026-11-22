// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>

#include <cmath>
#include <cstddef>

namespace anoesis::dsp
{

/// The last stage of the output (inventory §5, X12), chosen by the project
/// owner (docs/dsp/mixer.md): a non-finite sample (NaN, +-inf) becomes 0, and
/// is counted, so that tests can require none and a bug can't hide behind
/// it. Finite samples, subnormals included, pass unchanged: flush-to-zero
/// (ScopedFlushDenormals) handles subnormals. Nothing in the voice path
/// produces a non-finite sample; this is for M1.8's engine to place at its
/// very end.
///
/// Satisfies BlockProcessor. Nothing allocates.
/// Doc: docs/dsp/mixer.md
class OutputGuard
{
public:
    void prepare(const ProcessSpec& spec) noexcept
    {
        static_cast<void>(spec);
        reset();
    }
    /// Zeroes the count.
    void reset() noexcept
    {
        total = 0;
    }

    /// Zeroes every non-finite sample of every channel, in place; returns how
    /// many there were in this block.
    std::size_t process(const AudioBlock& block) noexcept
    {
        std::size_t found = 0;
        for (std::size_t c = 0; c < block.numChannels(); ++c)
        {
            for (auto& v : block.channel(c))
            {
                const bool bad = !std::isfinite(v);
                found += bad ? 1 : 0;
                v = bad ? 0.0f : v;
            }
        }
        total += found;
        return found;
    }
    /// Non-finite samples zeroed since prepare() or reset().
    [[nodiscard]] std::size_t count() const noexcept
    {
        return total;
    }

private:
    std::size_t total = 0;
};

} // namespace anoesis::dsp
