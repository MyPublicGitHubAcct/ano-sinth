// The voice report (docs/dsp/voice-architecture.md, M1.4 task 1, stage 1):
// measures the candidate voice structures (voice_candidates.hpp) on the real
// DSP classes (OscillatorSlot, LadderFilter, Adsr, PitchDrift), the filters
// fed through the Filter Control as decided (filter_control_support.hpp):
//
// - question 1, what each reading of the Input Select does: a part's
//   oscillators crossing into the other filter under each envelope reading,
//   unselected oscillators, "--", and Bypass;
// - question 2, the level into the filter: the peak and RMS that 1 saw, 2
//   saws, AB, 8-voice unison, and the worst case present at a filter's
//   input, in phase and free-running, under each AB summing law; and the
//   Drive at which the filter's own saturation reaches 1% distortion, Warm
//   on and off;
// - the engines (per-part or per-filter buffers, the grid or the block
//   schedule, the mono path): the same output to the bit for several
//   patches and routings at host blocks of 1, 37 and 4096, with no
//   allocation; the mono path's left and right identical;
// - each engine's memory per voice.
//
// Hidden from the default run; run it in an optimised build (about 20 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[voice-report]"
//
// CPU: the "[voice]" benchmarks (tools/bench.sh "[voice]").

#include "voice_candidates.hpp"

#include "../oscillators/oscillator_support.hpp"
#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/window.hpp>

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
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::voice;
namespace dsp = anoesis::dsp;
using filter_control::Knobs;

namespace
{

constexpr float rate = 48000.0f;
using Voice = PrototypeVoice<dsp::minBlepTaps>;
using Slot = dsp::OscillatorSlot<dsp::minBlepTaps>;
const Engine defaultEngine;
const Readings developerReadings;

std::string fixed(double x, int digits)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

double dbOf(double amplitude)
{
    return 20.0 * std::log10(amplitude);
}

std::string dbText(double amplitude)
{
    if (amplitude <= 0.0)
    {
        return "silent (exact 0)";
    }
    std::ostringstream s;
    s << std::showpos << std::fixed << std::setprecision(1) << dbOf(amplitude) << " dB";
    return s.str();
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

double peakOf(std::span<const float> x)
{
    double peak = 0.0;
    for (const float v : x)
    {
        peak = std::max(peak, static_cast<double>(std::abs(v)));
    }
    return peak;
}

std::size_t samples(double seconds)
{
    return static_cast<std::size_t>(seconds * static_cast<double>(rate));
}

// --- Rendering a voice -------------------------------------------------------------

struct Render
{
    Buffer out{2, 1};
    std::size_t allocations = 0;
    bool mono = false;
};

/// One voice from a note-on at sample 0, a note-off at `noteOff`, in host
/// blocks of `hostBlock`, split at the note-off; the knobs constant (the
/// Filter Control's defaults: both cutoffs at 5, resonance 0).
Render renderVoice(const Readings& readings, const Engine& engine, const std::function<void(Voice&)>& configure,
                   std::size_t length, std::size_t noteOff, std::size_t hostBlock, float note = 45.0f)
{
    Voice v{d6::minBlepTable64(), readings, engine};
    configure(v);
    v.prepare({.sampleRate = rate, .maxBlockSize = hostBlock, .numChannels = 1});
    const Knobs knobs;
    KnobRamps rampsA{knobs, rate};
    KnobRamps rampsB{knobs, rate};
    const FilterFeed feed{.a = &rampsA, .b = &rampsB, .keytrack = 0.0f};
    Render r;
    r.out = Buffer{2, length};
    const auto block = r.out.block();
    v.noteOn(note, feed, 0);
    r.mono = v.isMono();
    {
        const AllocationCounter counter;
        for (std::size_t start = 0; start < length; start += hostBlock)
        {
            const auto end = std::min(length, start + hostBlock);
            for (std::size_t from = start; from < end;)
            {
                const auto to = (from < noteOff && noteOff < end) ? noteOff : end;
                if (from == noteOff)
                {
                    v.noteOff();
                }
                v.render(block.subBlock(from, to - from), feed, from);
                rampsA.advance(to - from);
                rampsB.advance(to - from);
                from = to;
            }
        }
        r.allocations = counter.count();
    }
    return r;
}

void setEnvelope(Voice& v, std::size_t part, float attack, float decay, float sustain, float release)
{
    auto& e = v.envelope(part);
    e.setAttack(attack);
    e.setDecay(decay);
    e.setSustain(sustain);
    e.setRelease(release);
}

// --- Question 1: what each reading does ----------------------------------------------

struct Scenario
{
    std::string_view name;
    Input inputA;
    Input inputB;
    bool bypassB;
    float decayB; ///< amp envelope B's decay to a sustain of 0 (0: a gate, S 10)
};

void printReadings()
{
    std::cout << "\n## Question 1: what each reading does\n\n"
              << "Part B only: B1 a saw at A2 (110 Hz), the other oscillators off. Amp envelope A a gate (A 0, D 0, "
                 "S 10, R 0); amp envelope B a gate, or a 100 ms decay to S 0 where stated. Filters LP24 at the "
                 "Filter Control's defaults (4,677 Hz). The output's RMS (left channel) over 10-50 ms (early) and "
                 "200-300 ms (late), in dBFS.\n\n"
              << "| Patch | Reading | Early | Late |\n|---|---|---:|---:|\n";

    const std::array scenarios{
        Scenario{
            .name = "A: AB, B: --, env B decays in 100 ms",
            .inputA = Input::both,
            .inputB = Input::none,
            .bypassB = false,
            .decayB = 0.1f,
        },
        Scenario{
            .name = "A: A, B: -- (part B unselected)",
            .inputA = Input::own,
            .inputB = Input::none,
            .bypassB = false,
            .decayB = 0.0f,
        },
        Scenario{
            .name = "A: A, B: -- and Bypass",
            .inputA = Input::own,
            .inputB = Input::none,
            .bypassB = true,
            .decayB = 0.0f,
        },
        Scenario{
            .name = "A: A, B: B and Bypass",
            .inputA = Input::own,
            .inputB = Input::own,
            .bypassB = true,
            .decayB = 0.0f,
        },
        Scenario{
            .name = "A: --, B: -- (nothing selected)",
            .inputA = Input::none,
            .inputB = Input::none,
            .bypassB = false,
            .decayB = 0.0f,
        },
    };
    struct Variant
    {
        std::string_view name;
        Readings readings;
    };
    const Readings developer;
    const Readings i1{
        .cross = CrossEnvelope::filterPartOnly,
    };
    const Readings u2{
        .unselected = Unselected::unfiltered,
    };
    const Readings ownPart{
        .bypass = BypassInput::ownPart,
    };
    const auto pair = [](std::string_view a, const Readings& ra, std::string_view b, const Readings& rb)
    {
        return std::array{
            Variant{
                .name = a,
                .readings = ra,
            },
            Variant{
                .name = b,
                .readings = rb,
            },
        };
    };
    const std::array variants{
        pair("I1 filter's part's envelope only", i1, "I2 source's envelope, then the filter's (developer)", developer),
        pair("U1 silent", developer, "U2 heard unfiltered", u2),
        pair("bypass obeys the select", developer, "bypass passes its own part", ownPart),
        pair("bypass obeys the select", developer, "bypass passes its own part", ownPart),
        pair("U1 silent", developer, "U2 heard unfiltered", u2),
    };
    const auto length = samples(0.3);
    for (std::size_t i = 0; i < scenarios.size(); ++i)
    {
        const auto& s = scenarios[i];
        for (const auto& variant : variants[i])
        {
            const auto r = renderVoice(
                variant.readings, defaultEngine,
                [&s](Voice& v)
                {
                    setInitPatch(v);
                    v.slot(0).setVoices(0);
                    v.slot(2).setVoices(1);
                    setEnvelope(v, 1, 0.0f, s.decayB, s.decayB > 0.0f ? 0.0f : 1.0f, 0.0f);
                    Patch patch;
                    patch.filters[0].input = s.inputA;
                    patch.filters[1].input = s.inputB;
                    patch.filters[1].bypassed = s.bypassB;
                    v.setPatch(patch);
                },
                length, length, 512);
            const auto left = r.out.channel(0);
            std::cout << "| " << s.name << " | " << variant.name << " | "
                      << dbText(rmsOf(left.subspan(samples(0.01), samples(0.04)))) << " | "
                      << dbText(rmsOf(left.subspan(samples(0.2), samples(0.1)))) << " |\n";
        }
    }
}

// --- Question 2: the level into the filter -------------------------------------------

struct LevelCase
{
    std::string_view name;
    std::array<int, 4> voices; ///< unison voices of A1, A2, B1, B2 (0: off)
    bool ab;                   ///< both parts into the filter
};

constexpr std::array levelCases{
    LevelCase{
        .name = "1 saw (A1)",
        .voices = {1, 0, 0, 0},
        .ab = false,
    },
    LevelCase{
        .name = "2 saws in one part (A1 + A2)",
        .voices = {1, 1, 0, 0},
        .ab = false,
    },
    LevelCase{
        .name = "AB, 1 saw per part (A1 + B1)",
        .voices = {1, 0, 1, 0},
        .ab = true,
    },
    LevelCase{
        .name = "AB, 2 saws per part",
        .voices = {1, 1, 1, 1},
        .ab = true,
    },
    LevelCase{
        .name = "8-voice unison (A1)",
        .voices = {8, 0, 0, 0},
        .ab = false,
    },
    LevelCase{
        .name = "AB, 4 oscillators x 8 unison",
        .voices = {8, 8, 8, 8},
        .ab = true,
    },
};

/// The filter's input for a case: each part's slots added into its buffer
/// (centred, so channel 0 is the whole story), the parts summed by `law`
/// where AB. In phase: Retrig on, Phase 0, no detune, the same note. Free:
/// Retrig off (seeded random phases), Detune 5 (0.548 semitones at 8
/// voices), and the oscillators a few cents apart.
std::vector<float> filterInput(const LevelCase& c, bool inPhase, AbLaw law, std::size_t length)
{
    std::array<Buffer, partCount> parts{Buffer{2, length}, Buffer{2, length}};
    constexpr std::array fine{0.0f, 0.07f, -0.05f, 0.03f};
    for (std::size_t s = 0; s < 4; ++s)
    {
        if (c.voices[s] == 0)
        {
            continue;
        }
        Slot slot{d6::minBlepTable64()};
        slot.setRandomSeed(0x9E3779B97F4A7C15U * (s + 1));
        slot.setVoices(c.voices[s]);
        slot.setWaveform(dsp::Waveform::saw);
        slot.setRetrigger(inPhase);
        slot.setDetune(inPhase ? 0.0f : 0.548f);
        slot.setTuning({.octaves = 0, .semitones = 0, .fine = inPhase ? 0.0f : fine[s]});
        slot.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
        slot.noteOn();
        const auto block = parts[s / 2].block();
        for (std::size_t start = 0; start < length; start += 512)
        {
            slot.process(block.subBlock(start, 512), 45.0f);
        }
    }
    const float g = c.ab ? abGain(law) : 1.0f;
    std::vector<float> x(length);
    const auto a = parts[0].channel(0);
    const auto b = parts[1].channel(0);
    for (std::size_t i = 0; i < length; ++i)
    {
        x[i] = c.ab ? (g * a[i]) + (g * b[i]) : a[i];
    }
    return x;
}

/// LP24 at the Filter Control's defaults (both cutoffs at 5: 4,677 Hz), k 0,
/// fed as the voice feeds it, at a drive gain, from rest.
std::vector<float> filtered(std::span<const float> x, float drive, bool warm)
{
    dsp::LadderFilter f;
    f.setWarm(warm);
    f.setDrive(drive);
    f.prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 1});
    filter_control::feedFilter(f, {.cutoff = 5.0f, .controlCutoff = 5.0f}, {});
    f.reset();
    std::vector<float> y(x.begin(), x.end());
    const std::array<float*, 1> channels{y.data()};
    const dsp::AudioBlock block{channels.data(), 1, y.size()};
    for (std::size_t start = 0; start < y.size(); start += 4096)
    {
        f.process(block.subBlock(start, 4096));
    }
    return y;
}

/// The filter's nonlinear distortion at a drive: the RMS of the output less
/// the linear filter's (the same filter at -60 dB, scaled back), over the
/// linear output's RMS, from 100 ms on. At 1e-3 of full scale tanh and the
/// cubic are linear to 3e-7.
class Distortion
{
public:
    Distortion(std::span<const float> input, bool warm) : x(input.begin(), input.end()), warm(warm)
    {
        const double scale = 1e-3 / std::max(peakOf(x), 1e-9);
        std::vector<float> small(x.size());
        std::ranges::transform(x, small.begin(), [scale](float v) { return static_cast<float>(v * scale); });
        const auto y = filtered(small, 1.0f, warm);
        linear.resize(y.size());
        std::ranges::transform(y, linear.begin(), [scale](float v) { return static_cast<double>(v) / scale; });
    }

    [[nodiscard]] double at(float drive) const
    {
        const auto y = filtered(x, drive, warm);
        double error = 0.0;
        double power = 0.0;
        for (std::size_t i = samples(0.1); i < y.size(); ++i)
        {
            const double reference = static_cast<double>(drive) * linear[i];
            error += (y[i] - reference) * (y[i] - reference);
            power += reference * reference;
        }
        return std::sqrt(error / power);
    }

    /// The drive gain (in dB) at which the distortion reaches `target`, by
    /// bisection in dB between -36 and +60; NaN outside.
    [[nodiscard]] double driveDbFor(double target) const
    {
        double lo = -36.0;
        double hi = 60.0;
        const auto gain = [](double db)
        {
            return static_cast<float>(std::pow(10.0, db / 20.0));
        };
        if (at(gain(lo)) > target)
        {
            return -std::numeric_limits<double>::infinity();
        }
        if (at(gain(hi)) < target)
        {
            return std::numeric_limits<double>::infinity();
        }
        for (int i = 0; i < 20; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            (at(gain(mid)) < target ? lo : hi) = mid;
        }
        return 0.5 * (lo + hi);
    }

private:
    std::vector<float> x;
    std::vector<double> linear;
    bool warm;
};

std::string_view lawName(AbLaw law)
{
    switch (law)
    {
    case AbLaw::plainSum:
        return "plain sum";
    case AbLaw::half:
        return "0.5 each";
    case AbLaw::equalPower:
        return "1/sqrt 2 each";
    }
    return "?";
}

std::string driveText(double db)
{
    if (std::isinf(db))
    {
        return db < 0.0 ? "below -36 dB" : "above +60 dB";
    }
    // F8's placeholder law: 0 to +24 dB, dB-linear over the knob's 0-10.
    std::ostringstream s;
    s << std::showpos << std::fixed << std::setprecision(1) << db << " dB" << std::noshowpos << " (knob "
      << (db < 0.0 ? std::string{"< 0"} : fixed(db / 2.4, 1)) << ")";
    return s.str();
}

void printLevels()
{
    std::cout << "\n## Question 2: the level into the filter\n\n"
              << "A2 (110 Hz) saws at Volume 10 (unity), centred (each channel 0.7071 of the mono sum), 48 kHz. In "
                 "phase: Retrig on, Phase 0, Detune 0. Free: Retrig off, Detune 5 (0.548 semitones at 8 voices), "
                 "the oscillators 0, +7, -5, +3 cents. Peak and RMS of one channel over 1 s. Then the filter (LP24 "
                 "at 4,677 Hz, k 0): its distortion at drive x1 (knob 0), and the drive at which it reaches 1% "
                 "(-40 dB; the knob under F8's placeholder, 0 to +24 dB).\n\n"
              << "| Input | AB law | Phase | Peak | RMS | Warm on: at x1 | Warm on: 1% at | Warm off: at x1 | "
                 "Warm off: 1% at |\n|---|---|---|---:|---:|---:|---:|---:|---:|\n";
    const auto length = samples(1.1);
    for (const auto& c : levelCases)
    {
        const std::vector<AbLaw> laws =
            c.ab ? std::vector{AbLaw::plainSum, AbLaw::half, AbLaw::equalPower} : std::vector{AbLaw::plainSum};
        for (const auto law : laws)
        {
            for (const bool inPhase : {true, false})
            {
                const auto x = filterInput(c, inPhase, law, length);
                const std::span<const float> steady{x.data() + samples(0.1), samples(1.0)};
                std::cout << "| " << c.name << " | " << (c.ab ? lawName(law) : std::string_view{"-"}) << " | "
                          << (inPhase ? "in phase" : "free") << " | " << fixed(peakOf(steady), 2) << " ("
                          << dbText(peakOf(steady)) << ") | " << fixed(rmsOf(steady), 3) << " ("
                          << dbText(rmsOf(steady)) << ")";
                for (const bool warm : {true, false})
                {
                    const std::span<const float> head{x.data(), samples(0.6)};
                    const Distortion d{head, warm};
                    std::cout << " | " << fixed(100.0 * d.at(1.0f), 2) << "% | " << driveText(d.driveDbFor(0.01));
                }
                std::cout << " |\n";
            }
        }
    }
}

/// The knee: a sine's THD through the filter against the oscillator's
/// Volume, at drive x1, and the Volume where it reaches 1%. The same
/// measurement on the demo gives Sylenth1's internal level against ours: the
/// ratio of the two Volumes at 1% is the scale between them, if the
/// saturators share a shape (F9).
void printKnee()
{
    std::cout
        << "\n## The knee: a sine through the filter against Volume\n\n"
        << "A1 a sine at 220 Hz, one voice, centred; LP24 at 4,677 Hz, k 0, drive x1 (knob 0). THD (harmonics "
           "2-10) of the filter's output from 100 ms, 2^15 samples, Blackman-Harris. Volume as a linear gain "
           "(knob / 10, O4).\n\n| Volume knob | Peak at the filter | Warm on | Warm off |\n|---:|---:|---:|---:|\n";
    const auto length = samples(0.1) + 32768;
    const auto window = blackmanHarrisWindow(32768);
    const auto thdAt = [&](float volume, bool warm)
    {
        Slot slot{d6::minBlepTable64()};
        slot.setWaveform(dsp::Waveform::sine);
        slot.setVolume(volume);
        slot.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 2});
        slot.noteOn();
        Buffer part{2, length};
        for (std::size_t start = 0; start < length; start += 512)
        {
            slot.process(part.block().subBlock(start, 512), 57.0f);
        }
        const auto x = part.channel(0);
        const auto y = filtered(x, 1.0f, warm);
        return measureThd(std::span{y}.subspan(samples(0.1), 32768), 220.0, rate, window);
    };
    for (const float knob : {1.0f, 2.5f, 5.0f, 7.5f, 10.0f})
    {
        std::cout << "| " << fixed(knob, 1) << " | " << fixed(0.70710678 * knob / 10.0, 3) << " | "
                  << fixed(100.0 * thdAt(knob / 10.0f, true), 3) << "% | "
                  << fixed(100.0 * thdAt(knob / 10.0f, false), 3) << "% |\n";
    }
    for (const bool warm : {true, false})
    {
        double lo = 0.01;
        double hi = 1.0;
        for (int i = 0; i < 24; ++i)
        {
            const double mid = std::sqrt(lo * hi);
            (thdAt(static_cast<float>(mid), warm) < 0.01 ? lo : hi) = mid;
        }
        std::cout << "\nWarm " << (warm ? "on" : "off") << ": 1% THD at Volume " << fixed(10.0 * lo, 2) << " ("
                  << dbText(lo) << " re Volume 10)";
    }
    std::cout << ".\n";
}

// --- The engines: the same output, no allocation ------------------------------------

struct EnginePatch
{
    std::string_view name;
    std::function<void(Voice&)> configure;
};

bool sameBits(const Buffer& a, const Buffer& b)
{
    for (std::size_t c = 0; c < 2; ++c)
    {
        const auto x = a.channel(c);
        const auto y = b.channel(c);
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

bool leftIsRight(const Buffer& a)
{
    const auto l = a.channel(0);
    const auto r = a.channel(1);
    return std::ranges::equal(l, r, [](float x, float y)
                              { return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y); });
}

std::string_view leftRightText(bool monoTaken, bool leftRight)
{
    if (!monoTaken)
    {
        return "-";
    }
    return leftRight ? "yes" : "NO";
}

void printEngines()
{
    const auto routed = [](Input a, Input b)
    {
        return [a, b](Voice& v)
        {
            setWorstPatch(v);
            Patch patch;
            patch.filters[0].input = a;
            patch.filters[1].input = b;
            patch.filters[0].drive = 4.0f;
            v.setPatch(patch);
        };
    };
    const std::array patches{
        EnginePatch{
            .name = "init",
            .configure = [](Voice& v) { setInitPatch(v); },
        },
        EnginePatch{
            .name = "worst (AB into both)",
            .configure = [](Voice& v) { setWorstPatch(v); },
        },
        EnginePatch{
            .name = "worst, centred (Stereo 0)",
            .configure = [](Voice& v) { setWorstPatch(v, 0.0f); },
        },
        EnginePatch{
            .name = "worst, A: AB, B: --",
            .configure = routed(Input::both, Input::none),
        },
        EnginePatch{
            .name = "worst, A: A, B: B",
            .configure = routed(Input::own, Input::own),
        },
        EnginePatch{
            .name = "worst, A: A, B: AB",
            .configure = routed(Input::own, Input::both),
        },
    };
    struct Variant
    {
        std::string name;
        Engine engine;
    };
    std::vector<Variant> engines;
    for (const auto layout : {Layout::perPart, Layout::perFilter})
    {
        for (const auto schedule : {Schedule::grid32, Schedule::block})
        {
            for (const bool mono : {false, true})
            {
                engines.push_back({
                    .name = std::string{layout == Layout::perPart ? "per-part" : "per-filter"} + ", " +
                            (schedule == Schedule::grid32 ? "grid" : "block") + (mono ? ", mono" : ""),
                    .engine =
                        {
                            .layout = layout,
                            .schedule = schedule,
                            .monoWhenCentred = mono,
                        },
                });
            }
        }
    }
    const auto length = samples(0.5);
    const auto noteOff = samples(0.3) + 7;
    std::cout << "\n## The engines\n\n"
              << "Each patch rendered by 8 engines (per-part or per-filter buffers; the grid or the block schedule; "
                 "with or without the mono path) at host blocks of 1, 37 and 4096, under readings I1 and I2: 500 ms, "
                 "a note-off at 300 ms + 7 samples. Every render against per-part, grid, block 4096, I2's.\n\n"
              << "| Patch | Renders | Identical to the bit | Allocations | Mono path taken | Left = right |\n"
              << "|---|---:|---:|---:|---|---|\n";
    for (const auto& p : patches)
    {
        for (const auto cross : {CrossEnvelope::sourceThenFilterPart, CrossEnvelope::filterPartOnly})
        {
            const Readings readings{.cross = cross};
            const auto reference = renderVoice(readings, defaultEngine, p.configure, length, noteOff, 4096);
            std::size_t renders = 0;
            std::size_t same = 0;
            std::size_t allocations = 0;
            bool monoTaken = false;
            bool leftRight = true;
            for (const auto& e : engines)
            {
                for (const std::size_t hostBlock : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
                {
                    const auto r = renderVoice(readings, e.engine, p.configure, length, noteOff, hostBlock);
                    ++renders;
                    same += sameBits(r.out, reference.out) ? 1 : 0;
                    allocations += r.allocations;
                    if (r.mono)
                    {
                        monoTaken = true;
                        leftRight = leftRight && leftIsRight(r.out);
                    }
                }
            }
            std::cout << "| " << p.name << (cross == CrossEnvelope::filterPartOnly ? ", I1" : ", I2") << " | "
                      << renders << " | " << same << " | " << allocations << " | " << (monoTaken ? "yes" : "no")
                      << " | " << leftRightText(monoTaken, leftRight) << " |\n";
            CHECK(same == renders);
            CHECK(allocations == 0);
            CHECK(leftRight);
        }
    }
}

void printMemory()
{
    std::cout << "\n## Memory per voice\n\n| Engine | The voice object | Scratch at maxBlockSize 512 | at 4096 |\n"
              << "|---|---:|---:|---:|\n";
    for (const auto schedule : {Schedule::grid32, Schedule::block})
    {
        std::array<std::size_t, 2> bytes{};
        std::size_t i = 0;
        for (const std::size_t maxBlock : {std::size_t{512}, std::size_t{4096}})
        {
            const Engine engine{
                .schedule = schedule,
            };
            Voice v{d6::minBlepTable64(), developerReadings, engine};
            v.prepare({.sampleRate = rate, .maxBlockSize = maxBlock, .numChannels = 1});
            bytes[i++] = v.scratchBytes();
        }
        std::cout << "| " << (schedule == Schedule::grid32 ? "grid (32-sample chunks)" : "block (whole pieces)")
                  << " | " << sizeof(Voice) << " B | " << bytes[0] << " B | " << bytes[1] << " B |\n";
    }
    std::cout << "\nOf the voice object: 4 slots " << 4 * sizeof(Slot) << " B, 2 filters "
              << 2 * sizeof(dsp::LadderFilter) << " B, 2 envelopes " << 2 * sizeof(dsp::Adsr) << " B, drift "
              << sizeof(dsp::PitchDrift) << " B.\n";
}

} // namespace

TEST_CASE("Voice report: the candidate structures on the real DSP classes", "[.][voice-report]")
{
    std::cout << "# Voice report (M1.4 task 1, stage 1)\n";
    printReadings();
    printLevels();
    printKnee();
    printEngines();
    printMemory();
}
