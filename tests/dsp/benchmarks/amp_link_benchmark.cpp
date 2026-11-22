// CPU cost of the AmpEnv A/B link candidates (M1.4 task 6, stage 1;
// tests/dsp/voice/link_candidates.hpp, docs/dsp/amp-envelope-link.md), 512
// samples per run at 48 kHz, stereo. 16 voices held (keys 40-55) through the
// real VoiceAllocator, for four cases: the init patch (A1 a saw, B at
// Voices 0), two parts (A1 and B1 saws on their own filters), a pad (both
// parts' unison into filter A on AB, filter B on its own part), and two
// parts with both envelopes in a 10 s attack (the envelope's dearest stage:
// a level computed every sample). Each as built (dsp::Voice, unlinked and
// with a parameter link, which is the same code), as the prototype with no
// link, and with each candidate linked from before the notes.
//
// Tagged [!benchmark][amp-link]: run with tools/bench.sh "[amp-link]" in
// relwithdebinfo.

#include "../oscillators/oscillator_support.hpp"
#include "../voice/link_candidates.hpp"

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

using namespace anoesis::test::amp_link;
using anoesis::test::Buffer;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;

enum class Shape : std::uint8_t
{
    init,        ///< A1 a saw; B at Voices 0, filter B on B
    twoParts,    ///< A1 and B1 saws, own filters
    pad,         ///< A1 4, A2 4, B1 2 voices, detuned, spread, into filter A on AB; filter B on B
    longAttacks, ///< two parts, both envelopes in a 10 s attack
};

template <typename V>
void configure(V& v, Shape shape)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        auto& slot = v.slot(s);
        slot.setWaveform(dsp::Waveform::saw);
        int voices = 0;
        switch (shape)
        {
        case Shape::init:
            voices = s == 0 ? 1 : 0;
            break;
        case Shape::twoParts:
        case Shape::longAttacks:
            voices = s == 0 || s == 2 ? 1 : 0;
            break;
        case Shape::pad:
            if (s < 2)
            {
                voices = 4;
            }
            else if (s == 2)
            {
                voices = 2;
            }
            slot.setDetune(0.3f);
            slot.setStereo(0.7f);
            slot.setRetrigger(false);
            break;
        }
        slot.setVoices(voices);
    }
    if (shape == Shape::pad)
    {
        v.setFilterInput(0, dsp::FilterInput::both);
    }
}

EnvelopeParams envelopeFor(Shape shape)
{
    if (shape == Shape::longAttacks)
    {
        return {.attack = 10.0f, .decay = 0.5f, .sustain = 0.7f, .release = 0.3f};
    }
    return {.attack = 0.01f, .decay = 0.3f, .sustain = 0.7f, .release = 0.3f};
}

/// 16 voices through LinkEngine<V>, envelopes alike, linked or not.
template <typename V>
class Chord
{
public:
    Chord(Shape shape, const LinkPolicy& policy, bool linked)
        : engine(std::make_unique<LinkEngine<V>>(anoesis::test::d6::minBlepTable64(), policy)), out(2, blockSize)
    {
        engine->voices().forEachVoice([&](V& v) { configure(v, shape); });
        const auto env = envelopeFor(shape);
        engine->setEnvelope(0, env);
        // Part B's own knobs differ: unused by a signal link, mirrored over by a parameter link.
        engine->setEnvelope(1, linked ? EnvelopeParams{.attack = 0.5f, .decay = 1.0f, .sustain = 0.2f, .release = 1.0f}
                                      : env);
        engine->setLinked(linked);
        engine->prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        std::vector<EngineEvent> notes;
        notes.reserve(16);
        for (int k = 0; k < 16; ++k)
        {
            notes.push_back({.voice = {.kind = dsp::VoiceEventKind::noteOn, .value = 40 + k}});
        }
        engine->process(out.block(), notes);
        for (int i = 0; i < 20; ++i)
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
    std::unique_ptr<LinkEngine<V>> engine;
    Buffer out;
};

struct Candidate
{
    std::string name;
    bool library;
    LinkPolicy policy;
    bool linked;
};

} // namespace

TEST_CASE("AmpEnv A/B link: CPU, 16 voices", "[!benchmark][amp-link]")
{
    const std::vector<Candidate> candidates{
        {.name = "dsp::Voice, no link", .library = true, .linked = false},
        {.name = "(a) parameter link: dsp::Voice", .library = true, .policy = {.mirror = true}, .linked = true},
        {.name = "prototype, no link", .library = false, .linked = false},
        {
            .name = "(b) shared",
            .library = false,
            .policy = {.voice = {.kind = LinkKind::shared}, .mirror = true},
            .linked = true,
        },
        {.name = "(c) signal", .library = false, .policy = {.voice = {.kind = LinkKind::signal}}, .linked = true},
    };
    const std::vector<std::pair<Shape, std::string>> shapes{
        {Shape::init, "init"},
        {Shape::twoParts, "two parts"},
        {Shape::pad, "pad"},
        {Shape::longAttacks, "two parts, 10 s attacks"},
    };
    for (const auto& [shape, shapeName] : shapes)
    {
        for (const auto& c : candidates)
        {
            const std::string name = "amp link: 16 voices, " + shapeName + ", " + c.name + " x512";
            if (c.library)
            {
                Chord<dsp::Voice<>> bench{shape, c.policy, c.linked};
                BENCHMARK(name.c_str())
                {
                    return bench.run();
                };
            }
            else
            {
                Chord<LinkVoice<>> bench{shape, c.policy, c.linked};
                BENCHMARK(name.c_str())
                {
                    return bench.run();
                };
            }
        }
    }
}
