// CPU cost of the filter candidates (filter_candidates.hpp,
// docs/dsp/filters.md): one mono filter channel for a 512-sample block at
// 48 kHz, LP24 at 1 kHz, on a 110 Hz saw at -6 dBFS: typical settings (k 2,
// drive 2) and heavy ones (k 6, drive 16, where the Newton solver works
// hardest); oversampled models include the Oversampler. Then the cutoff set
// every sample (a coefficient per sample), and the coefficient computations
// alone. Tagged [!benchmark][filter]: run with tools/bench.sh "[filter]" in
// relwithdebinfo. Also four independent channels interleaved per sample, as
// a voice loop would run them.

#include "../filters/filter_candidates.hpp"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <tuple>

using namespace anoesis::test::filters;
using anoesis::dsp::OversamplingFactor;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr double rate = 48000.0;
using Block = std::array<float, blockSize>;

Block sawInput()
{
    Block x{};
    double phase = 0.0;
    for (auto& v : x)
    {
        v = static_cast<float>(phase - 0.5);
        phase += 110.0 / rate;
        phase -= std::floor(phase);
    }
    return x;
}

template <typename Model>
void configure(Model& model, float k, float drive)
{
    model.prepare(rate);
    model.setCutoff(1000.0);
    model.setResonance(k);
    model.setDrive(drive);
}

template <typename Model>
void benchmarkModel(const std::string& name, Model model)
{
    const auto input = sawInput();
    Block io{};
    for (const auto& [k, drive, setting] :
         {std::tuple{2.0f, 2.0f, "k 2, drive 2"}, std::tuple{6.0f, 16.0f, "k 6, drive 16"}})
    {
        configure(model, k, drive);
        BENCHMARK("filter: " + name + ", " + setting + ", x512")
        {
            io = input;
            processBlock(model, std::span{io});
            return io[blockSize - 1];
        };
    }
}

template <typename Model>
void benchmarkModulated(const std::string& name, Model model)
{
    const auto input = sawInput();
    Block cutoff{};
    for (std::size_t i = 0; i < blockSize; ++i)
    {
        cutoff[i] = static_cast<float>(1000.0 * std::exp2(std::sin(static_cast<double>(i) * 0.01)));
    }
    Block io{};
    configure(model, 2.0f, 2.0f);
    BENCHMARK("filter: " + name + ", cutoff every sample, x512")
    {
        io = input;
        processModulated(model, std::span{io}, std::span<const float>{cutoff});
        return io[blockSize - 1];
    };
}

/// Four independent channels, interleaved sample by sample: the throughput a
/// voice loop can reach when the out-of-order core overlaps four dependency
/// chains (per channel-sample: divide by 2048).
template <typename Model>
void benchmarkInterleaved(const std::string& name)
{
    const auto input = sawInput();
    std::array<Model, 4> models{};
    for (auto& m : models)
    {
        configure(m, 2.0f, 2.0f);
    }
    std::array<Block, 4> io{};
    BENCHMARK("filter: " + name + ", 4 channels interleaved, k 2, drive 2, x2048")
    {
        for (std::size_t i = 0; i < blockSize; ++i)
        {
            for (std::size_t c = 0; c < 4; ++c)
            {
                io[c][i] = models[c].process(input[i]);
            }
        }
        return io[3][blockSize - 1];
    };
}

} // namespace

TEST_CASE("Filters: CPU", "[!benchmark][filter]")
{
    benchmarkModel("Linear ZDF", LinearZdf{});
    benchmarkModel("Linearised ZDF (tanh)", LinearisedZdf<TanhSat>{});
    benchmarkModel("Linearised ZDF (cubic)", LinearisedZdf<CubicSat>{});
    benchmarkModel("Feedback-only ZDF (tanh)", FeedbackOnlyZdf<TanhSat>{});
    benchmarkModel("Newton ZDF, 1 step (tanh)", NewtonZdf<TanhSat, 1>{});
    benchmarkModel("Newton ZDF (tanh)", NewtonZdf<TanhSat>{});
    benchmarkModel("Newton ZDF (cubic)", NewtonZdf<CubicSat>{});
    benchmarkModel("Newton ZDF (algebraic)", NewtonZdf<AlgebraicSat>{});
    benchmarkModel("Unit-delay ladder (tanh)", UnitDelayLadder<TanhSat>{});
    benchmarkModel("Linear SVF", LinearSvf{});
    using HuovExact = Huovilainen<TanhSat, HuovilainenTuning::exact>;
    benchmarkModel("Huovilainen exact, 2x", Oversampled<HuovExact>{OversamplingFactor::two});
    benchmarkModel("Huovilainen exact, 4x", Oversampled<HuovExact>{OversamplingFactor::four});
    benchmarkModel("Newton ZDF (tanh), 2x", Oversampled<NewtonZdf<TanhSat>>{OversamplingFactor::two});
    benchmarkModel("Linearised ZDF (tanh), 2x", Oversampled<LinearisedZdf<TanhSat>>{OversamplingFactor::two});
    benchmarkModel("Linearised ZDF (tanh), 4x", Oversampled<LinearisedZdf<TanhSat>>{OversamplingFactor::four});

    benchmarkInterleaved<LinearZdf>("Linear ZDF");
    benchmarkInterleaved<LinearisedZdf<TanhSat>>("Linearised ZDF (tanh)");
    benchmarkInterleaved<NewtonZdf<TanhSat, 1>>("Newton ZDF, 1 step (tanh)");
    benchmarkInterleaved<NewtonZdf<TanhSat>>("Newton ZDF (tanh)");
    benchmarkInterleaved<NewtonZdf<CubicSat>>("Newton ZDF (cubic)");

    benchmarkModulated("Linear ZDF", LinearZdf{});
    benchmarkModulated("Newton ZDF (tanh)", NewtonZdf<TanhSat>{});
    benchmarkModulated("Linearised ZDF (tanh)", LinearisedZdf<TanhSat>{});
    benchmarkModulated("Huovilainen exact, 2x", Oversampled<HuovExact>{OversamplingFactor::two});

    double hz = 1000.0;
    BENCHMARK("filter: prewarpedGain() (std::tan), one call")
    {
        hz += 0.01;
        return prewarpedGain(hz, rate);
    };
    HuovExact huovilainen;
    huovilainen.prepare(2.0 * rate);
    BENCHMARK("filter: Huovilainen exact setCutoff(), one call")
    {
        hz += 0.01;
        huovilainen.setCutoff(hz);
        return huovilainen.thresholdScale();
    };
}
