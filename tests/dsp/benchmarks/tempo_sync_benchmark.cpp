// CPU cost of the tempo-sync candidates (tempo_sync_candidates.hpp,
// docs/dsp/tempo-sync.md): a synced sine LFO for 512 samples, in one block
// of 512 and in 16 blocks of 32 (where a model's per-block work shows), with
// the transport playing; and the conversions alone. Tagged
// [!benchmark][sync]: run with tools/bench.sh "[sync]" in relwithdebinfo.

#include "../control/tempo_sync_candidates.hpp"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

using namespace anoesis::test::sync;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr double rate = 48000.0;
using Block = std::array<float, blockSize>;

template <typename Model>
void benchmarkModel(const char* name)
{
    Block out{};
    SyncedLfo<Model> lfo;
    lfo.prepare(rate);
    lfo.setNote(dotted(1, 8));
    lfo.noteOn();
    double ppq = 0.0;
    BENCHMARK(std::string{"sync: "} + name + ", blocks of 512, x512")
    {
        lfo.process(out, HostClock{.bpm = 128.0, .ppq = ppq, .playing = true});
        ppq += 512.0 * 128.0 / (60.0 * rate);
        return out[blockSize - 1];
    };
    BENCHMARK(std::string{"sync: "} + name + ", blocks of 32, x512")
    {
        for (std::size_t b = 0; b < blockSize; b += 32)
        {
            lfo.process(std::span{out}.subspan(b, 32), HostClock{.bpm = 128.0, .ppq = ppq, .playing = true});
            ppq += 32.0 * 128.0 / (60.0 * rate);
        }
        return out[blockSize - 1];
    };
}

} // namespace

TEST_CASE("Tempo sync: CPU", "[!benchmark][sync]")
{
    benchmarkModel<RateOnly>(RateOnly::name);
    benchmarkModel<RateOnlyFloatHz>(RateOnlyFloatHz::name);
    benchmarkModel<Locked>(Locked::name);
    benchmarkModel<Hybrid>(Hybrid::name);
    benchmarkModel<NoteAnchored>(NoteAnchored::name);

    double bpm = 120.0;
    BENCHMARK("sync: phaseIncrement(), one call")
    {
        bpm += 0.001;
        return phaseIncrement(triplet(1, 16), bpm, rate);
    };
    double ppq = 0.0;
    BENCHMARK("sync: gridPhase(), one call")
    {
        ppq += 0.0107;
        return gridPhase(ppq, triplet(1, 16));
    };
}
