// Shared support for the range tests and report: the rates and waveforms
// they run over, the pitch paths through Nyquist, and the stress run through
// the whole OscillatorSlot (its pitch path, its events, and a renderer that
// applies them between process() calls at any block size).
// Doc: docs/dsp/oscillator-range.md

#pragma once

#include "oscillator_support.hpp"

#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/unison.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <span>
#include <vector>

namespace anoesis::test::range
{

using d6::Candidate;
using d6::MinBlep64;
using d6::minBlepTable64;
using dsp::Waveform;

using Slot = anoesis::dsp::OscillatorSlot<anoesis::dsp::minBlepTaps>;

inline constexpr std::array rates{44100.0, 48000.0, 96000.0, 192000.0};

// All 8, Noise included, in selector order.
inline constexpr std::array allWaveforms{
    Waveform::saw,      Waveform::pulse, Waveform::hpulse, Waveform::qpulse,
    Waveform::triangle, Waveform::sine,  Waveform::noise,  Waveform::trisaw,
};
inline constexpr double unitsPerCycle = 4294967296.0;

inline bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

inline bool bitIdentical(std::span<const float> a, std::span<const float> b)
{
    return std::ranges::equal(a, b, sameBits);
}

inline bool isSubnormal(float x)
{
    return std::fpclassify(x) == FP_SUBNORMAL;
}

inline MinBlep64 preparedOscillator(Waveform shape, double sampleRate = 48000.0)
{
    auto osc = Candidate<MinBlep64>::make();
    osc.prepare({.sampleRate = static_cast<float>(sampleRate), .maxBlockSize = 512, .numChannels = 1});
    osc.setWaveform(shape);
    return osc;
}

inline Slot preparedSlot(Waveform shape, int voices, double sampleRate)
{
    Slot slot{minBlepTable64()};
    slot.setVoices(voices);
    slot.prepare({.sampleRate = static_cast<float>(sampleRate), .maxBlockSize = 512, .numChannels = 2});
    slot.setWaveform(shape);
    return slot;
}

// The note whose frequency is `hz` at A4 = 440 Hz, in double.
inline double noteOf(double hz)
{
    return 69.0 + (12.0 * std::log2(hz / 440.0));
}

// The amplitude of the component at exactly `cycles` per sample: a least-
// squares fit of a cosine and a sine at that frequency.
inline double amplitudeAt(std::span<const float> x, double cycles)
{
    double cc = 0.0;
    double ss = 0.0;
    double cs = 0.0;
    double xc = 0.0;
    double xs = 0.0;
    const double centre = std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(x.size());
    for (std::size_t n = 0; n < x.size(); ++n)
    {
        const double angle = 2.0 * std::numbers::pi * cycles * static_cast<double>(n);
        const double c = std::cos(angle);
        const double s = std::sin(angle);
        cc += c * c;
        ss += s * s;
        cs += c * s;
        xc += (x[n] - centre) * c;
        xs += (x[n] - centre) * s;
    }
    const double det = (cc * ss) - (cs * cs);
    const double a = ((xc * ss) - (xs * cs)) / det;
    const double b = ((xs * cc) - (xc * cs)) / det;
    return std::hypot(a, b);
}

// Power from 10 Hz to `edgeHz`, about the mean, relative to a full-scale
// sine (0.5), in dB. 8192-point segments.
inline double powerBelowDb(std::span<const float> y, double edgeHz, double sampleRate)
{
    const double centre = std::accumulate(y.begin(), y.end(), 0.0) / static_cast<double>(y.size());
    std::vector<float> ac(y.size());
    std::ranges::transform(y, ac.begin(), [&](float v) { return static_cast<float>(v - centre); });
    const auto spectrum = averagedPowerSpectrum(ac, sampleRate, blackmanHarrisWindow(8192));
    double sum = 0.0;
    for (std::size_t k = spectrum.nearestBin(10.0); k < spectrum.power.size() && spectrum.frequencyOf(k) < edgeHz; ++k)
    {
        sum += spectrum.power[k];
    }
    return powerToDb(sum / 0.5);
}

/// The sweep of the Nyquist tests, in cycles per sample: 0.40 fs to 0.60 fs
/// and back, linear in pitch, `length` samples.
inline std::vector<double> sweepThroughNyquist(std::size_t length)
{
    std::vector<double> cycles(length);
    for (std::size_t i = 0; i < length / 2; ++i)
    {
        const double t = 2.0 * static_cast<double>(i) / static_cast<double>(length);
        cycles[i] = 0.40 * std::exp2(std::log2(1.5) * t);
        cycles[length - 1 - i] = cycles[i];
    }
    return cycles;
}

/// A 13 Hz vibrato of +-1 semitone around 0.485 fs (0.458 to 0.514 fs: in
/// and out of the fade band, and across Nyquist 26 times a second), in
/// cycles per sample at `sampleRate`.
inline std::vector<double> vibratoAcrossNyquist(std::size_t length, double sampleRate)
{
    std::vector<double> cycles(length);
    for (std::size_t i = 0; i < length; ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        cycles[i] = 0.485 * std::exp2(std::sin(2.0 * std::numbers::pi * 13.0 * t) / 12.0);
    }
    return cycles;
}

/// The paths' length in the tests and the report: 8 segments of 8192.
inline constexpr std::size_t nyquistPathLength = std::size_t{8} * 8192;

// A control change or note-on at a sample, applied between process() calls.
struct Event
{
    enum class Kind : std::uint8_t
    {
        noteOn,
        voices,
        invert,
        retrigger,
    };
    std::size_t at = 0;
    Kind kind = Kind::noteOn;
    int value = 0;
    float since = 0.0f;
};

// The pitch path at `sampleRate`, per sample, as notes: 0.01 Hz held, a
// sweep up to 0.6 fs (beyond Nyquist even before detune), held, back down
// to 0.01 Hz, then a 40 Hz vibrato of +-1 semitone around 0.485 fs.
inline std::vector<float> stressNotes(double sampleRate)
{
    const auto at = [&](double seconds)
    {
        return static_cast<std::size_t>(seconds * sampleRate);
    };
    const double bottom = noteOf(0.01);
    const double top = noteOf(0.6 * sampleRate);
    const double nearNyquist = noteOf(0.485 * sampleRate);
    std::vector<float> notes(at(0.1));
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        double note = bottom;
        if (t >= 0.005 && t < 0.025)
        {
            note = bottom + ((top - bottom) * (t - 0.005) / 0.02);
        }
        else if (t >= 0.025 && t < 0.03)
        {
            note = top;
        }
        else if (t >= 0.03 && t < 0.05)
        {
            note = top - ((top - bottom) * (t - 0.03) / 0.02);
        }
        else if (t >= 0.055)
        {
            note = nearNyquist + std::sin(2.0 * std::numbers::pi * 40.0 * t);
        }
        notes[i] = static_cast<float>(note);
    }
    return notes;
}

// Note-ons at random sample and sub-sample times, Retrig flipping, the
// voice count changing (often while a fade is under way), and Inv toggling.
inline std::vector<Event> stressEvents(std::size_t length, std::uint64_t seed)
{
    anoesis::dsp::Random random{seed};
    std::vector<Event> events;
    std::size_t at = 0;
    while (true)
    {
        at += 1 + (random.nextUint32() % 400U);
        if (at >= length)
        {
            return events;
        }
        const auto kind = static_cast<Event::Kind>(random.nextUint32() % 4U);
        events.push_back({
            .at = at,
            .kind = kind,
            .value = static_cast<int>(random.nextUint32() % 9U),
            .since = random.nextUnipolar(),
        });
    }
}

struct StressResult
{
    Buffer out{2, 0};
    float peak = 0.0f;
    bool finite = true;
    bool subnormalOut = false;
    bool subnormalState = false;
};

inline StressResult renderStress(Waveform shape, double sampleRate, std::size_t blockSize, std::span<const float> notes,
                                 std::span<const Event> events)
{
    auto slot = preparedSlot(shape, 8, sampleRate);
    slot.setDetune(3.3219f); // the knob law's largest (O13)
    slot.setStereo(1.0f);
    slot.noteOn();
    StressResult result{.out = Buffer{2, notes.size()}};
    const auto checkState = [&]
    {
        for (std::size_t v = 0; v < anoesis::dsp::maxUnisonVoices; ++v)
        {
            for (const float r : slot.oscillator(v).residuals())
            {
                result.subnormalState = result.subnormalState || isSubnormal(r);
            }
        }
    };
    const auto processSpan = [&](std::size_t from, std::size_t to)
    {
        if (to > from)
        {
            slot.process(result.out.block().subBlock(from, to - from), notes.subspan(from, to - from));
        }
    };
    std::size_t next = 0;
    bool inverted = false;
    bool retrigger = true;
    for (std::size_t start = 0; start < notes.size(); start += blockSize)
    {
        const std::size_t end = std::min(start + blockSize, notes.size());
        std::size_t position = start;
        while (next < events.size() && events[next].at < end)
        {
            processSpan(position, events[next].at);
            position = events[next].at;
            const auto& event = events[next];
            switch (event.kind)
            {
            case Event::Kind::noteOn:
                slot.noteOn(event.since);
                break;
            case Event::Kind::voices:
                slot.setVoices(event.value);
                break;
            case Event::Kind::invert:
                inverted = !inverted;
                slot.setInverted(inverted);
                break;
            case Event::Kind::retrigger:
                retrigger = !retrigger;
                slot.setRetrigger(retrigger);
                break;
            }
            ++next;
        }
        processSpan(position, end);
        if (blockSize > 1 || end % 64 == 0)
        {
            checkState();
        }
    }
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        for (const float y : result.out.channel(ch))
        {
            result.finite = result.finite && std::isfinite(y);
            result.subnormalOut = result.subnormalOut || isSubnormal(y);
            result.peak = std::max(result.peak, std::abs(y));
        }
    }
    return result;
}

} // namespace anoesis::test::range
