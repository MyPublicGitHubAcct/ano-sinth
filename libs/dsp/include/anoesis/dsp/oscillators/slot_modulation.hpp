// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <cstddef>
#include <span>

/// What a voice tells an oscillator slot of its modulation for one
/// process() or skip() call (M1.5 task 4, docs/dsp/
/// modulation-destinations.md): the sums of the routes to the slot's part,
/// in knob units, at the rates decided in M1.5 task 2.
/// Doc: docs/dsp/modulation-destinations.md
namespace anoesis::dsp
{

struct SlotModulation
{
    /// Volume's sum per sample of the call (Volume: 0-10 knob units);
    /// empty: no route to it.
    std::span<const float> volume;
    /// Pan is modulated: its sum ramps from `panBefore` at the segment's
    /// first grid point to `panNow` at its last (Pan: 0-10, 5 the centre).
    bool pan = false;
    float panBefore = 0.0f;
    float panNow = 0.0f;
    /// The call's first sample's position in the voice's grid segment, and
    /// the segment's length.
    std::size_t position = 0;
    std::size_t segment = 32;
    /// Phase's sum per sample (10 units a cycle); empty: the offset in force
    /// is kept.
    std::span<const float> phase;

    /// Volume or Pan is modulated: the slot computes its gains itself.
    [[nodiscard]] constexpr bool gainsModulated() const noexcept
    {
        return !volume.empty() || pan;
    }
    [[nodiscard]] constexpr bool active() const noexcept
    {
        return gainsModulated() || !phase.empty();
    }
};

} // namespace anoesis::dsp
