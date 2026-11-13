// Tests for the lifecycle contract checker. It must pass a processor that
// honours the contract, and catch one that does not.

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/lifecycle.hpp>

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/processor.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

using anoesis::dsp::AudioBlock;
using anoesis::dsp::ProcessSpec;
using anoesis::test::Buffer;
using Catch::Matchers::WithinULP;

namespace
{

// A running sum per channel: y[n] = y[n-1] + x[n]. Its state is exactly what
// reset() has to clear.
class Integrator
{
public:
    explicit Integrator(bool resetClearsState = true) noexcept : clearsState(resetClearsState) {}

    void prepare(const ProcessSpec& spec)
    {
        sums.assign(spec.numChannels, 0.0f);
    }

    void reset() noexcept
    {
        if (clearsState)
        {
            std::ranges::fill(sums, 0.0f);
        }
    }

    void process(const AudioBlock& block) noexcept
    {
        for (std::size_t ch = 0; ch < block.numChannels(); ++ch)
        {
            for (auto& sample : block.channel(ch))
            {
                sums[ch] += sample;
                sample = sums[ch];
            }
        }
    }

private:
    std::vector<float> sums;
    bool clearsState;
};

} // namespace

TEST_CASE("Buffer owns channels and exposes them as an AudioBlock", "[support]")
{
    Buffer buffer{2, 16};
    REQUIRE(buffer.numChannels() == 2);
    REQUIRE(buffer.numSamples() == 16);

    buffer.channel(1)[5] = 0.25f;
    const auto block = buffer.block();
    REQUIRE(block.numChannels() == 2);
    REQUIRE(block.numSamples() == 16);
    CHECK_THAT(block.channel(1)[5], WithinULP(0.25f, 0));
    CHECK_THAT(block.channel(0)[5], WithinULP(0.0f, 0)); // a new buffer is silent
}

TEST_CASE("Lifecycle contract: a processor whose reset() clears its state passes", "[support]")
{
    const ProcessSpec spec{.sampleRate = 48000.0f, .maxBlockSize = 64, .numChannels = 2};
    const auto result = anoesis::test::checkLifecycleContract([] { return Integrator{true}; }, spec, 200);
    CHECK(result.resetMatchesFreshPrepare);
    CHECK(result.rePrepareMatchesFreshPrepare);
    CHECK(result.passed());
}

TEST_CASE("Lifecycle contract: a processor that keeps state across reset() fails", "[support]")
{
    const ProcessSpec spec{.sampleRate = 48000.0f, .maxBlockSize = 64, .numChannels = 2};
    const auto result = anoesis::test::checkLifecycleContract([] { return Integrator{false}; }, spec, 200);
    CHECK_FALSE(result.resetMatchesFreshPrepare);
    CHECK(result.rePrepareMatchesFreshPrepare); // prepare() re-assigns the sums, so this part holds
    CHECK_FALSE(result.passed());
}
