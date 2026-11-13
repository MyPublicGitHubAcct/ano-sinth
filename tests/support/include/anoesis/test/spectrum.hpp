// Test support: windowed power spectra.

#pragma once

#include <anoesis/test/window.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace anoesis::test
{

/// A one-sided power spectrum, bins 0 .. N/2.
///
/// Normalisation: power[k] = c_k |X[k]|^2 / (N * sum(w^2)), with c_k = 2
/// except at DC and Nyquist. Then:
///   - the bins of a broadband signal sum to its mean square (Parseval);
///   - a sine of amplitude A puts A^2 / 2 into its main lobe, whatever the
///     window and wherever the frequency falls between bins.
/// So "power within the main lobe around f" is the power of the tone at f.
struct Spectrum
{
    std::vector<double> power;
    double sampleRate = 0.0;
    std::size_t fftSize = 0;
    std::size_t mainLobeBins = 1;

    [[nodiscard]] double binWidth() const noexcept;
    [[nodiscard]] double frequencyOf(std::size_t bin) const noexcept;
    /// The bin nearest `frequencyHz`, clamped to [0, N/2].
    [[nodiscard]] std::size_t nearestBin(double frequencyHz) const noexcept;
    /// The summed power of the bins within +-halfWidth of nearestBin(frequencyHz).
    [[nodiscard]] double powerAround(double frequencyHz, std::size_t halfWidth) const noexcept;
};

/// The power spectrum of `signal`, whose length must be a power of two and
/// equal to the window's.
[[nodiscard]] Spectrum powerSpectrum(std::span<const float> signal, double sampleRate, const Window& window);

/// Welch's method: the average of the power spectra of consecutive,
/// non-overlapping segments as long as the window (a power of two). Averaging
/// K segments cuts the per-bin estimation noise by sqrt(K). A trailing partial
/// segment is ignored.
[[nodiscard]] Spectrum averagedPowerSpectrum(std::span<const float> signal, double sampleRate, const Window& window);

} // namespace anoesis::test
