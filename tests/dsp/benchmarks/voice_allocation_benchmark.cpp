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
// - the library VoiceAllocator (stage 2) over dsp::Voice: 16 voices held at
//   the init patch and the worst case, a steal per block, and its own cost
//   per note event on a voice that does nothing.
//
// Tagged [!benchmark][voice-allocation]: run with
// tools/bench.sh "[voice-allocation]" in relwithdebinfo.

#include "../oscillators/oscillator_support.hpp"
#include "../voice/allocation_candidates.hpp"

#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
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

// --- The library allocator ----------------------------------------------------------------

/// A voice that does nothing: active from its note-on until stopped, for
/// the allocator's own cost.
class NullVoice
{
public:
    using Knobs = dsp::VoiceKnobs;

    explicit NullVoice(std::uint64_t /*seed*/) noexcept {}
    void prepare(const dsp::ProcessSpec& /*spec*/) noexcept {}
    void reset() noexcept
    {
        active = false;
    }
    void noteOn(float /*note*/, const Knobs& /*knobs*/, const dsp::NoteStart& /*start*/) noexcept
    {
        active = true;
    }
    void legatoNoteOn(float /*note*/, const Knobs& /*knobs*/, const dsp::NoteStart& /*start*/) noexcept
    {
        active = true;
    }
    [[nodiscard]] static float currentPitch() noexcept
    {
        return 69.0f;
    }
    [[nodiscard]] static bool hasPlayed() noexcept
    {
        return true;
    }
    [[nodiscard]] static float velocity() noexcept
    {
        return 1.0f;
    }
    void noteOff() noexcept {}
    void stop() noexcept
    {
        active = false;
    }
    void process(const dsp::AudioBlock& /*out*/, const Knobs& /*knobs*/) noexcept {}
    [[nodiscard]] bool isActive() const noexcept
    {
        return active;
    }
    [[nodiscard]] std::size_t activeOscillatorVoices() const noexcept
    {
        return active ? 1 : 0;
    }
    [[nodiscard]] static std::size_t oscillatorVoicesPerNote() noexcept
    {
        return 1;
    }

private:
    bool active = false;
};

/// The library allocator with 16 notes held, as Bench.
template <typename V>
class LibraryBench
{
public:
    template <typename... Args>
    LibraryBench(void (*configure)(V&), std::vector<dsp::VoiceEvent> perBlock, const Args&... args)
        : rampsA(Knobs(), rate),
          rampsB(Knobs(), rate),
          allocator(std::make_unique<dsp::VoiceAllocator<V>>(0, args...)),
          events(std::move(perBlock)),
          out(2, blockSize)
    {
        if (configure != nullptr)
        {
            allocator->forEachVoice(configure);
        }
        allocator->prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        std::vector<dsp::VoiceEvent> chord;
        chord.reserve(16);
        for (int k = 0; k < 16; ++k)
        {
            chord.push_back({.offset = 0, .kind = dsp::VoiceEventKind::noteOn, .value = 40 + k});
        }
        process(chord);
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
    [[nodiscard]] V::Knobs knobs() const
    {
        return {
            .cutoff = {&rampsA.cutoff, &rampsB.cutoff},
            .resonance = {&rampsA.resonance, &rampsB.resonance},
            .controlCutoff = &rampsA.controlCutoff,
            .controlResonance = &rampsA.controlResonance,
        };
    }

    void process(std::span<const dsp::VoiceEvent> blockEvents)
    {
        out.block().clear();
        allocator->process(out.block(), blockEvents, knobs());
        rampsA.advance(blockSize);
        rampsB.advance(blockSize);
    }

    KnobRamps rampsA;
    KnobRamps rampsB;
    std::unique_ptr<dsp::VoiceAllocator<V>> allocator;
    std::vector<dsp::VoiceEvent> events;
    Buffer out;
};

using LibraryVoice = dsp::Voice<dsp::minBlepTaps>;

void libraryInit(LibraryVoice& v)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        v.slot(s).setWaveform(dsp::Waveform::saw);
        v.slot(s).setVoices(s == 0 ? 1 : 0);
    }
}

void libraryWorst(LibraryVoice& v)
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
        v.setFilterInput(p, dsp::FilterInput::both);
        v.setFilterDrive(p, 4.0f);
    }
}

std::vector<dsp::VoiceEvent> libraryStorm()
{
    std::vector<dsp::VoiceEvent> e;
    for (std::size_t i = 0; i < 32; ++i)
    {
        const int key = 60 + static_cast<int>(i);
        e.push_back({.offset = i * 16, .kind = dsp::VoiceEventKind::noteOn, .value = key});
        e.push_back({.offset = (i * 16) + 8, .kind = dsp::VoiceEventKind::noteOff, .value = key});
    }
    return e;
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

    LibraryBench<LibraryVoice> libInit{libraryInit, {}, anoesis::test::d6::minBlepTable64()};
    BENCHMARK("allocator (library): 16 voices held, init patch x512")
    {
        return libInit.run();
    };
    LibraryBench<LibraryVoice> libWorst{libraryWorst, {}, anoesis::test::d6::minBlepTable64()};
    BENCHMARK("allocator (library): 16 voices held, worst case x512")
    {
        return libWorst.run();
    };
    LibraryBench<LibraryVoice> libSteal{libraryInit,
                                        {{.offset = 101, .kind = dsp::VoiceEventKind::noteOn, .value = 80}},
                                        anoesis::test::d6::minBlepTable64()};
    BENCHMARK("allocator (library): a steal per block, init patch x512")
    {
        return libSteal.run();
    };
    LibraryBench<NullVoice> libStorm{nullptr, libraryStorm()};
    BENCHMARK("allocator (library): 64 note events (32 steals) on null voices x512")
    {
        return libStorm.run();
    };
    LibraryBench<NullVoice> libQuiet{nullptr, {}};
    BENCHMARK("allocator (library): no events on null voices x512")
    {
        return libQuiet.run();
    };
}
