// CPU cost of the oscillator drift candidates (drift_candidates.hpp), and of
// applying a drift to oscillator slots: a note per sample against a note held
// for 64 samples. Tagged [!benchmark][drift]: run with
// tools/bench.sh "[drift]" in relwithdebinfo. Each benchmark renders one
// 512-sample block. Results: docs/dsp/oscillator-drift.md,
// docs/performance/baselines.md

#include "../oscillators/drift_candidates.hpp"
#include "../oscillators/oscillator_support.hpp"

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string>

using namespace anoesis::test::d6;
using namespace anoesis::test::drift;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr std::size_t controlInterval = 64;
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

Settings settings(double updateHz)
{
    return {.sigmaCents = 1.5, .rateHz = 0.5, .updateHz = updateHz};
}

template <typename Model>
void benchmarkModel()
{
    Block out{};
    {
        Model model{settings(rate), 1};
        BENCHMARK(std::string{"Drift: "} + Model::name + ", every sample x512")
        {
            for (auto& x : out)
            {
                x = model.next();
            }
            return out[blockSize - 1];
        };
    }
    {
        ControlRate<Model> model{Model{settings(rate / controlInterval), 1}, controlInterval, Interpolation::linear};
        BENCHMARK(std::string{"Drift: "} + Model::name + ", control rate (64), linear x512")
        {
            for (auto& x : out)
            {
                x = model.next();
            }
            return out[blockSize - 1];
        };
    }
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

// One voice's slots, rendered three ways: with no drift (a note per block),
// with a drift per sample (a note per sample), and with the drift held for
// each 64 samples (the block split into 64-sample parts, each at one note).
void benchmarkApplication(std::size_t slotsPerVoice, int voices)
{
    const std::string label = std::to_string(slotsPerVoice) + (slotsPerVoice == 1 ? " slot x " : " slots x ") +
                              std::to_string(voices) + (voices == 1 ? " voice" : " voices");
    std::array<Slot, 4> slots{unisonSlot(voices), unisonSlot(voices), unisonSlot(voices), unisonSlot(voices)};
    Bus bus;
    const auto noteOf = [](std::size_t s)
    {
        return 45.0f + (12.0f * static_cast<float>(s));
    };
    BENCHMARK("Drift applied: " + label + ", no drift (note per block) x512")
    {
        for (std::size_t s = 0; s < slotsPerVoice; ++s)
        {
            slots[s].process(bus.block(), noteOf(s));
        }
        return bus.left[blockSize - 1];
    };
    ControlRate<TwoPole> drift{TwoPole{settings(rate / controlInterval), 1}, controlInterval, Interpolation::linear};
    Block offsets{};
    Block notes{};
    BENCHMARK("Drift applied: " + label + ", two-pole at 64, linear, note per sample x512")
    {
        for (auto& x : offsets)
        {
            x = drift.next() * 0.01f;
        }
        for (std::size_t s = 0; s < slotsPerVoice; ++s)
        {
            for (std::size_t i = 0; i < blockSize; ++i)
            {
                notes[i] = noteOf(s) + offsets[i];
            }
            slots[s].process(bus.block(), notes);
        }
        return bus.left[blockSize - 1];
    };
    TwoPole held{settings(rate / controlInterval), 1};
    BENCHMARK("Drift applied: " + label + ", two-pole at 64, held, note per 64 samples x512")
    {
        for (std::size_t start = 0; start < blockSize; start += controlInterval)
        {
            const float offset = held.next() * 0.01f;
            const auto part = bus.block().subBlock(start, controlInterval);
            for (std::size_t s = 0; s < slotsPerVoice; ++s)
            {
                slots[s].process(part, noteOf(s) + offset);
            }
        }
        return bus.left[blockSize - 1];
    };
}

} // namespace

TEST_CASE("Benchmark: oscillator drift candidates", "[!benchmark][dsp][oscillators][drift]")
{
    benchmarkModel<OnePole>();
    benchmarkModel<TwoPole>();
    benchmarkModel<SampleHold<false>>();
    benchmarkModel<SampleHold<true>>();
    benchmarkModel<SumOfSines>();
    benchmarkModel<Pink<8>>();
    benchmarkApplication(1, 1);
    benchmarkApplication(1, 8);
    benchmarkApplication(4, 8);
}
