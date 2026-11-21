// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Test support for the Filter Control (docs/dsp/filter-control.md): a
// stand-in for the parameter layer's global knob ramps (M1.9) and for the
// voice (M1.4), feeding dsp::filterCutoffOctaves() and dsp::filterResonanceK()
// into LadderFilters; and the measurements the Filter Control tests and the
// [filter-control-report] share.
//
// The parameter layer: a host event sets a knob's target at its sample, and
// the knob ramps linearly over 10 ms from there, one LinearSmoother per knob,
// global (shared by every voice). A host block is split at events; within a
// piece a voice reads a ramp at any sample ahead with valueAfter().
//
// The voice (the decision, Feed::voiceRamp): its filter's 32-sample grid
// counts from its note-on. At each grid point it computes the clamped sums
// for the END of the coming segment (the knob ramps read there, its own
// modulation and pitch there), feeds the whole cutoff into
// setCutoffModulation() with setCutoff() held at the law's anchor, and the
// whole k into setResonanceModulation() with setResonance() at 0
// (ResonanceFeed::modulation). The other feeds are the alternatives the
// report measures against it.

#pragma once

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <span>
#include <vector>

namespace anoesis::test::filter_control
{

/// A value at each sample: a knob as the host sets it, a modulation, a pitch.
using Signal = std::function<float(std::size_t)>;

inline Signal constant(float value)
{
    return [value](std::size_t)
    {
        return value;
    };
}

/// How the voice feeds the cutoff.
enum class Feed : std::uint8_t
{
    voiceRamp,  ///< W2, the decision: the knob ramps in the voice's sum, all into setCutoffModulation(), setCutoff() at
                ///< the anchor
    filterRamp, ///< W1: the knobs' target sum through setCutoff() (the filter's ramp), the clamp's remainder as
                ///< modulation
    bothRamps,  ///< W3: the voice's sum into setCutoff(), which ramps it again
    noRamp,     ///< W4: the knobs' raw values, unramped, into setCutoffModulation()
};

/// How the voice feeds k.
enum class ResonanceFeed : std::uint8_t
{
    modulation,   ///< the decision: the voice's k at each grid point into setResonanceModulation(), setResonance(0)
    perGridPoint, ///< the voice's k at each grid point through setResonance(), which restarts its 10 ms ramp
    filterRamp,   ///< the knobs' target sum through setResonance() at each event (the filter's ramp)
};

/// The global knobs, as the host sets them (ramped by the parameter layer).
struct Knobs
{
    Signal cutoff = constant(5.0f); ///< filter X's Cutoff
    Signal controlCutoff = constant(5.0f);
    Signal resonance = constant(0.0f); ///< filter X's Resonance
    Signal controlResonance = constant(0.0f);
};

/// One voice: when its note starts, its own modulation (knob units), its pitch.
struct Voice
{
    std::size_t noteOn = 0;
    Signal cutoffModulation = constant(0.0f);  ///< to Cutoff X
    Signal controlModulation = constant(0.0f); ///< to Cutoff AB
    Signal resonanceModulation = constant(0.0f);
    Signal controlResonanceModulation = constant(0.0f);
    Signal pitch = constant(dsp::keytrackReferenceNote);
};

struct Setup
{
    double rate = 48000.0;
    float keytrack = 0.0f; ///< the Keytrack knob, 0-10
    bool warm = true;
    float drive = 1.0f;
    dsp::FilterResponse response = dsp::FilterResponse::lp24;
    Feed feed = Feed::voiceRamp;
    ResonanceFeed resonanceFeed = ResonanceFeed::modulation;
    std::size_t hostBlock = 512;
    dsp::FilterControlLaw law = dsp::filterControlLaw;
};

struct VoiceRun
{
    std::vector<float> y; ///< the voice's output from sample 0 (the input passed through before its note-on)
    std::vector<double>
        log2Cutoff;       ///< log2 currentCutoff() at the end of each of its segments: sample noteOn + 32 (i + 1)
    std::vector<float> k; ///< the k the voice computed for the end of each segment (before the filter's own ramp)
    float noteOnK = 0.0f; ///< the k it computed at its note-on
};

struct Run
{
    std::vector<VoiceRun> voices;
    std::size_t allocations = 0;
};

/// The global knob ramps: one LinearSmoother per knob, positioned at the
/// start of the piece of the block being processed.
class KnobRamps
{
public:
    KnobRamps(const Knobs& knobs, double rate) : knobs(knobs)
    {
        for (auto* r : all())
        {
            r->prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = 4096, .numChannels = 1});
            r->setRampTime(dsp::LadderFilter::rampSeconds);
        }
        cutoff.setCurrentAndTarget(knobs.cutoff(0));
        controlCutoff.setCurrentAndTarget(knobs.controlCutoff(0));
        resonance.setCurrentAndTarget(knobs.resonance(0));
        controlResonance.setCurrentAndTarget(knobs.controlResonance(0));
    }

    /// True if any knob's host value at `at` differs from its target.
    [[nodiscard]] bool eventAt(std::size_t at) const
    {
        const auto differs = [](float v, const dsp::LinearSmoother& r)
        {
            return v < r.targetValue() || v > r.targetValue();
        };
        return differs(knobs.cutoff(at), cutoff) || differs(knobs.controlCutoff(at), controlCutoff) ||
               differs(knobs.resonance(at), resonance) || differs(knobs.controlResonance(at), controlResonance);
    }

    /// Starts each changed knob's ramp at `at`, where the ramps stand.
    void applyEvents(std::size_t at)
    {
        cutoff.setTarget(knobs.cutoff(at));
        controlCutoff.setTarget(knobs.controlCutoff(at));
        resonance.setTarget(knobs.resonance(at));
        controlResonance.setTarget(knobs.controlResonance(at));
    }

    void advance(std::size_t samples)
    {
        for (auto* r : all())
        {
            r->skip(samples);
        }
        position += samples;
    }

    /// A ramp's value at sample `at` (at or after the current position): the
    /// ramp is at its start value on its event's sample and lands 10 ms later.
    [[nodiscard]] static float at(const dsp::LinearSmoother& ramp, std::size_t from, std::size_t sample)
    {
        return ramp.valueAfter(sample - from);
    }

    [[nodiscard]] std::size_t where() const noexcept
    {
        return position;
    }

    dsp::LinearSmoother cutoff;
    dsp::LinearSmoother controlCutoff;
    dsp::LinearSmoother resonance;
    dsp::LinearSmoother controlResonance;

private:
    std::array<dsp::LinearSmoother*, 4> all()
    {
        return {&cutoff, &controlCutoff, &resonance, &controlResonance};
    }

    Knobs knobs;
    std::size_t position = 0;
};

namespace detail
{

struct VoiceState
{
    explicit VoiceState(std::size_t samples) : buffer{1, samples} {}

    dsp::LadderFilter filter;
    Buffer buffer;
    bool started = false;
    VoiceRun out;
};

/// The voice's terms at sample `at`, with the knobs read from the ramps (or
/// their raw targets, for W1 and W4).
inline dsp::FilterCutoffTerms cutoffTerms(const Setup& s, const KnobRamps& ramps, const Voice& v, std::size_t at,
                                          bool rampedKnobs)
{
    const auto from = ramps.where();
    return {
        .cutoff = rampedKnobs ? KnobRamps::at(ramps.cutoff, from, at) : ramps.cutoff.targetValue(),
        .cutoffModulation = v.cutoffModulation(at),
        .controlCutoff = rampedKnobs ? KnobRamps::at(ramps.controlCutoff, from, at) : ramps.controlCutoff.targetValue(),
        .controlModulation = v.controlModulation(at),
        .keytrack = dsp::keytrackOctaves(v.pitch(at), s.keytrack / 10.0f),
    };
}

inline dsp::FilterResonanceTerms resonanceTerms(const KnobRamps& ramps, const Voice& v, std::size_t at,
                                                bool rampedKnobs)
{
    const auto from = ramps.where();
    return {
        .resonance = rampedKnobs ? KnobRamps::at(ramps.resonance, from, at) : ramps.resonance.targetValue(),
        .resonanceModulation = v.resonanceModulation(at),
        .controlResonance =
            rampedKnobs ? KnobRamps::at(ramps.controlResonance, from, at) : ramps.controlResonance.targetValue(),
        .controlModulation = v.controlResonanceModulation(at),
    };
}

/// W1's split: the knobs' target sum through the filter's ramp.
inline float knobTargetOctaves(const Setup& s, const KnobRamps& ramps)
{
    return s.law.octavesPerUnit * (ramps.cutoff.targetValue() + ramps.controlCutoff.targetValue());
}

inline float hzAbove(const Setup& s, float octaves)
{
    return s.law.anchorHz * std::exp2(octaves);
}

/// Feeds the filter for sample `at`: at a note-on the values there, at a
/// grid point those at the end of the coming segment, at an event (W1 and
/// the filter-ramp resonance only) the new targets.
inline void feed(const Setup& s, const KnobRamps& ramps, const Voice& v, VoiceState& state, std::size_t at,
                 bool gridPoint)
{
    auto& f = state.filter;
    switch (s.feed)
    {
    case Feed::voiceRamp:
        f.setCutoff(s.law.anchorHz);
        f.setCutoffModulation(dsp::filterCutoffOctaves(cutoffTerms(s, ramps, v, at, true), s.law));
        break;
    case Feed::filterRamp:
    {
        const float knobs = knobTargetOctaves(s, ramps);
        f.setCutoff(hzAbove(s, knobs));
        f.setCutoffModulation(dsp::filterCutoffOctaves(cutoffTerms(s, ramps, v, at, false), s.law) - knobs);
        break;
    }
    case Feed::bothRamps:
        f.setCutoff(hzAbove(s, dsp::filterCutoffOctaves(cutoffTerms(s, ramps, v, at, true), s.law)));
        break;
    case Feed::noRamp:
        f.setCutoff(s.law.anchorHz);
        f.setCutoffModulation(dsp::filterCutoffOctaves(cutoffTerms(s, ramps, v, at, false), s.law));
        break;
    }
    const float k =
        dsp::filterResonanceK(resonanceTerms(ramps, v, at, s.resonanceFeed != ResonanceFeed::filterRamp), s.law);
    switch (s.resonanceFeed)
    {
    case ResonanceFeed::modulation:
        f.setResonance(0.0f);
        f.setResonanceModulation(k);
        break;
    case ResonanceFeed::perGridPoint:
    case ResonanceFeed::filterRamp:
        f.setResonance(k);
        break;
    }
    if (gridPoint)
    {
        state.out.k.push_back(k);
    }
}

} // namespace detail

/// Filters `x` through one filter per voice, in host blocks of s.hostBlock,
/// split at knob events and at each voice's grid, feeding the Filter Control
/// as s.feed and s.resonanceFeed say. Before its note-on a voice passes `x`
/// through; at its note-on its controls are set and it is reset().
inline Run run(const Setup& s, const Knobs& knobs, std::span<const Voice> voices, std::span<const float> x)
{
    constexpr std::size_t grid = dsp::LadderFilter::gridSamples;
    KnobRamps ramps{knobs, s.rate};
    std::vector<detail::VoiceState> states;
    states.reserve(voices.size());
    for (std::size_t i = 0; i < voices.size(); ++i)
    {
        auto& state = states.emplace_back(x.size());
        state.filter.setWarm(s.warm);
        state.filter.setDrive(s.drive);
        state.filter.setResponse(s.response);
        state.filter.prepare({.sampleRate = static_cast<float>(s.rate), .maxBlockSize = 4096, .numChannels = 1});
        std::ranges::copy(x, state.buffer.channel(0).begin());
        state.out.log2Cutoff.reserve((x.size() / grid) + 1);
        state.out.k.reserve((x.size() / grid) + 1);
    }
    Run result;
    {
        const AllocationCounter counter;
        for (std::size_t block = 0; block < x.size(); block += s.hostBlock)
        {
            const auto blockEnd = std::min(x.size(), block + s.hostBlock);
            for (std::size_t start = block; start < blockEnd;)
            {
                // A piece runs to the next event (or the block's end).
                auto end = start + 1;
                while (end < blockEnd && !ramps.eventAt(end))
                {
                    ++end;
                }
                const bool event = ramps.eventAt(start);
                ramps.applyEvents(start);
                for (std::size_t i = 0; i < voices.size(); ++i)
                {
                    const auto& v = voices[i];
                    auto& state = states[i];
                    if (event && state.started &&
                        (s.feed == Feed::filterRamp || s.resonanceFeed == ResonanceFeed::filterRamp))
                    {
                        // The new targets at once: the filter ramps to them
                        // from its next grid point. (The modulation set here is
                        // replaced at that grid point before the filter reads it.)
                        detail::feed(s, ramps, v, state, start, false);
                    }
                    for (auto at = std::max(start, v.noteOn); at < end;)
                    {
                        if (!state.started)
                        {
                            detail::feed(s, ramps, v, state, at, false);
                            state.out.noteOnK =
                                dsp::filterResonanceK(detail::resonanceTerms(ramps, v, at, true), s.law);
                            state.filter.reset();
                            state.started = true;
                        }
                        const auto offset = (at - v.noteOn) % grid;
                        if (offset == 0)
                        {
                            detail::feed(s, ramps, v, state, at + grid, true);
                        }
                        const auto to = std::min(end, at + (grid - offset));
                        state.filter.process(state.buffer.block().subBlock(at, to - at));
                        if ((to - v.noteOn) % grid == 0)
                        {
                            state.out.log2Cutoff.push_back(
                                std::log2(static_cast<double>(state.filter.currentCutoff())));
                        }
                        at = to;
                    }
                }
                ramps.advance(end - start);
                start = end;
            }
        }
        result.allocations = counter.count();
    }
    for (auto& state : states)
    {
        state.out.y.assign(state.buffer.channel(0).begin(), state.buffer.channel(0).end());
        result.voices.push_back(std::move(state.out));
    }
    return result;
}

/// One voice from sample 0.
inline VoiceRun runOne(const Setup& s, const Knobs& knobs, const Voice& v, std::span<const float> x)
{
    return run(s, knobs, std::span{&v, 1}, x).voices.front();
}

// --- Measurements ------------------------------------------------------------------------

/// |H(f)| of an impulse response h scaled by 1/amplitude, in double.
inline double magnitudeAt(std::span<const float> h, double amplitude, double hz, double rate)
{
    const std::complex<double> step = std::polar(1.0, -2.0 * std::numbers::pi * hz / rate);
    std::complex<double> phasor{1.0, 0.0};
    std::complex<double> sum{0.0, 0.0};
    for (std::size_t n = 0; n < h.size(); ++n)
    {
        sum += static_cast<double>(h[n]) * phasor;
        phasor *= step;
        if (n % 4096 == 4095)
        {
            phasor /= std::abs(phasor); // keep the rotation on the unit circle
        }
    }
    return std::abs(sum) / amplitude;
}

/// The frequency where LP24 at k 0 falls to 1/4 (-12.04 dB), exactly the
/// cutoff for the prewarped ladder (filters.md), of a filter set up by the
/// caller (settled, k 0, LP24) whose cutoff is about `expectedHz`. Small
/// signal (-80 dBFS), so the saturators are linear; bisection in log2 f on
/// the impulse response's DTFT, which is monotone for LP24.
inline double minus12DbPoint(dsp::LadderFilter filter, double expectedHz, double rate)
{
    constexpr double amplitude = 1e-4;
    // The 4-pole impulse response falls as t^3 e^(-wc t): 40 time constants
    // leave e^-40 x 40^3 ~ 3e-13 of its peak.
    const auto n = std::max<std::size_t>(
        8192, static_cast<std::size_t>(40.0 / (2.0 * std::numbers::pi * expectedHz) * rate) + 4096);
    Buffer buffer{1, n};
    buffer.channel(0)[0] = static_cast<float>(amplitude);
    for (std::size_t start = 0; start < n; start += 4096)
    {
        filter.process(buffer.block().subBlock(start, std::min<std::size_t>(4096, n - start)));
    }
    const std::span<const float> h{buffer.channel(0).data(), n};
    double lo = std::log2(0.25);
    double hi = std::log2(0.4999 * rate);
    for (int i = 0; i < 60; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (magnitudeAt(h, amplitude, std::exp2(mid), rate) > 0.25 ? lo : hi) = mid;
    }
    return std::exp2(0.5 * (lo + hi));
}

/// A settled filter (k 0, LP24, Warm on) fed as the voice feeds it: the
/// anchor in setCutoff(), `octaves` in the modulation input.
inline dsp::LadderFilter fedFilter(float octaves, double rate, const dsp::FilterControlLaw& law = dsp::filterControlLaw)
{
    dsp::LadderFilter f;
    f.setCutoff(law.anchorHz);
    f.setCutoffModulation(octaves);
    f.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = 4096, .numChannels = 1});
    return f;
}

/// The law's cutoff in double, after the DSP's [1 Hz, 0.495 fs].
inline double lawHz(double octaves, double rate, const dsp::FilterControlLaw& law = dsp::filterControlLaw)
{
    const double hz = static_cast<double>(law.anchorHz) * std::exp2(static_cast<double>(law.octavesPerUnit) * octaves);
    return std::clamp(hz, 1.0, static_cast<double>(dsp::LadderFilter::maxCutoffRatio * static_cast<float>(rate)));
}

/// Energy above 1 kHz in a Hann 4096 centred on `at` (filters.md's click measure).
inline double energyAbove1k(std::span<const float> x, std::size_t at, double rate)
{
    static const Window window = hannWindow(4096);
    const auto spectrum = powerSpectrum(x.subspan(at - 2048, 4096), rate, window);
    double sum = 0.0;
    for (std::size_t k = 0; k < spectrum.power.size(); ++k)
    {
        sum += spectrum.frequencyOf(k) >= 1000.0 ? spectrum.power[k] : 0.0;
    }
    return sum;
}

} // namespace anoesis::test::filter_control
