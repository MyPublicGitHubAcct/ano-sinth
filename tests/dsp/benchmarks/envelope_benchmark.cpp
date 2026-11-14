// CPU cost of the ADSR envelope (Adsr, docs/dsp/envelopes.md): one
// 512-sample block in each stage, through process() (a run of a stage at a
// time) and through next() per sample, and a 16-voice synth's 64 envelopes.
// Tagged [!benchmark][envelope]: run with tools/bench.sh "[envelope]" in
// relwithdebinfo. The candidate models it was chosen from are benchmarked at
// commit c8c1ab1. Results: docs/dsp/envelopes.md,
// docs/performance/baselines.md

#include <anoesis/dsp/control/adsr.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string>

using anoesis::dsp::Adsr;
using anoesis::dsp::AdsrStage;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;
using Block = std::array<float, blockSize>;

// Times long enough that a block never leaves its stage; sustain 0.5.
Adsr preparedIn(AdsrStage stage)
{
    Adsr adsr;
    adsr.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
    adsr.setAttack(stage == AdsrStage::attack ? 50.0f : 0.0f);
    adsr.setDecay(stage == AdsrStage::decay ? 50.0f : 0.0f);
    adsr.setSustain(0.5f);
    adsr.setRelease(50.0f);
    adsr.gateOn();
    if (stage == AdsrStage::release)
    {
        adsr.gateOff();
    }
    return adsr;
}

void benchmarkStage(AdsrStage stage, const std::string& name)
{
    Block out{};
    auto blocks = preparedIn(stage);
    BENCHMARK("Envelope: " + name + ", process() x512")
    {
        if (blocks.stage() != stage)
        {
            blocks = preparedIn(stage);
        }
        blocks.process(out);
        return out[blockSize - 1];
    };

    auto samples = preparedIn(stage);
    BENCHMARK("Envelope: " + name + ", next() per sample x512")
    {
        if (samples.stage() != stage)
        {
            samples = preparedIn(stage);
        }
        for (auto& y : out)
        {
            y = samples.next();
        }
        return out[blockSize - 1];
    };
}

} // namespace

TEST_CASE("Envelope: CPU", "[!benchmark][envelope]")
{
    benchmarkStage(AdsrStage::attack, "attack");
    benchmarkStage(AdsrStage::decay, "decay");
    benchmarkStage(AdsrStage::sustain, "sustain");
    benchmarkStage(AdsrStage::release, "release");

    // A 16-voice synth: two amp and two mod envelopes per voice, in mixed
    // stages, one block each.
    // Each is put back in its stage if a long run of the benchmark ends it.
    std::array<Adsr, 64> envelopes{};
    std::array<AdsrStage, 64> stages{};
    for (std::size_t i = 0; i < envelopes.size(); ++i)
    {
        stages[i] = static_cast<AdsrStage>(1 + (i % 4));
        envelopes[i] = preparedIn(stages[i]);
    }
    Block out{};
    BENCHMARK("Envelope: 64 envelopes (16 voices x 4), a quarter in each stage, process() x512")
    {
        float sum = 0.0f;
        for (std::size_t i = 0; i < envelopes.size(); ++i)
        {
            if (envelopes[i].stage() != stages[i])
            {
                envelopes[i] = preparedIn(stages[i]);
            }
            envelopes[i].process(out);
            sum += out[blockSize - 1];
        }
        return sum;
    };
}
