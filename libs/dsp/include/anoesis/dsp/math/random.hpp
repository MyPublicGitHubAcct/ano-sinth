// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <array>
#include <cstdint>

namespace anoesis::dsp
{

/// A small, fast, seedable pseudo-random generator: xoshiro128+ (Blackman and
/// Vigna, 2018), seeded through SplitMix64.
///
/// - Deterministic: the same seed gives the same sequence on every compiler
///   and CPU (integer arithmetic only), so noise is reproducible in tests,
///   golden renders, and presets.
/// - 32-bit operations only, so it is cheap on a Cortex-M as well as a desktop.
/// - 128 bits of state, period 2^128 - 1. Not cryptographic.
///
/// xoshiro128+'s lowest bits are its weakest; the float conversions use only
/// the top 24 bits, as its authors recommend. Doc: docs/dsp/white-noise.md
class Random
{
public:
    explicit Random(std::uint64_t seedValue = 0) noexcept
    {
        setSeed(seedValue);
    }

    /// Restarts the generator from a new seed. Every seed, including 0, gives a
    /// valid (non-zero) state.
    void setSeed(std::uint64_t seedValue) noexcept
    {
        initialSeed = seedValue;
        reset();
    }

    /// Restarts the sequence from the current seed.
    void reset() noexcept
    {
        // SplitMix64 spreads any seed over 128 bits of well-mixed state. Its
        // outputs are a bijection of a counter, so two consecutive zeros (an
        // all-zero state, which xoshiro must never have) cannot occur.
        std::uint64_t counter = initialSeed;
        const auto first = splitMix64(counter);
        const auto second = splitMix64(counter);
        state = {
            static_cast<std::uint32_t>(first),
            static_cast<std::uint32_t>(first >> 32U),
            static_cast<std::uint32_t>(second),
            static_cast<std::uint32_t>(second >> 32U),
        };
    }

    [[nodiscard]] std::uint64_t seed() const noexcept
    {
        return initialSeed;
    }

    /// The next 32 random bits.
    [[nodiscard]] std::uint32_t nextUint32() noexcept
    {
        const auto result = state[0] + state[3];
        const auto t = state[1] << 9U;
        state[2] ^= state[0];
        state[3] ^= state[1];
        state[1] ^= state[2];
        state[0] ^= state[3];
        state[2] ^= t;
        state[3] = rotateLeft(state[3], 11U);
        return result;
    }

    /// Uniform on (-1, 1): the 2^24 values (2k + 1 - 2^24) / 2^24. Symmetric
    /// about zero, exactly zero-mean over all values, never exactly 0, and
    /// exactly representable in float (so identical on every platform).
    [[nodiscard]] float nextBipolar() noexcept
    {
        constexpr std::int32_t twoTo24 = 16777216;
        const auto k = static_cast<std::int32_t>(nextUint32() >> 8U);
        return static_cast<float>((2 * k) + 1 - twoTo24) * 0x1p-24f;
    }

    /// Uniform on [0, 1): the 2^24 values k / 2^24.
    [[nodiscard]] float nextUnipolar() noexcept
    {
        return static_cast<float>(nextUint32() >> 8U) * 0x1p-24f;
    }

private:
    static std::uint64_t splitMix64(std::uint64_t& counter) noexcept
    {
        counter += 0x9E3779B97F4A7C15U;
        auto z = counter;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9U;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBU;
        return z ^ (z >> 31U);
    }

    static std::uint32_t rotateLeft(std::uint32_t x, unsigned int k) noexcept
    {
        return (x << k) | (x >> (32U - k));
    }

    std::uint64_t initialSeed = 0;
    std::array<std::uint32_t, 4> state{};
};

} // namespace anoesis::dsp
