// The AmpEnv A/B link report (docs/dsp/amp-envelope-link.md, M1.4 task 6,
// stage 1): measures the candidates in link_candidates.hpp over the real
// dsp::VoiceAllocator:
//
// - the prototype against the library: LinkVoice with no link is dsp::Voice
//   to the bit; each candidate, linked from before the note, against
//   dsp::Voice unlinked with both envelopes set alike;
// - the link turned on and off mid-note: the click (filters.md's measure:
//   energy above 1 kHz in a Hann 4096, dB re a unit step) on a held note, a
//   release tail, a crossing part, a legato line and an in-place steal, for
//   each candidate and transition, and how soon each reaches the render that
//   was linked throughout;
// - task 5's skip test and isActive() reading the envelope that applies,
//   against reading each part's own;
// - the real-time terms: the same output at host blocks of 1, 37 and 4096
//   with the link toggled and edited mid-note, no allocation while
//   processing.
//
// Hidden from the default run; run it in an optimised build:
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[amp-link-report]"
//
// CPU: the "[amp-link]" benchmarks (tools/bench.sh "[amp-link]").

#include "link_candidates.hpp"

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
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::amp_link;

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

void row(std::initializer_list<std::string> cells)
{
    std::cout << "|";
    for (const auto& c : cells)
    {
        std::cout << " " << c << " |";
    }
    std::cout << "\n";
}

// --- Patches ------------------------------------------------------------------------------

struct Patch
{
    std::array<dsp::Waveform, 4> waves{dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw};
    std::array<int, 4> voices{1, 0, 1, 0};
    std::array<float, 4> volume{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<int, 4> semitones{0, 0, 7, 0};
    float detune = 0.0f;
    float stereo = 0.0f;
    bool retrigger = true;
    std::array<dsp::FilterInput, 2> inputs{dsp::FilterInput::own, dsp::FilterInput::own};
    float cutoff = 5.0f;
    float resonance = 0.0f;
};

/// A1 and B1 saws (B1 a fifth up), each on its own filter.
const Patch twoParts{};
/// Both parts into filter A (AB), filter B on "--": part B is heard only
/// through filter A, after both envelopes.
const Patch crossing{.inputs = {dsp::FilterInput::both, dsp::FilterInput::none}};
/// A pad: A1, A2 and B1 unison into filter A on AB, resonant; filter B on
/// its own part.
const Patch pad{
    .voices = {4, 4, 2, 0},
    .detune = 0.3f,
    .stereo = 0.7f,
    .retrigger = false,
    .inputs = {dsp::FilterInput::both, dsp::FilterInput::own},
    .cutoff = 2.0f,
    .resonance = 6.0f,
};
/// Only part B has an oscillator; filter A has nothing to filter.
const Patch partBOnly{.voices = {0, 0, 1, 0}};
/// Quiet sines through LP24 (near linear at Volume 0.25), free-running so a
/// retrigger doesn't restart them: no energy above 1 kHz of their own.
const Patch quietSines{
    .waves = {dsp::Waveform::sine, dsp::Waveform::sine, dsp::Waveform::sine, dsp::Waveform::sine},
    .volume = {0.25f, 0.25f, 0.25f, 0.25f},
    .retrigger = false,
};
const Patch quietCrossing = []
{
    Patch p = quietSines;
    p.inputs = {dsp::FilterInput::both, dsp::FilterInput::none};
    return p;
}();

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
    for (std::size_t f = 0; f < 2; ++f)
    {
        v.setFilterInput(f, p.inputs[f]);
    }
    v.drift().setDepthCents(1.5f);
}

/// Two envelopes far apart, so a link changes what is heard.
constexpr EnvelopeParams envA{.attack = 0.005f, .decay = 0.2f, .sustain = 0.8f, .release = 0.3f};
constexpr EnvelopeParams envB{.attack = 0.4f, .decay = 1.0f, .sustain = 0.3f, .release = 1.2f};

// --- Models -------------------------------------------------------------------------------

struct Model
{
    std::string name;
    bool library = false; ///< dsp::Voice (else LinkVoice)
    LinkPolicy policy{};
};

LinkPolicy signalPolicy(LinkTransition t, std::size_t master = 0)
{
    return {.voice = {.kind = LinkKind::signal, .transition = t, .master = master}};
}

const Model library{.name = "dsp::Voice, no link", .library = true};
const Model prototypeNone{.name = "LinkVoice, no link", .policy = {.voice = {.kind = LinkKind::none}}};
const Model paramSnap{.name = "(a) parameter, snap", .library = true, .policy = {.mirror = true}};
const Model paramKeep{
    .name = "(a) parameter, snap, B keeps A's at unlink",
    .library = true,
    .policy = {.mirror = true, .restoreOnUnlink = false},
};
const Model paramNext{
    .name = "(a) parameter, next note",
    .policy = {.voice = {.kind = LinkKind::parameter}, .mirror = true, .pendingAtNextNote = true},
};
const Model sharedSnap{.name = "(b) shared, snap", .policy = {.voice = {.kind = LinkKind::shared}, .mirror = true}};
const Model signalSnap{.name = "(c) signal, snap", .policy = signalPolicy(LinkTransition::snap)};
const Model signalRamp{.name = "(c) signal, 10 ms ramp", .policy = signalPolicy(LinkTransition::ramp)};
const Model signalNext{.name = "(c) signal, next note", .policy = signalPolicy(LinkTransition::nextNote)};

Model masterB(Model m)
{
    m.name += ", master B";
    m.policy.voice.master = 1;
    return m;
}

Model wrong(Model m)
{
    m.name += ", own envelope read";
    m.policy.voice.wrongReading = true;
    return m;
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
EngineEvent monoLegato(std::size_t at, bool on)
{
    return {.offset = at, .voice = {.kind = dsp::VoiceEventKind::monoLegato, .value = on ? 1 : 0}};
}
EngineEvent linkEvent(std::size_t at, bool on)
{
    return {.offset = at, .kind = EngineEventKind::link, .on = on};
}
EngineEvent editEvent(std::size_t at, std::size_t part, const EnvelopeParams& p)
{
    return {.offset = at, .kind = EngineEventKind::envelope, .part = part, .params = p};
}

struct Setup
{
    Patch patch = twoParts;
    EnvelopeParams a = envA;
    EnvelopeParams b = envB;
    bool linked = false;
    std::size_t polyphony = 16;
    std::size_t length = samples(1.2);
    std::size_t hostBlock = 480;
};

struct Render
{
    Buffer out{2, 1};
    std::size_t allocations = 0;
    LinkStats stats{};
    std::vector<std::size_t> busy; ///< notes sounding at the end of each host block
    std::size_t hostBlock = 1;

    [[nodiscard]] std::size_t busyAt(std::size_t sample) const
    {
        return busy[std::min(sample / hostBlock, busy.size() - 1)];
    }
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

template <typename V>
Render renderWith(const Model& m, const Setup& s, std::span<const EngineEvent> events)
{
    auto engine = std::make_unique<LinkEngine<V>>(table(), m.policy);
    engine->voices().forEachVoice([&](V& v) { apply(v, s.patch); });
    engine->voices().setPolyphony(s.polyphony);
    engine->filterKnobs().set(s.patch.cutoff, s.patch.resonance);
    engine->setEnvelope(0, s.a);
    engine->setEnvelope(1, s.b);
    engine->setLinked(s.linked);
    engine->prepare({.sampleRate = rate, .maxBlockSize = s.hostBlock, .numChannels = 2});
    const auto blocks = perBlock(events, s.length, s.hostBlock);
    Render r;
    r.hostBlock = s.hostBlock;
    r.out = Buffer{2, s.length};
    r.busy.reserve(blocks.size());
    const auto out = r.out.block();
    std::size_t allocations = 0;
    for (std::size_t b = 0; b < blocks.size(); ++b)
    {
        const auto start = b * s.hostBlock;
        {
            const AllocationCounter counter;
            engine->process(out.subBlock(start, std::min(s.hostBlock, s.length - start)), blocks[b]);
            allocations += counter.count();
        }
        r.busy.push_back(engine->voices().activeNotes());
    }
    r.allocations = allocations;
    r.stats = engine->stats();
    return r;
}

std::vector<EngineEvent> sorted(std::vector<EngineEvent> events)
{
    std::ranges::stable_sort(events, {}, &EngineEvent::offset);
    return events;
}

Render render(const Model& m, const Setup& s, const std::vector<EngineEvent>& events)
{
    const auto ordered = sorted(events);
    if (m.library)
    {
        return renderWith<dsp::Voice<>>(m, s, ordered);
    }
    return renderWith<LinkVoice<>>(m, s, ordered);
}

// --- Measures -----------------------------------------------------------------------------

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

/// The last sample at which a and b differ (in either channel), or -1.
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

/// The difference's RMS over n samples from `from`, dB re the reference's.
double differenceRe(const Buffer& candidate, const Buffer& reference, std::size_t from, std::size_t n)
{
    Buffer d{2, candidate.numSamples()};
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < candidate.numSamples(); ++i)
        {
            d.channel(c)[i] = candidate.channel(c)[i] - reference.channel(c)[i];
        }
    }
    return toDb(stereoRms(d, from, n) / stereoRms(reference, from, n));
}

/// Energy above 1 kHz in a Hann 4096 centred 32 samples after `at`, both
/// channels, dB re an instant unit step's.
double clickDb(const Buffer& b, std::size_t at)
{
    static const double step = []
    {
        std::vector<float> x(8192, 0.0f);
        std::fill(x.begin() + 4096, x.end(), 1.0f);
        return filter_control::energyAbove1k(x, 4096 + 32, rate);
    }();
    const double e = (filter_control::energyAbove1k(b.channel(0), at + 32, rate) +
                      filter_control::energyAbove1k(b.channel(1), at + 32, rate)) /
                     (2.0 * step);
    return e > 0.0 ? 10.0 * std::log10(e) : -std::numeric_limits<double>::infinity();
}

// --- Sequences ----------------------------------------------------------------------------

struct Sequence
{
    std::string name;
    Setup setup;
    std::vector<EngineEvent> events;
};

/// Four notes, staggered, released in turn.
std::vector<EngineEvent> chordEvents()
{
    std::vector<EngineEvent> e;
    const std::array<int, 4> keys{48, 52, 55, 59};
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        e.push_back(noteOn(samples(0.05 * static_cast<double>(i)), keys[i]));
        e.push_back(noteOff(samples(0.5 + (0.05 * static_cast<double>(i))), keys[i]));
    }
    return e;
}

std::vector<Sequence> identitySequences()
{
    std::vector<Sequence> seqs;
    seqs.push_back({.name = "chord, two parts", .setup = {.patch = twoParts}, .events = chordEvents()});
    {
        // Polyphony 2: five overlapping notes, so voices are stolen in place.
        std::vector<EngineEvent> e;
        for (int i = 0; i < 5; ++i)
        {
            const auto t = 0.12 * static_cast<double>(i);
            e.push_back(noteOn(samples(t), 50 + (3 * i)));
            e.push_back(noteOff(samples(t + 0.3), 50 + (3 * i)));
        }
        seqs.push_back({.name = "steals (Polyphony 2)", .setup = {.patch = twoParts, .polyphony = 2}, .events = e});
    }
    seqs.push_back({
        .name = "Mono Legato",
        .setup = {.patch = twoParts},
        .events =
            {
                monoLegato(0, true),
                noteOn(0, 48),
                noteOn(samples(0.2), 55),
                noteOff(samples(0.35), 55),
                noteOn(samples(0.5), 60),
                noteOff(samples(0.7), 60),
                noteOff(samples(0.75), 48),
            },
    });
    seqs.push_back({.name = "chord, crossing (A on AB)", .setup = {.patch = crossing}, .events = chordEvents()});
    seqs.push_back({.name = "chord, pad", .setup = {.patch = pad}, .events = chordEvents()});
    return seqs;
}

// --- The sections -------------------------------------------------------------------------

void prototypeAgainstLibrary()
{
    std::cout << "\n## The prototype against the library\n\n";
    std::cout << "LinkVoice with no link against dsp::Voice, both envelopes different (A's and B's settings below), "
                 "host blocks 1/37/4096; then each candidate linked from before the first note, part B's own knobs "
                 "left at B's settings, against dsp::Voice unlinked with both envelopes set to the master's.\n\n";
    row({"candidate", "sequences identical (of 5 x 3 blocks)", "allocations"});
    row({"---", "---", "---"});
    const std::array<std::size_t, 3> blocks{1, 37, 4096};
    const auto check = [&](const Model& m, bool linkedCandidate)
    {
        std::size_t same = 0;
        std::size_t total = 0;
        std::size_t allocations = 0;
        for (const auto& seq : identitySequences())
        {
            for (const auto hb : blocks)
            {
                Setup ref = seq.setup;
                ref.hostBlock = hb;
                Setup cand = ref;
                if (linkedCandidate)
                {
                    const auto masterParams = m.policy.voice.master == 0 ? envA : envB;
                    ref.a = masterParams;
                    ref.b = masterParams;
                    cand.linked = true;
                }
                const auto r = render(library, ref, seq.events);
                const auto c = render(m, cand, seq.events);
                same += sameBits(r.out, c.out) ? 1 : 0;
                ++total;
                allocations += c.allocations;
            }
        }
        row({m.name, std::to_string(same) + " of " + std::to_string(total), std::to_string(allocations)});
    };
    check(prototypeNone, false);
    for (const auto& m : {
             paramSnap,
             paramNext,
             sharedSnap,
             signalSnap,
             signalRamp,
             signalNext,
             masterB(paramSnap),
             masterB(sharedSnap),
             masterB(signalSnap),
         })
    {
        check(m, true);
    }
}

struct ToggleCase
{
    std::string name;
    Patch patch;
    std::size_t polyphony = 16;
    std::vector<EngineEvent> notes;
    std::size_t toggle = 0; ///< the link's toggle
    std::size_t later = 0;  ///< a later event to measure too (0: none)
    std::string laterName;
};

std::vector<ToggleCase> toggleCases()
{
    const int key = 57; // A3: sines at 220 and 330 Hz
    std::vector<ToggleCase> cases;
    cases.push_back({.name = "held", .patch = quietSines, .notes = {noteOn(0, key)}, .toggle = samples(0.6)});
    cases.push_back({
        .name = "release tail",
        .patch = quietSines,
        .notes = {noteOn(0, key), noteOff(samples(0.6), key)},
        .toggle = samples(0.75),
    });
    cases.push_back(
        {.name = "crossing (A on AB)", .patch = quietCrossing, .notes = {noteOn(0, key)}, .toggle = samples(0.6)});
    cases.push_back({
        .name = "Mono Legato",
        .patch = quietSines,
        // Downwards, so the filter's third harmonic stays below 1 kHz.
        .notes = {monoLegato(0, true), noteOn(0, key), noteOn(samples(0.3), key - 3), noteOn(samples(0.9), key - 5)},
        .toggle = samples(0.6),
        .later = samples(0.9),
        .laterName = "the next legato note",
    });
    cases.push_back({
        .name = "in-place steal (Polyphony 1)",
        .patch = quietSines,
        .polyphony = 1,
        .notes = {noteOn(0, key), noteOn(samples(0.6), key)},
        .toggle = samples(0.3),
        .later = samples(0.6),
        .laterName = "the steal",
    });
    return cases;
}

void toggles()
{
    std::cout << "\n## The link turned on and off mid-note\n\n";
    std::cout << "Quiet free-running sines (A1 220 Hz, B1 330 Hz, Volume 0.25) through LP24 at 4,677 Hz. AmpEnv A: "
                 "A 5 ms, D 0.2 s, S 0.8, R 0.3 s; AmpEnv B: A 0.4 s, D 1 s, S 0.3, R 1.2 s. Master A unless "
                 "named. Click: energy above 1 kHz in a Hann 4096 centred 32 samples after the event, dB re a unit "
                 "step. Floor: the same render without the toggle (the larger of the unlinked and linked renders). "
                 "Reached: the last sample at which the render differs from the one linked throughout (on) or "
                 "unlinked throughout (off), in ms after the toggle; 'never' if it differs to the end.\n\n";
    const std::vector<Model> onModels{
        paramSnap,  paramNext,          sharedSnap,          signalSnap,          signalRamp,
        signalNext, masterB(paramSnap), masterB(signalSnap), masterB(signalRamp),
    };
    const std::vector<Model> offModels{paramSnap, paramKeep, paramNext, sharedSnap, signalSnap, signalRamp, signalNext};
    const auto length = samples(1.6);
    for (const bool on : {true, false})
    {
        std::cout << "\n### Link " << (on ? "on" : "off") << "\n\n";
        row({"case", "candidate", "click at the toggle (dB)", "at the later event (dB)", "floor (dB)", "reached (ms)"});
        row({"---", "---", "---", "---", "---", "---"});
        for (const auto& tc : toggleCases())
        {
            for (const auto& m : on ? onModels : offModels)
            {
                const Setup s{.patch = tc.patch, .linked = !on, .polyphony = tc.polyphony, .length = length};
                auto events = tc.notes;
                events.push_back(linkEvent(tc.toggle, on));
                const auto r = render(m, s, events);
                // The floors: the same model without the toggle, unlinked and linked.
                Setup unlinked = s;
                unlinked.linked = false;
                Setup linkedThroughout = s;
                linkedThroughout.linked = true;
                const auto u = render(m, unlinked, tc.notes);
                const auto l = render(m, linkedThroughout, tc.notes);
                const double floor = std::max(clickDb(u.out, tc.toggle), clickDb(l.out, tc.toggle));
                double floorLater = -std::numeric_limits<double>::infinity();
                std::string later = "-";
                if (tc.later > 0)
                {
                    floorLater = std::max(clickDb(u.out, tc.later), clickDb(l.out, tc.later));
                    later = dbText(clickDb(r.out, tc.later)) + " (floor " + dbText(floorLater) + ")";
                }
                const auto& target = on ? l : u;
                const auto last = lastDifference(r.out, target.out);
                std::string reached = "at once";
                if (std::cmp_greater_equal(last, static_cast<std::ptrdiff_t>(length) - 1))
                {
                    reached = "never";
                }
                else if (std::cmp_greater_equal(last, static_cast<std::ptrdiff_t>(tc.toggle)))
                {
                    reached = fixed(1000.0 * static_cast<double>(static_cast<std::size_t>(last) + 1 - tc.toggle) /
                                        static_cast<double>(rate),
                                    1);
                }
                row({tc.name, m.name, dbText(clickDb(r.out, tc.toggle)), later, dbText(floor), reached});
            }
        }
    }
    std::cout << "\n(b) shared against (a) parameter, snap, through every toggle above (on and off): ";
    std::size_t same = 0;
    std::size_t total = 0;
    for (const auto& tc : toggleCases())
    {
        for (const bool on : {true, false})
        {
            const Setup s{.patch = tc.patch, .linked = !on, .polyphony = tc.polyphony, .length = length};
            auto events = tc.notes;
            events.push_back(linkEvent(tc.toggle, on));
            same += sameBits(render(paramSnap, s, events).out, render(sharedSnap, s, events).out) ? 1 : 0;
            ++total;
        }
    }
    std::cout << same << " of " << total << " identical to the bit.\n";
}

void readings()
{
    std::cout << "\n## The skip test and isActive() with the envelope that applies\n\n";
    std::cout << "Saws, linked from before the note, master A. 'Right' reads the envelope that applies to each "
                 "part; 'own' reads each part's own (dsp::Voice's code unchanged). Reference: dsp::Voice unlinked "
                 "with both envelopes set to the master's.\n\n";
    row({"case", "candidate", "difference from the reference (dB re its RMS)", "outputs skipped", "notes busy"});
    row({"---", "---", "---", "---", "---"});

    const auto report = [&](const std::string& name, const Model& m, const Setup& s,
                            const std::vector<EngineEvent>& events, std::size_t from, std::size_t n,
                            std::size_t busyProbe)
    {
        Setup ref = s;
        ref.linked = false;
        ref.b = ref.a;
        const auto r = render(library, ref, events);
        const auto c = render(m, s, events);
        const double d = differenceRe(c.out, r.out, from, n);
        row({
            name,
            m.name,
            sameBits(c.out, r.out) ? "identical" : dbText(d),
            percent(c.stats.outputsSkipped, c.stats.chunks),
            std::to_string(c.busyAt(busyProbe)) + " at " +
                fixed(static_cast<double>(busyProbe) / static_cast<double>(rate), 2) + " s (reference " +
                std::to_string(r.busyAt(busyProbe)) + ")",
        });
    };

    // W1: part B's own knobs hold at sustain 0 after 50 ms; the master's at 1.
    {
        const Setup s{
            .patch = twoParts,
            .a = {.attack = 0.0f, .decay = 0.0f, .sustain = 1.0f, .release = 0.3f},
            .b = {.attack = 0.0f, .decay = 0.05f, .sustain = 0.0f, .release = 0.05f},
            .linked = true,
        };
        const std::vector<EngineEvent> events{noteOn(0, 48), noteOff(samples(1.0), 48)};
        for (const auto& m : {signalSnap, wrong(signalSnap), paramSnap})
        {
            report("B's own sustain 0, the master's 1 (held 0.1-1.0 s)", m, s, events, samples(0.1), samples(0.9),
                   samples(0.5));
        }
    }
    // W2: only part B sounds; its own release is 0, the master's 1 s.
    {
        const Setup s{
            .patch = partBOnly,
            .a = {.attack = 0.0f, .decay = 0.0f, .sustain = 1.0f, .release = 1.0f},
            .b = {.attack = 0.0f, .decay = 0.0f, .sustain = 1.0f, .release = 0.0f},
            .linked = true,
            .length = samples(2.0),
        };
        const std::vector<EngineEvent> events{noteOn(0, 48), noteOff(samples(0.5), 48)};
        for (const auto& m : {signalSnap, wrong(signalSnap), paramSnap})
        {
            report("B only; B's own release 0, the master's 1 s (tail 0.5-1.5 s)", m, s, events, samples(0.5),
                   samples(1.0), samples(0.6));
        }
    }
    // W3: shared; the other envelope isn't run, so it never reaches idle.
    {
        const Setup s{
            .patch = twoParts,
            .a = {.attack = 0.0f, .decay = 0.0f, .sustain = 1.0f, .release = 0.3f},
            .b = {.attack = 0.0f, .decay = 0.0f, .sustain = 1.0f, .release = 0.3f},
            .linked = true,
            .length = samples(2.0),
        };
        const std::vector<EngineEvent> events{noteOn(0, 48), noteOff(samples(0.5), 48)};
        for (const auto& m : {sharedSnap, wrong(sharedSnap)})
        {
            report("shared, both R 0.3 s (busy after the release)", m, s, events, 0, samples(2.0), samples(1.5));
        }
    }
    // W4: shared, both held at sustain 0: the skip.
    {
        const Setup s{
            .patch = twoParts,
            .a = {.attack = 0.0f, .decay = 0.05f, .sustain = 0.0f, .release = 0.05f},
            .b = {.attack = 0.0f, .decay = 0.05f, .sustain = 0.0f, .release = 0.05f},
            .linked = true,
        };
        const std::vector<EngineEvent> events{noteOn(0, 48), noteOff(samples(1.0), 48)};
        for (const auto& m : {sharedSnap, wrong(sharedSnap), paramSnap})
        {
            report("shared, both sustain 0 (held to 1.0 s)", m, s, events, 0, samples(1.0), samples(0.5));
        }
    }
}

void realTime()
{
    std::cout << "\n## Real-time terms\n\n";
    std::cout << "A chord on the pad with the link turned on at 0.25 s, A's knobs edited at 0.35 s, a steal at "
                 "Polyphony 3, and the link turned off at 0.6 s; host blocks 1, 37 and 4096.\n\n";
    row({"candidate", "identical across block sizes", "allocations while processing"});
    row({"---", "---", "---"});
    std::vector<EngineEvent> events = chordEvents();
    events.push_back(noteOn(samples(0.3), 62));
    events.push_back(noteOff(samples(0.55), 62));
    events.push_back(linkEvent(samples(0.25), true));
    events.push_back(editEvent(samples(0.35), 0, {.attack = 0.02f, .decay = 0.1f, .sustain = 0.5f, .release = 0.2f}));
    events.push_back(linkEvent(samples(0.6), false));
    for (const auto& m : {library, paramSnap, paramKeep, paramNext, sharedSnap, signalSnap, signalRamp, signalNext})
    {
        std::vector<Render> renders;
        std::size_t allocations = 0;
        for (const std::size_t hb : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
        {
            const Setup s{.patch = pad, .polyphony = 3, .hostBlock = hb};
            renders.push_back(render(m, s, events));
            allocations += renders.back().allocations;
        }
        const bool same = sameBits(renders[0].out, renders[1].out) && sameBits(renders[0].out, renders[2].out);
        row({m.name, same ? "yes" : "no", std::to_string(allocations)});
    }
}

} // namespace

TEST_CASE("AmpEnv A/B link: the report", "[.][amp-link-report]")
{
    std::cout << "# AmpEnv A/B link report (M1.4 task 6, stage 1)\n";
    prototypeAgainstLibrary();
    toggles();
    readings();
    realTime();
    SUCCEED();
}
