// Tests for Adsr (libs/dsp/include/anoesis/dsp/control/adsr.hpp): the ADSR
// envelope chosen in docs/dsp/envelopes.md. Exponential segments towards an
// overshoot target (attack ratio 0.3, decay and release 0.001), counted in
// float as a closed form, constant rate, retrigger from the current level,
// sustain changes ramped over 10 ms.
//
// Reference values are computed in double from the model's closed form. The
// tolerance of a segment is derived in segmentTolerance() from fastExp2's
// documented error bound and float rounding.

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/core/processor.hpp>
#include <anoesis/dsp/math/random.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

using anoesis::dsp::Adsr;
using anoesis::dsp::AdsrEvent;
using anoesis::dsp::AdsrGate;
using anoesis::dsp::AdsrStage;
using Catch::Matchers::WithinAbs;

namespace
{

constexpr std::array<float, 4> sampleRates{44100.0f, 48000.0f, 96000.0f, 192000.0f};
constexpr double attackRatio = 0.3;
constexpr double decayReleaseRatio = 0.001;

/// The worst absolute error of a counted segment's level against the model
/// in double. Per sample the level is target + d0 * fastExp2(log2c * k):
///
/// - fastExp2's relative error is below 5e-7 (fast_math.hpp);
/// - the exponent log2c * k carries three float roundings (log2c, the
///   count k above 2^24, the product), each at most 2^-24 of |log2 q| with
///   q = r / (1 + r), so the power is off by ln 2 * 3 * 2^-24 * |log2 q|;
/// - d0 is rounded once (2^-24), and |d0| <= 1 + r;
/// - the target and the final sum are rounded once each (2^-24 * (1 + r)).
double segmentTolerance(double ratio)
{
    const double u = std::ldexp(1.0, -24);
    const double exponent = std::numbers::ln2 * 3.0 * u * std::abs(std::log2(ratio / (1.0 + ratio)));
    const double magnitude = 1.0 + ratio;
    return (magnitude * (5e-7 + exponent + u)) + (2.0 * u * magnitude);
}

const double attackTolerance = segmentTolerance(attackRatio);
const double decayReleaseTolerance = segmentTolerance(decayReleaseRatio);

std::size_t samplesOf(double seconds, double rate)
{
    return static_cast<std::size_t>(std::lround(seconds * rate));
}

/// A segment of the model: the full-scale curve from `start` to `end` over
/// `samples`, entered at `from`.
struct Segment
{
    double start = 0.0;
    double end = 1.0;
    double samples = 1.0;
    double ratio = attackRatio;
    double from = 0.0;

    [[nodiscard]] double target() const
    {
        return end + (ratio * (end - start));
    }

    /// The level after k samples (k >= 1).
    [[nodiscard]] double at(double k) const
    {
        const double q = ratio / (1.0 + ratio);
        return target() + ((from - target()) * std::pow(q, k / samples));
    }

    /// The samples from `from` to the end, as the model counts them.
    [[nodiscard]] std::size_t length() const
    {
        const double q = ratio / (1.0 + ratio);
        const double remaining = samples * std::log((-ratio * (end - start)) / (from - target())) / std::log(q);
        // A level the segment cannot start from (an envelope under test that
        // is wrong) gives 1, so the checks fail rather than the test crash.
        if (!std::isfinite(remaining) || remaining < 1.0 || remaining > samples + 1.0)
        {
            return 1;
        }
        return static_cast<std::size_t>(std::lround(remaining));
    }
};

Segment attackFrom(double from, double seconds, double rate)
{
    return {
        .start = 0.0,
        .end = 1.0,
        .samples = static_cast<double>(samplesOf(seconds, rate)),
        .ratio = attackRatio,
        .from = from,
    };
}

Segment decayFrom(double from, double sustain, double seconds, double rate)
{
    return {
        .start = 1.0,
        .end = sustain,
        .samples = static_cast<double>(samplesOf(seconds, rate)),
        .ratio = decayReleaseRatio,
        .from = from,
    };
}

Segment releaseFrom(double from, double seconds, double rate)
{
    return {
        .start = 1.0,
        .end = 0.0,
        .samples = static_cast<double>(samplesOf(seconds, rate)),
        .ratio = decayReleaseRatio,
        .from = from,
    };
}

Adsr prepared(float rate, float attack, float decay, float sustain, float release)
{
    Adsr adsr;
    adsr.prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 1});
    adsr.setAttack(attack);
    adsr.setDecay(decay);
    adsr.setSustain(sustain);
    adsr.setRelease(release);
    return adsr;
}

std::vector<float> renderNext(Adsr& adsr, std::size_t numSamples)
{
    std::vector<float> out(numSamples);
    for (auto& y : out)
    {
        y = adsr.next();
    }
    return out;
}

bool bitIdentical(std::span<const float> a, std::span<const float> b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](float x, float y)
                      { return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y); });
}

bool same(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool isSubnormal(float x)
{
    return std::fpclassify(x) == FP_SUBNORMAL;
}

/// Checks every sample of `y` against a segment, from its first sample.
void checkSegment(std::span<const float> y, const Segment& s, double tolerance)
{
    for (std::size_t k = 0; k < y.size(); ++k)
    {
        const double expected = s.at(static_cast<double>(k + 1));
        if (std::abs(static_cast<double>(y[k]) - expected) > tolerance)
        {
            INFO("sample " << k + 1 << " of " << s.samples << ": " << y[k] << " against " << expected);
            CHECK_THAT(y[k], WithinAbs(expected, tolerance));
            return;
        }
    }
    SUCCEED();
}

// A script of gates and control changes at fixed samples, rendered in blocks
// of a given size: the gates go through process(out, events), the control
// changes are applied between sub-blocks at their sample.
enum class Control : std::uint8_t
{
    none,
    attack,
    decay,
    sustain,
    release,
};

struct ScriptEvent
{
    std::size_t at = 0;
    AdsrGate gate = AdsrGate::on;
    Control control = Control::none;
    float value = 0.0f;
};

void applyControl(Adsr& adsr, const ScriptEvent& e)
{
    switch (e.control)
    {
    case Control::attack:
        adsr.setAttack(e.value);
        break;
    case Control::decay:
        adsr.setDecay(e.value);
        break;
    case Control::sustain:
        adsr.setSustain(e.value);
        break;
    case Control::release:
        adsr.setRelease(e.value);
        break;
    case Control::none:
        break;
    }
}

std::vector<float> renderScript(Adsr& adsr, std::size_t numSamples, std::span<const ScriptEvent> script,
                                std::size_t blockSize)
{
    std::vector<float> out(numSamples);
    std::vector<AdsrEvent> gates;
    gates.reserve(script.size());
    std::size_t next = 0;
    for (std::size_t block = 0; block < numSamples; block += blockSize)
    {
        const auto blockEnd = std::min(block + blockSize, numSamples);
        std::size_t start = block;
        while (start < blockEnd)
        {
            // Apply control changes at `start`, then render up to the next
            // control change, passing the gates in between as events.
            while (next < script.size() && script[next].at == start && script[next].control != Control::none)
            {
                applyControl(adsr, script[next]);
                ++next;
            }
            std::size_t end = blockEnd;
            gates.clear();
            for (std::size_t i = next; i < script.size() && script[i].at < blockEnd; ++i)
            {
                if (script[i].control != Control::none)
                {
                    end = std::max(start + 1, script[i].at);
                    break;
                }
                gates.push_back({.offset = script[i].at - start, .gate = script[i].gate});
            }
            std::erase_if(gates, [&](const AdsrEvent& g) { return g.offset >= end - start; });
            next += gates.size();
            adsr.process(std::span{out}.subspan(start, end - start), gates);
            start = end;
        }
    }
    return out;
}

// The same script, one next() per sample, with apply() for the gates.
std::vector<float> renderScriptPerSample(Adsr& adsr, std::size_t numSamples, std::span<const ScriptEvent> script)
{
    std::vector<float> out(numSamples);
    std::size_t next = 0;
    for (std::size_t n = 0; n < numSamples; ++n)
    {
        while (next < script.size() && script[next].at == n)
        {
            if (script[next].control == Control::none)
            {
                adsr.apply(script[next].gate);
            }
            else
            {
                applyControl(adsr, script[next]);
            }
            ++next;
        }
        out[n] = adsr.next();
    }
    return out;
}

} // namespace

TEST_CASE("ADSR: follows the Preparable lifecycle", "[dsp][adsr]")
{
    STATIC_CHECK(anoesis::dsp::Preparable<Adsr>);

    const auto play = [](Adsr& adsr)
    {
        adsr.gateOn();
        auto y = renderNext(adsr, 3000);
        adsr.gateOff();
        const auto tail = renderNext(adsr, 3000);
        y.insert(y.end(), tail.begin(), tail.end());
        return y;
    };
    auto fresh = prepared(48000.0f, 0.01f, 0.02f, 0.5f, 0.03f);
    const auto expected = play(fresh);

    auto used = prepared(48000.0f, 0.01f, 0.02f, 0.5f, 0.03f);
    used.gateOn();
    static_cast<void>(renderNext(used, 700));
    used.reset();
    CHECK(used.stage() == AdsrStage::idle);
    CHECK_FALSE(used.isActive());
    CHECK(bitIdentical(play(used), expected));

    auto rePrepared = prepared(48000.0f, 0.01f, 0.02f, 0.5f, 0.03f);
    rePrepared.gateOn();
    static_cast<void>(renderNext(rePrepared, 700));
    rePrepared.prepare({.sampleRate = 48000.0f, .maxBlockSize = 4096, .numChannels = 1});
    CHECK(bitIdentical(play(rePrepared), expected));
}

TEST_CASE("ADSR: defaults are a plain gate, and it is idle until a gate", "[dsp][adsr]")
{
    Adsr adsr;
    adsr.prepare({.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 1});
    CHECK_THAT(adsr.attackSeconds(), WithinAbs(0.0, 0.0));
    CHECK_THAT(adsr.decaySeconds(), WithinAbs(0.0, 0.0));
    CHECK_THAT(adsr.sustainLevel(), WithinAbs(1.0, 0.0));
    CHECK_THAT(adsr.releaseSeconds(), WithinAbs(0.0, 0.0));
    CHECK(adsr.stage() == AdsrStage::idle);
    CHECK_FALSE(adsr.isActive());
    CHECK_FALSE(adsr.isGateOn());
    CHECK(bitIdentical(renderNext(adsr, 64), std::vector<float>(64, 0.0f)));

    adsr.gateOn();
    CHECK(adsr.isGateOn());
    CHECK(adsr.isActive());
    CHECK_THAT(adsr.next(), WithinAbs(1.0, 0.0));
    adsr.gateOff();
    CHECK_FALSE(adsr.isGateOn());
    CHECK_THAT(adsr.next(), WithinAbs(0.0, 0.0));
    CHECK_FALSE(adsr.isActive());
}

TEST_CASE("ADSR: every stage lasts exactly its time, at every rate", "[dsp][adsr]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    const auto seconds = GENERATE(0.001f, 0.1f, 1.0f);
    const auto rateD = static_cast<double>(rate);
    const auto n = samplesOf(static_cast<double>(seconds), rateD);
    INFO("rate " << rate << ", " << seconds << " s = " << n << " samples");

    // Attack: reaches 1 exactly on its n-th sample, and not before.
    auto attack = prepared(rate, seconds, 1.0f, 0.25f, 1.0f);
    attack.gateOn();
    const auto a = renderNext(attack, n);
    CHECK(std::none_of(a.begin(), a.end() - 1, [](float y) { return y >= 1.0f; }));
    CHECK_THAT(a.back(), WithinAbs(1.0, 0.0));
    CHECK(attack.stage() == AdsrStage::decay);

    // Decay: from the peak to the sustain level exactly on its n-th sample.
    auto decay = prepared(rate, 0.0f, seconds, 0.25f, 1.0f);
    decay.gateOn();
    const auto d = renderNext(decay, n);
    CHECK(std::none_of(d.begin(), d.end() - 1, [](float y) { return y <= 0.25f; }));
    CHECK_THAT(d.back(), WithinAbs(0.25, 0.0));
    CHECK(decay.stage() == AdsrStage::sustain);

    // Release from 1: exactly 0 on its n-th sample, then idle.
    auto release = prepared(rate, 0.0f, 0.0f, 1.0f, seconds);
    release.gateOn();
    static_cast<void>(release.next());
    release.gateOff();
    const auto r = renderNext(release, n - 1);
    CHECK(release.isActive());
    CHECK(std::ranges::none_of(r, [](float y) { return y <= 0.0f; }));
    CHECK_THAT(release.next(), WithinAbs(0.0, 0.0));
    CHECK_FALSE(release.isActive());
    CHECK(release.stage() == AdsrStage::idle);
}

TEST_CASE("ADSR: long stages are exact in float, at 192 and 768 kHz", "[dsp][adsr]")
{
    // 25 s at 192 kHz is 4.8 million samples; at 768 kHz 19.2 million, past
    // 2^24, where the count is no longer exact in float. Streamed, every
    // 97th sample and the last 1000 checked against the model.
    const auto [rate, seconds] =
        GENERATE(std::pair{44100.0f, 25.0f}, std::pair{192000.0f, 25.0f}, std::pair{768000.0f, 25.0f});
    const auto rateD = static_cast<double>(rate);
    const auto n = samplesOf(static_cast<double>(seconds), rateD);
    INFO("rate " << rate << ", " << seconds << " s");

    const auto stream = [&](Adsr& adsr, const Segment& expected, double tolerance, float endLevel)
    {
        double worst = 0.0;
        for (std::size_t k = 1; k < n; ++k)
        {
            const float y = adsr.next();
            if (k % 97 == 0 || k + 1000 >= n)
            {
                worst = std::max(worst, std::abs(static_cast<double>(y) - expected.at(static_cast<double>(k))));
            }
        }
        CHECK(worst <= tolerance);
        CHECK(same(adsr.next(), endLevel));
    };

    auto attack = prepared(rate, seconds, 1.0f, 1.0f, 1.0f);
    attack.gateOn();
    stream(attack, attackFrom(0.0, static_cast<double>(seconds), rateD), attackTolerance, 1.0f);

    auto release = prepared(rate, 0.0f, 0.0f, 1.0f, seconds);
    release.gateOn();
    static_cast<void>(release.next());
    release.gateOff();
    stream(release, releaseFrom(1.0, static_cast<double>(seconds), rateD), decayReleaseTolerance, 0.0f);
    CHECK_FALSE(release.isActive());
}

TEST_CASE("ADSR: the stages follow the model's curves", "[dsp][adsr]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    const auto rateD = static_cast<double>(rate);
    const auto times = GENERATE(std::array{0.002f, 0.05f, 0.5f}, std::array{0.3f, 0.001f, 0.2f});
    const auto [attackTime, decayTime, releaseTime] = times;
    constexpr float sustain = 0.4f;
    INFO("rate " << rate << ", A " << attackTime << ", D " << decayTime << ", R " << releaseTime);

    auto adsr = prepared(rate, attackTime, decayTime, sustain, releaseTime);
    adsr.gateOn();
    const auto nA = samplesOf(static_cast<double>(attackTime), rateD);
    const auto nD = samplesOf(static_cast<double>(decayTime), rateD);
    const auto nR = samplesOf(static_cast<double>(releaseTime), rateD);
    const auto a = renderNext(adsr, nA);
    const auto d = renderNext(adsr, nD);
    const auto held = renderNext(adsr, 100);
    adsr.gateOff();
    const auto r = renderNext(adsr, nR);

    checkSegment(std::span{a}.first(nA - 1), attackFrom(0.0, static_cast<double>(attackTime), rateD), attackTolerance);
    checkSegment(std::span{d}.first(nD - 1),
                 decayFrom(1.0, static_cast<double>(sustain), static_cast<double>(decayTime), rateD),
                 decayReleaseTolerance);
    CHECK(bitIdentical(held, std::vector<float>(100, sustain)));
    // The release from the sustain level enters the full-scale release
    // curve where it passes 0.4: constant rate.
    const auto release = releaseFrom(static_cast<double>(sustain), static_cast<double>(releaseTime), rateD);
    CHECK(release.length() < nR);
    checkSegment(std::span{r}.first(release.length() - 1), release, decayReleaseTolerance);
    CHECK_THAT(r[release.length() - 1], WithinAbs(0.0, 0.0));
    CHECK_FALSE(adsr.isActive());

    // The shape, as numbers: the attack at half its time, the release in dB.
    if (nA >= 1000)
    {
        CHECK_THAT(a[(nA / 2) - 1], WithinAbs(1.3 * (1.0 - std::sqrt(0.3 / 1.3)), 2.0 / static_cast<double>(nA)));
    }
}

TEST_CASE("ADSR: a release from part-way runs at the release's rate", "[dsp][adsr]")
{
    const auto from = GENERATE(0.9f, 0.5f, 0.1f, 0.001f);
    constexpr float rate = 48000.0f;
    constexpr float seconds = 1.0f;
    INFO("release from " << from);

    auto adsr = prepared(rate, 0.0f, 0.0f, from, seconds);
    adsr.gateOn();
    static_cast<void>(adsr.next());
    adsr.gateOff();
    const auto expected = releaseFrom(static_cast<double>(from), static_cast<double>(seconds), 48000.0);
    const auto r = renderNext(adsr, expected.length());
    checkSegment(std::span{r}.first(expected.length() - 1), expected, decayReleaseTolerance);
    CHECK_THAT(r.back(), WithinAbs(0.0, 0.0));
    CHECK_FALSE(adsr.isActive());
    // At 0.5 the near-exponential release takes 90% of its time.
    if (from > 0.4f && from < 0.6f)
    {
        CHECK_THAT(static_cast<double>(r.size()) / 48000.0, WithinAbs(0.900, 0.001));
    }
}

TEST_CASE("ADSR: a retrigger from any stage continues from the current level", "[dsp][adsr]")
{
    constexpr float rate = 48000.0f;
    constexpr double rateD = 48000.0;
    constexpr float attack = 0.01f;
    constexpr float decay = 0.05f;
    constexpr float sustain = 0.5f;
    constexpr float release = 0.1f;
    // Where the retrigger comes, in samples from the first gate: mid-attack,
    // mid-decay, sustain, mid-release (released at 6000), and idle.
    const auto [label, at, releasedAt] = GENERATE(table<const char*, std::size_t, std::size_t>({
        {"attack", 200, 0},
        {"decay", 1000, 0},
        {"sustain", 5000, 0},
        {"release", 7000, 6000},
        {"idle", 20000, 6000},
    }));
    INFO("retrigger in " << label);

    auto adsr = prepared(rate, attack, decay, sustain, release);
    adsr.gateOn();
    if (releasedAt > 0)
    {
        static_cast<void>(renderNext(adsr, releasedAt));
        adsr.gateOff();
        static_cast<void>(renderNext(adsr, at - releasedAt));
    }
    else
    {
        static_cast<void>(renderNext(adsr, at));
    }
    const float level = adsr.level();
    adsr.gateOn();
    CHECK(adsr.stage() == AdsrStage::attack);
    const auto expected = attackFrom(static_cast<double>(level), static_cast<double>(attack), rateD);
    const auto y = renderNext(adsr, expected.length());
    checkSegment(std::span{y}.first(expected.length() - 1), expected, attackTolerance);
    CHECK_THAT(y.back(), WithinAbs(1.0, 0.0));
    // From 0 (idle) it is the whole attack.
    if (level <= 0.0f)
    {
        CHECK(expected.length() == samplesOf(static_cast<double>(attack), rateD));
    }
}

TEST_CASE("ADSR: a note-off from any held stage releases from the current level", "[dsp][adsr]")
{
    constexpr float rate = 48000.0f;
    constexpr double rateD = 48000.0;
    constexpr float release = 0.1f;
    const auto [label, at] = GENERATE(table<const char*, std::size_t>({
        {"attack", 200},
        {"decay", 1000},
        {"sustain", 5000},
    }));
    INFO("note-off in " << label);

    auto adsr = prepared(rate, 0.01f, 0.05f, 0.5f, release);
    adsr.gateOn();
    static_cast<void>(renderNext(adsr, at));
    const float level = adsr.level();
    adsr.gateOff();
    CHECK(adsr.stage() == AdsrStage::release);
    const auto expected = releaseFrom(static_cast<double>(level), static_cast<double>(release), rateD);
    const auto y = renderNext(adsr, expected.length());
    checkSegment(std::span{y}.first(expected.length() - 1), expected, decayReleaseTolerance);
    CHECK_THAT(y.back(), WithinAbs(0.0, 0.0));
    CHECK_FALSE(adsr.isActive());
}

TEST_CASE("ADSR: a legato gate leaves a held note alone, and a note-off when released does nothing", "[dsp][adsr]")
{
    constexpr float rate = 48000.0f;
    const auto at = GENERATE(std::size_t{200}, std::size_t{1000}, std::size_t{5000});
    INFO("legato gate at sample " << at);

    auto plain = prepared(rate, 0.01f, 0.05f, 0.5f, 0.1f);
    plain.gateOn();
    const auto expected = renderNext(plain, 8000);

    auto legato = prepared(rate, 0.01f, 0.05f, 0.5f, 0.1f);
    legato.gateOn();
    auto y = renderNext(legato, at);
    legato.legatoGateOn();
    const auto rest = renderNext(legato, 8000 - at);
    y.insert(y.end(), rest.begin(), rest.end());
    CHECK(bitIdentical(y, expected));

    // Legato from release or idle is an ordinary gate-on.
    auto a = prepared(rate, 0.01f, 0.05f, 0.5f, 0.1f);
    auto b = prepared(rate, 0.01f, 0.05f, 0.5f, 0.1f);
    for (Adsr* adsr : {&a, &b})
    {
        adsr->gateOn();
        static_cast<void>(renderNext(*adsr, 3000));
        adsr->gateOff();
        static_cast<void>(renderNext(*adsr, at));
    }
    a.gateOn();
    b.legatoGateOn();
    CHECK(bitIdentical(renderNext(a, 2000), renderNext(b, 2000)));

    // A note-off in release or idle changes nothing.
    auto c = prepared(rate, 0.01f, 0.05f, 0.5f, 0.1f);
    auto e = prepared(rate, 0.01f, 0.05f, 0.5f, 0.1f);
    for (Adsr* adsr : {&c, &e})
    {
        adsr->gateOn();
        static_cast<void>(renderNext(*adsr, 3000));
        adsr->gateOff();
        static_cast<void>(renderNext(*adsr, 1000));
    }
    e.gateOff();
    CHECK(bitIdentical(renderNext(c, 8000), renderNext(e, 8000)));
    e.gateOff();
    CHECK(e.stage() == AdsrStage::idle);
}

TEST_CASE("ADSR: stages of 0 are instant", "[dsp][adsr]")
{
    constexpr float rate = 48000.0f;
    constexpr double rateD = 48000.0;

    SECTION("attack 0: the gate's own sample is the decay's first")
    {
        auto adsr = prepared(rate, 0.0f, 0.1f, 0.5f, 0.1f);
        adsr.gateOn();
        CHECK(adsr.stage() == AdsrStage::decay);
        CHECK_THAT(adsr.next(), WithinAbs(decayFrom(1.0, 0.5, 0.1, rateD).at(1.0), decayReleaseTolerance));
    }
    SECTION("attack and decay 0: the gate's own sample is the sustain level")
    {
        auto adsr = prepared(rate, 0.0f, 0.0f, 0.5f, 0.1f);
        adsr.gateOn();
        CHECK(adsr.stage() == AdsrStage::sustain);
        CHECK_THAT(adsr.next(), WithinAbs(0.5, 0.0));
    }
    SECTION("decay 0: the sample after the peak is the sustain level")
    {
        auto adsr = prepared(rate, 0.01f, 0.0f, 0.5f, 0.1f);
        adsr.gateOn();
        const auto y = renderNext(adsr, 481);
        CHECK_THAT(y[479], WithinAbs(1.0, 0.0));
        CHECK_THAT(y[480], WithinAbs(0.5, 0.0));
    }
    SECTION("release 0: the note-off's own sample is 0, and the envelope is idle")
    {
        auto adsr = prepared(rate, 0.01f, 0.1f, 0.5f, 0.0f);
        adsr.gateOn();
        static_cast<void>(renderNext(adsr, 100));
        adsr.gateOff();
        CHECK_FALSE(adsr.isActive());
        CHECK_THAT(adsr.next(), WithinAbs(0.0, 0.0));
    }
    SECTION("times below half a sample are 0")
    {
        auto adsr = prepared(rate, 0.4f / rate, 0.0f, 1.0f, 0.0f);
        adsr.gateOn();
        CHECK_THAT(adsr.next(), WithinAbs(1.0, 0.0));
    }
}

TEST_CASE("ADSR: controls clamp, and ignore NaN", "[dsp][adsr]")
{
    Adsr adsr;
    adsr.prepare({.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 1});
    adsr.setAttack(-1.0f);
    adsr.setDecay(1e9f);
    adsr.setSustain(2.0f);
    adsr.setRelease(std::numeric_limits<float>::infinity());
    CHECK_THAT(adsr.attackSeconds(), WithinAbs(0.0, 0.0));
    CHECK_THAT(adsr.decaySeconds(), WithinAbs(static_cast<double>(Adsr::maxStageSeconds), 0.0));
    CHECK_THAT(adsr.sustainLevel(), WithinAbs(1.0, 0.0));
    CHECK_THAT(adsr.releaseSeconds(), WithinAbs(static_cast<double>(Adsr::maxStageSeconds), 0.0));
    adsr.setSustain(-1.0f);
    CHECK_THAT(adsr.sustainLevel(), WithinAbs(0.0, 0.0));

    adsr.setAttack(0.5f);
    adsr.setDecay(0.25f);
    adsr.setSustain(0.75f);
    adsr.setRelease(0.125f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    adsr.setAttack(nan);
    adsr.setDecay(nan);
    adsr.setSustain(nan);
    adsr.setRelease(nan);
    CHECK_THAT(adsr.attackSeconds(), WithinAbs(0.5, 0.0));
    CHECK_THAT(adsr.decaySeconds(), WithinAbs(0.25, 0.0));
    CHECK_THAT(adsr.sustainLevel(), WithinAbs(0.75, 0.0));
    CHECK_THAT(adsr.releaseSeconds(), WithinAbs(0.125, 0.0));

    // A subnormal sustain is 0.
    adsr.setSustain(1e-40f);
    CHECK_THAT(adsr.sustainLevel(), WithinAbs(0.0, 0.0));
}

TEST_CASE("ADSR: gates are sample-accurate within a block", "[dsp][adsr]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    const auto offset = GENERATE(std::size_t{0}, std::size_t{1}, std::size_t{200}, std::size_t{511});
    INFO("rate " << rate << ", offset " << offset);
    constexpr std::size_t block = 512;

    // A gate-on at `offset` is the same note as a gate-on at 0, delayed.
    auto reference = prepared(rate, 0.001f, 0.002f, 0.5f, 0.001f);
    reference.gateOn();
    const auto expected = renderNext(reference, 2 * block);

    auto adsr = prepared(rate, 0.001f, 0.002f, 0.5f, 0.001f);
    std::vector<float> y(2 * block, -1.0f);
    const std::array on{AdsrEvent{.offset = offset, .gate = AdsrGate::on}};
    adsr.process(std::span{y}.first(block), on);
    adsr.process(std::span{y}.subspan(block), {});
    CHECK(
        std::all_of(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(offset), [](float v) { return same(v, 0.0f); }));
    CHECK(bitIdentical(std::span{y}.subspan(offset), std::span{expected}.first((2 * block) - offset)));

    // A gate-off at `offset` takes effect on that sample.
    auto off = prepared(rate, 0.001f, 0.002f, 0.5f, 0.001f);
    off.gateOn();
    static_cast<void>(renderNext(off, 1000));
    auto offReference = off;
    offReference.gateOff();
    const auto expectedOff = renderNext(offReference, block);
    std::vector<float> z(block);
    const std::array gateOff{AdsrEvent{.offset = offset, .gate = AdsrGate::off}};
    auto held = off;
    const auto expectedHeld = renderNext(held, offset);
    off.process(z, gateOff);
    CHECK(bitIdentical(std::span{z}.first(offset), expectedHeld));
    CHECK(bitIdentical(std::span{z}.subspan(offset), std::span{expectedOff}.first(block - offset)));
}

TEST_CASE("ADSR: the output does not depend on the block size", "[dsp][adsr]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    INFO("rate " << rate);
    const std::array script{
        ScriptEvent{.at = 0, .control = Control::attack, .value = 0.01f},
        ScriptEvent{.at = 0, .control = Control::decay, .value = 0.05f},
        ScriptEvent{.at = 0, .control = Control::sustain, .value = 0.6f},
        ScriptEvent{.at = 0, .control = Control::release, .value = 0.03f},
        ScriptEvent{.at = 100, .gate = AdsrGate::on},
        ScriptEvent{.at = 900, .control = Control::attack, .value = 0.002f},
        ScriptEvent{.at = 1500, .gate = AdsrGate::legatoOn},
        ScriptEvent{.at = 2000, .control = Control::decay, .value = 0.2f},
        ScriptEvent{.at = 3000, .gate = AdsrGate::on},
        ScriptEvent{.at = 6000, .control = Control::sustain, .value = 0.2f},
        ScriptEvent{.at = 9000, .control = Control::sustain, .value = 0.9f},
        ScriptEvent{.at = 12000, .gate = AdsrGate::off},
        ScriptEvent{.at = 12500, .control = Control::release, .value = 0.5f},
        ScriptEvent{.at = 13000, .gate = AdsrGate::legatoOn},
        ScriptEvent{.at = 16000, .gate = AdsrGate::off},
        ScriptEvent{.at = 16001, .gate = AdsrGate::on},
        ScriptEvent{.at = 16001, .gate = AdsrGate::off},
        ScriptEvent{.at = 40000, .gate = AdsrGate::on},
    };
    constexpr std::size_t length = 50000;
    auto perSample = prepared(rate, 0.0f, 0.0f, 1.0f, 0.0f);
    const auto expected = renderScriptPerSample(perSample, length, script);
    for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
    {
        INFO("block " << block);
        auto adsr = prepared(rate, 0.0f, 0.0f, 1.0f, 0.0f);
        CHECK(bitIdentical(renderScript(adsr, length, script, block), expected));
    }
}

TEST_CASE("ADSR: a time changed mid-stage applies at once, at the new rate", "[dsp][adsr]")
{
    constexpr float rate = 48000.0f;
    constexpr double rateD = 48000.0;

    SECTION("attack")
    {
        auto adsr = prepared(rate, 0.1f, 1.0f, 0.5f, 1.0f);
        adsr.gateOn();
        static_cast<void>(renderNext(adsr, 2400));
        const float level = adsr.level();
        adsr.setAttack(0.01f);
        const auto expected = attackFrom(static_cast<double>(level), 0.01, rateD);
        const auto y = renderNext(adsr, expected.length());
        checkSegment(std::span{y}.first(expected.length() - 1), expected, attackTolerance);
        CHECK_THAT(y.back(), WithinAbs(1.0, 0.0));
        CHECK(adsr.stage() == AdsrStage::decay);
        // Constant rate: half-way up the 100 ms curve, 10 ms leaves 5 ms.
        CHECK(expected.length() == 240);
    }
    SECTION("decay")
    {
        auto adsr = prepared(rate, 0.0f, 1.0f, 0.25f, 1.0f);
        adsr.gateOn();
        static_cast<void>(renderNext(adsr, 4800));
        const float level = adsr.level();
        adsr.setDecay(0.1f);
        const auto expected = decayFrom(static_cast<double>(level), 0.25, 0.1, rateD);
        const auto y = renderNext(adsr, expected.length());
        checkSegment(std::span{y}.first(expected.length() - 1), expected, decayReleaseTolerance);
        CHECK_THAT(y.back(), WithinAbs(0.25, 0.0));
        CHECK(adsr.stage() == AdsrStage::sustain);
    }
    SECTION("release")
    {
        auto adsr = prepared(rate, 0.0f, 0.0f, 1.0f, 1.0f);
        adsr.gateOn();
        static_cast<void>(adsr.next());
        adsr.gateOff();
        static_cast<void>(renderNext(adsr, 4800));
        const float level = adsr.level();
        adsr.setRelease(0.1f);
        const auto expected = releaseFrom(static_cast<double>(level), 0.1, rateD);
        const auto y = renderNext(adsr, expected.length());
        checkSegment(std::span{y}.first(expected.length() - 1), expected, decayReleaseTolerance);
        CHECK_THAT(y.back(), WithinAbs(0.0, 0.0));
        CHECK_FALSE(adsr.isActive());
        CHECK(expected.length() == 4320); // 90 ms of the new 100 ms release
    }
    SECTION("a time of 0 mid-stage ends the stage at once")
    {
        auto adsr = prepared(rate, 0.1f, 0.1f, 0.5f, 0.1f);
        adsr.gateOn();
        static_cast<void>(renderNext(adsr, 100));
        adsr.setAttack(0.0f);
        CHECK(adsr.stage() == AdsrStage::decay);
        adsr.setDecay(0.0f);
        CHECK(adsr.stage() == AdsrStage::sustain);
        CHECK_THAT(adsr.next(), WithinAbs(0.5, 0.0));
    }
}

TEST_CASE("ADSR: a sustain change ramps over 10 ms, or re-aims the decay", "[dsp][adsr]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    const auto rateD = static_cast<double>(rate);
    const auto ramp = samplesOf(static_cast<double>(Adsr::sustainRampSeconds), rateD);
    INFO("rate " << rate);

    SECTION("in the sustain stage: a straight line, landing exactly")
    {
        const auto [from, to] = GENERATE(std::pair{0.5f, 0.25f}, std::pair{0.2f, 1.0f}, std::pair{1.0f, 0.0f});
        auto adsr = prepared(rate, 0.0f, 0.0f, from, 0.1f);
        adsr.gateOn();
        static_cast<void>(renderNext(adsr, 10));
        adsr.setSustain(to);
        const auto y = renderNext(adsr, ramp + 10);
        // The LinearSmoother computes start + step * k in float.
        const double tolerance = 4.0 * std::ldexp(1.0, -24);
        for (std::size_t k = 0; k + 1 < ramp; ++k)
        {
            const double expected =
                static_cast<double>(from) +
                (static_cast<double>(to - from) * static_cast<double>(k + 1) / static_cast<double>(ramp));
            REQUIRE_THAT(y[k], WithinAbs(expected, tolerance));
        }
        CHECK(std::all_of(y.begin() + static_cast<std::ptrdiff_t>(ramp) - 1, y.end(),
                          [&](float v) { return same(v, to); }));
        CHECK(adsr.stage() == AdsrStage::sustain);
    }
    SECTION("in the decay stage, below the level: the decay re-aims")
    {
        auto adsr = prepared(rate, 0.0f, 0.2f, 0.5f, 0.1f);
        adsr.gateOn();
        static_cast<void>(renderNext(adsr, samplesOf(0.02, rateD)));
        const float level = adsr.level();
        adsr.setSustain(0.1f);
        const auto expected = decayFrom(static_cast<double>(level), 0.1, 0.2, rateD);
        const auto y = renderNext(adsr, expected.length());
        checkSegment(std::span{y}.first(expected.length() - 1), expected, decayReleaseTolerance);
        CHECK(same(y.back(), 0.1f));
    }
    SECTION("in the decay stage, above the level: a ramp up to it")
    {
        auto adsr = prepared(rate, 0.0f, 0.2f, 0.1f, 0.1f);
        adsr.gateOn();
        static_cast<void>(renderNext(adsr, samplesOf(0.05, rateD)));
        const float level = adsr.level();
        REQUIRE(level < 0.5f);
        adsr.setSustain(0.5f);
        CHECK(adsr.stage() == AdsrStage::sustain);
        const auto y = renderNext(adsr, ramp);
        CHECK_THAT(y[(ramp / 2) - 1],
                   WithinAbs(static_cast<double>(level) + (static_cast<double>(0.5f - level) * 0.5), 1e-3));
        CHECK_THAT(y.back(), WithinAbs(0.5, 0.0));
    }
    SECTION("in the attack: the decay goes to the new level")
    {
        auto adsr = prepared(rate, 0.01f, 0.01f, 0.5f, 0.1f);
        adsr.gateOn();
        static_cast<void>(renderNext(adsr, 10));
        adsr.setSustain(0.3f);
        CHECK(adsr.stage() == AdsrStage::attack);
        static_cast<void>(renderNext(adsr, samplesOf(0.02, rateD)));
        CHECK(same(adsr.level(), 0.3f));
    }
}

TEST_CASE("ADSR: no subnormal levels, at any rate or sustain", "[dsp][adsr]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    const auto sustain = GENERATE(0.0f, 1e-30f, 1e-6f, 0.5f, 1.0f);
    INFO("rate " << rate << ", sustain " << sustain);
    auto adsr = prepared(rate, 0.001f, 0.01f, sustain, 1.0f);
    adsr.gateOn();
    const auto held = renderNext(adsr, static_cast<std::size_t>(rate));
    adsr.gateOff();
    const auto released = renderNext(adsr, static_cast<std::size_t>(rate) + 10);
    CHECK(std::ranges::none_of(held, isSubnormal));
    CHECK(std::ranges::none_of(released, isSubnormal));
    CHECK_FALSE(adsr.isActive());
}

TEST_CASE("ADSR: no allocation in any control, gate, or process call", "[dsp][adsr]")
{
    Adsr adsr;
    adsr.prepare({.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 1});
    std::array<float, 512> out{};
    const std::array events{
        AdsrEvent{.offset = 3, .gate = AdsrGate::on},
        AdsrEvent{.offset = 100, .gate = AdsrGate::legatoOn},
        AdsrEvent{.offset = 400, .gate = AdsrGate::off},
    };
    std::size_t count = 0;
    {
        const anoesis::test::AllocationCounter counter;
        adsr.setAttack(0.01f);
        adsr.setDecay(0.02f);
        adsr.setSustain(0.5f);
        adsr.setRelease(0.03f);
        adsr.gateOn();
        static_cast<void>(adsr.next());
        adsr.setSustain(0.25f);
        adsr.legatoGateOn();
        adsr.process(out);
        adsr.gateOff();
        adsr.process(out, events);
        adsr.apply(AdsrGate::off);
        adsr.reset();
        count = counter.count();
    }
    CHECK(count == 0);
}

TEST_CASE("ADSR: stable under random gates, controls, and block sizes", "[dsp][adsr]")
{
    const auto rate = GENERATE(from_range(sampleRates));
    INFO("rate " << rate);
    anoesis::dsp::Random random{static_cast<std::uint64_t>(rate)};
    const auto pick = [&](std::uint32_t n)
    {
        return random.nextUint32() % n;
    };
    const auto time = [&]
    {
        switch (pick(5))
        {
        case 0:
            return 0.0f;
        case 1:
            return 1.0f / rate; // one sample
        case 2:
            return std::numeric_limits<float>::quiet_NaN();
        default:
            return 0.5f * random.nextUnipolar() * random.nextUnipolar();
        }
    };

    std::vector<ScriptEvent> script;
    constexpr std::size_t length = 400000;
    for (std::size_t at = 0; at < length; at += 1 + pick(3000))
    {
        switch (pick(8))
        {
        case 0:
            script.push_back({.at = at, .gate = AdsrGate::on});
            break;
        case 1:
            script.push_back({.at = at, .gate = AdsrGate::legatoOn});
            break;
        case 2:
        case 3:
            script.push_back({.at = at, .gate = AdsrGate::off});
            break;
        case 4:
            script.push_back({.at = at, .control = Control::attack, .value = time()});
            break;
        case 5:
            script.push_back({.at = at, .control = Control::decay, .value = time()});
            break;
        case 6:
            script.push_back(
                {.at = at, .control = Control::sustain, .value = pick(4) == 0 ? 0.0f : random.nextUnipolar()});
            break;
        default:
            script.push_back({.at = at, .control = Control::release, .value = time()});
            break;
        }
    }

    auto perSample = prepared(rate, 0.01f, 0.1f, 0.5f, 0.1f);
    const auto expected = renderScriptPerSample(perSample, length, script);
    CHECK(std::ranges::all_of(expected, [](float y) { return std::isfinite(y) && y >= 0.0f && y <= 1.0f; }));
    CHECK(std::ranges::none_of(expected, isSubnormal));

    // Any block size gives the same output.
    for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{4096}})
    {
        auto adsr = prepared(rate, 0.01f, 0.1f, 0.5f, 0.1f);
        INFO("block " << block);
        CHECK(bitIdentical(renderScript(adsr, length, script, block), expected));
    }
}
