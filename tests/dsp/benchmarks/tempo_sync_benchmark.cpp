// CPU cost of tempo sync as built (docs/dsp/tempo-sync.md): the
// conversion alone, and a synced LFO as a voice drives it (the host's tempo
// into HostTempo, notePhaseIncrement() into Lfo::setPhaseIncrement(), then
// process()) for 512 samples, in one block and in 16 blocks of 32; and a
// 16-voice synth's 32 synced LFOs. The candidate models it was chosen from
// are benchmarked at commit d181628. Tagged [!benchmark][sync]: run with
// tools/bench.sh "[sync]" in relwithdebinfo. Results: docs/dsp/tempo-sync.md,
// docs/performance/baselines.md

#include <anoesis/dsp/control/host_tempo.hpp>
#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/control/note_value.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using anoesis::dsp::HostTempo;
using anoesis::dsp::Lfo;
using anoesis::dsp::LfoWaveform;
using anoesis::dsp::notePhaseIncrement;
using anoesis::dsp::NoteValue;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;
using Block = std::array<float, blockSize>;

Lfo prepared(LfoWaveform waveform, std::uint64_t seed = 1)
{
    Lfo lfo{seed};
    lfo.setWaveform(waveform);
    lfo.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
    return lfo;
}

} // namespace

TEST_CASE("Tempo sync: CPU", "[!benchmark][sync]")
{
    double bpm = 120.0;
    BENCHMARK("sync: notePhaseIncrement(), one call")
    {
        bpm += 0.001;
        return notePhaseIncrement(NoteValue::triplet(1, 16), bpm, 48000.0);
    };

    const NoteValue value = NoteValue::dotted(1, 8);
    for (const auto& [waveform, name] : {std::pair{LfoWaveform::sine, "sine"}, std::pair{LfoWaveform::saw, "saw"}})
    {
        Block out{};
        auto lfo = prepared(waveform);
        HostTempo tempo;
        double hostBpm = 128.0;
        BENCHMARK(std::string{"sync: "} + name + " 1/8D, blocks of 512, x512")
        {
            hostBpm = hostBpm > 129.0 ? 128.0 : hostBpm + 0.01;
            tempo.report(hostBpm);
            lfo.setPhaseIncrement(notePhaseIncrement(value, tempo.bpm(), 48000.0));
            lfo.process(out);
            return out[blockSize - 1];
        };
        BENCHMARK(std::string{"sync: "} + name + " 1/8D, blocks of 32, x512")
        {
            for (std::size_t b = 0; b < blockSize; b += 32)
            {
                hostBpm = hostBpm > 129.0 ? 128.0 : hostBpm + 0.01;
                tempo.report(hostBpm);
                lfo.setPhaseIncrement(notePhaseIncrement(value, tempo.bpm(), 48000.0));
                lfo.process(std::span{out}.subspan(b, 32));
            }
            return out[blockSize - 1];
        };
    }

    // A 16-voice synth's 32 synced LFOs (a sine and a saw per voice): one
    // increment per slot per block, shared by the voices.
    std::vector<Lfo> lfos;
    lfos.reserve(32);
    for (std::uint64_t i = 0; i < 32; ++i)
    {
        lfos.push_back(prepared(i % 2 == 0 ? LfoWaveform::sine : LfoWaveform::saw, i + 1));
    }
    Block out{};
    HostTempo tempo;
    double hostBpm = 128.0;
    BENCHMARK("sync: 32 synced LFOs (16 voices x 2), blocks of 512, x512")
    {
        hostBpm = hostBpm > 129.0 ? 128.0 : hostBpm + 0.01;
        tempo.report(hostBpm);
        const std::uint64_t slow = notePhaseIncrement(NoteValue::straight(1, 2), tempo.bpm(), 48000.0);
        const std::uint64_t fast = notePhaseIncrement(NoteValue::triplet(1, 16), tempo.bpm(), 48000.0);
        for (std::size_t i = 0; i < lfos.size(); ++i)
        {
            lfos[i].setPhaseIncrement(i % 2 == 0 ? slow : fast);
            lfos[i].process(out);
        }
        return out[blockSize - 1];
    };
}
