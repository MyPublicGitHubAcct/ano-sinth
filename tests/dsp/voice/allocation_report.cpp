// The voice allocation report (docs/dsp/voice-allocation.md, M1.4 task 2,
// stage 1): measures the candidate allocators (allocation_candidates.hpp)
// driving dsp::Voice (and the stage-1 prototype voice, for a retrigger that
// keeps the filters):
//
// - questions 1 and 6, the end of a stolen note: the click at a steal
//   (energy above 1 kHz in a Hann 4096 centred 32 samples after the event,
//   dB re a unit step, as filters.md measures) for a cut, fades of 1, 5 and
//   10 ms before the new note or on a spare voice, and a retrigger in place
//   with the filters reset (dsp::Voice) or kept (the prototype), on three
//   patches, the stolen note held or releasing;
// - question 2, a key played again: the level over the overlap and the
//   click, a new voice or its own voice;
// - question 3, freeing: release-end against thresholds, sustain-0 holds,
//   and an envelope on a part nothing hears;
// - question 5 and the counts: busy voices over time for fast repeated
//   notes and a melody, and the Voices readout's candidate counts against
//   the manual's screenshot (204/336);
// - question 1, the policies: a 32-note strum at Polyphony 16, and a held
//   bass under a melody at Polyphony 8;
// - question 4, Polyphony lowered and 0;
// - the allocator's real-time terms: the same output at host blocks of 1,
//   37 and 4096, no allocation, each voice's seed from its index, and a
//   freed voice reused with and without stopping it.
//
// Hidden from the default run; run it in an optimised build (about 30 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[voice-allocation-report]"
//
// CPU: the "[voice-allocation]" benchmarks (tools/bench.sh "[voice-allocation]").

#include "allocation_candidates.hpp"

#include "../filters/filter_control_support.hpp"
#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/voice/voice.hpp>

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
#include <string_view>
#include <type_traits>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::allocation;
namespace dsp = anoesis::dsp;
using filter_control::constant;

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

double secondsOf(std::size_t n)
{
    return static_cast<double>(n) / static_cast<double>(rate);
}

// --- Patches --------------------------------------------------------------------------

/// A patch every voice of a pool takes. The click patches use sines, so the
/// energy above 1 kHz around an event is the event's own (a saw's partials
/// would fill the band whatever happens at the event).
struct PatchSpec
{
    std::string_view name;
    dsp::Waveform wave = dsp::Waveform::sine;
    std::array<int, 4> voices{1, 0, 0, 0};
    float detune = 0.0f;
    float stereo = 0.0f;
    bool retrigger = true;
    float attack = 0.0f;
    float decay = 0.0f;
    float sustain = 1.0f;
    float release = 0.0f;
    std::array<voice::Input, 2> inputs{voice::Input::own, voice::Input::own};
    float drive = 1.0f;
    float cutoff = 5.0f;    ///< both filters' Cutoff knobs (the Filter Control's at 5)
    float resonance = 0.0f; ///< both filters' Resonance knobs (k = 0.6 x knob)
    float driftCents = 0.0f;
    std::array<float, 2> releases{-1.0f, -1.0f}; ///< per part, if not negative
};

/// The init patch as far as the evidence goes (V9), with a sine: A1 one voice
/// at Volume 10, LP24 at 4,677 Hz, k 0, both envelopes a gate.
const PatchSpec initPatch{.name = "init (sine)"};

/// A resonant pad: A1 and A2 four detuned sines each, spread, free-running;
/// LP24 at 585 Hz (Cutoff 2 + 5), k 3.6; A 200 ms, D 500 ms, S 0.8, R 1 s.
const PatchSpec padPatch{
    .name = "resonant pad",
    .voices = {4, 4, 0, 0},
    .detune = 0.3f,
    .stereo = 1.0f,
    .retrigger = false,
    .attack = 0.2f,
    .decay = 0.5f,
    .sustain = 0.8f,
    .release = 1.0f,
    .cutoff = 2.0f,
    .resonance = 6.0f,
};

/// The worst case for a steal: 4 x 8 sines, Retrig on, both filters on AB at
/// drive x4, 585 Hz, k 4.8 (self-oscillating); A 5 ms, D 300 ms, S 0.7, R 200 ms.
const PatchSpec worstPatch{
    .name = "worst",
    .voices = {8, 8, 8, 8},
    .detune = 0.548f,
    .stereo = 1.0f,
    .retrigger = true,
    .attack = 0.005f,
    .decay = 0.3f,
    .sustain = 0.7f,
    .release = 0.2f,
    .inputs = {voice::Input::both, voice::Input::both},
    .drive = 4.0f,
    .cutoff = 2.0f,
    .resonance = 8.0f,
};

/// The worst case for a filter reset: one sine into LP24 at 585 Hz, k 3.96,
/// just below self-oscillation (from a knob sum of 6.67), so the filter
/// rings long but its output stays clean; A 5 ms, S 1, R 300 ms, Retrig on.
const PatchSpec ringingPatch{
    .name = "ringing",
    .attack = 0.005f,
    .release = 0.3f,
    .cutoff = 2.0f,
    .resonance = 6.6f,
};

template <typename V>
void apply(V& v, const PatchSpec& p)
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
        env.setRelease(p.releases[e] >= 0.0f ? p.releases[e] : p.release);
    }
    std::array<voice::FilterSettings, 2> filters{};
    for (std::size_t f = 0; f < 2; ++f)
    {
        filters[f].input = p.inputs[f];
        filters[f].drive = p.drive;
    }
    v.setFilters(filters, true);
    v.drift().setDepthCents(p.driftCents);
}

filter_control::Knobs knobsFor(const PatchSpec& p)
{
    filter_control::Knobs k;
    k.cutoff = constant(p.cutoff);
    k.resonance = constant(p.resonance);
    return k;
}

// --- Rendering ------------------------------------------------------------------------

struct Run
{
    Buffer out{2, 1};
    Stats stats;
    std::vector<Freed> freed;
    std::vector<Readout> readouts; ///< after each host block
    std::size_t allocations = 0;   ///< while processing (and preparing, for dsp::Voice)
};

template <typename V, std::size_t Pool>
using Probe = std::function<void(Allocator<V, Pool>&, std::size_t)>;

struct Setup
{
    PatchSpec patch = initPatch;
    Policy policy{};
    std::size_t polyphony = 16;
    std::size_t length = 0;
    std::size_t hostBlock = 512;
    voice::Engine engine{};
    std::uint64_t seed = 0;
};

/// Renders `events` through an allocator of V in host blocks, the readout
/// read after each block (and `probe` called there, with the block's end).
template <typename V, std::size_t Pool = 2 * maxPolyphony>
Run run(const Setup& s, std::span<const Event> events, const Probe<V, Pool>& probe = {})
{
    GlobalKnobs knobs{knobsFor(s.patch), knobsFor(s.patch), rate};
    auto alloc = std::make_unique<Allocator<V, Pool>>(
        Context{.table = &d6::minBlepTable64(), .knobs = &knobs, .seed = s.seed, .engine = s.engine}, s.policy);
    alloc->forEachVoice([&](V& v) { apply(v, s.patch); });
    alloc->setPolyphony(s.polyphony);
    Run r;
    r.out = Buffer{2, s.length};
    r.readouts.reserve((s.length / s.hostBlock) + 2);
    const auto block = r.out.block();
    {
        const AllocationCounter counter;
        alloc->prepare({.sampleRate = rate, .maxBlockSize = s.hostBlock, .numChannels = 2});
        // The prototype allocates its scratch in prepare(): count from here for it.
        const auto prepared = counter.count();
        std::size_t e = 0;
        for (std::size_t start = 0; start < s.length; start += s.hostBlock)
        {
            const auto end = std::min(s.length, start + s.hostBlock);
            const auto first = e;
            while (e < events.size() && events[e].at < end)
            {
                ++e;
            }
            alloc->process(block.subBlock(start, end - start), events.subspan(first, e - first), start);
            knobs.advance(end - start);
            r.readouts.push_back(alloc->readout());
            if (probe)
            {
                probe(*alloc, end);
            }
        }
        r.allocations = std::is_same_v<V, BuiltVoice> ? counter.count() : counter.count() - prepared;
    }
    r.stats = alloc->stats();
    const auto f = alloc->freed();
    r.freed.assign(f.begin(), f.end());
    return r;
}

Event on(std::size_t at, int note)
{
    return {.at = at, .kind = EventKind::noteOn, .value = note};
}
Event off(std::size_t at, int note)
{
    return {.at = at, .kind = EventKind::noteOff, .value = note};
}
Event polyphony(std::size_t at, int notes)
{
    return {.at = at, .kind = EventKind::polyphony, .value = notes};
}

std::vector<Event> sorted(std::vector<Event> events)
{
    std::ranges::stable_sort(events, {}, &Event::at);
    return events;
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

/// filters.md's click: energy above 1 kHz in a Hann 4096 centred 32 samples
/// after `at`, both channels, dB re an instant unit step in both.
double clickDb(const Buffer& b, std::size_t at)
{
    static const double step = []
    {
        std::vector<float> x(8192, 0.0f);
        std::fill(x.begin() + 4096, x.end(), 1.0f);
        return filter_control::energyAbove1k(x, 4096 + 32, rate);
    }();
    const double e = filter_control::energyAbove1k(b.channel(0), at + 32, rate) +
                     filter_control::energyAbove1k(b.channel(1), at + 32, rate);
    return e > 0.0 ? 10.0 * std::log10(e / (2.0 * step)) : -std::numeric_limits<double>::infinity();
}

std::string dbText(double db)
{
    if (!std::isfinite(db))
    {
        return "silent";
    }
    return fixed(db, 1);
}

// --- Questions 1 and 6: the end of a stolen note ----------------------------------------

struct EndCase
{
    std::string_view name;
    StealEnd end;
    float fadeSeconds;
    bool baseline;  ///< Polyphony 2: no steal
    bool prototype; ///< the prototype voice, filters kept at a retrigger
    double latencyMs;
};

constexpr int oldNote = 45;
constexpr int newNote = 52;

Run stealRun(const PatchSpec& patch, const EndCase& c, bool releasing, std::size_t at)
{
    Setup s{.patch = patch, .polyphony = c.baseline ? 2U : 1U, .length = at + samples(0.2)};
    s.policy.end = c.end;
    s.policy.fadeSeconds = c.fadeSeconds;
    std::vector<Event> events{on(0, oldNote), on(at, newNote)};
    if (releasing)
    {
        events.push_back(off(at - samples(0.1), oldNote));
    }
    events = sorted(events);
    if (c.prototype)
    {
        s.engine.keepFiltersOnRetrigger = true;
        return run<PrototypeAdapter, 4>(s, events);
    }
    return run<BuiltVoice, 4>(s, events);
}

void printSteals()
{
    const std::array patches{initPatch, padPatch, ringingPatch, worstPatch};
    const std::array ends{
        EndCase{
            .name = "a free voice (Polyphony 2): no steal, the reference",
            .end = StealEnd::cut,
            .fadeSeconds = 0.0f,
            .baseline = true,
            .prototype = false,
            .latencyMs = 0.0,
        },
        EndCase{
            .name = "cut",
            .end = StealEnd::cut,
            .fadeSeconds = 0.0f,
            .baseline = false,
            .prototype = false,
            .latencyMs = 0.0,
        },
        EndCase{
            .name = "fade 1 ms, then the new note",
            .end = StealEnd::fadeThenStart,
            .fadeSeconds = 0.001f,
            .baseline = false,
            .prototype = false,
            .latencyMs = 1.0,
        },
        EndCase{
            .name = "fade 5 ms, then the new note",
            .end = StealEnd::fadeThenStart,
            .fadeSeconds = 0.005f,
            .baseline = false,
            .prototype = false,
            .latencyMs = 5.0,
        },
        EndCase{
            .name = "fade 10 ms, then the new note",
            .end = StealEnd::fadeThenStart,
            .fadeSeconds = 0.010f,
            .baseline = false,
            .prototype = false,
            .latencyMs = 10.0,
        },
        EndCase{
            .name = "fade 1 ms on a spare, the new note at once",
            .end = StealEnd::fadeOnSpare,
            .fadeSeconds = 0.001f,
            .baseline = false,
            .prototype = false,
            .latencyMs = 0.0,
        },
        EndCase{
            .name = "fade 5 ms on a spare, the new note at once",
            .end = StealEnd::fadeOnSpare,
            .fadeSeconds = 0.005f,
            .baseline = false,
            .prototype = false,
            .latencyMs = 0.0,
        },
        EndCase{
            .name = "fade 10 ms on a spare, the new note at once",
            .end = StealEnd::fadeOnSpare,
            .fadeSeconds = 0.010f,
            .baseline = false,
            .prototype = false,
            .latencyMs = 0.0,
        },
        EndCase{
            .name = "retrigger in place, filters reset (dsp::Voice)",
            .end = StealEnd::retrigger,
            .fadeSeconds = 0.0f,
            .baseline = false,
            .prototype = false,
            .latencyMs = 0.0,
        },
        EndCase{
            .name = "retrigger in place, filters and grid kept (prototype)",
            .end = StealEnd::retrigger,
            .fadeSeconds = 0.0f,
            .baseline = false,
            .prototype = true,
            .latencyMs = 0.0,
        },
    };
    const auto at = samples(0.5) + 13;

    std::cout << "\n## Questions 1 and 6: the end of a stolen note\n\n"
              << "Polyphony 1. Key " << oldNote << " at 0, key " << newNote << " at " << at
              << " (0.5 s, off the grid); *held*: the first key still down; *releasing*: released 100 ms "
                 "before. The click: energy above 1 kHz, Hann 4096 centred 32 samples after the new note-on, both "
                 "channels, dB re an instant unit step. *Steady*: the same window 8192 samples earlier, on the "
                 "reference (what the patch has above 1 kHz anyway). Patches with sines, so the band holds the "
                 "event's energy, not a saw's partials.\n\n"
              << "| End | latency (ms) |";
    for (const auto& p : patches)
    {
        std::cout << ' ' << p.name << ", held | " << p.name << ", releasing |";
    }
    std::cout << "\n|---|---:|";
    for (std::size_t i = 0; i < patches.size(); ++i)
    {
        std::cout << "---:|---:|";
    }
    std::cout << "\n| steady (the reference, 170 ms before) | |";
    for (const auto& p : patches)
    {
        const auto r = stealRun(p, ends[0], false, at);
        const auto rr = stealRun(p, ends[0], true, at);
        std::cout << ' ' << dbText(clickDb(r.out, at - 8192)) << " | " << dbText(clickDb(rr.out, at - 8192)) << " |";
    }
    std::cout << '\n';
    for (const auto& c : ends)
    {
        std::cout << "| " << c.name << " | " << fixed(c.latencyMs, 0) << " |";
        for (const auto& p : patches)
        {
            for (const bool releasing : {false, true})
            {
                const auto r = stealRun(p, c, releasing, at);
                std::cout << ' ' << dbText(clickDb(r.out, at));
                if (!c.baseline && r.stats.steals == 0)
                {
                    std::cout << " (free)";
                }
                std::cout << " |";
            }
        }
        std::cout << '\n';
    }

    // The prototype with its filters reset is dsp::Voice to the bit here too.
    std::size_t same = 0;
    for (const auto& p : patches)
    {
        Setup s{.patch = p, .polyphony = 1, .length = at + samples(0.2)};
        s.policy.end = StealEnd::retrigger;
        const auto events = sorted({on(0, oldNote), on(at, newNote)});
        same += sameBits(run<BuiltVoice, 4>(s, events).out, run<PrototypeAdapter, 4>(s, events).out) ? 1 : 0;
    }
    std::cout << "\nThe prototype voice with its filters reset at a retrigger against dsp::Voice, each patch held: "
                 "identical to the bit in "
              << same << " of " << patches.size() << ".\n";
    CHECK(same == patches.size());
}

// --- Question 2: a key played again ------------------------------------------------------

void printRepeatedKey()
{
    // A 110 Hz sine (period 436.36 samples), Retrig on, R 1 s: the second
    // note-on 12000 samples after the first is 27.5 periods on (opposite
    // phase), 12218 about 28 (in phase), 12109 about 27.75 (quadrature).
    PatchSpec patch = initPatch;
    patch.release = 1.0f;
    const auto offAt = samples(0.1);
    std::cout << "\n## Question 2: a key played again\n\n"
              << "The init patch with R 1 s (a 110 Hz sine, Retrig on), key 45 on at 0 and off at 100 ms, then on "
                 "again. The level over 100 ms from 10 ms after the second note-on, against one fresh note's over "
                 "the same span of its own; the busy voices after it; the click at the second note-on.\n\n"
              << "| Second note-on (samples) | Phase of the first | New voice: level, voices, click | Own voice "
                 "(retriggered): level, voices, click |\n|---:|---|---|---|\n";
    const auto reference = [&]
    {
        const Setup s{.patch = patch, .length = samples(0.5)};
        return run<BuiltVoice, 4>(s, std::vector{on(0, 45)});
    }();
    const double one = stereoRms(reference.out, samples(0.01), samples(0.1));
    const std::array<std::pair<std::size_t, std::string_view>, 3> cases{
        {
            {12000, "opposite"},
            {12109, "quadrature"},
            {12218, "in phase"},
        },
    };
    double lowest = 0.0;
    double highest = 0.0;
    for (const auto& [second, phase] : cases)
    {
        std::cout << "| " << second << " | " << phase << " |";
        for (const auto key : {RepeatedKey::newVoice, RepeatedKey::ownVoice})
        {
            Setup s{.patch = patch, .length = second + samples(0.3)};
            s.policy.repeatedKey = key;
            const auto r = run<BuiltVoice, 4>(s, std::vector{on(0, 45), off(offAt, 45), on(second, 45)});
            const double level = stereoRms(r.out, second + samples(0.01), samples(0.1));
            if (key == RepeatedKey::newVoice)
            {
                lowest = std::min(lowest, 20.0 * std::log10(level / one));
                highest = std::max(highest, 20.0 * std::log10(level / one));
            }
            const auto busy = r.readouts[(second / s.hostBlock) + 1].notes;
            std::cout << ' ' << std::showpos << fixed(20.0 * std::log10(level / one), 2) << std::noshowpos << " dB, "
                      << busy << ", " << dbText(clickDb(r.out, second)) << " dB |";
        }
        std::cout << '\n';
    }
    // Free-running (Retrig off): the new voice draws its own phase.
    patch.retrigger = false;
    std::cout << "| 12000, Retrig off | the new voice's own random phase |";
    for (const auto key : {RepeatedKey::newVoice, RepeatedKey::ownVoice})
    {
        Setup s{.patch = patch, .length = 12000 + samples(0.3)};
        s.policy.repeatedKey = key;
        const auto r = run<BuiltVoice, 4>(s, std::vector{on(0, 45), off(offAt, 45), on(12000, 45)});
        const double level = stereoRms(r.out, 12000 + samples(0.01), samples(0.1));
        std::cout << ' ' << std::showpos << fixed(20.0 * std::log10(level / one), 2) << std::noshowpos << " dB, "
                  << r.readouts[(12000 / s.hostBlock) + 1].notes << ", " << dbText(clickDb(r.out, 12000)) << " dB |";
    }
    std::cout << "\n\nA new voice adds the first note's release to the new note at a phase set by the timing: "
              << fixed(lowest, 2) << " to +" << fixed(highest, 2)
              << " dB here, held for as long as the release lasts (the 'static' change of character a user reported "
                 "in 2016).\n";
}

// --- Question 3: freeing --------------------------------------------------------------------

void printFreeing()
{
    std::cout << "\n## Question 3: when a voice is freed\n\n";

    // 1. The release: when the envelope passes each threshold.
    const float release = 25.0f * std::pow(0.8f, 4.64f); // knob 8 under E1's law
    {
        dsp::Adsr env;
        env.prepare({.sampleRate = rate, .maxBlockSize = 1, .numChannels = 1});
        env.setRelease(release);
        env.gateOn();
        (void)env.next();
        env.gateOff();
        const std::array<double, 3> thresholds{-60.0, -80.0, -100.0};
        std::array<std::size_t, 3> crossed{};
        std::size_t n = 0;
        while (env.isActive())
        {
            const double db = 20.0 * std::log10(std::max(1e-30, static_cast<double>(env.next())));
            ++n;
            for (std::size_t t = 0; t < thresholds.size(); ++t)
            {
                if (crossed[t] == 0 && db <= thresholds[t])
                {
                    crossed[t] = n;
                }
            }
        }
        std::cout << "A release at knob 8 (" << fixed(release, 3)
                  << " s under E1's law), from the sustain at 1: when the envelope passes each level.\n\n"
                  << "| Freed at | after the note-off | share of the release |\n|---|---:|---:|\n";
        for (std::size_t t = 0; t < thresholds.size(); ++t)
        {
            std::cout << "| " << fixed(thresholds[t], 0) << " dB | " << fixed(secondsOf(crossed[t]), 3) << " s | "
                      << fixed(100.0 * secondsOf(crossed[t]) / static_cast<double>(release), 1) << "% |\n";
        }
        std::cout << "| exactly 0 (E10, decided) | " << fixed(secondsOf(n), 3) << " s | "
                  << fixed(100.0 * secondsOf(n) / static_cast<double>(release), 1) << "% |\n";
    }

    // 2. A threshold's cut: the step it leaves.
    {
        PatchSpec patch = initPatch;
        patch.release = release;
        std::cout << "\nThe same release on the init patch's sine, key held 0.5 s: the voice freed by each rule, and "
                     "the click where it stops (dB re a unit step).\n\n"
                  << "| Rule | freed after the note-off | click at the free |\n|---|---:|---:|\n";
        const auto offAt = samples(0.5);
        for (const double db : {-60.0, -80.0, -100.0, 0.0})
        {
            Setup s{.patch = patch, .polyphony = 1, .length = offAt + samples(static_cast<double>(release) + 0.3)};
            s.hostBlock = 4096;
            s.policy.freeing = db < 0.0 ? Freeing::threshold : Freeing::releaseEnd;
            s.policy.thresholdDb = static_cast<float>(db);
            const auto r = run<BuiltVoice, 4>(s, std::vector{on(0, 45), off(offAt, 45)});
            REQUIRE(!r.freed.empty());
            const auto at = r.freed.front().at;
            std::cout << "| " << (db < 0.0 ? fixed(db, 0) + " dB" : std::string{"release end (exactly 0)"}) << " | "
                      << fixed(secondsOf(at - offAt), 3) << " s | " << dbText(clickDb(r.out, at - 32)) << " |\n";
        }
    }

    // 3. A held note whose envelopes sustain at 0 (E11).
    {
        PatchSpec patch = initPatch;
        patch.decay = 0.3f;
        patch.sustain = 0.0f;
        patch.release = 0.5f;
        const auto length = samples(3.0);
        std::cout << "\nE11: S 0, D 300 ms, R 500 ms, key held 2 s. Then the same with Sustain raised to 0.5 at 1 s "
                     "(automation), key still held.\n\n"
                  << "| Rule | freed at | output identical to release-end | Sustain raised at 1 s: RMS 1.1–1.9 "
                     "s |\n|---|---:|---|---:|\n";
        Buffer reference{2, 1};
        for (const auto freeing : {Freeing::releaseEnd, Freeing::sustainZeroToo})
        {
            Setup s{.patch = patch, .polyphony = 1, .length = length};
            s.policy.freeing = freeing;
            const std::vector events{on(0, 45), off(samples(2.0), 45)};
            const auto r = run<BuiltVoice, 4>(s, events);
            const Probe<BuiltVoice, 4> raise = [&](Allocator<BuiltVoice, 4>& a, std::size_t end)
            {
                if (end == samples(1.0))
                {
                    a.forEachVoice(
                        [](BuiltVoice& v)
                        {
                            v.envelope(0).setSustain(0.5f);
                            v.envelope(1).setSustain(0.5f);
                        });
                }
            };
            Setup raised = s;
            raised.hostBlock = 480;
            const auto rr = run<BuiltVoice, 4>(raised, events, raise);
            if (freeing == Freeing::releaseEnd)
            {
                reference = r.out;
            }
            std::cout << "| " << (freeing == Freeing::releaseEnd ? "release end" : "also sustain-0 holds") << " | "
                      << (r.freed.empty() ? std::string{"—"} : fixed(secondsOf(r.freed.front().at), 3) + " s") << " | "
                      << (sameBits(r.out, reference) ? "yes" : "no") << " | "
                      << fixed(stereoRms(rr.out, samples(1.1), samples(0.8)), 4) << " |\n";
            CHECK(sameBits(r.out, reference));
        }
    }

    // 4. An envelope on a part nothing hears.
    {
        std::cout << "\nAn amp envelope on a part nothing hears: A1 a sine, envelope A R 100 ms, envelope B R 5 s; "
                     "key held 0.5 s. When the voice is freed after the note-off:\n\n"
                  << "| Part B | either envelope keeps it busy (dsp::Voice::isActive) | only audible envelopes "
                     "|\n|---|---:|---:|\n";
        struct Case
        {
            std::string_view name;
            std::array<voice::Input, 2> inputs;
            std::array<int, 4> voices;
        };
        const std::array cases{
            Case{
                .name = "filter B on `--`, no oscillators",
                .inputs = {voice::Input::own, voice::Input::none},
                .voices = {1, 0, 0, 0},
            },
            Case{
                .name = "filter B on B, no oscillators",
                .inputs = {voice::Input::own, voice::Input::own},
                .voices = {1, 0, 0, 0},
            },
            Case{
                .name = "filter B on `--`, B1 a sine (unheard)",
                .inputs = {voice::Input::own, voice::Input::none},
                .voices = {1, 0, 1, 0},
            },
            Case{
                .name = "filter B on B, B1 a sine (heard)",
                .inputs = {voice::Input::own, voice::Input::own},
                .voices = {1, 0, 1, 0},
            },
        };
        for (const auto& c : cases)
        {
            PatchSpec patch = initPatch;
            patch.inputs = c.inputs;
            patch.voices = c.voices;
            patch.releases = {0.1f, 5.0f};
            std::cout << "| " << c.name << " |";
            for (const auto busy : {Busy::anyEnvelope, Busy::audibleEnvelopes})
            {
                Setup s{.patch = patch, .polyphony = 1, .length = samples(6.0)};
                s.hostBlock = 4096;
                s.policy.busy = busy;
                const auto r = run<BuiltVoice, 4>(s, std::vector{on(0, 45), off(samples(0.5), 45)});
                std::cout << ' '
                          << (r.freed.empty() ? std::string{"—"}
                                              : fixed(secondsOf(r.freed.front().at - samples(0.5)), 3) + " s")
                          << " |";
            }
            std::cout << '\n';
        }
    }
}

// --- Question 5: counts and the Voices readout -----------------------------------------------

struct CountStats
{
    double mean = 0.0;
    std::size_t max = 0;
};

CountStats countOver(const Run& r, std::size_t hostBlock, double fromSeconds, double toSeconds)
{
    CountStats c;
    std::size_t n = 0;
    for (std::size_t b = 0; b < r.readouts.size(); ++b)
    {
        const double t = secondsOf((b + 1) * hostBlock);
        if (t < fromSeconds || t > toSeconds)
        {
            continue;
        }
        c.mean += static_cast<double>(r.readouts[b].notes);
        c.max = std::max(c.max, r.readouts[b].notes);
        ++n;
    }
    c.mean /= static_cast<double>(std::max<std::size_t>(n, 1));
    return c;
}

void printCounts()
{
    std::cout << "\n## Question 5: busy voices over time, and the Voices readout\n\n"
              << "Fast notes: 16ths at 120 BPM (125 ms), each held 62.5 ms, for 4 s, on one key (48) or an 8-key "
                 "melody; the init patch's sine with A 5 ms, S 0.7, D 300 ms, R 1 s; Polyphony 16. Busy voices "
                 "(notes) read every 10 ms from 1 s to 4 s.\n\n"
              << "| Rule | same key: mean, max | melody: mean, max | steals (same key / melody) |\n"
                 "|---|---:|---:|---:|\n";
    PatchSpec patch = initPatch;
    patch.attack = 0.005f;
    patch.decay = 0.3f;
    patch.sustain = 0.7f;
    patch.release = 1.0f;
    const std::array<int, 8> melody{60, 62, 64, 65, 67, 69, 71, 72};
    const auto sixteenths = [&](bool sameKey)
    {
        std::vector<Event> e;
        for (std::size_t i = 0; i < 32; ++i)
        {
            const int key = sameKey ? 48 : melody[i % melody.size()];
            e.push_back(on(i * 6000, key));
            e.push_back(off((i * 6000) + 3000, key));
        }
        return sorted(e);
    };
    const auto events = sixteenths(true);
    const auto melodyEvents = sixteenths(false);
    struct Rule
    {
        std::string_view name;
        RepeatedKey key;
        Freeing freeing;
    };
    const std::array rules{
        Rule{
            .name = "new voice per note, freed at release end (the developer; E10)",
            .key = RepeatedKey::newVoice,
            .freeing = Freeing::releaseEnd,
        },
        Rule{
            .name = "new voice per note, freed at −80 dB",
            .key = RepeatedKey::newVoice,
            .freeing = Freeing::threshold,
        },
        Rule{
            .name = "own voice for a repeated key, freed at release end",
            .key = RepeatedKey::ownVoice,
            .freeing = Freeing::releaseEnd,
        },
    };
    for (const auto& rule : rules)
    {
        Setup s{.patch = patch, .length = samples(4.0), .hostBlock = 480};
        s.policy.repeatedKey = rule.key;
        s.policy.freeing = rule.freeing;
        const auto same = run<BuiltVoice>(s, events);
        const auto mel = run<BuiltVoice>(s, melodyEvents);
        const auto a = countOver(same, s.hostBlock, 1.0, 4.0);
        const auto b = countOver(mel, s.hostBlock, 1.0, 4.0);
        std::cout << "| " << rule.name << " | " << fixed(a.mean, 2) << ", " << a.max << " | " << fixed(b.mean, 2)
                  << ", " << b.max << " | " << same.stats.steals << " / " << mel.stats.steals << " |\n";
    }

    // The readout: the manual's screenshot reads Polyphony 12, Voices 204/336.
    PatchSpec readoutPatch = initPatch;
    readoutPatch.voices = {8, 4, 8, 8};
    readoutPatch.releases = {0.3f, 2.0f};
    const Setup s{.patch = readoutPatch, .polyphony = 12, .length = samples(1.5), .hostBlock = 480};
    std::vector<Event> e;
    for (int k = 0; k < 9; ++k)
    {
        e.push_back(on(static_cast<std::size_t>(k) * 96, 48 + k));
        if (k < 4)
        {
            e.push_back(off(samples(1.0), 48 + k));
        }
    }
    const auto r = run<BuiltVoice>(s, sorted(e));
    const auto& last = r.readouts.back();
    std::cout << "\nThe readout's candidates, against the manual's screenshot (p. 11: Polyphony 12, Voices 204/336). "
                 "A1 8 voices, A2 4, B1 8, B2 8 (28 per note, so the maximum is 12 × 28 = 336); envelope A R 300 "
                 "ms, B R 2 s; 9 notes held, 4 released at 1 s; read at 1.5 s:\n\n"
              << "| Count | reads |\n|---|---:|\n"
              << "| notes | " << last.notes << " |\n"
              << "| R1: notes × every oscillator's voices | " << last.perNoteVoices << " |\n"
              << "| R2: per note, the voices of parts whose amp envelope is active | " << last.perPartVoices << " |\n"
              << "| the maximum (the manual's formula) | " << last.maximum << " |\n";
    CHECK(last.maximum == 336);
    CHECK(last.perPartVoices == 204);
}

// --- Question 1: the policies -----------------------------------------------------------------

std::string_view stealName(Steal s)
{
    switch (s)
    {
    case Steal::oldest:
        return "oldest";
    case Steal::quietest:
        return "quietest";
    case Steal::releasingThenOldest:
        return "releasing first, then oldest";
    case Steal::sameKeyThenOldest:
        return "same key first, then oldest";
    }
    return "";
}

void printPolicies()
{
    constexpr std::array steals{Steal::oldest, Steal::quietest, Steal::releasingThenOldest, Steal::sameKeyThenOldest};

    // A 32-note strum at Polyphony 16.
    std::cout << "\n## Question 1: which voice a new note takes\n\n"
              << "A 32-note strum, keys 36–67 rising 2 ms apart and held, at Polyphony 16; the resonant pad (A 200 "
                 "ms) and the init patch (A 0); retrigger in place. The keys sounding at 1 s.\n\n"
              << "| Policy | pad: keys sounding | init: keys sounding | steals |\n|---|---|---|---:|\n";
    std::vector<Event> strum;
    strum.reserve(32);
    for (int k = 0; k < 32; ++k)
    {
        strum.push_back(on(static_cast<std::size_t>(k) * 96, 36 + k));
    }
    const auto keysAt = [](const PatchSpec& patch, Steal steal, std::span<const Event> events, std::size_t at,
                           std::size_t poly, Stats* stats)
    {
        std::vector<int> keys;
        Setup s{.patch = patch, .polyphony = poly, .length = at};
        s.policy.steal = steal;
        const Probe<BuiltVoice, 32> probe = [&](Allocator<BuiltVoice, 32>& a, std::size_t end)
        {
            if (end == at)
            {
                for (std::size_t i = 0; i < 32; ++i)
                {
                    if (a.isCounted(i))
                    {
                        keys.push_back(a.noteOf(i));
                    }
                }
            }
        };
        const auto r = run<BuiltVoice, 32>(s, events, probe);
        if (stats != nullptr)
        {
            *stats = r.stats;
        }
        std::ranges::sort(keys);
        return keys;
    };
    const auto keyText = [](const std::vector<int>& keys)
    {
        std::string text;
        int runStart = -1;
        int previous = -2;
        const auto flush = [&]
        {
            if (runStart < 0)
            {
                return;
            }
            text += (text.empty() ? "" : ", ") + std::to_string(runStart) +
                    (previous > runStart ? "–" + std::to_string(previous) : "");
        };
        for (const int k : keys)
        {
            if (k != previous + 1)
            {
                flush();
                runStart = k;
            }
            previous = k;
        }
        flush();
        return text;
    };
    for (const auto steal : steals)
    {
        Stats stats;
        const auto pad = keysAt(padPatch, steal, strum, samples(1.0) - (samples(1.0) % 512), 16, &stats);
        const auto init = keysAt(initPatch, steal, strum, samples(1.0) - (samples(1.0) % 512), 16, nullptr);
        std::cout << "| " << stealName(steal) << " | " << keyText(pad) << " | " << keyText(init) << " | "
                  << stats.steals << " |\n";
    }

    // A held bass under a melody, Polyphony 8.
    std::cout << "\nA bass key (36) held from 0 to 4 s under a melody of 16ths (8 keys, 60–72, each held 62.5 ms) "
                 "from 0.5 s to 3.5 s; the init patch's sine with R 1 s; Polyphony 8; retrigger in place. Whether "
                 "the bass still sounds at 3.9 s, the steals, and how many of them took a note whose key was still "
                 "down.\n\n"
              << "| Policy | bass sounding at 3.9 s | steals | of a held key |\n|---|---|---:|---:|\n";
    PatchSpec patch = initPatch;
    patch.release = 1.0f;
    std::vector<Event> e{on(0, 36), off(samples(4.0), 36)};
    const std::array<int, 8> melody{60, 62, 64, 65, 67, 69, 71, 72};
    for (std::size_t i = 0; i < 24; ++i)
    {
        const auto at = samples(0.5) + (i * 6000);
        e.push_back(on(at, melody[i % 8]));
        e.push_back(off(at + 3000, melody[i % 8]));
    }
    e = sorted(e);
    for (const auto steal : steals)
    {
        Stats stats;
        const auto keys = keysAt(patch, steal, e, samples(3.9) - (samples(3.9) % 512), 8, &stats);
        const bool bass = std::ranges::find(keys, 36) != keys.end();
        std::cout << "| " << stealName(steal) << " | " << (bass ? "yes" : "no, stolen") << " | " << stats.steals
                  << " | " << stats.stolenHeld << " |\n";
    }

    // How many spare voices a fade on a spare needs.
    std::cout << "\nSpares for a fade on a spare voice: the resonant pad at Polyphony 16, 16 keys held, then 16 more "
                 "(each a steal) strummed 2 ms apart or all at one sample; the most fading at once, and the steals "
                 "that found no spare (and cut instead), for pools of 16 + 16, 16 + 4 and 16 + 1 voices.\n\n"
              << "| Fade | strummed: 32 / 20 / 17 voices | one sample: 32 / 20 / 17 voices |\n|---|---|---|\n";
    const auto chord = [](std::size_t spacing)
    {
        std::vector<Event> e;
        for (int k = 0; k < 32; ++k)
        {
            const auto at =
                k < 16 ? static_cast<std::size_t>(k) * 96 : samples(0.5) + (static_cast<std::size_t>(k - 16) * spacing);
            e.push_back(on(at, 36 + k));
        }
        return sorted(e);
    };
    for (const float fade : {0.005f, 0.010f})
    {
        std::cout << "| " << fixed(static_cast<double>(fade) * 1000.0, 0) << " ms |";
        for (const std::size_t spacing : {std::size_t{96}, std::size_t{0}})
        {
            Setup s{.patch = padPatch, .polyphony = 16, .length = samples(0.6)};
            s.policy.end = StealEnd::fadeOnSpare;
            s.policy.fadeSeconds = fade;
            const auto events = chord(spacing);
            const auto a = run<BuiltVoice, 32>(s, events).stats;
            const auto b = run<BuiltVoice, 20>(s, events).stats;
            const auto c = run<BuiltVoice, 17>(s, events).stats;
            std::cout << ' ' << a.maxFading << " fading, " << a.spareExhausted << " cut / " << b.maxFading << ", "
                      << b.spareExhausted << " / " << c.maxFading << ", " << c.spareExhausted << " |";
        }
        std::cout << '\n';
    }
}

// --- Question 4: Polyphony lowered and 0 ------------------------------------------------------

void printPolyphonyChanges()
{
    std::cout << "\n## Question 4: Polyphony lowered, and Polyphony 0\n\n"
              << "The resonant pad, keys 48–55 strummed (2 ms apart) at 0 and held to 3 s; Polyphony 8 → 4 at 1 s, "
                 "→ 0 at 2 s; key 60 on at 2.5 s (off at 2.8 s); Polyphony back to 8 at 4 s. Busy voices at each "
                 "time, and the click at 1 s (dB re a unit step) against the same window at 0.8 s (the chord's own "
                 "energy above 1 kHz).\n\n"
              << "| Rule | 0.9 s | 1.5 s | 2.2 s | 2.6 s | 3.5 s | 5.5 s | at 0.8 s | click at 1 s | note-ons ignored "
                 "|\n|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    std::vector<Event> e;
    for (int k = 0; k < 8; ++k)
    {
        e.push_back(on(static_cast<std::size_t>(k) * 96, 48 + k));
        e.push_back(off(samples(3.0), 48 + k));
    }
    e.push_back(polyphony(samples(1.0), 4));
    e.push_back(polyphony(samples(2.0), 0));
    e.push_back(on(samples(2.5), 60));
    e.push_back(off(samples(2.8), 60));
    e.push_back(polyphony(samples(4.0), 8));
    e = sorted(e);
    struct Drop
    {
        PolyphonyDrop drop;
        std::string_view name;
    };
    const std::array drops{
        Drop{.drop = PolyphonyDrop::leave, .name = "leave them (only new notes obey)"},
        Drop{.drop = PolyphonyDrop::release, .name = "release the oldest over the limit"},
        Drop{.drop = PolyphonyDrop::cut, .name = "cut the oldest over the limit"},
    };
    for (const auto& [drop, name] : drops)
    {
        Setup s{.patch = padPatch, .polyphony = 8, .length = samples(5.6), .hostBlock = 480};
        s.policy.drop = drop;
        const auto r = run<BuiltVoice>(s, e);
        std::cout << "| " << name << " |";
        for (const double t : {0.9, 1.5, 2.2, 2.6, 3.5, 5.5})
        {
            std::cout << ' ' << r.readouts[(samples(t) / s.hostBlock) - 1].notes << " |";
        }
        std::cout << ' ' << dbText(clickDb(r.out, samples(0.8))) << " | " << dbText(clickDb(r.out, samples(1.0)))
                  << " | " << r.stats.ignored << " |\n";
        CHECK(r.readouts[(samples(5.5) / s.hostBlock) - 1].notes == 0);
    }
}

// --- The allocator's real-time terms ------------------------------------------------------------

void printRealTime()
{
    std::cout << "\n## Real-time terms: block sizes, allocation, seeds, a freed voice's reuse\n\n";
    // A sequence with steals, releases, a Polyphony change: the resonant pad
    // free-running with drift on, Polyphony 4.
    PatchSpec patch = padPatch;
    patch.driftCents = dsp::PitchDrift::defaultDepthCents;
    std::vector<Event> e{on(0, 36), off(samples(1.5), 36)};
    const std::array<int, 8> melody{60, 62, 64, 65, 67, 69, 71, 72};
    for (std::size_t i = 0; i < 14; ++i)
    {
        const auto at = samples(0.1) + (i * 6000) + (i * 7);
        e.push_back(on(at, melody[i % 8]));
        e.push_back(off(at + 3000 + i, melody[i % 8]));
    }
    e.push_back(polyphony(samples(1.0) + 5, 2));
    e = sorted(e);
    struct Case
    {
        std::string_view name;
        StealEnd end;
        Steal steal;
    };
    const std::array cases{
        Case{.name = "retrigger, releasing first", .end = StealEnd::retrigger, .steal = Steal::releasingThenOldest},
        Case{.name = "cut, oldest", .end = StealEnd::cut, .steal = Steal::oldest},
        Case{.name = "fade 5 ms then start, quietest", .end = StealEnd::fadeThenStart, .steal = Steal::quietest},
        Case{
            .name = "fade 5 ms on a spare, same key first",
            .end = StealEnd::fadeOnSpare,
            .steal = Steal::sameKeyThenOldest,
        },
    };
    std::cout << "The resonant pad free-running with drift on (1.5 cents), Polyphony 4 (2 from 1 s), a held key "
                 "and 14 melody notes at offsets off every grid; 2 s rendered at host blocks of 1, 37 and 4096 "
                 "against 4096; allocations counted from prepare() on.\n\n"
              << "| Policy | identical to the bit | allocations | steals |\n|---|---:|---:|---:|\n";
    for (const auto& c : cases)
    {
        std::size_t same = 0;
        std::size_t allocations = 0;
        Setup s{.patch = patch, .polyphony = 4, .length = samples(2.0), .hostBlock = 4096};
        s.policy.end = c.end;
        s.policy.steal = c.steal;
        const auto reference = run<BuiltVoice>(s, e);
        for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
        {
            s.hostBlock = block;
            const auto r = run<BuiltVoice>(s, e);
            same += sameBits(r.out, reference.out) ? 1 : 0;
            allocations += r.allocations;
        }
        std::cout << "| " << c.name << " | " << same << " of 3 | " << allocations << " | " << reference.stats.steals
                  << " |\n";
        CHECK(same == 3);
        CHECK(allocations == 0);
    }

    // Seeds from the index: two voices, the same free-running note.
    {
        GlobalKnobs knobs{knobsFor(padPatch), knobsFor(padPatch), rate};
        const Context context{.table = &d6::minBlepTable64(), .knobs = &knobs};
        auto a = std::make_unique<BuiltVoice>(context, std::size_t{0});
        auto b = std::make_unique<BuiltVoice>(context, std::size_t{1});
        auto c = std::make_unique<BuiltVoice>(context, std::size_t{0});
        Buffer outA{2, 4096};
        Buffer outB{2, 4096};
        Buffer outC{2, 4096};
        for (auto* v : {a.get(), b.get(), c.get()})
        {
            apply(*v, padPatch);
            v->prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 2});
            v->start(45.0f, 0);
        }
        a->render(outA.block(), 0);
        b->render(outB.block(), 0);
        c->render(outC.block(), 0);
        std::cout << "\nSeeds from the voice's index: two voices (index 0 and 1) on the same free-running note "
                     "differ: "
                  << (sameBits(outA, outB) ? "no" : "yes")
                  << "; the same index twice is identical: " << (sameBits(outA, outC) ? "yes" : "no") << ".\n";
        CHECK(!sameBits(outA, outB));
        CHECK(sameBits(outA, outC));
    }

    // A freed voice reused, with and without stopping it.
    {
        PatchSpec p = initPatch;
        p.release = 0.1f;
        const auto second = samples(1.0);
        const Setup fresh{.patch = p, .polyphony = 1, .length = samples(0.2)};
        const auto reference = run<BuiltVoice, 4>(fresh, std::vector{on(0, 52)});
        std::cout << "\nA voice reused after it finished (the init patch's sine, R 100 ms, key 45 held 0.2 s, key 52 "
                     "at 1 s on the same voice), against key 52 on a fresh voice; Retrig on:\n\n"
                  << "| The freed voice | largest difference over 200 ms |\n|---|---:|\n";
        for (const bool stop : {true, false})
        {
            Setup s{.patch = p, .polyphony = 1, .length = second + samples(0.2)};
            s.policy.stopOnFree = stop;
            const auto r = run<BuiltVoice, 4>(s, std::vector{on(0, 45), off(samples(0.2), 45), on(second, 52)});
            double worst = 0.0;
            for (std::size_t i = 0; i < samples(0.2); ++i)
            {
                worst = std::max(
                    worst, static_cast<double>(std::abs(r.out.channel(0)[second + i] - reference.out.channel(0)[i])));
            }
            std::cout << "| " << (stop ? "stopped (slots, drift, envelopes)" : "left as it stood (dsp::Voice today)")
                      << " | " << (worst > 0.0 ? fixed(worst, 4) : std::string{"0 (identical)"}) << " |\n";
            if (stop)
            {
                CHECK(worst <= 0.0);
            }
        }
    }
}

} // namespace

TEST_CASE("Voice allocation report: the candidate allocators on dsp::Voice", "[.][voice-allocation-report]")
{
    std::cout << "# Voice allocation report (M1.4 task 2, stage 1)\n";
    printSteals();
    printRepeatedKey();
    printFreeing();
    printCounts();
    printPolicies();
    printPolyphonyChanges();
    printRealTime();
}
