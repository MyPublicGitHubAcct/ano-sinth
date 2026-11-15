// CPU cost of keytracking (docs/dsp/keytracking.md): each candidate model
// (keytrack_candidates.hpp) evaluated at the 16 grid points of a 512-sample
// block, as a voice would feed it, and the filter with that feed against the
// filter fed once per block. One voice, 48 kHz, LP24 at k 2, a 110 Hz saw.
// Tagged [!benchmark][keytrack]: run with tools/bench.sh "[keytrack]" in
// relwithdebinfo. Results: docs/dsp/keytracking.md,
// docs/performance/baselines.md

#include "../filters/keytrack_candidates.hpp"

#include <anoesis/dsp/filters/ladder_filter.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

using anoesis::dsp::LadderFilter;
using anoesis::test::Buffer;
namespace kt = anoesis::test::keytrack;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr std::size_t grid = LadderFilter::gridSamples;
constexpr float rate = 48000.0f;

/// A voice gliding and bending: a different pitch at every grid point.
kt::VoicePitch pitchAt(std::size_t n)
{
    const auto t = static_cast<double>(n);
    return {.key = 60.0, .glide = -0.001 * t, .bend = 0.5 * std::sin(0.001 * t), .masterTuneHz = 438.0};
}

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
    f.setCutoff(1000.0f);
    f.setResonance(2.0f);
    f.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
    return f;
}

} // namespace

TEST_CASE("Keytracking: CPU", "[!benchmark][keytrack]")
{
    kt::DetuneTable<15> table;
    for (std::size_t i = 0; i < table.k.size(); ++i)
    {
        table.k[i] = 4.0f + (0.3f * static_cast<float>(i));
        table.cents[i] = -10.0f * static_cast<float>(i);
    }
    std::array<kt::VoicePitch, blockSize / grid> pitches{};
    for (std::size_t i = 0; i < pitches.size(); ++i)
    {
        pitches[i] = pitchAt(i * grid);
    }

    BENCHMARK("keytrack: (a) note number, 16 grid points, x512")
    {
        float sum = 0.0f;
        for (const auto& p : pitches)
        {
            sum += kt::noteNumberOctaves(p, 1.0, kt::referenceA4);
        }
        return sum;
    };
    BENCHMARK("keytrack: (b) sounding pitch, 16 grid points, x512")
    {
        float sum = 0.0f;
        for (const auto& p : pitches)
        {
            sum += kt::soundingPitchOctaves(p, 1.0, kt::referenceA4);
        }
        return sum;
    };
    BENCHMARK("keytrack: (c) linear in Hz, 16 grid points, x512")
    {
        float sum = 0.0f;
        for (const auto& p : pitches)
        {
            sum += kt::linearHzOctaves(p, 1.0, 100.0);
        }
        return sum;
    };
    BENCHMARK("keytrack: (d) sounding pitch + c(k), 16 grid points, x512")
    {
        float sum = 0.0f;
        for (std::size_t i = 0; i < pitches.size(); ++i)
        {
            sum +=
                kt::correctedOctaves(pitches[i], 1.0, kt::referenceA4, table, 4.0f + (0.25f * static_cast<float>(i)));
        }
        return sum;
    };

    const auto input = sawInput();
    {
        auto f = prepared();
        Buffer buffer{1, blockSize};
        BENCHMARK("keytrack: filter fed once per block, x512")
        {
            std::ranges::copy(input, buffer.channel(0).begin());
            f.setCutoffModulation(kt::soundingPitchOctaves(pitches[0], 1.0, kt::referenceA4));
            f.process(buffer.block());
            return buffer.channel(0)[blockSize - 1];
        };
    }
    {
        auto f = prepared();
        Buffer buffer{1, blockSize};
        BENCHMARK("keytrack: filter fed per grid point, x512")
        {
            std::ranges::copy(input, buffer.channel(0).begin());
            for (std::size_t i = 0; i < pitches.size(); ++i)
            {
                f.setCutoffModulation(kt::soundingPitchOctaves(pitches[i], 1.0, kt::referenceA4));
                f.process(buffer.block().subBlock(i * grid, grid));
            }
            return buffer.channel(0)[blockSize - 1];
        };
    }
}
