// Tests for the voice (libs/dsp/.../voice/voice.hpp; docs/dsp/voice-architecture.md,
// M1.4 task 1): two parts, each two OscillatorSlots, a LadderFilter and an
// amp Adsr; each filter's Input Select own, AB, or "--"; a part crossing into
// the other filter through its own envelope before the filter and the
// filter's part's after; unselected parts and "--" filters not run; Bypass
// obeying the select; AB a plain sum; a filter input level; the filters fed
// through the Filter Control at the voice's own 32-sample grid.
//
// The stage-1 prototype (voice_candidates.hpp) in its decided configuration
// is the reference the voice must match bit for bit on fixed routings.

#include "voice_candidates.hpp"

#include "../filters/filter_control_support.hpp"
#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/voice/voice.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <utility>
#include <vector>

using namespace anoesis::test;
namespace dsp = anoesis::dsp;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;
using dsp::FilterInput;
using filter_control::constant;
using filter_control::KnobRamps;
using filter_control::Knobs;

namespace
{

constexpr float rate = 48000.0f;
using Voice = dsp::Voice<dsp::minBlepTaps>;
using Prototype = voice::PrototypeVoice<dsp::minBlepTaps>;
using Slot = dsp::OscillatorSlot<dsp::minBlepTaps>;

std::size_t samples(double seconds)
{
    return static_cast<std::size_t>(seconds * static_cast<double>(rate));
}

dsp::VoiceFilterKnobs knobsOf(const KnobRamps& a, const KnobRamps& b, float keytrack = 0.0f)
{
    return {
        .cutoff = {&a.cutoff, &b.cutoff},
        .resonance = {&a.resonance, &b.resonance},
        .controlCutoff = &a.controlCutoff,
        .controlResonance = &a.controlResonance,
        .keytrack = keytrack,
    };
}

bool sameBits(std::span<const float> a, std::span<const float> b)
{
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](float x, float y)
                              { return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y); });
}

bool sameBits(const Buffer& a, const Buffer& b)
{
    return sameBits(a.channel(0), b.channel(0)) && sameBits(a.channel(1), b.channel(1));
}

bool allZero(std::span<const float> x)
{
    return std::ranges::all_of(x, [](float v) { return std::bit_cast<std::uint32_t>(v) == 0U; });
}

// --- Patches, the same on the voice and on the prototype -----------------------------

/// Slots and envelopes, as the prototype's setWorstPatch() sets them (both
/// classes have slot() and envelope()).
template <typename V>
void worstOscillators(V& v)
{
    v.drift().setDepthCents(dsp::PitchDrift::defaultDepthCents); // the plugin's default, on
    for (std::size_t s = 0; s < 4; ++s)
    {
        auto& slot = v.slot(s);
        slot.setWaveform(dsp::Waveform::saw);
        slot.setVoices(8);
        slot.setDetune(0.548f);
        slot.setStereo(1.0f);
        slot.setRetrigger(false);
        slot.setTuning({.octaves = 0, .semitones = s < 2 ? 0 : 7, .fine = 0.0f});
    }
    for (std::size_t p = 0; p < 2; ++p)
    {
        auto& e = v.envelope(p);
        e.setAttack(0.005f);
        e.setDecay(0.3f);
        e.setSustain(0.7f);
        e.setRelease(0.2f);
    }
}

/// One saw (1 voice, Retrig on, Phase 0) in each listed slot, the rest off;
/// both envelopes a gate (A 0, D 0, S 1, R 0).
template <typename V>
void saws(V& v, std::initializer_list<std::size_t> on, dsp::Waveform shape = dsp::Waveform::saw)
{
    for (std::size_t s = 0; s < 4; ++s)
    {
        v.slot(s).setWaveform(shape);
        v.slot(s).setVoices(std::ranges::find(on, s) != on.end() ? 1 : 0);
    }
    for (std::size_t p = 0; p < 2; ++p)
    {
        auto& e = v.envelope(p);
        e.setAttack(0.0f);
        e.setDecay(0.0f);
        e.setSustain(1.0f);
        e.setRelease(0.0f);
    }
}

struct Routing
{
    FilterInput a = FilterInput::own;
    FilterInput b = FilterInput::own;
    float drive = 1.0f;
    bool bypassB = false;
};

void route(Voice& v, const Routing& r)
{
    v.setFilterInput(0, r.a);
    v.setFilterInput(1, r.b);
    v.setFilterDrive(0, r.drive);
    v.setFilterDrive(1, r.drive);
    v.setFilterBypassed(1, r.bypassB);
}

voice::Input prototypeInput(FilterInput i)
{
    switch (i)
    {
    case FilterInput::none:
        return voice::Input::none;
    case FilterInput::both:
        return voice::Input::both;
    case FilterInput::own:
        break;
    }
    return voice::Input::own;
}

void route(Prototype& v, const Routing& r)
{
    voice::Patch patch;
    patch.filters[0].input = prototypeInput(r.a);
    patch.filters[1].input = prototypeInput(r.b);
    patch.filters[0].drive = r.drive;
    patch.filters[1].drive = r.drive;
    patch.filters[1].bypassed = r.bypassB;
    v.setPatch(patch);
}

// --- Rendering -------------------------------------------------------------------------

/// Something done to the voice between two process() calls, at a sample.
struct Action
{
    std::size_t at = 0;
    std::function<void(Voice&)> act;
};

const std::vector<Action>& noActions()
{
    static const std::vector<Action> none;
    return none;
}

const Knobs& defaultKnobs()
{
    static const Knobs knobs;
    return knobs;
}

const voice::Readings& decidedReadings()
{
    static const voice::Readings readings;
    return readings;
}

struct Run
{
    Buffer out{2, 1};
    std::size_t allocations = 0;
};

/// The voice from a note-on at sample 0, in host blocks of `hostBlock`, split
/// at the note-off and at each action; the knobs as given (ramped by the
/// stand-in parameter layer, which moves them at their events).
Run renderVoice(const std::function<void(Voice&)>& configure, std::size_t length, std::size_t noteOff,
                std::size_t hostBlock, const std::vector<Action>& actions = noActions(),
                const Knobs& knobs = defaultKnobs(), float keytrack = 0.0f, float note = 45.0f)
{
    Run r;
    r.out = Buffer{2, length};
    const auto block = r.out.block();
    KnobRamps a{knobs, rate};
    KnobRamps b{knobs, rate};
    Voice v{d6::minBlepTable64()}; // the shared table is built on its first use
    configure(v);
    {
        // From prepare() on, nothing allocates.
        const AllocationCounter counter;
        v.prepare({.sampleRate = rate, .maxBlockSize = hostBlock, .numChannels = 2});
        v.noteOn(note, knobsOf(a, b, keytrack));
        for (std::size_t start = 0; start < length; start += hostBlock)
        {
            const auto end = std::min(length, start + hostBlock);
            for (std::size_t from = start; from < end;)
            {
                auto to = end;
                if (from < noteOff && noteOff < to)
                {
                    to = noteOff;
                }
                for (const auto& action : actions)
                {
                    if (from < action.at && action.at < to)
                    {
                        to = action.at;
                    }
                }
                if (a.eventAt(from))
                {
                    a.applyEvents(from);
                    b.applyEvents(from);
                }
                if (from == noteOff)
                {
                    v.noteOff();
                }
                for (const auto& action : actions)
                {
                    if (action.at == from)
                    {
                        action.act(v);
                    }
                }
                // Knob events end a piece too.
                for (auto at = from + 1; at < to; ++at)
                {
                    if (a.eventAt(at))
                    {
                        to = at;
                        break;
                    }
                }
                v.process(block.subBlock(from, to - from), knobsOf(a, b, keytrack));
                a.advance(to - from);
                b.advance(to - from);
                from = to;
            }
        }
        r.allocations = counter.count();
    }
    return r;
}

/// The prototype in the decided configuration (I2, unselected silent, Bypass
/// obeying the select, AB a plain sum; per-filter buffers, the grid
/// schedule, unheard parts skipped), the same host loop with no actions.
Buffer renderPrototype(const std::function<void(Prototype&)>& configure, std::size_t length, std::size_t noteOff,
                       std::size_t hostBlock, const Knobs& knobs = defaultKnobs())
{
    Prototype v{d6::minBlepTable64(),
                decidedReadings(),
                {.layout = voice::Layout::perFilter, .schedule = voice::Schedule::grid32, .skipUnheard = true}};
    configure(v);
    v.prepare({.sampleRate = rate, .maxBlockSize = hostBlock, .numChannels = 1});
    KnobRamps a{knobs, rate};
    KnobRamps b{knobs, rate};
    const voice::FilterFeed feed{.a = &a, .b = &b, .keytrack = 0.0f};
    Buffer out{2, length};
    const auto block = out.block();
    v.noteOn(45.0f, feed, 0);
    for (std::size_t start = 0; start < length; start += hostBlock)
    {
        const auto end = std::min(length, start + hostBlock);
        for (std::size_t from = start; from < end;)
        {
            auto to = (from < noteOff && noteOff < end) ? noteOff : end;
            for (auto at = from + 1; at < to; ++at)
            {
                if (a.eventAt(at))
                {
                    to = at;
                    break;
                }
            }
            if (a.eventAt(from))
            {
                a.applyEvents(from);
                b.applyEvents(from);
            }
            if (from == noteOff)
            {
                v.noteOff();
            }
            v.render(block.subBlock(from, to - from), feed, from);
            a.advance(to - from);
            b.advance(to - from);
            from = to;
        }
    }
    return out;
}

/// A bare slot: one saw at note 45, Retrig on, rendered alone (what part B
/// presents when nothing else touches it).
Buffer bareSlot(std::size_t index, std::size_t length)
{
    Slot slot{d6::minBlepTable64()};
    slot.setRandomSeed(0x9E3779B97F4A7C15U * (index + 1));
    slot.setWaveform(dsp::Waveform::saw);
    slot.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
    slot.noteOn();
    Buffer out{2, length};
    for (std::size_t start = 0; start < length; start += 512)
    {
        slot.process(out.block().subBlock(start, std::min<std::size_t>(512, length - start)), 45.0f);
    }
    return out;
}

double thdOf(std::span<const float> x, double hz)
{
    static const auto window = blackmanHarrisWindow(16384);
    return measureThd(x.subspan(x.size() - 16384), hz, rate, window);
}

} // namespace

TEST_CASE("Voice: matches the stage-1 prototype's decided configuration bit for bit", "[voice]")
{
    // The prototype computes the decided equations with the same classes and
    // the same seeds: out = env_X . F_X(own + env_other . other) for AB, and
    // env_X . F_X(own) for its own part, nothing for "--". Same arithmetic in
    // the same order, so the outputs must agree to the bit.
    const std::array routings{
        Routing{.a = FilterInput::own, .b = FilterInput::own, .drive = 4.0f},
        Routing{.a = FilterInput::both, .b = FilterInput::none, .drive = 4.0f},
        Routing{.a = FilterInput::own, .b = FilterInput::both, .drive = 2.0f},
        Routing{.a = FilterInput::both, .b = FilterInput::both, .drive = 4.0f},
        Routing{.a = FilterInput::none, .b = FilterInput::own, .drive = 1.0f},
        Routing{.a = FilterInput::own, .b = FilterInput::both, .drive = 1.0f, .bypassB = true},
    };
    const auto length = samples(0.25);
    const auto noteOff = samples(0.15) + 7;
    // A knob steps mid-note (off the grid), so each grid point's feed is read
    // from a moving ramp.
    const Knobs knobs{
        .cutoff = [](std::size_t at) { return at < 3005 ? 5.0f : 2.5f; },
        .resonance = [](std::size_t at) { return at < 5000 ? 1.0f : 4.0f; },
    };
    for (const auto& r : routings)
    {
        CAPTURE(static_cast<int>(r.a), static_cast<int>(r.b), r.bypassB);
        const auto lib = renderVoice(
            [&r](Voice& v)
            {
                worstOscillators(v);
                route(v, r);
            },
            length, noteOff, 37, noActions(), knobs);
        const auto proto = renderPrototype(
            [&r](Prototype& v)
            {
                worstOscillators(v);
                route(v, r);
            },
            length, noteOff, 37, knobs);
        CHECK(sameBits(lib.out, proto));
        CHECK_FALSE(allZero(lib.out.channel(0)));
    }
}

TEST_CASE("Voice: '--' on both filters is silence, and nothing runs", "[voice]")
{
    const auto r = renderVoice(
        [](Voice& v)
        {
            saws(v, {0, 1, 2, 3});
            route(v, {.a = FilterInput::none, .b = FilterInput::none});
        },
        samples(0.1), samples(1.0), 512);
    CHECK(allZero(r.out.channel(0)));
    CHECK(allZero(r.out.channel(1)));
}

TEST_CASE("Voice: a part no filter selects is silent and not rendered", "[voice]")
{
    // Filter A on A, filter B on "--", only B1 sounding: nothing reads part B.
    Voice v{d6::minBlepTable64()};
    saws(v, {2});
    route(v, {.a = FilterInput::own, .b = FilterInput::none});
    v.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
    const Knobs knobs;
    const KnobRamps a{knobs, rate};
    const auto k = knobsOf(a, a);
    v.noteOn(45.0f, k);
    Buffer out{2, 4096};
    for (std::size_t start = 0; start < 4096; start += 512)
    {
        v.process(out.block().subBlock(start, 512), k);
    }
    CHECK(allZero(out.channel(0)));
    CHECK(allZero(out.channel(1)));
    CHECK_FALSE(v.slot(2).isVoiceRunning(0)); // never rendered
    CHECK(v.filter(1).isAtRest());
}

TEST_CASE("Voice: a part crossing into the other filter goes through both envelopes", "[voice]")
{
    // Filter A on AB, filter B on "--", only B1 sounding. Each envelope in
    // turn decays to 0 in 100 ms while the other is a gate: either one closing
    // silences B1 (the developer: "through amp env B, through filter A
    // through amp env A"). The filter's tail at 4,677 Hz, k 0, falls below
    // FLT_MIN (where the filter flushes it) within a few ms, so from 200 ms
    // the output is exactly 0.
    for (const std::size_t closing : {std::size_t{0}, std::size_t{1}})
    {
        CAPTURE(closing);
        const auto r = renderVoice(
            [closing](Voice& v)
            {
                saws(v, {2});
                v.envelope(closing).setDecay(0.1f);
                v.envelope(closing).setSustain(0.0f);
                route(v, {.a = FilterInput::both, .b = FilterInput::none});
            },
            samples(0.3), samples(1.0), 512);
        CHECK_FALSE(allZero(r.out.channel(0).first(samples(0.05))));
        CHECK(allZero(r.out.channel(0).subspan(samples(0.2))));
    }
}

TEST_CASE("Voice: the crossing part's envelope acts before the filter", "[voice]")
{
    // B1 a sine at 220 Hz into filter A (AB) at the Filter Control's
    // defaults (LP24, 4,677 Hz: a transistor ladder's stage saturators
    // largely cancel far below the cutoff, so the cutoff stays near the
    // signal) with drive x2.83, so its peak at the saturators is about 2. Envelope B held at a sustain of 0.25 against
    // 1: before the filter, the saturators see a quarter of the level and the
    // THD falls (tanh's cubic term scales as a^2: about 1/16 at small levels,
    // less at this one); after the filter, the output would only scale and
    // the THD ratio would be 1. Bound: below 1/2.
    const auto thdAt = [](float sustain)
    {
        const auto r = renderVoice(
            [sustain](Voice& v)
            {
                saws(v, {2}, dsp::Waveform::sine);
                v.envelope(1).setSustain(sustain);
                v.drift().setDepthCents(0.0f);
                route(v, {.a = FilterInput::both, .b = FilterInput::none, .drive = 2.83f});
                v.setFilterResponse(0, dsp::FilterResponse::lp24);
            },
            samples(0.2) + 16384, samples(10.0), 512, noActions(), defaultKnobs(), 0.0f, 57.0f);
        return thdOf(r.out.channel(0), 220.0);
    };
    const double full = thdAt(1.0f);
    const double quarter = thdAt(0.25f);
    CAPTURE(full, quarter);
    CHECK(full > 0.01);
    CHECK(quarter < 0.5 * full);
}

TEST_CASE("Voice: AB is a plain sum", "[voice]")
{
    // A1 and B1 the same saw (Retrig on, Phase 0, drift off) into filter A on
    // AB, bypassed; both envelopes gates at exactly 1. Then the output is
    // exactly twice A1's alone: 1 . (x + 1 . x) = 2x in float.
    const auto run = [](std::initializer_list<std::size_t> on)
    {
        return renderVoice(
            [on](Voice& v)
            {
                saws(v, on);
                v.drift().setDepthCents(0.0f);
                route(v, {.a = FilterInput::both, .b = FilterInput::none});
                v.setFilterBypassed(0, true);
            },
            samples(0.05), samples(1.0), 512);
    };
    const auto both = run({0, 2});
    const auto one = run({0});
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < both.out.channel(c).size(); ++i)
        {
            REQUIRE_THAT(both.out.channel(c)[i], WithinULP(2.0f * one.out.channel(c)[i], 0));
        }
    }
    CHECK_FALSE(allZero(one.out.channel(0)));
}

TEST_CASE("Voice: a bypassed filter passes its Input Select's sum, unfiltered", "[voice]")
{
    // Only B1 sounding, drift off, envelopes gates at 1. Filter B bypassed:
    // on "--" it passes nothing; on B, exactly the bare slot's output.
    const auto run = [](FilterInput b)
    {
        return renderVoice(
            [b](Voice& v)
            {
                saws(v, {2});
                v.drift().setDepthCents(0.0f);
                route(v, {.a = FilterInput::own, .b = b, .bypassB = true});
            },
            samples(0.05), samples(1.0), 512);
    };
    const auto none = run(FilterInput::none);
    CHECK(allZero(none.out.channel(0)));
    CHECK(allZero(none.out.channel(1)));
    const auto own = run(FilterInput::own);
    const auto bare = bareSlot(2, samples(0.05));
    CHECK(sameBits(own.out, bare));
}

TEST_CASE("Voice: the filter input level moves the knee, not the clean level", "[voice]")
{
    SECTION("Bypassed: the level comes back within its roundings")
    {
        // g x, then the filter's identity, then (1/g) e y with e = 1: three
        // roundings of half an ulp each on the way (g x, 1/g, the product),
        // so within 2 ulps; and NaN, 0, negative, and infinite levels count
        // as 1, bit for bit.
        const auto run = [](float level)
        {
            return renderVoice(
                [level](Voice& v)
                {
                    saws(v, {0});
                    v.setFilterBypassed(0, true);
                    v.setFilterInputLevel(level);
                },
                samples(0.05), samples(1.0), 512);
        };
        const auto unity = run(1.0f);
        const auto scaled = run(0.3f);
        for (std::size_t i = 0; i < unity.out.channel(0).size(); ++i)
        {
            REQUIRE_THAT(scaled.out.channel(0)[i], WithinULP(unity.out.channel(0)[i], 2));
        }
        for (const float bad :
             {std::numeric_limits<float>::quiet_NaN(), 0.0f, -1.0f, std::numeric_limits<float>::infinity()})
        {
            CAPTURE(bad);
            CHECK(sameBits(run(bad).out, unity.out));
        }
    }
    SECTION("Driven: a lower level saturates less")
    {
        // A sine at 220 Hz through filter A at the defaults (4,677 Hz) and
        // drive x2.83 (peak about 2 at the saturators): a level of 0.25 puts a quarter of that there, so
        // the THD falls well below half (as for the envelope test above).
        const auto thdAt = [](float level)
        {
            const auto r = renderVoice(
                [level](Voice& v)
                {
                    saws(v, {0}, dsp::Waveform::sine);
                    v.drift().setDepthCents(0.0f);
                    route(v, {.drive = 2.83f});
                    v.setFilterInputLevel(level);
                },
                samples(0.2) + 16384, samples(10.0), 512, noActions(), defaultKnobs(), 0.0f, 57.0f);
            return thdOf(r.out.channel(0), 220.0);
        };
        const double unity = thdAt(1.0f);
        const double quarter = thdAt(0.25f);
        CAPTURE(unity, quarter);
        CHECK(unity > 0.01);
        CHECK(quarter < 0.5 * unity);
    }
}

TEST_CASE("Voice: the same output at host blocks of 1, 37 and 4096, with no allocation", "[voice]")
{
    // The note-off falls off the grid and off every block boundary; a knob
    // moves mid-note (ramped by the stand-in parameter layer).
    const Knobs knobs{.cutoff = [](std::size_t at) { return at < 3000 ? 5.0f : 3.0f; }};
    for (const auto& r : {
             Routing{.a = FilterInput::both, .b = FilterInput::both, .drive = 4.0f},
             Routing{.a = FilterInput::own, .b = FilterInput::none, .drive = 2.0f},
         })
    {
        const auto run = [&r, &knobs](std::size_t hostBlock)
        {
            return renderVoice(
                [&r](Voice& v)
                {
                    worstOscillators(v);
                    route(v, r);
                },
                samples(0.2), samples(0.12) + 7, hostBlock, noActions(), knobs, 5.0f);
        };
        const auto reference = run(4096);
        CHECK(reference.allocations == 0);
        CHECK_FALSE(allZero(reference.out.channel(0)));
        for (const std::size_t hostBlock : {std::size_t{1}, std::size_t{37}})
        {
            CAPTURE(hostBlock);
            const auto other = run(hostBlock);
            CHECK(sameBits(other.out, reference.out));
            CHECK(other.allocations == 0);
        }
    }
}

TEST_CASE("Voice: routing and bypass changes apply at the voice's next grid point", "[voice]")
{
    // Part B is always heard (filter A on AB), so its oscillators run the
    // same way in every render below; only filter B's routing changes.
    const auto length = samples(0.15);
    const auto base = [](FilterInput b, bool bypassB)
    {
        return [b, bypassB](Voice& v)
        {
            worstOscillators(v);
            route(v, {.a = FilterInput::both, .b = b, .drive = 2.0f, .bypassB = bypassB});
        };
    };
    const auto setInput = [](FilterInput b)
    {
        return [b](Voice& v)
        {
            v.setFilterInput(1, b);
        };
    };
    const auto setBypass = [](bool on)
    {
        return [on](Voice& v)
        {
            v.setFilterBypassed(1, on);
        };
    };

    SECTION("A change between grid points is the same as one at the next grid point")
    {
        // The grid counts from the note-on at 0: 2005 falls between 1984 and 2016.
        const auto late = renderVoice(base(FilterInput::own, false), length, length, 512,
                                      {{.at = 2005, .act = setInput(FilterInput::none)}});
        const auto onGrid = renderVoice(base(FilterInput::own, false), length, length, 512,
                                        {{.at = 2016, .act = setInput(FilterInput::none)}});
        const auto never = renderVoice(base(FilterInput::own, false), length, length, 512);
        CHECK(sameBits(late.out, onGrid.out));
        CHECK(sameBits(std::span{late.out.channel(0)}.first(2016), std::span{never.out.channel(0)}.first(2016)));
        CHECK_FALSE(sameBits(late.out, never.out));
    }
    SECTION("A filter switched back on from '--' starts from rest")
    {
        // On, off at 2016, on at 4000 (applied at 4000, a grid point) against
        // off from the start and on at 4000: from 4000 the same, to the bit,
        // only if the filter restarts from rest rather than from its old state.
        const auto toggled = renderVoice(
            base(FilterInput::own, false), length, length, 512,
            {{.at = 2016, .act = setInput(FilterInput::none)}, {.at = 4000, .act = setInput(FilterInput::own)}});
        const auto fresh = renderVoice(base(FilterInput::none, false), length, length, 512,
                                       {{.at = 4000, .act = setInput(FilterInput::own)}});
        CHECK(sameBits(std::span{toggled.out.channel(0)}.subspan(4000), std::span{fresh.out.channel(0)}.subspan(4000)));
        CHECK(sameBits(std::span{toggled.out.channel(1)}.subspan(4000), std::span{fresh.out.channel(1)}.subspan(4000)));
    }
    SECTION("Leaving Bypass starts the filter from rest, on the voice's grid")
    {
        const auto toggled = renderVoice(base(FilterInput::own, false), length, length, 512,
                                         {{.at = 2005, .act = setBypass(true)}, {.at = 4000, .act = setBypass(false)}});
        const auto fresh =
            renderVoice(base(FilterInput::own, true), length, length, 512, {{.at = 4000, .act = setBypass(false)}});
        CHECK(sameBits(std::span{toggled.out.channel(0)}.subspan(4000), std::span{fresh.out.channel(0)}.subspan(4000)));
        const auto onGrid = renderVoice(base(FilterInput::own, false), length, length, 512,
                                        {{.at = 2016, .act = setBypass(true)}, {.at = 4000, .act = setBypass(false)}});
        CHECK(sameBits(toggled.out, onGrid.out));
    }
}

TEST_CASE("Voice: each filter is fed through the Filter Control at the voice's grid", "[voice]")
{
    // Cutoff A at 2, Cutoff B at 4, the Filter Control at 3, Keytrack 10 at
    // note 57 (4 octaves above A-1): S = 2 + 3 + 4 = 9 and 4 + 3 + 4 = 11
    // octaves, 4.5675 Hz x 2^9 and x 2^11 (under 0.495 fs) after the note-on
    // and every grid point. Tolerance as filter_control_test.cpp's for the law on the filter:
    // 2^-19 + 2^-24 relative.
    Voice v{d6::minBlepTable64()};
    saws(v, {0});
    v.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
    const Knobs knobsA{.cutoff = constant(2.0f), .controlCutoff = constant(3.0f)};
    const Knobs knobsB{.cutoff = constant(4.0f), .controlCutoff = constant(3.0f)};
    const KnobRamps a{knobsA, rate};
    const KnobRamps b{knobsB, rate};
    const auto k = knobsOf(a, b, 10.0f);
    REQUIRE_THAT(dsp::keytrackOctaves(57.0f, 1.0f), WithinULP(4.0f, 0));
    v.noteOn(57.0f, k);
    Buffer out{2, 512};
    const double tolerance = std::exp2(-19.0) + std::exp2(-24.0);
    const auto expected = [](double octaves)
    {
        return 4.5675 * std::exp2(octaves);
    };
    CHECK_THAT(v.filter(0).currentCutoff(), WithinRel(expected(9.0), tolerance));
    CHECK_THAT(v.filter(1).currentCutoff(), WithinRel(expected(11.0), tolerance));
    v.process(out.block(), k);
    CHECK_THAT(v.filter(0).currentCutoff(), WithinRel(expected(9.0), tolerance));
    CHECK_THAT(v.filter(1).currentCutoff(), WithinRel(expected(11.0), tolerance));
}

TEST_CASE("Voice: a NaN from the knobs leaves the filter where it was", "[voice]")
{
    // A NaN fed whole into the modulation inputs would mean the anchor (4.57
    // Hz) and k 0; the voice keeps the last finite cutoff and k instead. So a
    // voice whose knobs turn NaN plays on exactly as one whose knobs stay put.
    const auto play = [](bool nan)
    {
        Voice v{d6::minBlepTable64()};
        saws(v, {0});
        v.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
        dsp::LinearSmoother cutoff;
        dsp::LinearSmoother resonance;
        for (auto* s : {&cutoff, &resonance})
        {
            s->prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 1});
        }
        cutoff.setCurrentAndTarget(5.0f);
        resonance.setCurrentAndTarget(5.0f);
        const dsp::VoiceFilterKnobs k{
            .cutoff = {&cutoff, &cutoff},
            .resonance = {&resonance, &resonance},
            .controlCutoff = &cutoff,
            .controlResonance = nullptr,
            .keytrack = 0.0f,
        };
        v.noteOn(45.0f, k);
        Buffer out{2, 1024};
        v.process(out.block().subBlock(0, 512), k);
        const float before = v.filter(0).currentCutoff();
        if (nan)
        {
            cutoff.setCurrentAndTarget(std::numeric_limits<float>::quiet_NaN());
            resonance.setCurrentAndTarget(std::numeric_limits<float>::quiet_NaN());
        }
        v.process(out.block().subBlock(512, 512), k);
        CHECK_THAT(before, WithinRel(4.5675 * 1024.0, 1e-5));
        CHECK_THAT(v.filter(0).currentCutoff(), WithinULP(before, 0));
        return out;
    };
    const auto steady = play(false);
    const auto nan = play(true);
    CHECK(sameBits(nan, steady));
    CHECK_FALSE(allZero(nan.channel(0)));
}

TEST_CASE("Voice: the lifecycle", "[voice]")
{
    Voice v{d6::minBlepTable64(), 1234};
    worstOscillators(v);
    route(v, {.a = FilterInput::both, .b = FilterInput::own, .drive = 2.0f});
    v.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
    const Knobs knobs;
    const KnobRamps a{knobs, rate};
    const auto k = knobsOf(a, a);
    CHECK_FALSE(v.isActive());
    const auto play = [&]
    {
        Buffer out{2, samples(0.6)};
        v.noteOn(45.0f, k);
        CHECK(v.isActive());
        for (std::size_t start = 0; start < out.channel(0).size(); start += 512)
        {
            if (start == samples(0.2) / 512 * 512)
            {
                v.noteOff();
            }
            v.process(out.block().subBlock(start, 512), k);
        }
        return out;
    };
    const auto first = play();
    CHECK_FALSE(v.isActive()); // the 0.2 s release has ended
    v.reset();
    const auto again = play();
    CHECK(sameBits(first, again)); // reset() is as freshly prepared
    // Another seed: other free-running phases and drift.
    Voice w{d6::minBlepTable64(), 99};
    worstOscillators(w);
    route(w, {.a = FilterInput::both, .b = FilterInput::own, .drive = 2.0f});
    w.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
    w.noteOn(45.0f, k);
    Buffer out{2, 512};
    w.process(out.block(), k);
    CHECK_FALSE(sameBits(std::span{out.channel(0)}, std::span{first.channel(0)}.first(512)));
}
