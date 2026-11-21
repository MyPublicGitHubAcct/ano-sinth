// CPU cost of the Filter Control's candidate combining laws
// (filter_control_candidates.hpp, docs/dsp/filter-control.md): each model
// evaluated for one voice's two filters at the 16 grid points of a 512-sample
// block, with every term moving (modulation and keytrack), as a voice feeds
// them; the resonance models the same way; and LadderFilter fed the sum
// through its own ramp (W1) against the whole sum through its modulation
// input (W2), the same cutoffs both ways (the sum 5, modulation on A
// clamped to [0, 10], keytrack 5 at C4: about 150 Hz to 2.7 kHz). 48 kHz. Tagged [!benchmark][filter-control]: run with
// tools/bench.sh "[filter-control]" in relwithdebinfo.

#include "../filters/filter_control_candidates.hpp"

#include <anoesis/dsp/filters/ladder_filter.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace anoesis::test::filter_control;
using anoesis::dsp::LadderFilter;
using anoesis::test::Buffer;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr std::size_t grid = LadderFilter::gridSamples;
constexpr std::size_t points = blockSize / grid;
constexpr float rate = 48000.0f;

struct GridInputs
{
    std::array<float, points> modA{};
    std::array<float, points> modB{};
    std::array<float, points> modAB{};
    std::array<float, points> pitch{};
};

GridInputs moving()
{
    GridInputs in;
    for (std::size_t i = 0; i < points; ++i)
    {
        const auto t = static_cast<float>(i * grid);
        in.modA[i] = 4.0f * std::sin(0.002f * t);
        in.modB[i] = -3.0f * std::sin(0.003f * t);
        in.modAB[i] = 2.0f * std::cos(0.001f * t);
        in.pitch[i] = 60.0f + (0.5f * std::sin(0.001f * t));
    }
    return in;
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

float evaluate(CutoffModel m, const GridInputs& in)
{
    float sum = 0.0f;
    for (std::size_t i = 0; i < points; ++i)
    {
        sum += cutoffOctaves(m, Clamp::perControl,
                             {
                                 .filter = 6.0f,
                                 .control = 5.0f,
                                 .filterMod = in.modA[i],
                                 .controlMod = in.modAB[i],
                                 .keytrack = 5.0f,
                                 .pitch = in.pitch[i],
                             });
        sum += cutoffOctaves(m, Clamp::perControl,
                             {
                                 .filter = 3.0f,
                                 .control = 5.0f,
                                 .filterMod = in.modB[i],
                                 .controlMod = in.modAB[i],
                                 .keytrack = 5.0f,
                                 .pitch = in.pitch[i],
                             });
    }
    return sum;
}

} // namespace

TEST_CASE("Filter Control: CPU", "[!benchmark][filter-control]")
{
    GridInputs in = moving();

    BENCHMARK("filter-control: (a1) knob sum, F3's law, 2 filters x 16 grid points, x512")
    {
        return evaluate(CutoffModel::knobSumF3, in);
    };
    BENCHMARK("filter-control: (a2) knob sum, 1 oct/unit, 2 filters x 16 grid points, x512")
    {
        return evaluate(CutoffModel::knobSumOctave, in);
    };
    BENCHMARK("filter-control: (b) centred offset, 2 filters x 16 grid points, x512")
    {
        return evaluate(CutoffModel::centredOffset, in);
    };
    BENCHMARK("filter-control: (c) Hz added, 2 filters x 16 grid points, x512")
    {
        return evaluate(CutoffModel::hzSum, in);
    };
    BENCHMARK("filter-control: resonance, knob units added, 2 filters x 16 grid points, x512")
    {
        float sum = 0.0f;
        for (std::size_t i = 0; i < points; ++i)
        {
            sum += resonanceK(ResonanceModel::knobSum, {.filter = 5.0f, .control = 2.0f, .filterMod = in.modA[i]});
            sum += resonanceK(ResonanceModel::knobSum, {.filter = 3.0f, .control = 2.0f, .filterMod = in.modB[i]});
        }
        return sum;
    };

    const auto x = sawInput();
    Buffer buffer{1, blockSize};
    // The same cutoffs both ways: (a2) with A 2, Control 3, the moving
    // modulation on A, keytrack 5.
    const auto sumAt = [&](std::size_t i)
    {
        return cutoffOctaves(
            CutoffModel::knobSumOctave, Clamp::perControl,
            {.filter = 2.0f, .control = 3.0f, .filterMod = in.modA[i], .keytrack = 5.0f, .pitch = in.pitch[i]});
    };
    const auto prepared = [&](float hz)
    {
        LadderFilter f;
        f.setResonance(2.0f);
        f.setCutoff(hz);
        f.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
        return f;
    };
    LadderFilter w1 = prepared(lcdLaw.hz(5.0f));
    LadderFilter w2 = prepared(lcdLaw.hz(0.0f));

    BENCHMARK("filter-control: LadderFilter, W1: the knobs via setCutoff, the rest at each grid point, x512")
    {
        std::ranges::copy(x, buffer.channel(0).begin());
        w1.setCutoff(lcdLaw.hz(5.0f));
        for (std::size_t i = 0; i < points; ++i)
        {
            w1.setCutoffModulation(sumAt(i) - lcdLaw.octaves(5.0f));
            w1.process(buffer.block().subBlock(i * grid, grid));
        }
        return buffer.channel(0)[blockSize - 1];
    };
    BENCHMARK("filter-control: LadderFilter, W2: the whole sum via the modulation input at each grid point, x512")
    {
        std::ranges::copy(x, buffer.channel(0).begin());
        for (std::size_t i = 0; i < points; ++i)
        {
            w2.setCutoffModulation(sumAt(i) - lcdLaw.log2HzAtZero);
            w2.process(buffer.block().subBlock(i * grid, grid));
        }
        return buffer.channel(0)[blockSize - 1];
    };
}
