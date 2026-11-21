// CPU cost of the Filter Control's combining law (filter_control.hpp,
// docs/dsp/filter-control.md) and stage 1's other candidates
// (filter_control_candidates.hpp): each evaluated for one voice's two filters
// at the 16 grid points of a 512-sample block, with every term moving
// (modulation and keytrack), as a voice feeds them; the resonance the same
// way; the voice's whole feed (the four knob ramps read with valueAfter(),
// keytrack, both filters' cutoff and k); and LadderFilter fed the sum through
// its own ramp (W1) against the whole sum through its modulation input (W2,
// built), with and without k through the resonance modulation input, the
// same cutoffs every way (the sum 5, modulation on A clamped to [0, 10],
// keytrack 5 at C4: about 150 Hz to 2.7 kHz). 48 kHz. Tagged
// [!benchmark][filter-control]: run with tools/bench.sh "[filter-control]" in
// relwithdebinfo.

#include "../filters/filter_control_candidates.hpp"

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace anoesis::test::filter_control;
namespace dsp = anoesis::dsp;
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
    BENCHMARK("filter-control: (a2) built, filterCutoffOctaves, 2 filters x 16 grid points, x512")
    {
        float sum = 0.0f;
        for (std::size_t i = 0; i < points; ++i)
        {
            const float keytrack = dsp::keytrackOctaves(in.pitch[i], 0.5f);
            sum += dsp::filterCutoffOctaves({
                .cutoff = 6.0f,
                .cutoffModulation = in.modA[i],
                .controlCutoff = 5.0f,
                .controlModulation = in.modAB[i],
                .keytrack = keytrack,
            });
            sum += dsp::filterCutoffOctaves({
                .cutoff = 3.0f,
                .cutoffModulation = in.modB[i],
                .controlCutoff = 5.0f,
                .controlModulation = in.modAB[i],
                .keytrack = keytrack,
            });
        }
        return sum;
    };
    BENCHMARK("filter-control: (b) centred offset, 2 filters x 16 grid points, x512")
    {
        return evaluate(CutoffModel::centredOffset, in);
    };
    BENCHMARK("filter-control: (c) Hz added, 2 filters x 16 grid points, x512")
    {
        return evaluate(CutoffModel::hzSum, in);
    };
    BENCHMARK("filter-control: resonance built, filterResonanceK, 2 filters x 16 grid points, x512")
    {
        float sum = 0.0f;
        for (std::size_t i = 0; i < points; ++i)
        {
            sum +=
                dsp::filterResonanceK({.resonance = 5.0f, .resonanceModulation = in.modA[i], .controlResonance = 2.0f});
            sum +=
                dsp::filterResonanceK({.resonance = 3.0f, .resonanceModulation = in.modB[i], .controlResonance = 2.0f});
        }
        return sum;
    };

    // The voice's whole feed: the six global knob ramps mid-ramp (both
    // Cutoffs, both Resonances, the Filter Control's two), read at each
    // segment end with valueAfter(); keytrack once; both filters' cutoff and k.
    std::array<dsp::LinearSmoother, 6> ramps{};
    for (std::size_t r = 0; r < ramps.size(); ++r)
    {
        ramps[r].prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
        ramps[r].setRampTime(0.02f); // longer than the block: every read is mid-ramp
        ramps[r].setCurrentAndTarget(2.0f);
        ramps[r].setTarget(3.0f + static_cast<float>(r));
    }
    BENCHMARK("filter-control: the voice's feed, 6 ramps read + keytrack + 2 filters' cutoff and k, 16 grid points, "
              "x512")
    {
        float sum = 0.0f;
        for (std::size_t i = 0; i < points; ++i)
        {
            const auto end = (i + 1) * grid;
            const float keytrack = dsp::keytrackOctaves(in.pitch[i], 0.5f);
            const float controlCutoff = ramps[4].valueAfter(end);
            const float controlResonance = ramps[5].valueAfter(end);
            sum += dsp::filterCutoffOctaves({
                .cutoff = ramps[0].valueAfter(end),
                .cutoffModulation = in.modA[i],
                .controlCutoff = controlCutoff,
                .controlModulation = in.modAB[i],
                .keytrack = keytrack,
            });
            sum += dsp::filterCutoffOctaves({
                .cutoff = ramps[1].valueAfter(end),
                .cutoffModulation = in.modB[i],
                .controlCutoff = controlCutoff,
                .controlModulation = in.modAB[i],
                .keytrack = keytrack,
            });
            sum += dsp::filterResonanceK({
                .resonance = ramps[2].valueAfter(end),
                .resonanceModulation = in.modA[i],
                .controlResonance = controlResonance,
            });
            sum += dsp::filterResonanceK({
                .resonance = ramps[3].valueAfter(end),
                .resonanceModulation = in.modB[i],
                .controlResonance = controlResonance,
            });
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
    LadderFilter w2 = prepared(dsp::filterControlLaw.anchorHz);
    LadderFilter w2k = prepared(dsp::filterControlLaw.anchorHz);
    w2k.setResonance(0.0f);
    w2k.setResonanceModulation(2.0f);
    w2k.reset();

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
    BENCHMARK("filter-control: LadderFilter, W2 + k via setResonanceModulation at each grid point (built), x512")
    {
        std::ranges::copy(x, buffer.channel(0).begin());
        for (std::size_t i = 0; i < points; ++i)
        {
            w2k.setCutoffModulation(sumAt(i) - lcdLaw.log2HzAtZero);
            w2k.setResonanceModulation(2.0f + (0.1f * in.modA[i]));
            w2k.process(buffer.block().subBlock(i * grid, grid));
        }
        return buffer.channel(0)[blockSize - 1];
    };
}
