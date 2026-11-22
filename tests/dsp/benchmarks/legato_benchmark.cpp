// CPU cost of the legato and portamento candidates (M1.4 task 3, stage 1;
// tests/dsp/voice/legato_candidates.hpp, docs/dsp/legato-portamento.md), 512
// samples per run at 48 kHz, stereo:
//
// - one voice held: dsp::Voice against PitchedVoice at a constant pitch (the
//   pitch input's overhead), and PitchedVoice gliding (three octaves a
//   second, so the pitch changes every sample), at each pitch rate,
//   at the init patch (A1 one saw) and the worst case (4 x 8 unison saws,
//   both filters on AB, drive x4);
// - one voice with the wheel moving (a bend message every 96 samples);
// - the engine's cost per event: 64 legato note events per block in Mono
//   Legato, 64 pedal toggles, and 64 poly note events on voices with no
//   oscillators (each freed at the next event), against no events, and the
//   same poly events through VoiceAllocator<dsp::Voice>;
// - as built (stage 2): dsp::Voice at a constant pitch, gliding and with the
//   wheel moving, and VoiceAllocator's Mono Legato with legato notes and
//   pedal toggles, against the prototype's rows.
//
// Tagged [!benchmark][legato]: run with tools/bench.sh "[legato]" in
// relwithdebinfo.

#include "../oscillators/oscillator_support.hpp"
#include "../voice/legato_candidates.hpp"

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace anoesis::test::legato;
using anoesis::test::Buffer;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;

template <typename V>
void initPatch(V& v)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        v.slot(s).setWaveform(dsp::Waveform::saw);
        v.slot(s).setVoices(s == 0 ? 1 : 0);
    }
}

template <typename V>
void worstPatch(V& v)
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

template <typename V>
void silentPatch(V& v)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        v.slot(s).setVoices(0);
    }
}

struct Knobs
{
    Knobs()
    {
        cutoff.setCurrentAndTarget(5.0f);
        control.setCurrentAndTarget(5.0f);
    }
    [[nodiscard]] dsp::VoiceKnobs filter() const noexcept
    {
        return {
            .cutoff = {&cutoff, &cutoff},
            .resonance = {&zero, &zero},
            .controlCutoff = &control,
            .controlResonance = &zero,
        };
    }
    dsp::LinearSmoother cutoff;
    dsp::LinearSmoother control;
    dsp::LinearSmoother zero;
};

/// One voice held at its sustain: dsp::Voice, or PitchedVoice at a constant
/// pitch or gliding (a glide too long to end), with the wheel moving if
/// `bend` (a message every 96 samples).
template <typename V>
class VoiceBench
{
public:
    static constexpr bool library = std::is_same_v<V, dsp::Voice<>>;

    VoiceBench(void (*configure)(V&), PitchRate pitchRate, bool gliding, bool moveBend)
        : voice(std::make_unique<V>(anoesis::test::d6::minBlepTable64(), 1)),
          out(2, blockSize),
          bending(moveBend),
          glides(gliding)
    {
        configure(*voice);
        if constexpr (!library)
        {
            voice->setOptions({.rate = pitchRate});
        }
        voice->prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        libraryBend.prepare(rate);
        voice->noteOn(60.0f, knobsAt(0));
        restartGlide(0);
        for (int i = 0; i < 100; ++i)
        {
            (void)run();
        }
    }

    float run()
    {
        out.block().clear();
        if (glides && voice->currentPitch() > 95.0f)
        {
            restartGlide(position);
        }
        if (!bending)
        {
            voice->process(out.block(), knobsAt(position));
        }
        else
        {
            // A wheel message every 96 samples (+-1 semitone, a 240-sample
            // ramp), the voice rendered between.
            std::size_t from = 0;
            while (from < blockSize)
            {
                const auto to = std::min(blockSize, from + 96 - ((position + from) % 96));
                if ((position + from) % 96 == 0)
                {
                    const bool up = ((position + from) / 96) % 2 == 0;
                    if constexpr (library)
                    {
                        libraryBend.setValue(position + from, up ? 12288 : 4096);
                    }
                    else
                    {
                        bend.set(position + from, up ? 1.0f : -1.0f, 240);
                    }
                }
                voice->process(out.block().subBlock(from, to - from), knobsAt(position + from));
                from = to;
            }
        }
        position += blockSize;
        return out.channel(0)[0];
    }

private:
    /// Three octaves in 1 s (the pitch moves by more than a float's step
    /// every sample), restarted when it ends.
    void restartGlide(std::size_t at) noexcept
    {
        if (!glides)
        {
            return;
        }
        if constexpr (library)
        {
            voice->legatoNoteOn(96.0f, knobsAt(at), {.glide = true, .from = 60.0f, .glideSeconds = 1.0f});
        }
        else
        {
            voice->legatoNoteOn(96.0f, knobsAt(at), {.glide = true, .from = 60.0f, .law = {.seconds = 1.0f}}, false,
                                1.0f, VelocityLegato::atOnce);
        }
    }

    [[nodiscard]] V::Knobs knobsAt(std::size_t at) const noexcept
    {
        if constexpr (library)
        {
            auto k = knobs.filter();
            k.bend = &libraryBend;
            k.time = at;
            return k;
        }
        else
        {
            return {.filter = knobs.filter(), .bend = &bend, .at = at};
        }
    }

    Knobs knobs;
    BendRamp bend;
    dsp::PitchBend libraryBend;
    std::unique_ptr<V> voice;
    Buffer out;
    std::size_t position = 0;
    bool bending = false;
    bool glides = false;
};

/// The engine with a patch, a policy, a set-up, and the same events (offsets
/// within the block) every run.
class EngineBench
{
public:
    EngineBench(void (*configure)(Engine<16>::Voice&), const Policy& policy, const std::vector<Event>& setUp,
                std::vector<Event> perBlock)
        : engine(std::make_unique<Engine<16>>(1, anoesis::test::d6::minBlepTable64())),
          events(std::move(perBlock)),
          out(2, blockSize)
    {
        engine->forEachVoice(configure);
        engine->setPolicy(policy);
        engine->prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        process(setUp);
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
        out.block().clear();
        engine->process(out.block(), blockEvents, {.filter = knobs.filter(), .bend = nullptr, .at = position});
        position += blockSize;
    }

    Knobs knobs;
    std::unique_ptr<Engine<16>> engine;
    std::vector<Event> events;
    Buffer out;
    std::size_t position = 0;
};

/// 32 legato steps up from a held key and 32 returns to it.
std::vector<Event> legatoStorm()
{
    std::vector<Event> e;
    for (std::size_t i = 0; i < 32; ++i)
    {
        const int key = 50 + static_cast<int>(i % 24);
        e.push_back({.offset = i * 16, .kind = EventKind::noteOn, .value = key});
        e.push_back({.offset = (i * 16) + 8, .kind = EventKind::noteOff, .value = key});
    }
    return e;
}

std::vector<Event> pedalStorm()
{
    std::vector<Event> e;
    e.reserve(64);
    for (std::size_t i = 0; i < 64; ++i)
    {
        e.push_back({.offset = i * 8, .kind = EventKind::pedal, .value = i % 2 == 0 ? 127 : 0});
    }
    return e;
}

std::vector<Event> polyStorm()
{
    std::vector<Event> e;
    for (std::size_t i = 0; i < 32; ++i)
    {
        const int key = 60 + static_cast<int>(i);
        e.push_back({.offset = i * 16, .kind = EventKind::noteOn, .value = key});
        e.push_back({.offset = (i * 16) + 8, .kind = EventKind::noteOff, .value = key});
    }
    return e;
}

/// The library allocator over dsp::Voice: a patch, Mono Legato or not, a
/// set-up, and the same events (offsets within the block) every run.
class LibraryBench
{
public:
    LibraryBench(void (*configure)(dsp::Voice<>&), bool monoLegato, float portamento, const std::vector<Event>& setUp,
                 const std::vector<Event>& perBlock)
        : allocator(std::make_unique<dsp::VoiceAllocator<dsp::Voice<>>>(1, anoesis::test::d6::minBlepTable64())),
          out(2, blockSize)
    {
        allocator->forEachVoice(configure);
        allocator->setMonoLegato(monoLegato);
        allocator->setPortamento(portamento);
        allocator->prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        events = convert(perBlock);
        process(convert(setUp));
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
    static std::vector<dsp::VoiceEvent> convert(const std::vector<Event>& in)
    {
        std::vector<dsp::VoiceEvent> v;
        v.reserve(in.size());
        for (const auto& e : in)
        {
            auto kind = dsp::VoiceEventKind::noteOn;
            switch (e.kind)
            {
            case EventKind::noteOff:
                kind = dsp::VoiceEventKind::noteOff;
                break;
            case EventKind::pedal:
                kind = dsp::VoiceEventKind::pedal;
                break;
            case EventKind::bend:
                kind = dsp::VoiceEventKind::pitchBend;
                break;
            case EventKind::polyphony:
                kind = dsp::VoiceEventKind::polyphony;
                break;
            case EventKind::noteOn:
                break;
            }
            v.push_back({.offset = e.offset, .kind = kind, .value = e.value, .velocity = e.velocity});
        }
        return v;
    }
    void process(std::span<const dsp::VoiceEvent> blockEvents)
    {
        out.block().clear();
        allocator->process(out.block(), blockEvents, knobs.filter());
    }

    Knobs knobs;
    std::unique_ptr<dsp::VoiceAllocator<dsp::Voice<>>> allocator;
    std::vector<dsp::VoiceEvent> events;
    Buffer out;
};

Policy mono(float glideSeconds)
{
    Policy p;
    p.monoLegato = true;
    p.glide = {.seconds = glideSeconds};
    return p;
}

} // namespace

TEST_CASE("Legato and portamento: CPU", "[!benchmark][legato]")
{
    using Pitched = PitchedVoice<>;
    VoiceBench<dsp::Voice<>> libraryInit{initPatch<dsp::Voice<>>, PitchRate::gridStart, false, false};
    BENCHMARK("legato: dsp::Voice, init patch x512")
    {
        return libraryInit.run();
    };
    for (const auto& [r, name] : {
             std::pair{PitchRate::gridStart, "grid, held"},
             std::pair{PitchRate::gridRamped, "grid, ramped"},
             std::pair{PitchRate::perSample, "per sample"},
         })
    {
        VoiceBench<Pitched> constant{initPatch<Pitched>, r, false, false};
        BENCHMARK(std::string("legato: PitchedVoice, init, constant pitch, ") + name + " x512")
        {
            return constant.run();
        };
        VoiceBench<Pitched> gliding{initPatch<Pitched>, r, true, false};
        BENCHMARK(std::string("legato: PitchedVoice, init, gliding, ") + name + " x512")
        {
            return gliding.run();
        };
        VoiceBench<Pitched> bending{initPatch<Pitched>, r, false, true};
        BENCHMARK(std::string("legato: PitchedVoice, init, wheel every 96, ") + name + " x512")
        {
            return bending.run();
        };
    }
    VoiceBench<dsp::Voice<>> libraryWorst{worstPatch<dsp::Voice<>>, PitchRate::gridStart, false, false};
    BENCHMARK("legato: dsp::Voice, worst case x512")
    {
        return libraryWorst.run();
    };
    for (const auto& [r, name] : {
             std::pair{PitchRate::gridStart, "grid, held"},
             std::pair{PitchRate::gridRamped, "grid, ramped"},
             std::pair{PitchRate::perSample, "per sample"},
         })
    {
        VoiceBench<Pitched> gliding{worstPatch<Pitched>, r, true, false};
        BENCHMARK(std::string("legato: PitchedVoice, worst case, gliding, ") + name + " x512")
        {
            return gliding.run();
        };
    }

    const std::vector<Event> held{{.offset = 0, .kind = EventKind::noteOn, .value = 48}};
    EngineBench monoQuiet{initPatch<Pitched>, mono(0.0f), held, {}};
    BENCHMARK("legato: Mono Legato, one key held, no events, init x512")
    {
        return monoQuiet.run();
    };
    EngineBench monoStorm{initPatch<Pitched>, mono(0.0f), held, legatoStorm()};
    BENCHMARK("legato: Mono Legato, 64 legato note events, init x512")
    {
        return monoStorm.run();
    };
    EngineBench monoGlideStorm{initPatch<Pitched>, mono(0.03f), held, legatoStorm()};
    BENCHMARK("legato: Mono Legato, 64 legato note events with a glide, init x512")
    {
        return monoGlideStorm.run();
    };
    EngineBench pedals{initPatch<Pitched>, mono(0.0f), held, pedalStorm()};
    BENCHMARK("legato: Mono Legato, 64 pedal toggles, init x512")
    {
        return pedals.run();
    };
    Policy poly;
    poly.glide = {.seconds = 0.03f};
    EngineBench polyQuiet{silentPatch<Pitched>, poly, {}, {}};
    BENCHMARK("legato: poly, no events, no oscillators x512")
    {
        return polyQuiet.run();
    };
    EngineBench polyEvents{silentPatch<Pitched>, poly, {}, polyStorm()};
    BENCHMARK("legato: poly, 64 note events with glides, no oscillators x512")
    {
        return polyEvents.run();
    };
    LibraryBench libraryQuiet{silentPatch<dsp::Voice<>>, false, 0.03f, {}, {}};
    BENCHMARK("legato: VoiceAllocator<dsp::Voice>, no events, no oscillators x512")
    {
        return libraryQuiet.run();
    };
    LibraryBench libraryEvents{silentPatch<dsp::Voice<>>, false, 0.03f, {}, polyStorm()};
    BENCHMARK("legato: VoiceAllocator<dsp::Voice>, 64 note events with glides, no oscillators x512")
    {
        return libraryEvents.run();
    };

    // As built: dsp::Voice with its pitch input, and the allocator's modes.
    for (const auto& [gliding, bending, name] : {
             std::tuple{false, false, "constant pitch"},
             std::tuple{true, false, "gliding"},
             std::tuple{false, true, "wheel every 96"},
         })
    {
        VoiceBench<dsp::Voice<>> built{initPatch<dsp::Voice<>>, PitchRate::perSample, gliding, bending};
        BENCHMARK(std::string("legato (built): dsp::Voice, init, ") + name + " x512")
        {
            return built.run();
        };
    }
    VoiceBench<dsp::Voice<>> builtWorst{worstPatch<dsp::Voice<>>, PitchRate::perSample, true, false};
    BENCHMARK("legato (built): dsp::Voice, worst case, gliding x512")
    {
        return builtWorst.run();
    };
    LibraryBench monoLibraryQuiet{initPatch<dsp::Voice<>>, true, 0.0f, held, {}};
    BENCHMARK("legato (built): VoiceAllocator Mono Legato, one key held, no events, init x512")
    {
        return monoLibraryQuiet.run();
    };
    LibraryBench monoLibraryStorm{initPatch<dsp::Voice<>>, true, 0.0f, held, legatoStorm()};
    BENCHMARK("legato (built): VoiceAllocator Mono Legato, 64 legato note events, init x512")
    {
        return monoLibraryStorm.run();
    };
    LibraryBench monoLibraryGlides{initPatch<dsp::Voice<>>, true, 0.03f, held, legatoStorm()};
    BENCHMARK("legato (built): VoiceAllocator Mono Legato, 64 legato note events with a glide, init x512")
    {
        return monoLibraryGlides.run();
    };
    LibraryBench monoLibraryPedal{initPatch<dsp::Voice<>>, true, 0.0f, held, pedalStorm()};
    BENCHMARK("legato (built): VoiceAllocator Mono Legato, 64 pedal toggles, init x512")
    {
        return monoLibraryPedal.run();
    };
}
