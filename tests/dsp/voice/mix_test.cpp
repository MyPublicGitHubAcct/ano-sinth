// Mix A and Mix B as built (M1.4 task 4, stage 2; docs/dsp/mixer.md): the
// law (mix_law.hpp) and its use in dsp::Voice. The voice multiplies each
// filter's output, after its part's amp envelope, by its part's Mix gain,
// per sample, with Mix's global ramps read through VoiceKnobs::mix.
//
// The voice is checked bit for bit against the stage-1 prototype in its
// decided configuration (mixer_candidates.hpp: per sample, linear, unity at
// internal 10, clamped to [0, 15]) through VoiceAllocator, with Main Volume
// (output/main_volume.hpp) after it, at host blocks of 1, 37 and 4096.

#include "mixer_candidates.hpp"

#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/output/main_volume.hpp>
#include <anoesis/dsp/voice/mix_law.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::mixer;
using Catch::Matchers::WithinULP;

namespace
{

constexpr float rate = 48000.0f;

std::size_t samples(double seconds)
{
    return static_cast<std::size_t>(std::lround(seconds * static_cast<double>(rate)));
}

const dsp::MinBlepTable& table()
{
    return d6::minBlepTable64();
}

// --- Patches ------------------------------------------------------------------------------

struct Patch
{
    dsp::Waveform wave = dsp::Waveform::saw;
    std::array<int, 4> voices{1, 0, 0, 0};
    float detune = 0.0f;
    float stereo = 0.0f;
    bool retrigger = true;
    float attack = 0.0f;
    float decay = 0.0f;
    float sustain = 1.0f;
    float release = 0.0f;
    std::array<dsp::FilterInput, 2> inputs{dsp::FilterInput::own, dsp::FilterInput::own};
    float cutoff = 5.0f;
    float resonance = 0.0f;
    bool bypass = false;
};

/// A1 and B1 saws on their own filters, with a release.
const Patch twoParts{.voices = {1, 0, 1, 0}, .release = 0.2f};
/// A1, A2 and B1 unison into filter A on AB, resonant, with a release; filter
/// B on "--".
const Patch padPatch{
    .voices = {4, 4, 2, 0},
    .detune = 0.3f,
    .stereo = 1.0f,
    .retrigger = false,
    .attack = 0.05f,
    .decay = 0.3f,
    .sustain = 0.7f,
    .release = 0.3f,
    .inputs = {dsp::FilterInput::both, dsp::FilterInput::none},
    .cutoff = 2.0f,
    .resonance = 6.0f,
};

template <typename V>
void apply(V& v, const Patch& p)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        auto& slot = v.slot(s);
        slot.setWaveform(p.wave);
        slot.setVoices(p.voices[s]);
        slot.setDetune(p.detune);
        slot.setStereo(p.stereo);
        slot.setRetrigger(p.retrigger);
    }
    for (std::size_t e = 0; e < 2; ++e)
    {
        auto& env = v.envelope(e);
        env.setAttack(p.attack);
        env.setDecay(p.decay);
        env.setSustain(p.sustain);
        env.setRelease(p.release);
        v.setFilterInput(e, p.inputs[e]);
        v.setFilterBypassed(e, p.bypass);
    }
    v.drift().setDepthCents(0.0f);
}

// --- The library's mixer, as M1.8 will assemble it -----------------------------------------

/// VoiceAllocator over dsp::Voice with Mix A/B as global 10 ms ramps read
/// through VoiceKnobs::mix, and MainVolume after it; each host block split at
/// its parameter events, as the stage-1 prototype engine does.
class LibraryMixer
{
public:
    using Allocator = dsp::VoiceAllocator<dsp::Voice<>, 16>;

    LibraryMixer() : allocator(std::make_unique<Allocator>(1, table()))
    {
        for (auto& r : mixRamps)
        {
            r.setRampTime(0.01f);
            r.setCurrentAndTarget(10.0f);
        }
    }
    [[nodiscard]] Allocator& voices() noexcept
    {
        return *allocator;
    }
    [[nodiscard]] FilterKnobs& filterKnobs() noexcept
    {
        return filters;
    }
    void setMix(std::size_t part, float internal) noexcept
    {
        mixRamps[part].setCurrentAndTarget(internal);
    }
    void setMainVolume(float gain) noexcept
    {
        mainVolume.setGainImmediately(gain);
    }
    void prepare(const dsp::ProcessSpec& spec)
    {
        for (auto& r : mixRamps)
        {
            r.prepare(spec);
        }
        mainVolume.prepare(spec);
        allocator->prepare({.sampleRate = spec.sampleRate, .maxBlockSize = spec.maxBlockSize, .numChannels = 2});
        notes.assign(256, {});
    }
    void process(const dsp::AudioBlock& out, std::span<const EngineEvent> events) noexcept
    {
        const auto n = out.numSamples();
        out.clear();
        std::size_t from = 0;
        std::size_t e = 0;
        while (from < n)
        {
            std::size_t count = 0;
            std::size_t to = n;
            while (e < events.size())
            {
                const auto at = std::min(events[e].offset, n);
                if (events[e].kind != EngineEventKind::voice)
                {
                    if (at > from)
                    {
                        to = at;
                        break;
                    }
                    apply(events[e]);
                }
                else if (count < notes.size())
                {
                    notes[count] = events[e].voice;
                    notes[count].offset = at - from;
                    ++count;
                }
                ++e;
            }
            const auto piece = out.subBlock(from, to - from);
            allocator->process(piece, std::span{notes}.first(count), knobs());
            mainVolume.process(piece);
            for (auto& r : mixRamps)
            {
                r.skip(to - from);
            }
            from = to;
        }
        std::size_t count = 0;
        for (; e < events.size(); ++e)
        {
            if (events[e].kind != EngineEventKind::voice)
            {
                apply(events[e]);
            }
            else if (count < notes.size())
            {
                notes[count] = events[e].voice;
                notes[count].offset = 0;
                ++count;
            }
        }
        if (count > 0)
        {
            allocator->process(out.subBlock(n, 0), std::span{notes}.first(count), knobs());
        }
    }

private:
    [[nodiscard]] dsp::VoiceKnobs knobs() const noexcept
    {
        auto k = filters.knobs();
        k.mix = {mixRamps.data(), mixRamps.data() + 1};
        return k;
    }
    void apply(const EngineEvent& event) noexcept
    {
        if (event.kind == EngineEventKind::mix)
        {
            mixRamps[std::min<std::size_t>(event.part, 1)].setTarget(event.value);
        }
        else
        {
            mainVolume.setGain(event.value);
        }
    }

    std::unique_ptr<Allocator> allocator;
    FilterKnobs filters;
    std::array<dsp::LinearSmoother, 2> mixRamps{};
    dsp::MainVolume mainVolume;
    std::vector<dsp::VoiceEvent> notes;
};

// --- Rendering ------------------------------------------------------------------------------

EngineEvent noteOn(std::size_t at, int key)
{
    return {.offset = at, .voice = {.kind = dsp::VoiceEventKind::noteOn, .value = key}};
}
EngineEvent noteOff(std::size_t at, int key)
{
    return {.offset = at, .voice = {.kind = dsp::VoiceEventKind::noteOff, .value = key}};
}
EngineEvent mixEvent(std::size_t at, std::size_t part, float internal)
{
    return {.offset = at, .kind = EngineEventKind::mix, .part = part, .value = internal};
}
EngineEvent mainEvent(std::size_t at, float gain)
{
    return {.offset = at, .kind = EngineEventKind::mainVolume, .value = gain};
}

struct Setup
{
    Patch patch;
    std::array<float, 2> mix{10.0f, 10.0f};
    float mainVolume = 1.0f;
    std::size_t polyphony = 16;
    std::size_t length = samples(1.0);
    std::size_t hostBlock = 512;
};

struct Render
{
    Buffer out{2, 1};
    std::size_t allocations = 0;
};

template <typename Engine>
Render render(Engine& engine, const Setup& s, std::span<const EngineEvent> events)
{
    engine.voices().forEachVoice([&](auto& v) { apply(v, s.patch); });
    engine.voices().setPolyphony(s.polyphony);
    engine.filterKnobs().set(s.patch.cutoff, s.patch.resonance);
    engine.setMix(0, s.mix[0]);
    engine.setMix(1, s.mix[1]);
    engine.setMainVolume(s.mainVolume);
    engine.prepare({.sampleRate = rate, .maxBlockSize = s.hostBlock, .numChannels = 2});
    std::vector<std::vector<EngineEvent>> blocks((s.length + s.hostBlock - 1) / s.hostBlock);
    for (const auto& e : events)
    {
        const auto b = std::min(e.offset / s.hostBlock, blocks.size() - 1);
        auto moved = e;
        moved.offset -= b * s.hostBlock;
        blocks[b].push_back(moved);
    }
    Render r;
    r.out = Buffer{2, s.length};
    const auto out = r.out.block();
    const AllocationCounter counter;
    for (std::size_t b = 0; b < blocks.size(); ++b)
    {
        const auto start = b * s.hostBlock;
        engine.process(out.subBlock(start, std::min(s.hostBlock, s.length - start)), blocks[b]);
    }
    r.allocations = counter.count();
    return r;
}

Render renderLibrary(const Setup& s, std::span<const EngineEvent> events)
{
    auto engine = std::make_unique<LibraryMixer>();
    return render(*engine, s, events);
}

Render renderPrototype(const Setup& s, std::span<const EngineEvent> events)
{
    MixContext const decided; // per sample, linear, unity at internal 10, clamped to [0, 15]
    auto engine = std::make_unique<MixEngine>(table(), decided);
    return render(*engine, s, events);
}

bool sameBits(const Buffer& a, const Buffer& b)
{
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < a.numSamples(); ++i)
        {
            if (std::bit_cast<std::uint32_t>(a.channel(c)[i]) != std::bit_cast<std::uint32_t>(b.channel(c)[i]))
            {
                return false;
            }
        }
    }
    return true;
}

double energy(const Buffer& b)
{
    double e = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (const float v : b.channel(c))
        {
            e += static_cast<double>(v) * v;
        }
    }
    return e;
}

/// Steals at Polyphony 4, releases, Mix A and B automated every 300 samples
/// at odd offsets (including jumps past the knob's range), and Main Volume
/// jumps.
std::vector<EngineEvent> busySequence()
{
    std::vector<EngineEvent> e{
        noteOn(0, 45),      noteOn(1201, 52),   noteOn(5003, 57),       noteOn(9777, 60),
        noteOff(14001, 52), noteOn(15013, 64),  noteOn(20101, 69),      noteOff(26003, 45),
        noteOff(30011, 57), noteOff(40007, 60), noteOff(44009, 64),     noteOff(50003, 69),
        noteOn(60001, 48),  noteOff(80021, 48), mainEvent(28803, 0.5f), mainEvent(62411, 2.0f),
    };
    for (std::size_t k = 1; k < 300; ++k)
    {
        e.push_back(mixEvent((k * 300) + 7, k % 2, static_cast<float>((k * 7) % 19) - 2.0f));
    }
    std::ranges::stable_sort(e, {}, &EngineEvent::offset);
    return e;
}

} // namespace

// --- The law --------------------------------------------------------------------------------

TEST_CASE("Mix law: linear in internal units, unity at internal 10, x1.5 at the top", "[mixer]")
{
    static_assert(dsp::mixGain({.mix = 10.0f}) >= 1.0f && dsp::mixGain({.mix = 10.0f}) <= 1.0f);
    STATIC_REQUIRE(dsp::mixLaw.knobMax > 14.9f);
    CHECK_THAT(dsp::mixGain({.mix = 10.0f}), WithinULP(1.0f, 0));
    CHECK_THAT(dsp::mixGain({.mix = 15.0f}), WithinULP(1.5f, 0));
    CHECK_THAT(dsp::mixGain({.mix = 0.0f}), WithinULP(0.0f, 0));
    // Every quarter unit: exactly units / 10, the division rounded once.
    for (int q = 0; q <= 60; ++q)
    {
        const float units = static_cast<float>(q) * 0.25f;
        CHECK_THAT(dsp::mixGain({.mix = units}), WithinULP(units / 10.0f, 0));
        CHECK_THAT(dsp::mixUnits({.mix = units}), WithinULP(units, 0));
    }
    // 6.667 shown is internal 10, unity, within the shown value's rounding.
    CHECK_THAT(dsp::mixGain({.mix = 6.6666667f * dsp::mixInternalPerShown}), WithinULP(1.0f, 2));
    // Halving the internal value is -6.02 dB, as a linear law gives.
    CHECK_THAT(20.0 * std::log10(static_cast<double>(dsp::mixGain({.mix = 5.0f}))),
               Catch::Matchers::WithinAbs(-6.0206, 1e-4));
}

TEST_CASE("Mix law: the knob plus its modulation is clamped to [0, 15] before the law", "[mixer]")
{
    // The developer's silence point: 3.333 shown (internal 5) with -5.
    CHECK_THAT(dsp::mixGain({.mix = 5.0f, .modulation = -5.0f}), WithinULP(0.0f, 0));
    // A negative sum is silent (v3 "will mute negative volume"), not inverted.
    CHECK_THAT(dsp::mixGain({.mix = 0.0f, .modulation = -5.0f}), WithinULP(0.0f, 0));
    CHECK_THAT(dsp::mixUnits({.mix = 0.0f, .modulation = -5.0f}), WithinULP(0.0f, 0));
    // Above the top it stays at the top.
    CHECK_THAT(dsp::mixGain({.mix = 15.0f, .modulation = 15.0f}), WithinULP(1.5f, 0));
    CHECK_THAT(dsp::mixUnits({.mix = 12.0f, .modulation = 10.0f}), WithinULP(15.0f, 0));
    // Inside, the two add.
    CHECK_THAT(dsp::mixGain({.mix = 5.0f, .modulation = 2.5f}), WithinULP(0.75f, 0));
    CHECK_THAT(dsp::mixGain({.mix = 0.0f, .modulation = 5.0f}), WithinULP(0.5f, 0)); // Step Velocity 0.5 x +10
    // Not finite.
    constexpr float inf = std::numeric_limits<float>::infinity();
    CHECK_THAT(dsp::mixGain({.mix = std::numeric_limits<float>::quiet_NaN()}), WithinULP(0.0f, 0));
    CHECK_THAT(dsp::mixGain({.mix = 10.0f, .modulation = std::numeric_limits<float>::quiet_NaN()}), WithinULP(0.0f, 0));
    CHECK_THAT(dsp::mixGain({.mix = inf}), WithinULP(1.5f, 0));
    CHECK_THAT(dsp::mixGain({.mix = -inf}), WithinULP(0.0f, 0));
    // The law's numbers are parameters.
    constexpr dsp::MixLaw other{.knobMin = 0.0f, .knobMax = 10.0f, .unity = 10.0f};
    CHECK_THAT(dsp::mixGain({.mix = 15.0f}, other), WithinULP(1.0f, 0));
    CHECK_THAT(dsp::mixGain({.mix = 2.0f, .modulation = 1.0f}, other), WithinULP(0.3f, 0));
}

// --- The voice -------------------------------------------------------------------------------

TEST_CASE("Mix: the voice is the stage-1 prototype to the bit at host blocks 1, 37 and 4096", "[mixer]")
{
    const auto events = busySequence();
    for (const auto& patch : {padPatch, twoParts})
    {
        Setup s{.patch = patch, .mix = {7.0f, 12.0f}, .polyphony = 4, .length = samples(2.0)};
        for (const std::size_t block : {std::size_t{4096}, std::size_t{37}, std::size_t{1}})
        {
            s.hostBlock = block;
            const auto library = renderLibrary(s, events);
            const auto prototype = renderPrototype(s, events);
            CHECK(sameBits(library.out, prototype.out));
            CHECK(library.allocations == 0);
            CHECK(energy(library.out) > 1.0);
        }
    }
}

TEST_CASE("Mix: a null Mix ramp is unity, as a ramp at internal 10 is", "[mixer]")
{
    // The library allocator without Mix ramps (every caller before task 4).
    Setup s{.patch = twoParts, .length = samples(0.5)};
    const std::vector events{noteOn(0, 45), noteOn(333, 57), noteOff(12000, 45)};
    auto alloc = std::make_unique<dsp::VoiceAllocator<dsp::Voice<>>>(1, table());
    alloc->forEachVoice([&](dsp::Voice<>& v) { apply(v, s.patch); });
    FilterKnobs const knobs;
    alloc->prepare({.sampleRate = rate, .maxBlockSize = s.hostBlock, .numChannels = 2});
    Buffer bare{2, s.length};
    std::vector<dsp::VoiceEvent> blockEvents;
    for (std::size_t start = 0; start < s.length; start += s.hostBlock)
    {
        const auto end = std::min(s.length, start + s.hostBlock);
        blockEvents.clear();
        for (const auto& e : events)
        {
            if (e.offset >= start && e.offset < end)
            {
                auto v = e.voice;
                v.offset = e.offset - start;
                blockEvents.push_back(v);
            }
        }
        const auto piece = bare.block().subBlock(start, end - start);
        piece.clear();
        alloc->process(piece, blockEvents, knobs.knobs());
    }
    const auto atUnity = renderLibrary(s, events);
    CHECK(sameBits(bare, atUnity.out));
    CHECK(energy(bare) > 1.0);
}

TEST_CASE("Mix: each part's Mix scales its own filter's output, after the filter", "[mixer]")
{
    const std::vector events{noteOn(0, 45), noteOff(samples(0.3), 45)};
    Patch onlyA = twoParts;
    onlyA.voices = {1, 0, 0, 0};
    Patch onlyB = twoParts;
    onlyB.voices = {0, 0, 1, 0};
    const auto a = renderLibrary({.patch = onlyA, .length = samples(0.6)}, events);
    const auto b = renderLibrary({.patch = onlyB, .length = samples(0.6)}, events);

    // Mix A at the top, Mix B at 0: part A x1.5, part B gone. The filter's
    // saturation doesn't see Mix, so the scaling is exact to a rounding.
    const auto loudA = renderLibrary({.patch = twoParts, .mix = {15.0f, 0.0f}, .length = samples(0.6)}, events);
    // Mix A at 0, Mix B at internal 5: part B x0.5.
    const auto halfB = renderLibrary({.patch = twoParts, .mix = {0.0f, 5.0f}, .length = samples(0.6)}, events);
    double worstA = 0.0;
    double worstB = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < a.out.numSamples(); ++i)
        {
            // One rounding of the product (1.5 e) y against e y: 2^-24 relative, both ways.
            worstA = std::max(worstA, std::abs(loudA.out.channel(c)[i] - (1.5 * a.out.channel(c)[i])));
            worstB = std::max(worstB, std::abs(halfB.out.channel(c)[i] - (0.5 * b.out.channel(c)[i])));
        }
    }
    CHECK(worstA <= 1.5 * 1.2e-7 * 4.0); // the saw's peak stays below 4
    CHECK(worstB <= 0.5 * 1.2e-7 * 4.0);
    CHECK(energy(a.out) > 1.0);
    CHECK(energy(b.out) > 1.0);
}

TEST_CASE("Mix: a part crossing into the other filter takes the filter's part's Mix only (V3)", "[mixer]")
{
    Patch crossing = twoParts;
    crossing.voices = {0, 0, 1, 0}; // B1 alone, into filter A on AB
    crossing.inputs = {dsp::FilterInput::both, dsp::FilterInput::none};
    const std::vector events{noteOn(0, 57)};
    const auto bothUnity = renderLibrary({.patch = crossing, .length = samples(0.3)}, events);
    const auto mixBZero = renderLibrary({.patch = crossing, .mix = {10.0f, 0.0f}, .length = samples(0.3)}, events);
    const auto mixAZero = renderLibrary({.patch = crossing, .mix = {0.0f, 10.0f}, .length = samples(0.3)}, events);
    CHECK(energy(bothUnity.out) > 1.0);
    CHECK(sameBits(mixBZero.out, bothUnity.out)); // Mix B doesn't touch B1 in filter A
    CHECK(energy(mixAZero.out) <= 0.0);           // Mix A does
}

TEST_CASE("Mix: the ramp is read at every sample, and a jump ramps over 10 ms", "[mixer]")
{
    // A bypassed sine, gates: the voice's output before Mix is the slot's.
    Patch const sine{.wave = dsp::Waveform::sine, .bypass = true};
    const auto jumpAt = samples(0.2) + 7; // off the voice's 32-sample grid
    const std::vector still{noteOn(0, 57)};
    const std::vector jump{noteOn(0, 57), mixEvent(jumpAt, 0, 0.0f)};
    const auto reference = renderLibrary({.patch = sine, .mix = {10.0f, 10.0f}, .length = samples(0.4)}, still);
    const auto ramped = renderLibrary({.patch = sine, .mix = {15.0f, 10.0f}, .length = samples(0.4)}, jump);

    dsp::LinearSmoother knob;
    knob.setRampTime(0.01f);
    knob.setCurrentAndTarget(15.0f);
    knob.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
    knob.setTarget(0.0f);
    double worst = 0.0;
    for (std::size_t i = 0; i < ramped.out.numSamples(); ++i)
    {
        const float units = i < jumpAt ? 15.0f : knob.valueAfter(i - jumpAt);
        const double gain = units / 10.0f;
        for (std::size_t c = 0; c < 2; ++c)
        {
            const double expected = gain * reference.out.channel(c)[i];
            worst = std::max(worst, std::abs(ramped.out.channel(c)[i] - expected));
        }
    }
    // One rounding of (g e) y against e y, at a peak below 1.5: 1.5 x 2^-24 x 2.
    CHECK(worst <= 2.0e-7);
    // The ramp lands at 480 samples: silent from there.
    double after = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = jumpAt + 480; i < ramped.out.numSamples(); ++i)
        {
            after += std::abs(static_cast<double>(ramped.out.channel(c)[i]));
        }
    }
    CHECK(after <= 0.0);
    // ... and is still sounding one sample before.
    CHECK(std::abs(ramped.out.channel(0)[jumpAt + 478]) + std::abs(ramped.out.channel(0)[jumpAt + 477]) > 0.0f);
}
