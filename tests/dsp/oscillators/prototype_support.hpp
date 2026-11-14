// Shared test support for the anti-aliasing prototypes (decision D6): the
// list of candidates, how to construct each, the ideal waveforms, and
// renderers for the base rate and through the Oversampler.
// Doc: docs/dsp/band-limited-oscillators.md

#pragma once

#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/oscillators/prototype/common.hpp>
#include <anoesis/dsp/oscillators/prototype/min_blep.hpp>
#include <anoesis/dsp/oscillators/prototype/poly_blep.hpp>
#include <anoesis/dsp/oscillators/prototype/wavetable.hpp>
#include <anoesis/dsp/resampling/oversampler.hpp>

#include <anoesis/test/buffer.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <vector>

namespace anoesis::test::d6
{

namespace proto = anoesis::dsp::prototype;

using PolyBlep2 = proto::PolyBlepOscillator<2>;
using PolyBlep4 = proto::PolyBlepOscillator<4>;
using MinBlep32 = proto::MinBlepOscillator<32>;
using MinBlep64 = proto::MinBlepOscillator<64>;
using Wavetable = proto::WavetableOscillator;

// The shared tables, built once (off any audio thread, as the plugin would).
inline const proto::MinBlepTable& minBlepTable32()
{
    static const proto::MinBlepTable table{32};
    return table;
}

inline const proto::MinBlepTable& minBlepTable64()
{
    static const proto::MinBlepTable table{64};
    return table;
}

inline const proto::WavetableBank& wavetableBank()
{
    static const proto::WavetableBank bank;
    return bank;
}

/// How to build each candidate, its display name, and how closely it follows
/// the ideal waveform away from discontinuities.
template <typename Osc>
struct Candidate;

template <int Points>
struct Candidate<proto::PolyBlepOscillator<Points>>
{
    static constexpr const char* name = Points == 2 ? "PolyBLEP 2-point" : "PolyBLEP 4-point";
    static proto::PolyBlepOscillator<Points> make()
    {
        return {};
    }
    // The correction spans Points / 2 samples either side; beyond that the
    // output is the naive waveform, to float rounding.
    static constexpr std::size_t guardSamples = Points / 2;
    static constexpr double tolerance = 1e-5;
    static constexpr float peak = 1.5f; // measured at exactly 1: the residuals only round edges off
};

template <std::size_t Taps>
struct Candidate<proto::MinBlepOscillator<Taps>>
{
    static constexpr const char* name = Taps == 32 ? "minBLEP 32-tap" : "minBLEP 64-tap";
    static proto::MinBlepOscillator<Taps> make()
    {
        return proto::MinBlepOscillator<Taps>{Taps == 32 ? minBlepTable32() : minBlepTable64()};
    }
    // The residual lasts Taps samples after each discontinuity. Beyond that
    // only table truncation (the tables end at exactly zero) and the
    // linear-interpolated delay compensation are left.
    static constexpr std::size_t guardSamples = Taps;
    static constexpr double tolerance = 1e-4;
    // A minimum-phase step delivers its energy at once and overshoots by
    // about 37% (a linear-phase one by 9%); two close jumps add, up to 1.73
    // for a 10% pulse at 5 kHz.
    static constexpr float peak = 2.0f;
};

template <>
struct Candidate<proto::WavetableOscillator>
{
    static constexpr const char* name = "Wavetable (mipmapped)";
    static proto::WavetableOscillator make()
    {
        return proto::WavetableOscillator{wavetableBank()};
    }
    // A truncated Fourier series rings (Gibbs) on both sides of a jump, decaying
    // as 1 / distance: about 0.03 at 40 samples for a 100 Hz saw at 48 kHz,
    // whose table holds harmonics up to at least 12 kHz.
    static constexpr std::size_t guardSamples = 40;
    static constexpr double tolerance = 0.03;
    static constexpr float peak = 1.5f; // Gibbs overshoot, plus two saws' for a narrow pulse: 1.29 measured
};

/// The ideal (naive, continuous-time) waveforms at phase t in [0, 1).
inline double idealWave(proto::Shape shape, double t, double width = 0.5)
{
    switch (shape)
    {
    case proto::Shape::saw:
        return (2.0 * t) - 1.0;
    case proto::Shape::pulse:
        return t < width ? 1.0 : -1.0;
    case proto::Shape::triangle:
        return t < 0.5 ? (4.0 * t) - 1.0 : 3.0 - (4.0 * t);
    }
    return 0.0;
}

/// The ideal waveform's Fourier amplitude at harmonic k (sine/cosine
/// magnitude), for the droop measurement. Pulse: 50% duty (a square).
inline double idealHarmonic(proto::Shape shape, std::size_t k)
{
    const auto kd = static_cast<double>(k);
    switch (shape)
    {
    case proto::Shape::saw:
        return 2.0 / (std::numbers::pi * kd);
    case proto::Shape::pulse:
        return k % 2 == 1 ? 4.0 / (std::numbers::pi * kd) : 0.0;
    case proto::Shape::triangle:
        return k % 2 == 1 ? 8.0 / (std::numbers::pi * std::numbers::pi * kd * kd) : 0.0;
    }
    return 0.0;
}

/// The worst alias in a measureAliasing result whose "fundamental" was not a
/// tone of the signal (FM renders: the harmonic grid is the modulator's
/// rate), in dB relative to an ideal saw's fundamental, amplitude 2 / pi.
/// `gridPower` is the signal's power at that grid frequency.
inline double worstAliasReSawFundamental(double worstAliasDb, double gridPower)
{
    const double sawFundamentalPower = 2.0 / (std::numbers::pi * std::numbers::pi);
    return (10.0 * std::log10(gridPower / sawFundamentalPower)) + worstAliasDb;
}

/// The phase increment, in 2^-32 cycles per sample, that completes exactly
/// `cycles` cycles in `numSamples` samples (a power of two), when the
/// oscillator runs `ratio` times faster than the analysis rate. Exact, so the
/// render is periodic in numSamples and a rectangular window has no leakage.
inline std::uint32_t coherentIncrement(std::uint64_t cycles, std::size_t numSamples, std::size_t ratio = 1)
{
    return static_cast<std::uint32_t>((cycles << 32U) / (numSamples * ratio));
}

/// Called before each sample with the sample index (at the oscillator's own
/// rate); may change the oscillator's pitch. Empty: no modulation.
template <typename Osc>
using PerSample = std::function<void(Osc&, std::size_t)>;

/// Renders `numSamples` at the analysis rate after `settle` samples. With
/// ratio > 1 the oscillator runs at ratio x `sampleRate` and the result goes
/// through dsp::Oversampler::downsample, as an oversampled voice would.
template <typename Osc>
std::vector<float> render(Osc& osc, double sampleRate, std::size_t ratio, std::size_t settle, std::size_t numSamples,
                          const PerSample<Osc>& perSample = {})
{
    constexpr std::size_t blockSize = 512;
    // OversamplingFactor's values are the ratios themselves (1, 2, 4).
    dsp::Oversampler oversampler{static_cast<dsp::OversamplingFactor>(ratio)};
    const dsp::ProcessSpec baseSpec{
        .sampleRate = static_cast<float>(sampleRate),
        .maxBlockSize = blockSize,
        .numChannels = 1,
    };
    oversampler.prepare(baseSpec);

    std::vector<float> out;
    out.reserve(numSamples);
    Buffer buffer{1, blockSize};
    std::size_t innerIndex = 0;
    for (std::size_t done = 0; done < settle + numSamples; done += blockSize)
    {
        std::ranges::fill(buffer.channel(0), 0.0f);
        const auto inner = ratio == 1 ? buffer.block() : oversampler.upsample(buffer.block());
        for (auto& sample : inner.channel(0))
        {
            if (perSample)
            {
                perSample(osc, innerIndex);
            }
            ++innerIndex;
            sample = osc.next();
        }
        if (ratio > 1)
        {
            oversampler.downsample(buffer.block());
        }
        for (std::size_t i = 0; i < blockSize; ++i)
        {
            const auto n = done + i;
            if (n >= settle && n < settle + numSamples)
            {
                out.push_back(buffer.channel(0)[i]);
            }
        }
    }
    return out;
}

} // namespace anoesis::test::d6
