// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The mixer candidates (M1.4 task 4, stage 1; docs/dsp/mixer.md): Mix A and
// Mix B, Main Volume, the output meter, and what the output does at its
// extremes. Prototypes for measurement, not library code.
//
// - Question 1, the Mix law: four laws over Mix's internal 0-15 scale (the
//   developer: "internally scale from 0 to +15", shown as 0 to 10), and three
//   clamps of the knob plus its modulation (F24's [0, 15]; at 0 only, as the
//   v3 engine "will mute negative volume"; none, the v2 engine's inversion).
// - Question 2, where Mix applies: MixVoice, a copy of dsp::Voice with a gain
//   per part at the output stage (after the part's amp envelope), computed
//   per sample, per 32-sample grid point (held, or ramped across the
//   segment), or not at all, the voice writing each part to its own bus for
//   a global gain per part (partBus). Its modulation is a stand-in for M1.5:
//   a free LFO (global, so a bus can follow it) or a pluck from each note-on
//   (per voice, so a bus can't). With every Mix at unity and no modulation it
//   is dsp::Voice to the bit (tested by the report).
// - Question 3, Main Volume: a stereo gain at the end of the chain, a global
//   LinearSmoother over 10 ms (or none, for comparison), the block split at
//   its events.
// - Question 4, the meter: a sample peak with hold and fall, a block peak
//   (for contrast: it depends on the block size), a 300 ms RMS window and a
//   300 ms exponential RMS, a VU (IEC 60268-17 ballistics) and a Type I PPM
//   (IEC 60268-10). Every per-sample meter is the same to the bit whatever
//   the block split.
//
// MixEngine runs the real dsp::VoiceAllocator over 16 MixVoices, the Mix
// ramps, the part buses, Main Volume and the meters, splitting each host
// block at its parameter events.

#pragma once

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/pitch_drift.hpp>
#include <anoesis/dsp/voice/glide.hpp>
#include <anoesis/dsp/voice/pitch_bend.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace anoesis::test::mixer
{

namespace dsp = anoesis::dsp;

// --- Question 1: the Mix law ----------------------------------------------------------------

/// The top of Mix's internal scale, and the factor from the displayed value
/// (0-10) to it (the developer: 6.667 shown is "a real 10 internally").
inline constexpr float mixInternalMax = 15.0f;
inline constexpr float mixDisplayToInternal = 1.5f;

[[nodiscard]] constexpr float mixInternal(float display) noexcept
{
    return display * mixDisplayToInternal;
}

/// Gain laws over the internal value m (0-15).
enum class MixLaw : std::uint8_t
{
    linear10,  ///< m / 10: unity at internal 10 (6.67 shown), x1.5 (+3.5 dB) at the top
    linear15,  ///< m / 15: unity at the top (10 shown)
    squared10, ///< (m / 10)^2: unity at internal 10, x2.25 (+7.0 dB) at the top
    decibel,   ///< a fader: 0 at m = 0, -60 dB just above, 6 dB per unit to 0 dB at 10, then 1.2 per unit to +6 dB
};

/// How the knob plus its modulation is limited before the law.
enum class MixClamp : std::uint8_t
{
    knobRange, ///< F24: to the knob's range, [0, 15]
    floorOnly, ///< at 0 only (the v3 engine "will mute negative volume"), unbounded above
    none,      ///< unclamped: a negative value inverts (the v2 engine)
};

[[nodiscard]] inline float clampMix(float sum, MixClamp clamp) noexcept
{
    if (std::isnan(sum))
    {
        return 0.0f;
    }
    switch (clamp)
    {
    case MixClamp::knobRange:
        return std::clamp(sum, 0.0f, mixInternalMax);
    case MixClamp::floorOnly:
        return std::max(sum, 0.0f);
    case MixClamp::none:
        break;
    }
    return sum;
}

/// The law at m >= 0.
[[nodiscard]] inline float mixGainPositive(MixLaw law, float m) noexcept
{
    switch (law)
    {
    case MixLaw::linear10:
        return m / 10.0f;
    case MixLaw::linear15:
        return m / 15.0f;
    case MixLaw::squared10:
        return (m / 10.0f) * (m / 10.0f);
    case MixLaw::decibel:
    {
        if (m <= 0.0f)
        {
            return 0.0f;
        }
        const float db = m <= 10.0f ? -60.0f + (6.0f * m) : 1.2f * (m - 10.0f);
        return std::pow(10.0f, db / 20.0f);
    }
    }
    return 0.0f;
}

/// The law, odd below 0 (a negative value is the positive one inverted).
[[nodiscard]] inline float mixGain(MixLaw law, float m) noexcept
{
    return m < 0.0f ? -mixGainPositive(law, -m) : mixGainPositive(law, m);
}

/// Main Volume's candidate laws over its knob (0-10).
enum class VolumeLaw : std::uint8_t
{
    linear,  ///< knob / 10
    squared, ///< (knob / 10)^2
    decibel, ///< 0 at 0, else -60 dB to 0 dB, linear in dB
};

[[nodiscard]] inline float volumeGain(VolumeLaw law, float knob) noexcept
{
    const float x = std::clamp(std::isnan(knob) ? 0.0f : knob, 0.0f, 10.0f) / 10.0f;
    switch (law)
    {
    case VolumeLaw::linear:
        return x;
    case VolumeLaw::squared:
        return x * x;
    case VolumeLaw::decibel:
        return x > 0.0f ? std::pow(10.0f, (-60.0f + (60.0f * x)) / 20.0f) : 0.0f;
    }
    return 0.0f;
}

// --- Question 2: where Mix applies --------------------------------------------------------

enum class MixPlacement : std::uint8_t
{
    gridHeld,   ///< per voice, computed at each 32-sample grid point and held over the segment
    gridRamped, ///< per voice, computed for each grid point and ramped linearly across the segment
    perSample,  ///< per voice, every sample
    partBus,    ///< global: the voices write each part to its own bus; one gain per part bus
};

/// A stand-in for M1.5's modulation of Mix A/B, in internal units.
enum class MixModulation : std::uint8_t
{
    none,
    lfo,   ///< amount x sin(2 pi rate t), t the absolute time: a free LFO, the same for every voice
    pluck, ///< amount x e^(-n / decay), n the samples since the voice's note-on: a mod envelope
};

struct MixModulationSpec
{
    MixModulation kind = MixModulation::none;
    float amount = 0.0f; ///< internal units
    float rateHz = 5.0f;
    float decaySeconds = 0.01f;
};

/// What every MixVoice reads (the engine owns it): the Mix knob ramps, in
/// internal units, positioned as the filter knobs are (VoiceKnobs::offset).
struct MixContext
{
    std::array<const dsp::LinearSmoother*, 2> mix{}; ///< null: internal 10
    MixLaw law = MixLaw::linear10;
    MixClamp clamp = MixClamp::knobRange;
    MixPlacement placement = MixPlacement::perSample;
    MixModulationSpec modulation{};
    /// V3's alternative: a part crossing into the other filter also takes its
    /// own Mix, before that filter (with its envelope). Off: the filter's
    /// part's Mix only, after the envelope.
    bool crossingTakesSourceMix = false;
    double sampleRate = 48000.0;
};

[[nodiscard]] inline float modulationAt(const MixModulationSpec& m, double sampleRate, std::uint64_t time,
                                        std::uint64_t sinceNoteOn) noexcept
{
    switch (m.kind)
    {
    case MixModulation::none:
        return 0.0f;
    case MixModulation::lfo:
    {
        const double cycles = static_cast<double>(m.rateHz) * static_cast<double>(time) / sampleRate;
        const double phase = cycles - std::floor(cycles);
        return m.amount * static_cast<float>(std::sin(2.0 * std::numbers::pi * phase));
    }
    case MixModulation::pluck:
        return m.amount * static_cast<float>(std::exp(-static_cast<double>(sinceNoteOn) /
                                                      (static_cast<double>(m.decaySeconds) * sampleRate)));
    }
    return 0.0f;
}

/// A part's Mix gain: its knob ramp read `rampAhead` samples on, plus the
/// modulation, clamped, through the law.
[[nodiscard]] inline float mixAt(const MixContext& c, std::size_t part, std::size_t rampAhead, std::uint64_t time,
                                 std::uint64_t sinceNoteOn) noexcept
{
    const auto* ramp = c.mix[part];
    const float knob = ramp != nullptr ? ramp->valueAfter(rampAhead) : 10.0f;
    return mixGain(c.law, clampMix(knob + modulationAt(c.modulation, c.sampleRate, time, sinceNoteOn), c.clamp));
}

// --- Question 4: the meters ----------------------------------------------------------------

/// A sample-peak meter: instant attack, a hold, then a fall in dB per second
/// (by default Type I's return: 20 dB in 1.7 s), and an over light held for
/// a time after the last sample above full scale.
class PeakMeter
{
public:
    void prepare(double sampleRate, float holdSeconds = 0.5f, float fallDbPerSecond = 20.0f / 1.7f,
                 float overSeconds = 1.0f) noexcept
    {
        holdSamples = static_cast<std::size_t>(std::lround(holdSeconds * sampleRate));
        overSamples = static_cast<std::size_t>(std::lround(overSeconds * sampleRate));
        fall = static_cast<float>(std::pow(10.0, -static_cast<double>(fallDbPerSecond) / 20.0 / sampleRate));
        reset();
    }
    void reset() noexcept
    {
        level = 0.0f;
        hold = 0;
        overHold = 0;
    }
    void process(std::span<const float> x) noexcept
    {
        for (const float v : x)
        {
            const float a = std::abs(v);
            if (a >= level)
            {
                level = a;
                hold = holdSamples;
            }
            else if (hold > 0)
            {
                --hold;
            }
            else
            {
                level = level * fall;
                if (level < 1e-10f)
                {
                    level = 0.0f;
                }
            }
            if (a > 1.0f)
            {
                overHold = overSamples;
            }
            else if (overHold > 0)
            {
                --overHold;
            }
        }
    }
    [[nodiscard]] float reading() const noexcept
    {
        return level;
    }
    [[nodiscard]] bool over() const noexcept
    {
        return overHold > 0;
    }

private:
    std::size_t holdSamples = 0;
    std::size_t overSamples = 0;
    float fall = 1.0f;
    float level = 0.0f;
    std::size_t hold = 0;
    std::size_t overHold = 0;
};

/// The same ballistics applied once per block (the block's peak, the hold
/// and fall by the block's length): the common shortcut, which depends on
/// where the blocks fall.
class BlockPeakMeter
{
public:
    void prepare(double sampleRate, float holdSeconds = 0.5f, float fallDbPerSecond = 20.0f / 1.7f) noexcept
    {
        holdSamples = static_cast<std::size_t>(std::lround(holdSeconds * sampleRate));
        fall = static_cast<float>(std::pow(10.0, -static_cast<double>(fallDbPerSecond) / 20.0 / sampleRate));
        level = 0.0f;
        hold = 0;
    }
    void process(std::span<const float> x) noexcept
    {
        float peak = 0.0f;
        for (const float v : x)
        {
            peak = std::max(peak, std::abs(v));
        }
        const auto n = x.size();
        if (peak >= level)
        {
            level = peak;
            hold = holdSamples;
        }
        else if (hold >= n)
        {
            hold -= n;
        }
        else
        {
            level *= std::pow(fall, static_cast<float>(n - hold));
            hold = 0;
            level = std::max(level, peak);
        }
    }
    [[nodiscard]] float reading() const noexcept
    {
        return level;
    }

private:
    std::size_t holdSamples = 0;
    float fall = 1.0f;
    float level = 0.0f;
    std::size_t hold = 0;
};

/// RMS over a sliding window (300 ms by default): a ring of squares and a
/// running sum in double, summed afresh each time the ring wraps so that no
/// rounding accumulates. The ring is allocated by prepare().
class RmsWindowMeter
{
public:
    void prepare(double sampleRate, float seconds = 0.3f)
    {
        ring.assign(static_cast<std::size_t>(std::lround(seconds * sampleRate)), 0.0);
        reset();
    }
    void reset() noexcept
    {
        std::ranges::fill(ring, 0.0);
        sum = 0.0;
        position = 0;
    }
    void process(std::span<const float> x) noexcept
    {
        for (const float v : x)
        {
            const double square = static_cast<double>(v) * v;
            sum += square - ring[position];
            ring[position] = square;
            if (++position == ring.size())
            {
                position = 0;
                sum = 0.0;
                for (const double s : ring)
                {
                    sum += s;
                }
            }
        }
    }
    [[nodiscard]] float reading() const noexcept
    {
        return static_cast<float>(std::sqrt(std::max(sum, 0.0) / static_cast<double>(ring.size())));
    }

private:
    std::vector<double> ring;
    double sum = 0.0;
    std::size_t position = 0;
};

/// RMS through a one-pole on the square, time constant 300 ms by default.
class RmsExponentialMeter
{
public:
    void prepare(double sampleRate, float seconds = 0.3f) noexcept
    {
        coefficient = static_cast<float>(1.0 - std::exp(-1.0 / (static_cast<double>(seconds) * sampleRate)));
        meanSquare = 0.0f;
    }
    void process(std::span<const float> x) noexcept
    {
        for (const float v : x)
        {
            meanSquare += ((v * v) - meanSquare) * coefficient;
        }
        if (meanSquare < 1e-20f)
        {
            meanSquare = 0.0f;
        }
    }
    [[nodiscard]] float reading() const noexcept
    {
        return std::sqrt(meanSquare);
    }

private:
    float coefficient = 0.0f;
    float meanSquare = 0.0f;
};

/// A two-pole low-pass (a trapezoidal state-variable filter), the VU's needle.
struct TwoPole
{
    float a1 = 0.0f;
    float a2 = 0.0f;
    float a3 = 0.0f;
    float s1 = 0.0f;
    float s2 = 0.0f;

    void design(double cutoffHz, double damping, double sampleRate) noexcept
    {
        const double g = std::tan(std::numbers::pi * cutoffHz / sampleRate);
        const double k = 2.0 * damping;
        const double d1 = 1.0 / (1.0 + (g * (g + k)));
        a1 = static_cast<float>(d1);
        a2 = static_cast<float>(g * d1);
        a3 = static_cast<float>(g * g * d1);
        s1 = 0.0f;
        s2 = 0.0f;
    }
    float next(float x) noexcept
    {
        const float v3 = x - s2;
        const float v1 = (a1 * s1) + (a2 * v3);
        const float v2 = s2 + (a2 * s1) + (a3 * v3);
        s1 = (2.0f * v1) - s1;
        s2 = (2.0f * v2) - s2;
        return v2;
    }
};

/// A VU meter (IEC 60268-17): the full-wave rectified signal through a
/// two-pole needle damped for 1.25% overshoot, its corner found at prepare()
/// so that a step reaches 99% of its reading in 300 ms; calibrated so that a
/// steady sine reads its peak (a full-scale sine reads 0 dBFS).
class VuMeter
{
public:
    static constexpr double overshoot = 0.0125;

    void prepare(double sampleRate)
    {
        const double lnOvershoot = std::log(overshoot);
        damping = -lnOvershoot / std::sqrt((std::numbers::pi * std::numbers::pi) + (lnOvershoot * lnOvershoot));
        const auto target = static_cast<std::size_t>(std::lround(0.3 * sampleRate));
        double low = 0.1;
        double high = 20.0;
        for (int i = 0; i < 50; ++i)
        {
            const double mid = 0.5 * (low + high);
            (riseSamples(mid, sampleRate) > target ? low : high) = mid;
        }
        cutoff = 0.5 * (low + high);
        needle.design(cutoff, damping, sampleRate);
    }
    void reset() noexcept
    {
        needle.s1 = 0.0f;
        needle.s2 = 0.0f;
        value = 0.0f;
    }
    void process(std::span<const float> x) noexcept
    {
        constexpr auto scale = static_cast<float>(std::numbers::pi / 2.0); // mean of |sin| is 2/pi
        for (const float v : x)
        {
            value = needle.next(std::abs(v) * scale);
        }
        if (std::abs(value) < 1e-20f)
        {
            value = 0.0f;
        }
    }
    [[nodiscard]] float reading() const noexcept
    {
        return value;
    }
    [[nodiscard]] double cutoffHz() const noexcept
    {
        return cutoff;
    }
    [[nodiscard]] double dampingRatio() const noexcept
    {
        return damping;
    }

private:
    [[nodiscard]] std::size_t riseSamples(double hz, double sampleRate) const noexcept
    {
        TwoPole p;
        p.design(hz, damping, sampleRate);
        const auto limit = static_cast<std::size_t>(5.0 * sampleRate);
        for (std::size_t i = 0; i < limit; ++i)
        {
            if (p.next(1.0f) >= 0.99f)
            {
                return i + 1;
            }
        }
        return limit;
    }

    TwoPole needle;
    double damping = 0.8;
    double cutoff = 1.0;
    float value = 0.0f;
};

/// A Type I peak programme meter (IEC 60268-10, the DIN scale): a
/// quasi-peak rectifier that charges towards |x| with a coefficient found at
/// prepare() so that a 5 ms burst of 5 kHz reads 2 dB below the steady tone
/// (its integration time), and falls 20 dB in 1.7 s.
class PpmMeter
{
public:
    void prepare(double sampleRate)
    {
        release = static_cast<float>(std::pow(10.0, -1.0 / (1.7 * sampleRate)));
        double low = 1e-5;
        double high = 1.0;
        for (int i = 0; i < 50; ++i)
        {
            const double mid = std::sqrt(low * high);
            (burstDb(static_cast<float>(mid), sampleRate, 0.005) < -2.0 ? low : high) = mid;
        }
        attack = static_cast<float>(std::sqrt(low * high));
        level = 0.0f;
    }
    void reset() noexcept
    {
        level = 0.0f;
    }
    void process(std::span<const float> x) noexcept
    {
        level = run(level, attack, release, x);
    }
    [[nodiscard]] float reading() const noexcept
    {
        return level;
    }
    [[nodiscard]] float attackCoefficient() const noexcept
    {
        return attack;
    }
    /// The highest reading of a burst of `seconds` of a 5 kHz sine from rest,
    /// in dB re the steady tone's reading.
    [[nodiscard]] double burstDb(float a, double sampleRate, double seconds) const
    {
        const auto steadyLength = static_cast<std::size_t>(sampleRate);
        const auto burst = static_cast<std::size_t>(std::lround(seconds * sampleRate));
        const auto tone = [&](std::size_t i)
        {
            return static_cast<float>(std::sin(2.0 * std::numbers::pi * 5000.0 * static_cast<double>(i) / sampleRate));
        };
        float l = 0.0f;
        float steady = 0.0f;
        for (std::size_t i = 0; i < steadyLength; ++i)
        {
            const float v = tone(i);
            l = run(l, a, release, std::span{&v, 1});
            steady = i > steadyLength / 2 ? std::max(steady, l) : steady;
        }
        l = 0.0f;
        float highest = 0.0f;
        for (std::size_t i = 0; i < burst + 4800; ++i)
        {
            const float v = i < burst ? tone(i) : 0.0f;
            l = run(l, a, release, std::span{&v, 1});
            highest = std::max(highest, l);
        }
        return 20.0 * std::log10(static_cast<double>(highest) / static_cast<double>(steady));
    }

private:
    [[nodiscard]] static float run(float l, float a, float r, std::span<const float> x) noexcept
    {
        for (const float v : x)
        {
            const float rectified = std::abs(v);
            l = rectified > l ? l + ((rectified - l) * a) : l * r;
        }
        return l < 1e-10f ? 0.0f : l;
    }

    float attack = 1.0f;
    float release = 1.0f;
    float level = 0.0f;
};

/// Every candidate meter on both channels.
struct MeterSet
{
    std::array<PeakMeter, 2> peak{};
    std::array<RmsWindowMeter, 2> rmsWindow{};
    std::array<RmsExponentialMeter, 2> rmsExponential{};
    std::array<VuMeter, 2> vu{};
    std::array<PpmMeter, 2> ppm{};

    void prepare(double sampleRate)
    {
        for (std::size_t c = 0; c < 2; ++c)
        {
            peak[c].prepare(sampleRate);
            rmsWindow[c].prepare(sampleRate);
            rmsExponential[c].prepare(sampleRate);
            vu[c].prepare(sampleRate);
            ppm[c].prepare(sampleRate);
        }
    }
    void process(const dsp::AudioBlock& block) noexcept
    {
        for (std::size_t c = 0; c < 2; ++c)
        {
            const auto x = std::span<const float>{block.channel(c)};
            peak[c].process(x);
            rmsWindow[c].process(x);
            rmsExponential[c].process(x);
            vu[c].process(x);
            ppm[c].process(x);
        }
    }
};

// --- Question 5: an output guard -------------------------------------------------------------

/// Non-finite samples to 0 and subnormal ones to 0, in place: the guard whose
/// cost question 5 weighs.
inline void guardOutput(std::span<float> x) noexcept
{
    for (auto& v : x)
    {
        const bool keep = std::abs(v) >= std::numeric_limits<float>::min(); // false for NaN, 0 and subnormals
        v = keep && std::isfinite(v) ? v : 0.0f;
    }
}

// --- The voice -----------------------------------------------------------------------------

/// dsp::Voice (M1.4 tasks 1-3, as built) with a Mix gain per part at the
/// output stage, from a MixContext. Each filter's output, times its part's
/// amp envelope, is multiplied by its part's Mix gain before it is added to
/// the output (V3: the filter's part's Mix only); in partBus placement it is
/// added unscaled to channels 2f and 2f + 1 instead (the block needs 4
/// channels), for the engine's global gain per part. Everything else is
/// dsp::Voice's, line for line.
template <std::size_t Taps = dsp::minBlepTaps>
class MixVoice
{
public:
    using Slot = dsp::OscillatorSlot<Taps>;
    using Knobs = dsp::VoiceKnobs;
    static constexpr std::size_t partCount = 2;
    static constexpr std::size_t slotsPerPart = 2;
    static constexpr std::size_t slotCount = partCount * slotsPerPart;
    static constexpr std::size_t gridSamples = dsp::LadderFilter::gridSamples;
    /// The filter input level's range; anything else (NaN, 0, negative, inf)
    /// counts as 1.
    static constexpr float minInputLevel = 1e-3f;
    static constexpr float maxInputLevel = 1e3f;

    /// The table must outlive the voice. `seed` sets every random sequence
    /// (the slots' free-running phases and noise, the drift): give each voice
    /// its own.
    /// `context` (the Mix knobs, law, placement and modulation) must outlive
    /// the voice; null: every Mix at unity.
    MixVoice(const dsp::MinBlepTable& residuals, const MixContext* context, std::uint64_t seed = 0) noexcept
        : slots{Slot{residuals}, Slot{residuals}, Slot{residuals}, Slot{residuals}},
          mixContext(context != nullptr ? context : &unityContext)
    {
        for (std::size_t s = 0; s < slotCount; ++s)
        {
            slots[s].setRandomSeed(seed ^ (0x9E3779B97F4A7C15U * (s + 1)));
        }
        pitchDrift.setSeed(seed ^ 0x3C6EF372FE94F82BU);
        updateRoutes();
    }

    /// Slots 0 and 1 are A1 and A2; 2 and 3 are B1 and B2.
    [[nodiscard]] Slot& slot(std::size_t index) noexcept
    {
        return slots[index];
    }
    [[nodiscard]] const Slot& slot(std::size_t index) const noexcept
    {
        return slots[index];
    }
    /// The amp envelope of part 0 (A) or 1 (B).
    [[nodiscard]] dsp::Adsr& envelope(std::size_t part) noexcept
    {
        return envelopes[part];
    }
    [[nodiscard]] dsp::PitchDrift& drift() noexcept
    {
        return pitchDrift;
    }
    /// A part's filter, for measurement. The voice owns its controls: use the
    /// setters below.
    [[nodiscard]] const dsp::LadderFilter& filter(std::size_t part) const noexcept
    {
        return filters[part];
    }

    /// From the next grid point while a note sounds; at once before the
    /// first note-on, at a note-on, and at reset().
    void setFilterInput(std::size_t part, dsp::FilterInput input) noexcept
    {
        pendingInputs[part] = input;
    }
    [[nodiscard]] dsp::FilterInput filterInput(std::size_t part) const noexcept
    {
        return pendingInputs[part];
    }
    /// As setFilterInput(). Leaving Bypass starts the filter from rest.
    void setFilterBypassed(std::size_t part, bool bypassed) noexcept
    {
        pendingBypass[part] = bypassed;
    }
    /// Crossfaded by the filter over 10 ms.
    void setFilterResponse(std::size_t part, dsp::FilterResponse response) noexcept
    {
        filters[part].setResponse(response);
    }
    /// The drive gain (inventory F8), ramped by the filter over 10 ms.
    void setFilterDrive(std::size_t part, float gain) noexcept
    {
        filters[part].setDrive(gain);
    }
    /// Warm Drive, for both filters.
    void setWarm(bool on) noexcept
    {
        for (auto& f : filters)
        {
            f.setWarm(on);
        }
    }
    /// The filter input level g (inventory V8): each filter's input is scaled
    /// by g and its output by 1/g, so the clean level is unchanged and the
    /// saturation's knee moves. 1 by default; values outside
    /// [minInputLevel, maxInputLevel], and NaN, count as 1.
    void setFilterInputLevel(float gain) noexcept
    {
        const bool valid = gain >= minInputLevel && gain <= maxInputLevel; // false for NaN
        inputLevel = valid ? gain : 1.0f;
        outputLevel = 1.0f / inputLevel;
        unityLevel = inputLevel >= 1.0f && inputLevel <= 1.0f; // exactly 1, without -Wfloat-equal
    }
    [[nodiscard]] float filterInputLevel() const noexcept
    {
        return inputLevel;
    }

    /// Stereo throughout; nothing is allocated.
    void prepare(const dsp::ProcessSpec& spec) noexcept
    {
        const dsp::ProcessSpec stereo{
            .sampleRate = spec.sampleRate,
            .maxBlockSize = spec.maxBlockSize,
            .numChannels = 2,
        };
        for (auto& s : slots)
        {
            s.prepare(stereo);
        }
        for (auto& f : filters)
        {
            f.prepare(stereo);
        }
        for (auto& e : envelopes)
        {
            e.prepare(spec);
        }
        pitchDrift.prepare(spec);
        glide.prepare(spec.sampleRate);
        velocityRamp.prepare(spec.sampleRate);
        reset();
    }

    /// As freshly prepared: every random sequence restarts from the seed.
    void reset() noexcept
    {
        for (auto& s : slots)
        {
            s.reset();
        }
        for (auto& e : envelopes)
        {
            e.reset();
        }
        pitchDrift.reset();
        applyPending();
        for (auto& f : filters)
        {
            f.reset();
        }
        gridPosition = 0;
        lastOctaves = {};
        lastK = {};
        glide.jump(dsp::referenceNote);
        velocityRamp.jump(1.0f);
        played = false;
    }

    /// A note-on before the next process() call, with the knobs positioned at
    /// its sample. The pitch jumps to `note`, or glides to it from
    /// `start.from` over `start.glideSeconds` (linear in semitones); the
    /// velocity is `start.velocity`, or, while it glides, ramps there from
    /// `start.fromVelocity` over the same time. The slots, the drift and both
    /// envelopes start (or, on a sounding voice, retrigger: the slots as
    /// Retrig says, the envelopes attacking from their level).
    ///
    /// - From silence (isActive() false): pending routing applies, and each
    ///   filter is fed for this sample and reset(), so the voice's grid
    ///   starts here.
    /// - On a sounding voice (a steal, or a retrigger in place): the filters
    ///   keep their state and the voice keeps its grid, counted from the
    ///   note-on from silence; the new note's keytrack reaches the filters at
    ///   the next grid point, and pending routing applies there, as during
    ///   any note (M1.4 task 2: a reset there clicks as hard as a cut). The
    ///   oscillators take the new pitch at once.
    ///
    /// A voice that has finished should be stop()ped before its next note.
    void noteOn(float note, const dsp::VoiceKnobs& knobs, const dsp::NoteStart& start = {}) noexcept
    {
        const bool sounding = isActive();
        startPitch(note, start);
        if (glide.isGliding())
        {
            velocityRamp.startSamples(start.fromVelocity, start.velocity, glide.lengthSamples());
        }
        else
        {
            velocityRamp.jump(start.velocity);
        }
        played = true;
        sinceNoteOn = 0; // a pluck (a mod envelope) restarts at every note-on
        for (auto& s : slots)
        {
            s.noteOn();
        }
        pitchDrift.noteOn();
        for (auto& e : envelopes)
        {
            e.gateOn();
        }
        if (sounding)
        {
            return;
        }
        // From silence the grid starts here: the gain at this sample opens it.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            segmentEnd[p] = mixAt(*mixContext, p, knobs.offset, knobs.time, 0);
            segmentStart[p] = segmentEnd[p];
        }
        applyPending();
        feedFilters(knobs, 0, keytrackPitch(knobs, 0, 0));
        for (auto& f : filters)
        {
            f.reset();
        }
        gridPosition = 0;
    }

    /// A legato note (Mono Legato, M1.4 task 3): the pitch jumps or glides to
    /// `note` as at noteOn(), and the velocity ramps from where it is to
    /// `start.velocity` over the glide (at once without one). Nothing else
    /// restarts: the envelopes are left to dsp::Adsr::legatoGateOn() (a held one
    /// goes on; a released one opens again), the oscillators keep their
    /// phases whatever Retrig says, and the drift, the filters and the grid
    /// carry on. On a voice that isn't sounding it is noteOn().
    void legatoNoteOn(float note, const dsp::VoiceKnobs& knobs, const dsp::NoteStart& start) noexcept
    {
        if (!isActive())
        {
            noteOn(note, knobs, start);
            return;
        }
        startPitch(note, start);
        velocityRamp.startSamples(velocityRamp.at(0), start.velocity, glide.lengthSamples());
        played = true;
        for (auto& e : envelopes)
        {
            e.legatoGateOn();
        }
    }

    void noteOff() noexcept
    {
        for (auto& e : envelopes)
        {
            e.gateOff();
        }
    }

    /// The master tune: the frequency of A4 in Hz, for the four slots and for
    /// keytrack (inventory F19: the filter follows it). Anything not finite
    /// and positive counts as 440.
    void setMasterTune(float referenceHz) noexcept
    {
        const bool valid = std::isfinite(referenceHz) && referenceHz > 0.0f;
        masterTuneHz = valid ? referenceHz : dsp::concertPitchHz;
        masterTuneOffset = dsp::masterTuneSemitones(masterTuneHz);
        for (auto& s : slots)
        {
            s.setReferenceHz(masterTuneHz);
        }
    }
    [[nodiscard]] float masterTune() const noexcept
    {
        return masterTuneHz;
    }

    /// The pitch the glide has reached (a fractional note, without the bend
    /// or the master tune), and the note it is heading for. stop() keeps
    /// them, so a note after the voice has finished can glide from there;
    /// reset() puts them at A4.
    [[nodiscard]] float currentPitch() const noexcept
    {
        return glide.at(0);
    }
    [[nodiscard]] float targetPitch() const noexcept
    {
        return glide.target();
    }
    /// Whether a note has played since prepare() or reset().
    [[nodiscard]] bool hasPlayed() const noexcept
    {
        return played;
    }
    /// The note's velocity now, 0-1 (the velocity modulation source, M1.5).
    [[nodiscard]] float velocity() const noexcept
    {
        return velocityRamp.at(0);
    }

    /// Silences the voice at once: the slots and the drift stop and the
    /// envelopes go idle (the next note-on, from silence, resets the filters
    /// and the grid). Unlike reset(), the
    /// random sequences carry on, so the next note from silence draws new
    /// free-running phases and a new drift state. Call it when the voice
    /// has finished (isActive() false) and before it takes another note: a
    /// finished voice that isn't processed still counts its slots as
    /// sounding, so its next note would jump from where they stopped.
    void stop() noexcept
    {
        for (auto& s : slots)
        {
            s.stop();
        }
        pitchDrift.stop();
        for (auto& e : envelopes)
        {
            e.reset();
        }
    }

    /// True while anything can still be heard: a filter with an input
    /// (not "--") that has oscillators in it (its own part's, or the other
    /// part's on AB), whose part's amp envelope hasn't released to 0. An
    /// envelope on a part nothing hears, or with no oscillators, doesn't
    /// count; nor does a crossing part's envelope once the envelope after
    /// its filter has closed. False before the first note-on.
    [[nodiscard]] bool isActive() const noexcept
    {
        for (std::size_t f = 0; f < partCount; ++f)
        {
            const bool fed = partSounds(f) || (routes[f].both && partSounds(1 - f));
            if (routes[f].active && fed && envelopes[f].isActive())
            {
                return true;
            }
        }
        return false;
    }

    /// The Voices readout's active count for this voice (manual p. 11;
    /// decided 2026-11-21, docs/dsp/voice-allocation.md): the unison voices
    /// of each part whose amp envelope is active and which something hears
    /// (its filter has an input, or its oscillators cross into the other
    /// filter). 0 before the first note-on.
    [[nodiscard]] std::size_t activeOscillatorVoices() const noexcept
    {
        std::size_t n = 0;
        for (std::size_t p = 0; p < partCount; ++p)
        {
            if (heard[p] && envelopes[p].isActive())
            {
                n += partVoices(p);
            }
        }
        return n;
    }

    /// Every oscillator's unison voices: the manual's voices per note, which
    /// times Polyphony is the readout's maximum. An oscillator at Voices 0
    /// adds nothing.
    [[nodiscard]] std::size_t oscillatorVoicesPerNote() const noexcept
    {
        return partVoices(0) + partVoices(1);
    }

    /// Adds the voice into channels 0 and 1 of `out` (two channels needed),
    /// with the knobs positioned at its first sample.
    void process(const dsp::AudioBlock& out, const dsp::VoiceKnobs& knobs) noexcept
    {
        const auto n = out.numSamples();
        std::size_t done = 0;
        while (done < n)
        {
            if (gridPosition == 0)
            {
                applyPendingAtGrid(knobs, done);
                feedFilters(knobs, done + gridSamples, keytrackPitch(knobs, done, gridSamples));
                updateMixSegment(knobs, done);
            }
            const auto length = std::min(n - done, gridSamples - gridPosition);
            renderChunk(out.subBlock(done, length), knobs, done);
            glide.advance(length);
            velocityRamp.advance(length);
            sinceNoteOn += length;
            done += length;
            gridPosition = (gridPosition + length) % gridSamples;
        }
    }

private:
    using Chunk = std::array<float, gridSamples>;
    using StereoChunk = std::array<Chunk, 2>;

    static constexpr MixContext unityContext{};

    /// At a grid point (the block's sample `done`): held, the gain at this
    /// sample; ramped, from where the last segment ended to the gain at the
    /// next grid point.
    void updateMixSegment(const dsp::VoiceKnobs& knobs, std::size_t done) noexcept
    {
        const auto& c = *mixContext;
        for (std::size_t p = 0; p < partCount; ++p)
        {
            if (c.placement == MixPlacement::gridHeld)
            {
                segmentStart[p] = mixAt(c, p, knobs.offset + done, knobs.time + done, sinceNoteOn);
            }
            else if (c.placement == MixPlacement::gridRamped)
            {
                segmentStart[p] = segmentEnd[p];
                segmentEnd[p] = mixAt(c, p, knobs.offset + done + gridSamples, knobs.time + done + gridSamples,
                                      sinceNoteOn + gridSamples);
            }
        }
    }

    /// Each part's Mix gain over the chunk starting at the block's sample `done`.
    void fillMixGains(const dsp::VoiceKnobs& knobs, std::size_t done, std::size_t length) noexcept
    {
        const auto& c = *mixContext;
        for (std::size_t p = 0; p < partCount; ++p)
        {
            auto& g = mixGains[p];
            switch (c.placement)
            {
            case MixPlacement::partBus:
                std::fill_n(g.begin(), length, 1.0f);
                break;
            case MixPlacement::gridHeld:
                std::fill_n(g.begin(), length, segmentStart[p]);
                break;
            case MixPlacement::gridRamped:
            {
                const float s = segmentStart[p];
                const float step = (segmentEnd[p] - s) / static_cast<float>(gridSamples);
                for (std::size_t i = 0; i < length; ++i)
                {
                    g[i] = s + (step * static_cast<float>(gridPosition + i));
                }
                break;
            }
            case MixPlacement::perSample:
            {
                const auto* ramp = c.mix[p];
                const auto at = knobs.offset + done;
                // A linear ramp is monotonic: the same value at both ends of
                // the chunk means a constant gain over it.
                const bool steady = c.modulation.kind == MixModulation::none &&
                                    (ramp == nullptr || sameValue(ramp->valueAfter(at), ramp->valueAfter(at + length)));
                if (steady)
                {
                    std::fill_n(g.begin(), length, mixAt(c, p, at, knobs.time + done, sinceNoteOn));
                    break;
                }
                for (std::size_t i = 0; i < length; ++i)
                {
                    g[i] = mixAt(c, p, at + i, knobs.time + done + i, sinceNoteOn + i);
                }
                break;
            }
            }
        }
    }

    [[nodiscard]] static bool sameValue(float a, float b) noexcept
    {
        return a <= b && a >= b;
    }

    struct Route
    {
        bool active = false; ///< the filter has an input (not "--")
        bool both = false;   ///< AB
    };

    void startPitch(float note, const dsp::NoteStart& start) noexcept
    {
        if (start.glide)
        {
            glide.start(start.from, note, start.glideSeconds);
        }
        else
        {
            glide.jump(note);
        }
    }

    /// The bend `ahead` samples into the block these knobs are for.
    [[nodiscard]] static float bendAt(const dsp::VoiceKnobs& knobs, std::size_t ahead) noexcept
    {
        return knobs.bend != nullptr ? knobs.bend->semitonesAt(knobs.time + ahead) : 0.0f;
    }

    /// The oscillators' pitch `ahead` samples after the block's sample `done`,
    /// where the glide stands (the glide plus the bend; the master tune is
    /// the slots').
    [[nodiscard]] float pitchAt(const dsp::VoiceKnobs& knobs, std::size_t done, std::size_t ahead) const noexcept
    {
        return glide.at(ahead) + bendAt(knobs, done + ahead);
    }

    /// The pitch keytrack follows (F18, F19): the oscillators' plus the
    /// master tune.
    [[nodiscard]] float keytrackPitch(const dsp::VoiceKnobs& knobs, std::size_t done, std::size_t ahead) const noexcept
    {
        return pitchAt(knobs, done, ahead) + masterTuneOffset;
    }

    [[nodiscard]] std::size_t partVoices(std::size_t p) const noexcept
    {
        return slots[p * slotsPerPart].voices() + slots[(p * slotsPerPart) + 1].voices();
    }

    /// Whether part p has an oscillator voice on, or one still fading out.
    [[nodiscard]] bool partSounds(std::size_t p) const noexcept
    {
        for (std::size_t s = 0; s < slotsPerPart; ++s)
        {
            const auto& slot = slots[(p * slotsPerPart) + s];
            if (slot.voices() > 0 || slot.soundingVoices() > 0)
            {
                return true;
            }
        }
        return false;
    }

    void updateRoutes() noexcept
    {
        for (std::size_t f = 0; f < partCount; ++f)
        {
            routes[f] = {.active = inputs[f] != dsp::FilterInput::none, .both = inputs[f] == dsp::FilterInput::both};
        }
        for (std::size_t p = 0; p < partCount; ++p)
        {
            heard[p] = routes[p].active || routes[1 - p].both;
        }
    }

    void applyPending() noexcept
    {
        inputs = pendingInputs;
        for (std::size_t f = 0; f < partCount; ++f)
        {
            filters[f].setBypassed(pendingBypass[f]);
        }
        updateRoutes();
    }

    /// At a grid point: a filter that comes on (from "--" or out of Bypass)
    /// is fed for this sample and starts from rest, on the voice's grid.
    void applyPendingAtGrid(const dsp::VoiceKnobs& knobs, std::size_t ahead) noexcept
    {
        std::array<bool, partCount> wasRunning{};
        for (std::size_t f = 0; f < partCount; ++f)
        {
            wasRunning[f] = routes[f].active && !filters[f].isBypassed();
        }
        applyPending();
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (routes[f].active && !filters[f].isBypassed() && !wasRunning[f])
            {
                feedFilter(f, knobs, ahead,
                           dsp::keytrackOctaves(keytrackPitch(knobs, ahead, 0), knobs.keytrack / 10.0f));
                filters[f].reset();
            }
        }
    }

    [[nodiscard]] static float read(const dsp::LinearSmoother* ramp, std::size_t ahead) noexcept
    {
        return ramp != nullptr ? ramp->valueAfter(ahead) : 0.0f;
    }

    void feedFilter(std::size_t f, const dsp::VoiceKnobs& knobs, std::size_t ahead, float keytrack) noexcept
    {
        ahead += knobs.offset; // the ramps stand that many samples before this block
        const float octaves = dsp::filterCutoffOctaves({
            .cutoff = read(knobs.cutoff[f], ahead),
            .controlCutoff = read(knobs.controlCutoff, ahead),
            .keytrack = keytrack,
        });
        const float k = dsp::filterResonanceK({
            .resonance = read(knobs.resonance[f], ahead),
            .controlResonance = read(knobs.controlResonance, ahead),
        });
        if (std::isfinite(octaves))
        {
            lastOctaves[f] = octaves;
        }
        if (std::isfinite(k))
        {
            lastK[f] = k;
        }
        filters[f].setCutoff(dsp::filterControlLaw.anchorHz);
        filters[f].setCutoffModulation(lastOctaves[f]);
        filters[f].setResonance(0.0f);
        filters[f].setResonanceModulation(lastK[f]);
    }

    /// Each running filter, for the sample `ahead` samples into the block.
    void feedFilters(const dsp::VoiceKnobs& knobs, std::size_t ahead, float pitch) noexcept
    {
        const float keytrack = dsp::keytrackOctaves(pitch, knobs.keytrack / 10.0f); // once for both
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (routes[f].active && !filters[f].isBypassed())
            {
                feedFilter(f, knobs, ahead, keytrack);
            }
        }
    }

    [[nodiscard]] dsp::AudioBlock block(StereoChunk& chunk, std::array<float*, 2>& pointers,
                                        std::size_t length) noexcept
    {
        pointers = {chunk[0].data(), chunk[1].data()};
        return {pointers.data(), 2, length};
    }

    void renderChunk(const dsp::AudioBlock& out, const dsp::VoiceKnobs& knobs, std::size_t done) noexcept
    {
        const auto length = out.numSamples();

        // 1. The oscillators of each heard part, added into its buffer at the
        // pitch plus drift, in runs of constant drift. While the pitch moves
        // (a glide, a bend's ramp) the slots take it every sample; otherwise
        // once, which is the same to the bit at a constant pitch and cheaper.
        const bool moving =
            glide.isGliding() || (knobs.bend != nullptr && !knobs.bend->isSteadyFrom(knobs.time + done));
        const float steadyPitch = pitchAt(knobs, done, 0);
        if (moving)
        {
            for (std::size_t i = 0; i < length; ++i)
            {
                pitches[i] = pitchAt(knobs, done, i);
            }
        }
        std::array<dsp::AudioBlock, partCount> parts{};
        for (std::size_t p = 0; p < partCount; ++p)
        {
            parts[p] = block(partBuffers[p], partPointers[p], length);
            if (heard[p])
            {
                parts[p].clear();
            }
        }
        pitchDrift.process(length,
                           [&](std::size_t start, std::size_t run, float offset) noexcept
                           {
                               if (moving)
                               {
                                   for (std::size_t i = 0; i < run; ++i)
                                   {
                                       notes[i] = pitches[start + i] + offset;
                                   }
                               }
                               for (std::size_t p = 0; p < partCount; ++p)
                               {
                                   if (!heard[p])
                                   {
                                       continue;
                                   }
                                   const auto piece = parts[p].subBlock(start, run);
                                   for (std::size_t s = 0; s < slotsPerPart; ++s)
                                   {
                                       auto& slot = slots[(p * slotsPerPart) + s];
                                       if (slot.soundingVoices() == 0)
                                       {
                                           continue;
                                       }
                                       if (moving)
                                       {
                                           slot.process(piece, std::span<const float>{notes}.first(run));
                                       }
                                       else
                                       {
                                           slot.process(piece, steadyPitch + offset);
                                       }
                                   }
                               }
                           });

        // 2. The amp envelopes.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            envelopes[p].process(std::span{envelopeBuffers[p]}.first(length));
        }

        // 2b. Each part's Mix gain over the chunk.
        fillMixGains(knobs, done, length);

        // 3. Each running filter's input, in its own buffer; 4. the filter, in
        // place; 5. its output through its part's envelope and its part's Mix,
        // into `out` (partBus: unscaled, into the part's own two channels).
        const bool bus = mixContext->placement == MixPlacement::partBus;
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (!routes[f].active)
            {
                continue;
            }
            const auto left = out.channel(bus ? 2 * f : 0);
            const auto right = out.channel(bus ? (2 * f) + 1 : 1);
            const auto input = block(filterBuffers[f], filterPointers[f], length);
            buildInput(f, length);
            filters[f].process(input);
            const auto& e = envelopeBuffers[f];
            const auto& g = mixGains[f];
            const auto y0 = input.channel(0);
            const auto y1 = input.channel(1);
            if (unityLevel)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    const float ge = g[i] * e[i]; // exactly e[i] at unity
                    left[i] += ge * y0[i];
                    right[i] += ge * y1[i];
                }
            }
            else
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    const float ge = g[i] * e[i];
                    left[i] += (ge * y0[i]) * outputLevel;
                    right[i] += (ge * y1[i]) * outputLevel;
                }
            }
        }
    }

    /// own (x g), or for AB own + the other part through its envelope (x g).
    void buildInput(std::size_t f, std::size_t length) noexcept
    {
        const std::size_t other = 1 - f;
        const auto& envelopeOther = envelopeBuffers[other];
        for (std::size_t c = 0; c < 2; ++c)
        {
            const auto& own = partBuffers[f][c];
            const auto& crossing = partBuffers[other][c];
            auto& dst = filterBuffers[f][c];
            if (routes[f].both && mixContext->crossingTakesSourceMix)
            {
                // V3's alternative: the crossing part's own Mix too, before the filter.
                const auto& mixOther = mixGains[other];
                for (std::size_t i = 0; i < length; ++i)
                {
                    const float enveloped = (envelopeOther[i] * mixOther[i]) * crossing[i];
                    dst[i] = own[i] + enveloped;
                }
            }
            else if (routes[f].both)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    // Its own statement, so the product is rounded before the
                    // sum on every compiler (no fused multiply-add).
                    const float enveloped = envelopeOther[i] * crossing[i];
                    dst[i] = own[i] + enveloped;
                }
            }
            else
            {
                std::copy_n(own.begin(), length, dst.begin());
            }
            if (!unityLevel)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    dst[i] *= inputLevel;
                }
            }
        }
    }

    std::array<Slot, slotCount> slots;
    std::array<dsp::LadderFilter, partCount> filters{};
    std::array<dsp::Adsr, partCount> envelopes{};
    dsp::PitchDrift pitchDrift;

    std::array<dsp::FilterInput, partCount> pendingInputs{dsp::FilterInput::own, dsp::FilterInput::own};
    std::array<bool, partCount> pendingBypass{};
    std::array<dsp::FilterInput, partCount> inputs{dsp::FilterInput::own, dsp::FilterInput::own};
    std::array<Route, partCount> routes{};
    std::array<bool, partCount> heard{};

    float inputLevel = 1.0f;
    float outputLevel = 1.0f;
    bool unityLevel = true;

    std::array<StereoChunk, partCount> partBuffers{};
    std::array<StereoChunk, partCount> filterBuffers{};
    std::array<Chunk, partCount> envelopeBuffers{};
    std::array<std::array<float*, 2>, partCount> partPointers{};
    std::array<std::array<float*, 2>, partCount> filterPointers{};

    std::array<float, partCount> lastOctaves{};
    std::array<float, partCount> lastK{};
    dsp::Glide glide;
    dsp::Glide velocityRamp; ///< the note's velocity, 0-1, ramped over a glide
    Chunk pitches{};         ///< the pitch per sample of a chunk while it moves
    Chunk notes{};           ///< ... plus drift, for one of drift's runs
    float masterTuneHz = dsp::concertPitchHz;
    float masterTuneOffset = 0.0f; ///< dsp::masterTuneSemitones(masterTuneHz): 0 at 440
    bool played = false;
    std::size_t gridPosition = 0;

    const MixContext* mixContext;
    std::array<Chunk, partCount> mixGains{};
    std::array<float, partCount> segmentStart{1.0f, 1.0f};
    std::array<float, partCount> segmentEnd{1.0f, 1.0f};
    std::uint64_t sinceNoteOn = 0; ///< samples rendered since the latest note-on (the pluck's clock)
};

// --- The engine ---------------------------------------------------------------------------

/// The filter knobs every voice reads: Cutoff 5 + the Filter Control's 5
/// (4,677 Hz) and Resonance 0, unless set; constant.
struct FilterKnobs
{
    FilterKnobs()
    {
        set(5.0f, 0.0f);
    }
    void set(float cutoffKnob, float resonanceKnob)
    {
        cutoff.setCurrentAndTarget(cutoffKnob);
        control.setCurrentAndTarget(5.0f);
        resonance.setCurrentAndTarget(resonanceKnob);
        zero.setCurrentAndTarget(0.0f);
    }
    [[nodiscard]] dsp::VoiceKnobs knobs() const noexcept
    {
        return {
            .cutoff = {&cutoff, &cutoff},
            .resonance = {&resonance, &resonance},
            .controlCutoff = &control,
            .controlResonance = &zero,
        };
    }
    dsp::LinearSmoother cutoff;
    dsp::LinearSmoother control;
    dsp::LinearSmoother resonance;
    dsp::LinearSmoother zero;
};

enum class EngineEventKind : std::uint8_t
{
    voice,      ///< a VoiceEvent for the allocator
    mix,        ///< Mix A (part 0) or B (part 1): the knob's internal value
    mainVolume, ///< Main Volume's linear gain
};

struct EngineEvent
{
    std::size_t offset = 0;
    EngineEventKind kind = EngineEventKind::voice;
    dsp::VoiceEvent voice{};
    std::size_t part = 0;
    float value = 0.0f;
};

/// The candidate mixer around the real VoiceAllocator: 16 MixVoices, the Mix
/// A/B knob ramps (global LinearSmoothers, 10 ms), the part buses (partBus),
/// Main Volume (a global LinearSmoother on the gain, 10 ms or none), and the
/// meters (if attached), after Main Volume. Each host block is split at its
/// parameter events; note events go to the allocator at their samples. The
/// output is written, not added to.
class MixEngine
{
public:
    using Voice = MixVoice<>;
    using Allocator = dsp::VoiceAllocator<Voice, 16>;
    static constexpr float rampSeconds = 0.01f;

    MixEngine(const dsp::MinBlepTable& table, const MixContext& settings, std::uint64_t seed = 1) : context(settings)
    {
        context.mix = {mixRamps.data(), mixRamps.data() + 1};
        allocator = std::make_unique<Allocator>(seed, table, &context);
        for (auto& r : mixRamps)
        {
            r.setRampTime(rampSeconds);
            r.setCurrentAndTarget(10.0f);
        }
        mainVolume.setRampTime(rampSeconds);
        mainVolume.setCurrentAndTarget(1.0f);
    }

    [[nodiscard]] Allocator& voices() noexcept
    {
        return *allocator;
    }
    [[nodiscard]] FilterKnobs& filterKnobs() noexcept
    {
        return filters;
    }
    void attach(MeterSet* m) noexcept
    {
        meters = m;
    }
    /// Main Volume's ramp: 10 ms (true) or none.
    void setMainVolumeSmoothing(bool on) noexcept
    {
        mainVolume.setRampTime(on ? rampSeconds : 0.0f);
    }
    /// The Mix knobs' ramps: 10 ms (true) or none, for the next prepare().
    void setMixSmoothing(bool on) noexcept
    {
        for (auto& r : mixRamps)
        {
            r.setRampTime(on ? rampSeconds : 0.0f);
        }
    }
    void setMix(std::size_t part, float internal) noexcept
    {
        mixRamps[part].setCurrentAndTarget(internal);
    }
    void setMainVolume(float gain) noexcept
    {
        mainVolume.setCurrentAndTarget(gain);
    }

    void prepare(const dsp::ProcessSpec& spec)
    {
        context.sampleRate = spec.sampleRate;
        for (auto& r : mixRamps)
        {
            r.prepare(spec);
        }
        mainVolume.prepare(spec);
        allocator->prepare({.sampleRate = spec.sampleRate, .maxBlockSize = spec.maxBlockSize, .numChannels = 2});
        bus.assign(4 * spec.maxBlockSize, 0.0f);
        notes.assign(256, {});
        clock = 0;
    }

    void process(const dsp::AudioBlock& out, std::span<const EngineEvent> events) noexcept
    {
        const auto n = out.numSamples();
        out.clear();
        std::size_t from = 0;
        std::size_t e = 0;
        while (from < n)
        {
            std::size_t count = 0;
            std::size_t to = n;
            while (e < events.size())
            {
                const auto at = std::min(events[e].offset, n);
                if (events[e].kind != EngineEventKind::voice)
                {
                    if (at > from)
                    {
                        to = at;
                        break;
                    }
                    apply(events[e]);
                }
                else if (count < notes.size())
                {
                    notes[count] = events[e].voice;
                    notes[count].offset = at - from;
                    ++count;
                }
                ++e;
            }
            render(out.subBlock(from, to - from), std::span{notes}.first(count));
            from = to;
        }
        std::size_t count = 0;
        for (; e < events.size(); ++e)
        {
            if (events[e].kind != EngineEventKind::voice)
            {
                apply(events[e]);
            }
            else if (count < notes.size())
            {
                notes[count] = events[e].voice;
                notes[count].offset = 0;
                ++count;
            }
        }
        if (count > 0)
        {
            allocator->process(out.subBlock(n, 0), std::span{notes}.first(count), filters.knobs());
        }
    }

private:
    void apply(const EngineEvent& event) noexcept
    {
        if (event.kind == EngineEventKind::mix)
        {
            mixRamps[std::min<std::size_t>(event.part, 1)].setTarget(event.value);
        }
        else
        {
            mainVolume.setTarget(event.value);
        }
    }

    void render(const dsp::AudioBlock& piece, std::span<const dsp::VoiceEvent> pieceNotes) noexcept
    {
        const auto length = piece.numSamples();
        if (context.placement == MixPlacement::partBus)
        {
            std::array<float*, 4> pointers{};
            for (std::size_t c = 0; c < 4; ++c)
            {
                pointers[c] = bus.data() + (c * (bus.size() / 4));
            }
            const dsp::AudioBlock buses{pointers.data(), 4, length};
            buses.clear();
            allocator->process(buses, pieceNotes, filters.knobs());
            const auto left = piece.channel(0);
            const auto right = piece.channel(1);
            for (std::size_t i = 0; i < length; ++i)
            {
                // A global gain per part: the knob ramp and a free LFO only;
                // a per-voice source (the pluck) has nothing to act on here.
                const float ga = busGain(0, i);
                const float gb = busGain(1, i);
                left[i] = (ga * pointers[0][i]) + (gb * pointers[2][i]);
                right[i] = (ga * pointers[1][i]) + (gb * pointers[3][i]);
            }
        }
        else
        {
            allocator->process(piece, pieceNotes, filters.knobs());
        }
        const auto left = piece.channel(0);
        const auto right = piece.channel(1);
        if (mainVolume.isSmoothing())
        {
            for (std::size_t i = 0; i < length; ++i)
            {
                const float g = mainVolume.next();
                left[i] *= g;
                right[i] *= g;
            }
        }
        else
        {
            const float g = mainVolume.currentValue();
            for (std::size_t i = 0; i < length; ++i)
            {
                left[i] *= g;
                right[i] *= g;
            }
        }
        if (meters != nullptr)
        {
            meters->process(piece);
        }
        for (auto& r : mixRamps)
        {
            r.skip(length);
        }
        clock += length;
    }

    [[nodiscard]] float busGain(std::size_t part, std::size_t i) const noexcept
    {
        MixModulationSpec m = context.modulation;
        if (m.kind == MixModulation::pluck)
        {
            m.kind = MixModulation::none;
        }
        const float knob = mixRamps[part].valueAfter(i);
        return mixGain(context.law, clampMix(knob + modulationAt(m, context.sampleRate, clock + i, 0), context.clamp));
    }

    MixContext context;
    std::array<dsp::LinearSmoother, 2> mixRamps{};
    dsp::LinearSmoother mainVolume;
    FilterKnobs filters;
    std::unique_ptr<Allocator> allocator;
    MeterSet* meters = nullptr;
    std::vector<float> bus;
    std::vector<dsp::VoiceEvent> notes;
    std::uint64_t clock = 0;
};

} // namespace anoesis::test::mixer
