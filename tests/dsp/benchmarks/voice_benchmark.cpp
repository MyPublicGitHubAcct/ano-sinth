// CPU cost of the candidate voice structures (M1.4 task 1, stage 1;
// tests/dsp/voice/voice_candidates.hpp, docs/dsp/voice-architecture.md): one
// voice, its note held at the sustain, rendered 512 samples per run at
// 48 kHz, the filters fed through the Filter Control at the voice's grid
// points (the global ramps advanced every block):
//
// - the init patch (A1 one saw, both filters LP24 on their own parts, Warm
//   Drive on): as it stands (filter B running on part B's silence), with
//   filter B's input at "--" (task 5's saving), and on the mono path;
// - the worst case (4 oscillators x 8 unison saws, detuned, spread, both
//   filters on AB, drive x4, Warm Drive on), on each layout and schedule;
// - the worst case centred (Stereo 0), stereo against the mono path;
// - the init patch at 96 kHz (1024 samples per run: the voice's own cost at
//   2x, without a decimator).
//
// Tagged [!benchmark][voice]: run with tools/bench.sh "[voice]" in
// relwithdebinfo. The per-sample figures are per voice.

#include "../oscillators/oscillator_support.hpp"
#include "../voice/voice_candidates.hpp"

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <functional>
#include <string>

using namespace anoesis::test::voice;
using anoesis::test::Buffer;
using anoesis::test::filter_control::Knobs;

namespace
{

constexpr std::size_t blockSize = 512;
using Voice = PrototypeVoice<dsp::minBlepTaps>;

/// A voice with its note held, the global knob ramps, and an output bus.
class Bench
{
public:
    Bench(const Engine& engine, const std::function<void(Voice&)>& configure, float rate = 48000.0f,
          std::size_t samplesPerRun = blockSize)
        : voice(anoesis::test::d6::minBlepTable64(), readings(), engine),
          rampsA(knobs(), rate),
          rampsB(knobs(), rate),
          out(2, samplesPerRun)
    {
        configure(voice);
        voice.prepare({.sampleRate = rate, .maxBlockSize = samplesPerRun, .numChannels = 1});
        voice.noteOn(45.0f, feed(), 0);
        // Past the envelopes' attack and decay, so every run is at the sustain.
        for (int i = 0; i < 100; ++i)
        {
            run();
        }
    }

    float run()
    {
        out.block().clear();
        voice.render(out.block(), feed(), position);
        rampsA.advance(out.channel(0).size());
        rampsB.advance(out.channel(0).size());
        position += out.channel(0).size();
        return out.channel(0)[0];
    }

private:
    [[nodiscard]] static const Readings& readings()
    {
        static const Readings developer;
        return developer;
    }

    [[nodiscard]] static const Knobs& knobs()
    {
        static const Knobs defaults;
        return defaults;
    }

    [[nodiscard]] FilterFeed feed() const
    {
        return {
            .a = &rampsA,
            .b = &rampsB,
            .keytrack = 0.0f,
        };
    }

    Voice voice;
    KnobRamps rampsA;
    KnobRamps rampsB;
    Buffer out;
    std::size_t position = 0;
};

void initWithoutFilterB(Voice& v)
{
    setInitPatch(v);
    Patch patch;
    patch.filters[1].input = Input::none;
    v.setPatch(patch);
}

} // namespace

TEST_CASE("Voice: CPU", "[!benchmark][voice]")
{
    const auto init = [](Voice& v)
    {
        setInitPatch(v);
    };
    const auto worst = [](Voice& v)
    {
        setWorstPatch(v);
    };
    const auto worstCentred = [](Voice& v)
    {
        setWorstPatch(v, 0.0f);
    };
    const Engine perPartGrid{};
    const Engine perFilterGrid{.layout = Layout::perFilter};
    const Engine perPartBlock{.schedule = Schedule::block};
    const Engine perFilterBlock{.layout = Layout::perFilter, .schedule = Schedule::block};
    const Engine mono{.monoWhenCentred = true};

    {
        Bench b{perPartGrid, init};
        BENCHMARK("voice: init patch, per-part, grid, stereo (filter B on silence) x512")
        {
            return b.run();
        };
    }
    {
        Bench b{perPartGrid, initWithoutFilterB};
        BENCHMARK("voice: init patch, filter B at -- (not run) x512")
        {
            return b.run();
        };
    }
    {
        Bench b{mono, init};
        BENCHMARK("voice: init patch, mono path x512")
        {
            return b.run();
        };
    }
    {
        Bench b{perFilterGrid, init};
        BENCHMARK("voice: init patch, per-filter, grid x512")
        {
            return b.run();
        };
    }
    {
        Bench b{perPartBlock, init};
        BENCHMARK("voice: init patch, per-part, block x512")
        {
            return b.run();
        };
    }
    {
        Bench b{perPartGrid, worst};
        BENCHMARK("voice: worst case, per-part, grid x512")
        {
            return b.run();
        };
    }
    {
        Bench b{perFilterGrid, worst};
        BENCHMARK("voice: worst case, per-filter, grid x512")
        {
            return b.run();
        };
    }
    {
        Bench b{perPartBlock, worst};
        BENCHMARK("voice: worst case, per-part, block x512")
        {
            return b.run();
        };
    }
    {
        Bench b{perFilterBlock, worst};
        BENCHMARK("voice: worst case, per-filter, block x512")
        {
            return b.run();
        };
    }
    {
        Bench b{perPartGrid, worstCentred};
        BENCHMARK("voice: worst case centred, stereo path x512")
        {
            return b.run();
        };
    }
    {
        Bench b{mono, worstCentred};
        BENCHMARK("voice: worst case centred, mono path x512")
        {
            return b.run();
        };
    }
    {
        Bench b{perPartGrid, init, 96000.0f, 2 * blockSize};
        BENCHMARK("voice: init patch at 96 kHz, 1024 samples per 512 out x512")
        {
            return b.run();
        };
    }
}
