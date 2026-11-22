// Part solo, skipping what can't be heard, and the mono path, as built (M1.4
// task 5, stage 2; docs/dsp/part-solo-and-skipping.md):
//
// - LadderFilter::skip() and copyChannel(); OscillatorSlot's isSilent(),
//   isCentred(), canSkip() and skip();
// - dsp::PartSolo, Solo's two 10 ms ramps, read by the voice through
//   VoiceKnobs::solo and multiplied into Mix's gain per sample;
// - dsp::Voice's per-segment plan: the exact skips, the frozen skip of a fed
//   filter at gain 0 and its slots, the mono path.
//
// The voice is checked bit for bit against the stage-1 prototype in its
// decided configuration (solo_candidates.hpp, through solo_library_engine.hpp)
// through VoiceAllocator at host blocks of 1, 37 and 4096, and with nothing
// switched on against the prototype with nothing switched on (dsp::Voice as
// it was before this stage).

#include "solo_library_engine.hpp"

#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/waveform.hpp>
#include <anoesis/dsp/voice/part_solo.hpp>
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
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::solo;
using Catch::Matchers::WithinAbs;
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

// --- Patches (the stage-1 report's) -------------------------------------------------------

struct Patch
{
    std::array<dsp::Waveform, 4> waves{dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw};
    std::array<int, 4> voices{1, 0, 0, 0};
    std::array<float, 4> volume{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> pan{};
    std::array<int, 4> semitones{0, 0, 7, 0};
    float detune = 0.0f;
    float stereo = 0.0f;
    bool retrigger = true;
    std::array<float, 2> attack{0.0f, 0.0f};
    std::array<float, 2> decay{0.0f, 0.0f};
    std::array<float, 2> sustain{1.0f, 1.0f};
    std::array<float, 2> release{0.0f, 0.0f};
    std::array<dsp::FilterInput, 2> inputs{dsp::FilterInput::own, dsp::FilterInput::own};
    std::array<bool, 2> bypass{};
    float cutoff = 5.0f;
    float resonance = 0.0f;
    float inputLevel = 1.0f;
};

const Patch initPatch{};
const Patch twoParts{.voices = {1, 0, 1, 0}};
const Patch twoResonant{.voices = {1, 0, 1, 0}, .resonance = 6.0f};
const Patch crossing{.voices = {1, 0, 1, 0}, .inputs = {dsp::FilterInput::both, dsp::FilterInput::own}};
const Patch worst{.voices = {8, 8, 8, 8}, .detune = 0.548f, .stereo = 1.0f, .retrigger = false, .cutoff = 10.0f};
const Patch worstCentred{.voices = {8, 8, 8, 8}, .detune = 0.548f, .retrigger = false, .cutoff = 10.0f};
const Patch pad{
    .voices = {4, 4, 2, 0},
    .detune = 0.3f,
    .retrigger = false,
    .attack = {0.05f, 0.05f},
    .decay = {0.3f, 0.3f},
    .sustain = {0.7f, 0.7f},
    .release = {0.3f, 0.3f},
    .inputs = {dsp::FilterInput::both, dsp::FilterInput::none},
    .cutoff = 2.0f,
    .resonance = 6.0f,
};
const Patch noisePatch{
    .waves = {dsp::Waveform::noise, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw},
    .voices = {1, 0, 1, 0},
};
/// Env B decays to a sustain of 0 in 50 ms and holds there.
const Patch zeroHold = []
{
    Patch p = twoParts;
    p.decay = {0.0f, 0.05f};
    p.sustain = {1.0f, 0.0f};
    return p;
}();
const Patch crossingHold = []
{
    Patch p = crossing;
    p.decay = {0.0f, 0.05f};
    p.sustain = {1.0f, 0.0f};
    return p;
}();
/// Quiet sines, both filters bypassed: the voice's output is its slots'.
const Patch bypassedSines{
    .waves = {dsp::Waveform::sine, dsp::Waveform::sine, dsp::Waveform::sine, dsp::Waveform::sine},
    .voices = {1, 0, 1, 0},
    .volume = {0.25f, 0.25f, 0.25f, 0.25f},
    .bypass = {true, true},
};

template <typename V>
void apply(V& v, const Patch& p)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        auto& slot = v.slot(s);
        slot.setWaveform(p.waves[s]);
        slot.setVoices(p.voices[s]);
        slot.setDetune(p.detune);
        slot.setStereo(p.stereo);
        slot.setRetrigger(p.retrigger);
        slot.setVolume(p.volume[s]);
        slot.setPan(p.pan[s]);
        slot.setTuning({.semitones = p.semitones[s]});
    }
    for (std::size_t e = 0; e < 2; ++e)
    {
        auto& env = v.envelope(e);
        env.setAttack(p.attack[e]);
        env.setDecay(p.decay[e]);
        env.setSustain(p.sustain[e]);
        env.setRelease(p.release[e]);
        v.setFilterInput(e, p.inputs[e]);
        v.setFilterBypassed(e, p.bypass[e]);
    }
    v.drift().setDepthCents(1.5f);
    if constexpr (requires { v.setFilterInputLevel(1.0f); })
    {
        v.setFilterInputLevel(p.inputLevel);
    }
}

// --- Events and rendering -----------------------------------------------------------------

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
EngineEvent soloEvent(std::size_t at, bool on)
{
    return {.offset = at, .kind = EngineEventKind::solo, .value = on ? 1.0f : 0.0f};
}
EngineEvent selectEvent(std::size_t at, std::size_t part)
{
    return {.offset = at, .kind = EngineEventKind::select, .part = part};
}
EngineEvent volumeEvent(std::size_t at, std::size_t slot, float gain)
{
    return {.offset = at, .kind = EngineEventKind::slotVolume, .part = slot, .value = gain};
}
EngineEvent panEvent(std::size_t at, std::size_t slot, float pan)
{
    return {.offset = at, .kind = EngineEventKind::slotPan, .part = slot, .value = pan};
}

std::vector<EngineEvent> sorted(std::vector<EngineEvent> events)
{
    std::ranges::stable_sort(events, {}, &EngineEvent::offset);
    return events;
}

struct Setup
{
    Patch patch = initPatch;
    std::array<float, 2> mix{10.0f, 10.0f};
    bool solo = false;
    std::size_t selected = 0;
    std::size_t polyphony = 16;
    std::size_t length = samples(0.5);
    std::size_t hostBlock = 512;
};

struct Render
{
    Buffer out{2, 1};
    std::size_t allocations = 0;
};

template <typename Engine>
Render renderWith(Engine& engine, const Setup& s, std::span<const EngineEvent> events)
{
    engine.voices().forEachVoice([&](auto& v) { apply(v, s.patch); });
    engine.voices().setPolyphony(s.polyphony);
    engine.filterKnobs().set(s.patch.cutoff, s.patch.resonance);
    engine.setMix(0, s.mix[0]);
    engine.setMix(1, s.mix[1]);
    engine.prepare({.sampleRate = rate, .maxBlockSize = s.hostBlock, .numChannels = 2});
    engine.setSolo(s.solo, s.selected);
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

/// The library: dsp::Voice through VoiceAllocator, with PartSolo's ramps
/// (or, without Solo, VoiceKnobs::solo null).
Render renderLibrary(const Setup& s, std::span<const EngineEvent> events, bool withSolo = true)
{
    auto engine = std::make_unique<LibrarySoloEngine>(table(), withSolo);
    return renderWith(*engine, s, events);
}

/// The stage-1 prototype with the given switches.
Render renderPrototype(const Setup& s, std::span<const EngineEvent> events, const SkipContext& context)
{
    auto engine = std::make_unique<SoloEngine>(table(), context);
    return renderWith(*engine, s, events);
}

bool sameBits(const Buffer& a, const Buffer& b)
{
    if (a.numSamples() != b.numSamples())
    {
        return false;
    }
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

/// The first and last samples where the two differ in either channel (-1:
/// identical).
std::pair<std::ptrdiff_t, std::ptrdiff_t> differingSpan(const Buffer& a, const Buffer& b)
{
    std::ptrdiff_t first = -1;
    std::ptrdiff_t last = -1;
    for (std::size_t i = 0; i < a.numSamples(); ++i)
    {
        for (std::size_t c = 0; c < 2; ++c)
        {
            if (std::bit_cast<std::uint32_t>(a.channel(c)[i]) != std::bit_cast<std::uint32_t>(b.channel(c)[i]))
            {
                first = first < 0 ? static_cast<std::ptrdiff_t>(i) : first;
                last = static_cast<std::ptrdiff_t>(i);
            }
        }
    }
    return {first, last};
}

double energy(const Buffer& b, std::size_t from = 0, std::size_t to = 0)
{
    to = to == 0 ? b.numSamples() : to;
    double e = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = from; i < to; ++i)
        {
            e += static_cast<double>(b.channel(c)[i]) * b.channel(c)[i];
        }
    }
    return e;
}

/// The difference's RMS over [from, from + n), dB re the reference's there.
double differenceDb(const Buffer& candidate, const Buffer& reference, std::size_t from, std::size_t n)
{
    double d = 0.0;
    double r = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = from; i < from + n; ++i)
        {
            const double x = static_cast<double>(candidate.channel(c)[i]) - reference.channel(c)[i];
            d += x * x;
            r += static_cast<double>(reference.channel(c)[i]) * reference.channel(c)[i];
        }
    }
    return 10.0 * std::log10(d / r);
}

bool leftIsRight(const Buffer& b)
{
    for (std::size_t i = 0; i < b.numSamples(); ++i)
    {
        if (std::bit_cast<std::uint32_t>(b.channel(0)[i]) != std::bit_cast<std::uint32_t>(b.channel(1)[i]))
        {
            return false;
        }
    }
    return true;
}

/// Notes at Polyphony 4 (steals, releases) under every control this task
/// adds, each a few samples off the voice's grid: Mix B past 0 and back, Solo
/// on (A), Part select (B), Solo off, B1's Volume to 0 and back, and A1's Pan
/// moved (which leaves the mono path). 0.75 s.
std::vector<EngineEvent> busySequence()
{
    return sorted({
        noteOn(0, 45),
        noteOn(1201, 52),
        noteOn(5003, 57),
        noteOn(9777, 60),
        noteOff(14001, 52),
        noteOn(15013, 64), // a steal at Polyphony 4
        noteOn(20101, 69), // another
        noteOff(26003, 45),
        noteOn(27001, 48),
        mixEvent(6007, 1, -3.0f), // past the bottom: at most 0 from sample 6377, mid-segment
        mixEvent(12011, 1, 10.0f),
        soloEvent(16009, true),
        selectEvent(19001, 1),
        soloEvent(22007, false),
        volumeEvent(24003, 2, 0.0f),
        volumeEvent(28011, 2, 1.0f),
        panEvent(31002, 0, 0.2f),
    });
}

std::vector<EngineEvent> chordWith(std::vector<EngineEvent> more)
{
    std::vector<EngineEvent> e{noteOn(0, 45), noteOn(0, 52), noteOn(0, 57)};
    e.insert(e.end(), more.begin(), more.end());
    return sorted(e);
}

const std::array<std::size_t, 3> hostBlocks{4096, 37, 1};

} // namespace

// --- The voice against the prototype --------------------------------------------------------

TEST_CASE("Part solo and skipping: the voice is the stage-1 prototype as decided, to the bit at host blocks 1, 37 "
          "and 4096",
          "[part-solo]")
{
    struct Scenario
    {
        std::string name;
        Setup setup;
        std::vector<EngineEvent> events;
    };
    const auto busy = busySequence();
    const auto back = samples(0.25) + 7;
    const std::vector<Scenario> scenarios{
        {.name = "init", .setup = {.patch = initPatch, .polyphony = 4, .length = samples(0.75)}, .events = busy},
        {.name = "two parts", .setup = {.patch = twoParts, .polyphony = 4, .length = samples(0.75)}, .events = busy},
        {.name = "crossing", .setup = {.patch = crossing, .polyphony = 4, .length = samples(0.75)}, .events = busy},
        {.name = "pad", .setup = {.patch = pad, .polyphony = 4, .length = samples(0.75)}, .events = busy},
        {.name = "noise", .setup = {.patch = noisePatch, .polyphony = 4, .length = samples(0.75)}, .events = busy},
        {
            .name = "worst, centred",
            .setup = {.patch = worstCentred, .polyphony = 2, .length = samples(0.75)},
            .events = busy,
        },
        {.name = "worst, spread", .setup = {.patch = worst, .polyphony = 2, .length = samples(0.75)}, .events = busy},
        {
            .name = "Mix B 0 from the note-on, back at 0.25 s, resonant",
            .setup = {.patch = twoResonant, .mix = {10.0f, 0.0f}},
            .events = chordWith({mixEvent(back, 1, 10.0f)}),
        },
        {
            .name = "Solo A from the note-on, Part select B, Solo off",
            .setup = {.patch = crossing, .solo = true, .selected = 0},
            .events = chordWith({selectEvent(back, 1), soloEvent(back + 4801, false)}),
        },
        {
            .name = "env B held at sustain 0, retriggered in place (Polyphony 1)",
            .setup = {.patch = zeroHold, .polyphony = 1},
            .events = sorted({noteOn(0, 45), noteOn(back, 48), noteOff(back + 6000, 48)}),
        },
        {
            .name = "crossing, env B held at sustain 0, retriggered in place (Polyphony 1)",
            .setup = {.patch = crossingHold, .polyphony = 1},
            .events = sorted({noteOn(0, 45), noteOn(back, 48)}),
        },
    };
    for (const auto& sc : scenarios)
    {
        for (const std::size_t block : hostBlocks)
        {
            INFO(sc.name << ", host blocks of " << block);
            Setup s = sc.setup;
            s.hostBlock = block;
            const auto library = renderLibrary(s, sc.events);
            const auto prototype = renderPrototype(s, sc.events, decidedContext());
            CHECK(sameBits(library.out, prototype.out));
            CHECK(library.allocations == 0);
            CHECK(energy(library.out) > 1.0);
        }
    }
}

TEST_CASE("Part solo and skipping: with no Solo ramps and nothing switched on, the voice is dsp::Voice as it was",
          "[part-solo]")
{
    // The prototype with nothing switched on is dsp::Voice before this stage
    // (stage 1 measured it, 12 of 12). The library, with VoiceKnobs::solo
    // null, skips only what is exact and takes the mono path, which is exact:
    // the same to the bit on every patch, with Mix moving and back.
    const auto events = sorted({
        noteOn(0, 45),
        noteOn(1201, 52),
        noteOn(5003, 57),
        noteOff(14001, 52),
        noteOn(15013, 64),
        noteOff(20011, 45),
        mixEvent(3001, 1, 4.0f),
        mixEvent(9007, 0, 12.0f),
        mixEvent(17003, 1, 10.0f),
        panEvent(19005, 0, -0.4f),
    });
    for (const auto& patch : {initPatch, twoParts, crossing, pad, worst, noisePatch})
    {
        for (const std::size_t block : {std::size_t{4096}, std::size_t{37}})
        {
            const Setup s{.patch = patch, .polyphony = 4, .length = samples(0.5), .hostBlock = block};
            const auto library = renderLibrary(s, events, false);
            const auto asItWas = renderPrototype(s, events, asItWasContext());
            CHECK(sameBits(library.out, asItWas.out));
            CHECK(energy(library.out) > 1.0);
        }
    }
}

TEST_CASE("Part solo: a null Solo ramp is 1, so a caller without Solo is unchanged to the bit", "[part-solo]")
{
    const auto events = busySequence();
    for (const auto& patch : {twoParts, crossing, pad})
    {
        // Solo's ramps at 1 (Solo off throughout) against none at all.
        std::vector<EngineEvent> noSolo;
        std::ranges::copy_if(events, std::back_inserter(noSolo), [](const EngineEvent& e)
                             { return e.kind != EngineEventKind::solo && e.kind != EngineEventKind::select; });
        const Setup s{.patch = patch, .polyphony = 4, .length = samples(0.75)};
        const auto withRamps = renderLibrary(s, noSolo, true);
        const auto without = renderLibrary(s, noSolo, false);
        CHECK(sameBits(withRamps.out, without.out));
        CHECK(energy(without.out) > 1.0);
    }
}

// --- What is skipped --------------------------------------------------------------------------

TEST_CASE("Part solo and skipping: each exact skip is identical to not skipping", "[part-solo]")
{
    struct Case
    {
        std::string name;
        Setup setup;
    };
    Patch volumeB = twoParts;
    volumeB.volume = {1.0f, 1.0f, 0.0f, 1.0f};
    Patch silent = initPatch;
    silent.voices = {0, 0, 0, 0};
    const std::vector<Case> cases{
        {.name = "init: filter B at rest on part B's silence", .setup = {.patch = initPatch}},
        {.name = "B1 at Volume 0: its slot, and filter B on zeros", .setup = {.patch = volumeB}},
        {.name = "every oscillator at Voices 0", .setup = {.patch = silent}},
        {.name = "crossing part held at sustain 0", .setup = {.patch = crossingHold}},
        {.name = "Mix B at 0 throughout: the output stage", .setup = {.patch = twoParts, .mix = {10.0f, 0.0f}}},
        {.name = "B soloed out throughout", .setup = {.patch = twoParts, .solo = true, .selected = 0}},
        {.name = "A soloed out, crossing", .setup = {.patch = crossing, .solo = true, .selected = 1}},
    };
    const auto events = chordWith({});
    for (const auto& c : cases)
    {
        for (const std::size_t block : {std::size_t{512}, std::size_t{37}})
        {
            INFO(c.name << ", host blocks of " << block);
            Setup s = c.setup;
            s.hostBlock = block;
            const auto library = renderLibrary(s, events);
            // Nothing skipped, stereo: the same Solo ramps, the routes left alone.
            const SkipContext nothing;
            const auto reference = renderPrototype(s, events, nothing);
            CHECK(sameBits(library.out, reference.out));
        }
    }
}

TEST_CASE("Part solo and skipping: a fed filter frozen at gain 0 is identical while skipped, and after its return "
          "within the difference measured in stage 1",
          "[part-solo]")
{
    // Stage 1's figures (docs/dsp/part-solo-and-skipping.md, "Skipping: each
    // case"): the RMS of the difference over the 50 ms after the return, dB
    // re the reference's, and the last differing sample after it. Pinned on
    // both sides: within 1 dB (an ulp-level change in rounding, from another
    // compiler's contraction, moves these by hundredths of a dB) and the
    // last differing sample within 25% (the tail where the difference falls
    // below one ulp can move by a few samples with that rounding). A value
    // far below means the skip didn't happen; far above, that the state or
    // phase came back wrong.
    struct Case
    {
        std::string name;
        Setup setup;
        std::vector<EngineEvent> events;
        std::size_t resume;
        double db;
        std::ptrdiff_t until;
    };
    const auto back = samples(0.5) + 7;
    Patch volumeB = twoParts;
    volumeB.volume = {1.0f, 1.0f, 0.0f, 1.0f};
    Patch bypassedSaws = twoParts;
    bypassedSaws.bypass = {true, true};
    const std::vector<Case> cases{
        {
            .name = "Mix B 10, to 0 at 0.25 s, back at 0.5 s",
            .setup = {.patch = twoParts, .length = samples(1.0)},
            .events = chordWith({mixEvent(samples(0.25) + 3, 1, 0.0f), mixEvent(back, 1, 10.0f)}),
            .resume = back,
            .db = -64.0,
            .until = 208,
        },
        {
            .name = "the same, resonant",
            .setup = {.patch = twoResonant, .length = samples(1.0)},
            .events = chordWith({mixEvent(samples(0.25) + 3, 1, 0.0f), mixEvent(back, 1, 10.0f)}),
            .resume = back,
            .db = -39.4,
            .until = 1622,
        },
        {
            .name = "Solo A, then off at 0.5 s",
            .setup = {.patch = twoParts, .solo = true, .selected = 0, .length = samples(1.0)},
            .events = chordWith({soloEvent(back, false)}),
            .resume = back,
            .db = -68.4,
            .until = 207,
        },
        {
            .name = "env B held at sustain 0, retriggered in place at 0.5 s (Polyphony 1)",
            .setup = {.patch = zeroHold, .polyphony = 1, .length = samples(1.0)},
            .events = sorted({noteOn(0, 45), noteOn(back, 48)}),
            .resume = back,
            .db = -36.6,
            .until = 76,
        },
        {
            .name = "filters bypassed, B soloed out, then Solo off at 0.5 s (B1's slot skipped, the phase advanced)",
            .setup = {.patch = bypassedSaws, .solo = true, .selected = 0, .length = samples(1.0)},
            .events = chordWith({soloEvent(back, false)}),
            .resume = back,
            .db = -78.9, // measured in stage 2: the jump's residual alone, for the kernel's 64 samples
            .until = 63,
        },
        {
            .name = "B1 at Volume 0, then 1 at 0.5 s (its slot skipped, the phase advanced)",
            .setup = {.patch = volumeB, .length = samples(1.0)},
            .events = chordWith({volumeEvent(back, 2, 1.0f)}),
            .resume = back,
            .db = -82.4,
            .until = 122,
        },
    };
    for (const auto& c : cases)
    {
        INFO(c.name);
        const auto library = renderLibrary(c.setup, c.events);
        const SkipContext nothing;
        const auto reference = renderPrototype(c.setup, c.events, nothing);
        const auto [first, last] = differingSpan(library.out, reference.out);
        REQUIRE(first >= 0);
        CHECK(first >= static_cast<std::ptrdiff_t>(c.resume)); // identical while skipped
        const double db = differenceDb(library.out, reference.out, c.resume, samples(0.05));
        CHECK_THAT(db, WithinAbs(c.db, 1.0));
        const auto until = last - static_cast<std::ptrdiff_t>(c.resume);
        CHECK(until >= (c.until * 3) / 4);
        CHECK(until <= (c.until * 5) / 4);
    }
}

// --- The filter -------------------------------------------------------------------------------

TEST_CASE("LadderFilter: skip() on a filter at rest is process() on zeros, at any split", "[part-solo]")
{
    const dsp::ProcessSpec spec{.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2};
    const auto control = [](dsp::LadderFilter& f, std::size_t step)
    {
        // Every ramp the filter has, moving: cutoff, its modulation, resonance
        // and its modulation, drive, the response's crossfade.
        f.setCutoff(500.0f + (300.0f * static_cast<float>(step % 5)));
        f.setCutoffModulation(0.25f * static_cast<float>(step % 3));
        f.setResonance(0.5f * static_cast<float>(step % 4));
        f.setResonanceModulation(0.3f * static_cast<float>(step % 2));
        f.setDrive(1.0f + (0.7f * static_cast<float>(step % 3)));
        f.setResponse(static_cast<dsp::FilterResponse>(step % dsp::filterResponseCount));
    };
    for (const std::size_t split : {
             std::size_t{1},
             std::size_t{5},
             std::size_t{31},
             std::size_t{32},
             std::size_t{33},
             std::size_t{100},
             std::size_t{700},
         })
    {
        INFO("split " << split);
        dsp::LadderFilter processed;
        dsp::LadderFilter skipped;
        processed.prepare(spec);
        skipped.prepare(spec);
        Buffer zeros{2, 1500};
        std::size_t step = 0;
        for (std::size_t at = 0; at < 1500; at += split, ++step)
        {
            const auto n = std::min(split, std::size_t{1500} - at);
            control(processed, step);
            control(skipped, step);
            processed.process(zeros.block().subBlock(at, n));
            skipped.skip(n);
        }
        CHECK(energy(zeros) <= 0.0);
        CHECK(processed.isAtRest());
        CHECK(skipped.isAtRest());
        // Then the same signal into both, with the controls still moving: the
        // grid and every ramp stand where process() left them.
        Buffer a{2, 2000};
        for (std::size_t i = 0; i < 2000; ++i)
        {
            a.channel(0)[i] = 0.8f * std::sin(0.031f * static_cast<float>(i));
            a.channel(1)[i] = 0.5f * std::sin(0.047f * static_cast<float>(i));
        }
        Buffer b = a;
        for (std::size_t at = 0; at < 2000; at += 77, ++step)
        {
            const auto n = std::min(std::size_t{77}, std::size_t{2000} - at);
            control(processed, step);
            control(skipped, step);
            processed.process(a.block().subBlock(at, n));
            skipped.process(b.block().subBlock(at, n));
        }
        CHECK(sameBits(a, b));
        CHECK(energy(a) > 1.0);
    }
}

TEST_CASE("LadderFilter: skip() keeps the state as it was", "[part-solo]")
{
    // With the ramps settled, a filter skipped by whole segments and one not
    // skipped at all continue the same: the state is untouched and the grid
    // has come round.
    const dsp::ProcessSpec spec{.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2};
    dsp::LadderFilter f;
    f.prepare(spec);
    f.setCutoff(800.0f);
    f.setResonance(3.0f);
    f.setDrive(2.0f);
    Buffer warm{2, 1024};
    for (std::size_t i = 0; i < 1024; ++i)
    {
        warm.channel(0)[i] = std::sin(0.05f * static_cast<float>(i));
        warm.channel(1)[i] = std::sin(0.07f * static_cast<float>(i));
    }
    f.process(warm.block()); // past every 10 ms ramp, on the grid
    dsp::LadderFilter g = f;
    dsp::LadderFilter solved = f;
    f.skip(std::size_t{32} * 7);
    CHECK_FALSE(f.isAtRest());
    Buffer zeros{2, std::size_t{32} * 7};
    solved.process(zeros.block()); // what skipping isn't: solving silence
    Buffer a{2, 300};
    for (std::size_t i = 0; i < 300; ++i)
    {
        a.channel(0)[i] = 0.3f * std::sin(0.02f * static_cast<float>(i));
        a.channel(1)[i] = a.channel(0)[i];
    }
    Buffer b = a;
    Buffer c = a;
    f.process(a.block());
    g.process(b.block());
    solved.process(c.block());
    CHECK(sameBits(a, b));
    CHECK_FALSE(sameBits(a, c)); // the state carried energy, and the skip kept it
    CHECK(energy(a) > 0.1);
}

TEST_CASE("LadderFilter: copyChannel() sets one channel's state from another's", "[part-solo]")
{
    const dsp::ProcessSpec spec{.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2};
    dsp::LadderFilter f;
    f.prepare(spec);
    f.setResonance(3.5f);
    Buffer different{2, 400};
    for (std::size_t i = 0; i < 400; ++i)
    {
        different.channel(0)[i] = std::sin(0.05f * static_cast<float>(i));
        different.channel(1)[i] = 0.2f * std::sin(0.11f * static_cast<float>(i));
    }
    f.process(different.block());
    // The same input on both channels: their outputs differ until channel 1
    // takes channel 0's state.
    const auto same = [](std::size_t n)
    {
        Buffer b{2, n};
        for (std::size_t i = 0; i < n; ++i)
        {
            b.channel(0)[i] = 0.7f * std::sin(0.03f * static_cast<float>(i));
            b.channel(1)[i] = b.channel(0)[i];
        }
        return b;
    };
    dsp::LadderFilter g = f;
    auto before = same(200);
    g.process(before.block());
    CHECK_FALSE(leftIsRight(before));
    f.copyChannel(0, 1);
    auto after = same(200);
    f.process(after.block());
    CHECK(leftIsRight(after));
    CHECK(energy(after) > 1.0);
    // An index past the last channel is the last channel.
    dsp::LadderFilter h = g;
    h.copyChannel(0, 5);
    auto clamped = same(200);
    h.process(clamped.block());
    CHECK(leftIsRight(clamped));
}

// --- The slot ---------------------------------------------------------------------------------

namespace
{

using Slot = dsp::OscillatorSlot<dsp::minBlepTaps>;

void prepareSlot(Slot& s, int voices, dsp::Waveform wave = dsp::Waveform::saw)
{
    s.prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 2});
    s.setRandomSeed(77);
    s.setWaveform(wave);
    s.setVoices(voices);
    s.setDetune(0.5f);
    s.setStereo(0.6f);
    s.setRetrigger(false);
}

/// process() of an empty block: takes a skip's jump, renders nothing.
void touch(Slot& s, float note)
{
    Buffer none{2, 1};
    s.process(none.block().subBlock(0, 0), note);
}

} // namespace

TEST_CASE("OscillatorSlot: skip() advances each unison voice's phase exactly as rendering does", "[part-solo]")
{
    for (const std::size_t n :
         {std::size_t{1}, std::size_t{63}, std::size_t{64}, std::size_t{1000}, std::size_t{480000}})
    {
        INFO(n << " samples");
        Slot rendered{table()};
        Slot skipped{table()};
        prepareSlot(rendered, 8);
        prepareSlot(skipped, 8);
        rendered.noteOn();
        skipped.noteOn();
        Buffer warm{2, 100};
        rendered.process(warm.block(), 57.3f);
        skipped.process(warm.block(), 57.3f);
        Buffer through{2, n};
        rendered.process(through.block(), 57.3f);
        REQUIRE(skipped.canSkip());
        skipped.skip(n, 57.3f);
        touch(skipped, 57.3f);
        for (std::size_t v = 0; v < 8; ++v)
        {
            CHECK(skipped.oscillator(v).nextPhase() == rendered.oscillator(v).nextPhase());
        }
        // The jump is band-limited: the two differ only while its residual
        // is in flight, then are the same to the bit.
        Buffer a{2, 256};
        Buffer b{2, 256};
        rendered.process(a.block(), 57.3f);
        skipped.process(b.block(), 57.3f);
        for (std::size_t c = 0; c < 2; ++c)
        {
            for (std::size_t i = dsp::minBlepTaps; i < 256; ++i)
            {
                CHECK(std::bit_cast<std::uint32_t>(a.channel(c)[i]) == std::bit_cast<std::uint32_t>(b.channel(c)[i]));
            }
        }
    }
}

TEST_CASE("OscillatorSlot: skip() with a note per sample adds up each sample's step", "[part-solo]")
{
    Slot rendered{table()};
    Slot skipped{table()};
    prepareSlot(rendered, 5);
    prepareSlot(skipped, 5);
    rendered.noteOn();
    skipped.noteOn();
    std::vector<float> notes(777);
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        notes[i] = 50.0f + (0.013f * static_cast<float>(i)); // a glide
    }
    Buffer warm{2, 40};
    rendered.process(warm.block(), 50.0f);
    skipped.process(warm.block(), 50.0f);
    Buffer through{2, notes.size()};
    rendered.process(through.block(), notes);
    skipped.skip(notes);
    touch(skipped, notes.back());
    for (std::size_t v = 0; v < 5; ++v)
    {
        CHECK(skipped.oscillator(v).nextPhase() == rendered.oscillator(v).nextPhase());
    }
    // The jump also comes at a noteOn() or setVoices(), before they act.
    Slot again{table()};
    prepareSlot(again, 5);
    again.noteOn();
    again.process(warm.block(), 50.0f);
    again.skip(notes);
    again.setVoices(6); // the jump first, then a new voice at the next note
    for (std::size_t v = 0; v < 5; ++v)
    {
        CHECK(again.oscillator(v).nextPhase() == rendered.oscillator(v).nextPhase());
    }
}

TEST_CASE("OscillatorSlot: a Noise slot's skip() draws what rendering would", "[part-solo]")
{
    for (const float volume : {1.0f, 0.0f})
    {
        Slot rendered{table()};
        Slot skipped{table()};
        prepareSlot(rendered, 3, dsp::Waveform::noise);
        prepareSlot(skipped, 3, dsp::Waveform::noise);
        rendered.setVolume(volume);
        skipped.setVolume(volume);
        rendered.noteOn();
        skipped.noteOn();
        Buffer through{2, 1001};
        rendered.process(through.block(), 60.0f);
        skipped.skip(1001, 60.0f);
        rendered.setVolume(1.0f);
        skipped.setVolume(1.0f);
        Buffer a{2, 600};
        Buffer b{2, 600};
        rendered.process(a.block(), 60.0f);
        skipped.process(b.block(), 60.0f);
        CHECK(sameBits(a, b));
        CHECK(energy(a) > 1.0);
    }
}

TEST_CASE("OscillatorSlot: isSilent(), isCentred() and canSkip()", "[part-solo]")
{
    Buffer chunk{2, 600};
    SECTION("silent: every rendered gain settled at exactly 0")
    {
        Slot s{table()};
        prepareSlot(s, 1);
        CHECK_FALSE(s.isSilent());
        s.setVolume(0.0f); // before anything is heard: at once
        CHECK(s.isSilent());
        CHECK(s.canSkip());
        s.setVolume(0.5f);
        s.noteOn();
        s.process(chunk.block(), 60.0f);
        s.setVolume(0.0f); // while sounding: a 10 ms ramp
        CHECK_FALSE(s.isSilent());
        CHECK_FALSE(s.canSkip());
        s.process(chunk.block().subBlock(0, 479), 60.0f);
        CHECK_FALSE(s.isSilent());
        s.process(chunk.block().subBlock(0, 1), 60.0f);
        CHECK(s.isSilent());
        CHECK(s.canSkip());
        // Voices 0, faded out.
        Slot off{table()};
        prepareSlot(off, 2);
        off.noteOn();
        off.process(chunk.block(), 60.0f);
        off.setVoices(0);
        CHECK_FALSE(off.isSilent());
        CHECK_FALSE(off.canSkip()); // a fading voice's gain ramp moves
        off.process(chunk.block(), 60.0f);
        CHECK(off.isSilent());
        CHECK(off.canSkip());
        // Noise at Volume 0.
        Slot noise{table()};
        prepareSlot(noise, 2, dsp::Waveform::noise);
        CHECK_FALSE(noise.isSilent());
        noise.setVolume(0.0f);
        CHECK(noise.isSilent());
    }
    SECTION("centred: left and right the same to the bit, now and at the target")
    {
        Slot s{table()};
        prepareSlot(s, 8);
        s.setStereo(0.0f);
        CHECK(s.isCentred());
        s.setInverted(true);
        CHECK(s.isCentred());
        s.noteOn();
        s.process(chunk.block(), 60.0f);
        CHECK(leftIsRight(chunk));
        s.setPan(0.3f);
        CHECK_FALSE(s.isCentred());
        s.process(chunk.block(), 60.0f);
        CHECK_FALSE(s.isCentred());
        s.setPan(0.0f); // the target is centred, the gains on their way
        CHECK_FALSE(s.isCentred());
        s.process(chunk.block(), 60.0f);
        CHECK(s.isCentred());
        s.setStereo(0.4f);
        CHECK_FALSE(s.isCentred());
        // One voice at Stereo 1 is still at Pan.
        Slot one{table()};
        prepareSlot(one, 1);
        one.setStereo(1.0f);
        CHECK(one.isCentred());
        // Noise mixes two draws: never centred.
        Slot noise{table()};
        prepareSlot(noise, 1, dsp::Waveform::noise);
        noise.setStereo(0.0f);
        CHECK_FALSE(noise.isCentred());
    }
}

// --- Solo -------------------------------------------------------------------------------------

TEST_CASE("PartSolo: two 10 ms ramps, from Solo and the selected part", "[part-solo]")
{
    dsp::PartSolo solo;
    solo.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
    const auto gain = [&](std::size_t part)
    {
        return solo.gains()[part]->currentValue();
    };
    CHECK_FALSE(solo.isOn());
    CHECK(solo.selectedPart() == 0);
    CHECK_THAT(gain(0), WithinULP(1.0f, 0));
    CHECK_THAT(gain(1), WithinULP(1.0f, 0));

    // A reference ramp: Mix's, 10 ms linear.
    const auto ramp = [](float from, float to)
    {
        dsp::LinearSmoother r;
        r.setRampTime(0.01f);
        r.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
        r.setCurrentAndTarget(from);
        r.setTarget(to);
        return r;
    };

    // Solo on, A selected: B ramps to 0 over 480 samples, A stays at 1.
    solo.setSolo(true);
    CHECK(solo.isOn());
    auto down = ramp(1.0f, 0.0f);
    for (std::size_t i = 0; i < 200; ++i)
    {
        CHECK_THAT(solo.gains()[1]->valueAfter(i), WithinULP(down.valueAfter(i), 0));
        CHECK_THAT(solo.gains()[0]->valueAfter(i), WithinULP(1.0f, 0));
    }
    solo.skip(200);
    down.skip(200);
    // Part select under Solo, mid-ramp: both cross over 10 ms from where they are.
    solo.selectPart(1);
    CHECK(solo.selectedPart() == 1);
    const auto aDown = ramp(1.0f, 0.0f);
    const auto bUp = ramp(down.currentValue(), 1.0f);
    for (std::size_t i = 0; i < 600; ++i)
    {
        CHECK_THAT(solo.gains()[0]->valueAfter(i), WithinULP(aDown.valueAfter(i), 0));
        CHECK_THAT(solo.gains()[1]->valueAfter(i), WithinULP(bUp.valueAfter(i), 0));
    }
    CHECK_THAT(solo.gains()[0]->valueAfter(480), WithinULP(0.0f, 0));
    CHECK(solo.gains()[0]->valueAfter(479) > 0.0f);
    solo.skip(600);
    CHECK_THAT(gain(0), WithinULP(0.0f, 0));
    CHECK_THAT(gain(1), WithinULP(1.0f, 0));
    // Solo off: both to 1.
    solo.setSolo(false);
    const auto aUp = ramp(0.0f, 1.0f);
    for (std::size_t i = 0; i < 600; ++i)
    {
        CHECK_THAT(solo.gains()[0]->valueAfter(i), WithinULP(aUp.valueAfter(i), 0));
        CHECK_THAT(solo.gains()[1]->valueAfter(i), WithinULP(1.0f, 0));
    }
    solo.skip(600);
    // Part select with Solo off changes no gain, and is remembered.
    solo.selectPart(0);
    CHECK_THAT(gain(0), WithinULP(1.0f, 0));
    CHECK_FALSE(solo.gains()[0]->isSmoothing());
    CHECK_FALSE(solo.gains()[1]->isSmoothing());
    solo.selectPart(7); // past the last part: the last part
    CHECK(solo.selectedPart() == 1);
    // At once: a session restored soloed.
    solo.setImmediately(true, 0);
    CHECK(solo.isOn());
    CHECK(solo.selectedPart() == 0);
    CHECK_THAT(gain(0), WithinULP(1.0f, 0));
    CHECK_THAT(gain(1), WithinULP(0.0f, 0));
    CHECK_FALSE(solo.gains()[1]->isSmoothing());
    // prepare() finishes the ramps where they are going.
    solo.setSolo(false);
    solo.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
    CHECK_THAT(gain(1), WithinULP(1.0f, 0));
}

TEST_CASE("Part solo: the voice multiplies each part's Solo gain into its Mix gain, at every sample", "[part-solo]")
{
    // Bypassed sines, so the output is linear in each part. Solo on (A), then
    // Part select (B) before B's ramp reaches 0, then Solo off before A's
    // does: no gain reaches 0, nothing is skipped, and the output is A and B
    // each times its ramp, sample by sample.
    const std::size_t on = 4807;
    const std::size_t select = on + 203;
    const std::size_t off = select + 211;
    const auto events = sorted({noteOn(0, 57), soloEvent(on, true), selectEvent(select, 1), soloEvent(off, false)});
    const Setup both{.patch = bypassedSines, .length = 7000};
    Setup onlyA = both;
    onlyA.patch.voices = {1, 0, 0, 0};
    Setup onlyB = both;
    onlyB.patch.voices = {0, 0, 1, 0};
    const auto soloed = renderLibrary(both, events);
    const auto a = renderLibrary(onlyA, sorted({noteOn(0, 57)}));
    const auto b = renderLibrary(onlyB, sorted({noteOn(0, 57)}));

    dsp::PartSolo reference;
    reference.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
    double largest = 0.0;
    bool aMoved = false;
    bool bMoved = false;
    for (std::size_t i = 0; i < both.length; ++i)
    {
        if (i == on)
        {
            reference.setSolo(true);
        }
        if (i == select)
        {
            reference.selectPart(1);
        }
        if (i == off)
        {
            reference.setSolo(false);
        }
        const double ga = reference.gains()[0]->currentValue();
        const double gb = reference.gains()[1]->currentValue();
        aMoved = aMoved || ga < 1.0;
        bMoved = bMoved || gb < 1.0;
        CHECK(ga > 0.0);
        CHECK(gb > 0.0);
        for (std::size_t c = 0; c < 2; ++c)
        {
            const double expected = (ga * a.out.channel(c)[i]) + (gb * b.out.channel(c)[i]);
            largest = std::max(largest, std::abs(soloed.out.channel(c)[i] - expected));
        }
        reference.skip(1);
    }
    CHECK(aMoved);
    CHECK(bMoved);
    // Each part's (g e) y rounded once, then the sum: within 3 roundings of a
    // sum under 0.4 (each part's centred sine peaks below 0.18).
    CHECK(largest <= 3.0 * 0.4 * 6.0e-8);
    CHECK(energy(soloed.out) > 1.0);
}

TEST_CASE("Part solo: Solo silences the other part's filter output; a part crossing into the selected filter still "
          "sounds",
          "[part-solo]")
{
    // Filter A on AB, filter B on B: Solo A is Mix B at 0, to the bit, and B1
    // is still heard through filter A (Syntorial's reading, decided).
    const auto events = chordWith({noteOff(samples(0.3), 45)});
    const Setup base{.patch = crossing, .length = samples(0.4)};
    Setup soloA = base;
    soloA.solo = true;
    soloA.selected = 0;
    Setup mixB0 = base;
    mixB0.mix = {10.0f, 0.0f};
    CHECK(sameBits(renderLibrary(soloA, events).out, renderLibrary(mixB0, events).out));
    Setup soloAWithoutB1 = soloA;
    soloAWithoutB1.patch.voices = {1, 0, 0, 0};
    const auto withB1 = renderLibrary(soloA, events);
    const auto withoutB1 = renderLibrary(soloAWithoutB1, events);
    CHECK_FALSE(sameBits(withB1.out, withoutB1.out));
    CHECK(energy(withB1.out) > energy(withoutB1.out) * 1.1);
    // Solo B is Mix A at 0: B1 through filter A goes with it.
    Setup soloB = base;
    soloB.solo = true;
    soloB.selected = 1;
    Setup mixA0 = base;
    mixA0.mix = {0.0f, 10.0f};
    const auto b = renderLibrary(soloB, events);
    CHECK(sameBits(b.out, renderLibrary(mixA0, events).out));
    CHECK(energy(b.out) > 1.0);
}

// --- When the test is made ----------------------------------------------------------------------

TEST_CASE("Part solo and skipping: the test is per segment, so the output doesn't depend on the host's block size",
          "[part-solo]")
{
    // Skips that aren't exact (a fed filter frozen, a slot whose phase jumps
    // at its return) start and end where the bounds over whole segments say,
    // never where a host block ends.
    const auto events = sorted({
        noteOn(0, 45),
        noteOn(0, 52),
        noteOn(3001, 57),
        mixEvent(6007, 1, 0.0f), // lands at 6487: mid-segment
        mixEvent(12011, 1, 10.0f),
        soloEvent(16009, true),
        selectEvent(19001, 1),
        soloEvent(22007, false),
        volumeEvent(23003, 2, 0.0f), // B1's gain settles at 0 mid-segment: its slot's skip waits for a grid point
        volumeEvent(26011, 2, 1.0f),
        noteOff(27003, 52),
    });
    for (const auto& patch : {twoParts, twoResonant, crossing})
    {
        const Setup s{.patch = patch, .length = samples(0.6), .hostBlock = 4096};
        const auto reference = renderLibrary(s, events);
        for (const std::size_t block : {
                 std::size_t{1},
                 std::size_t{2},
                 std::size_t{3},
                 std::size_t{7},
                 std::size_t{31},
                 std::size_t{33},
                 std::size_t{37},
                 std::size_t{64},
             })
        {
            INFO("host blocks of " << block);
            Setup b = s;
            b.hostBlock = block;
            CHECK(sameBits(renderLibrary(b, events).out, reference.out));
        }
    }
    // The sequence can tell: the prototype deciding per chunk differs.
    SkipContext perChunk = decidedContext();
    perChunk.timing = SkipTiming::perChunk;
    const Setup s{.patch = twoParts, .length = samples(0.6), .hostBlock = 4096};
    Setup b = s;
    b.hostBlock = 37;
    CHECK_FALSE(sameBits(renderPrototype(s, events, perChunk).out, renderPrototype(b, events, perChunk).out));
}

TEST_CASE("Part solo and skipping: a call mid-segment ends a skip at once", "[part-solo]")
{
    // B soloed out from the note-on (its filter and slot skipped); Solo off 7
    // samples into a segment. B is heard from there, not from the next grid
    // point (the grid counts from the note-on at 0).
    const std::size_t off = (32 * 500) + 7;
    const Setup s{.patch = twoParts, .solo = true, .selected = 0, .length = off + 200, .hostBlock = 64};
    const auto stays = renderLibrary(s, sorted({noteOn(0, 45)}));
    const auto ends = renderLibrary(s, sorted({noteOn(0, 45), soloEvent(off, false)}));
    const auto [first, last] = differingSpan(ends.out, stays.out);
    CHECK(first >= static_cast<std::ptrdiff_t>(off));
    CHECK(first < static_cast<std::ptrdiff_t>(off + 3));
    CHECK(last > 0);
}

TEST_CASE("Part solo and skipping: an envelope held at level 0 is silent only while its sustain is 0", "[part-solo]")
{
    // Env B decays to a sustain of 0 and holds, so part B is skipped. Then
    // Sustain goes up while the note is held, 7 samples into a segment: the
    // level ramps up from 0 in the sustain stage, and part B is heard again
    // from there (the call there re-tests and ends the skip).
    const dsp::ProcessSpec spec{.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2};
    FilterKnobs knobs;
    knobs.set(5.0f, 0.0f);
    const std::size_t raise = (32 * 600) + 7;
    const std::size_t length = raise + 2000;
    const auto renderVoice = [&](bool raised)
    {
        auto voice = std::make_unique<dsp::Voice<>>(table(), 1);
        apply(*voice, zeroHold);
        voice->prepare(spec);
        voice->noteOn(45.0f, knobs.knobs());
        Buffer out{2, length};
        voice->process(out.block().subBlock(0, raise), knobs.knobs());
        if (raised)
        {
            voice->envelope(1).setSustain(0.7f);
        }
        voice->process(out.block().subBlock(raise, length - raise), knobs.knobs().advanced(raise));
        return out;
    };
    const auto held = renderVoice(false);
    const auto raised = renderVoice(true);
    const auto [first, last] = differingSpan(raised, held);
    CHECK(first >= static_cast<std::ptrdiff_t>(raise));
    CHECK(first < static_cast<std::ptrdiff_t>(raise + 3));
    CHECK(last > 0);
    // Part B is back: the raised render is louder after the ramp.
    CHECK(energy(raised, raise + 1000, length) > 1.5 * energy(held, raise + 1000, length));
}

// --- The mono path ------------------------------------------------------------------------------

TEST_CASE("Part solo and skipping: the mono path is exact, left with copyChannel() where a slot moves off "
          "centre, and never taken on Noise",
          "[part-solo]")
{
    // The reference never takes the mono path: B1 panned off centre, on a
    // part nothing hears (filter B on "--", filter A on its own part), so the
    // output is the same patch's and only the path differs.
    struct Centred
    {
        std::string name;
        Patch patch;
    };
    Patch padA = pad;
    padA.inputs = {dsp::FilterInput::own, dsp::FilterInput::none};
    padA.voices = {4, 4, 0, 0};
    Patch worstA = worstCentred;
    worstA.voices = {8, 8, 0, 0};
    worstA.inputs = {dsp::FilterInput::own, dsp::FilterInput::none};
    Patch initA = initPatch;
    initA.inputs = {dsp::FilterInput::own, dsp::FilterInput::none};
    const std::vector<Centred> patches{
        {.name = "init", .patch = initA},
        {.name = "worst, centred", .patch = worstA},
        {.name = "pad on filter A, resonant, attack and release", .patch = padA},
    };
    const auto moved = sorted({
        noteOn(0, 45),
        noteOn(1201, 52),
        noteOn(5003, 57),
        noteOff(9001, 45),
        panEvent(samples(0.2) + 5, 0, 0.3f),
        noteOff(samples(0.3), 52),
    });
    // A note-on on the sounding voice (Polyphony 1) after the Pan has come
    // back to the centre: the voice stays stereo, since the filters' two
    // channels no longer hold the same state.
    const auto stolen = sorted({
        noteOn(0, 45),
        panEvent(2001, 0, 0.3f),
        panEvent(4003, 0, 0.0f),
        noteOn(5009, 52),
        noteOff(samples(0.3), 52),
    });
    for (const auto& c : patches)
    {
        INFO(c.name << ", a steal after the Pan came back");
        const Setup mono{.patch = c.patch, .polyphony = 1, .length = samples(0.35), .hostBlock = 512};
        Setup stereo = mono;
        stereo.patch.voices[2] = 1;
        stereo.patch.pan[2] = 0.3f;
        CHECK(sameBits(renderLibrary(mono, stolen).out, renderLibrary(stereo, stolen).out));
    }
    for (const auto& c : patches)
    {
        for (const float level : {1.0f, 2.5f})
        {
            for (const std::size_t block : {std::size_t{4096}, std::size_t{37}})
            {
                INFO(c.name << ", input level " << level << ", host blocks of " << block);
                Setup mono{.patch = c.patch, .polyphony = 4, .length = samples(0.4), .hostBlock = block};
                mono.patch.inputLevel = level;
                Setup stereo = mono;
                stereo.patch.voices[2] = 1;
                stereo.patch.pan[2] = 0.3f;
                const auto m = renderLibrary(mono, moved);
                const auto r = renderLibrary(stereo, moved);
                CHECK(sameBits(m.out, r.out));
                CHECK(energy(m.out) > 1.0);
            }
        }
    }

    // Where it is taken and left.
    const auto taken = [](const Patch& p, std::span<const EngineEvent> events, std::size_t length)
    {
        auto engine = std::make_unique<LibrarySoloEngine>(table());
        engine->voices().forEachVoice([&](dsp::Voice<>& v) { apply(v, p); });
        engine->prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
        Buffer out{2, length};
        engine->process(out.block(), events);
        return std::pair{engine->voices().voice(0).isMono(), leftIsRight(out)};
    };
    const auto note = sorted({noteOn(0, 45)});
    CHECK(taken(initPatch, note, 600) == std::pair{true, true});
    CHECK(taken(worstCentred, note, 600) == std::pair{true, true});
    CHECK(taken(pad, note, 600) == std::pair{true, true});
    CHECK_FALSE(taken(worst, note, 600).first);
    CHECK_FALSE(taken(noisePatch, note, 600).first);
    // Left at the Pan move, and not taken again when the Pan comes back to
    // the centre within the note.
    const auto leaves = sorted({noteOn(0, 45), panEvent(300, 0, 0.3f), panEvent(400, 0, 0.0f)});
    CHECK_FALSE(taken(initPatch, leaves, 2000).first);
}
