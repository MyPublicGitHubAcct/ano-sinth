// Test support: DSP quality measurements (THD, aliasing, frequency response).
// How they work and how to use them: docs/dsp/measuring-dsp.md

#pragma once

#include <anoesis/test/fft.hpp>
#include <anoesis/test/window.hpp>

#include <cstddef>
#include <functional>
#include <limits>
#include <span>
#include <vector>

namespace anoesis::test
{

/// 20 log10(ratio): an amplitude ratio in dB.
[[nodiscard]] double toDb(double amplitudeRatio) noexcept;

/// 10 log10(ratio): a power ratio in dB.
[[nodiscard]] double powerToDb(double powerRatio) noexcept;

/// Total harmonic distortion as an amplitude ratio:
/// sqrt(sum of harmonic powers 2..maxHarmonic) / sqrt(fundamental power).
/// Harmonics above Nyquist are ignored (they are aliasing; see measureAliasing).
[[nodiscard]] double measureThd(std::span<const float> signal, double fundamentalHz, double sampleRate,
                                const Window& window, std::size_t maxHarmonic = 10);

/// Everything in a periodic signal's spectrum that is *not* a harmonic of the
/// fundamental (nor DC), up to `maxFrequencyHz`. For a synthesised waveform
/// that is aliasing plus the noise floor.
struct AliasingResult
{
    double harmonicPower = 0.0; ///< power at the harmonics (k f0 < Nyquist)
    double aliasPower = 0.0;    ///< power everywhere else
    double aliasToSignalDb = 0; ///< aliasPower / harmonicPower, in dB
    double worstAliasDb = 0;    ///< the strongest single non-harmonic tone, relative to the fundamental
    double worstAliasHz = 0;    ///< ...and its frequency
};

[[nodiscard]] AliasingResult measureAliasing(std::span<const float> signal, double fundamentalHz, double sampleRate,
                                             const Window& window,
                                             double maxFrequencyHz = std::numeric_limits<double>::infinity());

/// The amplitude of each harmonic k f0 (index k - 1) up to `maxFrequencyHz`
/// and below Nyquist: sqrt(2 x the power within the harmonic's main lobe),
/// so a sine of amplitude A reads A. Compare with an ideal waveform's
/// Fourier series to measure passband droop.
[[nodiscard]] std::vector<double> measureHarmonics(std::span<const float> signal, double fundamentalHz,
                                                   double sampleRate, const Window& window,
                                                   double maxFrequencyHz = std::numeric_limits<double>::infinity());

/// A mono process under test: takes a whole input signal, returns the output
/// (same length and rate).
using MonoProcess = std::function<std::vector<float>(std::span<const float>)>;

struct ResponsePoint
{
    double frequencyHz = 0.0;
    double magnitude = 0.0; ///< output amplitude / input amplitude
    double magnitudeDb = 0.0;
    double phaseRadians = 0.0; ///< output phase - input phase, in (-pi, pi]
};

/// The steady-state response of `process` at one frequency. Feeds a sine of
/// `amplitude`, discards the first `settleSamples` of output, and correlates
/// the next `measureSamples` of input and output with e^(-i w n) under a
/// Kaiser (beta 20) window, so leakage from the sine's negative-frequency
/// image is ~-190 dB. Works for any process, linear or not, as a measurement
/// of the component at the input frequency.
[[nodiscard]] ResponsePoint measureResponseAt(const MonoProcess& process, double frequencyHz, double sampleRate,
                                              double amplitude = 0.5, std::size_t settleSamples = 4096,
                                              std::size_t measureSamples = 16384);

/// For a linear time-invariant process: the FFT of its response to a unit
/// impulse, `fftSize` samples long (a power of two), bins 0 .. fftSize/2. Gives
/// the whole frequency response at once; the impulse response must have
/// decayed within fftSize samples.
[[nodiscard]] std::vector<Complex> impulseResponseSpectrum(const MonoProcess& process, std::size_t fftSize);

} // namespace anoesis::test
