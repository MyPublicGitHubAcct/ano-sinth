// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The per-voice destinations' report (docs/dsp/modulation-destinations.md,
// M1.5 task 4, stage 1): measures the candidates in destination_voice.hpp.
//
// 0. the prototype against the library: DestVoice with no Volume, Pan or
//    Phase route is dsp::Voice to the bit;
// 1. the knobs under modulation (Volume, Pan): a knob step while modulated,
//    a route appearing or going while the knob ramps, a note starting on a
//    voice that was idle while the knob moved;
// 2. a change of the routes: a route appearing on a held note (Mix, Volume,
//    Pan, Pitch, Phase, Cutoff), an amount automated in 7-bit steps, and how
//    long each takes to arrive;
// 3. Volume and Pan routes on a Noise slot;
// 4. the Volume skip test: how much of a part it skips, and what the skip
//    costs in accuracy;
// 5. Phase: an offset's step folded into one sample's phase step, against a
//    band-limited jump;
// 6. Pitch at its extremes (16 routes at +-10: +-160 octaves).
//
// Hidden from the default run; run it in an optimised build:
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[destinations-report]"
//
// CPU: the "[destinations]" benchmarks (tools/bench.sh "[destinations]").

#include "destination_support.hpp"

#include "../filters/filter_control_support.hpp"

#include <anoesis/dsp/oscillators/phase.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace anoesis::test::destinations;
using anoesis::test::Buffer;

namespace
{

namespace vs = anoesis::test::voice_sources;

constexpr float rate = 48000.0f;

std::string fixed(double x, int digits)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

std::string dbText(double db)
{
    if (!std::isfinite(db))
    {
        return db < 0.0 ? "exact 0" : "inf";
    }
    return fixed(db, 1);
}

/// filters.md's click: energy above 1 kHz in a Hann 4096 centred 32 samples
/// after `at`, both channels, re an instant unit step's, in dB.
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

double rmsOf(const Buffer& b, std::size_t from, std::size_t n, std::size_t channel = 2)
{
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        if (channel < 2 && c != channel)
        {
            continue;
        }
        for (std::size_t i = from; i < from + n && i < b.numSamples(); ++i)
        {
            sum += static_cast<double>(b.channel(c)[i]) * b.channel(c)[i];
            ++count;
        }
    }
    return count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
}

double peakOf(const Buffer& b, std::size_t from, std::size_t n)
{
    double peak = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = from; i < from + n && i < b.numSamples(); ++i)
        {
            peak = std::max(peak, std::abs(static_cast<double>(b.channel(c)[i])));
        }
    }
    return peak;
}

double toDb(double ratio)
{
    return ratio > 0.0 ? 20.0 * std::log10(ratio) : -std::numeric_limits<double>::infinity();
}

/// A1 a sine at A3, filters bypassed, one note.
DestVoicing sineVoicing()
{
    DestVoicing d;
    d.base.waves = {dsp::Waveform::sine, dsp::Waveform::saw, dsp::Waveform::saw, dsp::Waveform::saw};
    d.base.voices = {1, 0, 0, 0};
    d.base.bypass = true;
    d.base.release = {0.3f, 0.3f};
    d.base.polyphony = 1;
    return d;
}

constexpr std::array knobSources{KnobSource::targets, KnobSource::slotRamps, KnobSource::globalRamps};

// --- 0. The prototype against the library -----------------------------------------------

void printPrototype()
{
    std::cout << "\n## 0. DestVoice with no Volume, Pan or Phase route against dsp::Voice\n\n"
              << "| Patch | host blocks of 1 | of 37 | of 4096 |\n|---|---|---|---|\n";
    const auto busy = vs::busyEvents();
    const std::vector<SourceEvent> init{
        proto::noteOn(0, 57),
        proto::noteOn(4000, 64),
        proto::noteOff(9000, 57),
        proto::noteOff(12000, 64),
    };
    for (const auto* events : {&init, &busy})
    {
        const bool isBusy = events == &busy;
        std::cout << "| " << (isBusy ? "task 3's busy patch (Pitch, Mix, Cutoff, the LFOs; every source)" : "init")
                  << " |";
        for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
        {
            const auto sources = isBusy ? vs::busySettings() : proto::SourceSettings{};
            const auto voicing = isBusy ? vs::busyVoicing : Voicing{};
            const auto library = renderLibrary(sources, voicing, *events, 24000, block);
            DestVoicing d;
            d.base = voicing;
            const auto out = renderDest(defaultSettings, sources, d, wrapAll(*events), 24000, {.blockSize = block});
            std::cout << " " << (proto::identical(out, library) ? "identical" : "differs") << " |";
        }
        std::cout << "\n";
    }
}

// --- 1. The knobs under modulation --------------------------------------------------------

/// A route that reads the wheel, left at 0: the path is the modulated one,
/// the sum an exact 0.
DestEvent silentRoute(std::size_t t, Dest d)
{
    return route(t, 8, Source::modWheel, d, 1.0f);
}

void printKnobs()
{
    const std::size_t note = 2400;
    const std::size_t at = 24000; // a grid point of the note (21600 = 675 x 32 after it)
    const std::size_t length = 48000;
    const auto render = [&](KnobSource k, std::vector<DestEvent> e)
    {
        e.push_back(noteOn(note, 57));
        return renderDest({.knobs = k}, noSources, sineVoicing(), e, length);
    };

    std::cout << "\n## 1. The knobs under modulation\n\n"
              << "A1 a sine at A3, filters bypassed. A route from the wheel, left at 0, puts the slot on the "
                 "modulated path with a sum of exactly 0, so any difference from no route is the knob's.\n\n"
              << "### 1a. A knob step while modulated (click, dB re a unit step)\n\n"
              << "| Knob step at 0.5 s | no route (the slot's own gain ramps) | (a) the targets, unramped | (b) "
                 "the slot's own ramps | (c) global ramps |\n|---|---:|---:|---:|---:|\n";
    for (const bool isPan : {false, true})
    {
        const Dest d = isPan ? Dest::panA : Dest::volumeA;
        const auto knob = isPan ? panKnob(at, 0, 10.0f) : volumeKnob(at, 0, 2.0f);
        std::cout << "| " << (isPan ? "Pan 5 -> 10" : "Volume 10 -> 2") << " | "
                  << dbText(clickDb(render(KnobSource::slotRamps, {knob}), at));
        for (const auto k : knobSources)
        {
            std::cout << " | " << dbText(clickDb(render(k, {knob, silentRoute(0, d)}), at));
        }
        std::cout << " |\n";
    }

    std::cout << "\n### 1b. A route appearing or going while the knob ramps\n\n"
              << "The knob moves 3 ms before the grid point at which the route appears (or goes). The step is "
                 "the largest difference from no route at all over the next 50 ms, in dB re the sine's peak.\n\n"
              << "| Case | (a) the targets, unramped | (b) the slot's own ramps | (c) global ramps "
                 "|\n|---|---:|---:|---:|\n";
    for (const bool isPan : {false, true})
    {
        for (const bool appears : {true, false})
        {
            const Dest d = isPan ? Dest::panA : Dest::volumeA;
            const auto knob = isPan ? panKnob(at - 144, 0, 10.0f) : volumeKnob(at - 144, 0, 2.0f);
            const auto reference = render(KnobSource::slotRamps, {knob});
            const double peak = peakOf(reference, note + 4800, 4800);
            std::cout << "| " << (isPan ? "Pan 5 -> 10" : "Volume 10 -> 2") << ", the route "
                      << (appears ? "appears" : "goes") << " |";
            for (const auto k : knobSources)
            {
                std::vector<DestEvent> e{knob};
                if (appears)
                {
                    e.push_back(silentRoute(at - 10, d));
                }
                else
                {
                    e.push_back(silentRoute(0, d));
                    e.push_back(route(at - 10, 8, Source::none, Dest::none, 0.0f));
                }
                const auto diff = difference(render(k, e), reference);
                std::cout << " " << dbText(toDb(peakOf(diff, at, 2400) / peak)) << " |";
            }
            std::cout << "\n";
        }
    }

    std::cout << "\n### 1c. A note on a voice that was idle while the knob moved\n\n"
              << "Volume 10 -> 2 (or Pan 5 -> 10) at 0.5 s with no note sounding; a note 2 ms later, with a "
                 "silent route from the start, against the same with no route: the largest difference over the "
                 "note's first 20 ms, dB re its peak.\n\n"
              << "| Case | (a) the targets, unramped | (b) the slot's own ramps | (c) global ramps "
                 "|\n|---|---:|---:|---:|\n";
    for (const bool isPan : {false, true})
    {
        const Dest d = isPan ? Dest::panA : Dest::volumeA;
        const auto knob = isPan ? panKnob(at, 0, 10.0f) : volumeKnob(at, 0, 2.0f);
        const auto renderLate = [&](KnobSource k, bool withRoute)
        {
            std::vector<DestEvent> e{knob, noteOn(at + 96, 57)};
            if (withRoute)
            {
                e.push_back(silentRoute(0, d));
            }
            return renderDest({.knobs = k}, noSources, sineVoicing(), e, length);
        };
        const auto reference = renderLate(KnobSource::slotRamps, false);
        const double peak = peakOf(reference, at + 96, 960);
        std::cout << "| " << (isPan ? "Pan 5 -> 10" : "Volume 10 -> 2") << " |";
        for (const auto k : knobSources)
        {
            const auto diff = difference(renderLate(k, true), reference);
            std::cout << " " << dbText(toDb(peakOf(diff, at + 96, 960) / peak)) << " |";
        }
        std::cout << "\n";
    }
}

// --- 2. A change of the routes ------------------------------------------------------------

struct AmountCandidate
{
    std::string_view name;
    DestinationSettings settings;
};

const std::array<AmountCandidate, 3> amountCandidates{
    {
        {.name = "(a) step", .settings = {.amounts = AmountChange::step}},
        {.name = "(b) crossfade, 1 segment", .settings = {.amounts = AmountChange::crossfade, .fadeSegments = 1}},
        {
            .name = "(c) crossfade, 15 segments (10 ms)",
            .settings = {.amounts = AmountChange::crossfade, .fadeSegments = 15},
        },
    },
};

void printAmounts()
{
    const std::size_t note = 2400;
    const std::size_t at = 24010; // mid-segment: taken at the grid point 24032
    const std::size_t grid = 24032;
    const std::size_t length = 48000;

    struct Case
    {
        std::string_view name;
        Dest destination;
        float amount;
        bool saw;
    };
    const std::array<Case, 6> cases{
        {
            {.name = "Mix A -5", .destination = Dest::mixA, .amount = -5.0f, .saw = false},
            {.name = "Volume A -5", .destination = Dest::volumeA, .amount = -5.0f, .saw = false},
            {.name = "Pan A +5", .destination = Dest::panA, .amount = 5.0f, .saw = false},
            {.name = "Pitch A +0.10 (1.2 semitones)", .destination = Dest::pitchA, .amount = 0.1f, .saw = false},
            {.name = "Phase A +2.5 (90 degrees)", .destination = Dest::phaseA, .amount = 2.5f, .saw = false},
            {
                .name = "Cutoff A +3 (the sine near the cutoff, LP24)",
                .destination = Dest::cutoffA,
                .amount = 3.0f,
                .saw = true,
            },
        },
    };
    std::cout << "\n## 2. A change of the routes\n\n"
              << "### 2a. A route appearing on a held note\n\n"
              << "Velocity 127 (a source of 1) to the destination, set mid-segment at 0.5 s. Click: dB re a "
                 "unit step at the grid point that takes it.\n\n"
              << "| Route |";
    for (const auto& c : amountCandidates)
    {
        std::cout << " " << c.name << " |";
    }
    std::cout << "\n|---|---:|---:|---:|\n";
    for (const auto& c : cases)
    {
        std::cout << "| " << c.name << " |";
        for (const auto& candidate : amountCandidates)
        {
            auto voicing = sineVoicing();
            if (c.saw)
            {
                // Cutoff A 0 plus the Filter Control's 5.6: 221 Hz, at the
                // sine; +3 takes it to 1,770 Hz.
                voicing.base.bypass = false;
                voicing.base.cutoff = 0.0f;
                voicing.base.controlCutoff = 5.6f;
            }
            const std::vector<DestEvent> e{noteOn(note, 57), route(at, 8, Source::velocity, c.destination, c.amount)};
            std::cout << " " << dbText(clickDb(renderDest(candidate.settings, noSources, voicing, e, length), grid))
                      << " |";
        }
        std::cout << "\n";
    }

    std::cout << "\n### 2b. An amount automated in 7-bit steps\n\n"
              << "Velocity 127 on Mix A (or Volume A), the amount from 0 to -10 in 64 steps of 20/127, one "
                 "every 3 ms (a fast turn: 0.19 s). Zipper: the click measure averaged over 8 points of the "
                 "sweep. Lag: from the last step to the sum's arrival.\n\n"
              << "| Destination |";
    for (const auto& c : amountCandidates)
    {
        std::cout << " " << c.name << " |";
    }
    std::cout << "\n|---|---:|---:|---:|\n";
    for (const Dest d : {Dest::mixA, Dest::volumeA})
    {
        std::cout << "| " << (d == Dest::mixA ? "Mix A" : "Volume A") << " |";
        for (const auto& candidate : amountCandidates)
        {
            std::vector<DestEvent> e{noteOn(note, 57)};
            const std::size_t start = 12007; // the steps mid-segment
            for (int k = 1; k <= 64; ++k)
            {
                const float amount = -10.0f * std::min(1.0f, static_cast<float>(k) * (20.0f / 127.0f) / 10.0f);
                e.push_back(route(start + (static_cast<std::size_t>(k) * 144), 8, Source::velocity, d, amount));
            }
            const auto out = renderDest(candidate.settings, noSources, sineVoicing(), e, length);
            std::array<std::size_t, 8> points{};
            for (std::size_t k = 0; k < points.size(); ++k)
            {
                points[k] = start + 600 + (k * 1100);
            }
            std::cout << " " << dbText(clickDb(out, points)) << " |";
        }
        std::cout << "\n";
    }
    // The lag: an isolated change waits for the next grid point (1-32
    // samples), then takes its fade. In the sweep, Mix A reaches -10 from a
    // source of 1, Mix's 0, and the output is exactly 0 once it arrives.
    std::cout << "| Lag of an isolated change (samples at 48 kHz) |";
    for (const auto& candidate : amountCandidates)
    {
        const auto& c = candidate.settings;
        const std::size_t fade = c.amounts == AmountChange::step ? 0 : c.fadeSegments * 32;
        std::cout << " " << (fade == 0 ? std::string{"1-32"} : "1-32 + " + std::to_string(fade)) << " |";
    }
    std::cout << "\n| Lag of the sweep's last step, Mix A (samples) |";
    for (const auto& candidate : amountCandidates)
    {
        std::vector<DestEvent> e{noteOn(note, 57)};
        const std::size_t start = 12007; // the steps mid-segment
        for (int k = 1; k <= 64; ++k)
        {
            const float amount = -10.0f * std::min(1.0f, static_cast<float>(k) * (20.0f / 127.0f) / 10.0f);
            e.push_back(route(start + (static_cast<std::size_t>(k) * 144), 8, Source::velocity, Dest::mixA, amount));
        }
        const auto out = renderDest(candidate.settings, noSources, sineVoicing(), e, length);
        const std::size_t last = start + (std::size_t{64} * 144);
        std::size_t silentFrom = length;
        const auto zero = [](float x)
        {
            return x >= 0.0f && x <= 0.0f;
        };
        while (silentFrom > 0 && zero(out.channel(0)[silentFrom - 1]) && zero(out.channel(1)[silentFrom - 1]))
        {
            --silentFrom;
        }
        std::cout << " " << (silentFrom > last ? std::to_string(silentFrom - last) : std::string{"0"}) << " |";
    }
    std::cout << "\n";
}

// --- 3. Noise -----------------------------------------------------------------------------

void printNoise()
{
    std::cout << "\n## 3. Volume and Pan routes on a Noise slot\n\n"
              << "A1 Noise, 4 unison voices at Stereo 0.5, filters bypassed. The wheel at 127, so a route adds "
                 "its amount; against the knob turned there with no route. Left and right RMS over 0.5 s, dB re "
                 "the knob's.\n\n"
              << "| Case | (a) ignored: left / right | (b) mixed: left / right |\n|---|---|---|\n";
    for (const bool isPan : {false, true})
    {
        auto voicing = sineVoicing();
        voicing.base.waves[0] = dsp::Waveform::noise;
        voicing.base.voices[0] = 4;
        voicing.stereo[0] = 0.5f;
        auto reference = voicing;
        if (isPan)
        {
            voicing.pan[0] = 5.0f;
            reference.pan[0] = 10.0f;
        }
        else
        {
            voicing.volume[0] = 0.0f;
            reference.volume[0] = 5.0f;
        }
        const std::vector<DestEvent> base{wheel(0, 127), noteOn(2400, 57)};
        const auto knob = renderDest(defaultSettings, noSources, reference, base, 36000);
        auto e = base;
        e.push_back(route(0, 8, Source::modWheel, isPan ? Dest::panA : Dest::volumeA, 5.0f));
        std::cout << "| " << (isPan ? "Pan 5 + 5 against Pan 10" : "Volume 0 + 5 against Volume 5") << " |";
        for (const auto n : {NoiseModulation::ignored, NoiseModulation::mixed})
        {
            const auto out = renderDest({.noise = n}, noSources, voicing, e, 36000);
            const auto level = [&](std::size_t c)
            {
                const double db = toDb(rmsOf(out, 12000, 24000, c) / rmsOf(knob, 12000, 24000, c));
                return std::isfinite(db) ? fixed(db + 0.0, 2) : std::string{"silent"};
            };
            std::cout << " " << level(0) << " / " << level(1) << " |";
        }
        std::cout << "\n";
    }
}

// --- 4. The Volume skip test --------------------------------------------------------------

/// A slot the panel gives the source: Mod Env 1's first, LFO 1's first, or
/// the first misc slot.
std::size_t slotOf(Source s)
{
    if (s == Source::modEnv1)
    {
        return 0;
    }
    return s == Source::lfo1 ? 4 : 8;
}

void printSkip()
{
    std::cout << "\n## 4. The Volume skip test\n\n"
              << "A1 and B1 saws, own filters; B1 at Volume 0, one route lifting it; 16 notes of 0.2 s, one "
                 "every 0.25 s, releases of 50 ms. Skipped: B1's grid points skipped (silent, or its part not "
                 "taken), of those at which it sounded. Accuracy: the bound's render against never skipping, dB re "
                 "the render.\n\n"
              << "| Route on Volume B | never: skipped | the routes' bound: skipped | the bound against never "
                 "|\n|---|---:|---:|---:|\n";
    struct Case
    {
        std::string_view name;
        Source source;
        float amount;
        proto::SourceSettings settings;
    };
    proto::SourceSettings sine;
    sine.lfos[0] = {.waveform = dsp::LfoWaveform::sine, .rate = 6.34f, .gain = 10.0f};
    proto::SourceSettings pulse;
    pulse.lfos[0] = {.waveform = dsp::LfoWaveform::pulse, .rate = 6.34f, .gain = 10.0f, .offset = -10.0f};
    proto::SourceSettings pluck;
    pluck.modEnvelopes[0] = {.attack = 0.0f, .decay = 0.05f, .sustain = 0.0f, .release = 0.05f};
    const std::array<Case, 4> cases{
        {
            {.name = "LFO 1 sine 8 Hz, +10 (+-5 units)", .source = Source::lfo1, .amount = 10.0f, .settings = sine},
            {
                .name = "LFO 1 pulse 8 Hz, Offset -10, at -10 (0 to +10)",
                .source = Source::lfo1,
                .amount = -10.0f,
                .settings = pulse,
            },
            {
                .name = "Mod Env 1 pluck (decay 50 ms, sustain 0), +10",
                .source = Source::modEnv1,
                .amount = 10.0f,
                .settings = pluck,
            },
            {.name = "Velocity (1 to 127), +10", .source = Source::velocity, .amount = 10.0f, .settings = {}},
        },
    };
    DestVoicing d;
    d.base.voices = {1, 0, 1, 0};
    d.base.release = {0.05f, 0.05f};
    d.base.polyphony = 16;
    d.volume[2] = 0.0f;
    for (const auto& c : cases)
    {
        std::vector<DestEvent> e{
            route(0, slotOf(c.source), c.source, Dest::volumeB, c.amount),
        };
        for (int k = 0; k < 16; ++k)
        {
            const auto t = 2400 + (static_cast<std::size_t>(k) * 12000);
            e.push_back(noteOn(t, 45 + k, 1 + ((k * 37) % 127)));
            e.push_back(noteOff(t + 9600, 45 + k));
        }
        const auto run = [&](VolumeSkip rule, double& share)
        {
            std::uint64_t tests = 0;
            std::uint64_t skips = 0;
            const auto out = renderDest({.volumeSkip = rule}, c.settings, d, e, (16 * 12000) + 9600,
                                        {
                                            .after =
                                                [&](DestEngine& engine, std::size_t end)
                                            {
                                                if (end != (16 * 12000) + 9600)
                                                {
                                                    return;
                                                }
                                                engine.voices().forEachVoice(
                                                    [&](auto& v)
                                                    {
                                                        tests += v.slotSkipTests(2);
                                                        skips += v.slotSkips(2);
                                                    });
                                            },
                                        });
            share = tests > 0 ? 100.0 * static_cast<double>(skips) / static_cast<double>(tests) : 0.0;
            return out;
        };
        double never = 0.0;
        double bound = 0.0;
        const auto a = run(VolumeSkip::never, never);
        const auto b = run(VolumeSkip::bound, bound);
        const auto n = a.numSamples();
        std::cout << "| " << c.name << " | " << fixed(never, 1) << "% | " << fixed(bound, 1) << "% | "
                  << dbText(toDb(rmsOf(difference(b, a), 0, n) / rmsOf(a, 0, n))) << " |\n";
    }
}

// --- 5. Phase: a step folded into one sample's phase step ------------------------------------

void printPhaseStep()
{
    using Slot = DestSlot<64>;
    const dsp::ProcessSpec spec{.sampleRate = rate, .maxBlockSize = 4800, .numChannels = 2};
    const float note = 69.0f + (12.0f * std::log2(1090.0f / 440.0f)); // off any simple ratio to 48 kHz
    const std::size_t at = 2400;
    const std::size_t length = 4800;
    std::cout << "\n## 5. Phase: an offset's step\n\n"
              << "One saw at 1,090 Hz. The offset steps at one sample: folded into that sample's phase step "
                 "(the decision), or made as a band-limited jump (a retrigger to the same phase, as the "
                 "decision does where the step would leave [0, Nyquist)). Their difference's energy in a Hann "
                 "1024 centred on the step, below and above 16 kHz, dB re the jump's energy there.\n\n"
              << "| Step | folded against jump: below 16 kHz | above 16 kHz | made as a jump? |\n"
              << "|---|---:|---:|---|\n";
    for (const float degrees : {45.0f, 90.0f, 170.0f})
    {
        Buffer folded{2, length};
        Buffer jumped{2, length};
        bool asJump = false;
        {
            Slot s{proto::table()};
            s.prepare(spec);
            s.setWaveform(dsp::Waveform::saw);
            s.noteOn();
            std::vector<float> phase(length, 0.0f);
            std::fill(phase.begin() + static_cast<std::ptrdiff_t>(at), phase.end(), degrees / 36.0f);
            const auto before = s.phaseJumps();
            for (std::size_t start = 0; start < length; start += 32)
            {
                const SlotModulation m{
                    .position = 0,
                    .segment = 32,
                    .phase = std::span<const float>{phase}.subspan(start, 32),
                };
                s.process(folded.block().subBlock(start, 32), note, m);
            }
            asJump = s.phaseJumps() > before;
        }
        {
            Slot s{proto::table()};
            s.prepare(spec);
            s.setWaveform(dsp::Waveform::saw);
            s.noteOn();
            s.process(jumped.block().subBlock(0, at), note);
            const double now = static_cast<double>(s.oscillator(0).nextPhase()) * 360.0 / 4294967296.0;
            s.setPhaseDegrees(static_cast<float>(now + degrees));
            s.noteOn();
            s.process(jumped.block().subBlock(at, length - at), note);
        }
        // The difference's energy in a Hann 1024 centred on the step, split
        // at 16 kHz, re the jump's own energy in the same window.
        const auto diff = difference(folded, jumped);
        const auto window = anoesis::test::hannWindow(1024);
        const auto d = anoesis::test::powerSpectrum(diff.channel(0).subspan(at - 512, 1024), rate, window);
        const auto j = anoesis::test::powerSpectrum(jumped.channel(0).subspan(at - 512, 1024), rate, window);
        double low = 0.0;
        double high = 0.0;
        double total = 0.0;
        for (std::size_t k = 0; k < d.power.size(); ++k)
        {
            (d.frequencyOf(k) < 16000.0 ? low : high) += d.power[k];
            total += j.power[k];
        }
        std::cout << "| " << fixed(degrees, 0) << " degrees | " << dbText(10.0 * std::log10(low / total)) << " | "
                  << dbText(10.0 * std::log10(high / total)) << " | " << (asJump ? "yes" : "no") << " |\n";
    }
}

// --- 6. Pitch at its extremes ---------------------------------------------------------------

void printPitchExtremes()
{
    std::cout << "\n## 6. Pitch at its extremes\n\n"
              << "A1 a saw, LP24 at the init cutoff; 16 routes at +10 (or -10) from sources at 1: +-160 octaves "
                 "on the note.\n\n"
              << "| Routes | every sample finite | peak after 50 ms |\n|---|---|---:|\n";
    for (const float amount : {10.0f, -10.0f})
    {
        std::vector<DestEvent> e{wheel(0, 127), noteOn(2400, 57)};
        for (std::size_t slot = 0; slot < 16; ++slot)
        {
            e.push_back(route(0, slot, slot < 8 ? Source::velocity : Source::modWheel, Dest::pitchAB, amount));
        }
        auto d = sineVoicing();
        d.base.waves[0] = dsp::Waveform::saw;
        d.base.bypass = false;
        const auto out = renderDest(defaultSettings, noSources, d, e, 24000);
        bool finite = true;
        for (std::size_t c = 0; c < 2; ++c)
        {
            for (const float x : out.channel(c))
            {
                finite = finite && std::isfinite(x);
            }
        }
        std::cout << "| 16 x " << (amount > 0.0f ? "+10" : "-10") << " | " << (finite ? "yes" : "no") << " | "
                  << fixed(peakOf(out, 4800, 19200), 4) << " |\n";
    }
}

} // namespace

TEST_CASE("Destinations report", "[.][destinations-report]")
{
    printPrototype();
    printKnobs();
    printAmounts();
    printNoise();
    printSkip();
    printPhaseStep();
    printPitchExtremes();
    SUCCEED();
}
