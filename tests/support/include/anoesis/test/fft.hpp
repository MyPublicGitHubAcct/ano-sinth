// Test support: a radix-2 FFT in double precision (decision D5).

#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace anoesis::test
{

using Complex = std::complex<double>;

[[nodiscard]] bool isPowerOfTwo(std::size_t n) noexcept;

/// In-place iterative radix-2 Cooley-Tukey FFT: X[k] = sum x[n] e^(-2 pi i k n / N).
/// The inverse (inverse = true) includes the 1/N factor, so it undoes the
/// forward transform. Throws std::invalid_argument unless the size is a power
/// of two.
void fftInPlace(std::span<Complex> data, bool inverse = false);

/// The FFT of a real signal: bins 0 .. N/2 (N/2 + 1 values). N must be a
/// power of two.
[[nodiscard]] std::vector<Complex> realFft(std::span<const double> signal);

} // namespace anoesis::test
