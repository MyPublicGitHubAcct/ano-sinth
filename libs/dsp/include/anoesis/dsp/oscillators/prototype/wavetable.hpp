// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/oscillators/prototype/common.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

/// D6 prototype: mipmapped band-limited wavetables.
///
/// Each waveform is stored as a stack of single-cycle tables ("mipmap
/// levels"), each built additively from a truncated Fourier series: level m
/// holds harmonics 1..1024 / 2^m, so each level has an octave fewer. At a
/// given pitch the oscillator reads the richest level whose top harmonic
/// folds back, if at all, above 20 kHz (it may exceed Nyquist by that much),
/// with linear interpolation. The pulse is the difference of two saws half a
/// width apart, so the width can change freely. Tables are shared by all
/// voices; a voice holds only its phase and a level index.
///
/// Doc: docs/dsp/band-limited-oscillators.md
namespace anoesis::dsp::prototype
{

/// The tables for every level of the saw and the triangle. Built once at
/// construction (allocates); read-only afterwards, so any number of
/// oscillators on any thread may share one.
class WavetableBank
{
public:
    static constexpr std::size_t tableSize = 2048;
    static constexpr std::size_t levels = 11;

    WavetableBank();

    /// Harmonics in level `level`: 1024, 512, ..., 1.
    [[nodiscard]] static constexpr std::size_t harmonicsAt(std::size_t level) noexcept
    {
        return std::size_t{1024} >> level;
    }

    /// tableSize + 1 samples (the last repeats the first, for interpolation).
    /// Pulse has no table of its own: use the saw's.
    [[nodiscard]] const float* table(Shape shape, std::size_t level) const noexcept
    {
        const auto& source = shape == Shape::triangle ? triangles : saws;
        return source.data() + (level * (tableSize + 1));
    }

    [[nodiscard]] std::size_t bytes() const noexcept
    {
        return (saws.size() + triangles.size()) * sizeof(float);
    }

private:
    std::vector<float> saws;
    std::vector<float> triangles;
};

class WavetableOscillator
{
public:
    /// The bank must outlive the oscillator.
    explicit WavetableOscillator(const WavetableBank& bank) noexcept : bank(&bank) {}

    void prepare(const ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        // Harmonics may reach fs - 20 kHz: above Nyquist they fold to above
        // 20 kHz. At least Nyquist, so low rates never alias at all.
        const float folded = 1.0f - (audibleLimitHz / sampleRate);
        limit = folded > 0.5f ? folded : 0.5f;
        setIncrement(increment);
        reset();
    }

    void reset() noexcept
    {
        phase = 0;
    }

    void setShape(Shape newShape) noexcept
    {
        shape = newShape;
    }

    void setPulseWidth(float width) noexcept
    {
        widthPhase = widthToPhase(width);
        pulseOffset = (2.0f * toCycles(widthPhase)) - 1.0f;
    }

    void setFrequency(float hz) noexcept
    {
        setIncrement(incrementFor(hz, sampleRate));
    }

    void setIncrement(std::uint32_t newIncrement) noexcept
    {
        increment = newIncrement < halfCycle ? newIncrement : halfCycle;
        level = levelFor(toCycles(increment), limit);
    }

    [[nodiscard]] float latencySamples() const noexcept
    {
        return 0.0f;
    }

    /// The richest level whose top harmonic, at `cyclesPerSample`, is at most
    /// `limit` cycles per sample.
    [[nodiscard]] static std::size_t levelFor(float cyclesPerSample, float limit) noexcept
    {
        const float ratio = cyclesPerSample * static_cast<float>(WavetableBank::harmonicsAt(0)) / limit;
        if (!(ratio > 1.0f))
        {
            return 0;
        }
        const auto m = static_cast<std::size_t>(ceilLog2(ratio));
        return m < WavetableBank::levels ? m : WavetableBank::levels - 1;
    }

    [[nodiscard]] float next() noexcept
    {
        float y = 0.0f;
        switch (shape)
        {
        case Shape::saw:
            y = read(bank->table(Shape::saw, level), phase);
            break;
        case Shape::pulse:
        {
            const float* saw = bank->table(Shape::saw, level);
            y = read(saw, phase - widthPhase) - read(saw, phase) + pulseOffset;
            break;
        }
        case Shape::triangle:
            y = read(bank->table(Shape::triangle, level), phase);
            break;
        }
        phase += increment;
        return y;
    }

    static constexpr std::size_t stateBytes() noexcept
    {
        return sizeof(WavetableOscillator);
    }

private:
    static constexpr float audibleLimitHz = 20000.0f;
    static constexpr unsigned indexShift = 21; // 32 - log2(tableSize)
    static constexpr std::uint32_t fractionMask = (1U << indexShift) - 1U;

    [[nodiscard]] static float read(const float* table, std::uint32_t at) noexcept
    {
        const auto i = at >> indexShift;
        const float frac = static_cast<float>(at & fractionMask) * (1.0f / static_cast<float>(1U << indexShift));
        return table[i] + (frac * (table[i + 1] - table[i]));
    }

    const WavetableBank* bank;
    float sampleRate = 48000.0f;
    float limit = 0.5f;
    std::uint32_t phase = 0;
    std::uint32_t increment = 0;
    std::uint32_t widthPhase = halfCycle;
    float pulseOffset = 0.0f;
    std::size_t level = 0;
    Shape shape = Shape::saw;
};

} // namespace anoesis::dsp::prototype
