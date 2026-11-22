// Tests for M1.4 task 3 as decided (docs/dsp/legato-portamento.md): the
// voice's pitch input (voice.hpp) and the allocator's Mono Legato,
// portamento, pitch bend and hold pedal (voice_allocator.hpp).
//
// - The voice: a glide (linear in semitones, constant time) and the bend
//   reach the oscillators every sample while the pitch moves; a legato note
//   leaves the envelopes and the oscillators' phases alone; keytrack follows
//   the glide, the bend and the master tune; the velocity ramps over a
//   glide. Bit for bit the stage-1 prototype (legato_candidates.hpp,
//   PitchRate::perSample) at host blocks 1/37/4096, with no allocation.
// - The allocator: last-note priority with legato returns, Polyphony
//   ignored; Normal and Slide, from the pitch reached; poly portamento from
//   the last note; a switch of Mono Legato releasing everything; the pedal
//   (>= 64, pedal-up releasing keys that are up, a new voice for a key played
//   again, sustained notes between released and held for stealing, a key
//   under a pedal-held mono note legato); bend events reaching every voice.
//   Bit for bit the stage-1 prototype Engine in its decided configuration.
//
// The allocation logic is pinned on a stub voice (stub_voice.hpp) that
// records what it is told; pitches on dsp::Voice, read from the oscillator's
// phase increment, which is exact.

#include "legato_candidates.hpp"
#include "stub_voice.hpp"

#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/voice/pitch_bend.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

using namespace anoesis::test;
namespace dsp = anoesis::dsp;
namespace proto = anoesis::test::legato;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;
using dsp::VoiceEvent;
using dsp::VoiceEventKind;

namespace
{

constexpr float rate = 48000.0f;
using Voice = dsp::Voice<dsp::minBlepTaps>;

// --- Patches and knobs -------------------------------------------------------------------------

/// A sine on A1 only, filter A on Bypass (heard as it is), the envelopes a
/// gate: for reading the pitch.
template <typename V>
void sinePatch(V& v)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        v.slot(s).setWaveform(dsp::Waveform::sine);
        v.slot(s).setVoices(s == 0 ? 1 : 0);
    }
    v.setFilterBypassed(0, true);
}

/// The resonant pad of the stage-1 reports: A1 and A2 four detuned saws
/// each, free-running, drift on; A 20 ms, D 100 ms, S 0.8, R 150 ms.
template <typename V>
void padPatch(V& v)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        auto& slot = v.slot(s);
        slot.setWaveform(dsp::Waveform::saw);
        slot.setVoices(s < 2 ? 4 : 0);
        slot.setDetune(0.3f);
        slot.setStereo(1.0f);
        slot.setRetrigger(false);
    }
    for (std::size_t p = 0; p < 2; ++p)
    {
        auto& e = v.envelope(p);
        e.setAttack(0.02f);
        e.setDecay(0.1f);
        e.setSustain(0.8f);
        e.setRelease(0.15f);
    }
    v.drift().setDepthCents(dsp::PitchDrift::defaultDepthCents);
}

/// Retrig on, both parts into filter A on AB at drive x2, B an octave up.
template <typename V>
void crossingPatch(V& v)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        auto& slot = v.slot(s);
        slot.setWaveform(s % 2 == 0 ? dsp::Waveform::saw : dsp::Waveform::pulse);
        slot.setVoices(s == 3 ? 0 : 2);
        slot.setDetune(0.2f);
        slot.setRetrigger(true);
        slot.setPhaseDegrees(90.0f);
        slot.setTuning({.octaves = s < 2 ? 0 : 1, .semitones = 0, .fine = 0.0f});
    }
    for (std::size_t p = 0; p < 2; ++p)
    {
        auto& e = v.envelope(p);
        e.setAttack(0.005f);
        e.setDecay(0.2f);
        e.setSustain(0.6f);
        e.setRelease(0.1f);
    }
    v.setFilterInput(0, dsp::FilterInput::both);
    v.setFilterInput(1, dsp::FilterInput::none);
    v.setFilterDrive(0, 2.0f);
}

/// Constant global knobs: each filter's Cutoff and Resonance, the Filter
/// Control's, and Keytrack.
struct Ramps
{
    Ramps(float cutoffKnob, float resonanceKnob, float controlCutoffKnob, float keytrackKnob) : keytrack(keytrackKnob)
    {
        cutoff.setCurrentAndTarget(cutoffKnob);
        resonance.setCurrentAndTarget(resonanceKnob);
        controlCutoff.setCurrentAndTarget(controlCutoffKnob);
    }
    [[nodiscard]] dsp::VoiceKnobs knobs(const dsp::PitchBend* bend, std::uint64_t time) const noexcept
    {
        return {
            .cutoff = {&cutoff, &cutoff},
            .resonance = {&resonance, &resonance},
            .controlCutoff = &controlCutoff,
            .controlResonance = &controlResonance,
            .keytrack = keytrack,
            .bend = bend,
            .time = time,
        };
    }
    dsp::LinearSmoother cutoff;
    dsp::LinearSmoother resonance;
    dsp::LinearSmoother controlCutoff;
    dsp::LinearSmoother controlResonance;
    float keytrack = 0.0f;
};

/// The frequency a slot's voice plays, from its phase increment.
double hzOf(const Voice& v, std::size_t slot = 0)
{
    return static_cast<double>(v.slot(slot).phaseIncrement()) * static_cast<double>(rate) / 4294967296.0;
}

double centsBetween(double hz, double note)
{
    return 1200.0 * std::log2(hz / (440.0 * std::exp2((note - 69.0) / 12.0)));
}

std::unique_ptr<Voice> preparedVoice(void (*patch)(Voice&), std::uint64_t seed = 7)
{
    auto v = std::make_unique<Voice>(d6::minBlepTable64(), seed);
    patch(*v);
    v->prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 2});
    return v;
}

// --- One voice against the prototype -----------------------------------------------------------

/// One voice's script: note-ons, legato notes, a note-off, and a wheel
/// moving at 2 Hz (+-8191, a message every 96 samples, off the grid).
struct Action
{
    std::size_t at = 0;
    enum class Kind : std::uint8_t
    {
        noteOn,
        legato,
        noteOff,
        bend,
    } kind = Kind::noteOn;
    float note = 0.0f;
    bool glide = false;
    float seconds = 0.0f;
    float velocity = 1.0f;
    int bend = 8192;
};

std::vector<Action> script()
{
    std::vector<Action> a{
        {.at = 0, .kind = Action::Kind::noteOn, .note = 48.0f, .velocity = 0.9f},
        {.at = 9001, .kind = Action::Kind::legato, .note = 60.0f, .glide = true, .seconds = 0.06f, .velocity = 0.5f},
        {.at = 20003, .kind = Action::Kind::legato, .note = 55.0f, .velocity = 0.7f},
        {.at = 30007, .kind = Action::Kind::noteOn, .note = 52.0f, .glide = true, .seconds = 0.06f, .velocity = 0.4f},
        {.at = 40009, .kind = Action::Kind::legato, .note = 50.0f, .glide = true, .seconds = 0.03f, .velocity = 1.0f},
        {.at = 50011, .kind = Action::Kind::noteOff},
    };
    for (std::size_t n = 3013; n < 60000; n += 96)
    {
        const double x = std::sin(2.0 * std::numbers::pi * 2.0 * static_cast<double>(n - 3013) / rate);
        a.push_back(
            {.at = n, .kind = Action::Kind::bend, .bend = static_cast<int>(std::lround(8192.0 + (8191.0 * x)))});
    }
    std::ranges::stable_sort(a, {}, &Action::at);
    return a;
}

constexpr std::size_t scriptLength = 64000;

/// dsp::Voice through the script, in host blocks of `hostBlock` (split at
/// the actions); the allocations from prepare() on.
Buffer runLibrary(void (*patch)(Voice&), const Ramps& ramps, std::size_t hostBlock, std::size_t& allocations)
{
    const auto actions = script();
    auto v = std::make_unique<Voice>(d6::minBlepTable64(), 7);
    patch(*v);
    dsp::PitchBend bend;
    Buffer out{2, scriptLength};
    const auto block = out.block();
    const AllocationCounter counter;
    v->prepare({.sampleRate = rate, .maxBlockSize = hostBlock, .numChannels = 2});
    bend.prepare(rate);
    std::size_t next = 0;
    for (std::size_t start = 0; start < scriptLength;)
    {
        while (next < actions.size() && actions[next].at == start)
        {
            const auto& a = actions[next];
            const auto k = ramps.knobs(&bend, start);
            const dsp::NoteStart s{
                .velocity = a.velocity,
                .glide = a.glide,
                .from = v->currentPitch(),
                .glideSeconds = a.seconds,
                .fromVelocity = v->velocity(),
            };
            switch (a.kind)
            {
            case Action::Kind::noteOn:
                v->noteOn(a.note, k, s);
                break;
            case Action::Kind::legato:
                v->legatoNoteOn(a.note, k, s);
                break;
            case Action::Kind::noteOff:
                v->noteOff();
                break;
            case Action::Kind::bend:
                bend.setValue(start, a.bend);
                break;
            }
            ++next;
        }
        auto end = std::min(scriptLength, start + hostBlock);
        if (next < actions.size() && actions[next].at < end)
        {
            end = actions[next].at;
        }
        v->process(block.subBlock(start, end - start), ramps.knobs(&bend, start));
        start = end;
    }
    allocations = counter.count();
    return out;
}

/// The stage-1 prototype voice (per sample) through the same script, blocks
/// of 4096.
Buffer runPrototype(void (*patch)(proto::PitchedVoice<>&), const Ramps& ramps)
{
    const auto actions = script();
    auto v = std::make_unique<proto::PitchedVoice<>>(d6::minBlepTable64(), 7);
    patch(*v);
    v->setOptions({.rate = proto::PitchRate::perSample});
    v->prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 2});
    proto::BendRamp bend;
    const auto knobs = [&](std::size_t at)
    {
        return proto::VoiceKnobs{.filter = ramps.knobs(nullptr, 0), .bend = &bend, .at = at};
    };
    Buffer out{2, scriptLength};
    const auto block = out.block();
    std::size_t next = 0;
    for (std::size_t start = 0; start < scriptLength;)
    {
        while (next < actions.size() && actions[next].at == start)
        {
            const auto& a = actions[next];
            const proto::GlideStart g{.glide = a.glide, .from = v->currentPitch(), .law = {.seconds = a.seconds}};
            switch (a.kind)
            {
            case Action::Kind::noteOn:
                v->noteOn(a.note, knobs(start), g, a.velocity);
                break;
            case Action::Kind::legato:
                v->legatoNoteOn(a.note, knobs(start), g, false, a.velocity, proto::VelocityLegato::ramped);
                break;
            case Action::Kind::noteOff:
                v->noteOff();
                break;
            case Action::Kind::bend:
                bend.set(start, 2.0f * proto::bendFraction(a.bend, proto::BendScale::symmetric), 240);
                break;
            }
            ++next;
        }
        auto end = std::min(scriptLength, start + std::size_t{4096});
        if (next < actions.size() && actions[next].at < end)
        {
            end = actions[next].at;
        }
        v->process(block.subBlock(start, end - start), knobs(start));
        start = end;
    }
    return out;
}

// --- The allocator ---------------------------------------------------------------------------------

VoiceEvent on(std::size_t offset, int key, int velocity = 127)
{
    return {.offset = offset, .kind = VoiceEventKind::noteOn, .value = key, .velocity = velocity};
}
VoiceEvent off(std::size_t offset, int key)
{
    return {.offset = offset, .kind = VoiceEventKind::noteOff, .value = key};
}
VoiceEvent pedal(std::size_t offset, int value)
{
    return {.offset = offset, .kind = VoiceEventKind::pedal, .value = value};
}
VoiceEvent bendEvent(std::size_t offset, int value)
{
    return {.offset = offset, .kind = VoiceEventKind::pitchBend, .value = value};
}

using StubAllocator = dsp::VoiceAllocator<StubVoice>;

std::unique_ptr<StubAllocator> stubs(std::size_t release = 100000, std::size_t notes = 16)
{
    auto a = std::make_unique<StubAllocator>(42);
    a->forEachVoice([release](StubVoice& v) { v.release = release; });
    a->setPolyphony(notes);
    a->prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
    return a;
}

void block(StubAllocator& a, std::vector<VoiceEvent> events, std::size_t n = 64)
{
    Buffer out{2, n};
    a.process(out.block(), events, {});
}

std::size_t voicesOn(const StubAllocator& a, int key)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < StubAllocator::maxVoices; ++i)
    {
        n += a.noteOf(i) == key ? 1 : 0;
    }
    return n;
}

std::size_t voiceOf(const StubAllocator& a, int key)
{
    for (std::size_t i = 0; i < StubAllocator::maxVoices; ++i)
    {
        if (a.noteOf(i) == key)
        {
            return i;
        }
    }
    return StubAllocator::maxVoices;
}

/// The stage-1 report's two sequences (a mono phrase with returns, a gap and
/// the pedal; a poly one with steals and the pedal), both with the wheel.
std::vector<proto::Event> sequence(bool mono)
{
    using E = proto::Event;
    using K = proto::EventKind;
    std::vector<E> e;
    if (mono)
    {
        e = {
            {.offset = 1001, .kind = K::noteOn, .value = 48, .velocity = 100},
            {.offset = 9003, .kind = K::noteOn, .value = 55, .velocity = 60},
            {.offset = 15007, .kind = K::noteOff, .value = 55},
            {.offset = 20011, .kind = K::noteOn, .value = 60, .velocity = 127},
            {.offset = 27013, .kind = K::noteOff, .value = 48},
            {.offset = 30017, .kind = K::noteOn, .value = 52, .velocity = 30},
            {.offset = 36019, .kind = K::noteOff, .value = 60},
            {.offset = 40023, .kind = K::noteOff, .value = 52},
            {.offset = 45029, .kind = K::pedal, .value = 127},
            {.offset = 50031, .kind = K::noteOn, .value = 57, .velocity = 90},
            {.offset = 54037, .kind = K::noteOff, .value = 57},
            {.offset = 60041, .kind = K::noteOn, .value = 59, .velocity = 90},
            {.offset = 70043, .kind = K::pedal, .value = 0},
            {.offset = 80047, .kind = K::noteOff, .value = 59},
        };
    }
    else
    {
        e = {
            {.offset = 1001, .kind = K::noteOn, .value = 48},   {.offset = 1501, .kind = K::noteOn, .value = 52},
            {.offset = 2001, .kind = K::noteOn, .value = 55},   {.offset = 5003, .kind = K::pedal, .value = 127},
            {.offset = 9007, .kind = K::noteOff, .value = 48},  {.offset = 12011, .kind = K::noteOn, .value = 60},
            {.offset = 16013, .kind = K::noteOn, .value = 64},  {.offset = 18017, .kind = K::noteOff, .value = 52},
            {.offset = 22019, .kind = K::noteOn, .value = 67},  {.offset = 26023, .kind = K::noteOn, .value = 72},
            {.offset = 30029, .kind = K::pedal, .value = 0},    {.offset = 34031, .kind = K::noteOff, .value = 55},
            {.offset = 40037, .kind = K::noteOn, .value = 50},  {.offset = 50041, .kind = K::noteOff, .value = 60},
            {.offset = 60043, .kind = K::noteOff, .value = 64}, {.offset = 70047, .kind = K::noteOff, .value = 67},
            {.offset = 80051, .kind = K::noteOff, .value = 72}, {.offset = 85053, .kind = K::noteOff, .value = 50},
        };
    }
    for (std::size_t n = 3000; n < 90000; n += 96)
    {
        const double x = std::sin(2.0 * std::numbers::pi * 2.0 * static_cast<double>(n - 3000) / rate);
        e.push_back({.offset = n, .kind = K::bend, .value = static_cast<int>(std::lround(8192.0 + (8191.0 * x)))});
    }
    std::ranges::stable_sort(e, {}, &E::offset);
    return e;
}

constexpr std::size_t sequenceLength = 96000;

dsp::VoiceEvent toLibrary(const proto::Event& e, std::size_t start)
{
    VoiceEventKind kind = VoiceEventKind::noteOn;
    switch (e.kind)
    {
    case proto::EventKind::noteOff:
        kind = VoiceEventKind::noteOff;
        break;
    case proto::EventKind::pedal:
        kind = VoiceEventKind::pedal;
        break;
    case proto::EventKind::bend:
        kind = VoiceEventKind::pitchBend;
        break;
    case proto::EventKind::polyphony:
        kind = VoiceEventKind::polyphony;
        break;
    case proto::EventKind::noteOn:
        break;
    }
    return {.offset = e.offset - start, .kind = kind, .value = e.value, .velocity = e.velocity};
}

const Ramps& sequenceRamps()
{
    static const Ramps ramps{3.0f, 6.0f, 5.0f, 10.0f};
    return ramps;
}

Buffer runLibraryAllocator(bool mono, std::size_t hostBlock, std::size_t& allocations)
{
    const auto events = sequence(mono);
    auto a = std::make_unique<dsp::VoiceAllocator<Voice>>(0, d6::minBlepTable64());
    a->forEachVoice([](Voice& v) { padPatch(v); });
    a->setPolyphony(4);
    a->setMonoLegato(mono);
    a->setPortamento(0.06f);
    a->setPortamentoMode(mono ? dsp::PortamentoMode::slide : dsp::PortamentoMode::normal);
    a->setBendRange(2.0f);
    Buffer out{2, sequenceLength};
    const auto blockOut = out.block();
    std::vector<VoiceEvent> blockEvents;
    blockEvents.reserve(events.size());
    const AllocationCounter counter;
    a->prepare({.sampleRate = rate, .maxBlockSize = hostBlock, .numChannels = 2});
    std::size_t e = 0;
    for (std::size_t start = 0; start < sequenceLength; start += hostBlock)
    {
        const auto end = std::min(sequenceLength, start + hostBlock);
        blockEvents.clear();
        while (e < events.size() && events[e].offset < end)
        {
            blockEvents.push_back(toLibrary(events[e], start));
            ++e;
        }
        a->process(blockOut.subBlock(start, end - start), blockEvents, sequenceRamps().knobs(nullptr, 0));
    }
    allocations = counter.count();
    return out;
}

Buffer runPrototypeEngine(bool mono)
{
    const auto events = sequence(mono);
    auto engine = std::make_unique<proto::Engine<16>>(0, d6::minBlepTable64());
    engine->forEachVoice([](proto::PitchedVoice<>& v) { padPatch(v); });
    proto::Policy p;
    p.monoLegato = mono;
    p.priority = proto::Priority::last;
    p.returnTo = proto::ReturnTo::legato;
    p.legatoRestartsPhases = false;
    p.mode = mono ? proto::PortamentoMode::slide : proto::PortamentoMode::normal;
    p.glide = {.seconds = 0.06f};
    p.interrupt = proto::InterruptFrom::currentPitch;
    p.polyFrom = proto::PolyGlideFrom::lastNote;
    p.velocity = proto::VelocityLegato::ramped;
    p.bendRange = 2.0f;
    p.bendScale = proto::BendScale::symmetric;
    p.bendRampSeconds = 0.005f;
    p.pedalThreshold = proto::PedalThreshold::atLeast64;
    p.replay = proto::PedalReplay::newVoice;
    p.pedalSteal = proto::PedalSteal::tiered;
    p.monoPedal = proto::MonoPedal::sustainLegato;
    p.voice.rate = proto::PitchRate::perSample;
    engine->setPolicy(p);
    engine->setPolyphony(4);
    engine->prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 2});
    Buffer out{2, sequenceLength};
    const auto blockOut = out.block();
    std::vector<proto::Event> blockEvents;
    std::size_t e = 0;
    for (std::size_t start = 0; start < sequenceLength; start += 4096)
    {
        const auto end = std::min(sequenceLength, start + std::size_t{4096});
        blockEvents.clear();
        while (e < events.size() && events[e].offset < end)
        {
            auto ev = events[e];
            ev.offset -= start;
            blockEvents.push_back(ev);
            ++e;
        }
        engine->process(blockOut.subBlock(start, end - start), blockEvents,
                        {.filter = sequenceRamps().knobs(nullptr, 0), .bend = nullptr, .at = start});
    }
    return out;
}

} // namespace

// === The voice ===================================================================================

TEST_CASE("Voice: the pitch input is the stage-1 prototype's, bit for bit", "[voice][legato]")
{
    // Glides (legato and on a retrigger), a legato note at Portamento 0, a
    // moving wheel, velocities; three patches; host blocks 1/37/4096.
    const Ramps ramps{3.0f, 6.0f, 5.0f, 10.0f};
    const std::array<std::pair<void (*)(Voice&), void (*)(proto::PitchedVoice<>&)>, 3> patches{
        std::pair{&padPatch<Voice>, &padPatch<proto::PitchedVoice<>>},
        std::pair{&crossingPatch<Voice>, &crossingPatch<proto::PitchedVoice<>>},
        std::pair{&sinePatch<Voice>, &sinePatch<proto::PitchedVoice<>>},
    };
    for (const auto& [library, prototype] : patches)
    {
        const auto reference = runPrototype(prototype, ramps);
        for (const std::size_t hostBlock : {1U, 37U, 4096U})
        {
            std::size_t allocations = 1;
            const auto built = runLibrary(library, ramps, hostBlock, allocations);
            CHECK(bitIdentical(built, reference));
            CHECK(allocations == 0);
        }
    }
}

TEST_CASE("Voice: a glide reaches the oscillators every sample, on its line", "[voice][legato]")
{
    auto v = preparedVoice(sinePatch<Voice>);
    const Ramps ramps{5.0f, 0.0f, 5.0f, 0.0f};
    v->noteOn(48.0f, ramps.knobs(nullptr, 0), {});
    Buffer out{2, 1};
    for (std::size_t n = 0; n < 1000; ++n)
    {
        v->process(out.block(), ramps.knobs(nullptr, n));
    }
    REQUIRE_THAT(centsBetween(hzOf(*v), 48.0), WithinAbs(0.0, 1e-2));
    CHECK_THAT(v->currentPitch(), WithinULP(48.0f, 0));
    v->legatoNoteOn(60.0f, ramps.knobs(nullptr, 1000), {.glide = true, .from = 48.0f, .glideSeconds = 0.1f});
    CHECK_THAT(v->targetPitch(), WithinULP(60.0f, 0));
    // After each sample k, the oscillator plays the line's pitch at k: to 0.01
    // cents (the increment's quantum is 1e-4 cents, float notes 4e-4).
    double largest = 0.0;
    for (std::size_t k = 0; k < 5000; ++k)
    {
        v->process(out.block(), ramps.knobs(nullptr, 1000 + k));
        const double note = 48.0 + (12.0 * static_cast<double>(std::min<std::size_t>(k, 4800)) / 4800.0);
        largest = std::max(largest, std::abs(centsBetween(hzOf(*v), note)));
    }
    CHECK(largest < 1e-2);
    CHECK_THAT(v->currentPitch(), WithinULP(60.0f, 0));
}

TEST_CASE("Voice: the bend reaches the oscillators at its sample, ramped over 5 ms", "[voice][legato]")
{
    auto v = preparedVoice(sinePatch<Voice>);
    const Ramps ramps{5.0f, 0.0f, 5.0f, 0.0f};
    dsp::PitchBend bend;
    bend.prepare(rate);
    v->noteOn(48.0f, ramps.knobs(&bend, 0), {});
    Buffer out{2, 1};
    bend.setValue(1000, 16383);
    const double full = 2.0 * 8191.0 / 8192.0;
    double largest = 0.0;
    for (std::size_t n = 0; n < 2000; ++n)
    {
        v->process(out.block(), ramps.knobs(&bend, n));
        const double ramp = std::clamp((static_cast<double>(n) - 1000.0) / 240.0, 0.0, 1.0);
        largest = std::max(largest, std::abs(centsBetween(hzOf(*v), 48.0 + (full * ramp))));
    }
    CHECK(largest < 1e-2);
    // currentPitch() is the glide's, without the bend.
    CHECK_THAT(v->currentPitch(), WithinULP(48.0f, 0));

    SECTION("no bend in the knobs: none")
    {
        auto w = preparedVoice(sinePatch<Voice>);
        w->noteOn(48.0f, ramps.knobs(nullptr, 0), {});
        Buffer o{2, 2000};
        w->process(o.block(), ramps.knobs(nullptr, 0));
        CHECK_THAT(centsBetween(hzOf(*w), 48.0), WithinAbs(0.0, 1e-2));
    }
}

TEST_CASE("Voice: a legato note leaves the envelopes and the phases alone", "[voice][legato]")
{
    const Ramps ramps{5.0f, 0.0f, 5.0f, 0.0f};
    const auto envelopePatch = [](Voice& v)
    {
        sinePatch(v);
        for (std::size_t p = 0; p < 2; ++p)
        {
            v.envelope(p).setAttack(0.01f);
            v.envelope(p).setDecay(0.1f);
            v.envelope(p).setSustain(0.5f);
            v.envelope(p).setRelease(0.1f);
        }
    };

    SECTION("the envelopes go on through a legato note, and a note-on retriggers them")
    {
        auto legato = std::make_unique<Voice>(d6::minBlepTable64(), 1);
        auto plain = std::make_unique<Voice>(d6::minBlepTable64(), 1);
        auto retrigger = std::make_unique<Voice>(d6::minBlepTable64(), 1);
        for (auto* v : {legato.get(), plain.get(), retrigger.get()})
        {
            envelopePatch(*v);
            v->prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 2});
            v->noteOn(48.0f, ramps.knobs(nullptr, 0), {});
            Buffer out{2, 1200};
            v->process(out.block(), ramps.knobs(nullptr, 0));
            REQUIRE(v->envelope(0).stage() == dsp::AdsrStage::decay);
        }
        legato->legatoNoteOn(55.0f, ramps.knobs(nullptr, 1200), {});
        retrigger->noteOn(55.0f, ramps.knobs(nullptr, 1200), {});
        CHECK(legato->envelope(0).stage() == dsp::AdsrStage::decay);
        CHECK(retrigger->envelope(0).stage() == dsp::AdsrStage::attack);
        for (auto* v : {legato.get(), plain.get()})
        {
            Buffer out{2, 2400};
            v->process(out.block(), ramps.knobs(nullptr, 1200));
        }
        // The pitch moved; the envelope doesn't depend on it: exactly as
        // with no note.
        CHECK_THAT(centsBetween(hzOf(*legato), 55.0), WithinAbs(0.0, 1e-2));
        CHECK_THAT(legato->envelope(0).level(), WithinULP(plain->envelope(0).level(), 0));
        CHECK_THAT(legato->envelope(1).level(), WithinULP(plain->envelope(1).level(), 0));
    }

    SECTION("a legato note on a released voice opens the gate again")
    {
        auto v = preparedVoice(+[](Voice& w) { sinePatch(w); });
        v->envelope(0).setRelease(1.0f);
        v->noteOn(48.0f, ramps.knobs(nullptr, 0), {});
        Buffer out{2, 512};
        v->process(out.block(), ramps.knobs(nullptr, 0));
        v->noteOff();
        v->process(out.block(), ramps.knobs(nullptr, 512));
        REQUIRE_FALSE(v->envelope(0).isGateOn());
        v->legatoNoteOn(52.0f, ramps.knobs(nullptr, 1024), {});
        CHECK(v->envelope(0).isGateOn());
    }

    SECTION("a legato step keeps the waveform continuous, Retrig on and Phase 180")
    {
        // The largest step between samples around the note change, against
        // the sine's own at the new pitch (its slope, 2 pi f / fs x peak).
        const auto maxStep = [](const Buffer& b, std::size_t from, std::size_t to)
        {
            float m = 0.0f;
            const auto y = b.channel(0);
            for (std::size_t i = from + 1; i < to; ++i)
            {
                m = std::max(m, std::abs(y[i] - y[i - 1]));
            }
            return m;
        };
        const auto render = [&](bool legato)
        {
            auto v = preparedVoice(+[](Voice& w) { sinePatch(w); });
            v->slot(0).setPhaseDegrees(180.0f);
            v->noteOn(48.0f, ramps.knobs(nullptr, 0), {});
            Buffer out{2, 9600};
            v->process(out.block().subBlock(0, 4801), ramps.knobs(nullptr, 0));
            if (legato)
            {
                v->legatoNoteOn(55.0f, ramps.knobs(nullptr, 4801), {});
            }
            else
            {
                v->noteOn(55.0f, ramps.knobs(nullptr, 4801), {});
            }
            v->process(out.block().subBlock(4801, 4799), ramps.knobs(nullptr, 4801));
            return out;
        };
        const auto legato = render(true);
        const auto retriggered = render(false);
        // The note changed: the steady stretch is at 55's slope, 1.5 x 48's.
        CHECK(maxStep(legato, 7000, 9600) > maxStep(legato, 2000, 4700) * 1.4f);
        const float steady = maxStep(legato, 7000, 9600);
        CHECK(maxStep(legato, 4700, 4900) <= steady * 1.01f);
        CHECK(maxStep(retriggered, 4700, 4900) > steady * 2.0f);
    }
}

TEST_CASE("Voice: keytrack follows the glide, the bend and the master tune", "[voice][legato]")
{
    // Keytrack 10, the cutoff at 4.5675 Hz x 2^(2 + 4 + (pitch - 9) / 12).
    // currentCutoff() is the cutoff at the end of the segment just rendered,
    // fed at its start for that sample's pitch. Tolerance: the cutoff's law
    // in float, within 2e-5 relative.
    const Ramps ramps{2.0f, 0.0f, 4.0f, 10.0f};
    const auto cutoffAfter = [&](std::size_t n, float masterTune, const dsp::PitchBend* bend, bool glide)
    {
        auto v = preparedVoice(+[](Voice& w) { sinePatch(w); });
        v->setFilterBypassed(0, false);
        v->setMasterTune(masterTune);
        v->noteOn(48.0f, ramps.knobs(bend, 0), {});
        Buffer out{2, n};
        v->process(out.block().subBlock(0, 3200), ramps.knobs(bend, 0));
        if (glide)
        {
            v->legatoNoteOn(60.0f, ramps.knobs(bend, 3200), {.glide = true, .from = 48.0f, .glideSeconds = 0.1f});
        }
        v->process(out.block().subBlock(3200, n - 3200), ramps.knobs(bend, 3200));
        return static_cast<double>(v->filter(0).currentCutoff());
    };
    const double steady = cutoffAfter(4800, 440.0f, nullptr, false);
    // Half way... at 4800, 1600 samples into the 4800-sample glide: 52.
    CHECK_THAT(cutoffAfter(4800, 440.0f, nullptr, true), WithinRel(steady * std::exp2(4.0 / 12.0), 2e-5));
    CHECK_THAT(cutoffAfter(8000 + 3200, 440.0f, nullptr, true), WithinRel(steady * 2.0, 2e-5));

    dsp::PitchBend bend;
    bend.prepare(rate);
    bend.setRange(12.0f);
    bend.setValue(0, 16383);
    CHECK_THAT(cutoffAfter(4800, 440.0f, &bend, false), WithinRel(steady * std::exp2(8191.0 / 8192.0), 2e-5));

    CHECK_THAT(cutoffAfter(4800, 432.0f, nullptr, false), WithinRel(steady * 432.0 / 440.0, 2e-5));
}

TEST_CASE("Voice: the master tune", "[voice][legato]")
{
    auto v = preparedVoice(sinePatch<Voice>);
    const Ramps ramps{5.0f, 0.0f, 5.0f, 0.0f};
    v->setMasterTune(432.0f);
    CHECK_THAT(v->masterTune(), WithinULP(432.0f, 0));
    v->noteOn(69.0f, ramps.knobs(nullptr, 0), {});
    Buffer out{2, 256};
    v->process(out.block(), ramps.knobs(nullptr, 0));
    CHECK(v->slot(0).phaseIncrement() == dsp::incrementFor(432.0f, rate));
    for (const float bad :
         {0.0f, -5.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        v->setMasterTune(bad);
        CHECK_THAT(v->masterTune(), WithinULP(440.0f, 0));
    }
    v->process(out.block(), ramps.knobs(nullptr, 256));
    CHECK(v->slot(0).phaseIncrement() == dsp::incrementFor(440.0f, rate));
}

TEST_CASE("Voice: velocity at once, or ramped over a glide", "[voice][legato]")
{
    auto v = preparedVoice(sinePatch<Voice>);
    const Ramps ramps{5.0f, 0.0f, 5.0f, 0.0f};
    Buffer out{2, 2400};
    CHECK_FALSE(v->hasPlayed());
    v->noteOn(48.0f, ramps.knobs(nullptr, 0), {.velocity = 0.8f});
    CHECK(v->hasPlayed());
    CHECK_THAT(v->velocity(), WithinULP(0.8f, 0));

    // A legato glide: the velocity ramps from where it is over the glide.
    v->legatoNoteOn(60.0f, ramps.knobs(nullptr, 0),
                    {.velocity = 0.2f, .glide = true, .from = 48.0f, .glideSeconds = 0.1f});
    CHECK_THAT(v->velocity(), WithinULP(0.8f, 0));
    v->process(out.block(), ramps.knobs(nullptr, 0));
    CHECK_THAT(v->velocity(), WithinAbs(0.5f, 1e-6));
    v->process(out.block(), ramps.knobs(nullptr, 2400));
    CHECK_THAT(v->velocity(), WithinULP(0.2f, 0));

    // Portamento 0: at once.
    v->legatoNoteOn(55.0f, ramps.knobs(nullptr, 4800), {.velocity = 0.6f});
    CHECK_THAT(v->velocity(), WithinULP(0.6f, 0));

    // A gliding note-on ramps from fromVelocity; a jumping one doesn't.
    v->noteOn(50.0f, ramps.knobs(nullptr, 4800),
              {.velocity = 1.0f, .glide = true, .from = 55.0f, .glideSeconds = 0.1f, .fromVelocity = 0.4f});
    CHECK_THAT(v->velocity(), WithinULP(0.4f, 0));
    v->process(out.block(), ramps.knobs(nullptr, 4800));
    CHECK_THAT(v->velocity(), WithinAbs(0.7f, 1e-6));
    v->noteOn(52.0f, ramps.knobs(nullptr, 7200), {.velocity = 0.3f, .fromVelocity = 0.9f});
    CHECK_THAT(v->velocity(), WithinULP(0.3f, 0));
}

TEST_CASE("Voice: the pitch through stop() and reset()", "[voice][legato]")
{
    auto v = preparedVoice(sinePatch<Voice>);
    const Ramps ramps{5.0f, 0.0f, 5.0f, 0.0f};
    v->noteOn(48.0f, ramps.knobs(nullptr, 0), {});
    v->legatoNoteOn(60.0f, ramps.knobs(nullptr, 0), {.glide = true, .from = 48.0f, .glideSeconds = 0.1f});
    Buffer out{2, 2400};
    v->process(out.block(), ramps.knobs(nullptr, 0));
    CHECK_THAT(v->currentPitch(), WithinULP(54.0f, 0));
    // A stopped voice keeps where it was: Slide glides from there.
    v->stop();
    CHECK_THAT(v->currentPitch(), WithinULP(54.0f, 0));
    CHECK(v->hasPlayed());
    // A note-on without a glide jumps.
    v->noteOn(50.0f, ramps.knobs(nullptr, 2400), {});
    CHECK_THAT(v->currentPitch(), WithinULP(50.0f, 0));
    CHECK_THAT(v->targetPitch(), WithinULP(50.0f, 0));
    (*v).reset();
    CHECK_FALSE(v->hasPlayed());
    CHECK_THAT(v->currentPitch(), WithinULP(69.0f, 0));
    CHECK_THAT(v->velocity(), WithinULP(1.0f, 0));
}

// === The allocator ==================================================================================

TEST_CASE("VoiceAllocator: Mono Legato, last-note priority, legato returns, Polyphony ignored",
          "[voice-allocator][legato]")
{
    auto a = stubs();
    a->setMonoLegato(true);
    a->setPolyphony(0);
    CHECK(a->monoLegato());
    auto& v = a->voice(0);

    block(*a, {on(0, 48)});
    CHECK(v.noteOns == 1);
    CHECK(v.lastCall == StubCall::noteOn);
    CHECK(a->monoKey() == 48);
    CHECK(a->isKeyDown(0));

    block(*a, {on(0, 55)});
    CHECK(v.lastCall == StubCall::legato);
    CHECK_THAT(v.note, WithinULP(55.0f, 0));
    block(*a, {on(0, 52)});
    CHECK_THAT(v.note, WithinULP(52.0f, 0));
    block(*a, {off(0, 52)}); // back to the key held before: 55
    CHECK(v.lastCall == StubCall::legato);
    CHECK_THAT(v.note, WithinULP(55.0f, 0));
    CHECK(a->monoKey() == 55);
    block(*a, {off(0, 55)}); // back to 48
    CHECK_THAT(v.note, WithinULP(48.0f, 0));
    block(*a, {on(0, 55)});
    block(*a, {off(0, 48)}); // not sounding: nothing changes
    CHECK(v.legatoNotes == 5);
    CHECK_THAT(v.note, WithinULP(55.0f, 0));
    block(*a, {off(0, 55)});
    CHECK(v.lastCall == StubCall::noteOff);
    CHECK(v.noteOns == 1);
    CHECK(v.noteOffs == 1);
    CHECK_FALSE(a->isKeyDown(0));

    // Only voice 0 ever plays.
    for (std::size_t i = 1; i < StubAllocator::maxVoices; ++i)
    {
        CHECK(a->voice(i).noteOns + a->voice(i).legatoNotes == 0);
    }

    // A new note after everything is up is a new note (a retrigger in place).
    block(*a, {on(0, 60)});
    CHECK(v.lastCall == StubCall::noteOn);
    CHECK(v.retriggers == 1);
    CHECK(a->activeNotes() == 1);
}

TEST_CASE("VoiceAllocator: Normal and Slide in Mono Legato, from the pitch reached", "[voice-allocator][legato]")
{
    auto a = stubs(0); // a released voice is free at once
    a->setMonoLegato(true);
    a->setPortamento(0.05f);
    auto& v = a->voice(0);

    // Normal: the first note doesn't glide; a legato one does, from the
    // pitch the voice has reached.
    block(*a, {on(0, 48, 100)});
    CHECK_FALSE(v.lastStart.glide);
    CHECK_THAT(v.lastStart.velocity, WithinULP(100.0f / 127.0f, 0));
    v.pitchNow = 49.5f;
    block(*a, {on(0, 52, 64)});
    CHECK(v.lastStart.glide);
    CHECK_THAT(v.lastStart.from, WithinULP(49.5f, 0));
    CHECK_THAT(v.lastStart.glideSeconds, WithinULP(0.05f, 0));
    CHECK_THAT(v.lastStart.velocity, WithinULP(64.0f / 127.0f, 0));
    // A return glides too, from where the voice is, at its velocity now.
    v.pitchNow = 51.0f;
    block(*a, {off(0, 52)});
    CHECK(v.lastCall == StubCall::legato);
    CHECK(v.lastStart.glide);
    CHECK_THAT(v.lastStart.from, WithinULP(51.0f, 0));
    CHECK_THAT(v.lastStart.velocity, WithinULP(64.0f / 127.0f, 0));
    // After a gap, Normal doesn't glide.
    block(*a, {off(0, 48)});
    block(*a, {on(0, 55)});
    CHECK(v.lastCall == StubCall::noteOn);
    CHECK_FALSE(v.lastStart.glide);

    // Slide: after a gap, a new note gliding from the last note's pitch.
    a->setPortamentoMode(dsp::PortamentoMode::slide);
    block(*a, {off(0, 55)});
    block(*a, {on(0, 60, 32)});
    CHECK(v.lastCall == StubCall::noteOn);
    CHECK(v.lastStart.glide);
    CHECK_THAT(v.lastStart.from, WithinULP(55.0f, 0));
    CHECK_THAT(v.lastStart.fromVelocity, WithinULP(1.0f, 0)); // the last note's (127)

    // Portamento 0: never a glide.
    a->setPortamento(0.0f);
    block(*a, {on(0, 62)});
    CHECK(v.lastCall == StubCall::legato);
    CHECK_FALSE(v.lastStart.glide);
}

TEST_CASE("VoiceAllocator: an interrupted glide restarts from the pitch reached (dsp::Voice)",
          "[voice-allocator][legato]")
{
    auto a = std::make_unique<dsp::VoiceAllocator<Voice>>(0, d6::minBlepTable64());
    a->forEachVoice([](Voice& v) { sinePatch(v); });
    a->setMonoLegato(true);
    a->setPortamento(0.2f);
    a->prepare({.sampleRate = rate, .maxBlockSize = 4800, .numChannels = 2});
    const Ramps ramps{5.0f, 0.0f, 5.0f, 0.0f};
    Buffer out{2, 4800};
    a->process(out.block(), std::vector{on(0, 48), on(0, 72)}, ramps.knobs(nullptr, 0)); // half of 9600
    CHECK_THAT(a->voice(0).currentPitch(), WithinAbs(60.0f, 1e-5));
    a->process(out.block(), std::vector{on(0, 64)}, ramps.knobs(nullptr, 0));
    // From 60 to 64 in 200 ms: at 4800 samples, half way.
    CHECK_THAT(a->voice(0).currentPitch(), WithinAbs(62.0f, 1e-5));
}

TEST_CASE("VoiceAllocator: portamento in poly, from the last note played", "[voice-allocator][legato]")
{
    auto a = stubs(0);
    a->setPortamento(0.05f);

    block(*a, {on(0, 48)}); // Normal, nothing else held: no glide
    const auto v48 = voiceOf(*a, 48);
    CHECK_FALSE(a->voice(v48).lastStart.glide);
    a->voice(v48).pitchNow = 48.7f;
    block(*a, {on(0, 52, 64)}); // 48 held: from 48's voice, where it is
    const auto v52 = voiceOf(*a, 52);
    CHECK(a->voice(v52).lastStart.glide);
    CHECK_THAT(a->voice(v52).lastStart.from, WithinULP(48.7f, 0));
    CHECK_THAT(a->voice(v52).lastStart.fromVelocity, WithinULP(1.0f, 0));
    block(*a, {on(0, 55)}); // from the last note, 52's voice
    CHECK_THAT(a->voice(voiceOf(*a, 55)).lastStart.from, WithinULP(52.0f, 0));
    CHECK_THAT(a->voice(voiceOf(*a, 55)).lastStart.fromVelocity, WithinULP(64.0f / 127.0f, 0));

    block(*a, {off(0, 48), off(0, 52), off(0, 55)});
    block(*a, {on(0, 60)}); // Normal, nothing held: no glide
    CHECK_FALSE(a->voice(voiceOf(*a, 60)).lastStart.glide);

    a->setPortamentoMode(dsp::PortamentoMode::slide);
    block(*a, {off(0, 60)});
    block(*a, {on(0, 62)}); // 60's voice is free: from its key
    const auto& v62 = a->voice(voiceOf(*a, 62));
    CHECK(v62.lastStart.glide);
    CHECK_THAT(v62.lastStart.from, WithinULP(60.0f, 0));
}

TEST_CASE("VoiceAllocator: switching Mono Legato releases every note", "[voice-allocator][legato]")
{
    auto a = stubs();
    block(*a, {on(0, 48), on(0, 52), on(0, 55)});
    REQUIRE(a->activeNotes() == 3);
    a->setMonoLegato(true);
    for (std::size_t i = 0; i < 3; ++i)
    {
        CHECK(a->voice(i).noteOffs == 1);
        CHECK_FALSE(a->isKeyDown(i));
    }
    a->setMonoLegato(true); // no change: nothing more
    CHECK(a->voice(0).noteOffs == 1);
    // The keys still down sound only when played again.
    block(*a, {off(0, 52)});
    CHECK(a->voice(1).noteOffs == 1);
    block(*a, {on(0, 60)});
    CHECK(a->voice(0).lastCall == StubCall::noteOn);
    CHECK(a->monoKey() == 60);

    // As an event, at its sample; and back to poly.
    block(*a, {{.offset = 10, .kind = VoiceEventKind::monoLegato, .value = 0}});
    CHECK_FALSE(a->monoLegato());
    CHECK(a->voice(0).noteOffs == 2);
    block(*a, {on(0, 62), on(0, 64)});
    CHECK(voicesOn(*a, 62) == 1);
    CHECK(voicesOn(*a, 64) == 1);
}

TEST_CASE("VoiceAllocator: the hold pedal in poly", "[voice-allocator][legato]")
{
    auto a = stubs();

    SECTION("CC 64 is down at 64 and above")
    {
        block(*a, {pedal(0, 63)});
        CHECK_FALSE(a->isPedalDown());
        block(*a, {pedal(0, 64)});
        CHECK(a->isPedalDown());
        block(*a, {pedal(0, 127)});
        CHECK(a->isPedalDown());
        block(*a, {pedal(0, 0)});
        CHECK_FALSE(a->isPedalDown());
    }

    SECTION("pedal-up releases only keys that are up; a key played again takes a new voice (P7)")
    {
        block(*a, {pedal(0, 127), on(1, 48), off(2, 48)});
        const auto first = voiceOf(*a, 48);
        CHECK(a->voice(first).noteOffs == 0);
        CHECK(a->isSustained(first));
        CHECK_FALSE(a->isKeyDown(first));
        block(*a, {on(0, 48), on(0, 52)});
        CHECK(voicesOn(*a, 48) == 2);
        block(*a, {off(0, 48)}); // the second 48: sustained too
        CHECK(a->activeNotes() == 3);
        block(*a, {pedal(0, 0)});
        std::size_t released = 0;
        for (std::size_t i = 0; i < StubAllocator::maxVoices; ++i)
        {
            const auto& v = a->voice(i);
            if (v.note > 47.0f && v.note < 49.0f && v.noteOns > 0)
            {
                released += v.noteOffs;
                CHECK_FALSE(a->isSustained(i));
            }
        }
        CHECK(released == 2);
        CHECK(a->voice(voiceOf(*a, 52)).noteOffs == 0);
        CHECK(a->isKeyDown(voiceOf(*a, 52)));
    }

    SECTION("the steal order: released notes, then sustained ones, then held keys")
    {
        // 42 held (the oldest), 41 sustained, 40 released (the newest). Only
        // "released, then sustained, then held" steals 40, 41, 42 in that
        // order: counting a sustained note as released (or ignoring the
        // pedal) steals 41 first, and counting it as held steals 42 before it.
        a->setPolyphony(3);
        block(*a, {on(0, 42), on(1, 41), on(2, 40), off(3, 40)}); // 40 released before the pedal
        block(*a, {pedal(0, 127), off(1, 41)});                   // 41 sustained
        REQUIRE(a->isSustained(voiceOf(*a, 41)));
        block(*a, {on(0, 43)});
        CHECK(voicesOn(*a, 40) == 0); // the released note goes first
        CHECK(voicesOn(*a, 41) == 1);
        block(*a, {on(0, 44)});
        CHECK(voicesOn(*a, 41) == 0); // then the sustained one, though 42 is older
        CHECK(voicesOn(*a, 42) == 1);
        block(*a, {on(0, 45)});
        CHECK(voicesOn(*a, 42) == 0); // then the oldest held
        CHECK(a->steals() == 3);
    }
}

TEST_CASE("VoiceAllocator: the hold pedal in Mono Legato", "[voice-allocator][legato]")
{
    auto a = stubs();
    a->setMonoLegato(true);
    const auto& v = a->voice(0);

    block(*a, {pedal(0, 127), on(1, 48), on(2, 52)});
    CHECK(v.lastCall == StubCall::legato);
    block(*a, {off(0, 52)}); // the stack follows the keys: back to 48
    CHECK(v.lastCall == StubCall::legato);
    CHECK(a->monoKey() == 48);
    block(*a, {off(0, 48)}); // the pedal holds the note
    CHECK(v.noteOffs == 0);
    CHECK(a->isSustained(0));
    block(*a, {on(0, 55)}); // a key under a pedal-held note: legato
    CHECK(v.lastCall == StubCall::legato);
    CHECK(v.noteOns == 1);
    CHECK(a->monoKey() == 55);
    block(*a, {pedal(0, 0)}); // 55 is held: it sounds on
    CHECK(v.noteOffs == 0);
    block(*a, {off(0, 55)});
    CHECK(v.noteOffs == 1);

    // Pedal-up with nothing held releases the held note.
    block(*a, {pedal(0, 127), on(0, 60)});
    CHECK(v.lastCall == StubCall::noteOn);
    block(*a, {off(0, 60)});
    CHECK(v.noteOffs == 1);
    block(*a, {pedal(0, 0)});
    CHECK(v.noteOffs == 2);
}

TEST_CASE("VoiceAllocator: the bend reaches every voice at its sample", "[voice-allocator][legato]")
{
    auto a = std::make_unique<dsp::VoiceAllocator<Voice>>(0, d6::minBlepTable64());
    a->forEachVoice(
        [](Voice& v)
        {
            sinePatch(v);
            v.envelope(0).setRelease(1.0f);
        });
    a->prepare({.sampleRate = rate, .maxBlockSize = 2000, .numChannels = 2});
    const Ramps ramps{5.0f, 0.0f, 5.0f, 0.0f};
    Buffer out{2, 2000};
    a->process(out.block(), std::vector{on(0, 48), on(0, 55), off(10, 55), bendEvent(100, 16383)},
               ramps.knobs(nullptr, 0));
    REQUIRE(a->noteOf(0) == 48);
    REQUIRE(a->noteOf(1) == 55);
    REQUIRE_FALSE(a->isKeyDown(1));
    const double full = 2.0 * 8191.0 / 8192.0;
    CHECK_THAT(centsBetween(hzOf(a->voice(0)), 48.0 + full), WithinAbs(0.0, 1e-2));
    CHECK_THAT(centsBetween(hzOf(a->voice(1)), 55.0 + full), WithinAbs(0.0, 1e-2)); // releasing, still bent

    // A range change applies from the next value.
    a->setBendRange(12.0f);
    a->process(out.block(), {}, ramps.knobs(nullptr, 0));
    CHECK_THAT(centsBetween(hzOf(a->voice(0)), 48.0 + full), WithinAbs(0.0, 1e-2));
    a->process(out.block(), std::vector{bendEvent(0, 0)}, ramps.knobs(nullptr, 0));
    CHECK_THAT(centsBetween(hzOf(a->voice(0)), 36.0), WithinAbs(0.0, 1e-2));

    // reset(): no bend, no pedal, no mono key.
    a->setMonoLegato(true);
    a->process(out.block(), std::vector{pedal(0, 127), on(0, 50)}, ramps.knobs(nullptr, 0));
    (*a).reset();
    CHECK_FALSE(a->isPedalDown());
    CHECK(a->monoKey() == -1);
    CHECK(a->activeNotes() == 0);
    a->process(out.block(), std::vector{on(0, 50)}, ramps.knobs(nullptr, 0));
    CHECK_THAT(centsBetween(hzOf(a->voice(0)), 50.0), WithinAbs(0.0, 1e-2));
}

TEST_CASE("VoiceAllocator: velocity reaches the voice", "[voice-allocator][legato]")
{
    auto a = stubs();
    block(*a, {on(0, 48, 64), on(0, 50, 0), on(0, 52, 200)});
    CHECK_THAT(a->voice(voiceOf(*a, 48)).lastStart.velocity, WithinULP(64.0f / 127.0f, 0));
    CHECK_THAT(a->voice(voiceOf(*a, 50)).lastStart.velocity, WithinULP(1.0f / 127.0f, 0));
    CHECK_THAT(a->voice(voiceOf(*a, 52)).lastStart.velocity, WithinULP(1.0f, 0));
}

TEST_CASE("VoiceAllocator: Mono Legato, portamento, bend and pedal are the stage-1 prototype's, bit for bit",
          "[voice-allocator][legato]")
{
    for (const bool mono : {true, false})
    {
        const auto reference = runPrototypeEngine(mono);
        for (const std::size_t hostBlock : {1U, 37U, 4096U})
        {
            std::size_t allocations = 1;
            const auto built = runLibraryAllocator(mono, hostBlock, allocations);
            CHECK(bitIdentical(built, reference));
            CHECK(allocations == 0);
        }
    }
}
