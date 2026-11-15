// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <algorithm>
#include <cmath>

/// The tempo a synced parameter follows (docs/dsp/tempo-sync.md): the last
/// valid tempo the host reported, or 120 BPM until it reports one.
///
/// Decided with the model (2026-11-14): synced parameters follow the
/// reported tempo whether or not the transport plays, and never read the
/// song position. A host that reports no tempo (the standalone build, or a
/// host without a play head) leaves 120 BPM; a report that isn't positive
/// and finite is ignored; the rest are clamped to [1, 999] BPM, which every
/// common host's range lies within and which keeps every conversion finite.
/// Reading the host (JUCE's AudioPlayHead) is libs/interop's job (M1.9);
/// this takes a plain value. No allocation.
/// Doc: docs/dsp/tempo-sync.md
namespace anoesis::dsp
{

class HostTempo
{
public:
    static constexpr double defaultBpm = 120.0;
    static constexpr double minBpm = 1.0;
    static constexpr double maxBpm = 999.0;

    /// A tempo the host reported: kept if positive and finite, clamped.
    void report(double bpmValue) noexcept
    {
        if (bpmValue > 0.0 && std::isfinite(bpmValue))
        {
            current = std::clamp(bpmValue, minBpm, maxBpm);
        }
    }

    /// Back to 120 BPM (a new session with no host tempo yet).
    void reset() noexcept
    {
        current = defaultBpm;
    }

    [[nodiscard]] double bpm() const noexcept
    {
        return current;
    }

private:
    double current = defaultBpm;
};

} // namespace anoesis::dsp
