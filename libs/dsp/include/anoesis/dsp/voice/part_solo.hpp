// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/process_spec.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

/// Solo's two gains (M1.4 task 5, decided 2026-11-22; docs/dsp/
/// part-solo-and-skipping.md), for the engine (M1.8) to hold and every voice
/// to read through VoiceKnobs::solo:
///
/// - Solo plays only the part the interface has selected (manual pp. 9-10).
///   It silences the other part's filter output: a part crossing into the
///   selected part's filter (AB) is still heard through it (the voice reads
///   these gains with Mix's, after the filter).
/// - Each part's gain is 0 or 1, ramped linearly over 10 ms (Mix's ramp):
///   Solo on ramps the other part to 0, Solo off ramps it back, and Part
///   select under Solo crosses both. Part select with Solo off changes no
///   gain, and is remembered for when Solo comes on.
/// - Solo and Part select are interface state, not parameters: not in
///   presets, not automated, not MIDI-mapped. Where they are kept (with the
///   host's session) is M1.9's; setImmediately() restores them without a
///   ramp.
///
/// The ramps are global LinearSmoothers, read by each voice at its own
/// offsets (LinearSmoother::valueAfter), as the Mix knobs are; the caller
/// splits its block at Solo and Part select events and skip()s the ramps
/// past each piece it renders. No allocation; real-time safe.
/// Doc: docs/dsp/part-solo-and-skipping.md
namespace anoesis::dsp
{

class PartSolo
{
public:
    static constexpr std::size_t partCount = 2;
    /// The ramp of each gain, for Solo and Part select alike.
    static constexpr float rampSeconds = 0.01f;

    PartSolo() noexcept
    {
        for (auto& g : ramps)
        {
            g.setRampTime(rampSeconds);
            g.setCurrentAndTarget(1.0f);
        }
    }

    /// Finishes any ramp: each gain at its target.
    void prepare(const ProcessSpec& spec) noexcept
    {
        for (auto& g : ramps)
        {
            g.prepare(spec);
        }
    }

    /// Solo on or off, from the next sample; the gains ramp.
    void setSolo(bool on) noexcept
    {
        soloOn = on;
        updateTargets();
    }

    /// The selected part, 0 (A) or 1 (B; anything above counts as 1), from
    /// the next sample. Under Solo, both gains ramp.
    void selectPart(std::size_t part) noexcept
    {
        selected = std::min(part, partCount - 1);
        updateTargets();
    }

    /// Solo and the selected part at once, each gain jumping to its value
    /// (restoring a session, or before the first note).
    void setImmediately(bool on, std::size_t part) noexcept
    {
        soloOn = on;
        selected = std::min(part, partCount - 1);
        for (std::size_t p = 0; p < partCount; ++p)
        {
            ramps[p].setCurrentAndTarget(targetFor(p));
        }
    }

    [[nodiscard]] bool isOn() const noexcept
    {
        return soloOn;
    }
    [[nodiscard]] std::size_t selectedPart() const noexcept
    {
        return selected;
    }

    /// The two ramps, for VoiceKnobs::solo: positioned at the next sample.
    [[nodiscard]] std::array<const LinearSmoother*, partCount> gains() const noexcept
    {
        return {ramps.data(), ramps.data() + 1};
    }

    /// Moves both ramps on past `numSamples` samples the caller has
    /// rendered.
    void skip(std::size_t numSamples) noexcept
    {
        for (auto& g : ramps)
        {
            g.skip(numSamples);
        }
    }

private:
    [[nodiscard]] float targetFor(std::size_t part) const noexcept
    {
        return !soloOn || part == selected ? 1.0f : 0.0f;
    }

    void updateTargets() noexcept
    {
        for (std::size_t p = 0; p < partCount; ++p)
        {
            ramps[p].setTarget(targetFor(p));
        }
    }

    std::array<LinearSmoother, partCount> ramps{};
    bool soloOn = false;
    std::size_t selected = 0;
};

} // namespace anoesis::dsp
