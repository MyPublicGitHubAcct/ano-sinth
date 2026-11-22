// Tests for the voice allocator (libs/dsp/.../voice/voice_allocator.hpp;
// docs/dsp/voice-allocation.md, M1.4 task 2): Polyphony 0-16 over a fixed
// pool of 16 voices; a new note steals the oldest released voice, else the
// oldest, and retriggers it in place; a repeated key takes a new voice; a
// voice is freed (and stopped) when isActive() turns false, checked at
// every event and at each voice's own 32-sample points; a lowered
// Polyphony leaves sounding notes, 0 ignores new ones, note-offs reach
// every voice; the Voices readout per part.
//
// The allocation logic is pinned on a stub voice that records what it is
// told; the whole on dsp::Voice against the stage-1 prototype allocator
// (allocation_candidates.hpp) in its decided configuration, bit for bit.

#include "allocation_candidates.hpp"

#include "../filters/filter_control_support.hpp"
#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

using namespace anoesis::test;
namespace dsp = anoesis::dsp;
using dsp::VoiceEvent;
using dsp::VoiceEventKind;
using filter_control::constant;
using filter_control::KnobRamps;
using filter_control::Knobs;

namespace
{

constexpr float rate = 48000.0f;

VoiceEvent on(std::size_t offset, int key)
{
    return {.offset = offset, .kind = VoiceEventKind::noteOn, .value = key};
}
VoiceEvent off(std::size_t offset, int key)
{
    return {.offset = offset, .kind = VoiceEventKind::noteOff, .value = key};
}
VoiceEvent polyphony(std::size_t offset, int notes)
{
    return {.offset = offset, .kind = VoiceEventKind::polyphony, .value = notes};
}

// --- A stub voice --------------------------------------------------------------------------

struct StubKnobs
{
    std::size_t offset = 0;

    [[nodiscard]] StubKnobs advanced(std::size_t samples) const noexcept
    {
        return {.offset = offset + samples};
    }
};

/// Records what the allocator tells it. Active from a note-on until
/// `release` samples after its note-off (0: at once), counted in the blocks
/// it is given; it reports one oscillator voice while active.
class StubVoice
{
public:
    using Knobs = StubKnobs;

    explicit StubVoice(std::uint64_t voiceSeed) noexcept : seed(voiceSeed) {}

    void prepare(const dsp::ProcessSpec& /*spec*/) noexcept {}
    void reset() noexcept
    {
        active = false;
        gate = false;
    }
    void noteOn(float key, const Knobs& knobs) noexcept
    {
        retriggers += active ? 1 : 0;
        ++noteOns;
        note = key;
        gate = true;
        active = true;
        lastOffset = knobs.offset;
    }
    void noteOff() noexcept
    {
        ++noteOffs;
        gate = false;
        remaining = release;
        active = release > 0;
    }
    void stop() noexcept
    {
        ++stops;
        active = false;
        gate = false;
    }
    void process(const dsp::AudioBlock& out, const Knobs& /*knobs*/) noexcept
    {
        rendered += out.numSamples();
        if (active && !gate)
        {
            remaining -= std::min(remaining, out.numSamples());
            active = remaining > 0;
        }
    }
    [[nodiscard]] bool isActive() const noexcept
    {
        return active;
    }
    [[nodiscard]] std::size_t activeOscillatorVoices() const noexcept
    {
        return active ? 1 : 0;
    }
    [[nodiscard]] static std::size_t oscillatorVoicesPerNote() noexcept
    {
        return 1;
    }

    std::uint64_t seed;
    std::size_t release = 0;
    std::size_t remaining = 0;
    bool active = false;
    bool gate = false;
    float note = 0.0f;
    std::size_t noteOns = 0;
    std::size_t noteOffs = 0;
    std::size_t retriggers = 0; ///< note-ons on an active voice
    std::size_t stops = 0;
    std::size_t rendered = 0;
    std::size_t lastOffset = 0;
};

static_assert(dsp::AllocatableVoice<StubVoice>);
static_assert(dsp::AllocatableVoice<dsp::Voice<dsp::minBlepTaps>>);

using StubAllocator = dsp::VoiceAllocator<StubVoice>;

/// A stub allocator, prepared, every voice with a release of `release`.
std::unique_ptr<StubAllocator> stubs(std::size_t release = 0, std::size_t notes = 16)
{
    auto a = std::make_unique<StubAllocator>(42);
    a->forEachVoice([release](StubVoice& v) { v.release = release; });
    a->setPolyphony(notes);
    a->prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
    return a;
}

/// One block of `n` samples with `events`.
void block(StubAllocator& a, std::vector<VoiceEvent> events, std::size_t n = 64)
{
    Buffer out{2, n};
    a.process(out.block(), events, {});
}

/// The voice playing `key` with its key down, or maxVoices.
std::size_t heldVoiceOf(const StubAllocator& a, int key)
{
    for (std::size_t i = 0; i < StubAllocator::maxVoices; ++i)
    {
        if (a.isBusy(i) && a.noteOf(i) == key && a.isKeyDown(i))
        {
            return i;
        }
    }
    return StubAllocator::maxVoices;
}

std::size_t busyWith(const StubAllocator& a, int key)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < StubAllocator::maxVoices; ++i)
    {
        n += a.isBusy(i) && a.noteOf(i) == key ? 1 : 0;
    }
    return n;
}

// --- dsp::Voice through both allocators -------------------------------------------------------

using Voice = dsp::Voice<dsp::minBlepTaps>;
using Allocator = dsp::VoiceAllocator<Voice>;

/// The resonant pad of the stage-1 report, free-running, drift on.
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

/// A sequence with steals, releases, a repeated key and two Polyphony
/// changes, at absolute samples off every grid.
std::vector<allocation::Event> sequence()
{
    using allocation::Event;
    using allocation::EventKind;
    std::vector<Event> e{
        {.at = 0, .kind = EventKind::noteOn, .value = 36},
        {.at = 50000, .kind = EventKind::noteOff, .value = 36},
        {.at = 30001, .kind = EventKind::polyphony, .value = 2},
        {.at = 60007, .kind = EventKind::polyphony, .value = 6},
    };
    const std::array<int, 6> melody{60, 64, 67, 60, 72, 64};
    for (std::size_t i = 0; i < 16; ++i)
    {
        const auto at = 1000 + (i * 4001);
        e.push_back({.at = at, .kind = EventKind::noteOn, .value = melody[i % melody.size()]});
        e.push_back({.at = at + 2000 + i, .kind = EventKind::noteOff, .value = melody[i % melody.size()]});
    }
    std::ranges::stable_sort(e, {}, &Event::at);
    return e;
}

/// The knobs: a cutoff step at 20003 and a resonance on filter A.
Knobs knobs()
{
    Knobs k;
    k.cutoff = [](std::size_t at)
    {
        return at < 20003 ? 3.0f : 4.5f;
    };
    k.resonance = constant(5.0f);
    return k;
}

VoiceEventKind kindOf(allocation::EventKind kind)
{
    switch (kind)
    {
    case allocation::EventKind::noteOff:
        return VoiceEventKind::noteOff;
    case allocation::EventKind::polyphony:
        return VoiceEventKind::polyphony;
    case allocation::EventKind::noteOn:
        break;
    }
    return VoiceEventKind::noteOn;
}

struct LibraryRun
{
    Buffer out{2, 1};
    std::size_t allocations = 0;
    std::size_t steals = 0;
};

/// The library allocator over the sequence, in host blocks of `hostBlock`
/// (split at knob events as the parameter layer would), Polyphony 4.
LibraryRun runLibrary(std::size_t length, std::size_t hostBlock)
{
    LibraryRun r;
    r.out = Buffer{2, length};
    const auto blockOut = r.out.block();
    KnobRamps a{knobs(), rate};
    KnobRamps b{knobs(), rate};
    auto alloc = std::make_unique<Allocator>(0, d6::minBlepTable64());
    alloc->forEachVoice([](Voice& v) { padPatch(v); });
    alloc->setPolyphony(4);
    const auto events = sequence();
    std::vector<VoiceEvent> blockEvents;
    blockEvents.reserve(events.size());
    {
        const AllocationCounter counter;
        alloc->prepare({.sampleRate = rate, .maxBlockSize = hostBlock, .numChannels = 2});
        for (std::size_t start = 0; start < length;)
        {
            auto end = std::min(length, start + hostBlock);
            for (auto at = start + 1; at < end; ++at)
            {
                if (a.eventAt(at))
                {
                    end = at;
                    break;
                }
            }
            if (a.eventAt(start))
            {
                a.applyEvents(start);
                b.applyEvents(start);
            }
            blockEvents.clear();
            for (const auto& e : events)
            {
                if (e.at >= start && e.at < end)
                {
                    blockEvents.push_back({.offset = e.at - start, .kind = kindOf(e.kind), .value = e.value});
                }
            }
            const dsp::VoiceFilterKnobs k{
                .cutoff = {&a.cutoff, &b.cutoff},
                .resonance = {&a.resonance, &b.resonance},
                .controlCutoff = &a.controlCutoff,
                .controlResonance = &a.controlResonance,
            };
            alloc->process(blockOut.subBlock(start, end - start), blockEvents, k);
            a.advance(end - start);
            b.advance(end - start);
            start = end;
        }
        r.allocations = counter.count();
    }
    r.steals = alloc->steals();
    return r;
}

/// The stage-1 prototype allocator in its decided configuration, over the
/// same sequence and knobs, in host blocks of 4096 split at knob events.
Buffer runPrototype(std::size_t length)
{
    allocation::GlobalKnobs global{knobs(), knobs(), rate};
    allocation::Policy policy;
    policy.steal = allocation::Steal::releasingThenOldest;
    policy.repeatedKey = allocation::RepeatedKey::newVoice;
    policy.end = allocation::StealEnd::retrigger;
    policy.freeing = allocation::Freeing::releaseEnd;
    policy.busy = allocation::Busy::audibleEnvelopes;
    policy.drop = allocation::PolyphonyDrop::leave;
    auto alloc = std::make_unique<allocation::Allocator<allocation::BuiltVoice, 16>>(
        allocation::Context{.table = &d6::minBlepTable64(), .knobs = &global}, policy);
    alloc->forEachVoice([](allocation::BuiltVoice& v) { padPatch(v); });
    alloc->setPolyphony(4);
    alloc->prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 2});
    const auto events = sequence();
    Buffer out{2, length};
    const auto blockOut = out.block();
    for (std::size_t start = 0; start < length;)
    {
        auto end = std::min(length, start + 4096);
        for (auto at = start + 1; at < end; ++at)
        {
            if (global.a.eventAt(at))
            {
                end = at;
                break;
            }
        }
        if (global.a.eventAt(start))
        {
            global.a.applyEvents(start);
            global.b.applyEvents(start);
        }
        std::vector<allocation::Event> blockEvents;
        for (const auto& e : events)
        {
            if (e.at >= start && e.at < end)
            {
                blockEvents.push_back(e);
            }
        }
        alloc->process(blockOut.subBlock(start, end - start), blockEvents, start);
        global.advance(end - start);
        start = end;
    }
    return out;
}

bool sameBits(const Buffer& x, const Buffer& y)
{
    for (std::size_t c = 0; c < 2; ++c)
    {
        if (!std::ranges::equal(x.channel(c), y.channel(c), [](float p, float q)
                                { return std::bit_cast<std::uint32_t>(p) == std::bit_cast<std::uint32_t>(q); }))
        {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("VoiceAllocator: matches the stage-1 prototype's decided configuration bit for bit", "[voice-allocator]")
{
    // The same dsp::Voices, seeds, events and knobs; the prototype allocator
    // configured as decided (released first, then oldest; a retrigger in
    // place; a new voice for a repeated key; freed when isActive() turns
    // false, at events and each voice's 32-sample points; a lowered
    // Polyphony leaving sounding notes). Same decisions at the same samples,
    // so the same output to the bit, at any host block size, with nothing
    // allocated from prepare() on.
    const auto length = static_cast<std::size_t>(1.5f * rate);
    const auto reference = runPrototype(length);
    for (const std::size_t hostBlock : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
    {
        CAPTURE(hostBlock);
        const auto r = runLibrary(length, hostBlock);
        CHECK(sameBits(r.out, reference));
        CHECK(r.allocations == 0);
        CHECK(r.steals >= 4); // the sequence steals, at Polyphony 4 and 2
    }
    CHECK_FALSE(
        std::ranges::all_of(reference.channel(0), [](float x) { return std::bit_cast<std::uint32_t>(x) == 0U; }));
}

TEST_CASE("VoiceAllocator: a new note takes a free voice, else steals the oldest released, else the oldest",
          "[voice-allocator]")
{
    auto a = stubs(1000, 3);
    block(*a, {on(0, 60), on(1, 62), on(2, 64)});
    CHECK(a->activeNotes() == 3);
    // All held: the oldest (60) is stolen, retriggered in place, not stopped.
    const auto v60 = heldVoiceOf(*a, 60);
    block(*a, {on(0, 65)});
    CHECK(heldVoiceOf(*a, 60) == StubAllocator::maxVoices);
    CHECK(heldVoiceOf(*a, 65) == v60);
    CHECK(a->voice(v60).retriggers == 1);
    CHECK(a->voice(v60).stops == 0);
    CHECK(a->activeNotes() == 3);
    // 64 released (still sounding): it goes before the older held 62.
    const auto v64 = heldVoiceOf(*a, 64);
    block(*a, {off(0, 64), on(10, 67)});
    CHECK(heldVoiceOf(*a, 67) == v64);
    CHECK(heldVoiceOf(*a, 62) < StubAllocator::maxVoices);
    // Two released: the older of them goes first.
    const auto v62 = heldVoiceOf(*a, 62);
    block(*a, {off(0, 62), off(1, 65), on(10, 69)});
    CHECK(heldVoiceOf(*a, 69) == v62);
    CHECK(a->steals() == 3);
}

TEST_CASE("VoiceAllocator: a repeated key takes a new voice; its note-off releases the held one", "[voice-allocator]")
{
    auto a = stubs(10000);
    block(*a, {on(0, 60), off(20, 60), on(40, 60)});
    CHECK(busyWith(*a, 60) == 2); // the release overlaps the new note
    CHECK(a->activeNotes() == 2);
    const auto held = heldVoiceOf(*a, 60);
    REQUIRE(held < StubAllocator::maxVoices);
    CHECK(a->voice(held).retriggers == 0); // from silence, not the old voice
    block(*a, {off(5, 60)});
    CHECK(heldVoiceOf(*a, 60) == StubAllocator::maxVoices);
    CHECK(a->voice(held).noteOffs == 1);
    std::size_t offs = 0;
    a->forEachVoice([&offs](StubVoice& v) { offs += v.noteOffs; });
    CHECK(offs == 2); // each voice released once
}

TEST_CASE("VoiceAllocator: a finished voice is freed and stopped at its next check point or event", "[voice-allocator]")
{
    SECTION("At the voice's next 32-sample point from its note-on")
    {
        // On at 0, off at 64, a release of 40 samples, blocks of 16: the
        // stub runs out at the end of the piece [96, 112), and is freed at
        // the check point 128.
        auto a = stubs(40);
        for (std::size_t start = 0; start < 112; start += 16)
        {
            std::vector<VoiceEvent> events;
            if (start == 0)
            {
                events.push_back(on(0, 60));
            }
            if (start == 64)
            {
                events.push_back(off(0, 60));
            }
            block(*a, events, 16);
        }
        CHECK_FALSE(a->voice(0).isActive());
        CHECK(a->activeNotes() == 1); // not yet checked
        block(*a, {}, 16);
        CHECK(a->activeNotes() == 0);
        CHECK(a->voice(0).stops == 1);
        // Freed voices aren't rendered.
        const auto rendered = a->voice(0).rendered;
        block(*a, {}, 512);
        CHECK(a->voice(0).rendered == rendered);
    }
    SECTION("Counted from the voice's latest note-on: a retrigger restarts them")
    {
        // Polyphony 1: key 60 at 0, key 62 at 10 steals and retriggers the
        // voice, released at 11 with a release of 5 samples. The stub runs
        // out in its first piece after 11; from the retrigger the check
        // points are 42, 74, ..., so at the block's end (40) the voice is
        // still busy; counted from 0 it would have been freed at 32.
        auto a = stubs(5, 1);
        block(*a, {on(0, 60), on(10, 62), off(11, 62)}, 40);
        CHECK_FALSE(a->voice(0).isActive());
        CHECK(a->activeNotes() == 1);
        block(*a, {}, 2);
        CHECK(a->activeNotes() == 0);
    }
    SECTION("At an event, before the event acts")
    {
        // Polyphony 1: off at 64, a release of 40 samples; the stub runs out
        // in the piece [96, 110), before the check point 128. The note-on at
        // 110 frees it first, and takes it from silence instead of stealing.
        auto a = stubs(40, 1);
        block(*a, {on(0, 60)}, 64);
        block(*a, {off(0, 60), on(46, 62)}, 64);
        CHECK(a->steals() == 0);
        CHECK(a->voice(0).stops == 1);
        CHECK(a->voice(0).retriggers == 0);
        CHECK(heldVoiceOf(*a, 62) == 0);
    }
}

TEST_CASE("VoiceAllocator: Polyphony 0 ignores new notes; lowered, sounding notes play on", "[voice-allocator]")
{
    auto a = stubs(100000, 8);
    block(*a, {on(0, 60), on(1, 62), on(2, 64), on(3, 65)});
    block(*a, {polyphony(0, 2)});
    CHECK(a->polyphony() == 2);
    CHECK(a->activeNotes() == 4); // left sounding
    CHECK(a->maximumVoices() == 2);
    // A new note at the lowered limit replaces one (the oldest), not adds one.
    block(*a, {on(0, 67)});
    CHECK(a->activeNotes() == 4);
    CHECK(heldVoiceOf(*a, 60) == StubAllocator::maxVoices);
    block(*a, {polyphony(0, 0), on(5, 69)});
    CHECK(heldVoiceOf(*a, 69) == StubAllocator::maxVoices);
    CHECK(a->ignored() == 1);
    CHECK(a->maximumVoices() == 0);
    // Note-offs reach every voice, beyond any limit.
    block(*a, {off(0, 62), off(1, 64), off(2, 65), off(3, 67)});
    std::size_t held = 0;
    for (std::size_t i = 0; i < StubAllocator::maxVoices; ++i)
    {
        held += a->isBusy(i) && a->isKeyDown(i) ? 1 : 0;
    }
    CHECK(held == 0);
    // Out-of-range values clamp to [0, 16].
    block(*a, {polyphony(0, 40)});
    CHECK(a->polyphony() == 16);
    block(*a, {polyphony(0, -3)});
    CHECK(a->polyphony() == 0);
}

TEST_CASE("VoiceAllocator: a note-on reaches its voice at its sample, the knobs advanced to it", "[voice-allocator]")
{
    auto a = stubs();
    block(*a, {on(13, 60), on(13, 62), on(50, 64)});
    CHECK(a->voice(heldVoiceOf(*a, 60)).lastOffset == 13);
    CHECK(a->voice(heldVoiceOf(*a, 62)).lastOffset == 13);
    CHECK(a->voice(heldVoiceOf(*a, 64)).lastOffset == 50);
    // Rendered from its note-on only: 64 - 50 samples of this block.
    CHECK(a->voice(heldVoiceOf(*a, 64)).rendered == 14);
    // An offset past the block counts as its end: handled, not dropped.
    block(*a, {on(70, 65)});
    REQUIRE(heldVoiceOf(*a, 65) < StubAllocator::maxVoices);
    CHECK(a->voice(heldVoiceOf(*a, 65)).lastOffset == 64);
    CHECK(a->voice(heldVoiceOf(*a, 65)).rendered == 0);
}

TEST_CASE("VoiceAllocator: each voice's seed comes from the instance's and its index", "[voice-allocator]")
{
    StubAllocator a{1234};
    std::vector<std::uint64_t> seeds;
    for (std::size_t i = 0; i < StubAllocator::maxVoices; ++i)
    {
        CHECK(a.voice(i).seed == dsp::voiceSeed(1234, i));
        seeds.push_back(a.voice(i).seed);
    }
    std::ranges::sort(seeds);
    CHECK(std::ranges::adjacent_find(seeds) == seeds.end());
    CHECK(dsp::voiceSeed(1234, 3) == (1234U ^ (0xD1B54A32D192ED03U * 4U)));
}

TEST_CASE("VoiceAllocator: the Voices readout", "[voice-allocator]")
{
    // The manual's screenshot, Polyphony 12, Voices 204/336: 28 voices per
    // note (A1 8, A2 4, B1 8, B2 8); envelope A releases in 300 ms, B in
    // 2 s; 9 notes held, 4 released, read 0.5 s later: 5 x 28 + 4 x 16.
    auto alloc = std::make_unique<Allocator>(0, d6::minBlepTable64());
    alloc->forEachVoice(
        [](Voice& v)
        {
            const std::array<int, 4> voices{8, 4, 8, 8};
            for (std::size_t s = 0; s < 4; ++s)
            {
                v.slot(s).setVoices(voices[s]);
            }
            v.envelope(0).setRelease(0.3f);
            v.envelope(1).setRelease(2.0f);
        });
    alloc->setPolyphony(12);
    alloc->prepare({.sampleRate = rate, .maxBlockSize = 480, .numChannels = 2});
    const Knobs defaults;
    KnobRamps ramps{defaults, rate};
    const dsp::VoiceFilterKnobs k{
        .cutoff = {&ramps.cutoff, &ramps.cutoff},
        .resonance = {&ramps.resonance, &ramps.resonance},
        .controlCutoff = &ramps.controlCutoff,
        .controlResonance = &ramps.controlResonance,
    };
    Buffer out{2, 480};
    CHECK(alloc->maximumVoices() == 336);
    CHECK(alloc->activeVoices() == 0);
    std::vector<VoiceEvent> chord;
    for (int key = 48; key < 57; ++key)
    {
        chord.push_back(on(static_cast<std::size_t>(key - 48) * 40, key));
    }
    alloc->process(out.block(), chord, k);
    CHECK(alloc->activeNotes() == 9);
    CHECK(alloc->activeVoices() == 9 * 28);
    alloc->process(out.block(), std::vector{off(0, 48), off(0, 49), off(0, 50), off(0, 51)}, k);
    for (int i = 0; i < 50; ++i)
    {
        alloc->process(out.block(), {}, k);
    }
    CHECK(alloc->activeNotes() == 9);
    CHECK(alloc->activeVoices() == 204);
    CHECK(alloc->maximumVoices() == 336);
}

TEST_CASE("VoiceAllocator: reset() is as freshly prepared", "[voice-allocator]")
{
    auto a = stubs(1000, 1);
    block(*a, {on(0, 60), on(1, 62), polyphony(2, 0), on(3, 64)});
    REQUIRE(a->steals() == 1);
    REQUIRE(a->ignored() == 1);
    (*a).reset();
    CHECK(a->ignored() == 0);
    CHECK(a->activeNotes() == 0);
    a->setPolyphony(16); // reset() keeps Polyphony (0 here)
    CHECK(a->steals() == 0);
    block(*a, {on(0, 64)});
    CHECK(heldVoiceOf(*a, 64) == 0);
}
