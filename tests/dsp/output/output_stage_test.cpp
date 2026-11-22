// The output stages as built (M1.4 task 4, stage 2; docs/dsp/mixer.md):
// MainVolume (a gain on every channel, ramped over 10 ms, nothing clipped),
// PeakMeter (a sample peak with a hold, a fall and an over flag, the same at
// any block split, the stage-1 prototype's to the bit), and OutputGuard
// (non-finite samples to 0, counted; everything else untouched).

#include "../support/allocation_counter.hpp"
#include "../voice/mixer_candidates.hpp"

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/output/main_volume.hpp>
#include <anoesis/dsp/output/output_guard.hpp>
#include <anoesis/dsp/output/peak_meter.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/lifecycle.hpp>

#include <catch2/catch_test_macros.hpp>
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

using namespace anoesis::test;
namespace dsp = anoesis::dsp;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinULP;

namespace
{

constexpr float rate = 48000.0f;
constexpr dsp::ProcessSpec spec{.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 2};

std::size_t samples48k(double seconds)
{
    return static_cast<std::size_t>(std::lround(seconds * static_cast<double>(rate)));
}

/// A test signal: tones well above full scale on each channel.
Buffer loudSignal(std::size_t channels, std::size_t n)
{
    Buffer b{channels, n};
    for (std::size_t c = 0; c < channels; ++c)
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(i) / rate;
            b.channel(c)[i] = static_cast<float>(
                3.7 * std::sin(2.0 * std::numbers::pi * (220.0 + (110.0 * static_cast<double>(c))) * t));
        }
    }
    return b;
}

bool sameBits(std::span<const float> a, std::span<const float> b)
{
    return std::ranges::equal(a, b, [](float x, float y)
                              { return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y); });
}

} // namespace

// --- Main Volume -----------------------------------------------------------------------------

TEST_CASE("Main Volume: a gain on every channel, ramped linearly over 10 ms", "[mixer]")
{
    dsp::MainVolume volume;
    volume.setGainImmediately(1.0f);
    volume.prepare({.sampleRate = rate, .maxBlockSize = 1024, .numChannels = 3});
    volume.setGain(0.5f);
    CHECK(volume.isSmoothing());
    CHECK_THAT(volume.targetGain(), WithinULP(0.5f, 0));

    Buffer ones{3, 1000};
    for (std::size_t c = 0; c < 3; ++c)
    {
        std::ranges::fill(ones.channel(c), 1.0f);
    }
    volume.process(ones.block());

    // The same values as a LinearSmoother over 10 ms: 480 samples at 48 kHz.
    dsp::LinearSmoother expected;
    expected.setRampTime(dsp::MainVolume::defaultRampSeconds);
    expected.setCurrentAndTarget(1.0f);
    expected.prepare({.sampleRate = rate, .maxBlockSize = 1024, .numChannels = 3});
    expected.setTarget(0.5f);
    for (std::size_t i = 0; i < 1000; ++i)
    {
        const float g = expected.next();
        for (std::size_t c = 0; c < 3; ++c)
        {
            CHECK_THAT(ones.channel(c)[i], WithinULP(g, 0));
        }
    }
    CHECK_THAT(ones.channel(0)[478], Catch::Matchers::WithinRel(0.5f + (0.5f / 480.0f), 1e-5f));
    CHECK_THAT(ones.channel(2)[479], WithinULP(0.5f, 0));
    CHECK_FALSE(volume.isSmoothing());
    CHECK_THAT(volume.currentGain(), WithinULP(0.5f, 0));
}

TEST_CASE("Main Volume: the output is the input times the gain, however loud (nothing clips)", "[mixer]")
{
    for (const float gain : {1.0f, 0.25f, 10.0f, 1000.0f})
    {
        dsp::MainVolume volume;
        volume.setGainImmediately(gain);
        volume.prepare(spec);
        auto x = loudSignal(2, 2000);
        const auto original = loudSignal(2, 2000);
        volume.process(x.block());
        for (std::size_t c = 0; c < 2; ++c)
        {
            for (std::size_t i = 0; i < 2000; ++i)
            {
                CHECK_THAT(x.channel(c)[i], WithinULP(gain * original.channel(c)[i], 0));
            }
        }
    }
}

TEST_CASE("Main Volume: the same output whatever the block split", "[mixer]")
{
    // Gain changes at their samples, the block split there, as M1.8 will.
    const std::vector<std::pair<std::size_t, float>> events{{1001, 0.3f}, {1300, 2.0f}, {7777, 0.0f}, {9000, 1.0f}};
    const std::size_t n = 12000;
    const auto run = [&](std::size_t block)
    {
        dsp::MainVolume volume;
        volume.prepare({.sampleRate = rate, .maxBlockSize = block, .numChannels = 2});
        auto x = loudSignal(2, n);
        std::size_t from = 0;
        std::size_t e = 0;
        while (from < n)
        {
            if (e < events.size() && events[e].first == from)
            {
                volume.setGain(events[e].second);
                ++e;
            }
            const auto next = e < events.size() ? events[e].first : n;
            const auto to = std::min({n, from + block, next});
            volume.process(x.block().subBlock(from, to - from));
            from = to;
        }
        return x;
    };
    const auto reference = run(4096);
    for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{512}})
    {
        const auto x = run(block);
        CHECK(sameBits(x.channel(0), reference.channel(0)));
        CHECK(sameBits(x.channel(1), reference.channel(1)));
    }
    // And each change really ramped: halfway through the first, the gain is
    // halfway (1 to 0.3 over 480 samples).
    const auto original = loudSignal(2, n);
    CHECK_THAT(reference.channel(0)[1001 + 239] / original.channel(0)[1001 + 239], WithinAbs(0.65, 2e-3));
}

TEST_CASE("Main Volume: a gain that isn't finite and at least 0 counts as 0", "[mixer]")
{
    for (const float bad : {
             std::numeric_limits<float>::quiet_NaN(),
             -1.0f,
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(),
         })
    {
        dsp::MainVolume volume;
        volume.prepare(spec);
        volume.setGain(bad);
        CHECK_THAT(volume.targetGain(), WithinULP(0.0f, 0));
        volume.setGainImmediately(bad);
        CHECK_THAT(volume.currentGain(), WithinULP(0.0f, 0));
    }
}

TEST_CASE("Main Volume: lifecycle and allocation", "[mixer]")
{
    const auto result = checkLifecycleContract(
        []
        {
            dsp::MainVolume v;
            v.setGainImmediately(0.7f);
            return v;
        },
        spec, 3000);
    CHECK(result.passed());
    dsp::MainVolume volume;
    volume.prepare(spec);
    auto x = loudSignal(2, 4096);
    const auto original = loudSignal(2, 4096);
    std::size_t allocations = 0;
    {
        const AllocationCounter counter;
        volume.setGain(0.1f);
        volume.process(x.block());
        allocations = counter.count();
    }
    CHECK(allocations == 0);
    // It acted: past the ramp, a tenth.
    CHECK_THAT(x.channel(1)[4000], WithinULP(0.1f * original.channel(1)[4000], 0));
}

// --- The meter ---------------------------------------------------------------------------------

TEST_CASE("Peak meter: an instant attack, a 0.5 s hold, then 20 dB in 1.7 s", "[mixer]")
{
    dsp::PeakMeter meter;
    meter.prepare(spec);
    std::vector<float> x(samples48k(4.0), 0.0f);
    x[100] = -0.8f;
    std::vector<float> readings;
    readings.reserve(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        meter.process(std::span{x}.subspan(i, 1));
        readings.push_back(meter.reading());
    }
    CHECK_THAT(readings[99], WithinULP(0.0f, 0));
    CHECK_THAT(readings[100], WithinULP(0.8f, 0));         // instant, |x|
    CHECK_THAT(readings[100 + 24000], WithinULP(0.8f, 0)); // held 0.5 s
    CHECK(readings[100 + 24001] < 0.8f);                   // then falling
    // 20 dB down 1.7 s (81,600 samples) after the hold ends. The fall is a
    // float multiply per sample, each rounded by up to 2^-24, so after 81,600
    // of them the level is off by at most 81,600 x 2^-24 = 0.49% (0.042 dB),
    // which at 11.76 dB/s is 3.6 ms: 174 samples either way.
    const auto down = static_cast<std::size_t>(
        std::ranges::find_if(readings, [](float r) { return r > 0.0f && r <= 0.08f; }) - readings.begin());
    CHECK(down + 174 >= 100 + 24000 + 81600);
    CHECK(down <= 100 + 24000 + 81600 + 174);
    CHECK_FALSE(meter.over());
}

TEST_CASE("Peak meter: a peak equal to the held level restarts the hold", "[mixer]")
{
    dsp::PeakMeter meter;
    meter.prepare(spec);
    std::vector<float> x(samples48k(1.5), 0.0f);
    x[0] = 0.8f;
    x[samples48k(0.4)] = -0.8f; // the same level, while the first is still held
    meter.process(std::span{x}.first(samples48k(0.4) + 24000));
    CHECK_THAT(meter.reading(), WithinULP(0.8f, 0)); // 0.9 s after the first peak, 0.5 s after the second
    meter.process(std::span{x}.subspan(samples48k(0.4) + 24000, 2));
    CHECK(meter.reading() < 0.8f);
}

TEST_CASE("Peak meter: the over flag, above full scale, held 1 s", "[mixer]")
{
    dsp::PeakMeter meter;
    meter.prepare(spec);
    const std::array fullScale{1.0f, -1.0f};
    meter.process(fullScale);
    CHECK_FALSE(meter.over()); // 1.0 is full scale, not over it
    const std::array over{-1.0001f};
    meter.process(over);
    CHECK(meter.over());
    std::vector<float> quiet(48000, 0.5f);
    meter.process(std::span{quiet}.first(47999));
    CHECK(meter.over());
    meter.process(std::span{quiet}.subspan(47999));
    CHECK_FALSE(meter.over());
    // Its settings are its own.
    dsp::PeakMeter fast{{.holdSeconds = 0.0f, .fallDecibelsPerSecond = 60.0f, .overSeconds = 0.01f}};
    fast.prepare(spec);
    fast.process(over);
    std::vector<float> silence(480, 0.0f);
    fast.process(silence);
    CHECK_FALSE(fast.over());
    CHECK_THAT(20.0 * std::log10(static_cast<double>(fast.reading()) / 1.0001), WithinAbs(-0.6, 1e-3));
}

TEST_CASE("Peak meter: the same reading whatever the split, and the stage-1 prototype's to the bit", "[mixer]")
{
    // Bursts and decays on two channels.
    const std::size_t n = samples48k(3.0);
    Buffer x{2, n};
    for (std::size_t c = 0; c < 2; ++c)
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(i) / rate;
            const double envelope = std::exp(-3.0 * std::fmod(t + (0.3 * static_cast<double>(c)), 0.7));
            x.channel(c)[i] = static_cast<float>(1.3 * envelope * std::sin(2.0 * std::numbers::pi * 330.0 * t));
        }
    }
    const std::array checkpoints{samples48k(0.25), samples48k(0.9), samples48k(1.6), samples48k(3.0)};
    const auto run = [&](std::size_t block)
    {
        dsp::StereoPeakMeter meter;
        meter.prepare(spec);
        std::vector<float> readings;
        std::vector<bool> overs;
        std::size_t from = 0;
        for (const auto point : checkpoints)
        {
            while (from < point)
            {
                const auto length = std::min(block, point - from);
                meter.process(x.block().subBlock(from, length));
                from += length;
            }
            for (std::size_t c = 0; c < 2; ++c)
            {
                readings.push_back(meter.channel(c).reading());
                overs.push_back(meter.channel(c).over());
            }
        }
        return std::pair{readings, overs};
    };
    const auto reference = run(4096);
    for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{512}})
    {
        const auto r = run(block);
        CHECK(sameBits(r.first, reference.first));
        CHECK(r.second == reference.second);
    }
    // The prototype, channel by channel.
    for (std::size_t c = 0; c < 2; ++c)
    {
        mixer::PeakMeter prototype;
        prototype.prepare(rate);
        std::vector<float> expected;
        std::size_t from = 0;
        for (const auto point : checkpoints)
        {
            prototype.process(x.channel(c).subspan(from, point - from));
            from = point;
            expected.push_back(prototype.reading());
        }
        for (std::size_t k = 0; k < checkpoints.size(); ++k)
        {
            CHECK(std::bit_cast<std::uint32_t>(reference.first[(2 * k) + c]) ==
                  std::bit_cast<std::uint32_t>(expected[k]));
        }
    }
    // Each channel is its own: the readings differ.
    CHECK(std::bit_cast<std::uint32_t>(reference.first[0]) != std::bit_cast<std::uint32_t>(reference.first[1]));
    CHECK(reference.second[0]); // 1.3 peaks: over within the last second
}

TEST_CASE("Peak meter: the hold and fall are seconds at any rate; reset() clears it", "[mixer]")
{
    dsp::PeakMeter meter;
    meter.prepare({.sampleRate = 96000.0f, .maxBlockSize = 512, .numChannels = 1});
    const std::array peak{0.5f};
    meter.process(peak);
    std::vector<float> silence(48000, 0.0f);
    meter.process(silence);
    CHECK_THAT(meter.reading(), WithinULP(0.5f, 0)); // 48000 samples is the hold at 96 kHz
    meter.process(std::span{silence}.first(1));
    CHECK(meter.reading() < 0.5f);
    meter.reset();
    CHECK_THAT(meter.reading(), WithinULP(0.0f, 0));
    CHECK_FALSE(meter.over());
    std::size_t allocations = 0;
    {
        const AllocationCounter counter;
        meter.process(silence);
        allocations = counter.count();
    }
    CHECK(allocations == 0);
}

// --- The guard -----------------------------------------------------------------------------------

TEST_CASE("Output guard: non-finite samples to 0 and counted; everything else untouched", "[mixer]")
{
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float subnormal = std::numeric_limits<float>::denorm_min();
    Buffer x{2, 8};
    const std::array<float, 8> left{0.5f, nan, -inf, 1e30f, subnormal, -0.0f, 3.0f, inf};
    const std::array<float, 8> right{-1e-30f, 2.0f, nan, -subnormal, 0.0f, -7.5f, nan, 1.0f};
    std::ranges::copy(left, x.channel(0).begin());
    std::ranges::copy(right, x.channel(1).begin());

    dsp::OutputGuard guard;
    guard.prepare(spec);
    CHECK(guard.process(x.block()) == 5); // three on the left, two on the right
    CHECK(guard.count() == 5);
    const std::array<float, 8> leftOut{0.5f, 0.0f, 0.0f, 1e30f, subnormal, -0.0f, 3.0f, 0.0f};
    const std::array<float, 8> rightOut{-1e-30f, 2.0f, 0.0f, -subnormal, 0.0f, -7.5f, 0.0f, 1.0f};
    CHECK(sameBits(x.channel(0), leftOut));
    CHECK(sameBits(x.channel(1), rightOut));

    // A clean block counts nothing and changes nothing.
    auto clean = loudSignal(2, 512);
    const auto original = loudSignal(2, 512);
    std::size_t allocations = 0;
    std::size_t found = 0;
    {
        const AllocationCounter counter;
        found = guard.process(clean.block());
        allocations = counter.count();
    }
    CHECK(found == 0);
    CHECK(allocations == 0);
    CHECK(guard.count() == 5);
    CHECK(sameBits(clean.channel(0), original.channel(0)));
    CHECK(sameBits(clean.channel(1), original.channel(1)));
    guard.reset();
    CHECK(guard.count() == 0);
}
