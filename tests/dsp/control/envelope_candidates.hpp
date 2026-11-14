// Test-only prototypes of ADSR envelope segment models, measured by the
// [envelope-report] and the [envelope] benchmarks to choose one
// (docs/dsp/envelopes.md). Every prototype shares one stage machine and one
// set of rules, so the report compares the curves and their arithmetic, not
// incidental differences:
//
// - A stage is a piece of a full-scale curve: the attack runs 0 -> 1 in the
//   attack time, the decay 1 -> sustain in the decay time, the release
//   1 -> 0 in the release time. A stage entered part-way (a release from the
//   sustain level, a retrigger from the current level, a time change
//   mid-stage) enters its full-scale curve at the current level, so its rate
//   is fixed by its time ("constant rate"), and the level never jumps.
// - A stage of N samples ends on its N-th sample, which outputs the stage's
//   end level exactly. A stage of 0 samples is skipped at once: an attack of
//   0 outputs the decay's first value on the gate's own sample.
// - next() advances one sample and returns the level. A gate between two
//   next() calls takes effect on the following sample, so a caller that
//   splits its blocks at event offsets is sample-accurate.
//
// The models:
//
// - Linear: straight segments. Accumulated (y += step, ended by crossing
//   the end level) or counted (y = y0 + step * j, ended by a sample count).
// - Exponential: each segment approaches a target beyond its end level and
//   ends when it crosses the end level (the "overshoot target" ADSR). The
//   ratio r sets the curvature: target = end + r (end - start). Large r is
//   nearly linear; small r is close to a pure exponential. The state is the
//   distance to the target, in float or double, multiplied by the
//   coefficient each sample. Or, counted: the same curve in float as a
//   closed form, distance = d0 * 2^(k log2 c) with fastExp2, ended by a
//   sample count, so no error accumulates.
// - Analog RC: the attack charges towards 1.5 and switches at 1 (r = 0.5,
//   like a comparator at two thirds of the charge voltage); decay and
//   release are pure exponentials towards the sustain level and 0, with the
//   time defined as the time to fall to rcThreshold (-80 dB) of the span.
//   The decay never ends (the sustain stage is the decay running on), and
//   the release ends at rcThreshold. Optionally without flushDenormal(), to
//   measure the denormals in its tails.

#pragma once

#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/math/fast_math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace anoesis::test::envelope
{

enum class Stage : std::uint8_t
{
    idle,
    attack,
    decay,
    sustain,
    release,
};

enum class Kind : std::uint8_t
{
    linearAccumulated,
    linearCounted,
    exponential,
    exponentialCounted,
    analogRc,
};

/// The overshoot ratios of the exponential model: attack, then decay and
/// release.
struct Ratios
{
    double attack = 0.3;
    double decayRelease = 0.001;
};

/// The analog RC model's end threshold, and the level its decay and release
/// times are measured to: -80 dB of the span.
inline constexpr double rcThreshold = 1e-4;

/// The RC model's attack ratio: it charges towards 1.5 and switches at 1.
inline constexpr double rcAttackRatio = 0.5;

template <Kind K, typename Real = float, bool Flush = true>
class Prototype
{
public:
    explicit Prototype(Ratios ratios = {}) noexcept : ratios(ratios) {}

    void prepare(float rate) noexcept
    {
        sampleRate = rate;
        reset();
    }

    void reset() noexcept
    {
        current = Stage::idle;
        level = 0.0f;
        distance = 0;
        accumulated = 0;
    }

    void setAttack(float seconds) noexcept
    {
        attackSeconds = std::max(seconds, 0.0f);
        reenter(Stage::attack);
    }

    void setDecay(float seconds) noexcept
    {
        decaySeconds = std::max(seconds, 0.0f);
        reenter(Stage::decay);
    }

    void setRelease(float seconds) noexcept
    {
        releaseSeconds = std::max(seconds, 0.0f);
        reenter(Stage::release);
    }

    /// The sustain level. In the sustain stage the level jumps to it, except
    /// in the RC model, whose decay runs on towards it at the decay rate. In
    /// the decay stage the decay re-aims from the current level.
    void setSustain(float value) noexcept
    {
        sustainLevel = std::clamp(value, 0.0f, 1.0f);
        if (current == Stage::sustain)
        {
            level = sustainLevel;
        }
        reenter(Stage::decay);
    }

    /// Retrigger while sounding: from the current level (true) or from 0.
    void setRetriggerFromCurrent(bool fromCurrent) noexcept
    {
        retriggerFromCurrent = fromCurrent;
    }

    void gateOn() noexcept
    {
        if (current == Stage::idle || !retriggerFromCurrent)
        {
            level = 0.0f;
        }
        enter(Stage::attack);
    }

    void gateOff() noexcept
    {
        if (current != Stage::idle)
        {
            enter(Stage::release);
        }
    }

    [[nodiscard]] float next() noexcept
    {
        switch (current)
        {
        case Stage::idle:
            return 0.0f;
        case Stage::sustain:
            return level;
        case Stage::attack:
        case Stage::decay:
        case Stage::release:
            step();
            return level;
        }
        return level;
    }

    [[nodiscard]] Stage stage() const noexcept
    {
        return current;
    }

    [[nodiscard]] bool isActive() const noexcept
    {
        return current != Stage::idle;
    }

    [[nodiscard]] float value() const noexcept
    {
        return level;
    }

    /// The model's internal state that recursion can drive subnormal: the
    /// distance to the target (exponential, RC) or the level.
    [[nodiscard]] double state() const noexcept
    {
        if constexpr (K == Kind::exponential || K == Kind::exponentialCounted || K == Kind::analogRc)
        {
            return static_cast<double>(distance);
        }
        else
        {
            return static_cast<double>(level);
        }
    }

private:
    [[nodiscard]] double samplesFor(float seconds) const noexcept
    {
        return std::round(static_cast<double>(seconds) * static_cast<double>(sampleRate));
    }

    // Re-enters `stage` from the current level if it is the current stage
    // (a control change mid-stage).
    void reenter(Stage stage) noexcept
    {
        if (current == stage)
        {
            enter(stage);
        }
    }

    // The full-scale curve of each stage: start, end, length in samples.
    struct Curve
    {
        double start = 0.0;
        double end = 0.0;
        double samples = 0.0;
        double ratio = 0.0;
    };

    [[nodiscard]] Curve curveOf(Stage stage) const noexcept
    {
        switch (stage)
        {
        case Stage::attack:
            return {
                .start = 0.0,
                .end = 1.0,
                .samples = samplesFor(attackSeconds),
                .ratio = K == Kind::analogRc ? rcAttackRatio : ratios.attack,
            };
        case Stage::decay:
            return {
                .start = 1.0,
                .end = static_cast<double>(sustainLevel),
                .samples = samplesFor(decaySeconds),
                .ratio = ratios.decayRelease,
            };
        case Stage::release:
            return {.start = 1.0, .end = 0.0, .samples = samplesFor(releaseSeconds), .ratio = ratios.decayRelease};
        case Stage::idle:
        case Stage::sustain:
            break;
        }
        return {};
    }

    void enter(Stage stage) noexcept
    {
        for (;;)
        {
            current = stage;
            if (stage == Stage::idle || stage == Stage::sustain)
            {
                if (stage == Stage::idle)
                {
                    level = 0.0f;
                }
                return;
            }
            const auto curve = curveOf(stage);
            if (configure(stage, curve))
            {
                return;
            }
            // Zero length, or already at or past the end: skip the stage.
            level = static_cast<float>(curve.end);
            stage = following(stage);
        }
    }

    [[nodiscard]] static Stage following(Stage stage) noexcept
    {
        switch (stage)
        {
        case Stage::attack:
            return Stage::decay;
        case Stage::decay:
            return Stage::sustain;
        case Stage::release:
        case Stage::idle:
        case Stage::sustain:
            break;
        }
        return Stage::idle;
    }

    // Sets up the segment from the current level. False: skip it.
    bool configure(Stage stage, const Curve& curve) noexcept
    {
        const auto from = static_cast<double>(level);
        const double span = curve.end - curve.start;
        const bool rising = span > 0.0;
        ascending = rising;
        endLevel = curve.end;
        if (curve.samples <= 0.0)
        {
            return false;
        }

        if constexpr (K == Kind::analogRc)
        {
            if (stage != Stage::attack)
            {
                // A pure exponential towards the end level: -80 dB of the
                // span in the stage's time. The decay never ends; the
                // release ends at the threshold.
                target = static_cast<Real>(curve.end);
                coefficient = static_cast<Real>(std::pow(rcThreshold, 1.0 / curve.samples));
                distance = static_cast<Real>(from - curve.end);
                return stage == Stage::decay || from > rcThreshold;
            }
        }

        if (rising ? from >= curve.end : from <= curve.end)
        {
            return false;
        }

        if constexpr (K == Kind::linearAccumulated || K == Kind::linearCounted)
        {
            const double fraction = (curve.end - from) / span;
            const double remaining = std::round(fraction * curve.samples);
            if (remaining <= 0.0)
            {
                return false;
            }
            if constexpr (K == Kind::linearCounted)
            {
                origin = static_cast<Real>(from);
                increment = static_cast<Real>((curve.end - from) / remaining);
                count = 0;
                countEnd = static_cast<std::int64_t>(remaining);
            }
            else
            {
                accumulated = static_cast<Real>(from);
                increment = static_cast<Real>(span / curve.samples);
            }
        }
        else
        {
            const double r = curve.ratio;
            const double c = std::pow(r / (1.0 + r), 1.0 / curve.samples);
            target = static_cast<Real>(curve.end + (r * span));
            coefficient = static_cast<Real>(c);
            distance = static_cast<Real>(from) - target;
            endDistance = static_cast<Real>(-r * span);
            if constexpr (K == Kind::exponentialCounted)
            {
                // The samples left from the current level to the end, on the
                // full-scale curve.
                const double d0 = from - (curve.end + (r * span));
                const double remaining = std::round(std::log((-r * span) / d0) / std::log(c));
                if (remaining <= 0.0)
                {
                    return false;
                }
                origin = static_cast<Real>(d0);
                log2Coefficient = static_cast<float>(std::log2(c));
                count = 0;
                countEnd = static_cast<std::int64_t>(remaining);
            }
        }
        return true;
    }

    void finish() noexcept
    {
        level = static_cast<float>(endLevel);
        enter(following(current));
    }

    void step() noexcept
    {
        if constexpr (K == Kind::linearCounted)
        {
            ++count;
            if (count >= countEnd)
            {
                finish();
                return;
            }
            level = static_cast<float>(origin + (increment * static_cast<Real>(count)));
        }
        else if constexpr (K == Kind::linearAccumulated)
        {
            accumulated += increment;
            if (ascending ? accumulated >= static_cast<Real>(endLevel) : accumulated <= static_cast<Real>(endLevel))
            {
                finish();
                return;
            }
            level = static_cast<float>(accumulated);
        }
        else if constexpr (K == Kind::exponentialCounted)
        {
            ++count;
            if (count >= countEnd)
            {
                finish();
                return;
            }
            distance = origin * dsp::fastExp2(log2Coefficient * static_cast<float>(count));
            level = static_cast<float>(target + distance);
        }
        else
        {
            distance *= coefficient;
            if constexpr (Flush && std::is_same_v<Real, float>)
            {
                distance = dsp::flushDenormal(distance);
            }
            if constexpr (K == Kind::analogRc)
            {
                if (current == Stage::decay)
                {
                    level = static_cast<float>(target + distance);
                    return;
                }
                if (current == Stage::release)
                {
                    if (distance < static_cast<Real>(rcThreshold))
                    {
                        finish();
                        return;
                    }
                    level = static_cast<float>(distance);
                    return;
                }
            }
            if (ascending ? distance >= endDistance : distance <= endDistance)
            {
                finish();
                return;
            }
            level = static_cast<float>(target + distance);
        }
    }

    Ratios ratios;
    float sampleRate = 48000.0f;
    float attackSeconds = 0.0f;
    float decaySeconds = 0.0f;
    float sustainLevel = 1.0f;
    float releaseSeconds = 0.0f;
    bool retriggerFromCurrent = true;

    Stage current = Stage::idle;
    float level = 0.0f;
    double endLevel = 0.0;
    bool ascending = true;

    // Linear.
    Real origin = 0;
    Real increment = 0;
    Real accumulated = 0;
    std::int64_t count = 0;
    std::int64_t countEnd = 0;

    // Exponential and RC.
    Real target = 0;
    Real coefficient = 0;
    Real distance = 0;
    Real endDistance = 0;
    float log2Coefficient = 0.0f; // counted exponential
};

using LinearAccumulated = Prototype<Kind::linearAccumulated>;
using LinearCounted = Prototype<Kind::linearCounted>;
using ExponentialFloat = Prototype<Kind::exponential, float>;
using ExponentialDouble = Prototype<Kind::exponential, double>;
using ExponentialCounted = Prototype<Kind::exponentialCounted, float>;
using AnalogRc = Prototype<Kind::analogRc, float, false>;
using AnalogRcFlushed = Prototype<Kind::analogRc, float, true>;

} // namespace anoesis::test::envelope
