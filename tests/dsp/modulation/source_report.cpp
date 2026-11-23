// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The sources' report (docs/dsp/modulation-sources.md, M1.5 task 3, stage
// 1): measures the candidates in source_candidates.hpp.
//
// 0. the prototype against the library: SourceVoice with no routes is
//    dsp::Voice to the bit under every candidate;
// 1. how sources reach the per-sample destinations: buffered, inline or
//    folded, the same to the bit at host blocks of 1, 37 and 4096;
// 2. running only what is used: a route that appears mid-note under each
//    late start, the click and the difference from an always-run source;
//    the cost of a replay; a free LFO at a steal;
// 3. the controllers' smoothing: the zipper of a slow 7-bit sweep into
//    Pitch at +1.00, the latency and click of a 0 -> 127 step, and whether
//    the output depends on the host's blocks;
// 4. keytrack and velocity over a segment: monotonic or not while a glide
//    and a bend move together, and held against per sample;
// 5. an amp envelope as a source on a part that is skipped or unheard;
// 6. each source's bound against its values over random scenarios at 44.1,
//    48 and 96 kHz and host blocks of 1, 37 and 4096, and four small bounds.
//
// Hidden from the default run; run it in an optimised build:
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[sources-report]"
//
// CPU: the "[sources]" benchmarks (tools/bench.sh "[sources]").

#include "source_candidates.hpp"
#include "source_support.hpp"

#include "../filters/filter_control_support.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/modulation/registry.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace anoesis::test::sources;
using anoesis::test::Buffer;
using Dest = anoesis::dsp::modulation::Destination;

namespace
{

constexpr float rate = 48000.0f;
/// The default sources: no routes but the scenario's own.
const SourceSettings defaults{};

std::string fixed(double x, int digits)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

std::size_t samples(double seconds, double sampleRate = rate)
{
    return static_cast<std::size_t>(std::lround(seconds * sampleRate));
}

std::string dbText(double db)
{
    if (!std::isfinite(db))
    {
        return db < 0.0 ? "exact 0" : "inf";
    }
    return fixed(db, 1);
}

std::string_view yesNo(bool b)
{
    return b ? "identical" : "differs";
}

/// The Rate knob's units for `hz` (lfoRateHz's inverse).
float rateUnits(double hz)
{
    return static_cast<float>(10.0 * std::log2(hz / 0.1) / std::log2(1000.0));
}

/// filters.md's click: energy above 1 kHz in a Hann 4096 centred 32 samples
/// after each of `at`, both channels, re an instant unit step's, in dB.
double clickDb(const Buffer& b, std::span<const std::size_t> at)
{
    namespace fc = anoesis::test::filter_control;
    static const double unit = []
    {
        std::vector<float> x(8192, 0.0f);
        std::fill(x.begin() + 4096, x.end(), 1.0f);
        return fc::energyAbove1k(x, 4096 + 32, rate);
    }();
    double e = 0.0;
    for (const auto t : at)
    {
        e += (fc::energyAbove1k(b.channel(0), t + 32, rate) + fc::energyAbove1k(b.channel(1), t + 32, rate)) /
             (2.0 * unit);
    }
    e /= static_cast<double>(at.size());
    return e > 0.0 ? 10.0 * std::log10(e) : -std::numeric_limits<double>::infinity();
}

double clickDb(const Buffer& b, std::size_t at)
{
    const std::array one{at};
    return clickDb(b, one);
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

double rmsOf(const Buffer& b, std::size_t from, std::size_t n)
{
    double sum = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = from; i < from + n; ++i)
        {
            sum += static_cast<double>(b.channel(c)[i]) * b.channel(c)[i];
        }
    }
    return std::sqrt(sum / static_cast<double>(2 * n));
}

double toDb(double ratio)
{
    return ratio > 0.0 ? 20.0 * std::log10(ratio) : -std::numeric_limits<double>::infinity();
}

/// A1 a sine at A3 (or the note played), filters bypassed: the pitch reads
/// cleanly.
Voicing sineVoicing(std::size_t polyphony = 1)
{
    Voicing v;
    v.waves[0] = dsp::Waveform::sine;
    v.bypass = true;
    v.release = {0.3f, 0.3f};
    v.polyphony = polyphony;
    return v;
}

// --- 0. The prototype against the library -----------------------------------------------

void printPrototype()
{
    std::cout << "\n## 0. The prototype against the library\n\n"
                 "SourceVoice with no routes against dsp::Voice, through VoiceAllocator (Polyphony 2, two steals, "
                 "releases of 50 ms), 0.5 s, with LFO 1 a free S&H and Mod Env 1 set (so the sources exist and "
                 "run under 'always'):\n\n"
                 "| Patch | candidate | host blocks of 512 | of 37 |\n|---|---|---|---|\n";
    const std::vector<SourceEvent> events{
        noteOn(0, 57), noteOn(5001, 61), noteOn(9013, 64), noteOff(15000, 57), noteOff(17000, 61), noteOff(19000, 64),
    };
    struct Patch
    {
        std::string_view name;
        Voicing voicing;
    };
    const std::array patches{
        Patch{.name = "init (A1 saw)", .voicing = Voicing{.release = {0.05f, 0.05f}, .polyphony = 2}},
        Patch{
            .name = "A1 and B1 saws",
            .voicing = Voicing{.voices = {1, 0, 1, 0}, .release = {0.05f, 0.05f}, .polyphony = 2},
        },
        Patch{
            .name = "4 x 8 unison, resonant",
            .voicing = Voicing{.voices = {8, 8, 8, 8}, .resonance = 6.0f, .release = {0.05f, 0.05f}, .polyphony = 2},
        },
    };
    struct Candidate
    {
        std::string_view name;
        Delivery delivery;
        RunPolicy run;
        AmpEnvelopeRun amp = AmpEnvelopeRun::always;
    };
    const std::array candidates{
        Candidate{.name = "folded, used only", .delivery = Delivery::folded, .run = RunPolicy::usedOnly},
        Candidate{.name = "buffered, always", .delivery = Delivery::buffered, .run = RunPolicy::always},
        Candidate{.name = "inline, always", .delivery = Delivery::inlined, .run = RunPolicy::always},
        Candidate{
            .name = "unheard amp envelope only as a source",
            .delivery = Delivery::folded,
            .run = RunPolicy::usedOnly,
            .amp = AmpEnvelopeRun::heardOrUsed,
        },
    };
    for (const auto& p : patches)
    {
        for (const auto& c : candidates)
        {
            SourceSettings s;
            s.delivery = c.delivery;
            s.run = c.run;
            s.ampEnvelopes = c.amp;
            s.lfos[0] = {.waveform = dsp::LfoWaveform::sampleHold, .free = true};
            s.modEnvelopes[0] = {.attack = 0.01f, .sustain = 0.5f};
            std::cout << "| " << p.name << " | " << c.name << " |";
            for (const std::size_t block : {std::size_t{512}, std::size_t{37}})
            {
                std::size_t allocations = 0;
                const auto r = render(s, p.voicing, events, 24000, {.blockSize = block, .allocations = &allocations});
                std::cout << ' ' << yesNo(identical(r.out, renderLibrary(p.voicing, events, 24000, block)))
                          << (allocations == 0 ? "" : " (allocates)") << " |";
            }
            std::cout << '\n';
        }
    }
}

// --- 1. Delivery ----------------------------------------------------------------------------

std::vector<SourceEvent> busyEvents()
{
    std::vector<SourceEvent> e{
        route(0, 0, Source::modEnv1, Dest::pitchA, 0.1f),    route(0, 1, Source::modEnv1, Dest::mixA, 5.0f),
        route(0, 2, Source::modEnv2, Dest::cutoffA, 3.0f),   route(0, 4, Source::lfo1, Dest::pitchAB, 0.05f),
        route(0, 5, Source::lfo1, Dest::lfo2Gain, 5.0f),     route(0, 6, Source::lfo2, Dest::mixB, 4.0f),
        route(0, 8, Source::velocity, Dest::mixAB, 3.0f),    route(0, 9, Source::keytrack, Dest::pitchB, -0.5f),
        route(0, 10, Source::modWheel, Dest::pitchA, 0.2f),  route(0, 11, Source::aftertouch, Dest::mixA, -3.0f),
        route(0, 12, Source::ampEnvB, Dest::pitchA, 0.1f),   route(0, 13, Source::stepVelocity, Dest::mixB, 2.0f),
        route(0, 14, Source::ampEnvA, Dest::lfo1Rate, 2.0f),
    };
    for (const auto& ev : {
             noteOn(0, 57, 100),
             bend(1203, 12000),
             wheel(2001, 40),
             noteOn(5001, 61, 60),
             pressure(6117, 90),
             step(7000, 0.5f),
             noteOn(9013, 64, 127),
             bend(9500, 2000),
             wheel(10007, 127),
             noteOff(15000, 57),
             pressure(15500, 0),
             noteOff(17000, 61),
             noteOn(18000, 50, 30),
             noteOff(19000, 64),
             noteOff(23000, 50),
         })
    {
        e.push_back(ev);
    }
    return e;
}

SourceSettings busySettings()
{
    SourceSettings s;
    s.modEnvelopes[0] = {.attack = 0.02f, .decay = 0.1f, .sustain = 0.4f, .release = 0.05f};
    s.modEnvelopes[1] = {.attack = 0.005f, .decay = 0.3f, .sustain = 0.0f, .release = 0.02f};
    s.lfos[0] = {.waveform = dsp::LfoWaveform::triangle, .rate = 6.0f, .gain = 8.0f, .offset = 2.0f};
    s.lfos[1] = {.waveform = dsp::LfoWaveform::sampleHold, .rate = 5.5f, .gain = 4.0f};
    return s;
}

const Voicing busyVoicing{
    .voices = {1, 0, 1, 0},
    .release = {0.05f, 0.08f},
    .polyphony = 2,
    .portamento = 0.03f,
    .slide = true,
};

void printDelivery()
{
    std::cout
        << "\n## 1. How sources reach the per-sample destinations\n\n"
           "A busy patch: 13 routes reading every source (Pitch A/B/AB, Mix A/B/AB, Cutoff A, LFO 1 Rate, "
           "LFO 2 Gain), Polyphony 2 with steals, Slide glides, the bend, the wheel, pressure and Step "
           "Velocity moving, 0.54 s. Against folded at host blocks of 4096:\n\n"
           "| Delivery | sources run | host blocks of 1 | 37 | 4096 | allocations |\n|---|---|---|---|---|---:|\n";
    const auto expected = render(busySettings(), busyVoicing, busyEvents(), 26000, {.blockSize = 4096});
    for (const auto& [d, dn] : {
             std::pair{Delivery::buffered, "(a) buffered"},
             std::pair{Delivery::inlined, "(b) inline"},
             std::pair{Delivery::folded, "(c) folded"},
         })
    {
        for (const auto& [run, rn] :
             {std::pair{RunPolicy::always, "always"}, std::pair{RunPolicy::usedOnly, "used only"}})
        {
            std::cout << "| " << dn << " | " << rn << " |";
            std::size_t allocations = 0;
            for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
            {
                auto s = busySettings();
                s.delivery = d;
                s.run = run;
                const auto r =
                    render(s, busyVoicing, busyEvents(), 26000, {.blockSize = block, .allocations = &allocations});
                std::cout << ' ' << yesNo(identical(r.out, expected.out)) << " |";
            }
            std::cout << ' ' << allocations << " |\n";
        }
    }
}

// --- 2. Running only what is used --------------------------------------------------------

void printLateStart()
{
    std::cout << "\n## 2. A route that appears mid-note\n\n"
                 "Polyphony 1, A1 a sine at A2, filters bypassed (so the click measure reads only transients), amp A "
                 "sustain 1, release 0.3 s. The route "
                 "appears at sample 24,013 (0.5 s, mid-segment); the voice takes it at its next grid point. Mod "
                 "Env 1: A 0.2 s, D 0.3 s, S 0.5, R 0.3 s. LFO 1: a sine at 3 Hz, Gain 10. *Click*: filters.md's "
                 "measure at the route's sample. *Differs*: the RMS of the difference from 'always' over the "
                 "0.5 s after, dB re that render's RMS.\n\n"
                 "| Route | when | always (the reference) | next note: click / differs | next grid point | "
                 "reconstructed |\n|---|---|---|---|---|---|\n";
    struct Case
    {
        std::string_view name;
        std::string_view when;
        Source source;
        Dest destination;
        float amount;
        bool free = false;
        bool released = false;
    };
    const std::array cases{
        Case{
            .name = "Mod Env 1 to Mix A, -5",
            .when = "held, its sustain",
            .source = Source::modEnv1,
            .destination = Dest::mixA,
            .amount = -5.0f,
        },
        Case{
            .name = "Mod Env 1 to Mix A, -5",
            .when = "in its attack (key 0.4 s in)",
            .source = Source::modEnv1,
            .destination = Dest::mixA,
            .amount = -5.0f,
        },
        Case{
            .name = "Mod Env 1 to Mix A, -5",
            .when = "released 0.1 s before",
            .source = Source::modEnv1,
            .destination = Dest::mixA,
            .amount = -5.0f,
            .free = false,
            .released = true,
        },
        Case{
            .name = "LFO 1 to Pitch A, +0.05",
            .when = "held",
            .source = Source::lfo1,
            .destination = Dest::pitchA,
            .amount = 0.05f,
        },
        Case{
            .name = "LFO 1 (free) to Pitch A, +0.05",
            .when = "held",
            .source = Source::lfo1,
            .destination = Dest::pitchA,
            .amount = 0.05f,
            .free = true,
        },
    };
    const std::size_t at = 24013;
    const std::size_t length = samples(1.2);
    for (std::size_t ci = 0; ci < cases.size(); ++ci)
    {
        const auto& c = cases[ci];
        const std::size_t keyAt = ci == 1 ? at - samples(0.1) : 0;
        std::vector<SourceEvent> events{noteOn(keyAt, 45), route(at, 0, c.source, c.destination, c.amount)};
        events.push_back(noteOff(c.released ? at - samples(0.1) : samples(1.1), 45));
        const Voicing v = sineVoicing();
        SourceSettings s;
        s.modEnvelopes[0] = {.attack = 0.2f, .decay = 0.3f, .sustain = 0.5f, .release = 0.3f};
        s.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = rateUnits(3.0), .gain = 10.0f, .free = c.free};
        s.run = RunPolicy::always;
        const auto always = render(s, v, events, length);
        const double reference = rmsOf(always.out, at, samples(0.5));
        std::cout << "| " << c.name << " | " << c.when << " | " << dbText(clickDb(always.out, at)) << " |";
        for (const auto late : {LateStart::nextNote, LateStart::nextGrid, LateStart::reconstruct})
        {
            s.run = RunPolicy::usedOnly;
            s.late = late;
            const auto r = render(s, v, events, length, {.blockSize = 37});
            const auto d = difference(r.out, always.out);
            const double differs = rmsOf(d, at, samples(0.5));
            std::cout << ' ' << dbText(clickDb(r.out, at)) << " / "
                      << (differs > 0.0 ? dbText(toDb(differs / reference)) : "exact") << " |";
        }
        std::cout << '\n';
    }

    // The replay's cost: an envelope and an LFO reconstructed after 10 s.
    const ProcessSpec spec{.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 1};
    std::array<float, 32> scratch{};
    const auto time = [&](auto&& f)
    {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::micro>(t1 - t0).count();
    };
    dsp::Adsr e;
    e.prepare(spec);
    e.setAttack(0.2f);
    e.setDecay(0.3f);
    e.setSustain(0.5f);
    double envelopeUs = 1e30;
    double lfoUs = 1e30;
    float sink = 0.0f;
    for (int k = 0; k < 5; ++k)
    {
        envelopeUs = std::min(envelopeUs, time(
                                              [&]
                                              {
                                                  e.reset();
                                                  e.gateOn();
                                                  for (std::size_t n = 0; n < 480000; n += 32)
                                                  {
                                                      e.process(scratch);
                                                  }
                                                  sink += scratch[31];
                                              }));
        dsp::Lfo l;
        l.prepare(spec);
        l.setFrequency(3.0f);
        lfoUs = std::min(lfoUs, time(
                                    [&]
                                    {
                                        l.restart();
                                        for (std::size_t n = 0; n < 480000; n += 32)
                                        {
                                            l.process(scratch);
                                        }
                                        sink += scratch[31];
                                    }));
    }
    std::cout << "\nThe replay's cost, a note 10 s old (480,000 samples in 32-sample pieces), the best of 5: an "
                 "envelope "
              << fixed(envelopeUs, 0) << " us, a sine LFO " << fixed(lfoUs, 0)
              << " us (a 32-sample segment lasts 667 us at 48 kHz).";
    std::cout << (sink > 1e30f ? " " : "") << '\n';

    // A free LFO at a steal, its copy moved off the global by a keytracked rate.
    std::cout << "\n### A free LFO at a steal\n\n"
                 "Polyphony 1, A1 a sine, filters bypassed; LFO 1 free, a sine at 5 Hz on Pitch A at +0.05, "
                 "KeyTrack on LFO 1 Rate at +5 (so a voice's copy runs off the global one); A1 at 0, a steal to A3 "
                 "at 1.0 s (sample 48,000, a retrigger in place). The steal's click with the LFO's route and "
                 "without it (the note change alone), and how far the two policies' renders differ after it (RMS "
                 "of the difference over 0.5 s, re the render's):\n\n"
                 "| Free at a steal | click | without the LFO's route | differs from 'kept' |\n|---|---:|---:|---:|\n";
    Buffer kept{2, 1};
    for (const auto& [policy, name] : {
             std::pair{FreeCopy::fromSilence, "kept (copied only from silence)"},
             std::pair{FreeCopy::everyNoteOn, "copied again at every note-on"},
         })
    {
        SourceSettings s;
        s.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = rateUnits(5.0), .gain = 10.0f, .free = true};
        s.freeCopy = policy;
        const Voicing v = sineVoicing();
        const std::vector<SourceEvent> base{noteOn(0, 33), noteOn(48000, 57), noteOff(samples(1.5), 57)};
        auto events = base;
        events.push_back(route(0, 4, Source::lfo1, Dest::pitchA, 0.05f));
        events.push_back(route(0, 8, Source::keytrack, Dest::lfo1Rate, 5.0f));
        const auto with = render(s, v, events, samples(1.6));
        const auto without = render(s, v, base, samples(1.6));
        std::string differs = "(reference)";
        if (policy == FreeCopy::fromSilence)
        {
            kept = with.out;
        }
        else
        {
            const double d = rmsOf(difference(with.out, kept), 48000, samples(0.5));
            differs = d > 0.0 ? dbText(toDb(d / rmsOf(kept, 48000, samples(0.5)))) : "exact";
        }
        std::cout << "| " << name << " | " << dbText(clickDb(with.out, 48000)) << " | "
                  << dbText(clickDb(without.out, 48000)) << " | " << differs << " |\n";
    }
}

// --- 3. The controllers -------------------------------------------------------------------

struct ControllerCase
{
    std::string_view name;
    Smoothing smoothing;
    float ms;
};

constexpr std::array controllerCases{
    ControllerCase{.name = "step", .smoothing = Smoothing::step, .ms = 0.0f},
    ControllerCase{.name = "linear, 1 ms", .smoothing = Smoothing::linear, .ms = 1.0f},
    ControllerCase{.name = "linear, 2 ms", .smoothing = Smoothing::linear, .ms = 2.0f},
    ControllerCase{.name = "linear, 5 ms", .smoothing = Smoothing::linear, .ms = 5.0f},
    ControllerCase{.name = "linear, 10 ms", .smoothing = Smoothing::linear, .ms = 10.0f},
    ControllerCase{.name = "one-pole, 1 ms", .smoothing = Smoothing::onePole, .ms = 1.0f},
    ControllerCase{.name = "one-pole, 2 ms", .smoothing = Smoothing::onePole, .ms = 2.0f},
    ControllerCase{.name = "one-pole, 5 ms", .smoothing = Smoothing::onePole, .ms = 5.0f},
    ControllerCase{.name = "one-pole per host block, 5 ms", .smoothing = Smoothing::perBlock, .ms = 5.0f},
};

void printControllers()
{
    std::cout
        << "\n## 3. ModWheel and Aftertouch: smoothing 7-bit steps\n\n"
           "*Zipper*: A1 a sine at A3, filters bypassed, ModWheel on Pitch A at +1.00 (an octave over the "
           "wheel), swept 0 -> 127 one value every 20 ms (960 samples, from sample 4,813, off the voices' "
           "grid; each step 1/127 octave, 9.45 cents), or on Mix A at -10 (unity to silence): filters.md's "
           "measure (energy above 1 kHz in a Hann 4096, dB re a unit step; the sine stays below 440 Hz) "
           "averaged over 16 points 0.1 s apart from 0.5 s. *Latency*: from a 0 -> 127 message to 50% and "
           "90% of the way. *Click*: the same step at sample 24,013 on Pitch A at +1.00, and on Mix A at "
           "-10 (unity to silence), filters.md's measure. *Blocks*: the sweep at host blocks of 1, 37 and "
           "4096, to the bit.\n\n"
           "| Smoothing | zipper: Pitch | zipper: Mix | latency 50% / 90% (ms) | click: Pitch | click: Mix | blocks "
           "1 / 37 / 4096 |\n|---|---:|---:|---:|---:|---:|---|\n";
    const std::size_t start = 4813;
    const std::size_t length = start + (std::size_t{128} * 960) + 4800;
    std::vector<SourceEvent> sweep{noteOn(0, 57), route(0, 8, Source::modWheel, Dest::pitchA, 1.0f)};
    for (int v = 0; v < 128; ++v)
    {
        sweep.push_back(wheel(start + (static_cast<std::size_t>(v) * 960), v));
    }
    auto mixSweep = sweep;
    mixSweep[1] = route(0, 8, Source::modWheel, Dest::mixA, -10.0f);
    std::vector<std::size_t> points;
    points.reserve(16);
    for (std::size_t k = 0; k < 16; ++k)
    {
        points.push_back(samples(0.5 + (0.1 * static_cast<double>(k))));
    }
    for (const auto& c : controllerCases)
    {
        const RenderOptions o{.smoothing = c.smoothing, .smoothingMs = c.ms};
        const auto y = render(defaults, sineVoicing(), sweep, length, o);
        const auto ym = render(defaults, sineVoicing(), mixSweep, length, o);
        // Latency from the controller itself.
        Controller ctl;
        ctl.configure(c.smoothing, rate, c.ms);
        ctl.set(1000, 1.0f);
        std::size_t half = 0;
        std::size_t most = 0;
        for (std::size_t t = 1000; t < 100000 && most == 0; t += c.smoothing == Smoothing::perBlock ? 512 : 1)
        {
            if (c.smoothing == Smoothing::perBlock)
            {
                ctl.beginBlock(t, 512);
            }
            const float v = ctl.valueAt(t);
            half = half == 0 && v >= 0.5f ? t - 1000 + 1 : half;
            most = v >= 0.9f ? t - 1000 + 1 : 0;
        }
        // The step's clicks.
        std::array<double, 2> clicks{};
        for (std::size_t k = 0; k < 2; ++k)
        {
            const std::vector<SourceEvent> stepEvents{
                noteOn(0, 57),
                k == 0 ? route(0, 8, Source::modWheel, Dest::pitchA, 1.0f)
                       : route(0, 8, Source::modWheel, Dest::mixA, -10.0f),
                wheel(24013, 127),
            };
            clicks[k] = clickDb(render(defaults, sineVoicing(), stepEvents, samples(1.0), o).out, 24013);
        }
        bool same = true;
        for (const std::size_t block : {std::size_t{1}, std::size_t{37}})
        {
            RenderOptions ob = o;
            ob.blockSize = block;
            same = same && identical(render(defaults, sineVoicing(), sweep, samples(0.6), ob).out,
                                     render(defaults, sineVoicing(), sweep, samples(0.6),
                                            {.blockSize = 4096, .smoothing = c.smoothing, .smoothingMs = c.ms})
                                         .out);
        }
        std::cout << "| " << c.name << " | " << dbText(clickDb(y.out, points)) << " | "
                  << dbText(clickDb(ym.out, points)) << " | " << fixed(static_cast<double>(half) / 48.0, 2) << " / "
                  << fixed(static_cast<double>(most) / 48.0, 2) << " | " << dbText(clicks[0]) << " | "
                  << dbText(clicks[1]) << " | " << yesNo(same) << " |\n";
    }
}

// --- 4. KeyTrack and velocity over a segment ----------------------------------------------

std::vector<SourceEvent> randomScenario(dsp::Random& random, std::size_t length, std::size_t sampleRate)
{
    std::vector<SourceEvent> e{
        route(0, 0, Source::modEnv1, Dest::pitchA, 0.1f),    route(0, 1, Source::modEnv1, Dest::mixA, 5.0f),
        route(0, 2, Source::modEnv2, Dest::cutoffA, 3.0f),   route(0, 4, Source::lfo1, Dest::pitchAB, 0.05f),
        route(0, 5, Source::lfo1, Dest::lfo2Gain, 5.0f),     route(0, 6, Source::lfo2, Dest::mixB, 4.0f),
        route(0, 8, Source::velocity, Dest::mixAB, 3.0f),    route(0, 9, Source::keytrack, Dest::pitchB, -0.5f),
        route(0, 10, Source::modWheel, Dest::pitchA, 0.2f),  route(0, 11, Source::aftertouch, Dest::mixA, -3.0f),
        route(0, 12, Source::ampEnvB, Dest::pitchA, 0.1f),   route(0, 13, Source::stepVelocity, Dest::mixB, 2.0f),
        route(0, 14, Source::ampEnvA, Dest::lfo1Rate, 2.0f),
    };
    std::size_t t = 0;
    int key = 60;
    while (t < length)
    {
        const auto r = random.nextUint32();
        switch (r % 6)
        {
        case 0:
            key = 40 + static_cast<int>((r >> 8U) % 40);
            e.push_back(noteOn(t, key, 1 + static_cast<int>((r >> 16U) % 127)));
            e.push_back(bend(t + 1 + ((r >> 4U) % 200), (r & 1U) != 0 ? 0 : 16383));
            break;
        case 1:
            e.push_back(noteOff(t, key));
            break;
        case 2:
            e.push_back(bend(t, static_cast<int>((r >> 8U) % 16384)));
            break;
        case 3:
            e.push_back(wheel(t, static_cast<int>((r >> 8U) % 128)));
            break;
        case 4:
            e.push_back(pressure(t, static_cast<int>((r >> 8U) % 128)));
            break;
        default:
            e.push_back(step(t, random.nextUnipolar()));
            break;
        }
        t += 1 + ((r >> 20U) % (sampleRate / 20));
    }
    return e;
}

/// The bound check summed over 3 rates x 3 host block sizes, 2 s each.
BoundStats boundCheck(BoundRule rule, LinearRate linear = LinearRate::perSample)
{
    BoundStats total;
    dsp::Random random{0xB0C4D};
    for (const std::size_t sampleRate : {std::size_t{44100}, std::size_t{48000}, std::size_t{96000}})
    {
        for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
        {
            const std::size_t length = 2 * sampleRate;
            const auto events = randomScenario(random, length, sampleRate);
            auto s = busySettings();
            s.run = RunPolicy::always;
            s.checkBounds = true;
            s.bounds = rule;
            s.linearRate = linear;
            const auto out = render(s, busyVoicing, events, length,
                                    {.blockSize = block, .sampleRate = static_cast<float>(sampleRate)});
            addStats(total, out.stats);
        }
    }
    return total;
}

constexpr std::array<std::string_view, sourceCount> sourceNames{
    "-",         "Velocity",  "Mod Wheel", "KeyTrack", "Amp Env A",  "Amp Env B",
    "Mod Env 1", "Mod Env 2", "LFO 1",     "LFO 2",    "Aftertouch", "Step Velocity",
};

void printKeytrackAndVelocity(const BoundStats& tight, const BoundStats& ends)
{
    std::cout << "\n## 4. KeyTrack and velocity over a segment\n\n"
                 "### Monotonic within a segment?\n\n"
                 "From section 6's random scenarios (glides in Slide, a bend to an end 0-200 samples after each "
                 "note-on, legato and stolen notes): samples outside the bound taken at each grid point and call "
                 "start, for the segment's rest.\n\n"
                 "| Source | bound | samples | outside | worst excess |\n|---|---|---:|---:|---:|\n";
    for (const auto s : {Source::keytrack, Source::velocity})
    {
        const auto i = at(s);
        std::cout << "| " << sourceNames[i] << " | "
                  << (s == Source::keytrack ? "its parts' ends (the glide's plus the bend's)" : "its two ends") << " | "
                  << tight.checked[i] << " | " << tight.outside[i] << " | " << fixed(tight.worst[i], 6) << " |\n";
        if (s == Source::keytrack)
        {
            std::cout << "| | its own two ends | " << ends.checked[i] << " | " << ends.outside[i] << " | "
                      << fixed(ends.worst[i], 6) << " (" << fixed(ends.worst[i] * 120.0f * 100.0f, 2) << " cents) |\n";
        }
    }

    std::cout << "\n### Per sample or held at the grid point\n\n"
                 "*Added*: filters.md's click on the difference between held and per sample, averaged over four "
                 "points in the move, dB re a unit step. Velocity: Mono Legato, Portamento 0.2 s, A3 at velocity "
                 "127 then a legato A4 at 1 at 0.5 s (the velocity ramps over the glide), A1 a saw, Velocity on "
                 "Mix A at -10 (from unity). KeyTrack: A1 a sine, Polyphony 1, Slide 0.3 s, A2 then A4 at 0.5 s, "
                 "a bend to full down at 0.55 s (range 2), KeyTrack on Pitch A at +1.00.\n\n"
                 "| Source | per sample: its own click | held: added |\n|---|---:|---:|\n";
    struct Case
    {
        std::string_view name;
        Voicing voicing;
        std::vector<SourceEvent> events;
    };
    Voicing const legato{.release = {0.3f, 0.3f}, .polyphony = 1, .portamento = 0.2f, .monoLegato = true};
    Voicing glideSine = sineVoicing();
    glideSine.portamento = 0.3f;
    glideSine.slide = true;
    const std::array cases{
        Case{
            .name = "Velocity",
            .voicing = legato,
            .events = {noteOn(0, 57, 127), route(0, 8, Source::velocity, Dest::mixA, -10.0f), noteOn(24000, 69, 1)},
        },
        Case{
            .name = "KeyTrack",
            .voicing = glideSine,
            .events =
                {
                    noteOn(0, 45),
                    route(0, 8, Source::keytrack, Dest::pitchA, 1.0f),
                    noteOff(20000, 45),
                    noteOn(24000, 69),
                    bend(26400, 0),
                },
        },
    };
    const std::array points{samples(0.55), samples(0.6), samples(0.65), samples(0.7)};
    for (const auto& c : cases)
    {
        SourceSettings s;
        const auto perSample = render(s, c.voicing, c.events, samples(1.0));
        s.linearRate = LinearRate::held;
        const auto held = render(s, c.voicing, c.events, samples(1.0));
        std::cout << "| " << c.name << " | " << dbText(clickDb(perSample.out, points)) << " | "
                  << dbText(clickDb(difference(held.out, perSample.out), points)) << " |\n";
    }
}

// --- 5. An amp envelope as a source on a skipped part ----------------------------------------

void printSkippedPart()
{
    std::cout << "\n## 5. An amp envelope as a source on a part that is skipped\n\n"
                 "Part A: A1 a saw. Amp Env B: A 0.3 s, S 1, R 0.4 s, on Pitch A at +0.1 (or Mix A at "
                 "-5). Three notes (Polyphony 2, a steal), 1 s, host blocks of 37. *Skips on*: the voice as built "
                 "(task 5's plan); *nothing skipped*: every filter and slot run. *Only as a source*: an unheard "
                 "part's amp envelope run only while a route reads it.\n\n"
                 "| Part B | route | skips on = nothing skipped | only as a source = always | the route moves "
                 "A |\n|---|---|---|---|---|\n";
    struct Case
    {
        std::string_view name;
        Voicing voicing;
    };
    Voicing unheard{.release = {0.1f, 0.4f}, .polyphony = 2};
    unheard.inputs[1] = FilterInput::none;
    unheard.attack[1] = 0.3f;
    Voicing empty{.release = {0.1f, 0.4f}, .polyphony = 2};
    empty.attack[1] = 0.3f;
    Voicing crossing{.release = {0.1f, 0.4f}, .polyphony = 2};
    crossing.inputs = {FilterInput::both, FilterInput::none};
    crossing.attack[1] = 0.3f;
    const std::array cases{
        Case{.name = "unheard (filter B --, no oscillators)", .voicing = unheard},
        Case{.name = "heard, no oscillators (filter B at rest, skipped)", .voicing = empty},
        Case{.name = "filter A on AB, filter B -- (B crosses, no oscillators)", .voicing = crossing},
    };
    for (const auto& c : cases)
    {
        for (const auto& [d, dn] :
             {std::pair{Dest::pitchA, "Amp Env B to Pitch A, +0.1"}, std::pair{Dest::mixA, "Amp Env B to Mix A, -5"}})
        {
            const std::vector<SourceEvent> events{
                route(0, 8, Source::ampEnvB, d, d == Dest::pitchA ? 0.1f : -5.0f),
                noteOn(0, 57),
                noteOn(9001, 64),
                noteOn(20011, 60),
                noteOff(30000, 57),
                noteOff(33000, 64),
                noteOff(36000, 60),
            };
            SourceSettings s;
            const auto on = render(s, c.voicing, events, samples(1.0), {.blockSize = 37});
            s.neverSkip = true;
            const auto off = render(s, c.voicing, events, samples(1.0), {.blockSize = 37});
            s.neverSkip = false;
            s.ampEnvelopes = AmpEnvelopeRun::heardOrUsed;
            const auto lean = render(s, c.voicing, events, samples(1.0), {.blockSize = 37});
            const std::vector<SourceEvent> notesOnly(events.begin() + 1, events.end());
            const auto none = render(defaults, c.voicing, notesOnly, samples(1.0), {.blockSize = 37});
            std::cout << "| " << c.name << " | " << dn << " | " << yesNo(identical(on.out, off.out)) << " | "
                      << yesNo(identical(lean.out, on.out)) << " | " << (identical(on.out, none.out) ? "no" : "yes")
                      << " |\n";
        }
    }
}

// --- 6. Each source's bound ---------------------------------------------------------------

void printBounds(const BoundStats& tight)
{
    std::cout << "\n## 6. Each source's bound against its values\n\n"
                 "Random scenarios (notes with velocities 1-127, Slide glides, bends to either end and anywhere, "
                 "the wheel, pressure, Step Velocity, legato and stolen notes, events 1 sample to 50 ms apart), "
                 "13 routes reading every source, LFO 1 a triangle with its Rate from Amp Env A, LFO 2 an S&H with "
                 "its Gain from LFO 1; 2 s at each of 44.1, 48 and 96 kHz, at host blocks of 1, 37 and 4096 (9 "
                 "renders). Each source's bound is taken at each grid point and at the start of each process() "
                 "call (where the skip test may re-test), for the rest of the segment, and every sample's value is "
                 "checked against it. *Width*: the mean of hi - lo.\n\n"
                 "| Source | bound (task 2's kinds) | samples | outside | mean width |\n|---|---|---:|---:|---:|\n";
    for (std::size_t i = 1; i < sourceCount; ++i)
    {
        const auto kind = mod::sources[i].bound;
        std::string_view k = "its two ends";
        if (kind == mod::BoundKind::envelope)
        {
            k = "by stage";
        }
        else if (kind == mod::BoundKind::lfo)
        {
            k = "the segment rendered ahead";
        }
        else if (i == at(Source::keytrack))
        {
            k = "its parts' ends";
        }
        std::cout << "| " << sourceNames[i] << " | " << k << " | " << tight.checked[i] << " | " << tight.outside[i]
                  << " | "
                  << fixed(tight.width[i] / static_cast<double>(std::max<std::uint64_t>(1, tight.checked[i])), 4)
                  << " |\n";
    }
}

void printSmallBounds(const BoundStats& ends, const BoundStats& level, const BoundStats& lfoEnds)
{
    std::cout << "\n### Deliberately small bounds\n\n"
                 "The same 9 renders, samples outside the bound (worst excess):\n\n"
                 "| Source | keytrack from its own ends | every source its value at the start | an LFO from its two "
                 "ends |\n|---|---:|---:|---:|\n";
    for (std::size_t i = 1; i < sourceCount; ++i)
    {
        const auto cell = [&](const BoundStats& b)
        {
            return std::to_string(b.outside[i]) + (b.outside[i] > 0 ? " (" + fixed(b.worst[i], 4) + ")" : "");
        };
        std::cout << "| " << sourceNames[i] << " | " << cell(ends) << " | " << cell(level) << " | " << cell(lfoEnds)
                  << " |\n";
    }
}

} // namespace

TEST_CASE("Sources report", "[.][sources-report]")
{
    std::cout << "# Modulation sources: stage 1 report (M1.5 task 3)\n";
    printPrototype();
    printDelivery();
    printLateStart();
    printControllers();
    const auto tight = boundCheck(BoundRule::tight);
    const auto ends = boundCheck(BoundRule::keytrackEnds);
    printKeytrackAndVelocity(tight, ends);
    printSkippedPart();
    printBounds(tight);
    printSmallBounds(ends, boundCheck(BoundRule::levelOnly), boundCheck(BoundRule::lfoEnds));
    SUCCEED();
}
