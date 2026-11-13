#include <anoesis/test/fft.hpp>

#include <numbers>
#include <stdexcept>
#include <utility>

namespace anoesis::test
{

bool isPowerOfTwo(std::size_t n) noexcept
{
    return n != 0 && (n & (n - 1)) == 0;
}

void fftInPlace(std::span<Complex> data, bool inverse)
{
    const auto n = data.size();
    if (!isPowerOfTwo(n))
    {
        throw std::invalid_argument("fftInPlace: the size must be a power of two");
    }

    // Bit-reversal permutation: afterwards, every butterfly stage works on
    // adjacent pairs of sub-transforms.
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        auto bit = n >> 1U;
        for (; (j & bit) != 0; bit >>= 1U)
        {
            j ^= bit;
        }
        j ^= bit;
        if (i < j)
        {
            std::swap(data[i], data[j]);
        }
    }

    // Twiddle factors, each computed directly with polar() rather than by a
    // recurrence, so their error does not grow with N.
    const auto sign = inverse ? 1.0 : -1.0;
    std::vector<Complex> twiddles(n / 2);
    for (std::size_t k = 0; k < n / 2; ++k)
    {
        twiddles[k] = std::polar(1.0, sign * 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n));
    }

    // Butterflies: log2(N) stages, each combining pairs of length/2 transforms.
    for (std::size_t length = 2; length <= n; length <<= 1U)
    {
        const auto half = length / 2;
        const auto stride = n / length;
        for (std::size_t start = 0; start < n; start += length)
        {
            for (std::size_t k = 0; k < half; ++k)
            {
                const auto u = data[start + k];
                const auto v = data[start + k + half] * twiddles[k * stride];
                data[start + k] = u + v;
                data[start + k + half] = u - v;
            }
        }
    }

    if (inverse)
    {
        for (auto& value : data)
        {
            value /= static_cast<double>(n);
        }
    }
}

std::vector<Complex> realFft(std::span<const double> signal)
{
    std::vector<Complex> data(signal.begin(), signal.end());
    fftInPlace(data);
    data.resize((signal.size() / 2) + 1);
    return data;
}

} // namespace anoesis::test
