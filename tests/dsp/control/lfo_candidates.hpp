// Test-only prototypes of LFO models, measured by the [lfo-report] and the
// [lfo] benchmarks to choose one (docs/dsp/lfos.md).
//
// The waveforms are data, as in the oscillators: a constexpr table of rows,
// each a kind (periodic, or one of the random generators), a WaveShape for
// the periodic ones (reusing the oscillator's rows where the names match),
// and the phase a retrigger starts at. A new periodic waveform is a row.
//
// One engine, Lfo<PhaseKind, Edges>, runs every row, so the report compares
// one axis at a time:
//
// - PhaseKind: how the phase is accumulated. float or double in [0, 1), or
//   fixed point in 32 bits (as MinBlepOscillator) or 64 bits (32.32).
// - Edges: what happens at a jump (a corner with a step, a sample-and-hold
//   step, a retrigger). Naive: the step as it is. Slew: the step spread
//   linearly over a fixed time (1 ms by default), optionally at most a
//   fraction of a cycle (setSlewCycleFraction). PolyBLEP: the 2-point
//   polynomial band-limited step, with one sample of latency. MinBLEP: the
//   oscillator's 64-tap minimum-phase step residual, steps only (no BLAMP
//   at bends, no slope delay compensation).
//
// Two more models sit beside it:
//
// - OscillatorLfo: the oscillator itself (MinBlepOscillator<64>) for the
//   periodic rows: steps and bends band-limited, slope compensated, a
//   retrigger as its band-limited jumpTo(), rate steps as band-limited
//   bends. The random rows run on Lfo<fixed32, minBlep>.
// - ControlRateLfo<N, Interp>: fixed-point phase, naive edges, evaluated
//   once per N samples and held, or linearly interpolated towards the next
//   grid point. For the evaluation-rate axis.
//
// Shared rules: next() returns one sample; a control or a retrigger between
// two next() calls applies from the following sample, so a caller that
// splits its blocks at event offsets is sample-accurate. The first sample
// after reset() is the row's start value. The output is
// gain x (wave + offset), unclipped (the developer's Gain and Offset
// numbers, inventory L6).

#pragma once

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/oscillators/waveform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace anoesis::test::lfo
{

/// What generates a row's values.
enum class Kind : std::uint8_t
{
    periodic,     ///< a WaveShape, read at the phase
    sampleHold,   ///< a new uniform value at the start of each cycle, held
    smoothRandom, ///< a new uniform value per cycle, joined by smoothstep
    lorenz,       ///< the x of a Lorenz attractor, its time scaled by the rate
};

/// A row of the waveform table.
struct Shape
{
    const char* name = "";
    Kind kind = Kind::periodic;
    dsp::WaveShape wave;
    std::uint32_t start = 0; ///< where a retrigger starts the phase (2^32 per cycle)
};

/// The rows, in the order of Vintage Synth's list (inventory L1), plus a
/// candidate for the 11th waveform the official specification counts.
enum class Wave : std::uint8_t
{
    sine,
    saw,
    ramp,
    triangle,
    pulse,
    hpulse,
    qpulse,
    trisaw,
    lorenz,
    sampleHold,
    smooth,
};

inline constexpr std::size_t waveCount = 11;
inline constexpr std::uint32_t quarterCycle = 0x40000000U;

namespace detail
{

[[nodiscard]] constexpr const dsp::WaveShape& oscillatorShape(dsp::Waveform waveform) noexcept
{
    return dsp::waveShapes[static_cast<std::size_t>(waveform)];
}

} // namespace detail

/// The table. Start phases are working hypotheses (inventory L4): the sine
/// starts at its positive peak (one user's report); the triangle at its
/// peak; the ramp falls from the top; the saw, the pulses, and the TriSaw
/// start where the oscillator's do.
inline constexpr std::array<Shape, waveCount> shapes{
    Shape{
        .name = "Sine",
        .kind = Kind::periodic,
        .wave = dsp::WaveShape::sine(),
        .start = quarterCycle,
    },
    Shape{
        .name = "Saw",
        .kind = Kind::periodic,
        .wave = detail::oscillatorShape(dsp::Waveform::saw),
        .start = 0,
    },
    Shape{
        .name = "Ramp",
        .kind = Kind::periodic,
        .wave = dsp::WaveShape::piecewiseLinear(std::array{dsp::WaveSegment{.start = 0.0, .from = 1.0, .to = -1.0}}),
        .start = 0,
    },
    Shape{
        .name = "Triangle",
        .kind = Kind::periodic,
        .wave = detail::oscillatorShape(dsp::Waveform::triangle),
        .start = dsp::halfCycle,
    },
    Shape{
        .name = "Pulse",
        .kind = Kind::periodic,
        .wave = detail::oscillatorShape(dsp::Waveform::pulse),
        .start = 0,
    },
    Shape{
        .name = "HPulse",
        .kind = Kind::periodic,
        .wave = detail::oscillatorShape(dsp::Waveform::hpulse),
        .start = 0,
    },
    Shape{
        .name = "QPulse",
        .kind = Kind::periodic,
        .wave = detail::oscillatorShape(dsp::Waveform::qpulse),
        .start = 0,
    },
    Shape{
        .name = "TriSaw",
        .kind = Kind::periodic,
        .wave = detail::oscillatorShape(dsp::Waveform::trisaw),
        .start = 0,
    },
    Shape{
        .name = "Lorenz",
        .kind = Kind::lorenz,
        .wave = {},
        .start = 0,
    },
    Shape{
        .name = "S&H",
        .kind = Kind::sampleHold,
        .wave = {},
        .start = 0,
    },
    Shape{
        .name = "Smooth",
        .kind = Kind::smoothRandom,
        .wave = {},
        .start = 0,
    },
};

[[nodiscard]] constexpr const Shape& shapeOf(Wave wave) noexcept
{
    return shapes[static_cast<std::size_t>(wave)];
}

/// The Lorenz system (sigma 10, rho 28, beta 8/3). One LFO cycle is
/// lorenzTimePerCycle time units (calibrated by the report so the spectrum's
/// peak sits near the rate), the step is capped at lorenzMaxStep for
/// stability, and the output is x / lorenzScale.
inline constexpr float lorenzTimePerCycle = 0.7f;
inline constexpr float lorenzMaxStep = 0.02f;
inline constexpr float lorenzScale = 20.0f;

/// A retrigger at a sample offset within a block.
struct LfoEvent
{
    std::size_t offset = 0;
};

enum class PhaseKind : std::uint8_t
{
    float32,
    double64,
    fixed32,
    fixed64,
};

/// A phase accumulator in one representation. The phase as 2^32 units per
/// cycle is phase(), whatever the storage.
template <PhaseKind P>
class Accumulator
{
public:
    using Storage = std::conditional_t<
        P == PhaseKind::float32, float,
        std::conditional_t<P == PhaseKind::double64, double,
                           std::conditional_t<P == PhaseKind::fixed32, std::uint32_t, std::uint64_t>>>;

    /// The increment for `cycles` per sample, clamped to [0, 1/2], rounded
    /// to the representation.
    void setCycles(double cycles) noexcept
    {
        const double c = std::isnan(cycles) ? 0.0 : std::clamp(cycles, 0.0, 0.5);
        if constexpr (P == PhaseKind::float32)
        {
            increment = static_cast<float>(c);
        }
        else if constexpr (P == PhaseKind::double64)
        {
            increment = c;
        }
        else if constexpr (P == PhaseKind::fixed32)
        {
            increment = static_cast<std::uint32_t>(std::llround(std::ldexp(c, 32)));
        }
        else
        {
            increment = static_cast<std::uint64_t>(std::round(std::ldexp(c, 64)));
        }
    }

    void setPhase(std::uint32_t units) noexcept
    {
        if constexpr (P == PhaseKind::float32)
        {
            value = static_cast<float>(std::ldexp(static_cast<double>(units), -32));
            if (value >= 1.0f)
            {
                value = 0.0f;
            }
        }
        else if constexpr (P == PhaseKind::double64)
        {
            value = std::ldexp(static_cast<double>(units), -32);
        }
        else if constexpr (P == PhaseKind::fixed32)
        {
            value = units;
        }
        else
        {
            value = static_cast<std::uint64_t>(units) << 32U;
        }
    }

    void advance() noexcept
    {
        value += increment;
        if constexpr (P == PhaseKind::float32 || P == PhaseKind::double64)
        {
            if (value >= Storage{1})
            {
                value -= Storage{1};
            }
        }
    }

    [[nodiscard]] std::uint32_t phase() const noexcept
    {
        if constexpr (P == PhaseKind::float32)
        {
            return static_cast<std::uint32_t>(value * 4294967296.0f);
        }
        else if constexpr (P == PhaseKind::double64)
        {
            return static_cast<std::uint32_t>(value * 4294967296.0);
        }
        else if constexpr (P == PhaseKind::fixed32)
        {
            return value;
        }
        else
        {
            return static_cast<std::uint32_t>(value >> 32U);
        }
    }

    /// The phase in cycles, [0, 1), as exactly as double holds it.
    [[nodiscard]] double cycles() const noexcept
    {
        if constexpr (P == PhaseKind::float32 || P == PhaseKind::double64)
        {
            return static_cast<double>(value);
        }
        else if constexpr (P == PhaseKind::fixed32)
        {
            return std::ldexp(static_cast<double>(value), -32);
        }
        else
        {
            return std::ldexp(static_cast<double>(value), -64);
        }
    }

    /// The increment in cycles, as exactly as double holds it.
    [[nodiscard]] double incrementCycles() const noexcept
    {
        if constexpr (P == PhaseKind::float32 || P == PhaseKind::double64)
        {
            return static_cast<double>(increment);
        }
        else if constexpr (P == PhaseKind::fixed32)
        {
            return std::ldexp(static_cast<double>(increment), -32);
        }
        else
        {
            return std::ldexp(static_cast<double>(increment), -64);
        }
    }

private:
    Storage value{};
    Storage increment{};
};

enum class Edges : std::uint8_t
{
    naive,
    slew,
    polyBlep,
    minBlep,
};

/// The value of a periodic row at `phase`, unfiltered.
[[nodiscard]] inline float periodicValue(const Shape& shape, std::uint32_t phase) noexcept
{
    return shape.wave.valueAt(phase);
}

/// The Lorenz attractor's state, advanced by midpoint (RK2) steps.
struct Lorenz
{
    float x = 1.0f;
    float y = 1.0f;
    float z = 25.0f;

    void step(float dt) noexcept
    {
        constexpr float sigma = 10.0f;
        constexpr float rho = 28.0f;
        constexpr float beta = 8.0f / 3.0f;
        const float dx1 = sigma * (y - x);
        const float dy1 = (x * (rho - z)) - y;
        const float dz1 = (x * y) - (beta * z);
        const float h = 0.5f * dt;
        const float xm = x + (h * dx1);
        const float ym = y + (h * dy1);
        const float zm = z + (h * dz1);
        x += dt * (sigma * (ym - xm));
        y += dt * ((xm * (rho - zm)) - ym);
        z += dt * ((xm * ym) - (beta * zm));
    }

    /// A start from three random draws, settled onto the attractor.
    void start(dsp::Random& random) noexcept
    {
        x = 10.0f * random.nextBipolar();
        y = 10.0f * random.nextBipolar();
        z = 25.0f + (5.0f * random.nextBipolar());
        for (int i = 0; i < 1000; ++i)
        {
            step(0.01f);
        }
    }
};

/// The LFO engine: a row of the table, a phase accumulator, and a treatment
/// of jumps. See the file comment.
template <PhaseKind P, Edges E>
class Lfo
{
public:
    static constexpr std::size_t taps = dsp::minBlepTaps;

    /// `table`: the minBLEP step residuals, needed by Edges::minBlep only.
    explicit Lfo(const dsp::MinBlepTable* table = nullptr) noexcept : residuals(table) {}

    void prepare(const dsp::ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        updateIncrement();
        updateTimes();
        reset();
    }

    void reset() noexcept
    {
        random.reset();
        accumulator.setPhase(row().start);
        step = 0;
        started = false;
        held = random.nextBipolar();
        from = random.nextBipolar();
        to = random.nextBipolar();
        lorenz.start(random);
        lastNaive = 0.0f;
        slewValue = 0.0f;
        slewStep = 0.0f;
        slewLeft = 0;
        blepNow = 0.0f;
        pending = 0.0f;
        pendingValid = false;
        ring.fill(0.0f);
        position = 0;
        fadeLeft = 0;
    }

    void setWave(Wave newWave) noexcept
    {
        wave = newWave;
    }

    [[nodiscard]] Wave currentWave() const noexcept
    {
        return wave;
    }

    /// Hz; phase continuous. NaN is 0.
    void setFrequency(float hz) noexcept
    {
        frequency = std::isnan(hz) ? 0.0f : std::max(hz, 0.0f);
        updateIncrement();
    }

    /// Applied at the next reset().
    void setSeed(std::uint64_t seed) noexcept
    {
        random.setSeed(seed);
    }

    void setSlewSeconds(float seconds) noexcept
    {
        slewSeconds = std::max(seconds, 0.0f);
        updateTimes();
    }

    /// More than 0: the slew lasts at most this fraction of a cycle, so a
    /// fast LFO keeps its shape.
    void setSlewCycleFraction(float fraction) noexcept
    {
        slewCycleFraction = std::max(fraction, 0.0f);
        updateTimes();
    }

    /// 0: a retrigger jumps (through the Edges treatment). More: a
    /// retrigger of a periodic row crossfades from the old trajectory over
    /// this time.
    void setRetriggerFade(float seconds) noexcept
    {
        fadeSeconds = std::max(seconds, 0.0f);
        updateTimes();
    }

    void setGain(float value) noexcept
    {
        gain = value;
    }

    void setOffset(float value) noexcept
    {
        offset = value;
    }

    /// Restarts the cycle at the row's start phase, from the next sample.
    void retrigger() noexcept
    {
        const Shape& shape = row();
        if (!started)
        {
            accumulator.setPhase(shape.start);
            step = 0;
            return;
        }
        switch (shape.kind)
        {
        case Kind::periodic:
            if (fadeSamples > 0)
            {
                fadeFrom = accumulator;
                fadeLeft = fadeSamples;
                accumulator.setPhase(shape.start);
            }
            else
            {
                const float before = periodicValue(shape, accumulator.phase());
                accumulator.setPhase(shape.start);
                addEdge(periodicValue(shape, shape.start) - before, 0.0f);
            }
            break;
        case Kind::sampleHold:
        {
            const float next = random.nextBipolar();
            addEdge(next - held, 0.0f);
            held = next;
            accumulator.setPhase(shape.start);
            break;
        }
        case Kind::smoothRandom:
            from = lastNaive;
            to = random.nextBipolar();
            accumulator.setPhase(shape.start);
            break;
        case Kind::lorenz:
            break;
        }
        step = 0;
    }

    [[nodiscard]] float next() noexcept
    {
        const Shape& shape = row();
        const std::uint32_t now = accumulator.phase();
        float naive = 0.0f;
        switch (shape.kind)
        {
        case Kind::periodic:
            if (step != 0)
            {
                for (const auto& corner : shape.wave.corners())
                {
                    const std::uint32_t past = now - corner.phase;
                    if (corner.jumps && past < step)
                    {
                        addEdge(corner.jump, static_cast<float>(past) / static_cast<float>(step));
                    }
                }
            }
            naive = periodicValue(shape, now);
            break;
        case Kind::sampleHold:
            if (step != 0 && now < step)
            {
                const float next = random.nextBipolar();
                addEdge(next - held, static_cast<float>(now) / static_cast<float>(step));
                held = next;
            }
            naive = held;
            break;
        case Kind::smoothRandom:
        {
            if (step != 0 && now < step)
            {
                from = to;
                to = random.nextBipolar();
            }
            const float t = dsp::toCycles(now);
            naive = from + ((to - from) * (t * t * (3.0f - (2.0f * t))));
            break;
        }
        case Kind::lorenz:
            lorenz.step(
                std::min(static_cast<float>(accumulator.incrementCycles()) * lorenzTimePerCycle, lorenzMaxStep));
            naive = lorenz.x / lorenzScale;
            break;
        }
        if (fadeLeft > 0)
        {
            const float old = periodicValue(shape, fadeFrom.phase());
            fadeFrom.advance();
            const float weight = static_cast<float>(fadeSamples - fadeLeft) / static_cast<float>(fadeSamples);
            naive = old + ((naive - old) * weight);
            --fadeLeft;
        }
        lastNaive = naive;
        const float y = treat(naive);
        started = true;
        accumulator.advance();
        step = accumulator.phase() - now;
        return gain * (y + offset);
    }

    void process(std::span<float> out) noexcept
    {
        for (auto& y : out)
        {
            y = next();
        }
    }

    /// As process(out), with each retrigger before the sample at its offset.
    void process(std::span<float> out, std::span<const LfoEvent> events) noexcept
    {
        std::size_t start = 0;
        for (const auto& event : events)
        {
            const auto at = std::clamp(event.offset, start, out.size());
            process(out.subspan(start, at - start));
            retrigger();
            start = at;
        }
        process(out.subspan(start));
    }

    /// True if any internal state is subnormal.
    [[nodiscard]] bool hasSubnormalState() const noexcept
    {
        const auto subnormal = [](float v)
        {
            return std::fpclassify(v) == FP_SUBNORMAL;
        };
        return subnormal(slewValue) || subnormal(slewStep) || subnormal(pending) || subnormal(blepNow) ||
               subnormal(held) || subnormal(from) || subnormal(to) || subnormal(lorenz.x) || subnormal(lorenz.y) ||
               subnormal(lorenz.z) || std::ranges::any_of(ring, subnormal);
    }

    [[nodiscard]] const Accumulator<P>& phase() const noexcept
    {
        return accumulator;
    }

    /// Samples by which edges come out late: 1 for PolyBLEP (it needs the
    /// sample before an edge), the kernel's delay for minBLEP, else 0.
    [[nodiscard]] float latencySamples() const noexcept
    {
        if constexpr (E == Edges::polyBlep)
        {
            return 1.0f;
        }
        else if constexpr (E == Edges::minBlep)
        {
            return residuals->delay();
        }
        else
        {
            return 0.0f;
        }
    }

private:
    [[nodiscard]] const Shape& row() const noexcept
    {
        return shapeOf(wave);
    }

    void updateIncrement() noexcept
    {
        if (sampleRate > 0.0f)
        {
            accumulator.setCycles(static_cast<double>(frequency) / static_cast<double>(sampleRate));
        }
        if (slewCycleFraction > 0.0f)
        {
            updateTimes();
        }
    }

    void updateTimes() noexcept
    {
        float samples = slewSeconds * sampleRate;
        const auto cycle = static_cast<float>(accumulator.incrementCycles());
        if (slewCycleFraction > 0.0f && cycle > 0.0f)
        {
            samples = std::min(samples, slewCycleFraction / cycle);
        }
        slewSamples = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(samples)));
        fadeSamples = static_cast<std::uint32_t>(std::lround(fadeSeconds * sampleRate));
    }

    // A jump of `height` in the naive output, `since` samples before this
    // sample (which already holds the new value).
    void addEdge(float height, float since) noexcept
    {
        if constexpr (E == Edges::slew)
        {
            slewValue -= height;
            slewStep = -slewValue / static_cast<float>(slewSamples);
            slewLeft = slewSamples;
        }
        else if constexpr (E == Edges::polyBlep)
        {
            // The residual of a 2-point PolyBLEP step: +s^2/2 on the sample
            // before the edge, -(1 - s)^2/2 on the sample after it.
            pending += height * 0.5f * since * since;
            blepNow -= height * 0.5f * (1.0f - since) * (1.0f - since);
        }
        else if constexpr (E == Edges::minBlep)
        {
            const float scaled = since * static_cast<float>(residuals->phases());
            auto p = static_cast<std::size_t>(scaled);
            p = std::min(p, residuals->phases() - 1);
            const float w = scaled - static_cast<float>(p);
            const float* r0 = residuals->stepRow(p);
            const float* r1 = residuals->stepRow(p + 1);
            for (std::size_t i = 0; i < taps; ++i)
            {
                ring[(position + i) & (taps - 1)] += height * (r0[i] + (w * (r1[i] - r0[i])));
            }
        }
        else
        {
            static_cast<void>(height);
            static_cast<void>(since);
        }
    }

    float treat(float naive) noexcept
    {
        if constexpr (E == Edges::slew)
        {
            const float y = naive + slewValue;
            if (slewLeft > 0)
            {
                slewValue += slewStep;
                if (--slewLeft == 0)
                {
                    slewValue = 0.0f;
                    slewStep = 0.0f;
                }
            }
            return y;
        }
        else if constexpr (E == Edges::polyBlep)
        {
            const float y = naive + blepNow;
            blepNow = 0.0f;
            if (!pendingValid)
            {
                pending = y;
                pendingValid = true;
            }
            const float out = pending;
            pending = y;
            return out;
        }
        else if constexpr (E == Edges::minBlep)
        {
            const float y = naive + ring[position];
            ring[position] = 0.0f;
            position = (position + 1) & (taps - 1);
            return y;
        }
        else
        {
            return naive;
        }
    }

    const dsp::MinBlepTable* residuals = nullptr;
    float sampleRate = 0.0f;
    Wave wave = Wave::sine;
    float frequency = 1.0f;
    float slewSeconds = 0.001f;
    float slewCycleFraction = 0.0f;
    float fadeSeconds = 0.0f;
    float gain = 1.0f;
    float offset = 0.0f;

    dsp::Random random{0};
    Accumulator<P> accumulator;
    std::uint32_t step = 0;
    bool started = false;

    float held = 0.0f;
    float from = 0.0f;
    float to = 0.0f;
    Lorenz lorenz;
    float lastNaive = 0.0f;

    std::uint32_t slewSamples = 1;
    float slewValue = 0.0f;
    float slewStep = 0.0f;
    std::uint32_t slewLeft = 0;

    float blepNow = 0.0f;
    float pending = 0.0f;
    bool pendingValid = false;

    std::array<float, taps> ring{};
    std::size_t position = 0;

    std::uint32_t fadeSamples = 0;
    std::uint32_t fadeLeft = 0;
    Accumulator<P> fadeFrom;
};

/// The oscillator as an LFO: MinBlepOscillator<64> for the periodic rows,
/// Lfo<fixed32, minBlep> for the random ones.
class OscillatorLfo
{
public:
    explicit OscillatorLfo(const dsp::MinBlepTable& table) noexcept : oscillator(table), randomRows(&table) {}

    void prepare(const dsp::ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        oscillator.prepare(spec);
        randomRows.prepare(spec);
        setWave(wave);
        setFrequency(frequency);
        reset();
    }

    void reset() noexcept
    {
        oscillator.restart(shapeOf(wave).start);
        randomRows.reset();
    }

    void setWave(Wave newWave) noexcept
    {
        wave = newWave;
        randomRows.setWave(newWave);
        if (isPeriodic())
        {
            oscillator.setShape(shapeOf(newWave).wave);
        }
    }

    void setFrequency(float hz) noexcept
    {
        frequency = std::isnan(hz) ? 0.0f : std::max(hz, 0.0f);
        if (sampleRate > 0.0f)
        {
            oscillator.setIncrement(dsp::incrementFor(frequency, sampleRate));
        }
        randomRows.setFrequency(frequency);
    }

    void setSeed(std::uint64_t seed) noexcept
    {
        randomRows.setSeed(seed);
    }

    void setGain(float value) noexcept
    {
        gain = value;
        randomRows.setGain(value);
    }

    void setOffset(float value) noexcept
    {
        offset = value;
        randomRows.setOffset(value);
    }

    void retrigger() noexcept
    {
        if (!isPeriodic())
        {
            randomRows.retrigger();
        }
        else if (oscillator.isRunning())
        {
            oscillator.jumpTo(shapeOf(wave).start);
        }
        else
        {
            oscillator.restart(shapeOf(wave).start);
        }
    }

    [[nodiscard]] float next() noexcept
    {
        return isPeriodic() ? gain * (oscillator.next() + offset) : randomRows.next();
    }

    void process(std::span<float> out) noexcept
    {
        for (auto& y : out)
        {
            y = next();
        }
    }

    void process(std::span<float> out, std::span<const LfoEvent> events) noexcept
    {
        std::size_t start = 0;
        for (const auto& event : events)
        {
            const auto at = std::clamp(event.offset, start, out.size());
            process(out.subspan(start, at - start));
            retrigger();
            start = at;
        }
        process(out.subspan(start));
    }

    [[nodiscard]] bool hasSubnormalState() const noexcept
    {
        const auto subnormal = [](float v)
        {
            return std::fpclassify(v) == FP_SUBNORMAL;
        };
        return std::ranges::any_of(oscillator.residuals(), subnormal) || randomRows.hasSubnormalState();
    }

    [[nodiscard]] float latencySamples() const noexcept
    {
        return oscillator.latencySamples();
    }

private:
    [[nodiscard]] bool isPeriodic() const noexcept
    {
        return shapeOf(wave).kind == Kind::periodic;
    }

    dsp::MinBlepOscillator<dsp::minBlepTaps> oscillator;
    Lfo<PhaseKind::fixed32, Edges::minBlep> randomRows;
    float sampleRate = 0.0f;
    Wave wave = Wave::sine;
    float frequency = 1.0f;
    float gain = 1.0f;
    float offset = 0.0f;
};

/// A periodic row evaluated once per N samples on a grid counted from the
/// last reset or retrigger: held, or (Interp) a straight line from this grid
/// point's value to the next one's. Fixed-point phase, naive edges.
template <std::size_t N, bool Interp>
class ControlRateLfo
{
public:
    void prepare(const dsp::ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        setFrequency(frequency);
        reset();
    }

    void reset() noexcept
    {
        phase = shapeOf(wave).start;
        count = 0;
        fresh = true;
    }

    void setWave(Wave newWave) noexcept
    {
        wave = newWave;
    }

    /// Applies from the next grid point.
    void setFrequency(float hz) noexcept
    {
        frequency = std::isnan(hz) ? 0.0f : std::max(hz, 0.0f);
        if (sampleRate > 0.0f)
        {
            increment = dsp::incrementFor(frequency, sampleRate);
        }
    }

    void setGain(float value) noexcept
    {
        gain = value;
    }

    void setOffset(float value) noexcept
    {
        offset = value;
    }

    void retrigger() noexcept
    {
        phase = shapeOf(wave).start;
        count = 0;
        fresh = true;
    }

    [[nodiscard]] float next() noexcept
    {
        if (count == 0)
        {
            const Shape& shape = shapeOf(wave);
            const auto span = static_cast<std::uint32_t>(N) * increment;
            if constexpr (Interp)
            {
                a = fresh ? periodicValue(shape, phase) : b;
                b = periodicValue(shape, phase + span);
                slope = (b - a) / static_cast<float>(N);
            }
            else
            {
                a = periodicValue(shape, phase);
            }
            fresh = false;
            phase += span;
        }
        const float y = Interp ? a + (slope * static_cast<float>(count)) : a;
        count = count + 1 == N ? 0 : count + 1;
        return gain * (y + offset);
    }

    /// The same values as next(), a grid segment at a time (the loop over a
    /// segment vectorizes).
    void process(std::span<float> out) noexcept
    {
        std::size_t n = 0;
        while (n < out.size())
        {
            if (count == 0)
            {
                out[n] = next(); // evaluates the grid point
                ++n;
                continue;
            }
            const std::size_t run = std::min(N - count, out.size() - n);
            float* const y = out.data() + n;
            for (std::size_t i = 0; i < run; ++i)
            {
                const float value = Interp ? a + (slope * static_cast<float>(count + i)) : a;
                y[i] = gain * (value + offset);
            }
            n += run;
            count = count + run == N ? 0 : count + run;
        }
    }

    void process(std::span<float> out, std::span<const LfoEvent> events) noexcept
    {
        std::size_t start = 0;
        for (const auto& event : events)
        {
            const auto at = std::clamp(event.offset, start, out.size());
            process(out.subspan(start, at - start));
            retrigger();
            start = at;
        }
        process(out.subspan(start));
    }

    [[nodiscard]] bool hasSubnormalState() const noexcept
    {
        const auto subnormal = [](float v)
        {
            return std::fpclassify(v) == FP_SUBNORMAL;
        };
        return subnormal(a) || subnormal(b) || subnormal(slope);
    }

private:
    float sampleRate = 0.0f;
    Wave wave = Wave::sine;
    float frequency = 1.0f;
    float gain = 1.0f;
    float offset = 0.0f;
    std::uint32_t increment = 0;
    std::uint32_t phase = 0;
    std::size_t count = 0;
    bool fresh = true;
    float a = 0.0f;
    float b = 0.0f;
    float slope = 0.0f;
};

/// Any of the models above as a BlockProcessor, writing its output to every
/// channel, for checkLifecycleContract().
template <typename L>
class AsProcessor
{
public:
    explicit AsProcessor(L model) noexcept : lfo(model) {}

    void prepare(const dsp::ProcessSpec& spec) noexcept
    {
        lfo.prepare(spec);
    }

    void reset() noexcept
    {
        lfo.reset();
    }

    void process(const dsp::AudioBlock& block) noexcept
    {
        for (std::size_t n = 0; n < block.numSamples(); ++n)
        {
            const float y = lfo.next();
            for (std::size_t c = 0; c < block.numChannels(); ++c)
            {
                block.channel(c)[n] = y;
            }
        }
    }

    L lfo;
};

} // namespace anoesis::test::lfo
