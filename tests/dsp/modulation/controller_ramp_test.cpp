// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The mod wheel's and channel pressure's ramp (libs/dsp modulation/
// controller_ramp.hpp; M1.5 task 3, docs/dsp/modulation-sources.md): a
// value 0-1, ramped linearly over 5 ms from where it stands to each new one,
// read at absolute samples. The prototype's Controller (source_candidates.hpp,
// Smoothing::linear, 5 ms) to the bit.

#include "source_candidates.hpp"

#include <anoesis/dsp/modulation/controller_ramp.hpp>
#include <anoesis/dsp/modulation/source_bound.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace mod = anoesis::dsp::modulation;

namespace
{

[[nodiscard]] bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

} // namespace

TEST_CASE("ControllerRamp: 5 ms, rounded to the sample", "[modulation]")
{
    mod::ControllerRamp r;
    for (const auto& [rate, samples] : {
             std::array<double, 2>{48000.0, 240.0},
             std::array<double, 2>{96000.0, 480.0},
             std::array<double, 2>{44100.0, 221.0},
         })
    {
        INFO(rate << " Hz");
        r.prepare(rate);
        CHECK(r.rampSamples() == static_cast<std::uint64_t>(samples));
    }
}

TEST_CASE("ControllerRamp: from where it stands to each new value, linearly, landing exactly", "[modulation]")
{
    mod::ControllerRamp r;
    r.prepare(48000.0);
    CHECK(sameBits(r.valueAt(0), 0.0f));
    CHECK(r.isSteadyFrom(240)); // as from a value set at 0 (the prototype's Controller too)
    r.set(100, 1.0f);
    CHECK(sameBits(r.valueAt(100), 0.0f));
    CHECK(sameBits(r.valueAt(220), 0.5f));
    CHECK(sameBits(r.valueAt(339), 239.0f / 240.0f));
    CHECK(sameBits(r.valueAt(340), 1.0f));
    CHECK(sameBits(r.valueAt(100000), 1.0f));
    CHECK_FALSE(r.isSteadyFrom(339));
    CHECK(r.isSteadyFrom(340));
    // A new value mid-ramp starts from where the ramp stands.
    r.set(220, 0.0f);
    CHECK(sameBits(r.valueAt(220), 0.5f));
    CHECK(sameBits(r.valueAt(340), 0.25f));
    CHECK(sameBits(r.valueAt(460), 0.0f));
    // Monotonic between events, so its two ends bound it.
    const auto b = mod::linearBound(r.valueAt(230), r.valueAt(450));
    for (std::uint64_t t = 230; t <= 450; ++t)
    {
        CHECK(b.contains(r.valueAt(t)));
        CHECK(r.valueAt(t + 1) <= r.valueAt(t));
    }
    r.reset();
    CHECK(sameBits(r.valueAt(1000), 0.0f));
}

TEST_CASE("ControllerRamp: MIDI values / 127, clamped; values outside 0-1 clamped and NaN ignored", "[modulation]")
{
    mod::ControllerRamp r;
    r.prepare(48000.0);
    r.setMidi(0, 127);
    CHECK(sameBits(r.valueAt(240), 1.0f));
    r.setMidi(1000, 64);
    CHECK(sameBits(r.valueAt(1240), 64.0f / 127.0f));
    r.setMidi(2000, 300);
    CHECK(sameBits(r.valueAt(2240), 1.0f));
    r.setMidi(3000, -5);
    CHECK(sameBits(r.valueAt(3240), 0.0f));
    r.set(4000, 1.5f);
    CHECK(sameBits(r.valueAt(4240), 1.0f));
    r.set(5000, std::numeric_limits<float>::quiet_NaN());
    CHECK(sameBits(r.valueAt(5240), 1.0f));
    CHECK(r.isSteadyFrom(5000));
    r.set(6000, -2.0f);
    CHECK(sameBits(r.valueAt(6240), 0.0f));
}

TEST_CASE("ControllerRamp: the prototype's Controller (linear, 5 ms) to the bit", "[modulation]")
{
    namespace sources = anoesis::test::sources;
    for (const double rate : {44100.0, 48000.0, 96000.0})
    {
        mod::ControllerRamp r;
        sources::Controller c;
        r.prepare(rate);
        c.configure(sources::Smoothing::linear, rate, 5.0f);
        std::uint64_t t = 0;
        std::size_t wrong = 0;
        for (int k = 0; k < 400; ++k)
        {
            const int value = (k * 37) % 128;
            r.setMidi(t, value);
            c.set(t, static_cast<float>(value) / 127.0f);
            const auto next = t + 1 + static_cast<std::uint64_t>((k * 53) % 700);
            for (; t < next; ++t)
            {
                wrong += sameBits(r.valueAt(t), c.valueAt(t)) && r.isSteadyFrom(t) == c.isSteadyFrom(t) ? 0U : 1U;
            }
        }
        INFO(rate << " Hz");
        CHECK(wrong == 0);
    }
}
