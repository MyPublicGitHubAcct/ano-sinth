// CPU cost of the filter (LadderFilter, docs/dsp/filters.md): one channel
// for a 512-sample block at 48 kHz, LP24 at 1 kHz on a 110 Hz saw at
// -6 dBFS, Warm Drive on (tanh) and off (cubic), at typical settings (k 2,
// drive 2) and heavy ones (k 6, drive 16, where Newton works hardest); a
// stereo filter; a cutoff ramp in progress and the modulation input set
// every 32 samples; bypass; inside the Oversampler at 2x and 4x; and a
// 16-voice synth's 32 stereo filters. Tagged [!benchmark][filter]: run with
// tools/bench.sh "[filter]" in relwithdebinfo. The candidate models it was
// chosen from are benchmarked at commit a0e9899. Results:
// docs/dsp/filters.md, docs/performance/baselines.md

#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/resampling/oversampler.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <tuple>
#include <vector>

using anoesis::dsp::LadderFilter;
using anoesis::dsp::Oversampler;
using anoesis::dsp::OversamplingFactor;
using anoesis::test::Buffer;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;

std::vector<float> sawInput()
{
    std::vector<float> x(blockSize);
    double phase = 0.0;
    for (auto& v : x)
    {
        v = static_cast<float>(phase - 0.5);
        phase += 110.0 / static_cast<double>(rate);
        phase -= std::floor(phase);
    }
    return x;
}

LadderFilter prepared(bool warm, float k, float drive, std::size_t channels = 1, float sampleRate = rate)
{
    LadderFilter f;
    f.setWarm(warm);
    f.setCutoff(1000.0f);
    f.setResonance(k);
    f.setDrive(drive);
    f.prepare({.sampleRate = sampleRate, .maxBlockSize = blockSize * 4, .numChannels = channels});
    return f;
}

void fill(Buffer& buffer, const std::vector<float>& input)
{
    for (std::size_t ch = 0; ch < buffer.numChannels(); ++ch)
    {
        std::ranges::copy(input, buffer.channel(ch).begin());
    }
}

} // namespace

TEST_CASE("Filters: CPU", "[!benchmark][filter]")
{
    const auto input = sawInput();
    for (const bool warm : {true, false})
    {
        const std::string name = warm ? "Warm on (tanh)" : "Warm off (cubic)";
        for (const auto& [k, drive, setting] :
             {std::tuple{2.0f, 2.0f, "k 2, drive 2"}, std::tuple{6.0f, 16.0f, "k 6, drive 16"}})
        {
            auto f = prepared(warm, k, drive);
            Buffer buffer{1, blockSize};
            BENCHMARK("filter: " + name + ", " + setting + ", x512")
            {
                fill(buffer, input);
                f.process(buffer.block());
                return buffer.channel(0)[blockSize - 1];
            };
        }

        auto stereo = prepared(warm, 2.0f, 2.0f, 2);
        Buffer two{2, blockSize};
        BENCHMARK("filter: " + name + ", stereo (2 channels), k 2, drive 2, x512")
        {
            fill(two, input);
            stereo.process(two.block());
            return two.channel(1)[blockSize - 1];
        };

        auto ramping = prepared(warm, 2.0f, 2.0f);
        Buffer buffer{1, blockSize};
        bool high = false;
        BENCHMARK("filter: " + name + ", a cutoff ramp every block, x512")
        {
            high = !high;
            ramping.setCutoff(high ? 4000.0f : 500.0f);
            fill(buffer, input);
            ramping.process(buffer.block());
            return buffer.channel(0)[blockSize - 1];
        };

        auto modulated = prepared(warm, 2.0f, 2.0f);
        float octaves = 0.0f;
        BENCHMARK("filter: " + name + ", modulation every 32 samples, x512")
        {
            fill(buffer, input);
            for (std::size_t i = 0; i < blockSize; i += 32)
            {
                octaves = octaves > 2.0f ? -2.0f : octaves + 0.13f;
                modulated.setCutoffModulation(octaves);
                modulated.process(buffer.block().subBlock(i, 32));
            }
            return buffer.channel(0)[blockSize - 1];
        };

        for (const auto factor : {OversamplingFactor::two, OversamplingFactor::four})
        {
            Oversampler oversampler{factor};
            oversampler.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
            auto inner = prepared(warm, 2.0f, 2.0f, 1, rate * static_cast<float>(oversampler.ratio()));
            BENCHMARK("filter: " + name + (factor == OversamplingFactor::two ? ", 2x" : ", 4x") +
                      " (with the Oversampler), k 2, drive 2, x512")
            {
                fill(buffer, input);
                inner.process(oversampler.upsample(buffer.block()));
                oversampler.downsample(buffer.block());
                return buffer.channel(0)[blockSize - 1];
            };
        }
    }

    auto bypassed = prepared(true, 2.0f, 2.0f);
    bypassed.setBypassed(true);
    Buffer buffer{1, blockSize};
    BENCHMARK("filter: bypassed, x512")
    {
        fill(buffer, input);
        bypassed.process(buffer.block());
        return buffer.channel(0)[blockSize - 1];
    };

    // 16 voices x 2 filters, each stereo: 64 filter channels.
    std::vector<LadderFilter> voices;
    voices.reserve(32);
    for (std::size_t v = 0; v < 32; ++v)
    {
        voices.push_back(prepared(true, 2.0f, 2.0f, 2));
        voices.back().setCutoff(500.0f + (100.0f * static_cast<float>(v)));
        voices.back().reset(); // no ramp
    }
    Buffer stereo{2, blockSize};
    BENCHMARK("filter: 16 voices x 2 stereo filters, Warm on, k 2, drive 2, x512")
    {
        for (auto& f : voices)
        {
            fill(stereo, input);
            f.process(stereo.block());
        }
        return stereo.channel(1)[blockSize - 1];
    };
}
