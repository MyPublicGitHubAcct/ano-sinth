// CPU cost of the candidate LFO models (lfo_candidates.hpp,
// docs/dsp/lfos.md): one LFO for a 512-sample block per row, and a 16-voice
// synth's 32 LFOs (two per voice), per model. Tagged [!benchmark][lfo]: run
// with tools/bench.sh "[lfo]" in relwithdebinfo. Results: docs/dsp/lfos.md,
// docs/performance/baselines.md

#include "../control/lfo_candidates.hpp"
#include "../oscillators/oscillator_support.hpp"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

using namespace anoesis::test::lfo;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;
using Block = std::array<float, blockSize>;

template <typename L>
L prepared(L lfo, Wave wave, float hz, std::uint64_t seed = 1)
{
    if constexpr (requires { lfo.setSeed(seed); })
    {
        lfo.setSeed(seed);
    }
    lfo.setWave(wave);
    lfo.setFrequency(hz);
    lfo.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
    return lfo;
}

template <typename L>
void benchmarkModel(const std::string& name, const L& model, std::initializer_list<Wave> waves)
{
    Block out{};
    for (const auto wave : waves)
    {
        auto lfo = prepared(model, wave, 3.0f);
        BENCHMARK("LFO: " + name + ", " + shapeOf(wave).name + " 3 Hz, x512")
        {
            lfo.process(out);
            return out[blockSize - 1];
        };
    }

    // 16 voices x 2 LFOs, a sine and a saw, at different rates.
    std::vector<L> voices;
    voices.reserve(32);
    for (std::size_t v = 0; v < 32; ++v)
    {
        voices.push_back(
            prepared(model, v % 2 == 0 ? Wave::sine : Wave::saw, 0.5f + (0.37f * static_cast<float>(v)), v + 1));
    }
    BENCHMARK("LFO: " + name + ", 32 LFOs (16 voices x 2), sine and saw, x512")
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

} // namespace

TEST_CASE("LFO: CPU", "[!benchmark][lfo]")
{
    const auto& table = anoesis::test::d6::minBlepTable64();
    const auto all = {Wave::sine, Wave::saw, Wave::pulse, Wave::sampleHold, Wave::smooth, Wave::lorenz};
    benchmarkModel("naive, float phase", Lfo<PhaseKind::float32, Edges::naive>{}, all);
    benchmarkModel("naive, double phase", Lfo<PhaseKind::double64, Edges::naive>{}, {Wave::sine, Wave::saw});
    benchmarkModel("naive, fixed point", Lfo<PhaseKind::fixed32, Edges::naive>{}, all);
    benchmarkModel("naive, fixed 64", Lfo<PhaseKind::fixed64, Edges::naive>{}, {Wave::sine, Wave::saw});
    benchmarkModel("slew 1 ms", Lfo<PhaseKind::fixed32, Edges::slew>{}, {Wave::sine, Wave::saw, Wave::sampleHold});
    benchmarkModel("PolyBLEP", Lfo<PhaseKind::fixed32, Edges::polyBlep>{}, {Wave::sine, Wave::saw, Wave::sampleHold});
    benchmarkModel("minBLEP steps", Lfo<PhaseKind::fixed32, Edges::minBlep>{&table},
                   {Wave::sine, Wave::saw, Wave::sampleHold});
    benchmarkModel("oscillator (minBLEP 64)", OscillatorLfo{table}, {Wave::sine, Wave::saw, Wave::pulse});
    benchmarkModel("every 32, held", ControlRateLfo<32, false>{}, {Wave::sine, Wave::saw});
    benchmarkModel("every 32, linear", ControlRateLfo<32, true>{}, {Wave::sine, Wave::saw});
    benchmarkModel("every 64, linear", ControlRateLfo<64, true>{}, {Wave::sine, Wave::saw});
}
