// CPU cost of the candidate ADSR segment models (envelope_candidates.hpp,
// docs/dsp/envelopes.md): one 512-sample block in each stage. Tagged
// [!benchmark][envelope]: run with tools/bench.sh "[envelope]" in
// relwithdebinfo. Results: docs/dsp/envelopes.md

#include "../control/envelope_candidates.hpp"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string>

using namespace anoesis::test::envelope;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;

// One block in each stage. Times are long enough that a block never leaves
// its stage; sustain 0.5.
template <typename Model>
void benchmarkModel(const std::string& name, Ratios ratios)
{
    std::array<float, blockSize> out{};
    const auto prepared = [&]
    {
        Model m{ratios};
        m.prepare(rate);
        m.setAttack(10.0f);
        m.setDecay(10.0f);
        m.setSustain(0.5f);
        m.setRelease(10.0f);
        return m;
    };
    const auto run = [&](Model& m)
    {
        for (auto& y : out)
        {
            y = m.next();
        }
        return out[blockSize - 1];
    };

    auto attack = prepared();
    attack.gateOn();
    BENCHMARK("Envelope: " + name + ", attack x512")
    {
        if (attack.stage() != Stage::attack)
        {
            attack.reset();
            attack.gateOn();
        }
        return run(attack);
    };

    auto decay = prepared();
    decay.setAttack(0.0f);
    decay.gateOn();
    BENCHMARK("Envelope: " + name + ", decay x512")
    {
        if (decay.stage() != Stage::decay)
        {
            decay.reset();
            decay.gateOn();
        }
        return run(decay);
    };

    auto sustain = prepared();
    sustain.setAttack(0.0f);
    sustain.setDecay(0.0f);
    sustain.gateOn();
    BENCHMARK("Envelope: " + name + ", sustain x512")
    {
        return run(sustain);
    };

    auto release = prepared();
    release.setAttack(0.0f);
    release.setDecay(0.0f);
    release.gateOn();
    release.gateOff();
    BENCHMARK("Envelope: " + name + ", release x512")
    {
        if (!release.isActive())
        {
            release.gateOn();
            release.gateOff();
        }
        return run(release);
    };
}

} // namespace

TEST_CASE("Envelope candidates: CPU", "[!benchmark][envelope]")
{
    benchmarkModel<LinearAccumulated>("linear, accumulated", Ratios{});
    benchmarkModel<LinearCounted>("linear, counted", Ratios{});
    benchmarkModel<ExponentialFloat>("exponential r 0.3 / 0.001", Ratios{.attack = 0.3, .decayRelease = 0.001});
    benchmarkModel<ExponentialDouble>("exponential r 0.3 / 0.001, double",
                                      Ratios{.attack = 0.3, .decayRelease = 0.001});
    benchmarkModel<ExponentialCounted>("exponential r 0.3 / 0.001, counted",
                                       Ratios{.attack = 0.3, .decayRelease = 0.001});
    benchmarkModel<AnalogRc>("analog RC", Ratios{});
    benchmarkModel<AnalogRcFlushed>("analog RC, state flushed", Ratios{});
}
