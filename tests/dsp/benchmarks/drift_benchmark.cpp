// CPU cost of oscillator drift (PitchDrift, docs/dsp/oscillator-drift.md):
// the generator alone, and applied to a voice's oscillator slots, either
// held per 64 samples (a note per run, the default) or added to a note per
// sample (for a voice that passes one anyway). Tagged [!benchmark][drift]:
// run with tools/bench.sh "[drift]" in relwithdebinfo. Each benchmark
// renders one 512-sample block. The six candidate models are benchmarked at
// commit 264f362. Results: docs/dsp/oscillator-drift.md,
// docs/performance/baselines.md

#include "../oscillators/oscillator_support.hpp"

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/pitch_drift.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string>

using namespace anoesis::test::d6;
using anoesis::dsp::PitchDrift;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;
using Block = std::array<float, blockSize>;
using Slot = anoesis::dsp::OscillatorSlot<anoesis::dsp::minBlepTaps>;

struct Bus
{
    Block left{};
    Block right{};
    std::array<float*, 2> pointers{left.data(), right.data()};

    [[nodiscard]] anoesis::dsp::AudioBlock block() noexcept
    {
        return {pointers.data(), 2, blockSize};
    }
};

PitchDrift preparedDrift(float cents)
{
    PitchDrift drift{1};
    drift.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
    drift.setDepthCents(cents);
    drift.noteOn();
    return drift;
}

Slot unisonSlot(int voices)
{
    Slot slot{minBlepTable64()};
    slot.setVoices(voices);
    slot.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
    slot.setWaveform(Waveform::saw);
    slot.setDetune(0.2f);
    slot.setStereo(0.5f);
    slot.setRetrigger(false);
    slot.noteOn();
    return slot;
}

// One voice's slots three ways: no drift (a note per block), drift held per
// 64 samples (a note per run), and drift added to a note per sample.
void benchmarkApplied(std::size_t slotsPerVoice, int voices)
{
    const std::string label = std::to_string(slotsPerVoice) + (slotsPerVoice == 1 ? " slot x " : " slots x ") +
                              std::to_string(voices) + (voices == 1 ? " voice" : " voices");
    std::array<Slot, 4> slots{unisonSlot(voices), unisonSlot(voices), unisonSlot(voices), unisonSlot(voices)};
    Bus bus;
    const auto noteOf = [](std::size_t s)
    {
        return 45.0f + (12.0f * static_cast<float>(s));
    };
    BENCHMARK("Drift: " + label + ", no drift (a note per block) x512")
    {
        for (std::size_t s = 0; s < slotsPerVoice; ++s)
        {
            slots[s].process(bus.block(), noteOf(s));
        }
        return bus.left[blockSize - 1];
    };
    auto held = preparedDrift(PitchDrift::defaultDepthCents);
    BENCHMARK("Drift: " + label + ", held per 64 samples x512")
    {
        held.process(blockSize,
                     [&](std::size_t start, std::size_t length, float semitones)
                     {
                         const auto part = bus.block().subBlock(start, length);
                         for (std::size_t s = 0; s < slotsPerVoice; ++s)
                         {
                             slots[s].process(part, noteOf(s) + semitones);
                         }
                     });
        return bus.left[blockSize - 1];
    };
    auto perSample = preparedDrift(PitchDrift::defaultDepthCents);
    std::array<Block, 4> notes{};
    BENCHMARK("Drift: " + label + ", added to a note per sample x512")
    {
        for (std::size_t s = 0; s < slotsPerVoice; ++s)
        {
            notes[s].fill(noteOf(s));
        }
        // One drift per voice: the same offsets for every slot.
        perSample.addTo(notes[0]);
        for (std::size_t s = 1; s < slotsPerVoice; ++s)
        {
            for (std::size_t i = 0; i < blockSize; ++i)
            {
                notes[s][i] += notes[0][i] - noteOf(0);
            }
        }
        for (std::size_t s = 0; s < slotsPerVoice; ++s)
        {
            slots[s].process(bus.block(), notes[s]);
        }
        return bus.left[blockSize - 1];
    };
}

} // namespace

TEST_CASE("Benchmark: oscillator drift", "[!benchmark][dsp][oscillators][drift]")
{
    {
        auto drift = preparedDrift(PitchDrift::defaultDepthCents);
        BENCHMARK("Drift: the generator alone, process() x512")
        {
            float sum = 0.0f;
            drift.process(blockSize, [&](std::size_t, std::size_t, float semitones) { sum += semitones; });
            return sum;
        };
    }
    {
        auto drift = preparedDrift(PitchDrift::defaultDepthCents);
        Block notes{};
        BENCHMARK("Drift: the generator alone, addTo() a note per sample x512")
        {
            drift.addTo(notes);
            return notes[blockSize - 1];
        };
    }
    benchmarkApplied(1, 1);
    benchmarkApplied(1, 8);
    benchmarkApplied(4, 8);
}
