// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/oscillators/waveform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

/// The LFO: a modulation source with Sylenth1's controls (inventory §6,
/// LFOs): a waveform, a rate, a gain, and an offset. The model was chosen
/// from measured candidates (docs/dsp/lfos.md):
///
/// - The waveforms are data: a constexpr table of rows (lfoShapes), each a
///   kind, a WaveShape for the periodic ones (the oscillator's rows where
///   the names match), and the phase a cycle starts at. A new periodic
///   waveform is a row.
/// - The phase is 32.32 fixed point: exact for hours, without double.
/// - Piecewise-linear rows are computed exactly, a segment at a time. Curved
///   rows (sine, smoothed random, Lorenz) are evaluated every gridSamples
///   samples on a grid counted from the last (re)start, and interpolated
///   linearly in between (spurs at -73 dB or below on an amplitude for a
///   40 Hz sine, below -100 dB on pitch).
/// - Every jump (a waveform's edge, a sample-and-hold step, a retrigger, a
///   waveform change) is slewed linearly from the last output to the new
///   value over 1 ms, at most 1/16 of a cycle: a step clicks in band, which
///   band-limiting does not cure. While a slew lasts the wave can pass +-1
///   by at most its slope over the slew: 4 per cycle x 1/16 = 0.25.
/// - The output is gain x (wave + offset), unclipped; gain in [0, 1] and
///   offset in [-1, 1] ramp linearly over 10 ms when changed.
///
/// The rate is set in Hz (setFrequency) or, for tempo sync, as an exact
/// phase increment (setPhaseIncrement with notePhaseIncrement() from
/// note_value.hpp, once per block): a float in Hz drifts 0.016 cycles in
/// three hours at 1/64T (docs/dsp/tempo-sync.md).
///
/// A voice whose LFO is not free calls restart() when it starts from
/// silence and retrigger() when a note retriggers it while it sounds. A free
/// LFO is one instance running for the whole synth; a voice copies it at its
/// note-on (Lfo is a value type), and then runs on in step with it.
///
/// Everything after prepare() is real-time safe and never allocates.
/// reset() settles the Lorenz attractor (1000 small steps, about 10 us).
/// Doc: docs/dsp/lfos.md
namespace anoesis::dsp
{

/// The waveform selector's entries (inventory L1: the 10 names one source
/// lists, and a provisional 11th).
enum class LfoWaveform : std::uint8_t
{
    sine,       ///< sin 2 pi t, starting at its peak
    saw,        ///< rising, -1 to +1
    ramp,       ///< falling, +1 to -1
    triangle,   ///< starting at its peak
    pulse,      ///< +1 for half a cycle, -1 after
    hpulse,     ///< 25% high
    qpulse,     ///< 12.5% high
    trisaw,     ///< (triangle + saw) / 2
    lorenz,     ///< the x of a Lorenz attractor, its time scaled by the rate
    sampleHold, ///< a uniform value per cycle, held
    smooth,     ///< a uniform value per cycle, joined by smoothstep (provisional)
};

inline constexpr std::size_t lfoWaveformCount = 11;

/// A quarter of a cycle, 2^32 per cycle.
inline constexpr std::uint32_t quarterCycle = 0x40000000U;

/// What generates a row's values.
enum class LfoKind : std::uint8_t
{
    periodic,     ///< a WaveShape, read at the phase
    sampleHold,   ///< a new value from Random at the start of each cycle
    smoothRandom, ///< a new value per cycle, joined by smoothstep
    lorenz,       ///< a Lorenz attractor
};

/// A row of the waveform table.
struct LfoShape
{
    LfoKind kind = LfoKind::periodic;
    WaveShape wave;          ///< the periodic kind's shape
    std::uint32_t start = 0; ///< where a cycle starts, 2^32 per cycle

    /// Curved rows are interpolated on the grid; the others are exact.
    [[nodiscard]] constexpr bool isCurved() const noexcept
    {
        return kind == LfoKind::smoothRandom || kind == LfoKind::lorenz ||
               (kind == LfoKind::periodic && (wave.sineLevel() < 0.0f || wave.sineLevel() > 0.0f));
    }
};

/// The rows, indexed by LfoWaveform. The shapes and start phases are working
/// hypotheses (inventory L2, L5): only the sine's start has a source.
inline constexpr std::array<LfoShape, lfoWaveformCount> lfoShapes{
    LfoShape{.kind = LfoKind::periodic, .wave = WaveShape::sine(), .start = quarterCycle},
    LfoShape{.kind = LfoKind::periodic, .wave = waveShapes[static_cast<std::size_t>(Waveform::saw)], .start = 0},
    LfoShape{
        .kind = LfoKind::periodic,
        .wave = WaveShape::piecewiseLinear(std::array{WaveSegment{.start = 0.0, .from = 1.0, .to = -1.0}}),
        .start = 0,
    },
    LfoShape{
        .kind = LfoKind::periodic,
        .wave = waveShapes[static_cast<std::size_t>(Waveform::triangle)],
        .start = halfCycle,
    },
    LfoShape{.kind = LfoKind::periodic, .wave = waveShapes[static_cast<std::size_t>(Waveform::pulse)], .start = 0},
    LfoShape{.kind = LfoKind::periodic, .wave = waveShapes[static_cast<std::size_t>(Waveform::hpulse)], .start = 0},
    LfoShape{.kind = LfoKind::periodic, .wave = waveShapes[static_cast<std::size_t>(Waveform::qpulse)], .start = 0},
    LfoShape{.kind = LfoKind::periodic, .wave = waveShapes[static_cast<std::size_t>(Waveform::trisaw)], .start = 0},
    LfoShape{.kind = LfoKind::lorenz},
    LfoShape{.kind = LfoKind::sampleHold},
    LfoShape{.kind = LfoKind::smoothRandom},
};

/// The row for `waveform`; an out-of-range value gives the sine.
[[nodiscard]] constexpr const LfoShape& lfoShape(LfoWaveform waveform) noexcept
{
    const auto index = static_cast<std::size_t>(waveform);
    return lfoShapes[index < lfoWaveformCount ? index : 0];
}

/// A trigger, for Lfo::apply() and the events of Lfo::process().
enum class LfoTrigger : std::uint8_t
{
    restart,   ///< restart(): the cycle's start at once (a voice from silence)
    retrigger, ///< retrigger(): the cycle's start, the jump slewed (a sounding voice)
};

/// A trigger at a sample offset within a block.
struct LfoEvent
{
    std::size_t offset = 0;
    LfoTrigger trigger = LfoTrigger::retrigger;
};

/// The state of a Lorenz attractor.
struct LorenzState
{
    float x = 1.0f;
    float y = 1.0f;
    float z = 25.0f;
};

/// One midpoint (RK2) step of the Lorenz system (sigma 10, rho 28,
/// beta 8/3). Every product and every sum is a statement of its own, so no
/// compiler fuses a multiply into an add: Clang contracts only within one
/// expression by default, and the x86-64 baseline has no FMA instruction.
/// The attractor amplifies any rounding difference (renders parted after
/// 15-20 cycles), so this keeps a Lorenz render bit-identical on every
/// target built today. Tested against a reference that rounds every
/// operation through a volatile store. (A GCC build for arm64 contracts
/// across statements and would need -ffp-contract=off.)
inline void lorenzStep(LorenzState& s, float dt) noexcept
{
    constexpr float sigma = 10.0f;
    constexpr float rho = 28.0f;
    constexpr float beta = 8.0f / 3.0f;
    const float h = 0.5f * dt;
    const float yx = s.y - s.x;
    const float dx1 = sigma * yx;
    const float rz = rho - s.z;
    const float xrz = s.x * rz;
    const float dy1 = xrz - s.y;
    const float xy = s.x * s.y;
    const float bz = beta * s.z;
    const float dz1 = xy - bz;
    const float hx = h * dx1;
    const float hy = h * dy1;
    const float hz = h * dz1;
    const float xm = s.x + hx;
    const float ym = s.y + hy;
    const float zm = s.z + hz;
    const float ymx = ym - xm;
    const float dx2 = sigma * ymx;
    const float rzm = rho - zm;
    const float xrzm = xm * rzm;
    const float dy2 = xrzm - ym;
    const float xym = xm * ym;
    const float bzm = beta * zm;
    const float dz2 = xym - bzm;
    const float sx = dt * dx2;
    const float sy = dt * dy2;
    const float sz = dt * dz2;
    s.x += sx;
    s.y += sy;
    s.z += sz;
}

class Lfo
{
public:
    /// The curved rows' evaluation grid, in samples.
    static constexpr std::size_t gridSamples = 32;
    /// A jump is slewed over this long, or 1/16 of a cycle if shorter.
    static constexpr float slewSeconds = 0.001f;
    static constexpr double slewCycleFraction = 1.0 / 16.0;
    /// Gain and offset changes ramp over this long.
    static constexpr float rampSeconds = 0.01f;
    /// The Lorenz row: time units per LFO cycle, the largest step, and the
    /// output scale (its x stays within about +-19.6).
    static constexpr float lorenzTimePerCycle = 0.7f;
    static constexpr float lorenzMaxStep = 0.02f;
    static constexpr float lorenzScale = 20.0f;
    /// A gain, an offset, or a jump smaller than this is 0.
    static constexpr float smallest = 0x1p-24f;
    /// The largest phase increment: half a cycle per sample (Nyquist).
    static constexpr std::uint64_t maxIncrement = std::uint64_t{1} << 63U;

    explicit Lfo(std::uint64_t seed = 0) noexcept : seedValue(seed), random(seed) {}

    void prepare(const ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        gainRamp.prepare(spec);
        gainRamp.setRampTime(rampSeconds);
        offsetRamp.prepare(spec);
        offsetRamp.setRampTime(rampSeconds);
        updateRate();
        reset();
    }

    /// From the seed again: the random rows' streams, the Lorenz state, the
    /// cycle's start, and the gain and offset at their targets.
    void reset() noexcept
    {
        random.setSeed(seedValue);
        lorenz.x = 10.0f * random.nextBipolar();
        lorenz.y = 10.0f * random.nextBipolar();
        lorenz.z = 25.0f + (5.0f * random.nextBipolar());
        for (int i = 0; i < 1000; ++i)
        {
            lorenzStep(lorenz, 0.01f);
        }
        gainRamp.setCurrentAndTarget(gainTarget);
        offsetRamp.setCurrentAndTarget(offsetTarget);
        started = false;
        lastWave = 0.0f;
        lastStep = 0;
        restart();
    }

    /// The seed of the random rows, used from the next reset().
    void setSeed(std::uint64_t seed) noexcept
    {
        seedValue = seed;
    }

    /// From the next sample, the phase continuing; the jump is slewed. An
    /// out-of-range value is the sine.
    void setWaveform(LfoWaveform waveform) noexcept
    {
        if (static_cast<std::size_t>(waveform) >= lfoWaveformCount)
        {
            waveform = LfoWaveform::sine;
        }
        if (waveform == current)
        {
            return;
        }
        current = waveform;
        const float before = lastWave;
        enter(true);
        if (started)
        {
            slewFrom(before);
        }
    }

    /// Hz, clamped to [0, Nyquist]; NaN is ignored. The phase is continuous;
    /// a curved row takes it from its next grid point.
    void setFrequency(float hz) noexcept
    {
        if (std::isnan(hz))
        {
            return;
        }
        rateHz = std::max(hz, 0.0f);
        rateIsIncrement = false;
        updateRate();
    }

    /// The rate as an exact phase increment: cycles per sample x 2^64,
    /// clamped to half a cycle. For tempo sync (notePhaseIncrement() in
    /// note_value.hpp), whose rate no float frequency holds exactly. The
    /// phase is continuous; a curved row takes it from its next grid point.
    /// It is per sample, so prepare() keeps it (at another sample rate, set
    /// it again); frequency() reports it in Hz.
    void setPhaseIncrement(std::uint64_t value) noexcept
    {
        increment = std::min(value, maxIncrement);
        rateIsIncrement = true;
        updateRate();
    }

    /// Clamped to [0, 1], below `smallest` 0; NaN is ignored. Ramps over 10 ms.
    void setGain(float value) noexcept
    {
        if (std::isnan(value))
        {
            return;
        }
        value = std::clamp(value, 0.0f, 1.0f);
        gainTarget = value < smallest ? 0.0f : value;
        gainRamp.setTarget(gainTarget);
    }

    /// Clamped to [-1, 1], below `smallest` in size 0; NaN is ignored. Ramps
    /// over 10 ms.
    void setOffset(float value) noexcept
    {
        if (std::isnan(value))
        {
            return;
        }
        value = std::clamp(value, -1.0f, 1.0f);
        offsetTarget = std::abs(value) < smallest ? 0.0f : value;
        offsetRamp.setTarget(offsetTarget);
    }

    [[nodiscard]] LfoWaveform waveform() const noexcept
    {
        return current;
    }

    [[nodiscard]] float frequency() const noexcept
    {
        return rateHz;
    }

    [[nodiscard]] float gain() const noexcept
    {
        return gainTarget;
    }

    [[nodiscard]] float offset() const noexcept
    {
        return offsetTarget;
    }

    /// The cycle's start at once, with no slew: for a voice starting from
    /// silence. The random rows draw new values; the Lorenz row runs on.
    void restart() noexcept
    {
        position = static_cast<std::uint64_t>(shape().start) << 32U;
        enter(false);
        slewTotal = 0;
        slewCount = 0;
    }

    /// The cycle's start, with the jump from the current value slewed: for
    /// a note that retriggers a sounding voice. Before the first sample it
    /// is restart().
    void retrigger() noexcept
    {
        if (!started)
        {
            restart();
            return;
        }
        const float before = lastWave;
        position = static_cast<std::uint64_t>(shape().start) << 32U;
        enter(true);
        slewFrom(before);
    }

    void apply(LfoTrigger trigger) noexcept
    {
        if (trigger == LfoTrigger::restart)
        {
            restart();
        }
        else
        {
            retrigger();
        }
    }

    /// One sample: the same value process() gives.
    [[nodiscard]] float next() noexcept
    {
        float y = 0.0f;
        process(std::span{&y, 1});
        return y;
    }

    /// The next out.size() samples.
    void process(std::span<float> out) noexcept
    {
        std::size_t n = 0;
        while (n < out.size())
        {
            const auto rest = out.subspan(n);
            std::size_t run = 0;
            const LfoShape& row = shape();
            if (row.isCurved())
            {
                run = curvedRun(rest);
            }
            else if (row.kind == LfoKind::sampleHold)
            {
                run = holdRun(rest);
            }
            else
            {
                run = linearRun(rest);
            }
            applySlew(rest.first(run));
            lastWave = rest[run - 1];
            n += run;
        }
        if (!out.empty())
        {
            started = true;
        }
        applyGainAndOffset(out);
    }

    /// As process(out), with each trigger before the sample at its offset.
    /// Events must be in order of offset; offsets at or past the end apply
    /// after the block.
    void process(std::span<float> out, std::span<const LfoEvent> events) noexcept
    {
        std::size_t start = 0;
        for (const auto& event : events)
        {
            const auto at = std::clamp(event.offset, start, out.size());
            process(out.subspan(start, at - start));
            apply(event.trigger);
            start = at;
        }
        process(out.subspan(start));
    }

    /// The next sample's position, 2^64 per cycle.
    [[nodiscard]] std::uint64_t phase() const noexcept
    {
        return position;
    }

    /// The phase step per sample, 2^64 per cycle: the exact rate.
    [[nodiscard]] std::uint64_t phaseIncrement() const noexcept
    {
        return increment;
    }

    /// The length of a slew at the current rate, in samples.
    [[nodiscard]] std::size_t slewSamples() const noexcept
    {
        return slewLength;
    }

private:
    [[nodiscard]] const LfoShape& shape() const noexcept
    {
        return lfoShape(current);
    }

    [[nodiscard]] static std::uint32_t top(std::uint64_t value) noexcept
    {
        return static_cast<std::uint32_t>(value >> 32U);
    }

    void updateRate() noexcept
    {
        if (!(sampleRate > 0.0f))
        {
            return;
        }
        const auto fs = static_cast<double>(sampleRate);
        double cycles = 0.0;
        if (rateIsIncrement)
        {
            cycles = std::ldexp(static_cast<double>(increment), -64);
            rateHz = static_cast<float>(cycles * fs);
        }
        else
        {
            cycles = std::min(static_cast<double>(rateHz) / fs, 0.5);
            increment = static_cast<std::uint64_t>(std::round(std::ldexp(cycles, 64)));
        }
        double samples = static_cast<double>(slewSeconds) * fs;
        if (cycles > 0.0)
        {
            samples = std::min(samples, slewCycleFraction / cycles);
        }
        slewLength = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(samples)));
        // The Lorenz row's time per grid segment, in steps of at most
        // lorenzMaxStep.
        const double time = static_cast<double>(gridSamples) * cycles * static_cast<double>(lorenzTimePerCycle);
        const double steps = std::max(1.0, std::ceil(time / static_cast<double>(lorenzMaxStep)));
        lorenzSteps = static_cast<std::uint32_t>(steps);
        lorenzDt = static_cast<float>(time / steps);
    }

    // The per-row state on entering a row or a new cycle. `continuing`: the
    // smoothed random starts from the current value rather than a new draw.
    void enter(bool continuing) noexcept
    {
        segmentValid = false;
        crossingPending = false;
        gridCount = 0;
        gridFresh = true;
        switch (shape().kind)
        {
        case LfoKind::sampleHold:
            held = random.nextBipolar();
            break;
        case LfoKind::smoothRandom:
            from = continuing ? lastWave : random.nextBipolar();
            to = random.nextBipolar();
            break;
        case LfoKind::periodic:
        case LfoKind::lorenz:
            break;
        }
    }

    // The unslewed value the next sample would have, from the current state.
    [[nodiscard]] float naiveNow() const noexcept
    {
        const LfoShape& row = shape();
        switch (row.kind)
        {
        case LfoKind::periodic:
            return row.wave.valueAt(top(position));
        case LfoKind::sampleHold:
            return held;
        case LfoKind::smoothRandom:
            return smoothAt(top(position));
        case LfoKind::lorenz:
            break;
        }
        return lorenz.x / lorenzScale;
    }

    [[nodiscard]] float smoothAt(std::uint32_t at) const noexcept
    {
        const float t = toCycles(at);
        return from + ((to - from) * (t * (t * (3.0f - (2.0f * t)))));
    }

    // Slew from `before` (the last output's wave value) to the new state.
    void slewFrom(float before) noexcept
    {
        startSlew(before - naiveNow());
    }

    void startSlew(float offsetValue) noexcept
    {
        if (std::abs(offsetValue) < smallest)
        {
            slewTotal = 0;
            slewCount = 0;
            return;
        }
        slewStart = offsetValue;
        slewTotal = slewLength;
        slewCount = 0;
    }

    [[nodiscard]] float slewTerm(std::size_t k) const noexcept
    {
        if (k >= slewTotal)
        {
            return 0.0f;
        }
        return slewStart * static_cast<float>(slewTotal - k) / static_cast<float>(slewTotal);
    }

    void applySlew(std::span<float> y) noexcept
    {
        if (slewCount >= slewTotal)
        {
            return;
        }
        const std::size_t m = std::min(y.size(), slewTotal - slewCount);
        for (std::size_t i = 0; i < m; ++i)
        {
            y[i] += slewTerm(slewCount + i);
        }
        slewCount += m;
    }

    // Samples from the next one up to (not including) the first at or past
    // the phase `corner` (2^64 per cycle): a whole cycle if the phase is on
    // it. "Never" at a rate of 0.
    [[nodiscard]] std::uint64_t samplesTo(std::uint64_t corner) const noexcept
    {
        if (increment == 0)
        {
            return std::numeric_limits<std::uint64_t>::max();
        }
        const std::uint64_t distance = corner - position;
        if (distance == 0)
        {
            return (std::numeric_limits<std::uint64_t>::max() / increment) + 1;
        }
        return (distance / increment) + (distance % increment != 0 ? 1U : 0U);
    }

    // The straight segment containing the position, and the next corner.
    void findSegment() noexcept
    {
        const auto corners = shape().wave.corners();
        const WaveCorner& corner = shape().wave.cornerAt(top(position));
        const auto index = static_cast<std::size_t>(&corner - corners.data());
        segmentValue = corner.value;
        segmentSlope = corner.slope;
        segmentCorner = corner.phase;
        nextCorner = static_cast<std::uint64_t>(corners[(index + 1) % corners.size()].phase) << 32U;
        segmentValid = true;
    }

    // The segment after the corners the last step crossed. If any of them
    // jumps, the jump is slewed from the last output: not from the old
    // segment extended past its corner, which at audio rate would overshoot
    // by up to a sample's worth of slope.
    void crossCorners() noexcept
    {
        bool jumped = false;
        for (const auto& corner : shape().wave.corners())
        {
            jumped =
                jumped || (corner.jumps && position - (static_cast<std::uint64_t>(corner.phase) << 32U) < lastStep);
        }
        findSegment();
        if (jumped)
        {
            slewFrom(lastWave);
        }
    }

    std::size_t linearRun(std::span<float> y) noexcept
    {
        if (crossingPending)
        {
            crossingPending = false;
            crossCorners();
        }
        if (!segmentValid)
        {
            findSegment();
        }
        const std::uint64_t available = samplesTo(nextCorner);
        const auto run = static_cast<std::size_t>(std::min<std::uint64_t>(available, y.size()));
        std::uint64_t p = position;
        for (std::size_t i = 0; i < run; ++i)
        {
            y[i] = segmentValue + (segmentSlope * toCycles(top(p) - segmentCorner));
            p += increment;
        }
        position = p;
        lastStep = increment;
        crossingPending = run == available;
        return run;
    }

    std::size_t holdRun(std::span<float> y) noexcept
    {
        if (crossingPending)
        {
            crossingPending = false;
            held = random.nextBipolar();
            slewFrom(lastWave);
        }
        const std::uint64_t available = samplesTo(0);
        const auto run = static_cast<std::size_t>(std::min<std::uint64_t>(available, y.size()));
        std::fill(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(run), held);
        position += static_cast<std::uint64_t>(run) * increment;
        lastStep = increment;
        crossingPending = run == available;
        return run;
    }

    // How many cycle starts lie in the next n steps of `step` from `from`.
    [[nodiscard]] static std::uint64_t wrapsAhead(std::uint64_t from, std::uint64_t step, std::uint64_t n) noexcept
    {
        constexpr std::uint64_t low = 0xFFFFFFFFU;
        const std::uint64_t lowSum = (from & low) + (n * (step & low));
        const std::uint64_t highSum = (from >> 32U) + (n * (step >> 32U)) + (lowSum >> 32U);
        return highSum >> 32U;
    }

    // A grid point: the value here (or the last segment's end) and at the
    // next grid point, and the straight line between.
    void evaluateGrid() noexcept
    {
        gridIncrement = increment;
        const std::uint64_t ahead = position + (static_cast<std::uint64_t>(gridSamples) * increment);
        const LfoShape& row = shape();
        switch (row.kind)
        {
        case LfoKind::periodic:
            gridA = gridFresh ? row.wave.valueAt(top(position)) : gridB;
            gridB = row.wave.valueAt(top(ahead));
            break;
        case LfoKind::smoothRandom:
        {
            gridA = gridFresh ? smoothAt(top(position)) : gridB;
            for (auto w = wrapsAhead(position, increment, gridSamples); w > 0; --w)
            {
                from = to;
                to = random.nextBipolar();
            }
            gridB = smoothAt(top(ahead));
            break;
        }
        case LfoKind::lorenz:
            gridA = gridFresh ? lorenz.x / lorenzScale : gridB;
            for (std::uint32_t s = 0; s < lorenzSteps; ++s)
            {
                lorenzStep(lorenz, lorenzDt);
            }
            lorenz.x = flushDenormal(lorenz.x);
            lorenz.y = flushDenormal(lorenz.y);
            lorenz.z = flushDenormal(lorenz.z);
            gridB = lorenz.x / lorenzScale;
            break;
        case LfoKind::sampleHold:
            break;
        }
        gridSlope = (gridB - gridA) / static_cast<float>(gridSamples);
        gridFresh = false;
    }

    std::size_t curvedRun(std::span<float> y) noexcept
    {
        if (gridCount == 0)
        {
            evaluateGrid();
        }
        const std::size_t run = std::min(gridSamples - gridCount, y.size());
        for (std::size_t i = 0; i < run; ++i)
        {
            y[i] = gridA + (gridSlope * static_cast<float>(gridCount + i));
        }
        gridCount += run;
        if (gridCount == gridSamples)
        {
            gridCount = 0;
        }
        position += static_cast<std::uint64_t>(run) * gridIncrement;
        lastStep = gridIncrement;
        return run;
    }

    void applyGainAndOffset(std::span<float> out) noexcept
    {
        if (!gainRamp.isSmoothing() && !offsetRamp.isSmoothing())
        {
            const float g = gainRamp.currentValue();
            const float o = offsetRamp.currentValue();
            for (auto& y : out)
            {
                y = g * (y + o);
            }
            return;
        }
        for (auto& y : out)
        {
            const float g = gainRamp.next();
            const float o = offsetRamp.next();
            y = g * (y + o);
        }
    }

    // Settings.
    float sampleRate = 0.0f;
    std::uint64_t seedValue = 0;
    LfoWaveform current = LfoWaveform::sine;
    bool rateIsIncrement = false; ///< set by setPhaseIncrement(), not setFrequency()
    float rateHz = 1.0f;
    float gainTarget = 1.0f;
    float offsetTarget = 0.0f;
    std::uint64_t increment = 0;
    std::size_t slewLength = 1;
    std::uint32_t lorenzSteps = 1;
    float lorenzDt = 0.0f;

    // State.
    Random random;
    LinearSmoother gainRamp;
    LinearSmoother offsetRamp;
    std::uint64_t position = 0;
    std::uint64_t lastStep = 0;
    bool started = false;
    float lastWave = 0.0f;

    float slewStart = 0.0f;
    std::size_t slewTotal = 0;
    std::size_t slewCount = 0;

    bool segmentValid = false;
    bool crossingPending = false;
    float segmentValue = 0.0f;
    float segmentSlope = 0.0f;
    std::uint32_t segmentCorner = 0;
    std::uint64_t nextCorner = 0;

    float held = 0.0f;
    float from = 0.0f;
    float to = 0.0f;
    LorenzState lorenz;

    std::size_t gridCount = 0;
    bool gridFresh = true;
    std::uint64_t gridIncrement = 0;
    float gridA = 0.0f;
    float gridB = 0.0f;
    float gridSlope = 0.0f;
};

} // namespace anoesis::dsp
