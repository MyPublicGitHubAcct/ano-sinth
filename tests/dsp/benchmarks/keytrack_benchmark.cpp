// CPU cost of keytracking (dsp::keytrackOctaves, dsp::keytrackSource,
// docs/dsp/keytracking.md): each evaluated at the 16 grid points of a
// 512-sample block, as a voice feeds them; masterTuneSemitones() once; and
// the filter fed at each grid point against the filter fed once per block.
// One voice, 48 kHz, LP24 at k 2, a 110 Hz saw. Tagged
// [!benchmark][keytrack]: run with tools/bench.sh "[keytrack]" in
// relwithdebinfo. The candidate models it was chosen from are benchmarked at
// commit 3948d99. Results: docs/dsp/keytracking.md,
// docs/performance/baselines.md

#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/math/pitch.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

using anoesis::dsp::keytrackOctaves;
using anoesis::dsp::keytrackSource;
using anoesis::dsp::LadderFilter;
using anoesis::dsp::masterTuneSemitones;
using anoesis::test::Buffer;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr std::size_t grid = LadderFilter::gridSamples;
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

LadderFilter prepared()
{
    LadderFilter f;
    f.setCutoff(13.75f);
    f.setResonance(2.0f);
    f.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
    return f;
}

} // namespace

TEST_CASE("Keytracking: CPU", "[!benchmark][keytrack]")
{
    // A voice gliding and bending: a different pitch at every grid point.
    std::array<float, blockSize / grid> pitches{};
    for (std::size_t i = 0; i < pitches.size(); ++i)
    {
        const auto t = static_cast<float>(i * grid);
        pitches[i] = 60.0f - (0.001f * t) + (0.5f * std::sin(0.001f * t));
    }
    float tuneHz = 438.0f;

    BENCHMARK("keytrack: keytrackOctaves, 16 grid points, x512")
    {
        float sum = 0.0f;
        for (const float p : pitches)
        {
            sum += keytrackOctaves(p, 1.0f);
        }
        return sum;
    };
    BENCHMARK("keytrack: keytrackSource, 16 grid points, x512")
    {
        float sum = 0.0f;
        for (const float p : pitches)
        {
            sum += keytrackSource(p);
        }
        return sum;
    };
    BENCHMARK("keytrack: masterTuneSemitones, once per block, x512")
    {
        tuneHz += 1e-6f;
        return masterTuneSemitones(tuneHz);
    };

    const auto input = sawInput();
    {
        auto f = prepared();
        Buffer buffer{1, blockSize};
        BENCHMARK("keytrack: filter fed once per block, x512")
        {
            std::ranges::copy(input, buffer.channel(0).begin());
            f.setCutoffModulation(keytrackOctaves(pitches[0], 1.0f));
            f.process(buffer.block());
            return buffer.channel(0)[blockSize - 1];
        };
    }
    {
        auto f = prepared();
        Buffer buffer{1, blockSize};
        BENCHMARK("keytrack: filter fed at each grid point, x512")
        {
            std::ranges::copy(input, buffer.channel(0).begin());
            for (std::size_t i = 0; i < pitches.size(); ++i)
            {
                f.setCutoffModulation(keytrackOctaves(pitches[(i + 1) % pitches.size()], 1.0f));
                f.process(buffer.block().subBlock(i * grid, grid));
            }
            return buffer.channel(0)[blockSize - 1];
        };
    }
}
