// Tests for PitchDrift (docs/dsp/oscillator-drift.md): the drift's
// statistics (depth, rate, spectrum, distribution), drift off leaving every
// note bit for bit as it was, reproducibility and independent voices,
// note-ons, the hold on its own grid and independence of the block size,
// nothing accumulating over hours, no subnormals, no allocation, and the
// drift applied to oscillator slots: a voice's oscillators drift together,
// Noise is unaffected, and nothing is added beyond the pitch path itself.

#include "../support/allocation_counter.hpp"
#include "range_support.hpp"

#include <anoesis/dsp/oscillators/pitch_drift.hpp>
#include <anoesis/dsp/resampling/oversampler.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;
using namespace anoesis::test::range;
using anoesis::dsp::PitchDrift;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

namespace
{

constexpr std::size_t interval = PitchDrift::updateInterval;

PitchDrift preparedDrift(double sampleRate, float cents, float hz, std::uint64_t seed = 7)
{
    PitchDrift drift{seed};
    drift.prepare({.sampleRate = static_cast<float>(sampleRate), .maxBlockSize = 512, .numChannels = 1});
    drift.setDepthCents(cents);
    drift.setRateHz(hz);
    drift.noteOn();
    return drift;
}

// One value per run of constant offset, in cents, over count x 64 samples:
// one per update when the grid starts aligned (after a note-on from
// silence). No assertion inside: process() is noexcept.
std::vector<double> updates(PitchDrift& drift, std::size_t count)
{
    std::vector<double> cents;
    cents.reserve(count + 1);
    drift.process(count * interval, [&](std::size_t /*start*/, std::size_t /*length*/, float semitones)
                  { cents.push_back(100.0 * semitones); });
    return cents;
}

double meanOf(std::span<const double> x)
{
    double sum = 0.0;
    for (const double v : x)
    {
        sum += v;
    }
    return sum / static_cast<double>(x.size());
}

double sigmaOf(std::span<const double> x)
{
    const double mean = meanOf(x);
    double sum = 0.0;
    for (const double v : x)
    {
        sum += (v - mean) * (v - mean);
    }
    return std::sqrt(sum / static_cast<double>(x.size()));
}

double autocorrelation(std::span<const double> x, std::size_t lag)
{
    double sum = 0.0;
    double power = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        power += x[i] * x[i];
        if (i + lag < x.size())
        {
            sum += x[i] * x[i + lag];
        }
    }
    return (sum / static_cast<double>(x.size() - lag)) / (power / static_cast<double>(x.size()));
}

double updateHz(double sampleRate)
{
    return sampleRate / static_cast<double>(interval);
}

} // namespace

TEST_CASE("Pitch drift: off by default, and off leaves every note exactly as it was", "[drift]")
{
    PitchDrift drift{3};
    drift.prepare({.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 1});
    CHECK(drift.depthCents() <= 0.0f);
    drift.noteOn();

    // A note per sample, untouched bit for bit.
    std::vector<float> notes(5000);
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        notes[i] = -120.0f + (0.05f * static_cast<float>(i));
    }
    auto drifted = notes;
    drift.addTo(drifted);
    CHECK(bitIdentical(drifted, notes));

    // One run of the whole length at exactly +0.
    std::size_t runs = 0;
    drift.process(4096,
                  [&](std::size_t start, std::size_t length, float semitones)
                  {
                      ++runs;
                      CHECK(start == 0);
                      CHECK(length == 4096);
                      CHECK(sameBits(semitones, 0.0f));
                  });
    CHECK(runs == 1);

    // Turned on, then off again: exactly 0 once more.
    drift.setDepthCents(1.5f);
    drift.advance(interval * 50);
    CHECK(std::abs(drift.offset()) > 0.0f);
    drift.setDepthCents(0.0f);
    CHECK(sameBits(drift.offset(), 0.0f));
}

TEST_CASE("Pitch drift: a slot with drift off renders bit for bit as without it", "[drift]")
{
    for (const auto shape : allWaveforms)
    {
        CAPTURE(shapeName(shape));
        auto plain = preparedSlot(shape, 5, 48000.0);
        auto withDrift = preparedSlot(shape, 5, 48000.0);
        for (auto* slot : {&plain, &withDrift})
        {
            slot->setDetune(0.4f);
            slot->setStereo(0.7f);
            slot->noteOn();
        }
        auto drift = preparedDrift(48000.0, 0.0f, PitchDrift::defaultRateHz);
        Buffer a{2, 3000};
        Buffer b{2, 3000};
        for (std::size_t start = 0; start < 3000; start += 500)
        {
            plain.process(a.block().subBlock(start, 500), 60.0f);
            drift.process(500, [&](std::size_t from, std::size_t length, float semitones)
                          { withDrift.process(b.block().subBlock(start + from, length), 60.0f + semitones); });
        }
        CHECK(bitIdentical(a, b));
    }
}

TEST_CASE("Pitch drift: its standard deviation is the depth, at every rate", "[drift]")
{
    // One hour of updates. For two-pole noise with corner w = 2 pi f_c, the
    // estimate of the variance over T seconds has a relative spread of
    // sqrt(5 / (w T)) (its autocorrelation is (1 + w t) e^(-w t)), so the
    // estimated sigma spreads by half that: 1.1% at 0.5 Hz, 0.5% at 2 Hz.
    // 5% is more than 4 of those. The mean's spread is sigma sqrt(4 / (w T)),
    // 0.019 sigma at 0.5 Hz; 0.1 sigma is 5 of them.
    for (const double sampleRate : rates)
    {
        for (const float depth : {1.5f, 10.0f})
        {
            for (const float rate : {0.5f, 2.0f})
            {
                CAPTURE(sampleRate, depth, rate);
                auto drift = preparedDrift(sampleRate, depth, rate);
                const auto x = updates(drift, static_cast<std::size_t>(3600.0 * updateHz(sampleRate)));
                CHECK_THAT(sigmaOf(x), WithinRel(static_cast<double>(depth), 0.05));
                CHECK(std::abs(meanOf(x)) < 0.1 * static_cast<double>(depth));
            }
        }
    }
}

TEST_CASE("Pitch drift: the depth scales the offsets and nothing else", "[drift]")
{
    auto one = preparedDrift(48000.0, 1.0f, 0.5f);
    auto ten = preparedDrift(48000.0, 10.0f, 0.5f);
    const auto a = updates(one, 20000);
    const auto b = updates(ten, 20000);
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        REQUIRE_THAT(b[i], WithinAbs(10.0 * a[i], 1e-5 * (1.0 + std::abs(b[i]))));
    }
}

TEST_CASE("Pitch drift: its rate is the two-pole's autocorrelation, in seconds, at every sample rate", "[drift]")
{
    // rho(t) = (1 + w t) e^(-w t). Over an hour, an estimate at one lag
    // spreads by about sqrt(5 / (w T)): 0.021 at 0.5 Hz. The discrete process
    // updated every 64 samples has rho[k] = a^k (1 + k tanh(w D)), which is
    // the continuous form to (w D)^2 / 3 relative: far below the tolerance.
    for (const double sampleRate : rates)
    {
        for (const float rate : {0.5f, 2.0f})
        {
            CAPTURE(sampleRate, rate);
            auto drift = preparedDrift(sampleRate, 1.5f, rate);
            const auto x = updates(drift, static_cast<std::size_t>(3600.0 * updateHz(sampleRate)));
            const double w = 2.0 * std::numbers::pi * static_cast<double>(rate);
            for (const double wt : {0.5, 1.0, 2.0, 4.0})
            {
                const double t = wt / w;
                const auto lag = static_cast<std::size_t>(std::lround(t * updateHz(sampleRate)));
                const double tLag = static_cast<double>(lag) / updateHz(sampleRate);
                const double expected = (1.0 + (w * tLag)) * std::exp(-w * tLag);
                CAPTURE(wt, lag);
                CHECK_THAT(autocorrelation(x, lag), WithinAbs(expected, 0.08));
            }
        }
    }
}

TEST_CASE("Pitch drift: its spectrum is flat, then falls 40 dB per decade", "[drift]")
{
    // The two-pole's spectrum is 1 / (1 + (f / f_c)^2)^2: from 4 f_c to
    // 40 f_c it falls 10 log10(1601^2 / 17^2) = 39.5 dB. 99% of its variance
    // lies below 3.37 f_c, and half below 0.442 f_c. Welch over an hour in
    // 82 segments: each band's estimate spreads by under 1 dB.
    constexpr double sampleRate = 48000.0;
    constexpr float corner = 0.5f;
    auto drift = preparedDrift(sampleRate, 1.0f, corner);
    const auto x = updates(drift, static_cast<std::size_t>(3600.0 * updateHz(sampleRate)));
    const std::vector<float> xf(x.begin(), x.end());
    const auto spectrum = averagedPowerSpectrum(xf, updateHz(sampleRate), hannWindow(1U << 15U));
    const auto density = [&](double f)
    {
        const auto lo = spectrum.nearestBin(f * 0.9);
        const auto hi = spectrum.nearestBin(f * 1.1);
        double sum = 0.0;
        for (std::size_t k = lo; k <= hi; ++k)
        {
            sum += spectrum.power[k];
        }
        return sum / static_cast<double>(hi - lo + 1);
    };
    const double fc = corner;
    CHECK_THAT(10.0 * std::log10(density(40.0 * fc) / density(4.0 * fc)), WithinAbs(-39.5, 2.0));
    CHECK_THAT(10.0 * std::log10(density(fc / 10.0) / density(fc / 40.0)), WithinAbs(0.0, 1.5));

    double total = 0.0;
    double below50 = 0.0;
    double above99 = 0.0;
    for (std::size_t k = 1; k < spectrum.power.size(); ++k)
    {
        total += spectrum.power[k];
        if (spectrum.frequencyOf(k) < 0.442 * fc)
        {
            below50 += spectrum.power[k];
        }
        if (spectrum.frequencyOf(k) > 3.37 * fc)
        {
            above99 += spectrum.power[k];
        }
    }
    CHECK_THAT(below50 / total, WithinAbs(0.5, 0.06));
    CHECK_THAT(above99 / total, WithinAbs(0.01, 0.005));
}

TEST_CASE("Pitch drift: its distribution is close to Gaussian and does not run away", "[drift]")
{
    // Over an hour (about 7,000 corner periods) a Gaussian reaches about
    // 4 sigma; the kurtosis of a Gaussian is 3.
    auto drift = preparedDrift(48000.0, 1.0f, 0.5f);
    const auto x = updates(drift, static_cast<std::size_t>(3600.0 * updateHz(48000.0)));
    double m2 = 0.0;
    double m4 = 0.0;
    double peak = 0.0;
    for (const double v : x)
    {
        m2 += v * v;
        m4 += v * v * v * v;
        peak = std::max(peak, std::abs(v));
    }
    m2 /= static_cast<double>(x.size());
    m4 /= static_cast<double>(x.size());
    CHECK_THAT(m4 / (m2 * m2), WithinAbs(3.0, 0.3));
    CHECK(peak < 6.0);
    CHECK(peak > 3.0);
}

TEST_CASE("Pitch drift: nothing accumulates over six hours", "[drift]")
{
    // Each hour's sigma within 10% of the depth and its mean within 0.1
    // sigma (5 and 5 spreads, as above); the last hour as the first.
    auto drift = preparedDrift(48000.0, 2.0f, 0.5f);
    const auto perHour = static_cast<std::size_t>(3600.0 * updateHz(48000.0));
    for (int hour = 0; hour < 6; ++hour)
    {
        CAPTURE(hour);
        const auto x = updates(drift, perHour);
        CHECK_THAT(sigmaOf(x), WithinRel(2.0, 0.1));
        CHECK(std::abs(meanOf(x)) < 0.2);
    }
}

TEST_CASE("Pitch drift: the same seed gives the same offsets; voices get independent streams", "[drift]")
{
    auto a = preparedDrift(48000.0, 1.5f, 0.5f, 100);
    auto b = preparedDrift(48000.0, 1.5f, 0.5f, 100);
    const auto x = updates(a, 50000);
    const auto y = updates(b, 50000);
    CHECK(x == y);

    // Adjacent seeds (voice n and voice n + 1): 16 pairs of ten minutes.
    // Independent two-pole streams correlate by about sqrt(2.5 / (w T)) =
    // 0.036 over ten minutes at 0.5 Hz: a mean |r| near 0.03, and no pair
    // beyond 0.15 (4 spreads).
    double sumAbs = 0.0;
    for (std::uint64_t voice = 0; voice < 16; ++voice)
    {
        auto p = preparedDrift(48000.0, 1.0f, 0.5f, 1000 + voice);
        auto q = preparedDrift(48000.0, 1.0f, 0.5f, 1000 + voice + 1);
        const auto n = static_cast<std::size_t>(600.0 * updateHz(48000.0));
        const auto u = updates(p, n);
        const auto v = updates(q, n);
        CHECK_FALSE(u == v);
        double uv = 0.0;
        double uu = 0.0;
        double vv = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            uv += u[i] * v[i];
            uu += u[i] * u[i];
            vv += v[i] * v[i];
        }
        const double r = uv / std::sqrt(uu * vv);
        CAPTURE(voice, r);
        CHECK(std::abs(r) < 0.15);
        sumAbs += std::abs(r);
    }
    CHECK(sumAbs / 16.0 < 0.06);
}

TEST_CASE("Pitch drift: a note from silence draws from the stationary distribution; a held note continues", "[drift]")
{
    auto drift = preparedDrift(48000.0, 1.0f, 0.5f);
    // From silence: each note starts at a fresh value with the drift's own
    // spread, not at 0 (in tune) and not where the last note ended.
    std::vector<double> starts;
    for (int note = 0; note < 20000; ++note)
    {
        drift.stop();
        drift.noteOn();
        starts.push_back(100.0 * drift.offset());
        drift.advance(interval * 3);
    }
    CHECK_THAT(sigmaOf(starts), WithinRel(1.0, 0.03));
    CHECK(std::abs(meanOf(starts)) < 0.03);

    // While sounding, a note-on changes nothing: the offsets carry on bit
    // for bit as if there had been none.
    auto held = preparedDrift(48000.0, 1.5f, 0.5f, 9);
    auto untouched = preparedDrift(48000.0, 1.5f, 0.5f, 9);
    held.advance(1000);
    untouched.advance(1000);
    held.noteOn();
    CHECK(updates(held, 1000) == updates(untouched, 1000));

    // From silence the grid restarts at the note-on: the first update comes
    // 64 samples after it, whatever came before.
    auto drift2 = preparedDrift(48000.0, 1.5f, 0.5f);
    drift2.advance(37);
    drift2.stop();
    drift2.noteOn();
    CHECK(drift2.samplesToUpdate() == interval);

    // reset() restarts the whole sequence.
    auto again = preparedDrift(48000.0, 1.5f, 0.5f);
    const auto first = updates(again, 500);
    again.reset();
    again.noteOn();
    CHECK(updates(again, 500) == first);
}

TEST_CASE("Pitch drift: held for 64 samples on its own grid, whatever the block size", "[drift]")
{
    // The runs are the grid's, split only where a block ends.
    auto drift = preparedDrift(48000.0, 5.0f, 2.0f);
    std::vector<float> offsets;
    std::size_t total = 0;
    for (const std::size_t block : {10U, 100U, 54U, 1000U, 1U, 63U})
    {
        drift.process(block,
                      [&](std::size_t start, std::size_t length, float semitones)
                      {
                          CHECK(start + length <= block);
                          for (std::size_t i = 0; i < length; ++i)
                          {
                              offsets.push_back(semitones);
                          }
                      });
        total += block;
    }
    REQUIRE(offsets.size() == total);
    for (std::size_t i = 1; i < offsets.size(); ++i)
    {
        if (i % interval != 0)
        {
            REQUIRE(sameBits(offsets[i], offsets[i - 1]));
        }
    }

    // A note per sample, through addTo(), in blocks of 1, 37, and 4096.
    const auto render = [](std::size_t block)
    {
        auto d = preparedDrift(96000.0, 20.0f, 5.0f, 11);
        std::vector<float> notes(50000, 64.0f);
        for (std::size_t start = 0; start < notes.size(); start += block)
        {
            d.addTo(std::span{notes}.subspan(start, std::min(block, notes.size() - start)));
        }
        return notes;
    };
    const auto whole = render(4096);
    CHECK(bitIdentical(render(1), whole));
    CHECK(bitIdentical(render(37), whole));
    std::size_t changes = 0;
    for (std::size_t i = 1; i < whole.size(); ++i)
    {
        changes += sameBits(whole[i], whole[i - 1]) ? 0U : 1U;
    }
    CHECK(changes > whole.size() / interval / 2);
}

TEST_CASE("Pitch drift: controls clamp, and NaN is safe", "[drift]")
{
    PitchDrift drift;
    drift.setDepthCents(std::numeric_limits<float>::quiet_NaN());
    CHECK(drift.depthCents() <= 0.0f);
    drift.setDepthCents(-3.0f);
    CHECK(drift.depthCents() <= 0.0f);
    drift.setDepthCents(PitchDrift::minDepthCents * 0.5f);
    CHECK(drift.depthCents() <= 0.0f);
    drift.setDepthCents(1e9f);
    CHECK_THAT(drift.depthCents(), WithinULP(PitchDrift::maxDepthCents, 0));
    drift.setRateHz(std::numeric_limits<float>::quiet_NaN());
    CHECK_THAT(drift.rateHz(), WithinULP(PitchDrift::defaultRateHz, 0));
    drift.setRateHz(0.0f);
    CHECK_THAT(drift.rateHz(), WithinULP(PitchDrift::minRateHz, 0));
    drift.setRateHz(1e6f);
    CHECK_THAT(drift.rateHz(), WithinULP(PitchDrift::maxRateHz, 0));
    CHECK_THAT(PitchDrift::defaultDepthCents, WithinULP(1.5f, 0));
    CHECK_THAT(PitchDrift::defaultRateHz, WithinULP(0.5f, 0));
}

TEST_CASE("Pitch drift: no subnormals at any depth, rate, or sample rate", "[drift]")
{
    for (const double sampleRate : rates)
    {
        for (const float depth : {PitchDrift::minDepthCents, 1.5f, PitchDrift::maxDepthCents})
        {
            for (const float rate : {PitchDrift::minRateHz, 0.5f, PitchDrift::maxRateHz})
            {
                CAPTURE(sampleRate, depth, rate);
                auto drift = preparedDrift(sampleRate, depth, rate);
                bool subnormal = false;
                bool finite = true;
                drift.process(static_cast<std::size_t>(600.0 * sampleRate),
                              [&](std::size_t, std::size_t, float semitones)
                              {
                                  subnormal = subnormal || isSubnormal(semitones);
                                  finite = finite && std::isfinite(semitones);
                              });
                CHECK_FALSE(subnormal);
                CHECK(finite);
            }
        }
    }
    // Below the smallest depth it is off: exactly 0, never a subnormal.
    auto tiny = preparedDrift(48000.0, 1e-30f, 0.5f);
    tiny.advance(interval * 10);
    CHECK(sameBits(tiny.offset(), 0.0f));
}

TEST_CASE("Pitch drift: allocates nothing", "[drift]")
{
    PitchDrift drift{5};
    std::vector<float> notes(4096, 60.0f);
    std::size_t allocations = 0;
    {
        const AllocationCounter counter;
        drift.prepare({.sampleRate = 44100.0f, .maxBlockSize = 4096, .numChannels = 1});
        drift.setDepthCents(1.5f);
        drift.setRateHz(0.7f);
        drift.setSeed(9);
        drift.noteOn();
        drift.addTo(notes);
        float sum = 0.0f;
        drift.process(4096, [&](std::size_t, std::size_t, float semitones) { sum += semitones; });
        drift.advance(10000);
        drift.stop();
        drift.noteOn();
        drift.reset();
        allocations = counter.count();
        (void)sum;
    }
    CHECK(allocations == 0);
}

TEST_CASE("Pitch drift: a voice's oscillators drift together, and voices drift apart", "[drift]")
{
    // Two slots of one voice at the same tuning, retriggered at Phase 0, one
    // inverted, both driven by the voice's drift: they cancel, to float
    // rounding. (The bus adds g y and then -g y; with a fused multiply-add
    // the second leaves the first product's rounding error, at most
    // 2^-24 x 3 voices x a peak of 1.5: 3e-7.) Drifting apart by even a
    // cent would leave about 2 pi x 0.06% of the level, 4e-3 here.
    for (const auto shape : {Waveform::saw, Waveform::triangle, Waveform::sine})
    {
        CAPTURE(shapeName(shape));
        auto a = preparedSlot(shape, 3, 48000.0);
        auto b = preparedSlot(shape, 3, 48000.0);
        for (auto* slot : {&a, &b})
        {
            slot->setDetune(0.3f);
            slot->noteOn();
        }
        b.setInverted(true);
        auto drift = preparedDrift(48000.0, 20.0f, 5.0f);
        Buffer out{2, 20000};
        drift.process(20000,
                      [&](std::size_t start, std::size_t length, float semitones)
                      {
                          a.process(out.block().subBlock(start, length), 57.0f + semitones);
                          b.process(out.block().subBlock(start, length), 57.0f + semitones);
                      });
        float peak = 0.0f;
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            for (const float y : out.channel(ch))
            {
                peak = std::max(peak, std::abs(y));
            }
        }
        CHECK(peak < 1e-6f);
    }

    // Two voices, each with its own drift (adjacent seeds): their pitches
    // differ, so the same note beats.
    auto one = preparedDrift(48000.0, 1.5f, 0.5f, 40);
    auto two = preparedDrift(48000.0, 1.5f, 0.5f, 41);
    one.advance(48000);
    two.advance(48000);
    CHECK(std::abs(one.offset() - two.offset()) > 0.0f);
}

TEST_CASE("Pitch drift: Noise is unaffected", "[drift]")
{
    auto plain = preparedSlot(Waveform::noise, 4, 48000.0);
    auto drifted = preparedSlot(Waveform::noise, 4, 48000.0);
    for (auto* slot : {&plain, &drifted})
    {
        slot->setStereo(0.5f);
        slot->noteOn();
    }
    auto drift = preparedDrift(48000.0, 50.0f, 5.0f);
    Buffer a{2, 10000};
    Buffer b{2, 10000};
    plain.process(a.block(), 60.0f);
    drift.process(10000, [&](std::size_t start, std::size_t length, float semitones)
                  { drifted.process(b.block().subBlock(start, length), 60.0f + semitones); });
    CHECK(bitIdentical(a, b));
}

TEST_CASE("Pitch drift: a slot with drift does not depend on the block size", "[drift]")
{
    for (const auto shape : {Waveform::saw, Waveform::hpulse, Waveform::sine, Waveform::trisaw})
    {
        CAPTURE(shapeName(shape));
        const auto render = [shape](std::size_t block)
        {
            auto slot = preparedSlot(shape, 8, 44100.0);
            slot.setDetune(1.0f);
            slot.setStereo(1.0f);
            slot.noteOn();
            auto drift = preparedDrift(44100.0, 20.0f, 5.0f, 21);
            Buffer out{2, 30000};
            for (std::size_t start = 0; start < 30000; start += block)
            {
                const auto length = std::min(block, std::size_t{30000} - start);
                drift.process(length, [&](std::size_t from, std::size_t run, float semitones)
                              { slot.process(out.block().subBlock(start + from, run), 100.0f + semitones); });
            }
            return out;
        };
        const auto whole = render(4096);
        CHECK(bitIdentical(render(1), whole));
        CHECK(bitIdentical(render(37), whole));
    }
}

namespace
{

// The power below 20 kHz outside the harmonics of f0 (each masked by twice
// its peak deviation under the drift, plus 20 Hz), and the part of it at
// least 1.6 kHz from every harmonic, both relative to the fundamental's.
struct AwayFromHarmonics
{
    double outside = 0.0;
    double far = 0.0;
};

AwayFromHarmonics powerAwayFromHarmonics(std::span<const float> x, double f0, double sampleRate, double peakCents)
{
    const auto spectrum = powerSpectrum(x, sampleRate, blackmanHarrisWindow(x.size()));
    const double lobeHz = static_cast<double>(spectrum.mainLobeBins + 1) * spectrum.binWidth();
    AwayFromHarmonics result;
    double fundamental = 0.0;
    for (std::size_t k = 1; k < spectrum.power.size(); ++k)
    {
        const double f = spectrum.frequencyOf(k);
        if (f >= 20000.0)
        {
            break;
        }
        const double harmonic = std::max(std::round(f / f0), 1.0) * f0;
        const double offset = std::abs(f - harmonic);
        const double width = (2.0 * harmonic * peakCents * std::numbers::ln2 / 1200.0) + 20.0 + lobeHz;
        if (offset > width && f > lobeHz)
        {
            result.outside += spectrum.power[k];
        }
        if (offset >= 1600.0)
        {
            result.far += spectrum.power[k];
        }
        if (std::abs(f - f0) <= width)
        {
            fundamental += spectrum.power[k];
        }
    }
    result.outside /= fundamental;
    result.far /= fundamental;
    return result;
}

} // namespace

TEST_CASE("Pitch drift: adds no aliasing beyond the pitch path's own", "[drift]")
{
    // A saw and a triangle at fs / 10.5 (4.2 kHz at 44.1 kHz), so every
    // alias falls halfway between two harmonics. Beside each harmonic the
    // drift's pitch path has content of its own: FM skirts from its tail,
    // and the hold's sidebands at m x fs/64. So the reference is the same
    // slot on the same held pitch path at 4x the rate, decimated by the
    // library's Oversampler (alias rejection 104 dB): the same content, and
    // practically none of the oscillator's aliasing. Away from the
    // harmonics, the 1x render's excess over the reference is what the
    // 1x oscillator adds:
    //
    // - Without drift, its own aliasing, which the reference matches to
    //   within 1e-11 of the fundamental (-110 dB): the reference is valid.
    // - With drift, also the slow error of each held step, which is under
    //   the pitch-step threshold at this pitch, so it takes the unbanded
    //   bend path (|slope| x change x 2.79 samples, decaying; see
    //   docs/dsp/oscillator-range.md). It is broadband, measured at -96 dB
    //   at the default depth and -93 dB at 5 cents and 2 Hz (saw; the
    //   triangle's is lower). The bounds, -90 and -85 dB, hold it there,
    //   far below the -74 dB that smooth modulation is allowed by the bend
    //   path. Halfway between the harmonics, where the aliases are, the
    //   excess may be no larger.
    constexpr double sampleRate = 44100.0;
    constexpr double f0 = sampleRate / 10.5;
    constexpr std::size_t block = 512;
    constexpr std::size_t warmup = 16 * block;
    constexpr std::size_t length = 1U << 16U;
    const auto note = static_cast<float>(noteOf(f0));
    for (const auto shape : {Waveform::saw, Waveform::triangle})
    {
        // The 1x render and the 4x reference, both driven by one drift at
        // the base rate (the 4x slot runs 4 samples for each base sample).
        const auto render = [&](float depth, float rate, double& peakCents)
        {
            auto base = preparedSlot(shape, 1, sampleRate);
            auto fast = preparedSlot(shape, 1, 4.0 * sampleRate);
            anoesis::dsp::Oversampler oversampler{anoesis::dsp::OversamplingFactor::four};
            oversampler.prepare(
                {.sampleRate = static_cast<float>(sampleRate), .maxBlockSize = block, .numChannels = 2});
            for (auto* slot : {&base, &fast})
            {
                slot->setPan(-1.0f);
                slot->noteOn();
            }
            auto drift = preparedDrift(sampleRate, depth, rate, 5);
            Buffer one{2, warmup + length};
            Buffer four{2, warmup + length};
            peakCents = 0.0;
            for (std::size_t start = 0; start < warmup + length; start += block)
            {
                Buffer silent{2, block};
                const auto inner = oversampler.upsample(silent.block());
                drift.process(block,
                              [&](std::size_t from, std::size_t run, float semitones)
                              {
                                  peakCents = std::max(peakCents, std::abs(100.0 * semitones));
                                  base.process(one.block().subBlock(start + from, run), note + semitones);
                                  fast.process(inner.subBlock(4 * from, 4 * run), note + semitones);
                              });
                oversampler.downsample(four.block().subBlock(start, block));
            }
            const auto tail = [&](const Buffer& b)
            {
                return std::vector<float>(b.channel(0).begin() + warmup, b.channel(0).end());
            };
            return std::pair{tail(one), tail(four)};
        };
        for (const auto& [depth, rate, bound] : {std::tuple{1.5f, 0.5f, 1e-9}, std::tuple{5.0f, 2.0f, 3.2e-9}})
        {
            double peak = 0.0;
            const auto [drifted, driftedReference] = render(depth, rate, peak);
            double unused = 0.0;
            const auto [plain, plainReference] = render(0.0f, rate, unused);
            const auto with = powerAwayFromHarmonics(drifted, f0, sampleRate, peak);
            const auto withReference = powerAwayFromHarmonics(driftedReference, f0, sampleRate, peak);
            const auto without = powerAwayFromHarmonics(plain, f0, sampleRate, peak);
            const auto withoutReference = powerAwayFromHarmonics(plainReference, f0, sampleRate, peak);
            const double excessWith = with.outside - withReference.outside;
            const double excessWithout = without.outside - withoutReference.outside;
            const double farExcessWith = with.far - withReference.far;
            CAPTURE(shapeName(shape), depth, rate, peak, excessWith, excessWithout, farExcessWith);
            CHECK(std::abs(excessWithout) < 1e-11);
            CHECK(excessWith < bound);
            CHECK(farExcessWith <= std::max(excessWith, 0.0));
        }
    }
}
