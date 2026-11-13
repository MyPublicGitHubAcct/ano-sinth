// Test support: deterministic test signals.

#pragma once

#include <cstddef>
#include <vector>

namespace anoesis::test
{

/// amplitude * sin(2 pi f n / rate + phase), computed in double, stored as float.
[[nodiscard]] std::vector<float> sine(double frequencyHz, double sampleRate, std::size_t numSamples,
                                      double amplitude = 1.0, double phaseRadians = 0.0);

/// Zeros with `amplitude` at `position`.
[[nodiscard]] std::vector<float> impulse(std::size_t numSamples, std::size_t position = 0, float amplitude = 1.0f);

/// `numSamples` copies of `value`.
[[nodiscard]] std::vector<float> constant(std::size_t numSamples, float value);

/// The frequency nearest `targetHz` that completes an *odd* whole number of
/// cycles in `numSamples` samples ("coherent sampling"). With a power-of-two
/// length, an odd cycle count is coprime with it, so every harmonic, and
/// every alias of a harmonic, lands exactly on its own FFT bin, and a
/// rectangular window then has no leakage at all.
[[nodiscard]] double coherentFrequency(double targetHz, double sampleRate, std::size_t numSamples);

} // namespace anoesis::test
