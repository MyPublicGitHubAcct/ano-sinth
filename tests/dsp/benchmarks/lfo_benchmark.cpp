// CPU cost of the LFO (Lfo, docs/dsp/lfos.md): one LFO for a 512-sample
// block per row, through process() and through next() per sample, and a
// 16-voice synth's 32 LFOs (two per voice). Tagged [!benchmark][lfo]: run
// with tools/bench.sh "[lfo]" in relwithdebinfo. The candidate models it was
// chosen from are benchmarked at commit dae814e. Results: docs/dsp/lfos.md,
// docs/performance/baselines.md

#include <anoesis/dsp/control/lfo.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using anoesis::dsp::Lfo;
using anoesis::dsp::LfoWaveform;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;
using Block = std::array<float, blockSize>;

Lfo prepared(LfoWaveform waveform, float hz, std::uint64_t seed = 1)
{
    Lfo lfo{seed};
    lfo.setWaveform(waveform);
    lfo.setFrequency(hz);
    lfo.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
    return lfo;
}

} // namespace

TEST_CASE("LFO: CPU", "[!benchmark][lfo]")
{
    Block out{};
    for (const auto& [waveform, name] : {
             std::pair{LfoWaveform::sine, "sine"},
             std::pair{LfoWaveform::saw, "saw"},
             std::pair{LfoWaveform::triangle, "triangle"},
             std::pair{LfoWaveform::pulse, "pulse"},
             std::pair{LfoWaveform::sampleHold, "S&H"},
             std::pair{LfoWaveform::smooth, "smooth"},
             std::pair{LfoWaveform::lorenz, "Lorenz"},
         })
    {
        auto blocks = prepared(waveform, 3.0f);
        BENCHMARK(std::string{"LFO: "} + name + " 3 Hz, process() x512")
        {
            blocks.process(out);
            return out[blockSize - 1];
        };
    }
    for (const auto& [waveform, name] : {std::pair{LfoWaveform::sine, "sine"}, std::pair{LfoWaveform::saw, "saw"}})
    {
        auto samples = prepared(waveform, 3.0f);
        BENCHMARK(std::string{"LFO: "} + name + " 3 Hz, next() per sample x512")
        {
            for (auto& y : out)
            {
                y = samples.next();
            }
            return out[blockSize - 1];
        };
    }
    auto fast = prepared(LfoWaveform::saw, 1000.0f);
    BENCHMARK("LFO: saw 1 kHz (an edge every 48 samples), process() x512")
    {
        fast.process(out);
        return out[blockSize - 1];
    };
    auto ramping = prepared(LfoWaveform::sine, 3.0f);
    float gain = 1.0f;
    BENCHMARK("LFO: sine 3 Hz with its gain ramping, process() x512")
    {
        gain = gain > 0.75f ? 0.5f : 1.0f;
        ramping.setGain(gain);
        ramping.process(out);
        return out[blockSize - 1];
    };

    // 16 voices x 2 LFOs, a sine and a saw, at different rates.
    std::vector<Lfo> voices;
    voices.reserve(32);
    for (std::size_t v = 0; v < 32; ++v)
    {
        voices.push_back(
            prepared(v % 2 == 0 ? LfoWaveform::sine : LfoWaveform::saw, 0.5f + (0.37f * static_cast<float>(v)), v + 1));
    }
    BENCHMARK("LFO: 32 LFOs (16 voices x 2), sine and saw, process() x512")
    {
        float sum = 0.0f;
        for (auto& lfo : voices)
        {
            lfo.process(out);
            sum += out[blockSize - 1];
        }
        return sum;
    };
}
