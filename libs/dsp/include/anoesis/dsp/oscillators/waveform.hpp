// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/oscillators/phase.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

/// The oscillator waveforms, as data.
///
/// A periodic waveform is a list of *corners*: the phases where a straight
/// segment starts. Each corner records the naive value and slope just after
/// it, and how much the waveform jumps (a step in value) and bends (a step in
/// slope) there. A band-limiting method corrects each jump with a step
/// residual and each bend with a ramp residual, and evaluates the naive
/// waveform between corners from the same list. So the per-sample loop never
/// names a waveform, and a new piecewise-linear waveform is one more row in
/// the table below. A shape may also carry a sine, which has no corners and
/// needs no correction.
///
/// The 8 waveforms are Sylenth1's (inventory §2). The pulse widths and the
/// TriSaw shape are working hypotheses, to be measured against the demo in
/// M1.13 (inventory W2, W3); changing one is a change to this table only.
///
/// Doc: docs/dsp/oscillator-waveforms.md
namespace anoesis::dsp
{

/// The waveform selector's 8 entries, in the order third-party sources list
/// them (the demo's own order is inventory W9).
enum class Waveform : std::uint8_t
{
    saw,      ///< rising ramp: -1 at t = 0 to +1
    pulse,    ///< square: +1 for t < 1/2, -1 after
    hpulse,   ///< 25% pulse, +-1 (provisional width)
    qpulse,   ///< 12.5% pulse, +-1 (provisional width)
    triangle, ///< -1 at t = 0, +1 at t = 1/2
    sine,     ///< sin 2 pi t
    noise,    ///< white noise: not periodic, so not in the oscillators; see isPeriodic()
    trisaw,   ///< (triangle + saw) / 2 (provisional shape)
};

inline constexpr std::size_t waveformCount = 8;

/// One straight segment of a waveform definition: it starts at `start`
/// cycles at the value `from` and heads for `to`, which it would reach at the
/// next segment's start (or at t = 1 for the last). Used only to build a
/// WaveShape, so double precision costs nothing.
struct WaveSegment
{
    double start = 0.0; ///< in cycles, [0, 1); the first segment starts at 0
    double from = 0.0;
    double to = 0.0;
};

/// A corner of a WaveShape: where a segment starts.
struct WaveCorner
{
    std::uint32_t phase = 0; ///< fixed point, 2^32 per cycle
    float value = 0.0f;      ///< the naive value just after the corner
    float slope = 0.0f;      ///< per cycle, until the next corner
    float jump = 0.0f;       ///< value after minus value before
    float bend = 0.0f;       ///< slope after minus slope before, per cycle
    bool jumps = false;      ///< jump != 0: a step residual is needed
    bool bends = false;      ///< bend != 0: a ramp residual is needed
};

/// A periodic waveform: up to maxCorners straight segments, plus a sine of
/// level sineLevel(). Default-constructed, it is silence.
class WaveShape
{
public:
    static constexpr std::size_t maxCorners = 4;

    constexpr WaveShape() = default;

    /// The waveform made of `segments`, which must be in order of `start`,
    /// starting at 0. Jumps and bends are worked out here, where the segment
    /// before the first is the last one (the waveform is periodic).
    template <std::size_t N>
    [[nodiscard]] static constexpr WaveShape piecewiseLinear(const std::array<WaveSegment, N>& segments) noexcept
    {
        static_assert(N >= 1 && N <= maxCorners, "a WaveShape has 1 to maxCorners segments");
        std::array<double, N> slopes{};
        for (std::size_t i = 0; i < N; ++i)
        {
            const double end = i + 1 < N ? segments[i + 1].start : 1.0;
            slopes[i] = (segments[i].to - segments[i].from) / (end - segments[i].start);
        }
        WaveShape shape;
        shape.count = N;
        double sum = 0.0;
        for (std::size_t i = 0; i < N; ++i)
        {
            const double end = i + 1 < N ? segments[i + 1].start : 1.0;
            sum += (end - segments[i].start) * 0.5 * (segments[i].from + segments[i].to); // a trapezium
        }
        shape.average = static_cast<float>(sum);
        for (std::size_t i = 0; i < N; ++i)
        {
            const std::size_t before = (i + N - 1) % N;
            const double jump = segments[i].from - segments[before].to;
            const double bend = slopes[i] - slopes[before];
            shape.points[i] = WaveCorner{
                .phase = static_cast<std::uint32_t>(segments[i].start * 4294967296.0),
                .value = static_cast<float>(segments[i].from),
                .slope = static_cast<float>(slopes[i]),
                .jump = static_cast<float>(jump),
                .bend = static_cast<float>(bend),
                .jumps = jump < 0.0 || jump > 0.0,
                .bends = bend < 0.0 || bend > 0.0,
            };
        }
        return shape;
    }

    /// A sine of amplitude `level`, starting at 0 and rising.
    [[nodiscard]] static constexpr WaveShape sine(float level = 1.0f) noexcept
    {
        WaveShape shape;
        shape.sineAmplitude = level;
        return shape;
    }

    [[nodiscard]] constexpr std::span<const WaveCorner> corners() const noexcept
    {
        return {points.data(), count};
    }

    /// The waveform's mean over a cycle: its DC, and all a band-limited
    /// version of it keeps once its fundamental is above Nyquist. The sine
    /// part adds nothing.
    [[nodiscard]] constexpr float mean() const noexcept
    {
        return average;
    }

    [[nodiscard]] constexpr float sineLevel() const noexcept
    {
        return sineAmplitude;
    }

    /// The corner whose segment contains `phase`. Needs at least one corner;
    /// the first is at phase 0, so the search always ends.
    [[nodiscard]] constexpr const WaveCorner& cornerAt(std::uint32_t phase) const noexcept
    {
        std::size_t i = count > 0 ? count - 1 : 0;
        while (i > 0 && phase < points[i].phase)
        {
            --i;
        }
        return points[i];
    }

    /// The straight segments' naive value at `phase` (without the sine).
    [[nodiscard]] constexpr float linearValueAt(std::uint32_t phase) const noexcept
    {
        if (count == 0)
        {
            return 0.0f;
        }
        const auto& corner = cornerAt(phase);
        return corner.value + (corner.slope * toCycles(phase - corner.phase));
    }

    /// The whole waveform at `phase`, unfiltered. For tests and displays;
    /// the oscillators take the sine part separately (it is delayed).
    [[nodiscard]] float valueAt(std::uint32_t phase) const noexcept
    {
        float y = linearValueAt(phase);
        if (sineAmplitude < 0.0f || sineAmplitude > 0.0f)
        {
            y += sineAmplitude * std::sin(2.0f * std::numbers::pi_v<float> * toCycles(phase));
        }
        return y;
    }

private:
    std::array<WaveCorner, maxCorners> points{};
    std::size_t count = 0;
    float sineAmplitude = 0.0f;
    float average = 0.0f;
};

/// The fixed pulse widths. Provisional (inventory W2): "half" and "quarter"
/// of the square's width.
inline constexpr double hpulseWidth = 0.25;
inline constexpr double qpulseWidth = 0.125;

namespace detail
{

[[nodiscard]] constexpr WaveShape pulseShape(double width) noexcept
{
    return WaveShape::piecewiseLinear(std::array{
        WaveSegment{.start = 0.0, .from = 1.0, .to = 1.0},
        WaveSegment{.start = width, .from = -1.0, .to = -1.0},
    });
}

} // namespace detail

/// The shape of each Waveform, indexed by its value.
inline constexpr std::array<WaveShape, waveformCount> waveShapes{
    // saw: one segment, -1 to +1; jumps by -2 at t = 0
    WaveShape::piecewiseLinear(std::array{WaveSegment{.start = 0.0, .from = -1.0, .to = 1.0}}),
    detail::pulseShape(0.5),
    detail::pulseShape(hpulseWidth),
    detail::pulseShape(qpulseWidth),
    // triangle: bends by +8 at t = 0 and -8 at t = 1/2 (slope +-4 per cycle)
    WaveShape::piecewiseLinear(std::array{
        WaveSegment{.start = 0.0, .from = -1.0, .to = 1.0},
        WaveSegment{.start = 0.5, .from = 1.0, .to = -1.0},
    }),
    WaveShape::sine(),
    WaveShape{}, // noise: silence here; generated by WhiteNoise
    // TriSaw, provisional (inventory W3): (triangle + saw) / 2. Up at 3 per
    // cycle to 1/2, down at 1 per cycle to 0, then a jump of -1 back to -1.
    // Zero mean, peak 1, harmonics falling as 1/k like the saw.
    WaveShape::piecewiseLinear(std::array{
        WaveSegment{.start = 0.0, .from = -1.0, .to = 0.5},
        WaveSegment{.start = 0.5, .from = 0.5, .to = 0.0},
    }),
};

/// The shape for `waveform`. An out-of-range value (a corrupt parameter)
/// gives the saw rather than reading past the table.
[[nodiscard]] constexpr const WaveShape& waveShape(Waveform waveform) noexcept
{
    const auto index = static_cast<std::size_t>(waveform);
    return waveShapes[index < waveformCount ? index : 0];
}

/// False for noise, which has no phase, pitch, or edges: it is generated by
/// WhiteNoise alongside the oscillators, not by them.
[[nodiscard]] constexpr bool isPeriodic(Waveform waveform) noexcept
{
    return waveform != Waveform::noise;
}

} // namespace anoesis::dsp
