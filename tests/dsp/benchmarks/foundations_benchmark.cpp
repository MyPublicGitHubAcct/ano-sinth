// CPU baselines for the M1.0 building blocks (Phase 3 starts from these).
//
// Tagged [!benchmark]: hidden from the default run and from ctest. Run them
// in an optimised build with tools/bench.sh. Every benchmark processes one
// 512-sample block ("x512"), a typical host block, so per-call overhead is
// amortised as it will be in the plugin. Results: docs/performance/baselines.md

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/generators/white_noise.hpp>
#include <anoesis/dsp/math/fast_math.hpp>
#include <anoesis/dsp/resampling/oversampler.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace
{

constexpr std::size_t blockSize = 512;
using Block = std::array<float, blockSize>;

constexpr anoesis::dsp::ProcessSpec monoSpec{.sampleRate = 48000.0f, .maxBlockSize = blockSize, .numChannels = 1};

Block ramp(float from, float to)
{
    Block values{};
    for (std::size_t i = 0; i < blockSize; ++i)
    {
        values[i] = from + ((to - from) * static_cast<float>(i) / static_cast<float>(blockSize - 1));
    }
    return values;
}

// One-pole low-pass with no input (y += c (x - y), x = 0) over a block: the
// classic denormal generator. Returns the state so the work is not discarded.
float decayBlock(float state, float input)
{
    constexpr float coefficient = 0.1f;
    for (std::size_t i = 0; i < blockSize; ++i)
    {
        state += coefficient * (input - state);
    }
    return state;
}

} // namespace

TEST_CASE("Benchmark: fast math against the standard library", "[!benchmark][dsp][math]")
{
    const auto exponents = ramp(-10.0f, 10.0f);
    const auto drives = ramp(-4.0f, 4.0f);
    Block out{};

    BENCHMARK("std::exp2 x512")
    {
        for (std::size_t i = 0; i < blockSize; ++i)
        {
            out[i] = std::exp2(exponents[i]);
        }
        return out[blockSize - 1];
    };
    BENCHMARK("fastExp2 x512")
    {
        for (std::size_t i = 0; i < blockSize; ++i)
        {
            out[i] = anoesis::dsp::fastExp2(exponents[i]);
        }
        return out[blockSize - 1];
    };
    BENCHMARK("std::tanh x512")
    {
        for (std::size_t i = 0; i < blockSize; ++i)
        {
            out[i] = std::tanh(drives[i]);
        }
        return out[blockSize - 1];
    };
    BENCHMARK("fastTanh x512")
    {
        for (std::size_t i = 0; i < blockSize; ++i)
        {
            out[i] = anoesis::dsp::fastTanh(drives[i]);
        }
        return out[blockSize - 1];
    };
}

TEST_CASE("Benchmark: parameter smoothing", "[!benchmark][dsp][smoothing]")
{
    Block out{};
    anoesis::dsp::LinearSmoother linear;
    anoesis::dsp::ExponentialSmoother exponential;
    linear.setRampTime(1.0f); // longer than a block: every sample is mid-ramp
    exponential.setRampTime(1.0f);
    linear.prepare(monoSpec);
    exponential.prepare(monoSpec);

    BENCHMARK("LinearSmoother ramping x512")
    {
        linear.setCurrentAndTarget(0.0f);
        linear.setTarget(1.0f);
        linear.fill(out);
        return out[blockSize - 1];
    };
    BENCHMARK("ExponentialSmoother ramping x512")
    {
        exponential.setCurrentAndTarget(0.0f);
        exponential.setTarget(1.0f);
        exponential.fill(out);
        return out[blockSize - 1];
    };
    BENCHMARK("LinearSmoother settled x512")
    {
        linear.setCurrentAndTarget(0.5f);
        linear.fill(out);
        return out[blockSize - 1];
    };
}

TEST_CASE("Benchmark: white noise", "[!benchmark][dsp][noise]")
{
    Block out{};
    anoesis::dsp::WhiteNoise noise{1};
    BENCHMARK("WhiteNoise x512")
    {
        noise.fill(out);
        return out[blockSize - 1];
    };
}

TEST_CASE("Benchmark: oversampling round trip (mono)", "[!benchmark][dsp][oversampling]")
{
    auto signal = ramp(-0.5f, 0.5f);
    std::array<float*, 1> channels{signal.data()};
    const anoesis::dsp::AudioBlock block{channels.data(), 1, blockSize};

    for (const auto factor : {anoesis::dsp::OversamplingFactor::two, anoesis::dsp::OversamplingFactor::four})
    {
        anoesis::dsp::Oversampler oversampler{factor};
        oversampler.prepare(monoSpec);
        const auto* const name = factor == anoesis::dsp::OversamplingFactor::two ? "Oversampler 2x up+down x512"
                                                                                 : "Oversampler 4x up+down x512";
        BENCHMARK(name)
        {
            static_cast<void>(oversampler.upsample(block));
            oversampler.downsample(block);
            return signal[0];
        };
    }
}

TEST_CASE("Benchmark: the cost of denormals", "[!benchmark][dsp][denormals]")
{
    // The same loop three ways. Normal: state and input at 1e-3, so the state
    // stays normal. Denormal: state starts in the denormal range with no
    // input, and (by rounding) stays there. Every run restarts from values
    // read through volatile, so the compiler cannot specialise any of them.
    volatile float normalStart = 1e-3f;
    volatile float normalInput = 1e-3f;
    volatile float denormalStart = std::numeric_limits<float>::min() / 16.0f;
    volatile float silence = 0.0f;

    BENCHMARK("one-pole, normal state x512")
    {
        return decayBlock(normalStart, normalInput);
    };
    BENCHMARK("one-pole, denormal state, IEEE mode x512")
    {
        return decayBlock(denormalStart, silence);
    };
    {
        const anoesis::dsp::ScopedFlushDenormals guard;
        BENCHMARK("one-pole, denormal state, flush-to-zero x512")
        {
            return decayBlock(denormalStart, silence);
        };
    }
}
