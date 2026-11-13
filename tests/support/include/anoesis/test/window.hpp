// Test support: analysis windows for spectral measurement.

#pragma once

#include <cstddef>
#include <vector>

namespace anoesis::test
{

/// Window samples plus the half-width of the window's main lobe in bins:
/// a pure tone's power lies within +-mainLobeBins of its frequency.
/// All windows are periodic ("DFT-even"), the right form for spectral analysis.
struct Window
{
    std::vector<double> samples;
    std::size_t mainLobeBins = 1;
};

/// No window. Exact for coherently sampled signals; heavy leakage otherwise.
[[nodiscard]] Window rectangularWindow(std::size_t n);

/// Hann: first sidelobe -31 dB, falling 18 dB/octave. Main lobe +-2 bins.
[[nodiscard]] Window hannWindow(std::size_t n);

/// 4-term Blackman-Harris: sidelobes below -92 dB. Main lobe +-4 bins.
[[nodiscard]] Window blackmanHarrisWindow(std::size_t n);

/// Kaiser: sidelobe level set by beta (13.5: ~-130 dB, 20: ~-190 dB). Main
/// lobe +-ceil(sqrt(1 + (beta / pi)^2)) bins.
[[nodiscard]] Window kaiserWindow(std::size_t n, double beta);

} // namespace anoesis::test
