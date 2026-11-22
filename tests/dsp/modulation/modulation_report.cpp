// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The modulation report (docs/dsp/modulation.md, M1.5 task 2, stage 1):
// measures the candidates in registry_candidates.hpp, source_bounds.hpp and
// modulation_candidates.hpp:
//
// - the prototype against the library: ModVoice with no routes is
//   dsp::Voice to the bit;
// - the registries: their sizes, and what Phase 2 edits in each;
// - the rate per destination (Pitch, Phase, Volume, Pan, LFO Rate, LFO
//   Gain): the zipper a 40 Hz LFO adds (filters.md's click measure on the
//   difference from per sample), the same output at host blocks of 1, 37
//   and 4096, and Phase's band-limiting (energy off the phase modulation's
//   lines);
// - the fast-sweep overshoot (filter-control.md, recommendation 8) by how
//   the cutoff's modulation is read;
// - order and feedback: an LFO cycle and a self-route in segment and
//   sample order, and the delay through each;
// - a per-voice source on a global destination: newest, oldest held, sum,
//   or a global copy, on a stand-in global gain;
// - each source's bound against its values over random segments, and how
//   much each bound lets the skip test skip.
//
// Hidden from the default run; run it in an optimised build:
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[modulation-report]"
//
// CPU: the "[modulation]" benchmarks (tools/bench.sh "[modulation]").

#include "modulation_candidates.hpp"
#include "registry_candidates.hpp"
#include "source_bounds.hpp"

#include "../filters/filter_control_support.hpp"
#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/random.hpp>
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
#include <vector>

using namespace anoesis::test::modulation;
using anoesis::test::Buffer;

namespace
{

constexpr float rate = 48000.0f;
const dsp::ProcessSpec spec{.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 2};

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

const dsp::MinBlepTable& table()
{
    return anoesis::test::d6::minBlepTable64();
}

/// The Rate knob's units for `hz` (lfoRateHz's inverse).
float rateUnits(double hz)
{
    return static_cast<float>(10.0 * std::log2(hz / 0.1) / std::log2(1000.0));
}

// --- Rendering through the engine ------------------------------------------------------

struct Voicing
{
    std::array<dsp::Waveform, 4> waves{dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw};
    std::array<int, 4> voices{1, 0, 0, 0};
    std::array<float, 4> volume{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> pan{};
    std::array<int, 4> semitones{0, 0, 7, 0};
    bool bypass = false;
    float cutoff = 5.0f;
    float controlCutoff = 5.0f;
    float resonance = 0.0f;
    float controlResonance = 0.0f;
    bool warm = true; ///< LadderFilter's default
    std::array<float, 2> release{0.0f, 0.0f};
    std::size_t polyphony = 16;
};

void configure(ModEngine::Voice& v, const Voicing& s)
{
    for (std::size_t i = 0; i < 4; ++i)
    {
        v.slot(i).setWaveform(s.waves[i]);
        v.slot(i).setVoices(s.voices[i]);
        v.slot(i).setVolume(s.volume[i]);
        v.slot(i).setPan(s.pan[i]);
        v.slot(i).setTuning({.semitones = s.semitones[i]});
    }
    v.drift().setDepthCents(0.0f);
    for (std::size_t p = 0; p < 2; ++p)
    {
        v.setFilterBypassed(p, s.bypass);
        v.envelope(p).setRelease(s.release[p]);
    }
    v.setWarm(s.warm);
}

EngineEvent noteOn(std::size_t at, int key, int velocity = 127)
{
    return {
        .offset = at,
        .kind = EngineEvent::Kind::voice,
        .voice = {.kind = dsp::VoiceEventKind::noteOn, .value = key, .velocity = velocity},
    };
}

EngineEvent noteOff(std::size_t at, int key)
{
    return {
        .offset = at,
        .kind = EngineEvent::Kind::voice,
        .voice = {.kind = dsp::VoiceEventKind::noteOff, .value = key},
    };
}

struct RenderOptions
{
    std::size_t blockSize = 512;
    float sampleRate = rate;
    std::size_t* allocations = nullptr;
    std::function<void(ModEngine&)> afterBlock;
};

Buffer render(const ModPatch& patch, const Voicing& voicing, std::vector<EngineEvent> events, std::size_t length,
              const RenderOptions& options = {})
{
    std::ranges::stable_sort(events, {}, &EngineEvent::offset);
    auto engine = std::make_unique<ModEngine>(table(), patch, 3);
    engine->voices().forEachVoice([&](auto& v) { configure(v, voicing); });
    engine->setCutoff(voicing.cutoff, voicing.controlCutoff);
    engine->setResonance(voicing.resonance, voicing.controlResonance);
    engine->prepare({.sampleRate = options.sampleRate, .maxBlockSize = 4096, .numChannels = 2});
    engine->voices().setPolyphony(voicing.polyphony);
    Buffer out{2, length};
    std::size_t e = 0;
    std::vector<EngineEvent> here;
    here.reserve(events.size());
    for (std::size_t start = 0; start < length; start += options.blockSize)
    {
        const auto n = std::min(options.blockSize, length - start);
        here.clear();
        while (e < events.size() && events[e].offset < start + n)
        {
            auto ev = events[e];
            ev.offset -= start;
            here.push_back(ev);
            ++e;
        }
        const anoesis::test::AllocationCounter counter;
        engine->process(out.block().subBlock(start, n), here);
        if (options.allocations != nullptr)
        {
            *options.allocations += counter.count();
        }
        if (options.afterBlock)
        {
            options.afterBlock(*engine);
        }
    }
    return out;
}

/// Energy above 1 kHz in a Hann 4096 centred 32 samples after `at`, both
/// channels, as a fraction of an instant unit step's (filters.md's click).
double clickEnergy(std::span<const float> left, std::span<const float> right, std::size_t at)
{
    namespace fc = anoesis::test::filter_control;
    static const double step = []
    {
        std::vector<float> x(8192, 0.0f);
        std::fill(x.begin() + 4096, x.end(), 1.0f);
        return fc::energyAbove1k(x, 4096 + 32, rate);
    }();
    return (fc::energyAbove1k(left, at + 32, rate) + fc::energyAbove1k(right, at + 32, rate)) / (2.0 * step);
}

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

double peakOf(const Buffer& b, std::size_t from)
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

// --- The prototype against the library -----------------------------------------------------

void printPrototype()
{
    std::cout << "\n## The prototype against the library\n\n"
                 "ModVoice with no routes against dsp::Voice, through VoiceAllocator (Polyphony 2, two steals, "
                 "releases of 50 ms), 0.5 s at host blocks of 512 and 37 (also checked in the default run):\n\n"
                 "| Patch | 512 | 37 |\n|---|---|---|\n";
    struct Case
    {
        std::string_view name;
        Voicing voicing;
    };
    const std::array cases{
        Case{
            .name = "init (A1 saw)",
            .voicing = Voicing{.release = {0.05f, 0.05f}, .polyphony = 2},
        },
        Case{
            .name = "A1 and B1 saws, own filters",
            .voicing = Voicing{.voices = {1, 0, 1, 0}, .release = {0.05f, 0.05f}, .polyphony = 2},
        },
        Case{
            .name = "4 x 8 unison saws, resonant",
            .voicing = Voicing{.voices = {8, 8, 8, 8}, .resonance = 6.0f, .release = {0.05f, 0.05f}, .polyphony = 2},
        },
    };
    const std::vector<EngineEvent> events{
        noteOn(0, 57), noteOn(5001, 61), noteOn(9013, 64), noteOff(15000, 57), noteOff(17000, 61), noteOff(19000, 64),
    };
    for (const auto& c : cases)
    {
        std::cout << "| " << c.name << " |";
        for (const std::size_t blockSize : {std::size_t{512}, std::size_t{37}})
        {
            // The library through the same allocator and knobs.
            auto library = std::make_unique<dsp::VoiceAllocator<dsp::Voice<>, 16>>(3, table());
            library->forEachVoice(
                [&](auto& v)
                {
                    for (std::size_t i = 0; i < 4; ++i)
                    {
                        v.slot(i).setWaveform(c.voicing.waves[i]);
                        v.slot(i).setVoices(c.voicing.voices[i]);
                        v.slot(i).setTuning({.semitones = c.voicing.semitones[i]});
                    }
                    v.drift().setDepthCents(0.0f);
                    v.envelope(0).setRelease(0.05f);
                    v.envelope(1).setRelease(0.05f);
                });
            library->prepare(spec);
            library->setPolyphony(2);
            std::array<dsp::LinearSmoother, 4> ramps{};
            for (auto& r : ramps)
            {
                r.setCurrentAndTarget(5.0f);
            }
            ramps[2].setCurrentAndTarget(c.voicing.resonance);
            ramps[3].setCurrentAndTarget(0.0f);
            dsp::VoiceKnobs knobs;
            knobs.cutoff = {ramps.data(), ramps.data()};
            knobs.controlCutoff = &ramps[1];
            knobs.resonance = {&ramps[2], &ramps[2]};
            knobs.controlResonance = &ramps[3];
            const std::size_t length = samples(0.5);
            Buffer a{2, length};
            std::size_t e = 0;
            for (std::size_t start = 0; start < length; start += blockSize)
            {
                const auto n = std::min(blockSize, length - start);
                std::vector<dsp::VoiceEvent> here;
                while (e < events.size() && events[e].offset < start + n)
                {
                    auto ev = events[e].voice;
                    ev.offset = events[e].offset - start;
                    here.push_back(ev);
                    ++e;
                }
                library->process(a.block().subBlock(start, n), here, knobs);
            }
            const ModPatch noRoutes;
            const auto b = render(noRoutes, c.voicing, events, length, {.blockSize = blockSize});
            std::cout << ' ' << (anoesis::test::bitIdentical(a, b) ? "identical" : "differs") << " |";
        }
        std::cout << '\n';
    }
}

// --- The registries ------------------------------------------------------------------------

void printRegistries()
{
    std::cout << "\n## The registries\n\nData each candidate keeps (bytes, arm64; the strings' characters are "
                 "extra):\n\n| | (a) table | (b) functions | (c) types |\n|---|---:|---:|---:|\n";
    std::size_t chars = 0;
    for (const auto& d : destinationRows)
    {
        chars += d.key.size() + d.name.size();
    }
    for (const auto& s : sourceRows)
    {
        chars += s.key.size() + s.name.size();
    }
    std::cout << "| a destination descriptor | " << sizeof(DestinationInfo) << " | "
              << sizeof(function_registry::DestinationEntry) << " + " << sizeof(DestinationInfo) << " | "
              << sizeof(DestinationInfo) << " |\n";
    std::cout << "| the destination table | " << sizeof(table_registry::destinations) << " | "
              << sizeof(function_registry::destinations) << " + " << sizeof(destinationRows) << " | "
              << sizeof(type_registry::Dispatch<type_registry::Destinations>::infos) << " + " << sizeof(destinationRows)
              << " |\n";
    std::cout << "| the source table | " << sizeof(table_registry::sources) << " | "
              << sizeof(function_registry::sources) << " + " << sizeof(sourceRows) << " | " << sizeof(sourceRows)
              << " |\n";
    std::cout << "| a compiled route | " << sizeof(table_registry::CompiledRoute) << " | "
              << sizeof(function_registry::CompiledRoute) << " | " << sizeof(type_registry::Compiled::Entry) << " |\n";
    std::cout << "| a voice's compiled routes | " << sizeof(table_registry::Compiled) << " | "
              << sizeof(function_registry::Compiled) << " | " << sizeof(type_registry::Compiled) << " |\n";
    std::cout << "\nKeys and labels: " << chars << " characters. The DSP's own fields of a destination "
              << "(target, parts, units, range, scope, rate) need " << (sizeof(Target) + 1 + 4 + 12 + 2)
              << " bytes; the strings are for presets and the interface.\n";
}

// --- The rate per destination ------------------------------------------------------------

struct RateCase
{
    std::string_view name;
    Target target;
    std::function<void(ModPatch&)> route;
    Voicing voicing;
    std::vector<EvalRate> rates;
};

std::vector<RateCase> rateCases()
{
    const float units40 = rateUnits(40.0);
    const Voicing sine{
        .waves = {dsp::Waveform::sine, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw},
        .bypass = true,
    };
    auto quiet = sine;
    quiet.volume = {0.5f, 1.0f, 1.0f, 1.0f};
    const auto lfo40 = [units40](ModPatch& p)
    {
        p.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = units40, .gain = 10.0f};
    };
    const std::vector<EvalRate> all{EvalRate::perSample, EvalRate::gridHeld, EvalRate::gridRamped};
    return {
        {
            .name = "Pitch A, +-0.25 octave",
            .target = Target::pitch,
            .route =
                [=](ModPatch& p)
            {
                lfo40(p);
                p.route(4, SourceId::lfo1, DestinationId::pitchA, 0.5f);
            },
            .voicing = sine,
            .rates = all,
        },
        {
            .name = "Phase A, +-18 degrees",
            .target = Target::phase,
            .route =
                [=](ModPatch& p)
            {
                lfo40(p);
                p.route(4, SourceId::lfo1, DestinationId::phaseA, 1.0f);
            },
            .voicing = sine,
            .rates = all,
        },
        {
            .name = "Volume A, 5 +- 5",
            .target = Target::volume,
            .route =
                [=](ModPatch& p)
            {
                lfo40(p);
                p.route(4, SourceId::lfo1, DestinationId::volumeA, 10.0f);
            },
            .voicing = quiet,
            .rates = all,
        },
        {
            .name = "Pan A, centre +- 5 (hard left to hard right)",
            .target = Target::pan,
            .route =
                [=](ModPatch& p)
            {
                lfo40(p);
                p.route(4, SourceId::lfo1, DestinationId::panA, 10.0f);
            },
            .voicing = sine,
            .rates = all,
        },
        {
            .name = "LFO 2 Rate +-1 unit (LFO 2 at 3.2 Hz on Pitch A, +-0.25 octave)",
            .target = Target::lfoRate,
            .route =
                [=](ModPatch& p)
            {
                lfo40(p);
                p.lfos[1] = {.waveform = dsp::LfoWaveform::sine, .rate = 5.0f, .gain = 10.0f};
                p.route(4, SourceId::lfo1, DestinationId::lfo2Rate, 2.0f);
                p.route(6, SourceId::lfo2, DestinationId::pitchA, 0.5f);
            },
            .voicing = sine,
            .rates = {EvalRate::perSample, EvalRate::gridHeld},
        },
        {
            .name = "LFO 2 Gain 5 +- 5 (LFO 2 at 3.2 Hz on Volume A, 5 +- 5)",
            .target = Target::lfoGain,
            .route =
                [=](ModPatch& p)
            {
                lfo40(p);
                p.lfos[1] = {.waveform = dsp::LfoWaveform::sine, .rate = 5.0f, .gain = 5.0f};
                p.route(4, SourceId::lfo1, DestinationId::lfo2Gain, 10.0f);
                p.route(6, SourceId::lfo2, DestinationId::volumeA, 10.0f);
            },
            .voicing = quiet,
            .rates = all,
        },
    };
}

ModPatch ratePatch(const RateCase& c, EvalRate r)
{
    ModPatch p;
    c.route(p);
    p.rates[index(c.target)] = r;
    if (c.target == Target::lfoRate || c.target == Target::lfoGain)
    {
        // The LFOs' order follows their targets' rates; keep the other one
        // on the grid unless this one is per sample.
        const auto other = c.target == Target::lfoRate ? Target::lfoGain : Target::lfoRate;
        p.rates[index(other)] = r == EvalRate::perSample ? EvalRate::perSample : EvalRate::gridHeld;
    }
    return p;
}

void printRates()
{
    std::cout << "\n## The rate per destination\n\n### Zipper on a 40 Hz LFO\n\n"
                 "A1 a sine at A3 (220 Hz), both filters bypassed, drift off, one note; LFO 1 a sine at 40 Hz, Gain "
                 "10, on the destination. Energy above 1 kHz in a Hann 4096, dB re an instant unit step, averaged "
                 "over 0.3/0.5/0.7/0.9 s. *Added*: the same measure on the difference from per sample.\n\n"
                 "| Destination | per sample (its own) | grid, held | grid, ramped | added: held | added: ramped "
                 "|\n|---|---:|---:|---:|---:|---:|\n";
    const std::vector<std::size_t> windows{samples(0.3), samples(0.5), samples(0.7), samples(0.9)};
    for (const auto& c : rateCases())
    {
        std::vector<Buffer> r;
        for (const auto rt : evalRates)
        {
            const bool offered = std::ranges::find(c.rates, rt) != c.rates.end();
            r.push_back(offered ? render(ratePatch(c, rt), c.voicing, {noteOn(0, 57)}, samples(1.2)) : Buffer{2, 1});
        }
        std::cout << "| " << c.name << " |";
        for (std::size_t k = 0; k < 3; ++k)
        {
            std::cout << ' ' << (r[k].numSamples() > 1 ? dbText(clickDb(r[k], windows)) : "-") << " |";
        }
        for (std::size_t k = 1; k < 3; ++k)
        {
            std::cout << ' ' << (r[k].numSamples() > 1 ? dbText(clickDb(difference(r[k], r[0]), windows)) : "-")
                      << " |";
        }
        std::cout << '\n';
    }

    std::cout << "\n### The same output at any host block size\n\n"
                 "The same patches with a sequence (Polyphony 2: four notes, a steal, releases of 50 ms), plus "
                 "ModEnv 1 on Cutoff A and velocity on Mix A, 1 s at host blocks of 1 and 37 against 4096:\n\n"
                 "| Destination | per sample | grid, held | grid, ramped | allocations |\n|---|---|---|---|---:|\n";
    const std::vector<EngineEvent> sequence{
        noteOn(0, 57, 100),    noteOn(9001, 61, 70), noteOn(17013, 64, 120), noteOff(25100, 57),
        noteOn(30007, 69, 90), noteOff(36000, 61),   noteOff(40000, 64),     noteOff(44000, 69),
    };
    for (const auto& c : rateCases())
    {
        std::cout << "| " << c.name << " |";
        std::size_t allocations = 0;
        for (const auto rt : evalRates)
        {
            if (std::ranges::find(c.rates, rt) == c.rates.end())
            {
                std::cout << " - |";
                continue;
            }
            auto p = ratePatch(c, rt);
            p.modEnvelopes[0] = {.attack = 0.01f, .decay = 0.1f, .sustain = 0.3f, .release = 0.05f};
            p.route(0, SourceId::modEnv1, DestinationId::cutoffA, 3.0f);
            p.route(8, SourceId::velocity, DestinationId::mixA, 5.0f);
            auto v = c.voicing;
            v.polyphony = 2;
            v.release = {0.05f, 0.05f};
            const auto reference = render(p, v, sequence, samples(1.0), {.blockSize = 4096});
            int same = 0;
            for (const std::size_t b : {std::size_t{1}, std::size_t{37}})
            {
                const auto x = render(p, v, sequence, samples(1.0), {.blockSize = b, .allocations = &allocations});
                same += anoesis::test::bitIdentical(reference, x) ? 1 : 0;
            }
            std::cout << ' ' << same << " of 2 |";
        }
        std::cout << ' ' << allocations << " |\n";
    }
}

// --- Phase: band-limited? ----------------------------------------------------------------

/// The energy off the phase modulation's lines (k fc + m fm, |m| up to the
/// modulation's index plus 12), below 20 kHz, re the total: what aliasing
/// and zipper add. Blackman-Harris 65536, lines masked +-8 bins.
double offLineDb(std::span<const float> x, double fc, double fm, double depthCycles)
{
    namespace t = anoesis::test;
    static const auto window = t::blackmanHarrisWindow(65536);
    const auto s = t::powerSpectrum(x.subspan(x.size() - 65536, 65536), rate, window);
    std::vector<bool> line(s.power.size(), false);
    const double bin = static_cast<double>(rate) / 65536.0;
    for (int k = 1; static_cast<double>(k) * fc < 0.5 * static_cast<double>(rate); ++k)
    {
        const double beta = 2.0 * std::numbers::pi * static_cast<double>(k) * depthCycles;
        const int mMax = static_cast<int>(std::ceil(beta)) + 12;
        for (int m = -mMax; m <= mMax; ++m)
        {
            const double f = (static_cast<double>(k) * fc) + (static_cast<double>(m) * fm);
            if (f <= 0.0 || f >= 0.5 * static_cast<double>(rate))
            {
                continue;
            }
            const auto centre = std::lround(f / bin);
            for (long j = centre - 8; j <= centre + 8; ++j)
            {
                if (j >= 0 && static_cast<std::size_t>(j) < line.size())
                {
                    line[static_cast<std::size_t>(j)] = true;
                }
            }
        }
    }
    double off = 0.0;
    double total = 0.0;
    for (std::size_t k = 1; k < s.power.size(); ++k)
    {
        if (s.frequencyOf(k) > 20000.0)
        {
            continue;
        }
        total += s.power[k];
        off += line[k] ? 0.0 : s.power[k];
    }
    return 10.0 * std::log10(off / total);
}

void printPhaseBandLimiting()
{
    std::cout << "\n### Phase: does it stay band-limited?\n\n"
                 "One unison voice of a saw, filters bypassed, drift off; LFO 1 a sine at 40 Hz on Phase A. Energy "
                 "below 20 kHz off the phase modulation's lines (k fc +- m fm), dB re the total: the aliasing and "
                 "the zipper together (the oscillator alone: its own aliasing). *Jumps*: changes made as "
                 "band-limited jumps because the step would have left [0, Nyquist).\n\n"
                 "| Case | unmodulated | per sample | grid, held | grid, ramped | jumps (per sample) |\n"
                 "|---|---:|---:|---:|---:|---:|\n";
    struct Case
    {
        std::string_view name;
        int key;
        float fine;
        float amount;
    };
    // fc off any simple ratio to fm and fs, so aliases miss the lines.
    for (const auto& c : {
             Case{.name = "1,090 Hz, +-18 degrees", .key = 84, .fine = 0.62f, .amount = 1.0f},
             Case{.name = "1,090 Hz, +-180 degrees", .key = 84, .fine = 0.62f, .amount = 10.0f},
             Case{.name = "55.6 Hz, +-180 degrees (the step goes negative)", .key = 33, .fine = 0.19f, .amount = 10.0f},
         })
    {
        const double fc = 440.0 * std::exp2((static_cast<double>(c.key) + static_cast<double>(c.fine) - 69.0) / 12.0);
        const float units = rateUnits(40.0);
        const auto fm = static_cast<double>(lfoRateHz(units));
        Voicing const v{.bypass = true};
        std::cout << "| " << c.name << " |";
        std::uint64_t jumps = 0;
        for (int k = -1; k < 3; ++k)
        {
            ModPatch p;
            p.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = units, .gain = 10.0f};
            if (k >= 0)
            {
                p.route(4, SourceId::lfo1, DestinationId::phaseA, c.amount);
                p.rates[index(Target::phase)] = evalRates[static_cast<std::size_t>(k)];
            }
            const RenderOptions options{
                .blockSize = 512,
                .afterBlock =
                    [&](ModEngine& e)
                {
                    if (k == 0)
                    {
                        jumps = e.voices().voice(0).slot(0).phaseJumps();
                    }
                },
            };
            // The fine tune through the slot's tuning.
            auto engineVoicing = v;
            const auto out = [&]
            {
                std::vector<EngineEvent> const events{noteOn(0, c.key)};
                ModPatch const q = p;
                auto engine = std::make_unique<ModEngine>(table(), q, 3);
                engine->voices().forEachVoice(
                    [&](auto& voice)
                    {
                        configure(voice, engineVoicing);
                        voice.slot(0).setTuning({.fine = c.fine});
                    });
                engine->prepare(spec);
                const std::size_t length = 65536 + 4800;
                Buffer b{2, length};
                for (std::size_t start = 0; start < length; start += 512)
                {
                    const auto n = std::min<std::size_t>(512, length - start);
                    std::vector<EngineEvent> here;
                    if (start == 0)
                    {
                        here = events;
                    }
                    engine->process(b.block().subBlock(start, n), here);
                    options.afterBlock(*engine);
                }
                return b;
            }();
            const double depth = 0.5 * static_cast<double>(c.amount) / 10.0;
            if (c.key < 60)
            {
                std::cout << " n/a |"; // the lines are dense enough to mask the whole band
                continue;
            }
            std::cout << ' ' << dbText(offLineDb(out.channel(0), fc, fm, k >= 0 ? depth : 0.0)) << " |";
        }
        std::cout << ' ' << jumps << " |\n";
    }
}

// --- The fast-sweep overshoot -------------------------------------------------------------

void printOvershoot()
{
    std::cout
        << "\n## The fast-sweep overshoot (filter-control.md, recommendation 8)\n\n"
           "filter-control.md's patch: a 110 Hz saw, Cutoff A 5 + Control 5, LFO 1 on Cutoff A through both "
           "of its routes at +10 (+-10 units), Resonance A and Control 10 with ModEnv 1 (sustain 1) on Reso A "
           "at +10 (k 12 under the per-control clamp), Warm on, LP24, through the voice (a band-limited saw, "
           "the LFO starting at its peak). Peak from 0.2 s to 2 s, at 48 kHz and at 44.1 kHz (filter-control.md "
           "measured a naive saw at 44.1 kHz, its LFO from 0, the cutoff read at the segment's end: 13.10 at 5 Hz)."
           "\n\n| LFO | 48 kHz: as the sources stand at the grid point | 48 kHz: the LFO at the segment's end | "
           "44.1 kHz: as they stand | 44.1 kHz: at the end |\n|---|---:|---:|---:|---:|\n";
    for (const auto& [name, hz, gain] :
         {std::tuple{"held (Gain 0)", 5.0, 0.0f}, std::tuple{"0.5 Hz", 0.5, 10.0f}, std::tuple{"5 Hz", 5.0, 10.0f}})
    {
        std::cout << "| " << name << " |";
        for (const int k : {0, 1, 2, 3})
        {
            const bool end = k % 2 == 1;
            const float fs = k < 2 ? rate : 44100.0f;
            ModPatch p;
            p.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = rateUnits(hz), .gain = gain};
            p.modEnvelopes[0] = {.sustain = 1.0f};
            p.route(0, SourceId::modEnv1, DestinationId::resonanceA, 10.0f);
            p.route(4, SourceId::lfo1, DestinationId::cutoffA, 10.0f);
            p.route(5, SourceId::lfo1, DestinationId::cutoffA, 10.0f);
            p.cutoffAtSegmentEnd = end;
            const Voicing v{.resonance = 10.0f, .controlResonance = 10.0f, .warm = true};
            const auto out = render(p, v, {noteOn(0, 45)}, samples(2.0), {.sampleRate = fs});
            std::cout << ' ' << fixed(peakOf(out, samples(0.2)), 2) << " |";
        }
        std::cout << '\n';
    }
}

// --- Order and feedback -------------------------------------------------------------------

void printOrder()
{
    std::cout << "\n## Order and feedback\n\n"
                 "Segment order: LFO Rate held and Gain ramped on the grid, each LFO's segment rendered ahead from "
                 "the sources at its grid point. Sample order: both per sample, from the sources at the sample "
                 "before. A1 a sine at A3, filters bypassed. *Differs*: RMS of the difference re the render's, "
                 "0.2-2 s.\n\n"
                 "| Case | segment: same at blocks 1/37/4096 | sample: same at 1/37/4096 | differs | allocations |\n"
                 "|---|---|---|---:|---:|\n";
    struct Case
    {
        std::string_view name;
        std::function<void(ModPatch&)> route;
    };
    const std::array cases{
        Case{
            .name = "cycle: LFO 1 -> LFO 2 Rate (+5), LFO 2 -> LFO 1 Gain (+10); both on Pitch A",
            .route =
                [](ModPatch& p)
            {
                p.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = 7.0f, .gain = 5.0f};
                p.lfos[1] = {.waveform = dsp::LfoWaveform::triangle, .rate = 6.0f, .gain = 10.0f};
                p.route(4, SourceId::lfo1, DestinationId::lfo2Rate, 5.0f);
                p.route(5, SourceId::lfo1, DestinationId::pitchA, 0.3f);
                p.route(6, SourceId::lfo2, DestinationId::lfo1Gain, 10.0f);
                p.route(7, SourceId::lfo2, DestinationId::pitchA, 0.5f);
            },
        },
        Case{
            .name = "self: LFO 1 -> LFO 1 Rate (+5) and Pitch A",
            .route =
                [](ModPatch& p)
            {
                p.lfos[0] = {.waveform = dsp::LfoWaveform::saw, .rate = 6.0f, .gain = 10.0f};
                p.route(4, SourceId::lfo1, DestinationId::lfo1Rate, 5.0f);
                p.route(5, SourceId::lfo1, DestinationId::pitchA, 0.5f);
            },
        },
    };
    const Voicing v{
        .waves = {dsp::Waveform::sine, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw},
        .bypass = true,
        .release = {0.05f, 0.05f},
        .polyphony = 2,
    };
    const std::vector<EngineEvent> events{noteOn(0, 57), noteOn(30011, 64), noteOff(60000, 57), noteOff(80000, 64)};
    for (const auto& c : cases)
    {
        std::cout << "| " << c.name << " |";
        std::array<Buffer, 2> reference{Buffer{2, 1}, Buffer{2, 1}};
        std::size_t allocations = 0;
        for (const bool perSample : {false, true})
        {
            ModPatch p;
            c.route(p);
            p.rates[index(Target::lfoRate)] = perSample ? EvalRate::perSample : EvalRate::gridHeld;
            p.rates[index(Target::lfoGain)] = perSample ? EvalRate::perSample : EvalRate::gridRamped;
            reference[perSample ? 1 : 0] = render(p, v, events, samples(2.0), {.blockSize = 4096});
            int same = 0;
            for (const std::size_t b : {std::size_t{1}, std::size_t{37}})
            {
                const auto x = render(p, v, events, samples(2.0), {.blockSize = b, .allocations = &allocations});
                same += anoesis::test::bitIdentical(reference[perSample ? 1 : 0], x) ? 1 : 0;
            }
            std::cout << ' ' << same << " of 2 |";
        }
        const auto d = difference(reference[0], reference[1]);
        const std::size_t from = samples(0.2);
        const std::size_t n = samples(2.0) - from;
        std::cout << ' ' << dbText(toDb(rmsOf(d, from, n) / rmsOf(reference[1], from, n))) << " dB | " << allocations
                  << " |\n";
    }

    // The delay from a sample-and-hold step on LFO 1 to LFO 2's rate.
    std::cout << "\nThe delay through a route into a source (M11's protocol): LFO 1 S&H at 2.03 Hz (its steps fall "
                 "anywhere in a segment) -> LFO 2 Rate "
                 "(+5), LFO 2 a pulse on Pitch A. Samples from each of LFO 1's steps (its first slewed sample) to "
                 "LFO 2's rate changing, over 8 steps:\n\n| Order | min | mean | max |\n|---|---:|---:|---:|\n";
    for (const bool perSample : {false, true})
    {
        ModPatch p;
        p.lfos[0] = {.waveform = dsp::LfoWaveform::sampleHold, .rate = rateUnits(2.03), .gain = 10.0f};
        p.lfos[1] = {.waveform = dsp::LfoWaveform::pulse, .rate = 6.0f, .gain = 10.0f};
        p.route(4, SourceId::lfo1, DestinationId::lfo2Rate, 5.0f);
        p.route(6, SourceId::lfo2, DestinationId::pitchA, 0.5f);
        p.rates[index(Target::lfoRate)] = perSample ? EvalRate::perSample : EvalRate::gridHeld;
        p.rates[index(Target::lfoGain)] = perSample ? EvalRate::perSample : EvalRate::gridHeld;
        std::vector<std::size_t> delays;
        std::size_t t = 0;
        float previous = 0.0f;
        std::uint64_t lastIncrement = 0;
        std::size_t stepAt = 0;
        bool waiting = false;
        const RenderOptions options{
            .blockSize = 1,
            .afterBlock =
                [&](ModEngine& e)
            {
                const auto& voice = e.voices().voice(0);
                const float now = voice.sourceNow(SourceId::lfo1);
                const auto inc = voice.lfo(1).phaseIncrement();
                if (!waiting && t >= stepAt + 2000 && std::abs(now - previous) > 1e-6f)
                {
                    waiting = true; // LFO 1's step starts here
                    stepAt = t;
                }
                else if (waiting && inc != lastIncrement)
                {
                    delays.push_back(t - stepAt);
                    waiting = false;
                }
                previous = now;
                lastIncrement = inc;
                ++t;
            },
        };
        static_cast<void>(render(p, Voicing{.bypass = true}, {noteOn(0, 57)}, samples(4.2), options));
        if (delays.size() > 8)
        {
            delays.resize(8);
        }
        double mean = 0.0;
        for (const auto d : delays)
        {
            mean += static_cast<double>(d);
        }
        mean /= static_cast<double>(std::max<std::size_t>(1, delays.size()));
        std::cout << "| " << (perSample ? "sample (rate per sample)" : "segment (rate at grid points)") << " | "
                  << (delays.empty() ? 0 : *std::ranges::min_element(delays)) << " | " << fixed(mean, 1) << " | "
                  << (delays.empty() ? 0 : *std::ranges::max_element(delays)) << " |\n";
    }
}

// --- A per-voice source on a global destination ----------------------------------------------

enum class GlobalRule : std::uint8_t
{
    newest,
    oldestHeld,
    sum,
    globalCopy,
};

std::string_view ruleName(GlobalRule r)
{
    switch (r)
    {
    case GlobalRule::newest:
        return "newest voice";
    case GlobalRule::oldestHeld:
        return "oldest held voice";
    case GlobalRule::sum:
        return "sum over voices";
    case GlobalRule::globalCopy:
        return "a global copy";
    }
    return "?";
}

struct GlobalTrace
{
    std::vector<float> value; ///< the source's value per 32-sample grid point (absolute)
    Buffer out{2, 1};         ///< a 220 Hz sine through the stand-in global gain
};

/// ModEnv 1 or velocity, from the voices, onto a stand-in global gain
/// (clamp(5 + 5 x value, 0, 10) / 10, ramped over each 32-sample grid
/// segment, as a global destination on the absolute grid) on a fixed 220 Hz
/// sine. The global copy: one Adsr gated by the first key down and the last
/// key up, and the last note-on's velocity.
GlobalTrace globalTrace(GlobalRule rule, bool velocitySource)
{
    ModPatch p;
    const EnvelopeSettings env{.attack = 0.02f, .decay = 0.2f, .sustain = 0.5f, .release = 0.3f};
    p.modEnvelopes[0] = env;
    p.route(0, SourceId::modEnv1, DestinationId::cutoffA, 0.001f); // so the voices run it
    const Voicing v{.release = {0.3f, 0.3f}, .polyphony = 8};
    std::vector<EngineEvent> events;
    const std::array<int, 4> velocities{100, 60, 127, 80};
    events.reserve(4);
    for (std::size_t k = 0; k < 4; ++k)
    {
        events.push_back(
            noteOn(samples(0.1 + (0.4 * static_cast<double>(k))), 57 + (4 * static_cast<int>(k)), velocities[k]));
    }
    for (std::size_t k = 0; k < 4; ++k)
    {
        events.push_back(noteOff(samples(2.0 + (0.4 * static_cast<double>(k))), 57 + (4 * static_cast<int>(k))));
    }
    std::ranges::stable_sort(events, {}, &EngineEvent::offset);
    auto engine = std::make_unique<ModEngine>(table(), p, 3);
    engine->voices().forEachVoice([&](auto& voice) { configure(voice, v); });
    engine->prepare(spec);
    engine->voices().setPolyphony(8);
    dsp::Adsr copy;
    copy.prepare(spec);
    copy.setAttack(env.attack);
    copy.setDecay(env.decay);
    copy.setSustain(env.sustain);
    copy.setRelease(env.release);
    float lastVelocity = 0.0f;
    int keysDown = 0;
    const std::size_t length = samples(3.8);
    GlobalTrace trace;
    trace.out = Buffer{2, length};
    Buffer voices{2, 32};
    std::array<float, 32> copyLevels{};
    float before = 0.0f;
    std::size_t e = 0;
    for (std::size_t start = 0; start < length; start += 32)
    {
        std::vector<EngineEvent> here;
        while (e < events.size() && events[e].offset < start + 32)
        {
            auto ev = events[e];
            ev.offset -= start;
            here.push_back(ev);
            ++e;
        }
        // The global copy, gated at the same samples.
        std::size_t done = 0;
        for (const auto& ev : here)
        {
            copy.process(std::span{copyLevels}.subspan(done, ev.offset - done));
            done = ev.offset;
            if (ev.voice.kind == dsp::VoiceEventKind::noteOn)
            {
                if (keysDown == 0)
                {
                    copy.gateOn();
                }
                ++keysDown;
                lastVelocity = static_cast<float>(ev.voice.velocity) / 127.0f;
            }
            else
            {
                --keysDown;
                if (keysDown == 0)
                {
                    copy.gateOff();
                }
            }
        }
        copy.process(std::span{copyLevels}.subspan(done));
        engine->process(voices.block(), here);
        // The value at this grid point, by rule.
        auto& a = engine->voices();
        const auto valueOf = [&](std::size_t i)
        {
            return velocitySource ? a.voice(i).velocity() : a.voice(i).sourceNow(SourceId::modEnv1);
        };
        float value = 0.0f;
        std::size_t pick = 16;
        switch (rule)
        {
        case GlobalRule::newest:
        case GlobalRule::oldestHeld:
        {
            std::uint64_t best = rule == GlobalRule::newest ? 0 : std::numeric_limits<std::uint64_t>::max();
            for (std::size_t i = 0; i < 16; ++i)
            {
                if (!a.isBusy(i) || (rule == GlobalRule::oldestHeld && !a.isKeyDown(i)))
                {
                    continue;
                }
                const auto t = a.voice(i).noteOnTime();
                if ((rule == GlobalRule::newest && t >= best) || (rule == GlobalRule::oldestHeld && t < best))
                {
                    best = t;
                    pick = i;
                }
            }
            if (pick == 16 && rule == GlobalRule::oldestHeld)
            {
                // No key held: the newest sounding voice's tail.
                std::uint64_t newest = 0;
                for (std::size_t i = 0; i < 16; ++i)
                {
                    if (a.isBusy(i) && a.voice(i).noteOnTime() >= newest)
                    {
                        newest = a.voice(i).noteOnTime();
                        pick = i;
                    }
                }
            }
            value = pick < 16 ? valueOf(pick) : 0.0f;
            break;
        }
        case GlobalRule::sum:
            for (std::size_t i = 0; i < 16; ++i)
            {
                value += a.isBusy(i) ? valueOf(i) : 0.0f;
            }
            break;
        case GlobalRule::globalCopy:
        {
            const bool sounding = keysDown > 0 || copy.isActive();
            const float velocityNow = sounding ? lastVelocity : 0.0f;
            value = velocitySource ? velocityNow : copy.level();
            break;
        }
        }
        trace.value.push_back(value);
        // The stand-in global destination, ramped over the next segment.
        const float from = std::clamp(5.0f + (5.0f * before), 0.0f, 10.0f) / 10.0f;
        const float to = std::clamp(5.0f + (5.0f * value), 0.0f, 10.0f) / 10.0f;
        for (std::size_t i = 0; i < 32 && start + i < length; ++i)
        {
            const double t = static_cast<double>(start + i) / static_cast<double>(rate);
            const auto x = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * t));
            const float g = from + ((to - from) * (static_cast<float>(i) / 32.0f));
            trace.out.channel(0)[start + i] = g * x;
            trace.out.channel(1)[start + i] = g * x;
        }
        before = value;
    }
    return trace;
}

void printGlobal()
{
    std::cout << "\n## A per-voice source on a global destination\n\n"
                 "Four notes 0.4 s apart from 0.1 s (velocities 100, 60, 127, 80), released 0.4 s apart from 2.0 s "
                 "in the order played; ModEnv 1 (attack 20 ms, decay 200 ms, sustain 0.5, release 300 ms) or "
                 "velocity, read from the voices at each 32-sample point of the absolute grid, onto a stand-in "
                 "global gain (5 + 5 x value, ramped over each segment) on a fixed 220 Hz sine. *Largest step*: the "
                 "largest change between two grid points from just before a note-on (the second to the fourth) or a "
                 "note-off to 50 ms after it. *Held*: the "
                 "value 0.35 s after each note-on (1, 2, 3, 4 notes). *After release*: 0.1 s after each note-off. "
                 "*Click*: filters.md's measure at the note-ons and note-offs.\n\n"
                 "| Source | Rule | largest step at a note-on | at a note-off | held: 1 / 2 / 3 / 4 notes | after "
                 "each release | click |\n|---|---|---:|---:|---|---|---:|\n";
    for (const bool velocity : {false, true})
    {
        for (const auto rule : {GlobalRule::newest, GlobalRule::oldestHeld, GlobalRule::sum, GlobalRule::globalCopy})
        {
            const auto tr = globalTrace(rule, velocity);
            const auto at = [&](double seconds)
            {
                return tr.value[std::min(tr.value.size() - 1, samples(seconds) / 32)];
            };
            const auto largestStep = [&](double from)
            {
                float step = 0.0f;
                const std::size_t g0 = (samples(from) / 32) - 1; // the grid point before the event
                for (std::size_t g = g0; g < g0 + 75 && g + 1 < tr.value.size(); ++g)
                {
                    step = std::max(step, std::abs(tr.value[g + 1] - tr.value[g]));
                }
                return step;
            };
            float onStep = 0.0f;
            float offStep = 0.0f;
            std::vector<std::size_t> clicks;
            for (std::size_t k = 0; k < 4; ++k)
            {
                if (k > 0) // the first note starts every rule from silence
                {
                    onStep = std::max(onStep, largestStep(0.1 + (0.4 * static_cast<double>(k))));
                }
                offStep = std::max(offStep, largestStep(2.0 + (0.4 * static_cast<double>(k))));
                clicks.push_back(samples(0.1 + (0.4 * static_cast<double>(k))));
                clicks.push_back(samples(2.0 + (0.4 * static_cast<double>(k))));
            }
            std::cout << "| " << (velocity ? "velocity" : "ModEnv 1") << " | " << ruleName(rule) << " | "
                      << fixed(onStep, 3) << " | " << fixed(offStep, 3) << " | ";
            for (std::size_t k = 0; k < 4; ++k)
            {
                std::cout << fixed(at(0.45 + (0.4 * static_cast<double>(k))), 2) << (k < 3 ? " / " : "");
            }
            std::cout << " | ";
            for (std::size_t k = 0; k < 4; ++k)
            {
                std::cout << fixed(at(2.1 + (0.4 * static_cast<double>(k))), 2) << (k < 3 ? " / " : "");
            }
            std::cout << " | " << dbText(clickDb(tr.out, clicks)) << " |\n";
        }
    }
}

// --- Each source's bound -------------------------------------------------------------------

void printBounds()
{
    std::cout << "\n## Each source's bound\n\n### Against the source's values over random segments\n\n"
                 "Each bound taken at a random point for the next 1-32 samples, with events only between "
                 "segments. Envelopes: 60 random ADSRs, 200 segments each, with random gates and sustain changes. "
                 "LFOs: every waveform at random rates (0.1 Hz to 819 Hz), gains and offsets, with random "
                 "retriggers. *Too small*: segments with a value outside the bound. *Width*: the mean width of the "
                 "interval.\n\n| Source | Bound | segments | too small | worst excess | mean width |\n"
                 "|---|---|---:|---:|---:|---:|\n";
    {
        dsp::Random random{11};
        struct Stat
        {
            std::size_t bad = 0;
            double excess = 0.0;
            double width = 0.0;
        };
        std::array<Stat, 3> st{};
        std::size_t segments = 0;
        for (int trial = 0; trial < 60; ++trial)
        {
            dsp::Adsr e;
            e.prepare(spec);
            e.setAttack(0.0005f + (0.05f * random.nextUnipolar()));
            e.setDecay(0.0005f + (0.05f * random.nextUnipolar()));
            e.setSustain(trial % 4 == 0 ? 0.0f : random.nextUnipolar());
            e.setRelease(0.0005f + (0.05f * random.nextUnipolar()));
            e.gateOn();
            std::array<float, 32> y{};
            for (int seg = 0; seg < 200; ++seg)
            {
                const auto r = random.nextUint32() % 40;
                if (r == 0)
                {
                    e.gateOff();
                }
                else if (r == 1)
                {
                    e.gateOn();
                }
                else if (r == 2)
                {
                    e.setSustain(random.nextUnipolar());
                }
                const auto n = 1 + (random.nextUint32() % 32);
                const auto state = envelopeState(e);
                const std::array bounds{envelopeRangeBound(state), envelopePlanBound(state), envelopeStageBound(state)};
                e.process(std::span{y}.first(n));
                for (std::size_t b = 0; b < 3; ++b)
                {
                    bool bad = false;
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        const double ex = std::max(static_cast<double>(y[i] - bounds[b].hi),
                                                   static_cast<double>(bounds[b].lo - y[i]));
                        if (ex > 0.0)
                        {
                            bad = true;
                            st[b].excess = std::max(st[b].excess, ex);
                        }
                    }
                    st[b].bad += bad ? 1U : 0U;
                    st[b].width += static_cast<double>(bounds[b].hi - bounds[b].lo);
                }
                ++segments;
            }
        }
        const std::array<std::string_view, 3> names{
            "its range [0, 1]",
            "PLAN's: 0 when idle or held at 0, else [0, 1]",
            "by stage (level to where the stage heads)",
        };
        for (std::size_t b = 0; b < 3; ++b)
        {
            std::cout << "| envelope | " << names[b] << " | " << segments << " | " << st[b].bad << " | "
                      << fixed(st[b].excess, 6) << " | " << fixed(st[b].width / static_cast<double>(segments), 3)
                      << " |\n";
        }
    }
    {
        dsp::Random random{13};
        struct Stat
        {
            std::size_t bad = 0;
            double excess = 0.0;
            double width = 0.0;
        };
        std::array<Stat, 3> st{};
        std::size_t segments = 0;
        double largestAtOffset0 = 0.0;
        double largestAtOffset10 = 0.0;
        for (int trial = 0; trial < 88; ++trial)
        {
            dsp::Lfo lfo{static_cast<std::uint64_t>(trial)};
            lfo.prepare(spec);
            lfo.setWaveform(static_cast<dsp::LfoWaveform>(static_cast<std::size_t>(trial) % dsp::lfoWaveformCount));
            lfo.setFrequency(0.1f * std::exp2(13.0f * random.nextUnipolar()));
            std::array<float, 32> wave{};
            for (int seg = 0; seg < 300; ++seg)
            {
                if (random.nextUint32() % 50 == 0)
                {
                    lfo.retrigger();
                }
                const auto n = 1 + (random.nextUint32() % 32);
                const float gain = random.nextUnipolar();
                const float offset = random.nextBipolar();
                const auto crest = lfoEndsAndCrestBound(lfo, n);
                lfo.process(std::span{wave}.first(n));
                const std::array bounds{
                    lfoSourceBound(lfoWaveRange, {.lo = 0.0f, .hi = 1.0f}, {.lo = -1.0f, .hi = 1.0f}),
                    lfoSourceBound(crest, {.lo = gain, .hi = gain}, {.lo = offset, .hi = offset}),
                    lfoSourceBound(renderedBound(std::span{wave}.first(n)), {.lo = gain, .hi = gain},
                                   {.lo = offset, .hi = offset}),
                };
                for (std::size_t b = 0; b < 3; ++b)
                {
                    bool bad = false;
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        const float source = 0.5f * gain * (wave[i] + offset);
                        const double ex = std::max(static_cast<double>(source - bounds[b].hi),
                                                   static_cast<double>(bounds[b].lo - source));
                        if (ex > 0.0)
                        {
                            bad = true;
                            st[b].excess = std::max(st[b].excess, ex);
                        }
                    }
                    st[b].bad += bad ? 1U : 0U;
                    st[b].width += static_cast<double>(bounds[b].hi - bounds[b].lo);
                }
                for (std::size_t i = 0; i < n; ++i)
                {
                    largestAtOffset0 = std::max(largestAtOffset0, std::abs(0.5 * static_cast<double>(wave[i])));
                    largestAtOffset10 = std::max(largestAtOffset10, 0.5 * (static_cast<double>(wave[i]) + 1.0));
                }
                ++segments;
            }
        }
        const std::array<std::string_view, 3> names{
            "its range (+-1.125)",
            "the wave's ends and crests (PLAN's), no slews",
            "exact: the segment rendered ahead",
        };
        for (std::size_t b = 0; b < 3; ++b)
        {
            std::cout << "| LFO | " << names[b] << " | " << segments << " | " << st[b].bad << " | "
                      << fixed(st[b].excess, 6) << " | " << fixed(st[b].width / static_cast<double>(segments), 3)
                      << " |\n";
        }
        std::cout << "\nThe LFO as a source at Gain 10 (M6's halving): largest |value| at Offset 0: "
                  << fixed(largestAtOffset0, 4) << "; largest value at Offset +10: " << fixed(largestAtOffset10, 4)
                  << " (a slew carries the wave up to 0.25 past +-1).\n";
    }

    std::cout << "\n### How much each bound lets the skip test skip\n\n"
                 "A1 and B1 saws, own filters, Mix B at 0 with one route lifting it; 16 notes of 0.25 s each over 4 "
                 "s (Polyphony 1). *Skipped*: part B's outputs skipped for their gain, of those tested at grid "
                 "points.\n\n| Route on Mix B | the sources' ranges | tight |\n|---|---:|---:|\n";
    struct Case
    {
        std::string_view name;
        std::function<void(ModPatch&)> route;
    };
    const std::array cases{
        Case{
            .name = "LFO 1 sine 8 Hz, +10 (+-5 internal)",
            .route =
                [](ModPatch& p)
            {
                p.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = rateUnits(8.0), .gain = 10.0f};
                p.route(4, SourceId::lfo1, DestinationId::mixB, 10.0f);
            },
        },
        Case{
            .name = "LFO 1 pulse 8 Hz, Offset -10, -10 (0 to +10)",
            .route =
                [](ModPatch& p)
            {
                p.lfos[0] = {
                    .waveform = dsp::LfoWaveform::pulse,
                    .rate = rateUnits(8.0),
                    .gain = 10.0f,
                    .offset = -10.0f,
                };
                p.route(4, SourceId::lfo1, DestinationId::mixB, -10.0f);
            },
        },
        Case{
            .name = "ModEnv 1 pluck (decay 50 ms, sustain 0), +10",
            .route =
                [](ModPatch& p)
            {
                p.modEnvelopes[0] = {.decay = 0.05f, .sustain = 0.0f, .release = 0.01f};
                p.route(0, SourceId::modEnv1, DestinationId::mixB, 10.0f);
            },
        },
    };
    for (const auto& c : cases)
    {
        std::cout << "| " << c.name << " |";
        for (const auto mode : {BoundMode::range, BoundMode::tight})
        {
            ModPatch p;
            c.route(p);
            p.bounds = mode;
            auto engine = std::make_unique<ModEngine>(table(), p, 3);
            Voicing v{.voices = {1, 0, 1, 0}, .polyphony = 1};
            engine->voices().forEachVoice([&](auto& voice) { configure(voice, v); });
            engine->prepare(spec);
            engine->voices().setPolyphony(1);
            // Mix B at 0: the knob ramps through VoiceKnobs::mix aren't wired in
            // ModEngine, so use the patch's own Mix A/B through a route offset:
            // Mix B's knob is unity (10) unless lowered, so lower it with a
            // constant route: Step Velocity 1 x -10 on Mix B.
            engine->modulation().stepVelocity = 1.0f;
            engine->modulation().route(15, SourceId::stepVelocity, DestinationId::mixB, -10.0f);
            std::vector<EngineEvent> events;
            for (std::size_t k = 0; k < 16; ++k)
            {
                events.push_back(noteOn(samples(0.25 * static_cast<double>(k)), 57));
                events.push_back(noteOff(samples(0.25 * static_cast<double>(k)) + samples(0.2), 57));
            }
            std::ranges::stable_sort(events, {}, &EngineEvent::offset);
            Buffer out{2, 512};
            std::size_t e = 0;
            const std::size_t length = samples(4.0);
            std::uint64_t tested = 0;
            std::uint64_t skipped = 0;
            for (std::size_t start = 0; start < length; start += 512)
            {
                std::vector<EngineEvent> here;
                while (e < events.size() && events[e].offset < start + 512)
                {
                    auto ev = events[e];
                    ev.offset -= start;
                    here.push_back(ev);
                    ++e;
                }
                engine->process(out.block(), here);
            }
            tested = engine->voices().voice(0).testedOutputs();
            skipped = engine->voices().voice(0).skippedOutputs();
            std::cout << ' '
                      << (tested > 0
                              ? fixed(100.0 * static_cast<double>(skipped) / static_cast<double>(tested), 1) + "%"
                              : "-")
                      << " |";
        }
        std::cout << '\n';
    }
}

} // namespace

TEST_CASE("Modulation: the candidates, measured", "[.][modulation-report]")
{
    std::cout << "# Modulation report (M1.5 task 2, stage 1)\n";
    printPrototype();
    printRegistries();
    printRates();
    printPhaseBandLimiting();
    printOvershoot();
    printOrder();
    printGlobal();
    printBounds();
    SUCCEED();
}
