// The legato and portamento report (docs/dsp/legato-portamento.md, M1.4
// task 3, stage 1): measures the candidates in legato_candidates.hpp on the
// prototype PitchedVoice (dsp::Voice with a pitch input) through the
// prototype Engine (the decided VoiceAllocator with a mono mode, the pedal
// and the glide's bookkeeping):
//
// - the prototype against the library: PitchedVoice against dsp::Voice, and
//   the Engine and VoiceAllocator<PitchedVoice> against
//   VoiceAllocator<dsp::Voice>, bit for bit at a constant pitch;
// - Mono Legato's note priorities and a released key's return;
// - the click at a legato step and at a returning key (energy above 1 kHz
//   in a Hann 4096 centred 32 samples after the event, dB re a unit step,
//   as filters.md and voice-allocation.md measure);
// - the velocity at a legato note (on the amp, as a stand-in route);
// - the glide's pitch against its formula, read period by period, for each
//   curve, time law and pitch rate; glide lengths; an interrupted glide;
// - Normal and Slide, in mono and poly, and where a poly voice glides from;
// - the bend: the zipper of 7-bit and 14-bit wheels, stepped or ramped, per
//   sample or per grid point; its range's ends; a released voice and the
//   wheel;
// - the filter following the glide and the bend at keytrack 10;
// - the hold pedal: a key played again (P7), the steal order, CC 64
//   thresholds, the pedal in Mono Legato;
// - real-time terms: the same output at host blocks of 1, 37 and 4096, and
//   no allocation;
// - the portamento knob's candidate laws.
//
// Hidden from the default run; run it in an optimised build (about 30 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[legato-report]"
//
// CPU: the "[legato]" benchmarks (tools/bench.sh "[legato]").

#include "legato_candidates.hpp"

#include "../filters/filter_control_support.hpp"
#include "../filters/keytrack_support.hpp"
#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::legato;

namespace
{

constexpr double rate = 48000.0;
constexpr float rateF = 48000.0f;

std::string fixed(double x, int digits)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

std::size_t samples(double seconds)
{
    return static_cast<std::size_t>(std::llround(seconds * rate));
}

// --- Patches and knobs --------------------------------------------------------------------------

struct PatchSpec
{
    dsp::Waveform wave = dsp::Waveform::sine;
    std::array<int, 4> voices{1, 0, 0, 0};
    float detune = 0.0f;
    float stereo = 0.0f;
    bool retrigger = true;
    float volume = 1.0f;
    float attack = 0.0f;
    float decay = 0.0f;
    float sustain = 1.0f;
    float release = 0.0f;
    std::array<dsp::FilterInput, 2> inputs{dsp::FilterInput::own, dsp::FilterInput::own};
    std::array<bool, 2> bypass{false, false};
    float drive = 1.0f;
    float driftCents = 0.0f;
};

/// The global knobs, constant: each filter's Cutoff and Resonance, the
/// Filter Control's, and Keytrack.
struct KnobValues
{
    float cutoff = 5.0f;
    float resonance = 0.0f;
    float controlCutoff = 5.0f;
    float controlResonance = 0.0f;
    float keytrack = 0.0f;
};

struct KnobSet
{
    explicit KnobSet(const KnobValues& v) : keytrack(v.keytrack)
    {
        cutoff.setCurrentAndTarget(v.cutoff);
        resonance.setCurrentAndTarget(v.resonance);
        controlCutoff.setCurrentAndTarget(v.controlCutoff);
        controlResonance.setCurrentAndTarget(v.controlResonance);
    }
    [[nodiscard]] dsp::VoiceKnobs filter() const noexcept
    {
        return {
            .cutoff = {&cutoff, &cutoff},
            .resonance = {&resonance, &resonance},
            .controlCutoff = &controlCutoff,
            .controlResonance = &controlResonance,
            .keytrack = keytrack,
        };
    }
    dsp::LinearSmoother cutoff;
    dsp::LinearSmoother resonance;
    dsp::LinearSmoother controlCutoff;
    dsp::LinearSmoother controlResonance;
    float keytrack = 0.0f;
};

/// The init patch as far as the evidence goes, with a sine: A1 one voice,
/// LP24 at 4,677 Hz (Cutoff 5 + 5), k 0, the envelopes a gate, Retrig on.
const PatchSpec initPatch{};
const KnobValues initKnobs{};

/// Rings long but stays clean: one sine into LP24 at 585 Hz, k 3.96; A 5 ms,
/// S 1, R 300 ms, Retrig on (voice-allocation.md's ringing patch).
const PatchSpec ringingPatch{.attack = 0.005f, .release = 0.3f};
const KnobValues ringingKnobs{.cutoff = 2.0f, .resonance = 6.6f};

/// A resonant pad: A1 and A2 four detuned sines each, spread, free-running,
/// LP24 at 585 Hz, k 3.6; A 200 ms, D 500 ms, S 0.8, R 1 s.
const PatchSpec padPatch{
    .voices = {4, 4, 0, 0},
    .detune = 0.3f,
    .stereo = 1.0f,
    .retrigger = false,
    .attack = 0.2f,
    .decay = 0.5f,
    .sustain = 0.8f,
    .release = 1.0f,
};
const KnobValues padKnobs{.cutoff = 2.0f, .resonance = 6.0f};

/// For reading pitch: one sine, filter A bypassed (so unfiltered).
const PatchSpec pitchPatch{.bypass = {true, false}};

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
        slot.setVolume(p.volume);
    }
    for (std::size_t e = 0; e < 2; ++e)
    {
        auto& env = v.envelope(e);
        env.setAttack(p.attack);
        env.setDecay(p.decay);
        env.setSustain(p.sustain);
        env.setRelease(p.release);
    }
    for (std::size_t f = 0; f < 2; ++f)
    {
        v.setFilterInput(f, p.inputs[f]);
        v.setFilterBypassed(f, p.bypass[f]);
        v.setFilterDrive(f, p.drive);
    }
    v.drift().setDepthCents(p.driftCents);
}

// --- Events and rendering -------------------------------------------------------------------------

Event on(std::size_t at, int key, int velocity = 100)
{
    return {.offset = at, .kind = EventKind::noteOn, .value = key, .velocity = velocity};
}
Event off(std::size_t at, int key)
{
    return {.offset = at, .kind = EventKind::noteOff, .value = key};
}
Event pedalAt(std::size_t at, int value)
{
    return {.offset = at, .kind = EventKind::pedal, .value = value};
}
Event bendAt(std::size_t at, int value)
{
    return {.offset = at, .kind = EventKind::bend, .value = value};
}
Event polyphonyAt(std::size_t at, int notes)
{
    return {.offset = at, .kind = EventKind::polyphony, .value = notes};
}

std::vector<Event> sorted(std::vector<Event> events)
{
    std::ranges::stable_sort(events, {}, &Event::offset);
    return events;
}

using TestEngine = Engine<16>;
using Probe = std::function<void(TestEngine&, std::size_t)>;

struct Scenario
{
    PatchSpec patch = initPatch;
    KnobValues knobs = initKnobs;
    Policy policy{};
    std::size_t polyphony = 16;
    std::size_t length = 0;
    std::size_t hostBlock = 512;
    std::uint64_t seed = 0;
    std::vector<std::size_t> checks; ///< the probe is called at each (blocks split there)
};

struct Rendered
{
    Buffer out{2, 1};
    std::size_t allocations = 0;
    Counts counts{};
};

/// The engine over absolute-time `events`, in host blocks (split at the
/// checks too, where `probe` is called).
Rendered runEngine(const Scenario& s, std::span<const Event> events, const Probe& probe = {})
{
    const KnobSet knobs{s.knobs};
    auto engine = std::make_unique<TestEngine>(s.seed, d6::minBlepTable64());
    engine->forEachVoice([&](TestEngine::Voice& v) { apply(v, s.patch); });
    engine->setPolicy(s.policy);
    engine->setPolyphony(s.polyphony);
    Rendered r;
    r.out = Buffer{2, s.length};
    const auto block = r.out.block();
    std::vector<Event> blockEvents;
    blockEvents.reserve(events.size());
    std::size_t check = 0;
    {
        const AllocationCounter counter;
        engine->prepare({.sampleRate = rate, .maxBlockSize = s.hostBlock, .numChannels = 2});
        std::size_t e = 0;
        for (std::size_t start = 0; start < s.length;)
        {
            auto end = std::min(s.length, start + s.hostBlock);
            while (check < s.checks.size() && s.checks[check] <= start)
            {
                ++check;
            }
            if (check < s.checks.size() && s.checks[check] < end)
            {
                end = s.checks[check];
            }
            blockEvents.clear();
            while (e < events.size() && events[e].offset < end)
            {
                auto ev = events[e];
                ev.offset -= start;
                blockEvents.push_back(ev);
                ++e;
            }
            const VoiceKnobs k{.filter = knobs.filter(), .bend = nullptr, .at = start};
            engine->process(block.subBlock(start, end - start), blockEvents, k);
            if (probe && check < s.checks.size() && s.checks[check] == end)
            {
                probe(*engine, end);
            }
            start = end;
        }
        r.allocations = counter.count();
    }
    r.counts = engine->counts();
    return r;
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
    return std::isfinite(db) ? fixed(db, 1) : "silent";
}

// --- 0. The prototype against the library ------------------------------------------------------

template <typename V>
V::Knobs knobsOf(const dsp::VoiceKnobs& filter, std::size_t at)
{
    if constexpr (std::is_same_v<typename V::Knobs, dsp::VoiceKnobs>)
    {
        (void)at;
        return filter;
    }
    else
    {
        return VoiceKnobs{.filter = filter, .bend = nullptr, .at = at};
    }
}

/// One voice: key 45 at 0, key 52 retriggered on it at 24013, off at 40000.
template <typename V>
Buffer runVoice(const PatchSpec& patch, const KnobValues& kv, std::size_t hostBlock, const VoiceOptions* options)
{
    const KnobSet knobs{kv};
    auto v = std::make_unique<V>(d6::minBlepTable64(), 7);
    apply(*v, patch);
    if constexpr (!std::is_same_v<V, dsp::Voice<>>)
    {
        if (options != nullptr)
        {
            v->setOptions(*options);
        }
    }
    v->prepare({.sampleRate = rate, .maxBlockSize = hostBlock, .numChannels = 2});
    constexpr std::size_t length = 60000;
    const std::array<std::size_t, 3> at{0, 24013, 40000};
    Buffer out{2, length};
    const auto block = out.block();
    std::size_t next = 0;
    for (std::size_t start = 0; start < length;)
    {
        auto end = std::min(length, start + hostBlock);
        if (next < at.size() && at[next] > start && at[next] < end)
        {
            end = at[next];
        }
        if (next < at.size() && at[next] == start)
        {
            const auto k = knobsOf<V>(knobs.filter(), start);
            if (next == 2)
            {
                v->noteOff();
            }
            else
            {
                v->noteOn(next == 0 ? 45.0f : 52.0f, k);
            }
            ++next;
        }
        v->process(block.subBlock(start, end - start), knobsOf<V>(knobs.filter(), start));
        start = end;
    }
    return out;
}

/// The allocator sequence of voice_allocator_test.cpp: steals, releases, a
/// repeated key, Polyphony 4 -> 2 -> 6, the pad with drift.
std::vector<Event> allocatorSequence()
{
    std::vector<Event> e{on(0, 36), off(50000, 36), polyphonyAt(30001, 2), polyphonyAt(60007, 6)};
    const std::array<int, 6> melody{60, 64, 67, 60, 72, 64};
    for (std::size_t i = 0; i < 16; ++i)
    {
        const auto at = 1000 + (i * 4001);
        e.push_back(on(at, melody[i % melody.size()]));
        e.push_back(off(at + 2000 + i, melody[i % melody.size()]));
    }
    return sorted(e);
}

dsp::VoiceEventKind kindOf(EventKind k)
{
    switch (k)
    {
    case EventKind::noteOff:
        return dsp::VoiceEventKind::noteOff;
    case EventKind::polyphony:
        return dsp::VoiceEventKind::polyphony;
    case EventKind::noteOn:
    case EventKind::pedal:
    case EventKind::bend:
        break;
    }
    return dsp::VoiceEventKind::noteOn;
}

template <typename V>
Buffer runAllocator(const PatchSpec& patch, const KnobValues& kv, std::span<const Event> events, std::size_t length,
                    std::size_t hostBlock)
{
    const KnobSet knobs{kv};
    auto alloc = std::make_unique<dsp::VoiceAllocator<V>>(0, d6::minBlepTable64());
    alloc->forEachVoice([&](V& v) { apply(v, patch); });
    alloc->setPolyphony(4);
    alloc->prepare({.sampleRate = rate, .maxBlockSize = hostBlock, .numChannels = 2});
    Buffer out{2, length};
    const auto block = out.block();
    std::vector<dsp::VoiceEvent> blockEvents;
    std::size_t e = 0;
    for (std::size_t start = 0; start < length; start += hostBlock)
    {
        const auto end = std::min(length, start + hostBlock);
        blockEvents.clear();
        while (e < events.size() && events[e].offset < end)
        {
            blockEvents.push_back(
                {.offset = events[e].offset - start, .kind = kindOf(events[e].kind), .value = events[e].value});
            ++e;
        }
        alloc->process(block.subBlock(start, end - start), blockEvents, knobsOf<V>(knobs.filter(), start));
    }
    return out;
}

void printEquivalence()
{
    std::cout << "\n## 0. The prototype against the library (bit for bit, at a constant pitch)\n\n";
    std::cout << "| Patch | rate | blocks 1 / 37 / 4096 against dsp::Voice at 4096 |\n|---|---|---|\n";
    struct Case
    {
        std::string_view name;
        PatchSpec patch;
        KnobValues knobs;
    };
    PatchSpec padDrift = padPatch;
    padDrift.driftCents = 1.5f;
    const std::array cases{
        Case{.name = "init (sine)", .patch = initPatch, .knobs = initKnobs},
        Case{.name = "ringing", .patch = ringingPatch, .knobs = ringingKnobs},
        Case{.name = "pad, drift on", .patch = padDrift, .knobs = padKnobs},
    };
    const std::array rates{
        std::pair{PitchRate::gridStart, "grid, held from the start"},
        std::pair{PitchRate::gridMid, "grid, held from the midpoint"},
        std::pair{PitchRate::gridRamped, "grid, ramped"},
        std::pair{PitchRate::perSample, "per sample"},
    };
    for (const auto& c : cases)
    {
        const auto reference = runVoice<dsp::Voice<>>(c.patch, c.knobs, 4096, nullptr);
        for (const auto& [r, name] : rates)
        {
            const VoiceOptions o{.rate = r};
            std::string cells;
            for (const std::size_t b : {1U, 37U, 4096U})
            {
                const auto x = runVoice<PitchedVoice<>>(c.patch, c.knobs, b, &o);
                cells +=
                    std::string(cells.empty() ? "" : " / ") + (bitIdentical(x, reference) ? "identical" : "differs");
            }
            std::cout << "| " << c.name << " | " << name << " | " << cells << " |\n";
        }
    }
    const auto events = allocatorSequence();
    constexpr std::size_t length = 72000;
    const auto library = runAllocator<dsp::Voice<>>(padDrift, padKnobs, events, length, 37);
    Scenario const s{.patch = padDrift, .knobs = padKnobs, .polyphony = 4, .length = length, .hostBlock = 37};
    const auto engine = runEngine(s, events);
    std::cout << "\nThe allocator sequence (the pad with drift, steals, a repeated key, Polyphony 4 -> 2 -> 6), "
                 "blocks of 37:\n\n";
    std::cout << "- Engine (poly, no glide, bend or pedal) against VoiceAllocator<dsp::Voice>: "
              << (bitIdentical(engine.out, library) ? "identical" : "differs") << "\n";
}

// --- 1. Note priority, and a released key -------------------------------------------------------

std::string keyName(int key)
{
    static constexpr std::array<std::string_view, 12> names{
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
    };
    if (key < 0)
    {
        return "-";
    }
    return std::string(names[static_cast<std::size_t>(key % 12)]) + std::to_string((key / 12) - 1);
}

void printPriorities()
{
    std::cout << "\n## 1. Mono Legato: note priority and a released key\n\n";
    // C3, E3 and G3 pressed in turn and released in a different order, then
    // a trill on top of a held note.
    struct Step
    {
        std::string_view what;
        Event event;
    };
    const std::vector<Step> steps{
        {.what = "C3 down", .event = on(1000, 48)}, {.what = "G3 down", .event = on(2000, 55)},
        {.what = "E3 down", .event = on(3000, 52)}, {.what = "E3 up", .event = off(4000, 52)},
        {.what = "G3 up", .event = off(5000, 55)},  {.what = "G3 down", .event = on(6000, 55)},
        {.what = "C3 up", .event = off(7000, 48)},  {.what = "A3 down", .event = on(8000, 57)},
        {.what = "A3 up", .event = off(9000, 57)},  {.what = "G3 up", .event = off(10000, 55)},
    };
    const std::array priorities{
        std::pair{Priority::last, "last"},
        std::pair{Priority::low, "low"},
        std::pair{Priority::high, "high"},
    };
    std::cout << "The key sounding after each event, and whether the voice's gate is on (Portamento 0, Normal):\n\n";
    std::cout << "| Event |";
    for (const auto& p : priorities)
    {
        std::cout << " " << p.second << " |";
    }
    std::cout << "\n|---|";
    for (std::size_t i = 0; i < priorities.size(); ++i)
    {
        std::cout << "---|";
    }
    std::cout << "\n";
    std::vector<std::vector<std::string>> cells(steps.size());
    std::vector<Counts> counts;
    for (const auto& [priority, name] : priorities)
    {
        Scenario s{.length = 12000, .hostBlock = 64};
        s.policy.monoLegato = true;
        s.policy.priority = priority;
        std::vector<Event> events;
        for (const auto& st : steps)
        {
            events.push_back(st.event);
            s.checks.push_back(st.event.offset + 1);
        }
        std::size_t index = 0;
        const auto r =
            runEngine(s, events,
                      [&](TestEngine& e, std::size_t /*at*/)
                      {
                          cells[index].push_back(keyName(e.soundingKey()) + (e.isGateOn(0) ? "" : " (released)"));
                          ++index;
                      });
        counts.push_back(r.counts);
    }
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        std::cout << "| " << steps[i].what << " |";
        for (const auto& c : cells[i])
        {
            std::cout << " " << c << " |";
        }
        std::cout << "\n";
    }
    std::cout << "\nLegato notes / retriggers per priority:";
    for (std::size_t p = 0; p < priorities.size(); ++p)
    {
        std::cout << " " << priorities[p].second << " " << counts[p].legatoNotes << " / " << counts[p].retriggers
                  << ";";
    }
    std::cout << "\n";
}

// --- 1b. Clicks at a legato step and at a returning key ----------------------------------------

struct ClickCase
{
    std::string_view name;
    bool mono = true;
    bool restartPhases = false;
    ReturnTo returnTo = ReturnTo::legato;
    bool returning = false; ///< the event is a release that returns to a held key
    bool steady = false;    ///< no event: the patch's own level above 1 kHz
    int steadyKey = 45;     ///< the key held for a steady row
};

double clickFor(const PatchSpec& patch, const KnobValues& knobs, const ClickCase& c, float glideSeconds)
{
    constexpr std::size_t at = 24013;
    Scenario s{.patch = patch, .knobs = knobs, .length = at + samples(0.15), .hostBlock = 512};
    s.policy.monoLegato = c.mono;
    s.policy.legatoRestartsPhases = c.restartPhases;
    s.policy.returnTo = c.returnTo;
    s.policy.glide = {.seconds = glideSeconds};
    if (!c.mono)
    {
        s.polyphony = 1;
    }
    std::vector<Event> events{on(0, c.steady ? c.steadyKey : 45)};
    if (c.returning)
    {
        events.push_back(on(12007, 52));
        events.push_back(off(at, 52));
    }
    else if (!c.steady)
    {
        events.push_back(on(at, 52));
    }
    const auto r = runEngine(s, sorted(events));
    return clickDb(r.out, at);
}

void printClicks()
{
    std::cout << "\n## 1b. The click at a legato step and at a returning key\n\n";
    std::cout
        << "Key 45 held from 0; key 52 at sample 24013 (off the grid), or key 52 pressed at 12007 and "
           "released at 24013 to return to 45. Energy above 1 kHz, Hann 4096 centred 32 samples after, dB re a "
           "unit step. Mono Legato except the retrigger rows (Polyphony 1, not legato). The pad's oscillators are at "
           "Volume 2.5 (-12 dB): at full level eight sines drive the ladder hard enough that its own band above "
           "1 kHz is -37 dB at E3, which hides any event.\n\n";
    const std::array cases{
        ClickCase{.name = "steady at key 45 (no event)", .steady = true},
        ClickCase{.name = "steady at key 52 (no event)", .steady = true, .steadyKey = 52},
        ClickCase{.name = "legato, phases kept"},
        ClickCase{.name = "legato, phases restarted (Retrig)", .restartPhases = true},
        ClickCase{.name = "retrigger (Polyphony 1)", .mono = false},
        ClickCase{.name = "return: legato, phases kept", .returning = true},
        ClickCase{.name = "return: legato, phases restarted", .restartPhases = true, .returning = true},
        ClickCase{.name = "return: retriggered", .returnTo = ReturnTo::retrigger, .returning = true},
    };
    struct P
    {
        std::string_view name;
        PatchSpec patch;
        KnobValues knobs;
    };
    PatchSpec quietPad = padPatch;
    quietPad.volume = 0.25f; // -12 dB, so the ladder barely saturates and the band above 1 kHz is the event's
    const std::array patches{
        P{.name = "init", .patch = initPatch, .knobs = initKnobs},
        P{.name = "ringing", .patch = ringingPatch, .knobs = ringingKnobs},
        P{.name = "pad at -12 dB", .patch = quietPad, .knobs = padKnobs},
    };
    std::cout << "| Event | portamento |";
    for (const auto& p : patches)
    {
        std::cout << " " << p.name << " |";
    }
    std::cout << "\n|---|---|---:|---:|---:|\n";
    for (const auto& c : cases)
    {
        for (const float glide : {0.0f, 0.03f})
        {
            if (c.steady && glide > 0.0f)
            {
                continue;
            }
            std::cout << "| " << c.name << " | " << (glide > 0.0f ? "30 ms" : "0") << " |";
            for (const auto& p : patches)
            {
                std::cout << " " << dbText(clickFor(p.patch, p.knobs, c, glide)) << " |";
            }
            std::cout << "\n";
        }
    }
}

// --- 1c. Velocity at a legato note --------------------------------------------------------------

void printVelocity()
{
    std::cout << "\n## 1c. Velocity at a legato note (velocity on the amp, a stand-in route)\n\n";
    std::cout << "Init patch, Mono Legato, phases kept; key 45 at velocity 127 held, key 52 at velocity 32 legato at "
                 "24013. Click as 1b; the level 50-150 ms after against 50-150 ms before.\n\n";
    std::cout << "| Velocity at a legato note | portamento | click (dB) | level after (dB) |\n|---|---|---:|---:|\n";
    const std::array rules{
        std::pair{VelocityLegato::atOnce, "taken at once"},
        std::pair{VelocityLegato::ramped, "ramped over the glide"},
        std::pair{VelocityLegato::kept, "kept from the first note"},
    };
    constexpr std::size_t at = 24013;
    for (const auto& [rule, name] : rules)
    {
        for (const float glide : {0.0f, 0.03f})
        {
            Scenario s{.length = at + samples(0.2)};
            s.policy.monoLegato = true;
            s.policy.velocity = rule;
            s.policy.glide = {.seconds = glide};
            s.policy.voice.velocityToAmp = true;
            const auto r = runEngine(s, std::vector{on(0, 45, 127), on(at, 52, 32)});
            const auto before = rmsOf(r.out.channel(0).subspan(at - samples(0.15), samples(0.1)));
            const auto after = rmsOf(r.out.channel(0).subspan(at + samples(0.05), samples(0.1)));
            std::cout << "| " << name << " | " << (glide > 0.0f ? "30 ms" : "0") << " | " << dbText(clickDb(r.out, at))
                      << " | " << fixed(20.0 * std::log10(after / before), 2) << " |\n";
        }
    }
}

// --- 2. The glide's pitch -------------------------------------------------------------------------

/// The glide's formula, in double, independent of Glide: the note at sample
/// n after the glide's start.
double formulaNote(const GlideLaw& law, double from, double to, double n)
{
    double seconds = law.seconds;
    if (law.time == GlideTime::constantRate)
    {
        seconds *= std::abs(to - from) / 12.0;
    }
    const double length = std::round(seconds * rate);
    if (n >= length || length <= 0.0)
    {
        return to;
    }
    switch (law.curve)
    {
    case GlideCurve::exponential:
        return to + ((from - to) * std::exp(-n * std::log(1000.0) / length));
    case GlideCurve::linearHz:
    {
        const double f0 = std::exp2(from / 12.0);
        const double f1 = std::exp2(to / 12.0);
        return 12.0 * std::log2(f0 + ((f1 - f0) * n / length));
    }
    case GlideCurve::linearSemitones:
        break;
    }
    return from + ((to - from) * n / length);
}

struct Trace
{
    double maxCents = 0.0;
    double rmsCents = 0.0;
    double lengthMs = 0.0; ///< from the note-on to the last period more than 1 cent from the target
};

/// The rendered pitch, period by period, against `expected` (the note at
/// each sample, as the oscillator's increment sees it), over [from, to).
Trace traceAgainst(std::span<const float> y, const std::function<double(double)>& expected, double latency,
                   std::size_t from, std::size_t to, std::size_t noteOn, double target)
{
    const auto ps = keytrack::periods(y, rate);
    Trace t;
    double sum = 0.0;
    std::size_t count = 0;
    auto last = static_cast<double>(noteOn);
    for (std::size_t i = 1; i < ps.size(); ++i)
    {
        // The period's ends, from its midpoint and frequency.
        const double half = 0.5 * rate / ps[i].hz;
        const double a = ps[i].at - half - latency;
        const double b = ps[i].at + half - latency;
        if (a < static_cast<double>(from) || b > static_cast<double>(to))
        {
            continue;
        }
        // The mean frequency over [a, b): each sample's increment holds from n to n + 1.
        double integral = 0.0;
        for (auto n = static_cast<std::size_t>(std::floor(a)); static_cast<double>(n) < b; ++n)
        {
            const double lo = std::max(a, static_cast<double>(n));
            const double hi = std::min(b, static_cast<double>(n) + 1.0);
            integral += keytrack::noteHz(expected(static_cast<double>(n))) * (hi - lo);
        }
        const double hz = integral / (b - a);
        const double cents = 1200.0 * std::log2(ps[i].hz / hz);
        t.maxCents = std::max(t.maxCents, std::abs(cents));
        sum += cents * cents;
        ++count;
        if (std::abs(1200.0 * std::log2(ps[i].hz / keytrack::noteHz(target))) > 1.0)
        {
            last = ps[i].at;
        }
    }
    t.rmsCents = std::sqrt(sum / static_cast<double>(std::max<std::size_t>(count, 1)));
    t.lengthMs = 1000.0 * (last - static_cast<double>(noteOn)) / rate;
    return t;
}

struct GlideCase
{
    std::string_view name;
    GlideLaw law;
};

const std::array glideCases{
    GlideCase{
        .name = "linear in semitones, constant time",
        .law = {.curve = GlideCurve::linearSemitones, .seconds = 0.1f},
    },
    GlideCase{
        .name = "linear in semitones, 100 ms per octave",
        .law = {.curve = GlideCurve::linearSemitones, .time = GlideTime::constantRate, .seconds = 0.1f},
    },
    GlideCase{.name = "exponential (RC), constant time", .law = {.curve = GlideCurve::exponential, .seconds = 0.1f}},
    GlideCase{.name = "linear in Hz, constant time", .law = {.curve = GlideCurve::linearHz, .seconds = 0.1f}},
};

const std::array pitchRates{
    std::pair{PitchRate::perSample, "per sample"},
    std::pair{PitchRate::gridStart, "grid, held from the start"},
    std::pair{PitchRate::gridMid, "grid, held from the midpoint"},
    std::pair{PitchRate::gridRamped, "grid, ramped"},
};

/// Mono Legato: `from` held, `to` legato at 24000, the glide by `law`.
Buffer renderGlide(const GlideLaw& law, PitchRate r, int from, int to)
{
    Scenario s{.patch = pitchPatch, .length = 24000 + samples(0.4)};
    s.policy.monoLegato = true;
    s.policy.glide = law;
    s.policy.voice.rate = r;
    return runEngine(s, std::vector{on(0, from), on(24000, to)}).out;
}

void printGlides()
{
    std::cout << "\n## 2. The glide's pitch against its formula\n\n";
    std::cout << "A sine, filter A bypassed; Mono Legato, C3 held, C4 legato at 24000, 100 ms. Each period's pitch "
                 "(zero crossings) against the formula's mean over that period, from the note-on to 50 ms after the "
                 "glide; cents, largest / RMS:\n\n";
    const double latency = [&]
    {
        auto v = std::make_unique<PitchedVoice<>>(d6::minBlepTable64());
        return static_cast<double>(v->slot(0).latencySamples());
    }();
    std::cout << "| Curve |";
    for (const auto& r : pitchRates)
    {
        std::cout << " " << r.second << " |";
    }
    std::cout << "\n|---|---|---|---|---|\n";
    for (const auto& g : glideCases)
    {
        std::cout << "| " << g.name << " |";
        for (const auto& [r, name] : pitchRates)
        {
            const auto y = renderGlide(g.law, r, 48, 60);
            const auto expected = [&](double n)
            {
                return n < 24000.0 ? 48.0 : formulaNote(g.law, 48.0, 60.0, n - 24000.0);
            };
            const auto t =
                traceAgainst(y.channel(0), expected, latency, 24000 - 2400, 24000 + samples(0.15), 24000, 60.0);
            std::cout << " " << fixed(t.maxCents, 2) << " / " << fixed(t.rmsCents, 2) << " |";
        }
        std::cout << "\n";
    }

    std::cout << "\nThe glide's length, measured (the last period more than 1 cent from the target), per sample:\n\n";
    std::cout << "| Curve | a semitone (C3-C#3) | an octave (C3-C4) | two octaves (C3-C5) |\n|---|---:|---:|---:|\n";
    for (const auto& g : glideCases)
    {
        std::cout << "| " << g.name << " |";
        for (const int to : {49, 60, 72})
        {
            const auto y = renderGlide(g.law, PitchRate::perSample, 48, to);
            const auto expected = [&](double n)
            {
                return n < 24000.0 ? 48.0 : formulaNote(g.law, 48.0, to, n - 24000.0);
            };
            const auto t = traceAgainst(y.channel(0), expected, latency, 24000 - 2400, 24000 + samples(0.35), 24000,
                                        static_cast<double>(to));
            std::cout << " " << fixed(t.lengthMs, 1) << " ms |";
        }
        std::cout << "\n";
    }

    // An interrupted glide: C5 legato over C3, 200 ms; E4 pressed at half way.
    std::cout
        << "\nAn interrupted glide: C3 held, C5 legato at 24000 (200 ms, linear), E4 at 28800 (the glide at "
           "C4). The largest pitch change from one period to the next around 28800, and the pitch just after:\n\n";
    std::cout
        << "| Restarts from | largest step between periods (cents) | pitch 2 ms after (note) |\n|---|---:|---:|\n";
    for (const auto& [from, name] : {
             std::pair{InterruptFrom::currentPitch, "the pitch reached"},
             std::pair{InterruptFrom::lastTarget, "the last target"},
         })
    {
        Scenario s{.patch = pitchPatch, .length = 24000 + samples(0.5)};
        s.policy.monoLegato = true;
        s.policy.glide = {.seconds = 0.2f};
        s.policy.interrupt = from;
        s.policy.voice.rate = PitchRate::perSample;
        const auto y = runEngine(s, std::vector{on(0, 48), on(24000, 72), on(28800, 64)}).out;
        const auto ps = keytrack::periods(y.channel(0), rate);
        double largest = 0.0;
        double after = 0.0;
        for (std::size_t i = 1; i < ps.size(); ++i)
        {
            if (ps[i].at > 28000.0 && ps[i].at < 30000.0)
            {
                largest = std::max(largest, std::abs(1200.0 * std::log2(ps[i].hz / ps[i - 1].hz)));
            }
            if (after <= 0.0 && ps[i].at > 28800.0 + 96.0)
            {
                after = 69.0 + (12.0 * std::log2(ps[i].hz / 440.0));
            }
        }
        std::cout << "| " << name << " | " << fixed(largest, 1) << " | " << fixed(after, 2) << " |\n";
    }
}

// --- 3. Normal and Slide --------------------------------------------------------------------------

void printModes()
{
    std::cout << "\n## 3. Normal and Slide\n\n";
    std::cout << "Mono Legato, 50 ms linear glide. The pitch 1 ms after E3's note-on, and whether it glides:\n\n";
    std::cout << "| Playing | Normal | Slide |\n|---|---|---|\n";
    for (const bool overlap : {true, false})
    {
        std::cout << "| C3 then E3, " << (overlap ? "overlapping" : "with a gap") << " |";
        for (const auto mode : {PortamentoMode::normal, PortamentoMode::slide})
        {
            Scenario s{.patch = pitchPatch, .length = 30000, .hostBlock = 64, .checks = {24048}};
            s.policy.monoLegato = true;
            s.policy.mode = mode;
            s.policy.glide = {.seconds = 0.05f};
            std::vector<Event> events{on(0, 48), on(24000, 52)};
            events.push_back(off(overlap ? 26000 : 20000, 48));
            float pitch = 0.0f;
            const auto r = runEngine(s, sorted(events), [&](TestEngine& e, std::size_t) { pitch = e.pitchOf(0); });
            std::cout << " " << fixed(pitch, 2) << (r.counts.glides > 0 ? " (glides)" : " (jumps)")
                      << (r.counts.legatoNotes > 0 ? ", legato" : ", retriggered") << " |";
        }
        std::cout << "\n";
    }

    std::cout << "\nPoly (Mono Legato off), 50 ms glide: the chord C3 E3 G3 (100-sample strum) held to 24000, then F3 "
                 "A3 C4 at 30000. Each new note's pitch 1 sample after its note-on (where it glides from), "
                 "R 1 s:\n\n";
    std::cout << "| Polyphony | glides from | mode | F3 | A3 | C4 |\n|---|---|---|---:|---:|---:|\n";
    for (const std::size_t poly : {16U, 3U})
    {
        for (const auto& [from, fname] : {
                 std::pair{PolyGlideFrom::lastNote, "the last note"},
                 std::pair{PolyGlideFrom::voiceOwn, "its voice's last pitch"},
                 std::pair{PolyGlideFrom::noGlide, "no glide"},
             })
        {
            for (const auto& [mode, mname] :
                 {std::pair{PortamentoMode::normal, "Normal"}, std::pair{PortamentoMode::slide, "Slide"}})
            {
                Scenario s{.patch = pitchPatch, .polyphony = poly, .length = 31000, .hostBlock = 64};
                s.patch.release = 1.0f;
                s.policy.mode = mode;
                s.policy.polyFrom = from;
                s.policy.glide = {.seconds = 0.05f};
                s.checks = {30001, 30101, 30201};
                const std::array<int, 3> second{53, 57, 60};
                std::vector<Event> const events{
                    on(0, 48),      on(100, 52),   on(200, 55),   off(24000, 48), off(24000, 52),
                    off(24000, 55), on(30000, 53), on(30100, 57), on(30200, 60),
                };
                std::array<std::string, 3> cells{};
                std::size_t index = 0;
                (void)runEngine(s, sorted(events),
                                [&](TestEngine& e, std::size_t)
                                {
                                    for (std::size_t i = 0; i < TestEngine::maxVoices; ++i)
                                    {
                                        if (e.isGateOn(i) && e.noteOf(i) == second[index])
                                        {
                                            cells[index] = fixed(e.pitchOf(i), 2);
                                        }
                                    }
                                    ++index;
                                });
                std::cout << "| " << poly << " | " << fname << " | " << mname << " | " << cells[0] << " | " << cells[1]
                          << " | " << cells[2] << " |\n";
            }
        }
    }
}

// --- 4. The bend ----------------------------------------------------------------------------------

struct BendCase
{
    std::string_view name;
    bool sevenBit = false;
    float rangeSemitones = 2.0f;
    float rampSeconds = 0.0f;
};

/// The wheel: +-range at 2 Hz, sent every `interval` samples, as 14-bit or
/// MSB-only (7-bit) values.
std::vector<Event> wheel(std::size_t from, std::size_t to, std::size_t interval, bool sevenBit)
{
    std::vector<Event> e;
    for (std::size_t n = from; n < to; n += interval)
    {
        const double x = std::sin(2.0 * std::numbers::pi * 2.0 * static_cast<double>(n - from) / rate);
        auto v = static_cast<int>(std::lround(8192.0 + (8191.0 * x)));
        if (sevenBit)
        {
            v = (v / 128) * 128; // the MSB only (v >= 0)
        }
        e.push_back(bendAt(n, v));
    }
    return e;
}

/// The zipper: the power outside 150-320 Hz (the band A3 sweeps at +-2
/// semitones, with the window's lobes), dB re the total.
double zipperDb(std::span<const float> y, std::size_t from)
{
    static const Window window = blackmanHarrisWindow(32768);
    const auto spectrum = powerSpectrum(y.subspan(from, 32768), rate, window);
    double inside = 0.0;
    double outside = 0.0;
    for (std::size_t k = 0; k < spectrum.power.size(); ++k)
    {
        const double f = spectrum.frequencyOf(k);
        (f >= 150.0 && f <= 320.0 ? inside : outside) += spectrum.power[k];
    }
    return 10.0 * std::log10(outside / (inside + outside));
}

void printBend()
{
    std::cout << "\n## 4. The bend\n\n";
    std::cout << "A3 (a sine, filter A bypassed) held; the wheel +-Bend Range at 2 Hz from 0.1 s, a message every "
                 "2 ms (96 samples, 13 samples after each grid point), 14-bit or MSB only. Zipper: the power outside "
                 "150-320 Hz over 0.68 s, dB re the "
                 "total (at range 12 the band is wider, so only its own rows compare). Pitch error: each period "
                 "against the wheel's exact sine, largest / RMS cents (it includes any lag).\n\n";
    const std::array cases{
        BendCase{.name = "14-bit, stepped"},
        BendCase{.name = "7-bit, stepped", .sevenBit = true},
        BendCase{.name = "14-bit, 5 ms ramp", .rampSeconds = 0.005f},
        BendCase{.name = "7-bit, 5 ms ramp", .sevenBit = true, .rampSeconds = 0.005f},
        BendCase{.name = "7-bit, 10 ms ramp", .sevenBit = true, .rampSeconds = 0.01f},
        BendCase{.name = "7-bit, 20 ms ramp", .sevenBit = true, .rampSeconds = 0.02f},
    };
    const double latency = [&]
    {
        auto v = std::make_unique<PitchedVoice<>>(d6::minBlepTable64());
        return static_cast<double>(v->slot(0).latencySamples());
    }();
    std::cout << "| Wheel | rate | zipper (dB) | pitch error (cents) |\n|---|---|---:|---:|\n";
    constexpr std::size_t start = 4813; // off the voices' 32-sample grid
    const std::size_t length = samples(1.2);
    for (const auto& c : cases)
    {
        for (const auto& [r, name] : {
                 std::pair{PitchRate::perSample, "per sample"},
                 std::pair{PitchRate::gridStart, "grid, held"},
                 std::pair{PitchRate::gridRamped, "grid, ramped"},
             })
        {
            Scenario s{.patch = pitchPatch, .length = length};
            s.policy.bendRange = c.rangeSemitones;
            s.policy.bendRampSeconds = c.rampSeconds;
            s.policy.voice.rate = r;
            auto events = wheel(start, length, 96, c.sevenBit);
            events.insert(events.begin(), on(0, 57));
            const auto y = runEngine(s, events).out;
            const auto expected = [&](double n)
            {
                const double t = std::max(0.0, n - static_cast<double>(start)) / rate;
                return 57.0 + (2.0 * std::sin(2.0 * std::numbers::pi * 2.0 * t));
            };
            const auto t = traceAgainst(y.channel(0), expected, latency, start + 2400, length - 2400, 0, 57.0);
            std::cout << "| " << c.name << " | " << name << " | " << fixed(zipperDb(y.channel(0), start + 9600), 1)
                      << " | " << fixed(t.maxCents, 1) << " / " << fixed(t.rmsCents, 1) << " |\n";
        }
    }
    for (const auto& c : {
             BendCase{.name = "14-bit, stepped, range 12", .rangeSemitones = 12.0f},
             BendCase{.name = "7-bit, stepped, range 12", .sevenBit = true, .rangeSemitones = 12.0f},
             BendCase{
                 .name = "7-bit, 10 ms ramp, range 12",
                 .sevenBit = true,
                 .rangeSemitones = 12.0f,
                 .rampSeconds = 0.01f,
             },
         })
    {
        Scenario s{.patch = pitchPatch, .length = length};
        s.policy.bendRange = c.rangeSemitones;
        s.policy.bendRampSeconds = c.rampSeconds;
        s.policy.voice.rate = PitchRate::perSample;
        auto events = wheel(start, length, 96, c.sevenBit);
        events.insert(events.begin(), on(0, 45));
        const auto y = runEngine(s, events).out;
        // The band A2 sweeps at +-12: 55-220 Hz; the power outside 40-300 Hz.
        static const Window window = blackmanHarrisWindow(32768);
        const auto spectrum = powerSpectrum(y.channel(0).subspan(start + 9600, 32768), rate, window);
        double inside = 0.0;
        double outside = 0.0;
        for (std::size_t k = 0; k < spectrum.power.size(); ++k)
        {
            const double f = spectrum.frequencyOf(k);
            (f >= 40.0 && f <= 300.0 ? inside : outside) += spectrum.power[k];
        }
        std::cout << "| " << c.name << " (A2, outside 40-300 Hz) | per sample | "
                  << fixed(10.0 * std::log10(outside / (inside + outside)), 1) << " | |\n";
    }

    std::cout
        << "\nThe range's ends: the bend at the 14-bit extremes (semitones), and how far full up falls short:\n\n";
    std::cout << "| Scale | 0 (full down) | 8192 | 16383 (full up) | short at range 2 / 12 / 24 (cents) |\n"
                 "|---|---:|---:|---:|---|\n";
    for (const auto& [scale, name] : {
             std::pair{BendScale::symmetric, "/ 8192 both ways"},
             std::pair{BendScale::exactEnds, "/ 8191 up, / 8192 down"},
         })
    {
        std::cout << "| " << name << " | " << fixed(bendFraction(0, scale), 6) << " | "
                  << fixed(bendFraction(8192, scale), 6) << " | " << fixed(bendFraction(16383, scale), 6) << " |";
        for (const float range : {2.0f, 12.0f, 24.0f})
        {
            std::cout << " " << fixed(100.0 * range * (1.0 - bendFraction(16383, scale)), 3);
        }
        std::cout << " |\n";
    }

    std::cout << "\nA released voice and the wheel: A3 bent to +2 (range 2), released at 0.5 s with R 1 s, the wheel "
                 "back to 0 at 0.6 s. The pitch in the release at 0.8 s:\n\n";
    std::cout << "| A released voice | pitch at 0.8 s (note) |\n|---|---:|\n";
    for (const bool freeze : {false, true})
    {
        Scenario s{.patch = pitchPatch, .length = samples(1.0)};
        s.patch.release = 1.0f;
        s.policy.voice.freezeBendOnRelease = freeze;
        s.policy.voice.rate = PitchRate::perSample;
        const auto y =
            runEngine(s, std::vector{on(0, 57), bendAt(4800, 16383), off(samples(0.5), 57), bendAt(samples(0.6), 8192)})
                .out;
        const auto ps = keytrack::periods(y.channel(0), rate);
        double note = 0.0;
        for (const auto& p : ps)
        {
            if (p.at > rate * 0.8)
            {
                note = 69.0 + (12.0 * std::log2(p.hz / 440.0));
                break;
            }
        }
        std::cout << "| " << (freeze ? "keeps the bend it had at its note-off" : "follows the wheel") << " | "
                  << fixed(note, 2) << " |\n";
    }
}

// --- 5. The filter following the glide and the bend -----------------------------------------------

void printKeytrack()
{
    std::cout << "\n## 5. The filter following the glide and the bend (keytrack 10)\n\n";
    std::cout << "LP24 self-oscillating (Resonance 7.5 knob, k 4.5), keytrack 10, A1 a sine at -80 dB to start "
                 "it, the cutoff set so the filter sings near the note, 3 s to settle. Its pitch, period by period, "
                 "against the oscillators' pitch (the key, the glide's formula, and the bend the voice reads, sample "
                 "by sample), after taking out the filter's steady detune (the mean over the 0.5 s before), cents "
                 "largest / RMS, over the 0.9 s from the motion's start:\n\n";
    std::cout << "| Motion | largest | RMS |\n|---|---:|---:|\n";
    struct M
    {
        std::string_view name;
        std::size_t at; ///< the motion's start
        bool glide;     ///< a legato C3-C4 at `at`, else the wheel from `at`
        float rampSeconds;
    };
    const std::size_t onGrid = samples(3.0); // a multiple of 32: a grid point of the voice
    const std::array motions{
        M{
            .name = "a legato octave glide, C3-C4, 100 ms, at a grid point",
            .at = onGrid,
            .glide = true,
            .rampSeconds = 0.0f,
        },
        M{.name = "the same, 13 samples after a grid point", .at = onGrid + 13, .glide = true, .rampSeconds = 0.0f},
        M{
            .name = "the wheel +-2 at 2 Hz on C3, 14-bit every 2 ms, stepped",
            .at = onGrid + 13,
            .glide = false,
            .rampSeconds = 0.0f,
        },
        M{.name = "the same wheel, 5 ms ramp", .at = onGrid + 13, .glide = false, .rampSeconds = 0.005f},
    };
    for (const auto& m : motions)
    {
        Scenario s{.patch = initPatch, .length = m.at + samples(1.0)};
        s.patch.volume = 1e-4f;
        // The cutoff at the note: 4.5675 Hz x 2^(cutoff + (48 - 9) / 12) = 130.8 Hz at cutoff 1.59.
        s.knobs = {.cutoff = 1.59f, .resonance = 7.5f, .controlCutoff = 0.0f, .keytrack = 10.0f};
        s.policy.voice.rate = PitchRate::perSample;
        s.policy.bendRampSeconds = m.rampSeconds;
        std::vector<Event> events{on(0, 48)};
        if (m.glide)
        {
            s.policy.monoLegato = true;
            s.policy.glide = {.seconds = 0.1f};
            events.push_back(on(m.at, 60));
        }
        else
        {
            const auto w = wheel(m.at, m.at + samples(1.0), 96, false);
            events.insert(events.end(), w.begin(), w.end());
        }
        // The bend the voices read at each sample: the same ramp, replayed.
        std::vector<double> bendTrack(s.length, 0.0);
        {
            BendRamp ramp;
            std::size_t e = 0;
            for (std::size_t n = 0; n < s.length; ++n)
            {
                while (e < events.size() && events[e].offset == n)
                {
                    if (events[e].kind == EventKind::bend)
                    {
                        ramp.set(n, s.policy.bendRange * bendFraction(events[e].value, s.policy.bendScale),
                                 static_cast<std::uint64_t>(std::llround(m.rampSeconds * rate)));
                    }
                    ++e;
                }
                bendTrack[n] = ramp.at(n);
            }
        }
        const auto pitch = [&](double n)
        {
            const auto i = static_cast<std::size_t>(std::clamp(n, 0.0, static_cast<double>(s.length - 1)));
            const auto g = static_cast<double>(m.at);
            const double key = m.glide && n >= g ? formulaNote({.seconds = 0.1f}, 48.0, 60.0, n - g) : 48.0;
            return key + bendTrack[i];
        };
        const auto y = runEngine(s, events).out;
        const auto ps = keytrack::periods(y.channel(0), rate);
        double offset = 0.0;
        std::size_t count = 0;
        const auto start = static_cast<double>(m.at);
        for (const auto& p : ps)
        {
            if (p.at > start - 24000.0 && p.at < start)
            {
                offset += 1200.0 * std::log2(p.hz / keytrack::noteHz(48.0));
                ++count;
            }
        }
        offset /= static_cast<double>(std::max<std::size_t>(count, 1));
        double largest = 0.0;
        double sum = 0.0;
        std::size_t n = 0;
        for (const auto& p : ps)
        {
            if (p.at < start || p.at > start + (0.9 * rate))
            {
                continue;
            }
            // The oscillators' mean pitch over the period.
            const double half = 0.5 * rate / p.hz;
            double hz = 0.0;
            for (int k = 0; k < 8; ++k)
            {
                hz += keytrack::noteHz(pitch(p.at - half + ((2.0 * half) * (static_cast<double>(k) + 0.5) / 8.0)));
            }
            hz /= 8.0;
            const double cents = (1200.0 * std::log2(p.hz / hz)) - offset;
            largest = std::max(largest, std::abs(cents));
            sum += cents * cents;
            ++n;
        }
        std::cout << "| " << m.name << " | " << fixed(largest, 2) << " | "
                  << fixed(std::sqrt(sum / static_cast<double>(std::max<std::size_t>(n, 1))), 2) << " |\n";
    }
}

// --- 6. The hold pedal ----------------------------------------------------------------------------

void printPedal()
{
    std::cout << "\n## 6. The hold pedal\n\n";
    // (a) A key played again under the pedal (P7).
    std::cout << "A key played again under the pedal (P7): pedal down at 0; C3 on 1000 off 5000; C3 again at 10000, "
                 "off 14000; E3 on 12000, held; pedal up at 20000. R 1 s. Voices on C3 at 16000; at 21000 the voices "
                 "on C3 with their gate on, and E3's:\n\n";
    std::cout
        << "| C3 again | voices on C3 at 16000 | C3 gates on at 21000 | E3 gate on at 21000 |\n|---|---:|---:|---|\n";
    for (const auto& [replay, name] :
         {std::pair{PedalReplay::newVoice, "a new voice"}, std::pair{PedalReplay::sustained, "its sustained voice"}})
    {
        Scenario s{.length = 22000, .hostBlock = 64, .checks = {16000, 21000}};
        s.patch.release = 1.0f;
        s.policy.replay = replay;
        std::array<std::size_t, 2> onC{};
        std::array<std::size_t, 2> gateC{};
        bool gateE = false;
        std::size_t index = 0;
        (void)runEngine(s,
                        sorted({
                            pedalAt(0, 127),
                            on(1000, 48),
                            off(5000, 48),
                            on(10000, 48),
                            off(14000, 48),
                            on(12000, 52),
                            pedalAt(20000, 0),
                        }),
                        [&](TestEngine& e, std::size_t)
                        {
                            for (std::size_t i = 0; i < TestEngine::maxVoices; ++i)
                            {
                                if (e.noteOf(i) == 48)
                                {
                                    ++onC[index];
                                    gateC[index] += e.isGateOn(i) ? 1 : 0;
                                }
                                if (e.noteOf(i) == 52 && index == 1)
                                {
                                    gateE = e.isGateOn(i);
                                }
                            }
                            ++index;
                        });
        std::cout << "| " << name << " | " << onC[0] << " | " << gateC[1] << " | " << (gateE ? "yes" : "no") << " |\n";
    }

    // (b) The steal order with the pedal down.
    std::cout << "\nThe steal order under the pedal: Polyphony 8, a bass C2 held 0-4 s, the pedal down at 0.25 s "
                 "and up at 3.7 s, 16ths over 8 keys 0.5-3.5 s (each 62.5 ms, sustained by the pedal), R 1 s:\n\n";
    std::cout << "| A sustained note counts as | the bass at 3.6 s | steals |\n|---|---|---:|\n";
    for (const auto& [rule, name] : {
             std::pair{PedalSteal::sustainedHeld, "held"},
             std::pair{PedalSteal::sustainedReleased, "released"},
             std::pair{PedalSteal::tiered, "between: after released notes, before held keys"},
         })
    {
        Scenario s{.polyphony = 8, .length = samples(4.0), .hostBlock = 256, .checks = {samples(3.6)}};
        s.patch.release = 1.0f;
        s.policy.pedalSteal = rule;
        std::vector<Event> events{
            on(0, 36),
            off(samples(4.0) - 1, 36),
            pedalAt(samples(0.25), 127),
            pedalAt(samples(3.7), 0),
        };
        const std::array<int, 8> melody{60, 62, 64, 65, 67, 69, 71, 72};
        for (std::size_t i = 0; i < 48; ++i)
        {
            const auto at = samples(0.5 + (0.0625 * static_cast<double>(i)));
            events.push_back(on(at, melody[i % 8]));
            events.push_back(off(at + samples(0.0625) - 1, melody[i % 8]));
        }
        bool bass = false;
        const auto r = runEngine(s, sorted(events),
                                 [&](TestEngine& e, std::size_t)
                                 {
                                     for (std::size_t i = 0; i < TestEngine::maxVoices; ++i)
                                     {
                                         bass = bass || (e.noteOf(i) == 36 && e.isGateOn(i));
                                     }
                                 });
        std::cout << "| " << name << " | " << (bass ? "sounding" : "stolen") << " | " << r.counts.steals << " |\n";
    }

    // (c) CC 64 thresholds.
    std::cout << "\nCC 64's threshold: pedal toggles (down or up) for three controller signals, a value every 5 ms "
                 "with +-4 of noise:\n\n";
    std::cout
        << "| Down at | a full press and release over 2 s | resting at 20 | resting at 62 |\n|---|---:|---:|---:|\n";
    const auto noise = [](std::size_t i)
    {
        std::uint32_t x = static_cast<std::uint32_t>(i) * 2654435761U;
        x ^= x >> 15U;
        return static_cast<int>(x % 9U) - 4;
    };
    const auto signal = [&](int kind)
    {
        std::vector<Event> e;
        for (std::size_t i = 0; i < 400; ++i)
        {
            const double t = static_cast<double>(i) / 400.0;
            double v = 0.0;
            switch (kind)
            {
            case 0:
                v = 127.0 * (t < 0.5 ? 2.0 * t : 2.0 * (1.0 - t));
                break;
            case 1:
                v = 20.0;
                break;
            default:
                v = 62.0;
                break;
            }
            e.push_back(pedalAt(i * 240, std::clamp(static_cast<int>(std::lround(v)) + noise(i), 0, 127)));
        }
        return e;
    };
    for (const auto& [rule, name] : {
             std::pair{PedalThreshold::atLeast64, ">= 64"},
             std::pair{PedalThreshold::nonZero, "> 0"},
             std::pair{PedalThreshold::hysteresis, ">= 64, up below 48"},
         })
    {
        std::cout << "| " << name << " |";
        for (int kind = 0; kind < 3; ++kind)
        {
            Scenario s{.length = 96000, .hostBlock = 512};
            s.patch.voices = {0, 0, 0, 0};
            s.policy.pedalThreshold = rule;
            const auto r = runEngine(s, signal(kind));
            std::cout << " " << r.counts.pedalToggles << " |";
        }
        std::cout << "\n";
    }

    // (d) The pedal in Mono Legato.
    std::cout
        << "\nThe pedal in Mono Legato (Portamento 0, last-note priority). The key sounding after each event:\n\n";
    struct Step
    {
        std::string_view what;
        Event event;
    };
    const std::vector<std::vector<Step>> sequences{
        {
            {.what = "pedal down", .event = pedalAt(1000, 127)},
            {.what = "C3 down", .event = on(2000, 48)},
            {.what = "E3 down", .event = on(3000, 52)},
            {.what = "E3 up", .event = off(4000, 52)},
            {.what = "pedal up", .event = pedalAt(5000, 0)},
            {.what = "C3 up", .event = off(6000, 48)},
        },
        {
            {.what = "pedal down", .event = pedalAt(1000, 127)},
            {.what = "C3 down", .event = on(2000, 48)},
            {.what = "C3 up", .event = off(3000, 48)},
            {.what = "E3 down", .event = on(4000, 52)},
            {.what = "E3 up", .event = off(5000, 52)},
            {.what = "pedal up", .event = pedalAt(6000, 0)},
        },
    };
    for (const auto& seq : sequences)
    {
        std::cout << "| Event | the stack follows the keys; the pedal holds the last gate | released keys stay in the "
                     "stack until pedal-up | as the first, and a key played under a held note is legato |\n"
                     "|---|---|---|---|\n";
        constexpr std::array rules{MonoPedal::delaysRelease, MonoPedal::holdsKeys, MonoPedal::sustainLegato};
        std::vector<std::array<std::string, 3>> cells(seq.size());
        std::array<Counts, 3> counts{};
        for (std::size_t rule = 0; rule < rules.size(); ++rule)
        {
            Scenario s{.length = 8000, .hostBlock = 64};
            s.patch.release = 0.5f;
            s.policy.monoLegato = true;
            s.policy.monoPedal = rules[rule];
            std::vector<Event> events;
            for (const auto& st : seq)
            {
                events.push_back(st.event);
                s.checks.push_back(st.event.offset + 1);
            }
            std::size_t index = 0;
            counts[rule] = runEngine(s, events,
                                     [&](TestEngine& e, std::size_t)
                                     {
                                         cells[index][rule] =
                                             keyName(e.soundingKey()) + (e.isGateOn(0) ? "" : " (released)");
                                         ++index;
                                     })
                               .counts;
        }
        for (std::size_t i = 0; i < seq.size(); ++i)
        {
            std::cout << "| " << seq[i].what << " | " << cells[i][0] << " | " << cells[i][1] << " | " << cells[i][2]
                      << " |\n";
        }
        std::cout << "| *legato notes / retriggers* |";
        for (const auto& c : counts)
        {
            std::cout << " " << c.legatoNotes << " / " << c.retriggers << " |";
        }
        std::cout << "\n\n";
    }
}

// --- 7. Real-time terms ---------------------------------------------------------------------------

void printRealTime()
{
    std::cout << "\n## 7. Real-time terms\n\n";
    std::cout << "Two sequences, 2 s: Mono Legato (Slide, 60 ms, last note, returns, a gap, the pedal) and poly "
                 "(Polyphony 4, Normal, 60 ms from the last note, steals, the pedal), both with the wheel every 2 ms "
                 "(5 ms ramp), the pad with drift, keytrack 10, k 3.6. Host blocks of 1 and 37 against 4096:\n\n";
    std::cout << "| Sequence | rate | 1 | 37 | allocations |\n|---|---|---|---|---:|\n";
    PatchSpec pad = padPatch;
    pad.driftCents = 1.5f;
    std::vector<Event> mono{
        on(1001, 48),   on(9003, 55),   off(15007, 55),    on(20011, 60),       off(27013, 48),
        on(30017, 52),  off(36019, 60), off(40023, 52),    pedalAt(45029, 127), on(50031, 57),
        off(54037, 57), on(60041, 59),  pedalAt(70043, 0), off(80047, 59),
    };
    std::vector<Event> poly{
        on(1001, 48),  on(1501, 52),   on(2001, 55),   pedalAt(5003, 127), off(9007, 48),     on(12011, 60),
        on(16013, 64), off(18017, 52), on(22019, 67),  on(26023, 72),      pedalAt(30029, 0), off(34031, 55),
        on(40037, 50), off(50041, 60), off(60043, 64), off(70047, 67),     off(80051, 72),    off(85053, 50),
    };
    for (auto* seq : {&mono, &poly})
    {
        const auto w = wheel(3000, 90000, 96, false);
        seq->insert(seq->end(), w.begin(), w.end());
        *seq = sorted(*seq);
    }
    for (std::size_t which = 0; which < 2; ++which)
    {
        for (const auto& [r, name] : pitchRates)
        {
            Scenario s{
                .patch = pad,
                .knobs = {.cutoff = 3.0f, .resonance = 6.0f, .keytrack = 10.0f},
                .polyphony = 4,
                .length = samples(2.0),
            };
            s.policy.monoLegato = which == 0;
            s.policy.mode = which == 0 ? PortamentoMode::slide : PortamentoMode::normal;
            s.policy.glide = {.seconds = 0.06f};
            s.policy.bendRampSeconds = 0.005f;
            s.policy.voice.rate = r;
            const auto& events = which == 0 ? mono : poly;
            s.hostBlock = 4096;
            const auto reference = runEngine(s, events);
            std::string cells;
            std::size_t allocations = reference.allocations;
            for (const std::size_t b : {1U, 37U})
            {
                s.hostBlock = b;
                const auto x = runEngine(s, events);
                allocations = std::max(allocations, x.allocations);
                cells += std::string(" | ") + (bitIdentical(x.out, reference.out) ? "identical" : "differs");
            }
            std::cout << "| " << (which == 0 ? "mono" : "poly") << " | " << name << cells << " | " << allocations
                      << " |\n";
        }
    }
}

// --- 8. The portamento knob ------------------------------------------------------------------------

void printKnobLaws()
{
    std::cout << "\n## 8. The portamento knob: candidate laws (seconds)\n\n";
    std::cout
        << "| Knob | E1's law, 25 s x^4.64 | (576^x - 1) / 23 | 10 s x^3 | 2 s x^2 |\n|---:|---:|---:|---:|---:|\n";
    for (const double knob : {0.0, 0.5, 1.0, 2.0, 3.0, 5.0, 7.5, 10.0})
    {
        const double x = knob / 10.0;
        std::cout << "| " << fixed(knob, 1) << " | " << fixed(25.0 * std::pow(x, 4.64), 4) << " | "
                  << fixed((std::pow(576.0, x) - 1.0) / 23.0, 4) << " | " << fixed(10.0 * x * x * x, 4) << " | "
                  << fixed(2.0 * x * x, 4) << " |\n";
    }
}

} // namespace

TEST_CASE("Legato and portamento report", "[.][legato-report]")
{
    std::cout << "# Legato and portamento report (M1.4 task 3, stage 1), " << rateF << " Hz\n";
    printEquivalence();
    printPriorities();
    printClicks();
    printVelocity();
    printGlides();
    printModes();
    printBend();
    printKeytrack();
    printPedal();
    printRealTime();
    printKnobLaws();
    SUCCEED();
}
