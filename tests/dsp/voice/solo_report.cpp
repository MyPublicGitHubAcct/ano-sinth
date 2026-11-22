// The part-solo and skipping report (docs/dsp/part-solo-and-skipping.md, M1.4
// task 5, stage 1): measures the candidates in solo_candidates.hpp over the
// real dsp::VoiceAllocator:
//
// - the prototype against the library: SkipVoice with nothing switched on
//   is dsp::Voice to the bit;
// - Solo: its two readings on a part crossing into the other filter (AB),
//   and the clicks of Solo on and off, of Part select changing under Solo,
//   and of Solo on a release tail, for a gain ramped over 10 ms, an instant
//   gain, and a routing change (filters.md's click: energy above 1 kHz in a
//   Hann 4096, dB re a unit step);
// - each V13 case under each skip candidate: identical to not skipping or
//   not, by how much where it comes back, and how much was skipped;
// - an oscillator's phase through a skip (advanced or frozen);
// - when the test is made (per chunk, with hysteresis, at the note-on);
// - bounds on the modulation (a stand-in for M1.5: an LFO and a pluck on
//   Mix B), conservative and tight;
// - the mono path: exact against stereo, taken and left where it should be;
// - the real-time terms: the same output at host blocks of 1, 37 and 4096,
//   no allocation while processing.
//
// Hidden from the default run; run it in an optimised build:
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[solo-report]"
//
// CPU: the "[solo]" benchmarks (tools/bench.sh "[solo]").

#include "solo_candidates.hpp"

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
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::solo;

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
        return db < 0.0 ? "exact 0" : "inf";
    }
    return fixed(db, digits);
}

double toDb(double gain)
{
    return gain > 0.0 ? 20.0 * std::log10(gain) : -std::numeric_limits<double>::infinity();
}

std::string percent(std::uint64_t part, std::uint64_t whole)
{
    if (whole == 0)
    {
        return "-";
    }
    return fixed(100.0 * static_cast<double>(part) / static_cast<double>(whole), 1) + "%";
}

const dsp::MinBlepTable& table()
{
    return d6::minBlepTable64();
}

// --- Patches ------------------------------------------------------------------------------

struct Patch
{
    std::array<dsp::Waveform, 4> waves{dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw};
    std::array<int, 4> voices{1, 0, 0, 0};
    std::array<float, 4> volume{1.0f, 1.0f, 1.0f, 1.0f};
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
    float drive = 1.0f;
};

/// The init patch as far as the evidence goes (V9): A1 one saw, both filters
/// on their own parts, LP24 at 4,677 Hz, gates. Filter B runs on part B's
/// silence in dsp::Voice.
const Patch initPatch{};
const Patch initNone{.inputs = {dsp::FilterInput::own, dsp::FilterInput::none}};
/// A1 and B1 saws (B1 a fifth up), each on its own filter.
const Patch twoParts{.voices = {1, 0, 1, 0}};
/// The same, resonant (k 3.6, below self-oscillation at 4): a filter that
/// rings.
const Patch twoResonant{.voices = {1, 0, 1, 0}, .resonance = 6.0f};
/// Both parts into filter A (AB), filter B on its own part.
const Patch crossing{.voices = {1, 0, 1, 0}, .inputs = {dsp::FilterInput::both, dsp::FilterInput::own}};
/// Quiet sines through LP24 (near linear at Volume 0.25), for clicks: no
/// energy above 1 kHz of their own.
const Patch quietSines{
    .waves = {dsp::Waveform::sine, dsp::Waveform::sine, dsp::Waveform::sine, dsp::Waveform::sine},
    .voices = {1, 0, 1, 0},
    .volume = {0.25f, 0.25f, 0.25f, 0.25f},
    .release = {0.5f, 0.5f},
};
/// 4 x 8 unison saws, spread (Stereo 1), on their own filters.
const Patch worst{.voices = {8, 8, 8, 8}, .detune = 0.548f, .stereo = 1.0f, .retrigger = false, .cutoff = 10.0f};
/// The same, centred (Stereo 0): the mono path's case.
const Patch worstCentred{.voices = {8, 8, 8, 8}, .detune = 0.548f, .retrigger = false, .cutoff = 10.0f};
/// A pad: A1, A2 and B1 unison into filter A on AB, resonant, with a
/// release; filter B on "--".
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
const Patch padSpread = []
{
    Patch p = pad;
    p.stereo = 1.0f;
    return p;
}();
const Patch noisePatch{.waves = {dsp::Waveform::noise, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw}};

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
        v.setFilterDrive(e, p.drive);
        v.setFilterBypassed(e, p.bypass[e]);
    }
    v.drift().setDepthCents(1.5f);
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
    SkipContext context{};
    std::array<float, 2> mixKnobs{10.0f, 10.0f};
    bool solo = false;
    std::size_t selected = 0;
    bool soloSmooth = true;
    std::size_t polyphony = 16;
    std::size_t length = samples(1.0);
    std::size_t hostBlock = 512;
};

struct Render
{
    Buffer out{2, 1};
    std::size_t allocations = 0;
    SkipStats stats{};
};

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
    auto engine = std::make_unique<SoloEngine>(table(), s.context);
    engine->voices().forEachVoice([&](SoloEngine::Voice& v) { apply(v, s.patch); });
    engine->voices().setPolyphony(s.polyphony);
    engine->filterKnobs().set(s.patch.cutoff, s.patch.resonance);
    engine->setMix(0, s.mixKnobs[0]);
    engine->setMix(1, s.mixKnobs[1]);
    engine->setSoloSmoothing(s.soloSmooth);
    engine->prepare({.sampleRate = rate, .maxBlockSize = s.hostBlock, .numChannels = 2});
    engine->setSolo(s.solo, s.selected);
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
    r.stats = engine->stats();
    return r;
}

/// The library (dsp::VoiceAllocator over dsp::Voice, seed 1 as the engine's)
/// on the same patch and note events, Mix at unity.
Buffer renderLibrary(const Setup& s, std::span<const EngineEvent> events)
{
    auto alloc = std::make_unique<dsp::VoiceAllocator<dsp::Voice<>>>(1, table());
    alloc->forEachVoice([&](dsp::Voice<>& v) { apply(v, s.patch); });
    alloc->setPolyphony(s.polyphony);
    FilterKnobs knobs;
    knobs.set(s.patch.cutoff, s.patch.resonance);
    std::array<dsp::LinearSmoother, 2> mix{};
    for (auto& m : mix)
    {
        m.setRampTime(0.01f);
        m.prepare({.sampleRate = rate, .maxBlockSize = s.hostBlock, .numChannels = 2});
        m.setCurrentAndTarget(10.0f);
    }
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
        auto k = knobs.knobs();
        k.mix = {mix.data(), mix.data() + 1};
        k.time = start;
        alloc->process(piece, voiceEvents, k);
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

/// The last sample at which a and b differ (in either channel), or none.
std::ptrdiff_t lastDifference(const Buffer& a, const Buffer& b)
{
    std::ptrdiff_t last = -1;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < a.numSamples(); ++i)
        {
            if (std::bit_cast<std::uint32_t>(a.channel(c)[i]) != std::bit_cast<std::uint32_t>(b.channel(c)[i]))
            {
                last = std::max(last, static_cast<std::ptrdiff_t>(i));
            }
        }
    }
    return last;
}

std::ptrdiff_t firstDifference(const Buffer& a, const Buffer& b)
{
    std::ptrdiff_t first = -1;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < a.numSamples(); ++i)
        {
            if (std::bit_cast<std::uint32_t>(a.channel(c)[i]) != std::bit_cast<std::uint32_t>(b.channel(c)[i]))
            {
                if (first < 0 || std::cmp_less(i, first))
                {
                    first = static_cast<std::ptrdiff_t>(i);
                }
                break;
            }
        }
    }
    return first;
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

Buffer difference(const Buffer& a, const Buffer& b)
{
    Buffer d{2, a.numSamples()};
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < a.numSamples(); ++i)
        {
            d.channel(c)[i] = a.channel(c)[i] - b.channel(c)[i];
        }
    }
    return d;
}

/// The difference's RMS over n samples from `from`, dB re the reference's.
double differenceRe(const Buffer& candidate, const Buffer& reference, std::size_t from, std::size_t n)
{
    const auto d = difference(candidate, reference);
    return toDb(stereoRms(d, from, n) / stereoRms(reference, from, n));
}

/// Energy above 1 kHz in a Hann 4096 centred 32 samples after `at`, both
/// channels, as a fraction of an instant unit step's.
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

double clickDb(const Buffer& b, std::size_t at)
{
    const double e = clickEnergy(b.channel(0), b.channel(1), at);
    return e > 0.0 ? 10.0 * std::log10(e) : -std::numeric_limits<double>::infinity();
}

void row(std::initializer_list<std::string> cells)
{
    std::cout << "|";
    for (const auto& c : cells)
    {
        std::cout << " " << c << " |";
    }
    std::cout << "\n";
}

std::string_view filterName(FilterSkip f)
{
    switch (f)
    {
    case FilterSkip::never:
        return "nothing skipped";
    case FilterSkip::atRest:
        return "atRest + slots";
    case FilterSkip::freeze:
        return "freeze + slots";
    case FilterSkip::clear:
        return "clear + slots";
    }
    return "";
}

std::string_view timingName(SkipTiming t)
{
    switch (t)
    {
    case SkipTiming::perChunk:
        return "per chunk";
    case SkipTiming::perSegment:
        return "per segment";
    case SkipTiming::hysteresis:
        return "per segment, hysteresis 128";
    case SkipTiming::noteOn:
        return "at the note-on";
    }
    return "";
}

std::string_view leftRightText(bool checked, bool equal)
{
    if (!checked)
    {
        return "-";
    }
    return equal ? "yes" : "no";
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
                 "SkipVoice with nothing switched on (FilterSkip::never, no oscillator skip, no mono path) through\n"
                 "VoiceAllocator against dsp::Voice, Polyphony 4 with two steals, 60,000 samples:\n\n";
    row({"Patch", "host 512", "host 37"});
    row({"---", "---", "---"});
    const std::array<std::pair<std::string_view, const Patch*>, 6> patches{
        {
            {"init", &initPatch},
            {"init, filter B --", &initNone},
            {"A1 and B1, own filters", &twoParts},
            {"crossing (A: AB)", &crossing},
            {"pad (AB, resonant, release)", &pad},
            {"worst (4 x 8, spread)", &worst},
        },
    };
    int same = 0;
    int total = 0;
    for (const auto& [name, patch] : patches)
    {
        std::array<std::string, 2> cells;
        for (std::size_t b = 0; b < 2; ++b)
        {
            const Setup s{.patch = *patch, .polyphony = 4, .length = 60000, .hostBlock = b == 0 ? 512U : 37U};
            const auto events = identitySequence();
            const auto proto = render(s, events);
            const auto lib = renderLibrary(s, events);
            const bool ok = sameBits(proto.out, lib);
            same += ok ? 1 : 0;
            ++total;
            cells[b] = ok ? "identical" : "differs by " + fixed(maxAbsDifference(proto.out, lib), 9);
        }
        row({std::string{name}, cells[0], cells[1]});
    }
    std::cout << "\n" << same << " of " << total << " identical to the bit.\n";
}

// --- Solo: the readings -------------------------------------------------------------------

void printSoloReadings()
{
    std::cout << "\n## Solo: its readings on a crossing part\n\n"
                 "Filter A on AB (A1 a saw at A2, B1 a saw a fifth up), filter B on B. Solo on from the start\n"
                 "(gain mode, settled). The output's RMS over 0.1-0.9 s, and B1's share: the RMS of the output\n"
                 "less the same render with B1 at Voices 0, dB re the whole output without Solo.\n\n";
    row({"Reading", "Solo", "output", "B1 in it"});
    row({"---", "---", "---:", "---:"});
    const auto events = sorted({noteOn(0, 45)});
    const auto from = samples(0.1);
    const auto n = samples(0.8);
    Patch withoutB1 = crossing;
    withoutB1.voices = {1, 0, 0, 0};
    const auto reference = render({.patch = crossing}, events).out;
    const double ref = stereoRms(reference, from, n);
    row({
        "(no Solo)",
        "off",
        "0.0 dB",
        dbText(toDb(stereoRms(difference(reference, render({.patch = withoutB1}, events).out), from, n) / ref)),
    });
    for (const auto reading : {SoloReading::filterOutput, SoloReading::partSources})
    {
        for (const std::size_t selected : {0U, 1U})
        {
            Setup s{.patch = crossing, .solo = true, .selected = selected};
            s.context.soloReading = reading;
            const auto out = render(s, events).out;
            Setup w = s;
            w.patch = withoutB1;
            const auto noB1 = render(w, events).out;
            row({
                reading == SoloReading::filterOutput ? "filter output (Syntorial)" : "part sources",
                selected == 0 ? "A" : "B",
                dbText(toDb(stereoRms(out, from, n) / ref)),
                dbText(toDb(stereoRms(difference(out, noB1), from, n) / ref)),
            });
        }
    }

    // Solo A on two parts on their own filters is exactly part B removed.
    Patch onlyA = twoParts;
    onlyA.voices = {1, 0, 0, 0};
    const auto soloA = render({.patch = twoParts, .solo = true, .selected = 0}, events).out;
    const auto noB = render({.patch = onlyA}, events).out;
    std::cout << "\nOwn filters, Solo A against B1 at Voices 0: "
              << (sameBits(soloA, noB) ? "identical to the bit"
                                       : "differs by " + fixed(maxAbsDifference(soloA, noB), 9))
              << ".\n";
}

// --- Solo: clicks -------------------------------------------------------------------------

struct SoloMode
{
    std::string_view name;
    bool routing = false;
    bool smooth = true;
};

void printSoloClicks()
{
    std::cout << "\n## Solo: clicks\n\n"
                 "A1 and B1 sines at A3 (220 Hz) and a fifth up, Volume 0.25, through LP24 at 4,677 Hz, own\n"
                 "filters, release 0.5 s. A note held from 0; Solo on (A selected) at 0.5 s + 7; Part select to B\n"
                 "at 1.0 s + 13; Solo off at 1.5 s + 3; note-off at 2.0 s; Solo on (A) at 2.2 s + 11, in the\n"
                 "release. Click: energy above 1 kHz around the event, dB re a unit step.\n\n";
    const std::array<std::size_t, 4> at{samples(0.5) + 7, samples(1.0) + 13, samples(1.5) + 3, samples(2.2) + 11};
    const auto events = sorted({
        noteOn(0, 57),
        soloEvent(at[0], true),
        selectEvent(at[1], 1),
        soloEvent(at[2], false),
        noteOff(samples(2.0), 57),
        selectEvent(samples(2.1), 0),
        soloEvent(at[3], true),
    });
    row({"Solo", "on", "Part select", "off", "on, in a release"});
    row({"---", "---:", "---:", "---:", "---:"});
    const std::array<SoloMode, 3> modes{
        {
            {.name = "gain, ramped 10 ms", .routing = false, .smooth = true},
            {.name = "gain, instant", .routing = false, .smooth = false},
            {.name = "routing (at the grid point, from rest)", .routing = true, .smooth = true},
        },
    };
    for (const auto& mode : modes)
    {
        Setup s{.patch = quietSines, .soloSmooth = mode.smooth, .length = samples(2.6)};
        s.context.soloByRouting = mode.routing;
        const auto out = render(s, events).out;
        row({
            std::string{mode.name},
            dbText(clickDb(out, at[0])),
            dbText(clickDb(out, at[1])),
            dbText(clickDb(out, at[2])),
            dbText(clickDb(out, at[3])),
        });
    }
    const Setup none{.patch = quietSines, .length = samples(2.6)};
    const auto plain = render(none, sorted({noteOn(0, 57), noteOff(samples(2.0), 57)})).out;
    row({
        "(no Solo: the floor)",
        dbText(clickDb(plain, at[0])),
        dbText(clickDb(plain, at[1])),
        dbText(clickDb(plain, at[2])),
        dbText(clickDb(plain, at[3])),
    });
}

// --- Skipping: the V13 cases --------------------------------------------------------------

struct Candidate
{
    std::string_view name;
    FilterSkip filter = FilterSkip::never;
    bool oscillators = false;
    bool advance = true;
};

constexpr std::array<Candidate, 5> candidates{
    {
        {.name = "atRest", .filter = FilterSkip::atRest, .oscillators = false},
        {.name = "atRest + slots", .filter = FilterSkip::atRest, .oscillators = true},
        {.name = "freeze + slots", .filter = FilterSkip::freeze, .oscillators = true},
        {.name = "clear + slots", .filter = FilterSkip::clear, .oscillators = true},
        {.name = "clear + slots, phase frozen", .filter = FilterSkip::clear, .oscillators = true, .advance = false},
    },
};

struct Case
{
    std::string_view name;
    Setup setup;
    std::vector<EngineEvent> events;
    std::size_t resume = 0; ///< where the skipped part comes back (0: it doesn't)
};

std::vector<Case> skipCases()
{
    const auto chord = [](std::vector<EngineEvent> more)
    {
        std::vector<EngineEvent> e{noteOn(0, 45), noteOn(0, 52), noteOn(0, 57)};
        e.insert(e.end(), more.begin(), more.end());
        return sorted(e);
    };
    const auto back = samples(0.5) + 7;
    Patch volumeB = twoParts;
    volumeB.volume = {1.0f, 1.0f, 0.0f, 1.0f};
    Patch zeroHold = twoParts;
    zeroHold.decay = {0.0f, 0.05f};
    zeroHold.sustain = {1.0f, 0.0f};
    Patch crossingHold = crossing;
    crossingHold.decay = {0.0f, 0.05f};
    crossingHold.sustain = {1.0f, 0.0f};
    return {
        {
            .name = "init (B at Voices 0, filter B on B)",
            .setup = {.patch = initPatch},
            .events = chord({}),
            .resume = 0,
        },
        {.name = "filter B --", .setup = {.patch = initNone}, .events = chord({}), .resume = 0},
        {
            .name = "every oscillator at Voices 0",
            .setup = {.patch = Patch{.voices = {0, 0, 0, 0}}},
            .events = chord({}),
            .resume = 0,
        },
        {
            .name = "B1 at Volume 0, then 1 at 0.5 s",
            .setup = {.patch = volumeB},
            .events = chord({volumeEvent(back, 2, 1.0f)}),
            .resume = back,
        },
        {
            .name = "Mix B 0, then 10 at 0.5 s",
            .setup = {.patch = twoParts, .mixKnobs = {10.0f, 0.0f}},
            .events = chord({mixEvent(back, 1, 10.0f)}),
            .resume = back,
        },
        {
            .name = "the same, resonant (k 3.6)",
            .setup = {.patch = twoResonant, .mixKnobs = {10.0f, 0.0f}},
            .events = chord({mixEvent(back, 1, 10.0f)}),
            .resume = back,
        },
        {
            .name = "Mix B 10, to 0 at 0.25 s, back to 10 at 0.5 s",
            .setup = {.patch = twoParts},
            .events = chord({mixEvent(samples(0.25) + 3, 1, 0.0f), mixEvent(back, 1, 10.0f)}),
            .resume = back,
        },
        {
            .name = "the same, resonant (k 3.6)",
            .setup = {.patch = twoResonant},
            .events = chord({mixEvent(samples(0.25) + 3, 1, 0.0f), mixEvent(back, 1, 10.0f)}),
            .resume = back,
        },
        {
            .name = "Solo A, then off at 0.5 s",
            .setup = {.patch = twoParts, .solo = true, .selected = 0},
            .events = chord({soloEvent(back, false)}),
            .resume = back,
        },
        {
            .name = "env B held at sustain 0; retriggered at 0.5 s (Polyphony 1)",
            .setup = {.patch = zeroHold, .polyphony = 1},
            .events = sorted({noteOn(0, 45), noteOn(back, 48)}),
            .resume = back,
        },
        {
            .name = "crossing, env B held at sustain 0",
            .setup = {.patch = crossingHold},
            .events = chord({}),
            .resume = 0,
        },
    };
}

void printSkipCases()
{
    std::cout << "\n## Skipping: each case against not skipping\n\n"
                 "A three-note chord (A2, E3, A3) held 1 s, through VoiceAllocator, host blocks of 512; drift on.\n"
                 "The reference is the prototype with nothing skipped (dsp::Voice to the bit). Filters: the share\n"
                 "of filter samples not solved; slots: of slot samples not rendered. Where the part comes back:\n"
                 "the difference's RMS over the 50 ms after, dB re the reference's; the difference's click (energy\n"
                 "above 1 kHz around the return, dB re a unit step) beside the reference's own there; and the last\n"
                 "sample that differs, in samples\n"
                 "after the event.\n\n";
    row({
        "Case",
        "Candidate",
        "Filters skipped",
        "Slots skipped",
        "Identical",
        "After the return",
        "Click of the difference",
        "The reference's own click",
        "Differs until",
    });
    row({"---", "---", "---:", "---:", "---", "---:", "---:", "---:", "---:"});
    for (const auto& c : skipCases())
    {
        const auto reference = render(c.setup, c.events).out;
        for (const auto& k : candidates)
        {
            Setup s = c.setup;
            s.context.filterSkip = k.filter;
            s.context.skipOscillators = k.oscillators;
            s.context.advancePhase = k.advance;
            const auto r = render(s, c.events);
            const bool same = sameBits(r.out, reference);
            std::string after = "-";
            std::string until = "-";
            std::string click = "-";
            std::string own = "-";
            if (!same && c.resume > 0)
            {
                own = dbText(clickDb(reference, c.resume));
                after = dbText(differenceRe(r.out, reference, c.resume, samples(0.05)));
                click = dbText(clickDb(difference(r.out, reference), c.resume));
                const auto last = lastDifference(r.out, reference);
                until = std::cmp_greater_equal(last, c.resume)
                            ? std::to_string(last - static_cast<std::ptrdiff_t>(c.resume))
                            : "before";
                if (last == static_cast<std::ptrdiff_t>(reference.numSamples()) - 1)
                {
                    until = "the end";
                }
            }
            else if (!same)
            {
                after = "max " + fixed(maxAbsDifference(r.out, reference), 7);
                until = "from " + std::to_string(firstDifference(r.out, reference));
            }
            row({
                std::string{c.name},
                std::string{k.name},
                percent(r.stats.filterSkipped, r.stats.filterSamples),
                percent(r.stats.slotSkipped, r.stats.slotSamples),
                same ? "yes" : "no",
                after,
                click,
                own,
                until,
            });
        }
    }
}

/// An oscillator's phase through a skip, isolated: both filters bypassed.
void printOscillatorPhase()
{
    std::cout << "\n## An oscillator through a skip\n\n"
                 "Both filters bypassed; A1 a saw, B1 a saw (free-running, Retrig off, or Retrig on) at Volume 0\n"
                 "from the note-on, raised to 1 at 0.5 s + 7 (its gain ramps over 10 ms). B1's slot is skipped\n"
                 "while its gain is settled at 0. The difference from not skipping after the return:\n\n";
    row({"B1", "Phase through the skip", "Peak difference", "RMS over 50 ms, re the reference", "Differs until"});
    row({"---", "---", "---:", "---:", "---:"});
    const auto back = samples(0.5) + 7;
    const auto events = sorted({noteOn(0, 45), volumeEvent(back, 2, 1.0f)});
    for (const bool retrig : {false, true})
    {
        Patch p = twoParts;
        p.bypass = {true, true};
        p.volume = {1.0f, 1.0f, 0.0f, 1.0f};
        p.retrigger = retrig;
        const auto reference = render({.patch = p}, events).out;
        for (const bool advance : {true, false})
        {
            Setup s{.patch = p};
            s.context.filterSkip = FilterSkip::atRest;
            s.context.skipOscillators = true;
            s.context.advancePhase = advance;
            const auto out = render(s, events).out;
            const auto last = lastDifference(out, reference);
            std::string until = last < 0 ? "identical" : std::to_string(last - static_cast<std::ptrdiff_t>(back));
            if (last == static_cast<std::ptrdiff_t>(reference.numSamples()) - 1)
            {
                until = "the end";
            }
            row({
                retrig ? "Retrig on" : "free-running",
                advance ? "advanced (a jump at the return)" : "frozen",
                fixed(maxAbsDifference(out, reference), 6),
                dbText(differenceRe(out, reference, back, samples(0.05))),
                until,
            });
        }
    }
}

// --- When the test is made ----------------------------------------------------------------

void printTiming()
{
    std::cout << "\n## When the test is made\n\n"
                 "A1 and B1 saws on their own filters, a chord held 2 s. (1) Mix B 0, raised to 10 at 0.5 s + 7.\n"
                 "(2) Mix B at 0 with a free 2 Hz LFO of +-5 on it (tight bound). The share of filter samples\n"
                 "skipped, the number of returns (a filter skipped for its gain, then solved), and the\n"
                 "difference from not skipping over the whole render, dB re the reference.\n\n";
    row({"Sequence", "Candidate", "Timing", "Filters skipped", "Returns", "Identical", "Difference"});
    row({"---", "---", "---", "---:", "---:", "---", "---:"});
    const auto back = samples(0.5) + 7;
    const auto chordAt = [](std::vector<EngineEvent> more)
    {
        std::vector<EngineEvent> e{noteOn(0, 45), noteOn(0, 52), noteOn(0, 57)};
        e.insert(e.end(), more.begin(), more.end());
        return sorted(e);
    };
    struct Sequence
    {
        std::string_view name;
        Setup setup;
        std::vector<EngineEvent> events;
    };
    Setup lfo{.patch = twoParts, .mixKnobs = {10.0f, 0.0f}, .length = samples(2.0)};
    lfo.context.modulation = {.kind = ModulationKind::lfo, .amount = 5.0f, .rateHz = 2.0f};
    lfo.context.bound = BoundKind::tight;
    const std::array<Sequence, 2> sequences{
        {
            {
                .name = "(1) Mix B 0 -> 10",
                .setup = {.patch = twoParts, .mixKnobs = {10.0f, 0.0f}, .length = samples(2.0)},
                .events = chordAt({mixEvent(back, 1, 10.0f)}),
            },
            {.name = "(2) LFO on Mix B at 0", .setup = lfo, .events = chordAt({})},
        },
    };
    for (const auto& seq : sequences)
    {
        const auto reference = render(seq.setup, seq.events).out;
        const double ref = stereoRms(reference, 0, reference.numSamples());
        for (const auto filter : {FilterSkip::atRest, FilterSkip::freeze, FilterSkip::clear})
        {
            for (const auto timing :
                 {SkipTiming::perChunk, SkipTiming::perSegment, SkipTiming::hysteresis, SkipTiming::noteOn})
            {
                Setup s = seq.setup;
                s.context.filterSkip = filter;
                s.context.skipOscillators = true;
                s.context.timing = timing;
                const auto r = render(s, seq.events);
                const bool same = sameBits(r.out, reference);
                const double d = stereoRms(difference(r.out, reference), 0, reference.numSamples()) / ref;
                row({
                    std::string{seq.name},
                    std::string{filterName(filter)},
                    std::string{timingName(timing)},
                    percent(r.stats.filterSkipped, r.stats.filterSamples),
                    std::to_string(r.stats.filterResumes),
                    same ? "yes" : "no",
                    same ? "-" : dbText(toDb(d)),
                });
            }
        }
    }
}

// --- Bounds on modulation -----------------------------------------------------------------

void printBounds()
{
    std::cout << "\n## Bounds on modulation (M1.5)\n\n"
                 "A1 and B1 saws, Mix B at 0 with its modulation; atRest + slots, so the gain's test only skips\n"
                 "the output stage (exact if the bound holds) and freeze + slots, which skips the filter too.\n"
                 "Outputs skipped: the share of filter outputs whose gain the bound proved 0.\n\n";
    row({"Modulation of Mix B", "Bound", "Candidate", "Outputs skipped", "Filters skipped", "Identical"});
    row({"---", "---", "---", "---:", "---:", "---"});
    struct Mod
    {
        std::string_view name;
        MixModulation modulation;
        std::vector<EngineEvent> events;
    };
    std::vector<EngineEvent> plucks;
    for (int k = 0; k < 8; ++k)
    {
        plucks.push_back(noteOn(static_cast<std::size_t>(k) * samples(0.25), 45 + k));
        plucks.push_back(noteOff((static_cast<std::size_t>(k) * samples(0.25)) + samples(0.2), 45 + k));
    }
    const std::array<Mod, 3> mods{
        {
            {
                .name = "free LFO 2 Hz, +-5",
                .modulation = {.kind = ModulationKind::lfo, .amount = 5.0f, .rateHz = 2.0f},
                .events = sorted({noteOn(0, 45)}),
            },
            {
                .name = "free LFO 2 Hz, +-5, Mix B at 5",
                .modulation = {.kind = ModulationKind::lfo, .amount = 5.0f, .rateHz = 2.0f},
                .events = sorted({noteOn(0, 45)}),
            },
            {
                .name = "pluck +10, tau 20 ms, a note every 0.25 s",
                .modulation = {.kind = ModulationKind::pluck, .amount = 10.0f, .tauSeconds = 0.02f},
                .events = sorted(plucks),
            },
        },
    };
    for (std::size_t m = 0; m < mods.size(); ++m)
    {
        Setup base{.patch = twoParts, .mixKnobs = {10.0f, m == 1 ? 5.0f : 0.0f}, .length = samples(2.0)};
        base.context.modulation = mods[m].modulation;
        const auto reference = render(base, mods[m].events).out;
        for (const auto bound : {BoundKind::conservative, BoundKind::tight})
        {
            for (const auto filter : {FilterSkip::atRest, FilterSkip::freeze})
            {
                Setup s = base;
                s.context.bound = bound;
                s.context.filterSkip = filter;
                s.context.skipOscillators = true;
                const auto r = render(s, mods[m].events);
                row({
                    std::string{mods[m].name},
                    bound == BoundKind::tight ? "tight" : "conservative",
                    filter == FilterSkip::atRest ? "atRest + slots" : "freeze + slots",
                    percent(r.stats.outputSkipped, r.stats.outputSamples),
                    percent(r.stats.filterSkipped, r.stats.filterSamples),
                    sameBits(r.out, reference)
                        ? "yes"
                        : "no (" + dbText(differenceRe(r.out, reference, 0, reference.numSamples())) + " dB)",
                });
            }
        }
    }
}

// --- The mono path ------------------------------------------------------------------------

void printMono()
{
    std::cout << "\n## The mono path\n\n"
                 "Mono on against mono off (the same skipping), the off render at host blocks of 4096 the\n"
                 "reference; the on renders at 1, 37 and 4096. Two sequences: a chord held 0.5 s (A2, E3, A3,\n"
                 "C4), and the identity sequence's notes (Polyphony 4, steals) with A1's Pan moved to 0.3 at\n"
                 "0.2 s + 5 (every centred voice leaves the mono path there).\n\n";
    row({"Patch", "Sequence", "Skipping", "Renders", "Identical", "Mono share", "Notes that left", "Left = right"});
    row({"---", "---", "---", "---:", "---:", "---:", "---:", "---"});
    const std::array<std::pair<std::string_view, const Patch*>, 6> patches{
        {
            {"init", &initPatch},
            {"worst, centred", &worstCentred},
            {"worst, spread", &worst},
            {"pad, centred", &pad},
            {"pad, spread", &padSpread},
            {"A1 noise, centred", &noisePatch},
        },
    };
    const auto chord = sorted({noteOn(0, 45), noteOn(0, 52), noteOn(0, 57), noteOn(0, 60)});
    auto moved = identitySequence();
    moved.push_back(panEvent(samples(0.2) + 5, 0, 0.3f));
    moved = sorted(moved);
    int total = 0;
    int same = 0;
    for (const auto& [name, patch] : patches)
    {
        for (std::size_t q = 0; q < 2; ++q)
        {
            const auto& events = q == 0 ? chord : moved;
            for (const bool skipping : {false, true})
            {
                Setup s{
                    .patch = *patch,
                    .polyphony = q == 0 ? 16U : 4U,
                    .length = q == 0 ? samples(0.5) : 60000U,
                    .hostBlock = 4096,
                };
                if (skipping)
                {
                    s.context.filterSkip = FilterSkip::atRest;
                    s.context.skipOscillators = true;
                }
                const auto reference = render(s, events).out;
                int ok = 0;
                SkipStats stats{};
                bool leftRight = true;
                for (const std::size_t block : {1U, 37U, 4096U})
                {
                    Setup m = s;
                    m.context.mono = true;
                    m.hostBlock = block;
                    const auto r = render(m, events);
                    ok += sameBits(r.out, reference) ? 1 : 0;
                    stats = r.stats;
                    if (q == 0)
                    {
                        for (std::size_t i = 0; i < r.out.numSamples(); ++i)
                        {
                            leftRight = leftRight && std::bit_cast<std::uint32_t>(r.out.channel(0)[i]) ==
                                                         std::bit_cast<std::uint32_t>(r.out.channel(1)[i]);
                        }
                    }
                }
                total += 3;
                same += ok;
                row({
                    std::string{name},
                    q == 0 ? "chord" : "steals, Pan moved",
                    skipping ? "atRest + slots" : "none",
                    "3",
                    std::to_string(ok),
                    stats.monoSamples > 0 ? "yes" : "never",
                    std::to_string(stats.monoLeft),
                    std::string{leftRightText(q == 0, leftRight)},
                });
            }
        }
    }
    std::cout << "\n" << same << " of " << total << " mono renders identical to the bit to the stereo reference.\n";
}

// --- Real-time terms ----------------------------------------------------------------------

void printRealTime()
{
    std::cout << "\n## Real-time terms\n\n"
                 "16 voices: the identity sequence's notes at Polyphony 16 plus a chord of 8 held from 0, with\n"
                 "Mix B to 0 at 0.3 s + 3 and back to 10 at 0.6 s + 5, Solo A at 0.7 s + 9, Part select B at\n"
                 "0.8 s + 1, Solo off at 0.9 s + 7, B1's Volume to 0 at 1.0 s + 3 and back at 1.1 s + 11, A1's Pan\n"
                 "to 0.2 at 1.2 s + 2; A1 and B1 saws on their own filters, 1.3 s. Host blocks of 1, 37 and 4096\n"
                 "against 4096.\n\n";
    auto events = identitySequence();
    for (int k = 0; k < 8; ++k)
    {
        events.push_back(noteOn(0, 70 + k));
    }
    for (const auto& e : std::vector<EngineEvent>{
             mixEvent(samples(0.3) + 3, 1, 0.0f),
             mixEvent(samples(0.6) + 5, 1, 10.0f),
             soloEvent(samples(0.7) + 9, true),
             selectEvent(samples(0.8) + 1, 1),
             soloEvent(samples(0.9) + 7, false),
             volumeEvent(samples(1.0) + 3, 2, 0.0f),
             volumeEvent(samples(1.1) + 11, 2, 1.0f),
             panEvent(samples(1.2) + 2, 0, 0.2f),
         })
    {
        events.push_back(e);
    }
    events = sorted(events);
    row({"Candidate", "Solo", "Identical at 1 / 37", "Allocations"});
    row({"---", "---", "---", "---:"});
    struct Config
    {
        std::string_view name;
        FilterSkip filter;
        bool oscillators;
        bool mono;
        SkipTiming timing;
    };
    const std::array<Config, 8> configs{
        {
            {
                .name = "nothing skipped",
                .filter = FilterSkip::never,
                .oscillators = false,
                .mono = false,
                .timing = SkipTiming::perSegment,
            },
            {
                .name = "atRest + slots + mono, per chunk",
                .filter = FilterSkip::atRest,
                .oscillators = true,
                .mono = true,
                .timing = SkipTiming::perChunk,
            },
            {
                .name = "atRest + slots + mono, per segment",
                .filter = FilterSkip::atRest,
                .oscillators = true,
                .mono = true,
                .timing = SkipTiming::perSegment,
            },
            {
                .name = "freeze + slots + mono, per chunk",
                .filter = FilterSkip::freeze,
                .oscillators = true,
                .mono = true,
                .timing = SkipTiming::perChunk,
            },
            {
                .name = "freeze + slots + mono, per segment",
                .filter = FilterSkip::freeze,
                .oscillators = true,
                .mono = true,
                .timing = SkipTiming::perSegment,
            },
            {
                .name = "clear + slots + mono, per chunk",
                .filter = FilterSkip::clear,
                .oscillators = true,
                .mono = true,
                .timing = SkipTiming::perChunk,
            },
            {
                .name = "clear + slots + mono, per segment",
                .filter = FilterSkip::clear,
                .oscillators = true,
                .mono = true,
                .timing = SkipTiming::perSegment,
            },
            {
                .name = "clear + slots + mono, hysteresis",
                .filter = FilterSkip::clear,
                .oscillators = true,
                .mono = true,
                .timing = SkipTiming::hysteresis,
            },
        },
    };
    int same = 0;
    int total = 0;
    for (const auto& c : configs)
    {
        for (const bool routing : {false, true})
        {
            Setup s{.patch = twoParts, .length = samples(1.3), .hostBlock = 4096};
            s.context.filterSkip = c.filter;
            s.context.skipOscillators = c.oscillators;
            s.context.mono = c.mono;
            s.context.timing = c.timing;
            s.context.soloByRouting = routing;
            const auto reference = render(s, events);
            std::size_t allocations = reference.allocations;
            int ok = 0;
            for (const std::size_t block : {1U, 37U})
            {
                Setup b = s;
                b.hostBlock = block;
                const auto r = render(b, events);
                ok += sameBits(r.out, reference.out) ? 1 : 0;
                allocations += r.allocations;
            }
            same += ok;
            total += 2;
            row({
                std::string{c.name},
                routing ? "routing" : "gain, ramped",
                std::to_string(ok) + " of 2",
                std::to_string(allocations),
            });
        }
    }
    std::cout << "\n" << same << " of " << total << " identical to the bit at every host block.\n";
}

} // namespace

TEST_CASE("Part solo and skipping: the candidates, measured", "[.][solo-report]")
{
    std::cout << "# Part solo and skipping: the report (M1.4 task 5, stage 1)\n";
    printIdentity();
    printSoloReadings();
    printSoloClicks();
    printSkipCases();
    printOscillatorPhase();
    printTiming();
    printBounds();
    printMono();
    printRealTime();
    SUCCEED();
}
