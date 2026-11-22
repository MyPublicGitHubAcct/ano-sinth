// CPU cost of the part-solo and skipping candidates (M1.4 task 5, stage 1;
// tests/dsp/voice/solo_candidates.hpp, docs/dsp/part-solo-and-skipping.md),
// 512 samples per run at 48 kHz, stereo. 16 voices held (keys 40-55)
// through the real VoiceAllocator, for each case (V12, V13): the init patch,
// filter B on "--", Mix B at 0, B soloed out, every oscillator at Voices 0,
// the worst case centred (the mono path) and spread (nothing to skip), and
// two parts sounding centred. Each as built (dsp::Voice), as the prototype
// with nothing skipped, and with each candidate.
//
// Tagged [!benchmark][solo]: run with tools/bench.sh "[solo]" in
// relwithdebinfo.

#include "../oscillators/oscillator_support.hpp"
#include "../voice/solo_candidates.hpp"

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace anoesis::test::solo;
using anoesis::test::Buffer;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;

enum class Shape : std::uint8_t
{
    init,         ///< A1 a saw; B at Voices 0, filter B on B
    initNone,     ///< the same, filter B on "--"
    twoParts,     ///< A1 and B1 saws, own filters, centred
    silent,       ///< every oscillator at Voices 0
    worstCentred, ///< 4 x 8 unison saws, Stereo 0, own filters
    worstSpread,  ///< the same at Stereo 1
};

template <typename V>
void configure(V& v, Shape shape)
{
    const bool worst = shape == Shape::worstCentred || shape == Shape::worstSpread;
    for (std::size_t s = 0; s < 4; ++s)
    {
        auto& slot = v.slot(s);
        slot.setWaveform(dsp::Waveform::saw);
        int voices = 0;
        switch (shape)
        {
        case Shape::init:
        case Shape::initNone:
            voices = s == 0 ? 1 : 0;
            break;
        case Shape::twoParts:
            voices = s == 0 || s == 2 ? 1 : 0;
            break;
        case Shape::silent:
            voices = 0;
            break;
        case Shape::worstCentred:
        case Shape::worstSpread:
            voices = 8;
            break;
        }
        slot.setVoices(voices);
        if (worst)
        {
            slot.setDetune(0.548f);
            slot.setRetrigger(false);
            slot.setStereo(shape == Shape::worstSpread ? 1.0f : 0.0f);
        }
    }
    v.setFilterInput(1, shape == Shape::initNone ? dsp::FilterInput::none : dsp::FilterInput::own);
}

std::vector<dsp::VoiceEvent> chord()
{
    std::vector<dsp::VoiceEvent> c;
    c.reserve(16);
    for (int k = 0; k < 16; ++k)
    {
        c.push_back({.offset = 0, .kind = dsp::VoiceEventKind::noteOn, .value = 40 + k});
    }
    return c;
}

/// 16 voices of dsp::Voice, with Mix ramps.
class LibraryChord
{
public:
    LibraryChord(Shape shape, float mixB)
        : allocator(std::make_unique<dsp::VoiceAllocator<dsp::Voice<>>>(1, anoesis::test::d6::minBlepTable64())),
          out(2, blockSize)
    {
        for (auto& r : ramps)
        {
            r.setRampTime(0.01f);
            r.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        }
        ramps[0].setCurrentAndTarget(10.0f);
        ramps[1].setCurrentAndTarget(mixB);
        allocator->forEachVoice([&](dsp::Voice<>& v) { configure(v, shape); });
        allocator->prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        const auto notes = chord();
        out.block().clear();
        allocator->process(out.block(), notes, knobs());
        for (int i = 0; i < 100; ++i)
        {
            run();
        }
    }
    float run()
    {
        out.block().clear();
        allocator->process(out.block(), {}, knobs());
        return out.channel(0)[0];
    }

private:
    [[nodiscard]] dsp::VoiceKnobs knobs() const noexcept
    {
        auto k = filters.knobs();
        k.mix = {ramps.data(), ramps.data() + 1};
        return k;
    }
    FilterKnobs filters;
    std::array<dsp::LinearSmoother, 2> ramps{};
    std::unique_ptr<dsp::VoiceAllocator<dsp::Voice<>>> allocator;
    Buffer out;
};

struct Candidate
{
    FilterSkip filter = FilterSkip::never;
    bool oscillators = false;
    bool mono = false;
    bool routing = false;
};

/// 16 SkipVoices through the engine.
class EngineChord
{
public:
    EngineChord(Shape shape, const Candidate& c, float mixB, bool soloA = false) : out(2, blockSize)
    {
        SkipContext context;
        context.filterSkip = c.filter;
        context.skipOscillators = c.oscillators;
        context.mono = c.mono;
        context.soloByRouting = c.routing;
        engine = std::make_unique<SoloEngine>(anoesis::test::d6::minBlepTable64(), context);
        engine->voices().forEachVoice([&](SoloEngine::Voice& v) { configure(v, shape); });
        engine->setMix(1, mixB);
        engine->prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        engine->setSolo(soloA, 0);
        std::vector<EngineEvent> notes;
        for (const auto& e : chord())
        {
            notes.push_back({.voice = e});
        }
        engine->process(out.block(), notes);
        for (int i = 0; i < 100; ++i)
        {
            run();
        }
    }
    float run()
    {
        engine->process(out.block(), {});
        return out.channel(0)[0];
    }

private:
    std::unique_ptr<SoloEngine> engine;
    Buffer out;
};

constexpr Candidate nothing{};
constexpr Candidate atRest{.filter = FilterSkip::atRest};
constexpr Candidate atRestSlots{.filter = FilterSkip::atRest, .oscillators = true};
constexpr Candidate atRestSlotsMono{.filter = FilterSkip::atRest, .oscillators = true, .mono = true};
constexpr Candidate freezeSlots{.filter = FilterSkip::freeze, .oscillators = true};
constexpr Candidate freezeSlotsMono{.filter = FilterSkip::freeze, .oscillators = true, .mono = true};
constexpr Candidate monoOnly{.mono = true};
constexpr Candidate routing{.routing = true};

} // namespace

TEST_CASE("Part solo and skipping: CPU, 16 voices", "[!benchmark][solo]")
{
    struct Row
    {
        std::string name;
        Shape shape;
        float mixB;
        bool soloA;
        bool library;
        Candidate candidate;
    };
    const std::vector<Row> rows{
        {
            .name = "init: dsp::Voice",
            .shape = Shape::init,
            .mixB = 10.0f,
            .soloA = false,
            .library = true,
        },
        {
            .name = "init: prototype, nothing skipped",
            .shape = Shape::init,
            .mixB = 10.0f,
            .soloA = false,
            .library = false,
            .candidate = nothing,
        },
        {
            .name = "init: atRest",
            .shape = Shape::init,
            .mixB = 10.0f,
            .soloA = false,
            .library = false,
            .candidate = atRest,
        },
        {
            .name = "init: atRest + slots + mono",
            .shape = Shape::init,
            .mixB = 10.0f,
            .soloA = false,
            .library = false,
            .candidate = atRestSlotsMono,
        },
        {
            .name = "init: mono only",
            .shape = Shape::init,
            .mixB = 10.0f,
            .soloA = false,
            .library = false,
            .candidate = monoOnly,
        },
        {
            .name = "filter B --: dsp::Voice",
            .shape = Shape::initNone,
            .mixB = 10.0f,
            .soloA = false,
            .library = true,
        },
        {
            .name = "filter B --: atRest + slots + mono",
            .shape = Shape::initNone,
            .mixB = 10.0f,
            .soloA = false,
            .library = false,
            .candidate = atRestSlotsMono,
        },
        {
            .name = "Mix B 0: dsp::Voice",
            .shape = Shape::twoParts,
            .mixB = 0.0f,
            .soloA = false,
            .library = true,
        },
        {
            .name = "Mix B 0: prototype, nothing skipped",
            .shape = Shape::twoParts,
            .mixB = 0.0f,
            .soloA = false,
            .library = false,
            .candidate = nothing,
        },
        {
            .name = "Mix B 0: atRest + slots",
            .shape = Shape::twoParts,
            .mixB = 0.0f,
            .soloA = false,
            .library = false,
            .candidate = atRestSlots,
        },
        {
            .name = "Mix B 0: freeze + slots",
            .shape = Shape::twoParts,
            .mixB = 0.0f,
            .soloA = false,
            .library = false,
            .candidate = freezeSlots,
        },
        {
            .name = "Mix B 0: freeze + slots + mono",
            .shape = Shape::twoParts,
            .mixB = 0.0f,
            .soloA = false,
            .library = false,
            .candidate = freezeSlotsMono,
        },
        {
            .name = "B soloed out: gain, nothing skipped",
            .shape = Shape::twoParts,
            .mixB = 10.0f,
            .soloA = true,
            .library = false,
            .candidate = nothing,
        },
        {
            .name = "B soloed out: gain, freeze + slots",
            .shape = Shape::twoParts,
            .mixB = 10.0f,
            .soloA = true,
            .library = false,
            .candidate = freezeSlots,
        },
        {
            .name = "B soloed out: routing",
            .shape = Shape::twoParts,
            .mixB = 10.0f,
            .soloA = true,
            .library = false,
            .candidate = routing,
        },
        {
            .name = "two parts sounding: dsp::Voice",
            .shape = Shape::twoParts,
            .mixB = 10.0f,
            .soloA = false,
            .library = true,
        },
        {
            .name = "two parts sounding: freeze + slots + mono",
            .shape = Shape::twoParts,
            .mixB = 10.0f,
            .soloA = false,
            .library = false,
            .candidate = freezeSlotsMono,
        },
        {
            .name = "Voices 0 everywhere: dsp::Voice",
            .shape = Shape::silent,
            .mixB = 10.0f,
            .soloA = false,
            .library = true,
        },
        {
            .name = "Voices 0 everywhere: atRest",
            .shape = Shape::silent,
            .mixB = 10.0f,
            .soloA = false,
            .library = false,
            .candidate = atRest,
        },
        {
            .name = "worst centred: dsp::Voice",
            .shape = Shape::worstCentred,
            .mixB = 10.0f,
            .soloA = false,
            .library = true,
        },
        {
            .name = "worst centred: mono only",
            .shape = Shape::worstCentred,
            .mixB = 10.0f,
            .soloA = false,
            .library = false,
            .candidate = monoOnly,
        },
        {
            .name = "worst centred: freeze + slots + mono",
            .shape = Shape::worstCentred,
            .mixB = 10.0f,
            .soloA = false,
            .library = false,
            .candidate = freezeSlotsMono,
        },
        {
            .name = "worst spread: dsp::Voice",
            .shape = Shape::worstSpread,
            .mixB = 10.0f,
            .soloA = false,
            .library = true,
        },
        {
            .name = "worst spread: prototype, nothing skipped",
            .shape = Shape::worstSpread,
            .mixB = 10.0f,
            .soloA = false,
            .library = false,
            .candidate = nothing,
        },
        {
            .name = "worst spread: freeze + slots + mono (nothing to skip)",
            .shape = Shape::worstSpread,
            .mixB = 10.0f,
            .soloA = false,
            .library = false,
            .candidate = freezeSlotsMono,
        },
    };
    for (const auto& r : rows)
    {
        if (r.library)
        {
            LibraryChord bench{r.shape, r.mixB};
            BENCHMARK("solo: 16 voices, " + r.name + " x512")
            {
                return bench.run();
            };
        }
        else
        {
            EngineChord bench{r.shape, r.candidate, r.mixB, r.soloA};
            BENCHMARK("solo: 16 voices, " + r.name + " x512")
            {
                return bench.run();
            };
        }
    }
}
