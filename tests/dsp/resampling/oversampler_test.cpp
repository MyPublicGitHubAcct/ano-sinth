// Tests for 2x and 4x oversampling: passband ripple, image and alias
// rejection, latency, and the lifecycle. Specification (decision log):
// pass 0..20 kHz at a 44.1 kHz base rate, reject >= 100 dB from 24.1 kHz.
// Doc: docs/dsp/oversampling.md

#include <anoesis/dsp/resampling/oversampler.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/lifecycle.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/signals.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <anoesis/dsp/core/processor.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

using anoesis::dsp::AudioBlock;
using anoesis::dsp::Oversampler;
using anoesis::dsp::OversamplingFactor;
using anoesis::dsp::ProcessSpec;
using anoesis::test::Buffer;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinULP;

namespace
{

constexpr double baseRate = 44100.0;
constexpr std::size_t blockSize = 256;
constexpr std::size_t settle = 4096; // base-rate samples: the IIR filters decay far below -200 dB by then

ProcessSpec monoSpec()
{
    return {.sampleRate = static_cast<float>(baseRate), .maxBlockSize = blockSize, .numChannels = 1};
}

std::size_t ratioOf(OversamplingFactor factor)
{
    return static_cast<std::size_t>(factor);
}

// Up then down, block by block: the oversampler as an effect would use it,
// with nothing processed in between.
anoesis::test::MonoProcess roundTrip(OversamplingFactor factor)
{
    return [factor](std::span<const float> input)
    {
        Oversampler oversampler{factor};
        oversampler.prepare(monoSpec());
        Buffer buffer{1, input.size()};
        std::ranges::copy(input, buffer.channel(0).begin());
        for (std::size_t start = 0; start < input.size(); start += blockSize)
        {
            const auto block = buffer.block().subBlock(start, blockSize);
            static_cast<void>(oversampler.upsample(block));
            oversampler.downsample(block);
        }
        return std::vector<float>(buffer.channel(0).begin(), buffer.channel(0).end());
    };
}

// The upsampled signal for `input`, at ratio x the base rate.
std::vector<float> upsampleAll(OversamplingFactor factor, std::span<const float> input)
{
    Oversampler oversampler{factor};
    oversampler.prepare(monoSpec());
    Buffer buffer{1, input.size()};
    std::ranges::copy(input, buffer.channel(0).begin());
    std::vector<float> output;
    for (std::size_t start = 0; start < input.size(); start += blockSize)
    {
        const auto up = oversampler.upsample(buffer.block().subBlock(start, blockSize));
        output.insert(output.end(), up.channel(0).begin(), up.channel(0).end());
    }
    return output;
}

// Downsamples a signal that is already at ratio x the base rate, by writing it
// into the oversampled buffer that upsample() hands out.
std::vector<float> downsampleAll(OversamplingFactor factor, std::span<const float> highRateInput)
{
    const auto ratio = ratioOf(factor);
    Oversampler oversampler{factor};
    oversampler.prepare(monoSpec());
    Buffer output{1, highRateInput.size() / ratio};
    for (std::size_t start = 0; start < output.numSamples(); start += blockSize)
    {
        const auto block = output.block().subBlock(start, blockSize);
        const auto up = oversampler.upsample(block); // its contents are replaced by the test signal
        std::ranges::copy(highRateInput.subspan(start * ratio, up.numSamples()), up.channel(0).begin());
        oversampler.downsample(block);
    }
    return {output.channel(0).begin(), output.channel(0).end()};
}

// The strongest component other than the one at `signalHz` (and DC),
// relative to it, in dB. Rectangular window: callers use coherent frequencies.
double worstSpuriousDb(std::span<const float> x, double rate, double signalHz)
{
    const auto spectrum = anoesis::test::powerSpectrum(x, rate, anoesis::test::rectangularWindow(x.size()));
    const auto signalBin = spectrum.nearestBin(signalHz);
    double worst = 0.0;
    for (std::size_t k = 1; k < spectrum.power.size(); ++k)
    {
        if (k + 1 < signalBin || k > signalBin + 1)
        {
            worst = std::max(worst, spectrum.power[k]);
        }
    }
    return anoesis::test::powerToDb(worst / spectrum.powerAround(signalHz, 1));
}

// A BlockProcessor wrapper (up, then down), for the lifecycle contract.
struct RoundTripProcessor
{
    Oversampler oversampler;
    void prepare(const ProcessSpec& spec)
    {
        oversampler.prepare(spec);
    }
    void reset() noexcept
    {
        oversampler.reset();
    }
    void process(const AudioBlock& block) noexcept
    {
        static_cast<void>(oversampler.upsample(block));
        oversampler.downsample(block);
    }
};

} // namespace

TEST_CASE("Oversampler: configuration and the oversampled spec", "[dsp][oversampling]")
{
    STATIC_CHECK(anoesis::dsp::Preparable<Oversampler>);

    Oversampler oversampler{OversamplingFactor::four};
    oversampler.prepare({.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 2});
    CHECK(oversampler.ratio() == 4);
    const auto inner = oversampler.oversampledSpec();
    CHECK_THAT(inner.sampleRate, WithinULP(192000.0f, 0));
    CHECK(inner.maxBlockSize == 2048);
    CHECK(inner.numChannels == 2);

    oversampler.setFactor(OversamplingFactor::two); // takes effect at the next prepare()
    CHECK(oversampler.ratio() == 4);
    oversampler.prepare({.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 2});
    CHECK(oversampler.ratio() == 2);
}

TEST_CASE("Oversampler honours the lifecycle contract", "[dsp][oversampling]")
{
    const auto factor = GENERATE(OversamplingFactor::two, OversamplingFactor::four);
    const ProcessSpec stereo{.sampleRate = 44100.0f, .maxBlockSize = 128, .numChannels = 2};
    const auto result = anoesis::test::checkLifecycleContract(
        [factor] { return RoundTripProcessor{Oversampler{factor}}; }, stereo, 700);
    CHECK(result.resetMatchesFreshPrepare);
    CHECK(result.rePrepareMatchesFreshPrepare);
}

TEST_CASE("Oversampler: factor 1 passes audio through unchanged", "[dsp][oversampling]")
{
    const auto input = anoesis::test::sine(1000.0, baseRate, 1000, 0.8);
    const auto output = roundTrip(OversamplingFactor::none)(input);
    CHECK(std::ranges::equal(input, output, [](float a, float b) { return a <= b && a >= b; }));
}

TEST_CASE("Oversampler: passband is flat from DC to 20 kHz", "[dsp][oversampling]")
{
    // Up + down is linear and time-invariant to within the images' -100 dB
    // twice over, so its impulse response gives the whole passband at once.
    const auto factor = GENERATE(OversamplingFactor::two, OversamplingFactor::four);
    const auto response = anoesis::test::impulseResponseSpectrum(roundTrip(factor), 16384);
    const auto lastPassBin = static_cast<std::size_t>(20000.0 / (baseRate / 16384.0));

    double lowest = 1e9;
    double highest = -1e9;
    for (std::size_t k = 0; k <= lastPassBin; ++k)
    {
        const auto db = anoesis::test::toDb(std::abs(response[k]));
        lowest = std::min(lowest, db);
        highest = std::max(highest, db);
    }
    const auto rippleDb = highest - lowest;
    CAPTURE(ratioOf(factor), rippleDb, lowest, highest);
    // Designed ripple: 3.4e-10 dB for two passes. Measuring through float
    // filters adds rounding of about 1e-7 relative per sample (1e-6 dB), so
    // 1e-4 dB is a bound the float implementation must meet with margin.
    CHECK(rippleDb < 1e-4);
    CHECK(std::abs(highest) < 1e-4); // unity gain: 0 dB, not just flat
}

TEST_CASE("Oversampler: upsampling rejects images by at least 100 dB", "[dsp][oversampling]")
{
    // A tone at f in the passband must come out of the upsampler alone: its
    // images (k * 44.1 kHz +- f) are rejected by the halfband stopbands.
    const auto factor = GENERATE(OversamplingFactor::two, OversamplingFactor::four);
    const auto target = GENERATE(1000.0, 10000.0, 17000.0, 20000.0);
    constexpr std::size_t n = 8192;
    const auto f = anoesis::test::coherentFrequency(target, baseRate, n);

    const auto up = upsampleAll(factor, anoesis::test::sine(f, baseRate, settle + n, 0.5));
    const auto ratio = ratioOf(factor);
    const std::span<const float> steady{up.data() + (settle * ratio), n * ratio};
    const auto worst = worstSpuriousDb(steady, baseRate * static_cast<double>(ratio), f);
    CAPTURE(ratio, f, worst);
    CHECK(worst < -100.0);
}

TEST_CASE("Oversampler: downsampling rejects aliases by at least 100 dB", "[dsp][oversampling]")
{
    // A tone above 24.1 kHz in the oversampled domain (for example a
    // distortion harmonic) would fold back into 0..20 kHz. The decimation
    // filters must remove it before it can.
    const auto factor = GENERATE(OversamplingFactor::two, OversamplingFactor::four);
    const auto ratio = ratioOf(factor);
    const auto highRate = baseRate * static_cast<double>(ratio);
    constexpr std::size_t n = 8192;

    for (const double target : {24200.0, 30000.0, 40000.0, 44000.0, 60000.0, 87000.0})
    {
        if (target >= highRate / 2.0)
        {
            continue;
        }
        const auto f = anoesis::test::coherentFrequency(target, highRate, n * ratio);
        const auto input = anoesis::test::sine(f, highRate, (settle + n) * ratio, 0.5);
        const auto output = downsampleAll(factor, input);
        const std::span<const float> steady{output.data() + settle, n};

        double outputPower = 0.0;
        for (const auto v : steady)
        {
            outputPower += static_cast<double>(v) * static_cast<double>(v);
        }
        outputPower /= static_cast<double>(n);
        const auto rejectionDb = anoesis::test::powerToDb(outputPower / 0.125); // input power: 0.5^2 / 2
        CAPTURE(ratio, f, rejectionDb);
        CHECK(rejectionDb < -100.0);
    }
}

TEST_CASE("Oversampler: latencySamples() is the measured delay", "[dsp][oversampling]")
{
    // Phase delay at 20 Hz equals the DC group delay to within O(w^2): about
    // 1e-5 samples here. The tolerance of 0.01 samples is for the
    // measurement's own window and rounding.
    const auto factor = GENERATE(OversamplingFactor::two, OversamplingFactor::four);
    const auto point = anoesis::test::measureResponseAt(roundTrip(factor), 20.0, baseRate, 0.5, 8192, 65536);
    const auto measuredDelay = -point.phaseRadians / (2.0 * std::numbers::pi * 20.0 / baseRate);

    Oversampler oversampler{factor};
    oversampler.prepare(monoSpec());
    CAPTURE(ratioOf(factor), measuredDelay);
    CHECK_THAT(oversampler.latencySamples(), WithinAbs(measuredDelay, 0.01));
    CHECK(oversampler.latencySamples() < 5.0); // a few samples, unlike a linear-phase FIR
}

TEST_CASE("Oversampler: channels are independent", "[dsp][oversampling]")
{
    Oversampler oversampler{OversamplingFactor::four};
    oversampler.prepare({.sampleRate = 44100.0f, .maxBlockSize = 64, .numChannels = 2});
    Buffer buffer{2, 64};
    std::ranges::copy(anoesis::test::sine(1000.0, baseRate, 64, 1.0), buffer.channel(0).begin());
    const auto up = oversampler.upsample(buffer.block());
    REQUIRE(up.numSamples() == 256);
    oversampler.downsample(buffer.block());
    CHECK(
        std::ranges::all_of(buffer.channel(1), [](float v) { return v <= 0.0f && v >= 0.0f; })); // silent stays silent
}

TEST_CASE("Oversampler: stable and bounded on a full-scale square wave", "[dsp][oversampling]")
{
    const auto factor = GENERATE(OversamplingFactor::two, OversamplingFactor::four);
    std::vector<float> square(20000);
    for (std::size_t i = 0; i < square.size(); ++i)
    {
        square[i] = (i / 50) % 2 == 0 ? 1.0f : -1.0f;
    }
    const auto output = roundTrip(factor)(square);
    // A band-limited square overshoots by the Gibbs phenomenon (~9% of the
    // step), plus the filters' non-linear phase. 1.5 bounds it with room.
    const auto peak = std::ranges::max(output, {}, [](float v) { return std::abs(v); });
    CHECK(std::isfinite(peak));
    CHECK(std::abs(peak) < 1.5f);
}
