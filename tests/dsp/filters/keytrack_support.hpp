// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Test support for keytracking (docs/dsp/keytracking.md): a stand-in for the
// voice (M1.4) that feeds dsp::keytrackOctaves() into a LadderFilter, and the
// measurements the keytrack tests and the [keytrack-report] share.
//
// The voice composes the pitch (the key or the arpeggiator's note, plus the
// glide and the bend, plus dsp::masterTuneSemitones()) and feeds the offset
// at each of the filter's 32-sample grid points, with the pitch at the end of
// the coming segment ("aligned"): the filter reads a modulation at a grid
// point as the value g should reach at the next one. The other feeds are the
// alternatives the report measures.

#pragma once

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/math/pitch.hpp>

#include <anoesis/test/buffer.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace anoesis::test::keytrack
{

/// The voice's pitch (a fractional note) at a sample.
using PitchTrack = std::function<double(std::size_t)>;

enum class Feed : std::uint8_t
{
    aligned,   ///< at each grid point, the pitch at the segment's end (the decision)
    gridStart, ///< at each grid point, the pitch there: one segment late
    perBlock,  ///< once at the start of each host block
};

struct Setup
{
    double rate = 48000.0;
    bool warm = true;
    float k = 4.4f;
    float drive = 1.0f;
    double cutoffHz = 13.75; ///< the cutoff knob: the cutoff at the reference note (A-1)
    double amount = 1.0;     ///< Keytrack / 10
    Feed feed = Feed::aligned;
    std::size_t hostBlock = 512;
};

struct Run
{
    std::vector<float> y;
    std::size_t allocations = 0;
};

inline void applyKeytrack(dsp::LadderFilter& f, const Setup& s, double pitch)
{
    f.setCutoffModulation(dsp::keytrackOctaves(static_cast<float>(pitch), static_cast<float>(s.amount)));
}

/// Filters `x` in host blocks of s.hostBlock, split at the filter's grid,
/// feeding the keytrack as s.feed says. The filter starts as a voice's note
/// does: controls and the first pitch set, then reset (here, prepare()).
inline Run run(const Setup& s, std::span<const float> x, const PitchTrack& pitch)
{
    constexpr std::size_t grid = dsp::LadderFilter::gridSamples;
    dsp::LadderFilter f;
    f.setWarm(s.warm);
    f.setResonance(s.k);
    f.setDrive(s.drive);
    f.setCutoff(static_cast<float>(s.cutoffHz));
    applyKeytrack(f, s, pitch(0));
    f.prepare({.sampleRate = static_cast<float>(s.rate), .maxBlockSize = 4096, .numChannels = 1});
    Buffer buffer{1, x.size()};
    std::ranges::copy(x, buffer.channel(0).begin());
    Run result;
    {
        const AllocationCounter counter;
        for (std::size_t block = 0; block < x.size(); block += s.hostBlock)
        {
            const auto blockEnd = std::min(x.size(), block + s.hostBlock);
            if (s.feed == Feed::perBlock)
            {
                applyKeytrack(f, s, pitch(block));
            }
            for (std::size_t start = block; start < blockEnd;)
            {
                const auto end = std::min(blockEnd, ((start / grid) + 1) * grid);
                if (s.feed != Feed::perBlock && start % grid == 0)
                {
                    applyKeytrack(f, s, pitch(s.feed == Feed::aligned ? start + grid : start));
                }
                f.process(buffer.block().subBlock(start, end - start));
                start = end;
            }
        }
        result.allocations = counter.count();
    }
    result.y.assign(buffer.channel(0).begin(), buffer.channel(0).end());
    return result;
}

/// Silence with one impulse of 0.5 at the start: what starts a self-oscillation.
inline std::vector<float> kick(std::size_t n)
{
    std::vector<float> x(n, 0.0f);
    if (n > 0)
    {
        x[0] = 0.5f;
    }
    return x;
}

inline double centsBetween(double hz, double referenceHz)
{
    return 1200.0 * std::log2(hz / referenceHz);
}

/// 12-TET at A4 = 440 Hz, in double.
inline double noteHz(double note)
{
    return 440.0 * std::exp2((note - 69.0) / 12.0);
}

/// One period of a self-oscillation: its midpoint (samples) and frequency.
struct Period
{
    double at = 0.0;
    double hz = 0.0;
};

/// Upward zero crossings, interpolated linearly; each period's frequency.
inline std::vector<Period> periods(std::span<const float> y, double rate)
{
    std::vector<Period> out;
    double last = -1.0;
    for (std::size_t i = 1; i < y.size(); ++i)
    {
        if (y[i - 1] < 0.0f && y[i] >= 0.0f)
        {
            const auto a = static_cast<double>(y[i - 1]);
            const auto b = static_cast<double>(y[i]);
            const double t = static_cast<double>(i - 1) + (a / (a - b));
            if (last >= 0.0)
            {
                out.push_back({.at = 0.5 * (last + t), .hz = rate / (t - last)});
            }
            last = t;
        }
    }
    return out;
}

struct Tracking
{
    double maxCents = 0.0;
    double rmsCents = 0.0;
};

/// How far a self-oscillating filter strays from the oscillators' pitch over
/// [from, to), after taking out its steady detune (the mean over the 0.5 s
/// before `from`). The oscillators' frequency is averaged over each period
/// (Simpson's rule), as the measurement averages the filter's.
inline Tracking tracking(const Setup& s, const PitchTrack& pitch, std::size_t from, std::size_t to)
{
    const auto r = run(s, kick(to), pitch);
    const auto all = periods(r.y, s.rate);
    const auto expectedHz = [&](const Period& p)
    {
        const double width = s.rate / p.hz;
        const auto at = [&](double n)
        {
            return noteHz(pitch(static_cast<std::size_t>(std::max(0.0, n))));
        };
        return (at(p.at - (0.5 * width)) + (4.0 * at(p.at)) + at(p.at + (0.5 * width))) / 6.0;
    };
    double steady = 0.0;
    std::size_t count = 0;
    const double steadyFrom = static_cast<double>(from) - (0.5 * s.rate);
    for (const auto& p : all)
    {
        if (p.at > steadyFrom && p.at < static_cast<double>(from))
        {
            steady += centsBetween(p.hz, expectedHz(p));
            ++count;
        }
    }
    steady /= static_cast<double>(std::max<std::size_t>(count, 1));
    Tracking t;
    double sum = 0.0;
    count = 0;
    for (const auto& p : all)
    {
        if (p.at >= static_cast<double>(from) && p.at < static_cast<double>(to))
        {
            const double e = centsBetween(p.hz, expectedHz(p)) - steady;
            t.maxCents = std::max(t.maxCents, std::fabs(e));
            sum += e * e;
            ++count;
        }
    }
    t.rmsCents = std::sqrt(sum / static_cast<double>(std::max<std::size_t>(count, 1)));
    return t;
}

/// A portamento glide: constant time, linear in semitones, from `from` to
/// `to` over `samples`, starting at `start`. Returns the glided note.
struct Glide
{
    double from = 0.0;
    double to = 0.0;
    std::size_t start = 0;
    std::size_t samples = 1;

    [[nodiscard]] double noteAt(std::size_t n) const noexcept
    {
        if (n < start)
        {
            return from;
        }
        const double t = std::min(1.0, static_cast<double>(n - start) / static_cast<double>(samples));
        return from + ((to - from) * t);
    }
};

} // namespace anoesis::test::keytrack
