// Test-only prototypes of tempo sync, measured by the [sync-report],
// [sync-long-report], and the [sync] benchmarks to choose a model
// (docs/dsp/tempo-sync.md).
//
// Note values are exact: a Ratio of whole notes (8/1 is eight whole notes,
// 1/4 a quarter note), and a kind (straight, triplet x 2/3, dotted x 3/2). A
// whole note is four quarter notes whatever the host's time signature: "one
// bar contains four beats" (manual p. 12). Conversions to seconds, hertz,
// samples, and the LFO's 64-bit phase increment take plain values: the
// tempo in quarter notes per minute and the sample rate.
//
// The host is a HostClock per block, as VST3 and AU give it: the tempo, the
// position in quarter notes (PPQ) at the block's first sample, and whether
// the transport plays. Each sync model turns it into a BlockPlan: the phase
// increment for the block and, optionally, a jump of the phase first. One
// renderer, SyncedLfo<Model>, runs every model: it reads a periodic row of
// lfoShapes at the phase and slews a jump as Lfo does (linearly from the
// last output, over 1 ms or 1/16 cycle if shorter), so the models differ
// only in their plans:
//
// - RateOnly: the increment from the tempo; the phase starts at the note-on
//   and runs free (inventory L12's hypothesis).
// - RateOnlyFloatHz: the same through today's Lfo::setFrequency(float): the
//   rate rounded to a float in hertz first.
// - Locked: the phase is the bar grid's, cycles counted from PPQ 0, set at
//   every block while the transport plays (slewed if the set is large).
// - Hybrid: rate from the tempo; at each block the error against the bar
//   grid is corrected by nudging the increment for the block, or by a
//   slewed jump if it is large (a loop, the transport starting).
// - NoteAnchored: L12's semantics (the phase starts at the note-on) but the
//   tempo's integral comes from the host's position: the error against
//   "note-on phase + quarters played since" is nudged away, and a jump in
//   the position (a loop, the transport starting) re-anchors instead.
//
// Every model free-runs at the host tempo while the transport is stopped.

#pragma once

#include <anoesis/dsp/control/lfo.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>

namespace anoesis::test::sync
{

namespace dsp = anoesis::dsp;

/// An exact fraction, always reduced, with a positive denominator.
class Ratio
{
public:
    constexpr Ratio() noexcept = default;

    constexpr Ratio(std::uint64_t numerator, std::uint64_t denominator) noexcept
    {
        const std::uint64_t g = std::gcd(numerator, denominator);
        num = numerator / g;
        den = denominator / g;
    }

    [[nodiscard]] constexpr std::uint64_t numerator() const noexcept
    {
        return num;
    }

    [[nodiscard]] constexpr std::uint64_t denominator() const noexcept
    {
        return den;
    }

    [[nodiscard]] constexpr double toDouble() const noexcept
    {
        return static_cast<double>(num) / static_cast<double>(den);
    }

    friend constexpr Ratio operator*(Ratio a, Ratio b) noexcept
    {
        // Cross-reduce first, so the products stay as small as the result.
        const std::uint64_t g1 = std::gcd(a.num, b.den);
        const std::uint64_t g2 = std::gcd(b.num, a.den);
        return Ratio{(a.num / g1) * (b.num / g2), (a.den / g2) * (b.den / g1)};
    }

    friend constexpr Ratio operator+(Ratio a, Ratio b) noexcept
    {
        const std::uint64_t g = std::gcd(a.den, b.den);
        return Ratio{(a.num * (b.den / g)) + (b.num * (a.den / g)), a.den * (b.den / g)};
    }

    friend constexpr bool operator==(Ratio a, Ratio b) noexcept = default;

    friend constexpr std::strong_ordering operator<=>(Ratio a, Ratio b) noexcept
    {
        return (a.num * b.den) <=> (b.num * a.den);
    }

private:
    std::uint64_t num = 0;
    std::uint64_t den = 1;
};

/// Straight, triplet (2/3 of the length), or dotted (3/2 of the length): the
/// developer's "A triplet is 2/3 of the length, while dotted means 3/2".
enum class NoteKind : std::uint8_t
{
    straight,
    triplet,
    dotted,
};

/// A note value: a length in whole notes and a kind.
struct NoteValue
{
    Ratio base{1, 4};
    NoteKind kind = NoteKind::straight;

    /// The length in whole notes, exactly.
    [[nodiscard]] constexpr Ratio wholeNotes() const noexcept
    {
        switch (kind)
        {
        case NoteKind::triplet:
            return base * Ratio{2, 3};
        case NoteKind::dotted:
            return base * Ratio{3, 2};
        case NoteKind::straight:
            break;
        }
        return base;
    }

    /// The length in quarter notes (beats), exactly.
    [[nodiscard]] constexpr Ratio quarterNotes() const noexcept
    {
        return wholeNotes() * Ratio{4, 1};
    }

    friend constexpr bool operator==(const NoteValue& a, const NoteValue& b) noexcept = default;
};

[[nodiscard]] constexpr NoteValue straight(std::uint64_t num, std::uint64_t den) noexcept
{
    return {.base = Ratio{num, den}, .kind = NoteKind::straight};
}

[[nodiscard]] constexpr NoteValue triplet(std::uint64_t num, std::uint64_t den) noexcept
{
    return {.base = Ratio{num, den}, .kind = NoteKind::triplet};
}

[[nodiscard]] constexpr NoteValue dotted(std::uint64_t num, std::uint64_t den) noexcept
{
    return {.base = Ratio{num, den}, .kind = NoteKind::dotted};
}

/// Seconds per note at `bpm` quarter notes per minute.
[[nodiscard]] inline double seconds(NoteValue value, double bpm) noexcept
{
    const Ratio q = value.quarterNotes();
    return 60.0 * static_cast<double>(q.numerator()) / (bpm * static_cast<double>(q.denominator()));
}

/// Cycles per second of an LFO whose cycle is `value`.
[[nodiscard]] inline double hertz(NoteValue value, double bpm) noexcept
{
    const Ratio q = value.quarterNotes();
    return bpm * static_cast<double>(q.denominator()) / (60.0 * static_cast<double>(q.numerator()));
}

/// Samples per note (fractional).
[[nodiscard]] inline double samples(NoteValue value, double bpm, double sampleRate) noexcept
{
    const Ratio q = value.quarterNotes();
    return 60.0 * sampleRate * static_cast<double>(q.numerator()) / (bpm * static_cast<double>(q.denominator()));
}

/// 2^64 x a value in [0, 1), rounded; 1 and above wrap to 0.
[[nodiscard]] inline std::uint64_t toFixed(double cycles) noexcept
{
    const double scaled = std::round(std::ldexp(cycles, 64));
    if (!(scaled < 0x1p64) || !(scaled > 0.0))
    {
        return 0;
    }
    return static_cast<std::uint64_t>(scaled);
}

/// The LFO's phase increment for a cycle of `value`: cycles per sample x
/// 2^64, rounded once from double (two roundings before it, each 2^-53
/// relative), at most half a cycle.
[[nodiscard]] inline std::uint64_t phaseIncrement(NoteValue value, double bpm, double sampleRate) noexcept
{
    const Ratio q = value.quarterNotes();
    const double cycles =
        (bpm * static_cast<double>(q.denominator())) / (60.0 * sampleRate * static_cast<double>(q.numerator()));
    return toFixed(std::min(cycles, 0.5));
}

/// floor(a x 2^64 / b) and the remainder, exactly, for a < b < 2^63: long
/// division one bit at a time.
struct FixedQuotient
{
    std::uint64_t quotient = 0;
    std::uint64_t remainder = 0;
};

[[nodiscard]] constexpr FixedQuotient fixedQuotient(std::uint64_t a, std::uint64_t b) noexcept
{
    FixedQuotient result{.quotient = 0, .remainder = a};
    for (int bit = 0; bit < 64; ++bit)
    {
        result.remainder <<= 1U;
        result.quotient <<= 1U;
        if (result.remainder >= b)
        {
            result.remainder -= b;
            result.quotient |= 1U;
        }
    }
    return result;
}

/// The bar grid's phase at `ppq` for a cycle of `value`: the fraction of a
/// cycle since the last cycle start, cycles counted from PPQ 0, 2^64 per
/// cycle. Computed as fmod(ppq x den, num) / num, so the cycle length is
/// exact and only the host's double position rounds.
[[nodiscard]] inline std::uint64_t gridPhase(double ppq, NoteValue value) noexcept
{
    const Ratio q = value.quarterNotes();
    const auto units = static_cast<double>(q.numerator());
    double r = std::fmod(ppq * static_cast<double>(q.denominator()), units);
    if (r < 0.0)
    {
        r += units;
    }
    return toFixed(r / units);
}

/// The size of a wrapped phase difference, 2^64 per cycle (at most half).
[[nodiscard]] constexpr std::uint64_t distance(std::uint64_t difference) noexcept
{
    return std::min(difference, std::uint64_t{0} - difference);
}

/// A wrapped phase difference as signed cycles.
[[nodiscard]] inline double signedCycles(std::uint64_t difference) noexcept
{
    return std::ldexp(static_cast<double>(static_cast<std::int64_t>(difference)), -64);
}

// ---------------------------------------------------------------------------
// Value lists

inline constexpr std::array<Ratio, 10> straightBases{
    Ratio{8, 1}, Ratio{4, 1}, Ratio{2, 1},  Ratio{1, 1},  Ratio{1, 2},
    Ratio{1, 4}, Ratio{1, 8}, Ratio{1, 16}, Ratio{1, 32}, Ratio{1, 64},
};

inline constexpr std::size_t valueCount = straightBases.size() * 3;

/// Candidate order 1: every straight value from 8/1 to 1/64 with its dotted
/// and triplet value (30), grouped by base: 8/1D, 8/1, 8/1T, 4/1D, ...
[[nodiscard]] constexpr std::array<NoteValue, valueCount> groupedValues() noexcept
{
    std::array<NoteValue, valueCount> list{};
    std::size_t i = 0;
    for (const Ratio base : straightBases)
    {
        list[i++] = NoteValue{.base = base, .kind = NoteKind::dotted};
        list[i++] = NoteValue{.base = base, .kind = NoteKind::straight};
        list[i++] = NoteValue{.base = base, .kind = NoteKind::triplet};
    }
    return list;
}

/// Candidate order 2: the same 30, longest first (a knob that turns
/// monotonically).
[[nodiscard]] constexpr std::array<NoteValue, valueCount> sortedValues() noexcept
{
    auto list = groupedValues();
    std::ranges::sort(list, [](const NoteValue& a, const NoteValue& b) { return a.wholeNotes() > b.wholeNotes(); });
    return list;
}

// ---------------------------------------------------------------------------
// The host and the models

/// What the host says at a block's first sample.
struct HostClock
{
    double bpm = 120.0;
    double ppq = 0.0; ///< quarter notes since the song's start
    bool playing = false;
};

/// What a model asks of the renderer at a block's start.
struct BlockPlan
{
    std::uint64_t increment = 0; ///< per sample, for the whole block
    bool jump = false;           ///< set the phase to `target` first
    std::uint64_t target = 0;
    bool slewed = false; ///< slew the jump in the output
};

/// What every model shares: the sample rate, the note value, and the
/// increment for a tempo (cached until the tempo changes).
class SyncModel
{
public:
    void prepare(double rate) noexcept
    {
        sampleRate = rate;
        cachedBpm = noTempo;
    }

    void setNote(NoteValue value) noexcept
    {
        note = value;
        cachedBpm = noTempo;
    }

    [[nodiscard]] NoteValue noteValue() const noexcept
    {
        return note;
    }

protected:
    [[nodiscard]] std::uint64_t rateFor(double bpm) noexcept
    {
        // The tempo's bits are the cache key: any change recomputes.
        if (std::bit_cast<std::uint64_t>(bpm) != cachedBpm)
        {
            cachedBpm = std::bit_cast<std::uint64_t>(bpm);
            cachedIncrement = phaseIncrement(note, bpm, sampleRate);
        }
        return cachedIncrement;
    }

    double sampleRate = 48000.0;
    NoteValue note{};

private:
    static constexpr std::uint64_t noTempo = ~std::uint64_t{0};
    std::uint64_t cachedBpm = noTempo;
    std::uint64_t cachedIncrement = 0;
};

class RateOnly : public SyncModel
{
public:
    static constexpr const char* name = "rate only";

    void noteOn() noexcept {}

    [[nodiscard]] BlockPlan begin(const HostClock& clock, std::uint64_t /*phase*/, std::size_t /*length*/) noexcept
    {
        return {.increment = rateFor(clock.bpm)};
    }
};

/// Today's Lfo API: the rate in hertz as a float, the increment from that.
class RateOnlyFloatHz : public SyncModel
{
public:
    static constexpr const char* name = "rate only, float Hz";

    void noteOn() noexcept {}

    [[nodiscard]] BlockPlan begin(const HostClock& clock, std::uint64_t /*phase*/, std::size_t /*length*/) noexcept
    {
        const auto hz = static_cast<float>(hertz(note, clock.bpm));
        return {.increment = toFixed(std::min(static_cast<double>(hz) / sampleRate, 0.5))};
    }
};

class Locked : public SyncModel
{
public:
    static constexpr const char* name = "locked";
    /// A set larger than 2^-20 cycle is slewed; a smaller one is not.
    static constexpr std::uint64_t slewAbove = std::uint64_t{1} << 44U;

    void noteOn() noexcept {}

    [[nodiscard]] BlockPlan begin(const HostClock& clock, std::uint64_t phase, std::size_t /*length*/) noexcept
    {
        BlockPlan plan{.increment = rateFor(clock.bpm)};
        if (clock.playing)
        {
            const std::uint64_t target = gridPhase(clock.ppq, note);
            if (target != phase)
            {
                plan.jump = true;
                plan.target = target;
                plan.slewed = distance(target - phase) > slewAbove;
            }
        }
        return plan;
    }
};

/// The shared correction of Hybrid and NoteAnchored: an error up to
/// `deadband` is left alone; one past `capture` is a slewed jump; one in
/// between is nudged away over the block, by at most `maxNudge` of the rate.
struct Correction
{
    static constexpr std::uint64_t deadband = std::uint64_t{1} << 40U; ///< 2^-24 cycle
    static constexpr std::uint64_t capture = std::uint64_t{1} << 58U;  ///< 1/64 cycle
    static constexpr double maxNudge = 1.0 / 16.0;

    static void apply(BlockPlan& plan, std::uint64_t target, std::uint64_t phase, std::size_t length) noexcept
    {
        const std::uint64_t difference = target - phase;
        const std::uint64_t size = distance(difference);
        if (size <= deadband || length == 0)
        {
            return;
        }
        if (size > capture)
        {
            plan.jump = true;
            plan.target = target;
            plan.slewed = true;
            return;
        }
        const auto error = static_cast<std::int64_t>(difference);
        const auto limit = static_cast<std::int64_t>(static_cast<double>(plan.increment) * maxNudge);
        const std::int64_t nudge = std::clamp<std::int64_t>(error / static_cast<std::int64_t>(length), -limit, limit);
        plan.increment = static_cast<std::uint64_t>(static_cast<std::int64_t>(plan.increment) + nudge);
    }
};

class Hybrid : public SyncModel
{
public:
    static constexpr const char* name = "hybrid (bar grid)";

    void noteOn() noexcept {}

    [[nodiscard]] BlockPlan begin(const HostClock& clock, std::uint64_t phase, std::size_t length) noexcept
    {
        BlockPlan plan{.increment = rateFor(clock.bpm)};
        if (clock.playing)
        {
            Correction::apply(plan, gridPhase(clock.ppq, note), phase, length);
        }
        return plan;
    }
};

class NoteAnchored : public SyncModel
{
public:
    static constexpr const char* name = "note-anchored";
    /// A position more than this far (in quarter notes) from where the last
    /// block predicts is a jump (a loop, a locate): re-anchor.
    static constexpr double jumpTolerance = 1.0 / 64.0;

    void noteOn() noexcept
    {
        anchored = false;
    }

    [[nodiscard]] BlockPlan begin(const HostClock& clock, std::uint64_t phase, std::size_t length) noexcept
    {
        BlockPlan plan{.increment = rateFor(clock.bpm)};
        if (!clock.playing)
        {
            anchored = false;
            return plan;
        }
        const std::uint64_t grid = gridPhase(clock.ppq, note);
        if (anchored)
        {
            const double expected = lastPpq + (lastBpm * static_cast<double>(lastLength) / (60.0 * sampleRate));
            if (std::abs(clock.ppq - expected) > jumpTolerance)
            {
                anchored = false;
            }
        }
        if (!anchored)
        {
            offset = phase - grid;
            anchored = true;
        }
        else
        {
            Correction::apply(plan, grid + offset, phase, length);
        }
        lastPpq = clock.ppq;
        lastBpm = clock.bpm;
        lastLength = length;
        return plan;
    }

private:
    bool anchored = false;
    std::uint64_t offset = 0;
    double lastPpq = 0.0;
    double lastBpm = 120.0;
    std::size_t lastLength = 0;
};

// ---------------------------------------------------------------------------
// The renderer

template <typename Model>
class SyncedLfo
{
public:
    static constexpr float slewSeconds = dsp::Lfo::slewSeconds;
    static constexpr double slewCycleFraction = dsp::Lfo::slewCycleFraction;

    Model model;

    void prepare(double rate) noexcept
    {
        sampleRate = rate;
        model.prepare(rate);
    }

    /// A periodic row of lfoShapes.
    void setWaveform(dsp::LfoWaveform waveform) noexcept
    {
        row = &dsp::lfoShape(waveform);
    }

    void setNote(NoteValue value) noexcept
    {
        model.setNote(value);
    }

    /// The cycle's start, at once (a voice from silence).
    void noteOn() noexcept
    {
        position = 0;
        model.noteOn();
        started = false;
        slewTotal = 0;
        slewCount = 0;
    }

    /// One block, the clock at its first sample.
    void process(std::span<float> out, const HostClock& clock) noexcept
    {
        plan(clock, out.size());
        for (auto& y : out)
        {
            y = valueAt(position) + slewTerm();
            position += increment;
        }
        if (!out.empty())
        {
            lastOut = out.back();
            started = true;
        }
    }

    /// One block without rendering: the phase only.
    void advance(std::size_t length, const HostClock& clock) noexcept
    {
        plan(clock, length);
        position += static_cast<std::uint64_t>(length) * increment;
        started = true;
    }

    /// The model's phase (the row's start not added), 2^64 per cycle.
    [[nodiscard]] std::uint64_t phase() const noexcept
    {
        return position;
    }

    [[nodiscard]] std::uint64_t phaseIncrement() const noexcept
    {
        return increment;
    }

    [[nodiscard]] std::size_t jumps() const noexcept
    {
        return jumpCount;
    }

private:
    void plan(const HostClock& clock, std::size_t length) noexcept
    {
        const BlockPlan p = model.begin(clock, position, length);
        increment = p.increment;
        if (p.jump)
        {
            ++jumpCount;
            if (p.slewed && started)
            {
                startSlew(lastOut - valueAt(p.target));
            }
            position = p.target;
        }
    }

    [[nodiscard]] float valueAt(std::uint64_t p) const noexcept
    {
        return row->wave.valueAt(static_cast<std::uint32_t>(p >> 32U) + row->start);
    }

    void startSlew(float from) noexcept
    {
        if (std::abs(from) < dsp::Lfo::smallest)
        {
            slewTotal = 0;
            slewCount = 0;
            return;
        }
        double length = static_cast<double>(slewSeconds) * sampleRate;
        const double cycles = std::ldexp(static_cast<double>(increment), -64);
        if (cycles > 0.0)
        {
            length = std::min(length, slewCycleFraction / cycles);
        }
        slewStart = from;
        slewTotal = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(length)));
        slewCount = 0;
    }

    [[nodiscard]] float slewTerm() noexcept
    {
        if (slewCount >= slewTotal)
        {
            return 0.0f;
        }
        const float term = slewStart * static_cast<float>(slewTotal - slewCount) / static_cast<float>(slewTotal);
        ++slewCount;
        return term;
    }

    double sampleRate = 48000.0;
    const dsp::LfoShape* row = &dsp::lfoShape(dsp::LfoWaveform::sine);
    std::uint64_t position = 0;
    std::uint64_t increment = 0;
    bool started = false;
    float lastOut = 0.0f;
    float slewStart = 0.0f;
    std::size_t slewTotal = 0;
    std::size_t slewCount = 0;
    std::size_t jumpCount = 0;
};

} // namespace anoesis::test::sync
