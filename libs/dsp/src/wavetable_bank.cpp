// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio
//
// Builds the mipmapped saw and triangle tables (D6 prototype), additively,
// in double precision. Runs once, at initialisation.

#include <anoesis/dsp/oscillators/prototype/wavetable.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace anoesis::dsp::prototype
{

WavetableBank::WavetableBank() : saws(levels * (tableSize + 1)), triangles(levels * (tableSize + 1))
{
    // One period of sine, so harmonic k at sample i is sine[(k i) mod N]:
    // exact, and no trigonometry in the inner loop.
    std::vector<double> sine(tableSize);
    std::vector<double> cosine(tableSize);
    for (std::size_t i = 0; i < tableSize; ++i)
    {
        const double w = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(tableSize);
        sine[i] = std::sin(w);
        cosine[i] = std::cos(w);
    }

    std::vector<double> saw(tableSize);
    std::vector<double> triangle(tableSize);
    for (std::size_t level = 0; level < levels; ++level)
    {
        std::ranges::fill(saw, 0.0);
        std::ranges::fill(triangle, 0.0);
        const auto harmonics = harmonicsAt(level);
        for (std::size_t k = 1; k <= harmonics; ++k)
        {
            // Rising saw 2t - 1 = -(2 / pi) sum sin(2 pi k t) / k.
            // Triangle (-1 at t = 0) = -(8 / pi^2) sum over odd k of cos(2 pi k t) / k^2.
            const auto kd = static_cast<double>(k);
            const double sawAmplitude = -2.0 / (std::numbers::pi * kd);
            const double triangleAmplitude = k % 2 == 1 ? -8.0 / (std::numbers::pi * std::numbers::pi * kd * kd) : 0.0;
            for (std::size_t i = 0; i < tableSize; ++i)
            {
                const auto index = (k * i) % tableSize;
                saw[i] += sawAmplitude * sine[index];
                triangle[i] += triangleAmplitude * cosine[index];
            }
        }
        auto* sawOut = saws.data() + (level * (tableSize + 1));
        auto* triangleOut = triangles.data() + (level * (tableSize + 1));
        for (std::size_t i = 0; i < tableSize; ++i)
        {
            sawOut[i] = static_cast<float>(saw[i]);
            triangleOut[i] = static_cast<float>(triangle[i]);
        }
        sawOut[tableSize] = sawOut[0];
        triangleOut[tableSize] = triangleOut[0];
    }
}

} // namespace anoesis::dsp::prototype
