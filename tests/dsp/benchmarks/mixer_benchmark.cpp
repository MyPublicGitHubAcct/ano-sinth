// CPU cost of the mixer candidates (M1.4 task 4, stage 1;
// tests/dsp/voice/mixer_candidates.hpp, docs/dsp/mixer.md), 512 samples per
// run at 48 kHz, stereo:
//
// - one voice held at the init patch (A1 one saw): dsp::Voice against
//   MixVoice with Mix per grid point (held, ramped), per sample, or on a part
//   bus; Mix still, its knob ramping all the time, or a free 40 Hz LFO on it;
// - 16 voices held through the real VoiceAllocator: dsp::Voice against the
//   engine per sample and on part buses, at the init patch and the worst case;
// - the global stages per block: Main Volume (ramping, settled), each meter
//   on both channels, and an output guard;
// - room for task 5 (V13): a voice with A1 and B1 at Mix B 10, at Mix B 0,
//   and with B1 at Voices 0;
// - as built (stage 2): dsp::Voice with its Mix ramps still and ramping, 16
//   voices through VoiceAllocator with Mix, MainVolume, StereoPeakMeter and
//   OutputGuard.
//
// Tagged [!benchmark][mixer]: run with tools/bench.sh "[mixer]" in
// relwithdebinfo.

#include "../oscillators/oscillator_support.hpp"
#include "../voice/mixer_candidates.hpp"

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/output/main_volume.hpp>
#include <anoesis/dsp/output/output_guard.hpp>
#include <anoesis/dsp/output/peak_meter.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace anoesis::test::mixer;
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
void twoPartPatch(V& v)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        v.slot(s).setWaveform(dsp::Waveform::saw);
        v.slot(s).setVoices(s == 0 || s == 2 ? 1 : 0);
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
    }
    for (std::size_t p = 0; p < 2; ++p)
    {
        v.setFilterInput(p, dsp::FilterInput::both);
        v.setFilterDrive(p, 4.0f);
    }
}

/// What the Mix knob does while a voice is measured.
enum class Motion : std::uint8_t
{
    still,
    ramping, ///< retargeted every block: moving 480 of every 512 samples
    lfo,     ///< still, with a free 40 Hz LFO of +-5 on it
};

/// One voice held at its sustain: dsp::Voice, or MixVoice in a placement.
template <typename V>
class VoiceBench
{
public:
    VoiceBench(void (*configure)(V&), MixPlacement placement, Motion m, float mixB = 10.0f)
        : out(4, blockSize), motion(m)
    {
        for (auto& r : ramps)
        {
            r.setRampTime(0.01f);
            r.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        }
        ramps[0].setCurrentAndTarget(7.5f);
        ramps[1].setCurrentAndTarget(mixB);
        context.mix = {ramps.data(), ramps.data() + 1};
        context.placement = placement;
        if (motion == Motion::lfo)
        {
            context.modulation = {.kind = MixModulation::lfo, .amount = 5.0f, .rateHz = 40.0f};
        }
        if constexpr (library)
        {
            voice = std::make_unique<V>(anoesis::test::d6::minBlepTable64(), 1);
        }
        else
        {
            voice = std::make_unique<V>(anoesis::test::d6::minBlepTable64(), &context, 1);
        }
        configure(*voice);
        voice->prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        voice->noteOn(45.0f, knobs.knobs());
        for (int i = 0; i < 100; ++i)
        {
            run();
        }
    }

    float run()
    {
        if (motion == Motion::ramping)
        {
            ramps[0].setTarget(up ? 15.0f : 5.0f);
            up = !up;
        }
        out.block().clear();
        voice->process(out.block(), withTime()); // 4 channels: a part bus uses them all, the others 0 and 1
        for (auto& r : ramps)
        {
            r.skip(blockSize);
        }
        position += blockSize;
        return out.channel(0)[0];
    }

private:
    static constexpr bool library = std::is_same_v<V, dsp::Voice<>>;

    [[nodiscard]] dsp::VoiceKnobs withTime() const noexcept
    {
        auto k = knobs.knobs();
        k.time = position;
        return k;
    }

    FilterKnobs knobs;
    std::array<dsp::LinearSmoother, 2> ramps{};
    MixContext context{};
    std::unique_ptr<V> voice;
    Buffer out;
    Motion motion;
    bool up = true;
    std::uint64_t position = 0;
};

/// 16 voices held: the library allocator over dsp::Voice, or the engine.
class LibraryChord
{
public:
    explicit LibraryChord(void (*configure)(dsp::Voice<>&))
        : allocator(std::make_unique<dsp::VoiceAllocator<dsp::Voice<>>>(1, anoesis::test::d6::minBlepTable64())),
          out(2, blockSize)
    {
        allocator->forEachVoice(configure);
        allocator->prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        std::vector<dsp::VoiceEvent> chord;
        chord.reserve(16);
        for (int k = 0; k < 16; ++k)
        {
            chord.push_back({.offset = 0, .kind = dsp::VoiceEventKind::noteOn, .value = 40 + k});
        }
        out.block().clear();
        allocator->process(out.block(), chord, knobs.knobs());
        for (int i = 0; i < 100; ++i)
        {
            run();
        }
    }
    float run()
    {
        out.block().clear();
        allocator->process(out.block(), {}, knobs.knobs());
        return out.channel(0)[0];
    }

private:
    FilterKnobs knobs;
    std::unique_ptr<dsp::VoiceAllocator<dsp::Voice<>>> allocator;
    Buffer out;
};

class EngineChord
{
public:
    EngineChord(void (*configure)(MixEngine::Voice&), MixPlacement placement)
        : engine(std::make_unique<MixEngine>(anoesis::test::d6::minBlepTable64(), contextFor(placement))),
          out(2, blockSize)
    {
        engine->voices().forEachVoice(configure);
        engine->setMix(0, 7.5f);
        engine->setMix(1, 7.5f);
        engine->prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        std::vector<EngineEvent> chord;
        chord.reserve(16);
        for (int k = 0; k < 16; ++k)
        {
            chord.push_back({.voice = {.offset = 0, .kind = dsp::VoiceEventKind::noteOn, .value = 40 + k}});
        }
        engine->process(out.block(), chord);
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
    static MixContext contextFor(MixPlacement placement)
    {
        MixContext c;
        c.placement = placement;
        return c;
    }
    std::unique_ptr<MixEngine> engine;
    Buffer out;
};

std::vector<float> noiseBlock()
{
    std::vector<float> x(blockSize);
    for (std::size_t i = 0; i < blockSize; ++i)
    {
        x[i] = 0.5f * static_cast<float>(std::sin(0.37 * static_cast<double>(i * i)));
    }
    return x;
}

/// As built: dsp::Voice held with its Mix ramps (VoiceKnobs::mix), still or
/// ramping all the time.
class LibraryMixBench
{
public:
    LibraryMixBench(void (*configure)(dsp::Voice<>&), bool ramping)
        : voice(std::make_unique<dsp::Voice<>>(anoesis::test::d6::minBlepTable64(), 1)),
          out(2, blockSize),
          moving(ramping)
    {
        for (auto& r : ramps)
        {
            r.setRampTime(0.01f);
            r.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
            r.setCurrentAndTarget(7.5f);
        }
        configure(*voice);
        voice->prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        voice->noteOn(45.0f, knobs());
        for (int i = 0; i < 100; ++i)
        {
            run();
        }
    }
    float run()
    {
        if (moving)
        {
            ramps[0].setTarget(up ? 15.0f : 5.0f);
            up = !up;
        }
        out.block().clear();
        voice->process(out.block(), knobs());
        for (auto& r : ramps)
        {
            r.skip(blockSize);
        }
        return out.channel(0)[0];
    }

private:
    [[nodiscard]] dsp::VoiceKnobs knobs() const noexcept
    {
        auto k = filterKnobs.knobs();
        k.mix = {ramps.data(), ramps.data() + 1};
        return k;
    }
    FilterKnobs filterKnobs;
    std::array<dsp::LinearSmoother, 2> ramps{};
    std::unique_ptr<dsp::Voice<>> voice;
    Buffer out;
    bool moving;
    bool up = true;
};

/// As built: 16 voices through VoiceAllocator with Mix, then MainVolume, the
/// meter and the guard, as M1.8 will chain them.
class LibraryChain
{
public:
    explicit LibraryChain(void (*configure)(dsp::Voice<>&))
        : allocator(std::make_unique<dsp::VoiceAllocator<dsp::Voice<>>>(1, anoesis::test::d6::minBlepTable64())),
          out(2, blockSize)
    {
        for (auto& r : ramps)
        {
            r.setRampTime(0.01f);
            r.setCurrentAndTarget(7.5f);
        }
        const dsp::ProcessSpec spec{.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2};
        for (auto& r : ramps)
        {
            r.prepare(spec);
        }
        volume.prepare(spec);
        meter.prepare(spec);
        guard.prepare(spec);
        allocator->forEachVoice(configure);
        allocator->prepare(spec);
        std::vector<dsp::VoiceEvent> chord;
        chord.reserve(16);
        for (int k = 0; k < 16; ++k)
        {
            chord.push_back({.offset = 0, .kind = dsp::VoiceEventKind::noteOn, .value = 40 + k});
        }
        process(chord);
        for (int i = 0; i < 100; ++i)
        {
            run();
        }
    }
    float run()
    {
        process({});
        return out.channel(0)[0];
    }

private:
    void process(std::span<const dsp::VoiceEvent> events)
    {
        out.block().clear();
        auto k = filterKnobs.knobs();
        k.mix = {ramps.data(), ramps.data() + 1};
        allocator->process(out.block(), events, k);
        volume.process(out.block());
        meter.process(out.block());
        static_cast<void>(guard.process(out.block()));
    }
    FilterKnobs filterKnobs;
    std::array<dsp::LinearSmoother, 2> ramps{};
    std::unique_ptr<dsp::VoiceAllocator<dsp::Voice<>>> allocator;
    dsp::MainVolume volume;
    dsp::StereoPeakMeter meter;
    dsp::OutputGuard guard;
    Buffer out;
};

} // namespace

TEST_CASE("Mixer: CPU", "[!benchmark][mixer]")
{
    using Mix = MixVoice<>;
    VoiceBench<dsp::Voice<>> library{initPatch<dsp::Voice<>>, MixPlacement::perSample, Motion::still};
    BENCHMARK("mixer: dsp::Voice, init x512")
    {
        return library.run();
    };
    for (const auto& [placement, name] : {
             std::pair{MixPlacement::gridHeld, "grid, held"},
             std::pair{MixPlacement::gridRamped, "grid, ramped"},
             std::pair{MixPlacement::perSample, "per sample"},
             std::pair{MixPlacement::partBus, "part bus (voice only)"},
         })
    {
        for (const auto& [motion, motionName] : {
                 std::pair{Motion::still, "Mix still"},
                 std::pair{Motion::ramping, "Mix ramping"},
                 std::pair{Motion::lfo, "LFO 40 Hz"},
             })
        {
            if (placement == MixPlacement::partBus && motion != Motion::still)
            {
                continue;
            }
            auto bench = std::make_unique<VoiceBench<Mix>>(initPatch<Mix>, placement, motion);
            BENCHMARK(std::string("mixer: MixVoice, init, ") + name + ", " + motionName + " x512")
            {
                return bench->run();
            };
        }
    }

    // 16 voices.
    for (const auto& [configure, engineConfigure, patch] : {
             std::tuple{initPatch<dsp::Voice<>>, initPatch<MixEngine::Voice>, "init"},
             std::tuple{worstPatch<dsp::Voice<>>, worstPatch<MixEngine::Voice>, "worst"},
         })
    {
        LibraryChord lib{configure};
        BENCHMARK(std::string("mixer: 16 voices, VoiceAllocator<dsp::Voice>, ") + patch + " x512")
        {
            return lib.run();
        };
        for (const auto& [placement, name] :
             {std::pair{MixPlacement::perSample, "per sample"}, std::pair{MixPlacement::partBus, "part bus"}})
        {
            EngineChord engine{engineConfigure, placement};
            BENCHMARK(std::string("mixer: 16 voices, engine, ") + name + ", " + patch + " x512")
            {
                return engine.run();
            };
        }
    }

    // The global stages, per block of 512 stereo samples.
    {
        const auto x = noiseBlock();
        Buffer out(2, blockSize);
        dsp::LinearSmoother gain;
        gain.setRampTime(0.01f);
        gain.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        bool up = true;
        BENCHMARK("mixer: Main Volume, ramping x512")
        {
            gain.setTarget(up ? 1.0f : 0.5f);
            up = !up;
            for (std::size_t i = 0; i < blockSize; ++i)
            {
                const float g = gain.next();
                out.channel(0)[i] = g * x[i];
                out.channel(1)[i] = g * x[i];
            }
            return out.channel(0)[0];
        };
        BENCHMARK("mixer: Main Volume, settled x512")
        {
            const float g = gain.currentValue();
            for (std::size_t i = 0; i < blockSize; ++i)
            {
                out.channel(0)[i] = g * x[i];
                out.channel(1)[i] = g * x[i];
            }
            return out.channel(0)[0];
        };
        std::array<PeakMeter, 2> peak{};
        std::array<BlockPeakMeter, 2> blockPeak{};
        std::array<RmsWindowMeter, 2> rmsWindow{};
        std::array<RmsExponentialMeter, 2> rmsExp{};
        std::array<VuMeter, 2> vu{};
        std::array<PpmMeter, 2> ppm{};
        for (std::size_t c = 0; c < 2; ++c)
        {
            peak[c].prepare(rate);
            blockPeak[c].prepare(rate);
            rmsWindow[c].prepare(rate);
            rmsExp[c].prepare(rate);
            vu[c].prepare(rate);
            ppm[c].prepare(rate);
        }
        const auto meter = [&](auto& m)
        {
            m[0].process(x);
            m[1].process(x);
            return m[0].reading();
        };
        BENCHMARK("mixer: meter, sample peak x512")
        {
            return meter(peak);
        };
        BENCHMARK("mixer: meter, block peak x512")
        {
            return meter(blockPeak);
        };
        BENCHMARK("mixer: meter, RMS window x512")
        {
            return meter(rmsWindow);
        };
        BENCHMARK("mixer: meter, RMS exponential x512")
        {
            return meter(rmsExp);
        };
        BENCHMARK("mixer: meter, VU x512")
        {
            return meter(vu);
        };
        BENCHMARK("mixer: meter, PPM x512")
        {
            return meter(ppm);
        };
        std::vector<float> guarded(x);
        BENCHMARK("mixer: output guard x512")
        {
            guardOutput(guarded);
            guardOutput(guarded);
            return guarded[0];
        };
    }

    // Room for task 5: Mix B at 0 alone saves nothing.
    {
        VoiceBench<Mix> both{twoPartPatch<Mix>, MixPlacement::perSample, Motion::still, 10.0f};
        VoiceBench<Mix> mutedB{twoPartPatch<Mix>, MixPlacement::perSample, Motion::still, 0.0f};
        VoiceBench<Mix> noB{initPatch<Mix>, MixPlacement::perSample, Motion::still, 10.0f};
        BENCHMARK("mixer: MixVoice, A1 and B1, Mix B 10 x512")
        {
            return both.run();
        };
        BENCHMARK("mixer: MixVoice, A1 and B1, Mix B 0 x512")
        {
            return mutedB.run();
        };
        BENCHMARK("mixer: MixVoice, A1 only (B1 at Voices 0) x512")
        {
            return noB.run();
        };
    }
}

TEST_CASE("Mixer as built: CPU", "[!benchmark][mixer]")
{
    LibraryMixBench still{initPatch<dsp::Voice<>>, false};
    LibraryMixBench ramping{initPatch<dsp::Voice<>>, true};
    BENCHMARK("mixer, built: dsp::Voice, init, Mix still x512")
    {
        return still.run();
    };
    BENCHMARK("mixer, built: dsp::Voice, init, Mix ramping x512")
    {
        return ramping.run();
    };
    LibraryChord plain{initPatch<dsp::Voice<>>};
    LibraryChain chain{initPatch<dsp::Voice<>>};
    BENCHMARK("mixer, built: 16 voices, VoiceAllocator<dsp::Voice>, no Mix ramps, init x512")
    {
        return plain.run();
    };
    BENCHMARK("mixer, built: 16 voices with Mix, MainVolume, StereoPeakMeter, OutputGuard, init x512")
    {
        return chain.run();
    };

    const auto x = noiseBlock();
    Buffer out(2, blockSize);
    dsp::MainVolume volume;
    volume.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
    bool up = true;
    BENCHMARK("mixer, built: MainVolume, ramping x512")
    {
        volume.setGain(up ? 1.0f : 0.5f);
        up = !up;
        std::ranges::copy(x, out.channel(0).begin());
        std::ranges::copy(x, out.channel(1).begin());
        volume.process(out.block());
        return out.channel(0)[0];
    };
    BENCHMARK("mixer, built: MainVolume, settled x512")
    {
        std::ranges::copy(x, out.channel(0).begin());
        std::ranges::copy(x, out.channel(1).begin());
        volume.process(out.block());
        return out.channel(0)[0];
    };
    BENCHMARK("mixer, built: copying the block (the two rows above include it) x512")
    {
        std::ranges::copy(x, out.channel(0).begin());
        std::ranges::copy(x, out.channel(1).begin());
        return out.channel(0)[0];
    };
    std::ranges::copy(x, out.channel(0).begin());
    std::ranges::copy(x, out.channel(1).begin());
    dsp::StereoPeakMeter meter;
    meter.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
    BENCHMARK("mixer, built: StereoPeakMeter x512")
    {
        meter.process(out.block());
        return meter.channel(0).reading();
    };
    dsp::OutputGuard guard;
    guard.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
    BENCHMARK("mixer, built: OutputGuard x512")
    {
        return guard.process(out.block());
    };
}
