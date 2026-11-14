// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/math/fast_math.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace anoesis::dsp
{

/// The stage an Adsr is in. The gate is on in attack, decay, and sustain.
enum class AdsrStage : std::uint8_t
{
    idle,
    attack,
    decay,
    sustain,
    release,
};

/// A gate change, for Adsr::apply() and the events of Adsr::process().
enum class AdsrGate : std::uint8_t
{
    on,       ///< gateOn(): retriggers a sounding envelope from its level
    legatoOn, ///< legatoGateOn(): leaves a held envelope alone
    off,      ///< gateOff()
};

/// A gate change at a sample offset within a block.
struct AdsrEvent
{
    std::size_t offset = 0;
    AdsrGate gate = AdsrGate::on;
};

/// An ADSR envelope generator, with sample-accurate gates.
///
/// The model (docs/dsp/envelopes.md, chosen from measured candidates):
///
/// - Each stage is an exponential segment that approaches a target beyond
///   its end level and stops when it gets there: attack 0 -> 1 towards
///   1 + 0.3, decay 1 -> sustain and release 1 -> 0 each overshooting by
///   0.001 of the span. The attack is concave, like an analog envelope
///   charging; decay and release are close to straight lines in dB.
/// - A stage of N = round(time x rate) samples ends on its N-th sample,
///   which outputs the end level exactly. A time of 0 is instant: an attack
///   of 0 puts the decay's first value on the gate's own sample, a release
///   of 0 puts 0 on the note-off's.
/// - Constant rate: a stage entered part-way (a release from the sustain
///   level, a retrigger, a time changed mid-stage) enters its full-scale
///   curve where it passes the current level, so the level never jumps and
///   the time sets the speed.
/// - A retrigger (gateOn() while sounding) attacks from the current level.
///   legatoGateOn() leaves a held envelope alone.
/// - A sustain change while sustaining ramps linearly over 10 ms; during the
///   decay, the decay re-aims at the new level (or, if the level is already
///   past it, ramps to it).
/// - isActive() is false exactly when the release has reached 0. The level
///   is never subnormal.
///
/// Exact in float: each level is computed from the sample count since the
/// stage began, target + d0 * 2^(k log2 c) with fastExp2, so no error
/// accumulates over long stages and the output does not depend on how a
/// render is split into blocks. Times are seconds, sustain a linear level;
/// the knob laws belong to the parameter table.
///
/// prepare() sets the sample rate (and is the only call that may be made
/// off the audio thread only); everything else is real-time safe and never
/// allocates. Doc: docs/dsp/envelopes.md
class Adsr
{
public:
    static constexpr float attackRatio = 0.3f;
    static constexpr float decayReleaseRatio = 0.001f;
    static constexpr float maxStageSeconds = 60.0f;
    static constexpr float sustainRampSeconds = 0.01f;

    void prepare(const ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        sustainRamp.prepare(spec);
        sustainRamp.setRampTime(sustainRampSeconds);
        reset();
    }

    /// Idle, at 0. The settings are kept.
    void reset() noexcept
    {
        current = AdsrStage::idle;
        value = 0.0f;
        sustainRamp.setCurrentAndTarget(0.0f);
    }

    /// The attack time in seconds, clamped to [0, maxStageSeconds]; NaN is
    /// ignored. Mid-attack it applies at once, at the new rate.
    void setAttack(float seconds) noexcept
    {
        if (setTime(attack, seconds) && current == AdsrStage::attack)
        {
            enter(AdsrStage::attack);
        }
    }

    /// The decay time; as setAttack().
    void setDecay(float seconds) noexcept
    {
        if (setTime(decay, seconds) && current == AdsrStage::decay)
        {
            enter(AdsrStage::decay);
        }
    }

    /// The sustain level, linear, clamped to [0, 1]; NaN is ignored and a
    /// subnormal level is 0. While sustaining the level ramps to it over
    /// sustainRampSeconds; during the decay the decay re-aims at it.
    void setSustain(float level) noexcept
    {
        if (std::isnan(level))
        {
            return;
        }
        sustain = std::clamp(flushDenormal(level), 0.0f, 1.0f);
        if (current == AdsrStage::decay)
        {
            enter(AdsrStage::decay);
        }
        else if (current == AdsrStage::sustain)
        {
            sustainRamp.setTarget(sustain);
        }
    }

    /// The release time; as setAttack().
    void setRelease(float seconds) noexcept
    {
        if (setTime(release, seconds) && current == AdsrStage::release)
        {
            enter(AdsrStage::release);
        }
    }

    [[nodiscard]] float attackSeconds() const noexcept
    {
        return attack;
    }

    [[nodiscard]] float decaySeconds() const noexcept
    {
        return decay;
    }

    [[nodiscard]] float sustainLevel() const noexcept
    {
        return sustain;
    }

    [[nodiscard]] float releaseSeconds() const noexcept
    {
        return release;
    }

    /// Starts a note, or retriggers a sounding one: the attack from the
    /// current level. Takes effect from the next sample.
    void gateOn() noexcept
    {
        enter(AdsrStage::attack);
    }

    /// As gateOn(), except that a held envelope (attack, decay, sustain) is
    /// left alone: for a legato note.
    void legatoGateOn() noexcept
    {
        if (!isGateOn())
        {
            gateOn();
        }
    }

    /// Releases a held envelope from its current level. Does nothing in the
    /// release or when idle.
    void gateOff() noexcept
    {
        if (isGateOn())
        {
            enter(AdsrStage::release);
        }
    }

    void apply(AdsrGate gate) noexcept
    {
        switch (gate)
        {
        case AdsrGate::on:
            gateOn();
            break;
        case AdsrGate::legatoOn:
            legatoGateOn();
            break;
        case AdsrGate::off:
            gateOff();
            break;
        }
    }

    /// Advances one sample and returns the level.
    [[nodiscard]] float next() noexcept
    {
        switch (current)
        {
        case AdsrStage::idle:
            return 0.0f;
        case AdsrStage::sustain:
            value = sustainRamp.next();
            return value;
        case AdsrStage::attack:
        case AdsrStage::decay:
        case AdsrStage::release:
            break;
        }
        return step();
    }

    /// Writes the next out.size() levels: the same values as next(), a run
    /// of a stage at a time.
    void process(std::span<float> out) noexcept
    {
        std::size_t n = 0;
        while (n < out.size())
        {
            switch (current)
            {
            case AdsrStage::idle:
                std::fill(out.begin() + static_cast<std::ptrdiff_t>(n), out.end(), 0.0f);
                return;
            case AdsrStage::sustain:
                if (sustainRamp.isSmoothing())
                {
                    out[n] = next();
                    ++n;
                }
                else
                {
                    value = sustainRamp.currentValue();
                    std::fill(out.begin() + static_cast<std::ptrdiff_t>(n), out.end(), value);
                    return;
                }
                break;
            case AdsrStage::attack:
            case AdsrStage::decay:
            case AdsrStage::release:
                n += run(out.subspan(n));
                if (n < out.size())
                {
                    out[n] = step(); // the stage's last sample
                    ++n;
                }
                break;
            }
        }
    }

    /// As process(out), applying each event before the sample at its
    /// offset. Events must be in order of offset; offsets at or past the
    /// end apply after the block.
    void process(std::span<float> out, std::span<const AdsrEvent> events) noexcept
    {
        std::size_t start = 0;
        for (const auto& event : events)
        {
            const auto at = std::clamp(event.offset, start, out.size());
            process(out.subspan(start, at - start));
            apply(event.gate);
            start = at;
        }
        process(out.subspan(start));
    }

    [[nodiscard]] AdsrStage stage() const noexcept
    {
        return current;
    }

    /// False once the release has reached 0 (or before the first gate):
    /// the voice can be freed.
    [[nodiscard]] bool isActive() const noexcept
    {
        return current != AdsrStage::idle;
    }

    /// True in attack, decay, and sustain.
    [[nodiscard]] bool isGateOn() const noexcept
    {
        return current == AdsrStage::attack || current == AdsrStage::decay || current == AdsrStage::sustain;
    }

    /// The level most recently returned (0 when idle).
    [[nodiscard]] float level() const noexcept
    {
        return value;
    }

private:
    static bool setTime(float& time, float seconds) noexcept
    {
        if (std::isnan(seconds))
        {
            return false;
        }
        time = std::clamp(seconds, 0.0f, maxStageSeconds);
        return true;
    }

    // The level after `count` samples of the current segment.
    [[nodiscard]] float levelAt(std::uint32_t k) const noexcept
    {
        return target + (distance * fastExp2(log2Coefficient * static_cast<float>(k)));
    }

    // One sample of a moving stage, ending it on its last sample.
    float step() noexcept
    {
        ++count;
        if (count >= length)
        {
            // The stage's last sample is its end level, even when the next
            // stage is instant (a decay of 0 starts at the sustain level on
            // the following sample).
            const float last = endLevel;
            value = last;
            enter(following(current));
            value = last;
            return last;
        }
        value = levelAt(count);
        return value;
    }

    // The samples of a moving stage before its last one, at most out.size().
    // The same arithmetic as step(), so the output does not depend on the
    // block size. Returns how many it wrote.
    std::size_t run(std::span<float> out) noexcept
    {
        const auto available = static_cast<std::size_t>(length - count - 1);
        const auto n = std::min(available, out.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            out[i] = levelAt(count + 1 + static_cast<std::uint32_t>(i));
        }
        if (n > 0)
        {
            count += static_cast<std::uint32_t>(n);
            value = out[n - 1];
        }
        return n;
    }

    [[nodiscard]] static AdsrStage following(AdsrStage stage) noexcept
    {
        switch (stage)
        {
        case AdsrStage::attack:
            return AdsrStage::decay;
        case AdsrStage::decay:
            return AdsrStage::sustain;
        case AdsrStage::release:
        case AdsrStage::idle:
        case AdsrStage::sustain:
            break;
        }
        return AdsrStage::idle;
    }

    // Enters `stage` from the current level, skipping stages of 0 samples
    // and stages the level is already at or past the end of (an attack of 0
    // goes on to the decay, a decay of 0 to the sustain).
    void enter(AdsrStage stage) noexcept
    {
        for (;;)
        {
            current = stage;
            switch (stage)
            {
            case AdsrStage::attack:
                if (configure(0.0, 1.0, attack, attackRatio))
                {
                    return;
                }
                value = 1.0f;
                stage = AdsrStage::decay;
                break;
            case AdsrStage::decay:
                if (configure(1.0, static_cast<double>(sustain), decay, decayReleaseRatio))
                {
                    return;
                }
                if (samplesFor(decay) < 1.0)
                {
                    value = sustain; // a decay of 0 is instant
                }
                enterSustain();
                return;
            case AdsrStage::sustain:
                enterSustain();
                return;
            case AdsrStage::release:
                if (configure(1.0, 0.0, release, decayReleaseRatio))
                {
                    return;
                }
                value = 0.0f;
                current = AdsrStage::idle;
                return;
            case AdsrStage::idle:
                value = 0.0f;
                return;
            }
        }
    }

    // From the current level: exactly the sustain level, or a ramp to it.
    void enterSustain() noexcept
    {
        current = AdsrStage::sustain;
        sustainRamp.setCurrentAndTarget(value);
        sustainRamp.setTarget(sustain);
    }

    [[nodiscard]] double samplesFor(float seconds) const noexcept
    {
        return std::round(static_cast<double>(seconds) * static_cast<double>(sampleRate));
    }

    // Sets up the segment of the full-scale curve start -> end over the
    // stage's time, entered at the current level. False: nothing to do (a
    // time of 0, or the level at or past the end).
    bool configure(double start, double end, float seconds, float ratio) noexcept
    {
        const double samples = samplesFor(seconds);
        const double span = end - start;
        const auto from = static_cast<double>(value);
        if (samples < 1.0 || !(span > 0.0 ? from < end : from > end))
        {
            return false;
        }
        const auto r = static_cast<double>(ratio);
        const double overshoot = end + (r * span);
        const double perSample = std::log2(r / (1.0 + r)) / samples;
        const double initial = from - overshoot;
        // The samples left on the full-scale curve from here to the end.
        const double remaining = std::round(std::log2((-r * span) / initial) / perSample);
        if (!(remaining >= 1.0))
        {
            return false;
        }
        target = static_cast<float>(overshoot);
        distance = static_cast<float>(initial);
        log2Coefficient = static_cast<float>(perSample);
        endLevel = static_cast<float>(end);
        count = 0;
        length = static_cast<std::uint32_t>(std::min({remaining, samples, 2147483647.0}));
        return true;
    }

    float sampleRate = 0.0f;
    float attack = 0.0f;
    float decay = 0.0f;
    float sustain = 1.0f;
    float release = 0.0f;

    AdsrStage current = AdsrStage::idle;
    float value = 0.0f;
    LinearSmoother sustainRamp;

    // The current segment: level = target + distance * 2^(log2Coefficient k)
    // for k = 1 .. length - 1, then endLevel at k = length.
    float target = 0.0f;
    float distance = 0.0f;
    float log2Coefficient = 0.0f;
    float endLevel = 0.0f;
    std::uint32_t count = 0;
    std::uint32_t length = 0;
};

} // namespace anoesis::dsp
