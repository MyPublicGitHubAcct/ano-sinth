// The mixer report (docs/dsp/mixer.md, M1.4 task 4, stage 1): measures the
// candidates in mixer_candidates.hpp over the real dsp::VoiceAllocator:
//
// - the prototype against the library: MixVoice at every Mix at unity is
//   dsp::Voice to the bit, per voice; the part buses within rounding;
// - question 1, the Mix law: the four laws and three clamps against the
//   developer's numbers, and Mix acting after the filter (the level scales
//   exactly);
// - question 2, where Mix applies: zipper and clicks (energy above 1 kHz in
//   a Hann 4096, dB re a unit step, as filters.md measures) for Mix swept by
//   host automation, jumped, and modulated by a free LFO and a per-voice
//   pluck, per grid point held or ramped, per sample, or on a part bus;
//   and V3's two readings of a crossing part;
// - question 3, Main Volume: its laws, and zipper and clicks with and
//   without the 10 ms ramp;
// - question 4, the meters: readings of reference signals, ballistics
//   against IEC 60268-17 (VU) and 60268-10 Type I (PPM), and the same
//   reading at the same sample whatever the block split;
// - question 5, no output clipping: absolute peak and RMS of the init patch,
//   a 16-voice chord and the worst case; the output scaling exactly with
//   Main Volume up to x1000; non-finite and subnormal samples;
// - the real-time terms: the same output at host blocks of 1, 37 and 4096,
//   no allocation while processing.
//
// Hidden from the default run; run it in an optimised build (about 40 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[mixer-report]"
//
// CPU: the "[mixer]" benchmarks (tools/bench.sh "[mixer]").

#include "mixer_candidates.hpp"

#include "../filters/filter_control_support.hpp"
#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::mixer;

namespace
{

constexpr float rate = 48000.0f;

std::string fixed(double x, int digits)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

std::size_t samples(double seconds)
{
    return static_cast<std::size_t>(std::lround(seconds * static_cast<double>(rate)));
}

std::string dbText(double db, int digits = 1)
{
    if (!std::isfinite(db))
    {
        return db < 0.0 ? "silent" : "inf";
    }
    return fixed(db, digits);
}

double toDb(double gain)
{
    return gain > 0.0 ? 20.0 * std::log10(gain) : -std::numeric_limits<double>::infinity();
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
    float cutoff = 5.0f; ///< both filters' Cutoff knob (the Filter Control's at 5)
    float resonance = 0.0f;
    float drive = 1.0f;
    bool bypass = false; ///< both filters bypassed
};

/// The init patch as far as the evidence goes (V9): A1 a saw at Volume 10,
/// LP24 at 4,677 Hz, k 0, both envelopes a gate.
const Patch initSaw{};
/// The same with a sine.
const Patch initSine{.wave = dsp::Waveform::sine};
/// A sine with both filters bypassed, for zipper: at Volume 10 the filter's
/// saturation adds harmonics above 1 kHz (V8), which would mask it.
const Patch cleanSine{.wave = dsp::Waveform::sine, .bypass = true};
/// Both parts sounding: A1 and B1 saws on their own filters.
const Patch twoParts{.voices = {1, 0, 1, 0}};
/// The worst case for level: 4 oscillators x 8 unison saws at Volume 10,
/// both parts on their own filters, the cutoff open (Cutoff 10 + 5).
const Patch worstSaw{
    .voices = {8, 8, 8, 8},
    .detune = 0.548f,
    .stereo = 1.0f,
    .retrigger = false,
    .cutoff = 10.0f,
};
/// A pad for the identity checks: A1, A2 and B1 unison into filter A on AB,
/// resonant, with a release; filter B on "--".
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
        slot.setVolume(1.0f);
    }
    for (std::size_t e = 0; e < 2; ++e)
    {
        auto& env = v.envelope(e);
        env.setAttack(p.attack);
        env.setDecay(p.decay);
        env.setSustain(p.sustain);
        env.setRelease(p.release);
        v.setFilterInput(e, p.inputs[e]);
        v.setFilterDrive(e, p.drive);
        v.setFilterBypassed(e, p.bypass);
    }
    v.drift().setDepthCents(0.0f);
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
EngineEvent mainEvent(std::size_t at, float gain)
{
    return {.offset = at, .kind = EngineEventKind::mainVolume, .value = gain};
}

std::vector<EngineEvent> sorted(std::vector<EngineEvent> events)
{
    std::ranges::stable_sort(events, {}, &EngineEvent::offset);
    return events;
}

struct Setup
{
    Patch patch = initSine;
    MixContext mix{};
    std::array<float, 2> mixKnobs{10.0f, 10.0f}; ///< internal units
    float mainVolume = 1.0f;
    bool smoothMix = true;
    bool smoothMain = true;
    std::size_t polyphony = 16;
    std::size_t length = samples(1.0);
    std::size_t hostBlock = 512;
    MeterSet* meters = nullptr;
};

struct Render
{
    Buffer out{2, 1};
    std::size_t allocations = 0; ///< while processing
};

/// The events of each host block, at offsets within it, built before the
/// render so that building them isn't counted.
std::vector<std::vector<EngineEvent>> perBlock(std::span<const EngineEvent> events, std::size_t length,
                                               std::size_t hostBlock)
{
    std::vector<std::vector<EngineEvent>> blocks((length + hostBlock - 1) / hostBlock);
    for (const auto& e : events)
    {
        const auto b = std::min(e.offset / hostBlock, blocks.size() - 1);
        auto moved = e;
        moved.offset = e.offset - (b * hostBlock);
        blocks[b].push_back(moved);
    }
    return blocks;
}

Render render(const Setup& s, std::span<const EngineEvent> events)
{
    auto engine = std::make_unique<MixEngine>(table(), s.mix);
    engine->voices().forEachVoice([&](MixEngine::Voice& v) { apply(v, s.patch); });
    engine->voices().setPolyphony(s.polyphony);
    engine->filterKnobs().set(s.patch.cutoff, s.patch.resonance);
    engine->setMixSmoothing(s.smoothMix);
    engine->setMainVolumeSmoothing(s.smoothMain);
    engine->setMix(0, s.mixKnobs[0]);
    engine->setMix(1, s.mixKnobs[1]);
    engine->setMainVolume(s.mainVolume);
    engine->prepare({.sampleRate = rate, .maxBlockSize = s.hostBlock, .numChannels = 2});
    engine->attach(s.meters);
    const auto blocks = perBlock(events, s.length, s.hostBlock);
    Render r;
    r.out = Buffer{2, s.length};
    const auto out = r.out.block();
    {
        const AllocationCounter counter;
        for (std::size_t b = 0; b < blocks.size(); ++b)
        {
            const auto start = b * s.hostBlock;
            engine->process(out.subBlock(start, std::min(s.hostBlock, s.length - start)), blocks[b]);
        }
        r.allocations = counter.count();
    }
    return r;
}

/// The library (dsp::VoiceAllocator over dsp::Voice, seed 1 as the engine's)
/// on the same patch and note events, in host blocks.
Buffer renderLibrary(const Setup& s, std::span<const EngineEvent> events)
{
    auto alloc = std::make_unique<dsp::VoiceAllocator<dsp::Voice<>>>(1, table());
    alloc->forEachVoice([&](dsp::Voice<>& v) { apply(v, s.patch); });
    alloc->setPolyphony(s.polyphony);
    FilterKnobs knobs;
    knobs.set(s.patch.cutoff, s.patch.resonance);
    alloc->prepare({.sampleRate = rate, .maxBlockSize = s.hostBlock, .numChannels = 2});
    Buffer out{2, s.length};
    const auto block = out.block();
    std::vector<dsp::VoiceEvent> voiceEvents;
    for (std::size_t start = 0; start < s.length; start += s.hostBlock)
    {
        const auto end = std::min(s.length, start + s.hostBlock);
        voiceEvents.clear();
        for (const auto& e : events)
        {
            if (e.kind == EngineEventKind::voice && e.offset >= start && e.offset < end)
            {
                auto v = e.voice;
                v.offset = e.offset - start;
                voiceEvents.push_back(v);
            }
        }
        const auto piece = block.subBlock(start, end - start);
        piece.clear();
        alloc->process(piece, voiceEvents, knobs.knobs());
    }
    return out;
}

bool sameBits(const Buffer& a, const Buffer& b)
{
    for (std::size_t c = 0; c < 2; ++c)
    {
        const auto x = a.channel(c);
        const auto y = b.channel(c);
        if (x.size() != y.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            if (std::bit_cast<std::uint32_t>(x[i]) != std::bit_cast<std::uint32_t>(y[i]))
            {
                return false;
            }
        }
    }
    return true;
}

double maxAbsDifference(const Buffer& a, const Buffer& b)
{
    double d = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < a.numSamples(); ++i)
        {
            d = std::max(d, std::abs(static_cast<double>(a.channel(c)[i]) - b.channel(c)[i]));
        }
    }
    return d;
}

double peakOf(const Buffer& b, std::size_t from = 0)
{
    double p = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = from; i < b.numSamples(); ++i)
        {
            p = std::max(p, static_cast<double>(std::abs(b.channel(c)[i])));
        }
    }
    return p;
}

double rmsOf(std::span<const float> x)
{
    double sum = 0.0;
    for (const float v : x)
    {
        sum += static_cast<double>(v) * v;
    }
    return x.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(x.size()));
}

double stereoRms(const Buffer& b, std::size_t from, std::size_t n)
{
    const double l = rmsOf(b.channel(0).subspan(from, n));
    const double r = rmsOf(b.channel(1).subspan(from, n));
    return std::sqrt(0.5 * ((l * l) + (r * r)));
}

/// Energy above 1 kHz in a Hann 4096 centred 32 samples after `at`, both
/// channels, as a fraction of an instant unit step's in both.
double clickEnergy(std::span<const float> left, std::span<const float> right, std::size_t at)
{
    static const double step = []
    {
        std::vector<float> x(8192, 0.0f);
        std::fill(x.begin() + 4096, x.end(), 1.0f);
        return filter_control::energyAbove1k(x, 4096 + 32, rate);
    }();
    return (filter_control::energyAbove1k(left, at + 32, rate) + filter_control::energyAbove1k(right, at + 32, rate)) /
           (2.0 * step);
}

/// filters.md's click, dB re a unit step, averaged (in power) over `at`.
double clickDb(const Buffer& b, std::span<const std::size_t> at)
{
    double e = 0.0;
    for (const auto t : at)
    {
        e += clickEnergy(b.channel(0), b.channel(1), t);
    }
    e /= static_cast<double>(at.size());
    return e > 0.0 ? 10.0 * std::log10(e) : -std::numeric_limits<double>::infinity();
}

/// The same measure on the difference a - b: what a candidate adds to the reference.
double differenceDb(const Buffer& a, const Buffer& b, std::span<const std::size_t> at)
{
    Buffer d{2, a.numSamples()};
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < a.numSamples(); ++i)
        {
            d.channel(c)[i] = a.channel(c)[i] - b.channel(c)[i];
        }
    }
    return clickDb(d, at);
}

constexpr std::array placements{
    MixPlacement::gridHeld,
    MixPlacement::gridRamped,
    MixPlacement::perSample,
    MixPlacement::partBus,
};

std::string_view placementName(MixPlacement p)
{
    switch (p)
    {
    case MixPlacement::gridHeld:
        return "per voice, grid, held";
    case MixPlacement::gridRamped:
        return "per voice, grid, ramped";
    case MixPlacement::perSample:
        return "per voice, per sample";
    case MixPlacement::partBus:
        return "part bus (global)";
    }
    return "";
}

std::string_view lawName(MixLaw l)
{
    switch (l)
    {
    case MixLaw::linear10:
        return "linear, unity at internal 10 (6.67 shown)";
    case MixLaw::linear15:
        return "linear, unity at the top (10 shown)";
    case MixLaw::squared10:
        return "squared, unity at internal 10";
    case MixLaw::decibel:
        return "dB fader, unity at internal 10, +6 dB top";
    }
    return "";
}

// --- The prototype against the library ----------------------------------------------------

std::vector<EngineEvent> identitySequence()
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
        noteOff(30011, 57),
        noteOff(40007, 60),
        noteOff(44009, 64),
        noteOff(50003, 69),
    });
}

void printIdentity()
{
    std::cout << "\n## The prototype against the library\n\n"
                 "MixVoice through dsp::VoiceAllocator (the engine), every Mix at internal 10 (linear10: exactly 1), "
                 "Main Volume 1, no modulation, against dsp::VoiceAllocator over dsp::Voice on the same events "
                 "(Polyphony 4, two steals), 1.5 s at host blocks of 512 and 37.\n\n"
                 "| Patch | placement | block 512 | block 37 |\n|---|---|---|---|\n";
    const auto events = identitySequence();
    for (const auto& [name, patch] : {
             std::pair{"init (saw)", initSaw},
             std::pair{"pad (AB, resonant)", padPatch},
             std::pair{"A1 and B1 saws, own filters", twoParts},
         })
    {
        for (const auto placement : placements)
        {
            std::cout << "| " << name << " | " << placementName(placement) << " |";
            for (const std::size_t block : {std::size_t{512}, std::size_t{37}})
            {
                Setup s{.patch = patch, .polyphony = 4, .length = samples(1.5), .hostBlock = block};
                s.mix.placement = placement;
                const auto r = render(s, events);
                const auto lib = renderLibrary(s, events);
                if (sameBits(r.out, lib))
                {
                    std::cout << " identical |";
                }
                else
                {
                    std::cout << " max |difference| " << std::scientific << std::setprecision(2)
                              << maxAbsDifference(r.out, lib) << std::defaultfloat << " (peak " << fixed(peakOf(lib), 3)
                              << ") |";
                }
            }
            std::cout << '\n';
        }
    }
}

// --- Question 1: the law ------------------------------------------------------------------

constexpr std::array laws{MixLaw::linear10, MixLaw::linear15, MixLaw::squared10, MixLaw::decibel};

void printLaws()
{
    std::cout
        << "\n## Question 1: the Mix law\n\nGain in dB at the displayed position (internal = shown x 1.5).\n\n"
           "| Law | 0 | 1 | 2.5 | 3.333 | 5 | 6.667 | 7.5 | 10 |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto law : laws)
    {
        std::cout << "| " << lawName(law) << " |";
        for (const float shown : {0.0f, 1.0f, 2.5f, 3.3333333f, 5.0f, 6.6666667f, 7.5f, 10.0f})
        {
            std::cout << ' ' << dbText(toDb(mixGain(law, mixInternal(shown))), 2) << " |";
        }
        std::cout << '\n';
    }

    std::cout << "\nThe developer's cases, gain (linear) through each clamp, law linear10 unless stated:\n\n"
                 "| Case | knob + modulation (internal) | [0, 15] (F24) | at 0 only (v3) | none (v2) |\n"
                 "|---|---:|---:|---:|---:|\n";
    struct Case
    {
        std::string_view name;
        float knob;
        float modulation;
    };
    for (const auto& c : {
             Case{
                 .name = "Mix 3.333 shown, -10 x keytrack 0.5 at A4 (silence, 2016)",
                 .knob = mixInternal(3.3333333f),
                 .modulation = -5.0f,
             },
             Case{.name = "Mix 5 shown, -10 x 0.5", .knob = mixInternal(5.0f), .modulation = -5.0f},
             Case{
                 .name = "Mix 0 shown, -10 x 0.5 (\"a negative volume simply inverts\")",
                 .knob = 0.0f,
                 .modulation = -5.0f,
             },
             Case{.name = "Mix 0 shown, -10 x 1 (A9)", .knob = 0.0f, .modulation = -10.0f},
             Case{
                 .name = "Mix 10 shown, +15 (\"in 2 mod dest slots\")",
                 .knob = mixInternal(10.0f),
                 .modulation = 15.0f,
             },
             Case{.name = "Mix 0, Step Velocity 0.5 x +10 to Mix AB (manual p. 22)", .knob = 0.0f, .modulation = 5.0f},
             Case{.name = "Mix 0, Step Velocity 1 x +10", .knob = 0.0f, .modulation = 10.0f},
         })
    {
        std::cout << "| " << c.name << " | " << fixed(c.knob + c.modulation, 2) << " |";
        for (const auto clamp : {MixClamp::knobRange, MixClamp::floorOnly, MixClamp::none})
        {
            std::cout << ' ' << fixed(mixGain(MixLaw::linear10, clampMix(c.knob + c.modulation, clamp)), 3) << " |";
        }
        std::cout << '\n';
    }

    std::cout << "\nStep Velocity to Mix AB at +10 with the Mix knobs at 0 (manual p. 22: \"the output volume will be "
                 "set purely by the Step Velocity\"), dB re velocity 1, through [0, 15]:\n\n"
                 "| Law | velocity 0.125 | 0.25 | 0.5 | 0.75 |\n|---|---:|---:|---:|---:|\n";
    for (const auto law : laws)
    {
        std::cout << "| " << lawName(law) << " |";
        const float top = mixGain(law, 10.0f);
        for (const float v : {0.125f, 0.25f, 0.5f, 0.75f})
        {
            std::cout << ' ' << dbText(toDb(mixGain(law, clampMix(10.0f * v, MixClamp::knobRange)) / top), 2) << " |";
        }
        std::cout << '\n';
    }

    // Mix after the filter: the output scales exactly, the filter's drive unchanged.
    Setup s{.patch = initSaw, .length = samples(0.5)};
    s.mix.placement = MixPlacement::perSample;
    const std::vector events{noteOn(0, 45)};
    const auto unity = render(s, events);
    s.mixKnobs = {15.0f, 15.0f};
    const auto top = render(s, events);
    double worst = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < s.length; ++i)
        {
            const double expected = 1.5 * unity.out.channel(c)[i];
            if (std::abs(expected) > 1e-3)
            {
                worst = std::max(worst, std::abs((top.out.channel(c)[i] - expected) / expected));
            }
        }
    }
    std::cout << "\nMix after the filter (init saw, A2, linear10): internal 15 against 10, sample by sample, is x1.5 "
                 "to a relative "
              << std::scientific << std::setprecision(2) << worst << std::defaultfloat << " (RMS ratio "
              << fixed(stereoRms(top.out, 0, s.length) / stereoRms(unity.out, 0, s.length), 7)
              << "): the filter's saturation doesn't see Mix.\n";
}

// --- Question 2: where Mix applies --------------------------------------------------------

struct ZipperCase
{
    std::string_view name;
    std::vector<EngineEvent> events;
    std::array<float, 2> knobs{10.0f, 10.0f};
    bool smoothMix = true;
    MixModulationSpec modulation{};
    std::vector<std::size_t> windows;
};

std::vector<ZipperCase> zipperCases()
{
    std::vector<ZipperCase> cases;
    const std::vector<std::size_t> sweepWindows{samples(0.3), samples(0.5), samples(0.7), samples(0.9)};
    auto const automation = [&](bool smooth)
    {
        ZipperCase c{
            .name = smooth ? "automation 15 to 0 over 1 s, an event every 512 samples, ramped 10 ms"
                           : "the same automation, unramped (each event a step)",
            .knobs = {15.0f, 10.0f},
            .smoothMix = smooth,
            .windows = sweepWindows,
        };
        c.events.push_back(noteOn(0, 57));
        for (std::size_t k = 1; k <= 94; ++k)
        {
            c.events.push_back(
                mixEvent(samples(0.1) + (k * 512) + 7, 0, 15.0f * (1.0f - (static_cast<float>(k) / 94.0f))));
        }
        return c;
    };
    cases.push_back(
        {.name = "Mix still (the floor: the sine's own energy)", .events = {noteOn(0, 57)}, .windows = sweepWindows});
    cases.push_back(automation(true));
    cases.push_back(automation(false));
    // At 0.5 s + 7 samples: off the voice's grid, as host events usually are.
    const auto jumpAt = samples(0.5) + 7;
    cases.push_back({
        .name = "a jump 15 to 0 at 0.5 s, ramped 10 ms",
        .events = {noteOn(0, 57), mixEvent(jumpAt, 0, 0.0f)},
        .knobs = {15.0f, 10.0f},
        .windows = {jumpAt},
    });
    cases.push_back({
        .name = "a jump 15 to 0 at 0.5 s, unramped",
        .events = {noteOn(0, 57), mixEvent(jumpAt, 0, 0.0f)},
        .knobs = {15.0f, 10.0f},
        .smoothMix = false,
        .windows = {jumpAt},
    });
    cases.push_back({
        .name = "a free LFO, 5 Hz, +-5 around 7.5 (5 shown)",
        .events = {noteOn(0, 57)},
        .knobs = {7.5f, 10.0f},
        .modulation = {.kind = MixModulation::lfo, .amount = 5.0f, .rateHz = 5.0f},
        .windows = sweepWindows,
    });
    cases.push_back({
        .name = "a free LFO, 40 Hz, +-5 around 7.5",
        .events = {noteOn(0, 57)},
        .knobs = {7.5f, 10.0f},
        .modulation = {.kind = MixModulation::lfo, .amount = 5.0f, .rateHz = 40.0f},
        .windows = sweepWindows,
    });
    ZipperCase pluck{
        .name = "a pluck per note, +15 on Mix 0, 10 ms, a note every 0.25 s",
        .knobs = {0.0f, 10.0f},
        .modulation = {.kind = MixModulation::pluck, .amount = 15.0f, .decaySeconds = 0.01f},
        .windows = {samples(0.25), samples(0.5), samples(0.75)},
    };
    for (int k = 0; k < 4; ++k)
    {
        const auto at = samples(0.25 * k);
        if (k > 0)
        {
            pluck.events.push_back(noteOff(at - 1, 57));
        }
        pluck.events.push_back(noteOn(at, 57));
    }
    cases.push_back(pluck);
    return cases;
}

Render zipperRender(const ZipperCase& c, MixPlacement placement)
{
    Setup s{.patch = cleanSine, .mixKnobs = c.knobs, .smoothMix = c.smoothMix, .length = samples(1.2)};
    s.mix.placement = placement;
    s.mix.modulation = c.modulation;
    return render(s, sorted(c.events));
}

void printZipper()
{
    std::cout << "\n## Question 2: zipper and clicks, by where Mix applies\n\n"
                 "A1 a sine at A3 (220 Hz), Volume 10, both filters bypassed (their saturation would add harmonics "
                 "above 1 kHz: V8), gates; Mix A swept or modulated, "
                 "linear10 through [0, 15]. Energy above 1 kHz in a Hann 4096, dB re an instant unit step, averaged "
                 "over the windows (the sweeps at 0.3/0.5/0.7/0.9 s; a jump at its sample; the pluck at three "
                 "note-ons). *Added*: the same measure on the difference from per sample.\n\n"
                 "| Case | grid, held | grid, ramped | per sample | part bus | added: held | added: ramped | "
                 "added: bus |\n|---|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& c : zipperCases())
    {
        std::array<Render, 4> r{};
        for (std::size_t p = 0; p < placements.size(); ++p)
        {
            r[p] = zipperRender(c, placements[p]);
        }
        const bool pluck = c.modulation.kind == MixModulation::pluck;
        std::cout << "| " << c.name << " |";
        for (std::size_t p = 0; p < 4; ++p)
        {
            std::cout << ' ' << (pluck && p == 3 ? "(can't)" : dbText(clickDb(r[p].out, c.windows))) << " |";
        }
        for (const std::size_t p : {std::size_t{0}, std::size_t{1}, std::size_t{3}})
        {
            std::cout << ' ' << (pluck && p == 3 ? "-" : dbText(differenceDb(r[p].out, r[2].out, c.windows))) << " |";
        }
        std::cout << '\n';
    }
}

// --- V3: a crossing part ---------------------------------------------------------------

void printCrossing()
{
    std::cout << "\n## V3: which Mix acts on a part crossing into the other filter\n\n"
                 "Filter A on AB, filter B on --, B1 a sine at A3 only, both envelopes gates. Stereo RMS (dBFS) "
                 "from 0.1 s to 0.6 s.\n\n"
                 "| Mix A, Mix B (internal) | the filter's part's Mix only (the hypothesis) | the crossing part's own "
                 "Mix too, before the filter |\n|---|---:|---:|\n";
    Patch p = initSine;
    p.voices = {0, 0, 1, 0};
    p.inputs = {dsp::FilterInput::both, dsp::FilterInput::none};
    for (const auto& knobs :
         {std::array{10.0f, 10.0f}, std::array{10.0f, 0.0f}, std::array{0.0f, 10.0f}, std::array{10.0f, 15.0f}})
    {
        std::cout << "| " << fixed(knobs[0], 0) << ", " << fixed(knobs[1], 0) << " |";
        for (const bool source : {false, true})
        {
            Setup s{.patch = p, .mixKnobs = knobs, .length = samples(0.7)};
            s.mix.crossingTakesSourceMix = source;
            const auto r = render(s, std::vector{noteOn(0, 57)});
            std::cout << ' ' << dbText(toDb(stereoRms(r.out, samples(0.1), samples(0.5))), 2) << " |";
        }
        std::cout << '\n';
    }
}

// --- Question 3: Main Volume --------------------------------------------------------------

void printMainVolume()
{
    std::cout << "\n## Question 3: Main Volume\n\nCandidate laws, gain in dB at the knob:\n\n"
                 "| Law | 0 | 1 | 2.5 | 5 | 7.5 | 10 |\n|---|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& [law, name] : {
             std::pair{VolumeLaw::linear, "linear"},
             std::pair{VolumeLaw::squared, "squared"},
             std::pair{VolumeLaw::decibel, "dB, -60 to 0"},
         })
    {
        std::cout << "| " << name << " |";
        for (const float k : {0.0f, 1.0f, 2.5f, 5.0f, 7.5f, 10.0f})
        {
            std::cout << ' ' << dbText(toDb(volumeGain(law, k)), 2) << " |";
        }
        std::cout << '\n';
    }

    std::cout << "\nZipper and clicks (the bypassed sine, the same measure as question 2), the gain linear:\n\n"
                 "| Case | ramped 10 ms | unramped |\n|---|---:|---:|\n";
    const std::vector<std::size_t> sweepWindows{samples(0.3), samples(0.5), samples(0.7), samples(0.9)};
    std::vector<EngineEvent> sweep{noteOn(0, 57)};
    for (std::size_t k = 1; k <= 94; ++k)
    {
        sweep.push_back(mainEvent(samples(0.1) + (k * 512) + 7, 1.0f - (static_cast<float>(k) / 94.0f)));
    }
    const std::vector jump{noteOn(0, 57), mainEvent(samples(0.5) + 7, 0.0f)};
    for (const auto& [name, events, windows] : {
             std::tuple{"automation 1 to 0 over 1 s, an event every 512 samples", sweep, sweepWindows},
             std::tuple{"a jump 1 to 0 at 0.5 s", jump, std::vector<std::size_t>{samples(0.5) + 7}},
         })
    {
        std::cout << "| " << name << " |";
        for (const bool smooth : {true, false})
        {
            Setup const s{.patch = cleanSine, .smoothMain = smooth, .length = samples(1.2)};
            const auto r = render(s, sorted(events));
            std::cout << ' ' << dbText(clickDb(r.out, windows)) << " |";
        }
        std::cout << '\n';
    }
}

// --- Question 4: the meters ---------------------------------------------------------------

std::vector<float> sineAt(double hz, double amplitude, std::size_t n, double phase = 0.0)
{
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        x[i] = static_cast<float>(amplitude *
                                  std::sin((2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate) + phase));
    }
    return x;
}

struct Readings
{
    double peak = 0.0;
    double rmsWindow = 0.0;
    double rmsExponential = 0.0;
    double vu = 0.0;
    double ppm = 0.0;
};

Readings readAll(std::span<const float> x)
{
    PeakMeter peak;
    RmsWindowMeter rmsWindow;
    RmsExponentialMeter rmsExp;
    VuMeter vu;
    PpmMeter ppm;
    peak.prepare(rate);
    rmsWindow.prepare(rate);
    rmsExp.prepare(rate);
    vu.prepare(rate);
    ppm.prepare(rate);
    peak.process(x);
    rmsWindow.process(x);
    rmsExp.process(x);
    vu.process(x);
    ppm.process(x);
    return {
        .peak = peak.reading(),
        .rmsWindow = rmsWindow.reading(),
        .rmsExponential = rmsExp.reading(),
        .vu = vu.reading(),
        .ppm = ppm.reading(),
    };
}

/// The highest reading each meter reaches over x.
Readings highestOf(std::span<const float> x)
{
    PeakMeter peak;
    RmsWindowMeter rmsWindow;
    RmsExponentialMeter rmsExp;
    VuMeter vu;
    PpmMeter ppm;
    peak.prepare(rate);
    rmsWindow.prepare(rate);
    rmsExp.prepare(rate);
    vu.prepare(rate);
    ppm.prepare(rate);
    Readings h;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const auto one = x.subspan(i, 1);
        peak.process(one);
        rmsWindow.process(one);
        rmsExp.process(one);
        vu.process(one);
        ppm.process(one);
        h.peak = std::max(h.peak, static_cast<double>(peak.reading()));
        h.rmsWindow = std::max(h.rmsWindow, static_cast<double>(rmsWindow.reading()));
        h.rmsExponential = std::max(h.rmsExponential, static_cast<double>(rmsExp.reading()));
        h.vu = std::max(h.vu, static_cast<double>(vu.reading()));
        h.ppm = std::max(h.ppm, static_cast<double>(ppm.reading()));
    }
    return h;
}

void printReadings(std::string_view name, const Readings& r, double reference = 1.0)
{
    std::cout << "| " << name << " | " << dbText(toDb(r.peak / reference), 2) << " | "
              << dbText(toDb(r.rmsWindow / reference), 2) << " | " << dbText(toDb(r.rmsExponential / reference), 2)
              << " | " << dbText(toDb(r.vu / reference), 2) << " | " << dbText(toDb(r.ppm / reference), 2) << " |\n";
}

void printMeters()
{
    std::cout << "\n## Question 4: the meters\n\nSteady readings after 2 s (dBFS; the VU and PPM calibrated so a "
                 "steady sine reads its peak, the RMS meters reading true RMS):\n\n"
                 "| Signal | sample peak | RMS, 300 ms window | RMS, 300 ms exponential | VU | PPM Type I |\n"
                 "|---|---:|---:|---:|---:|---:|\n";
    const auto n = samples(2.0);
    printReadings("1 kHz sine, 0 dBFS", readAll(sineAt(1000.0, 1.0, n)));
    printReadings("1 kHz sine, -20 dBFS", readAll(sineAt(1000.0, 0.1, n)));
    {
        std::vector<float> square(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            square[i] = (i / 24) % 2 == 0 ? 1.0f : -1.0f;
        }
        printReadings("1 kHz square, 0 dBFS", readAll(square));
    }
    printReadings("12 kHz sine (fs/4), 0 dBFS, samples at 45 degrees",
                  readAll(sineAt(12000.0, 1.0, n, 0.25 * std::numbers::pi)));
    printReadings("100 Hz sine, 0 dBFS", readAll(sineAt(100.0, 1.0, n)));

    // Ballistics.
    std::cout << "\nBursts from silence, the highest reading in dB re the steady tone's reading (the PPM's 5 kHz; the "
                 "others 1 kHz):\n\n| Burst | sample peak | RMS window | RMS exponential | VU | PPM Type I |\n"
                 "|---|---:|---:|---:|---:|---:|\n";
    const Readings steady1k = readAll(sineAt(1000.0, 1.0, n));
    const Readings steady5k = readAll(sineAt(5000.0, 1.0, n));
    for (const double ms : {1.0, 3.0, 5.0, 10.0, 100.0, 300.0})
    {
        auto x = sineAt(1000.0, 1.0, samples(2.0));
        std::fill(x.begin() + static_cast<std::ptrdiff_t>(samples(ms / 1000.0)), x.end(), 0.0f);
        auto h = highestOf(x);
        auto y = sineAt(5000.0, 1.0, samples(2.0));
        std::fill(y.begin() + static_cast<std::ptrdiff_t>(samples(ms / 1000.0)), y.end(), 0.0f);
        h.ppm = highestOf(y).ppm / steady5k.ppm * steady1k.ppm;
        std::cout << "| " << fixed(ms, 0) << " ms |";
        for (const auto& [value, reference] : {
                 std::pair{h.peak, steady1k.peak},
                 std::pair{h.rmsWindow, steady1k.rmsWindow},
                 std::pair{h.rmsExponential, steady1k.rmsExponential},
                 std::pair{h.vu, steady1k.vu},
                 std::pair{h.ppm, steady1k.ppm},
             })
        {
            std::cout << ' ' << dbText(toDb(value / reference), 2) << " |";
        }
        std::cout << '\n';
    }

    // VU: rise to 99% and overshoot; PPM and peak: the return.
    {
        VuMeter vu;
        vu.prepare(rate);
        const auto x = sineAt(1000.0, 1.0, samples(2.0));
        std::size_t rise = 0;
        double highest = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            vu.process(std::span{x}.subspan(i, 1));
            if (rise == 0 && vu.reading() >= 0.99f * steady1k.vu)
            {
                rise = i + 1;
            }
            highest = std::max(highest, static_cast<double>(vu.reading()));
        }
        std::cout << "\nVU: damping " << fixed(vu.dampingRatio(), 4) << ", needle corner " << fixed(vu.cutoffHz(), 3)
                  << " Hz; a 1 kHz tone from silence reaches 99% of its reading at "
                  << fixed(1000.0 * static_cast<double>(rise) / rate, 1) << " ms (IEC 60268-17: 300 ms +-10%) and "
                  << "overshoots by " << fixed(100.0 * ((highest / steady1k.vu) - 1.0), 2)
                  << "% (the standard: 1 to 1.5%).\n";
    }
    for (const bool isPpm : {true, false})
    {
        auto x = sineAt(5000.0, 1.0, samples(4.0));
        std::fill(x.begin() + static_cast<std::ptrdiff_t>(samples(1.0)), x.end(), 0.0f);
        PeakMeter peak;
        PpmMeter ppm;
        peak.prepare(rate);
        ppm.prepare(rate);
        double atStop = 0.0;
        std::size_t fallen = 0;
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            const auto one = std::span{x}.subspan(i, 1);
            peak.process(one);
            ppm.process(one);
            const double v = isPpm ? ppm.reading() : peak.reading();
            if (i + 1 == samples(1.0))
            {
                atStop = v;
            }
            if (i >= samples(1.0) && fallen == 0 && v <= atStop * 0.1)
            {
                fallen = i + 1 - samples(1.0);
            }
        }
        std::cout << (isPpm ? "PPM: attack coefficient " + fixed(ppm.attackCoefficient(), 5) + "; " : "Sample peak: ")
                  << "after a tone stops, 20 dB down in " << fixed(static_cast<double>(fallen) / rate, 3) << " s"
                  << (isPpm ? " (Type I: 1.7 s +-0.3).\n" : " (0.5 s hold, then 11.76 dB/s).\n");
    }

    // The same reading whatever the block split.
    std::cout << "\nThe same reading at the same sample whatever the split: a 2 s render (16 staggered notes of "
                 "the init saw, R 0.3 s, released from 0.5 s, so every meter rises and falls) metered in blocks of 1, "
                 "37, 512 and 4096 (split also at the reading points 0.25, 0.9, 1.2 and 2.0 s), the readings compared "
                 "to the bit against blocks of 4096.\n\n"
                 "| Meter | identical | largest difference |\n|---|---|---:|\n";
    std::vector<EngineEvent> chord;
    for (int k = 0; k < 16; ++k)
    {
        const auto i = static_cast<std::size_t>(k);
        chord.push_back(noteOn(i * 701, 36 + (3 * k)));
        chord.push_back(noteOff(samples(0.5) + (i * 997), 36 + (3 * k)));
    }
    Patch plucked = initSaw;
    plucked.release = 0.3f;
    Setup const s{.patch = plucked, .length = samples(2.0)};
    const auto source = render(s, sorted(chord));
    const std::array checkpoints{samples(0.25), samples(0.9), samples(1.2), samples(2.0)};
    auto meterRun = [&](auto makeMeter, std::size_t block)
    {
        auto meter = makeMeter();
        std::vector<float> readings;
        std::size_t from = 0;
        for (const auto point : checkpoints)
        {
            while (from < point)
            {
                const auto length = std::min(block, point - from);
                meter.process(source.out.channel(0).subspan(from, length));
                from += length;
            }
            readings.push_back(meter.reading());
        }
        return readings;
    };
    auto const compare = [&](std::string_view name, auto makeMeter)
    {
        const auto reference = meterRun(makeMeter, 4096);
        bool same = true;
        double worst = 0.0;
        for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{512}})
        {
            const auto r = meterRun(makeMeter, block);
            for (std::size_t i = 0; i < r.size(); ++i)
            {
                same = same && std::bit_cast<std::uint32_t>(r[i]) == std::bit_cast<std::uint32_t>(reference[i]);
                worst = std::max(worst, std::abs(toDb(r[i]) - toDb(reference[i])));
            }
        }
        std::cout << "| " << name << " | " << (same ? "yes" : "no") << " | " << fixed(worst, 3) << " dB |\n";
    };
    compare("sample peak, hold and fall",
            []
            {
                PeakMeter m;
                m.prepare(rate);
                return m;
            });
    compare("block peak (per block)",
            []
            {
                BlockPeakMeter m;
                m.prepare(rate);
                return m;
            });
    compare("RMS, 300 ms window",
            []
            {
                RmsWindowMeter m;
                m.prepare(rate);
                return m;
            });
    compare("RMS, 300 ms exponential",
            []
            {
                RmsExponentialMeter m;
                m.prepare(rate);
                return m;
            });
    compare("VU",
            []
            {
                VuMeter m;
                m.prepare(rate);
                return m;
            });
    compare("PPM Type I",
            []
            {
                PpmMeter m;
                m.prepare(rate);
                return m;
            });

    // Where in a block a lone peak falls, for the block peak.
    {
        double low = 1e9;
        double high = -1e9;
        for (std::size_t at = 0; at < 512; at += 37)
        {
            std::vector<float> x(samples(1.0), 0.0f);
            x[samples(0.5) + at] = 1.0f;
            BlockPeakMeter b;
            b.prepare(rate, 0.0f);
            for (std::size_t i = 0; i < x.size(); i += 512)
            {
                b.process(std::span{x}.subspan(i, std::min<std::size_t>(512, x.size() - i)));
            }
            low = std::min(low, toDb(b.reading()));
            high = std::max(high, toDb(b.reading()));
        }
        std::cout << "\nThe block peak with no hold, a lone full-scale sample at different places in a 512-sample "
                     "block, read 0.5 s later: "
                  << fixed(low, 2) << " to " << fixed(high, 2) << " dB (the per-sample meter reads the same for "
                  << "every place).\n";
    }
}

// --- Question 5: levels, and no clipping -------------------------------------------------

struct Level
{
    double peak = 0.0;
    double rms = 0.0;
    std::size_t nonFinite = 0;
    std::size_t subnormal = 0;
};

Level levelOf(const Buffer& b, std::size_t from, std::size_t n)
{
    Level l;
    l.peak = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < b.numSamples(); ++i)
        {
            const float v = b.channel(c)[i];
            l.nonFinite += std::isfinite(v) ? 0 : 1;
            l.subnormal += std::fpclassify(v) == FP_SUBNORMAL ? 1 : 0;
            if (i >= from && i < from + n)
            {
                l.peak = std::max(l.peak, static_cast<double>(std::abs(v)));
            }
        }
    }
    l.rms = stereoRms(b, from, n);
    return l;
}

std::vector<EngineEvent> chordEvents(int notes, std::size_t offAt)
{
    std::vector<EngineEvent> e;
    constexpr std::array<int, 16> keys{36, 43, 48, 52, 55, 60, 64, 67, 71, 72, 76, 79, 83, 84, 88, 91};
    for (int k = 0; k < notes; ++k)
    {
        e.push_back(noteOn(0, keys[static_cast<std::size_t>(k)]));
        e.push_back(noteOff(offAt, keys[static_cast<std::size_t>(k)]));
    }
    return sorted(e);
}

void printLevels()
{
    std::cout << "\n## Question 5: absolute levels, and no output clipping\n\n"
                 "Mix linear10 (internal 10 = unity) and Main Volume 1 unless stated, the decided voice; peak and "
                 "stereo RMS (dBFS) from 0.1 s to 1 s, held; then released (R 0.3 s) and rendered to 3 s. "
                 "Non-finite and subnormal samples counted over the whole render.\n\n"
                 "| Case | peak | RMS | non-finite | subnormal |\n|---|---:|---:|---:|---:|\n";
    Patch releasing = initSaw;
    releasing.release = 0.3f;
    Patch worst = worstSaw;
    worst.release = 0.3f;
    Patch worstDriven = worst;
    worstDriven.drive = 16.0f;
    Patch worstResonant = worst;
    worstResonant.resonance = 10.0f;
    struct Case
    {
        std::string_view name;
        Patch patch;
        int notes;
        float mix;
        int key = 45;
    };
    std::vector<Buffer> renders;
    for (const auto& c : {
             Case{.name = "init patch, one note (A2)", .patch = releasing, .notes = 1, .mix = 10.0f},
             Case{
                 .name = "init patch, one note (A2), Mix at the top (15)",
                 .patch = releasing,
                 .notes = 1,
                 .mix = 15.0f,
             },
             Case{.name = "init patch, a 16-voice chord", .patch = releasing, .notes = 16, .mix = 10.0f},
             Case{
                 .name = "worst: 4 x 8 saws, both parts, cutoff open, 16 voices, Mix 15",
                 .patch = worst,
                 .notes = 16,
                 .mix = 15.0f,
             },
             Case{.name = "worst, drive x16 (F8's top)", .patch = worstDriven, .notes = 16, .mix = 15.0f},
             Case{.name = "worst, Resonance 10 on both knobs (k 6)", .patch = worstResonant, .notes = 16, .mix = 15.0f},
         })
    {
        Setup const s{.patch = c.patch, .mixKnobs = {c.mix, c.mix}, .length = samples(3.0)};
        const auto events = c.notes == 1 ? std::vector{noteOn(0, c.key), noteOff(samples(1.0), c.key)}
                                         : chordEvents(c.notes, samples(1.0));
        const auto r = render(s, events);
        const auto l = levelOf(r.out, samples(0.1), samples(0.9));
        std::cout << "| " << c.name << " | " << dbText(toDb(l.peak), 2) << " | " << dbText(toDb(l.rms), 2) << " | "
                  << l.nonFinite << " | " << l.subnormal << " |\n";
        renders.push_back(r.out);
    }

    // The output scales exactly with Main Volume: nothing clips anywhere.
    {
        Setup s{.patch = worst, .mixKnobs = {15.0f, 15.0f}, .length = samples(1.0)};
        const auto events = chordEvents(16, samples(2.0));
        const auto unity = render(s, events);
        double worstRatio = 0.0;
        double peak = 0.0;
        for (const float gain : {10.0f, 1000.0f})
        {
            s.mainVolume = gain;
            const auto loud = render(s, events);
            peak = std::max(peak, peakOf(loud.out));
            for (std::size_t c = 0; c < 2; ++c)
            {
                for (std::size_t i = 0; i < s.length; ++i)
                {
                    const double expected = static_cast<double>(gain) * unity.out.channel(c)[i];
                    if (std::abs(expected) > 1e-6)
                    {
                        worstRatio = std::max(worstRatio, std::abs((loud.out.channel(c)[i] - expected) / expected));
                    }
                }
            }
        }
        std::cout << "\nThe worst case at Main Volume x10 and x1000 against x1, sample by sample: the largest relative "
                     "difference is "
                  << std::scientific << std::setprecision(2) << worstRatio << std::defaultfloat << " (one rounding), "
                  << "at a peak of " << dbText(toDb(peak), 1)
                  << " dBFS: no stage clips or limits the output; it is a float, as loud as the patch makes it.\n";
    }
    std::size_t changed = 0;
    for (auto& r : renders)
    {
        for (std::size_t c = 0; c < 2; ++c)
        {
            std::vector<float> copy(r.channel(c).begin(), r.channel(c).end());
            guardOutput(copy);
            for (std::size_t i = 0; i < copy.size(); ++i)
            {
                changed +=
                    std::bit_cast<std::uint32_t>(copy[i]) != std::bit_cast<std::uint32_t>(r.channel(c)[i]) ? 1 : 0;
            }
        }
    }
    std::cout << "An output guard (non-finite and subnormal samples to 0) changes " << changed
              << " samples of these renders.\n";
}

// --- Real-time terms ----------------------------------------------------------------------

void printExactness()
{
    std::cout << "\n## Real-time terms\n\n"
                 "A 2 s sequence: the pad at Polyphony 4 with steals, Mix A and B automated every 300 samples at "
                 "odd offsets, Main Volume jumps, plus the modulation stated; host blocks of 1, 37 and 4096. "
                 "Identical to the bit against 4096; allocations while processing.\n\n"
                 "| Placement | modulation | identical (1, 37) | allocations |\n|---|---|---|---:|\n";
    std::vector<EngineEvent> events = identitySequence();
    for (std::size_t k = 1; k < 300; ++k)
    {
        events.push_back(mixEvent((k * 300) + 7, k % 2, static_cast<float>((k * 7) % 16)));
    }
    events.push_back(mainEvent(samples(0.6) + 3, 0.5f));
    events.push_back(mainEvent(samples(1.3) + 11, 2.0f));
    events = sorted(events);
    for (const auto placement : placements)
    {
        for (const auto& [name, modulation] : {
                 std::pair{"none", MixModulationSpec{}},
                 std::pair{"LFO 40 Hz +-5",
                           MixModulationSpec{.kind = MixModulation::lfo, .amount = 5.0f, .rateHz = 40.0f}},
                 std::pair{"pluck +10, 20 ms",
                           MixModulationSpec{.kind = MixModulation::pluck, .amount = 10.0f, .decaySeconds = 0.02f}},
             })
        {
            Setup s{.patch = padPatch, .polyphony = 4, .length = samples(2.0)};
            s.mix.placement = placement;
            s.mix.modulation = modulation;
            s.hostBlock = 4096;
            const auto reference = render(s, events);
            std::size_t allocations = reference.allocations;
            int same = 0;
            for (const std::size_t block : {std::size_t{1}, std::size_t{37}})
            {
                s.hostBlock = block;
                const auto r = render(s, events);
                same += sameBits(r.out, reference.out) ? 1 : 0;
                allocations += r.allocations;
            }
            std::cout << "| " << placementName(placement) << " | " << name << " | " << same << " of 2 | " << allocations
                      << " |\n";
        }
    }
}

} // namespace

TEST_CASE("Mixer: the candidates, measured", "[.][mixer-report]")
{
    printIdentity();
    printLaws();
    printZipper();
    printCrossing();
    printMainVolume();
    printMeters();
    printLevels();
    printExactness();
    SUCCEED();
}
