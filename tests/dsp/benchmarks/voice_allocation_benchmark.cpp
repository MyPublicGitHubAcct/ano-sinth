// CPU cost of the candidate voice allocators (M1.4 task 2, stage 1;
// tests/dsp/voice/allocation_candidates.hpp, docs/dsp/voice-allocation.md),
// 512 samples per run at 48 kHz, stereo:
//
// - the full instrument's voice cost: 16 dsp::Voices held at the sustain
//   through the allocator, at the init patch (A1 one saw, LP24 on their own
//   parts) and the worst case (4 x 8 unison saws, both filters on AB, drive
//   x4);
// - the allocator's own cost per note event: 64 note events per block on
//   voices that render nothing, every note-on a steal at Polyphony 16;
// - a steal every block on 16 voices of the init patch: a cut, a retrigger
//   in place, a 5 ms fade on a spare voice, a 5 ms fade before the new note.
//
// Tagged [!benchmark][voice-allocation]: run with
// tools/bench.sh "[voice-allocation]" in relwithdebinfo.

#include "../oscillators/oscillator_support.hpp"
#include "../voice/allocation_candidates.hpp"

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

using namespace anoesis::test::allocation;
using anoesis::test::Buffer;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;

void initPatch(BuiltVoice& v)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        v.slot(s).setWaveform(dsp::Waveform::saw);
        v.slot(s).setVoices(s == 0 ? 1 : 0);
    }
}

void worstPatch(BuiltVoice& v)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        auto& slot = v.slot(s);
        slot.setWaveform(dsp::Waveform::saw);
        slot.setVoices(8);
        slot.setDetune(0.548f);
        slot.setStereo(1.0f);
        slot.setRetrigger(false);
        slot.setTuning({.octaves = 0, .semitones = s < 2 ? 0 : 7, .fine = 0.0f});
    }
    for (std::size_t p = 0; p < 2; ++p)
    {
        auto& e = v.envelope(p);
        e.setAttack(0.005f);
        e.setDecay(0.3f);
        e.setSustain(0.7f);
        e.setRelease(0.2f);
    }
    std::array<anoesis::test::voice::FilterSettings, 2> filters{};
    for (auto& f : filters)
    {
        f.input = anoesis::test::voice::Input::both;
        f.drive = 4.0f;
    }
    v.setFilters(filters, true);
}

/// An allocator of V with 16 notes held, the global knobs at their defaults,
/// and the block's events (sample offsets within each run) repeated every run.
template <typename V>
class Bench
{
public:
    Bench(const Policy& policy, void (*configure)(V&), std::vector<Event> perBlock)
        : knobs(Knobs(), Knobs(), rate),
          allocator(std::make_unique<Allocator<V>>(
              Context{.table = &anoesis::test::d6::minBlepTable64(), .knobs = &knobs}, policy)),
          events(std::move(perBlock)),
          out(2, blockSize)
    {
        if (configure != nullptr)
        {
            allocator->forEachVoice(configure);
        }
        allocator->setPolyphony(maxPolyphony);
        allocator->prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        std::vector<Event> chord;
        chord.reserve(16);
        for (int k = 0; k < 16; ++k)
        {
            chord.push_back({.at = 0, .kind = EventKind::noteOn, .value = 40 + k});
        }
        process(chord);
        // Past the envelopes' attack and decay, so every run is at the sustain.
        for (int i = 0; i < 100; ++i)
        {
            process({});
        }
    }

    float run()
    {
        process(events);
        return out.channel(0)[0];
    }

private:
    void process(std::span<const Event> blockEvents)
    {
        // The events are at offsets within the block: move them to its start.
        positioned.clear();
        for (const auto& e : blockEvents)
        {
            positioned.push_back({.at = position + e.at, .kind = e.kind, .value = e.value});
        }
        out.block().clear();
        allocator->process(out.block(), positioned, position);
        knobs.advance(blockSize);
        position += blockSize;
    }

    GlobalKnobs knobs;
    std::unique_ptr<Allocator<V>> allocator;
    std::vector<Event> events;
    std::vector<Event> positioned;
    Buffer out;
    std::size_t position = 0;
};

/// 32 note-ons and 32 note-offs per block, every note-on a new key (a steal
/// at Polyphony 16).
std::vector<Event> eventStorm()
{
    std::vector<Event> e;
    for (std::size_t i = 0; i < 32; ++i)
    {
        const int key = 60 + static_cast<int>(i % 32);
        e.push_back({.at = i * 16, .kind = EventKind::noteOn, .value = key});
        e.push_back({.at = (i * 16) + 8, .kind = EventKind::noteOff, .value = key});
    }
    return e;
}

/// One note-on per block (the same key): at Polyphony 16, each one steals
/// the oldest voice.
std::vector<Event> oneSteal()
{
    return {{.at = 101, .kind = EventKind::noteOn, .value = 80}};
}

Policy withEnd(StealEnd end)
{
    Policy p;
    p.steal = Steal::oldest;
    p.end = end;
    p.fadeSeconds = 0.005f;
    return p;
}

} // namespace

TEST_CASE("Voice allocation: CPU", "[!benchmark][voice-allocation]")
{
    Bench<BuiltVoice> init{Policy(), initPatch, {}};
    BENCHMARK("allocation: 16 voices held, init patch x512")
    {
        return init.run();
    };
    Bench<BuiltVoice> worst{Policy(), worstPatch, {}};
    BENCHMARK("allocation: 16 voices held, worst case x512")
    {
        return worst.run();
    };

    Bench<StubVoice> storm{withEnd(StealEnd::retrigger), nullptr, eventStorm()};
    BENCHMARK("allocation: 64 note events (32 steals) on silent voices x512")
    {
        return storm.run();
    };
    Bench<StubVoice> quiet{withEnd(StealEnd::retrigger), nullptr, {}};
    BENCHMARK("allocation: no events on silent voices x512")
    {
        return quiet.run();
    };

    Bench<BuiltVoice> cut{withEnd(StealEnd::cut), initPatch, oneSteal()};
    BENCHMARK("allocation: a steal per block, cut, init patch x512")
    {
        return cut.run();
    };
    Bench<BuiltVoice> retrigger{withEnd(StealEnd::retrigger), initPatch, oneSteal()};
    BENCHMARK("allocation: a steal per block, retrigger, init patch x512")
    {
        return retrigger.run();
    };
    Bench<BuiltVoice> spare{withEnd(StealEnd::fadeOnSpare), initPatch, oneSteal()};
    BENCHMARK("allocation: a steal per block, 5 ms fade on a spare, init patch x512")
    {
        return spare.run();
    };
    Bench<BuiltVoice> delayed{withEnd(StealEnd::fadeThenStart), initPatch, oneSteal()};
    BENCHMARK("allocation: a steal per block, 5 ms fade then the note, init patch x512")
    {
        return delayed.run();
    };
}
