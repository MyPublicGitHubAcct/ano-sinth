// Tests for the oscillator's range, from 0.01 Hz to beyond Nyquist: the
// Nyquist fade (a voice fades to its waveform's mean from 0.45 fs, and plays
// the mean alone above Nyquist), the slot reaching 0.01 Hz through its note
// input with the period and phase of its fixed-point increment, state that
// never accumulates (the long-run guarantee), and a stress run through the
// whole slot at every rate and block size. Measurements:
// oscillator_range_report.cpp. Doc: docs/dsp/oscillator-range.md

#include "range_support.hpp"

#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/oscillators/unison.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

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
#include <numeric>
#include <span>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;
using namespace anoesis::test::range;
using anoesis::dsp::halfCycle;
using anoesis::dsp::incrementFor;
using anoesis::dsp::nyquistFadeGain;
using anoesis::dsp::nyquistFadeStart;
using anoesis::dsp::waveShape;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// --- The Nyquist fade ---------------------------------------------------------

TEST_CASE("The Nyquist fade is unity to 0.45 fs, then a raised cosine to 0 at Nyquist", "[dsp][oscillator][range]")
{
    CHECK(nyquistFadeStart == 0x73333333U); // 0.45 cycles, rounded down
    CHECK_THAT(nyquistFadeGain(0), WithinAbs(1.0, 0.0));
    CHECK_THAT(nyquistFadeGain(incrementFor(1000.0f, 48000.0f)), WithinAbs(1.0, 0.0));
    CHECK_THAT(nyquistFadeGain(nyquistFadeStart), WithinAbs(1.0, 0.0));
    CHECK_THAT(nyquistFadeGain(halfCycle), WithinAbs(0.0, 0.0));
    CHECK_THAT(nyquistFadeGain(0xFFFFFFFFU), WithinAbs(0.0, 0.0)); // above Nyquist, as a raw increment

    // The raised cosine, against double, across the band; never rising, and
    // continuous: neighbouring increments a thousandth of the band apart
    // differ by at most pi/2 x 1/1000 (the cosine's steepest slope).
    float previous = 1.0f;
    constexpr std::uint32_t band = halfCycle - nyquistFadeStart;
    for (std::uint32_t k = 0; k <= 1000; ++k)
    {
        const std::uint32_t increment = nyquistFadeStart + static_cast<std::uint32_t>((std::uint64_t{band} * k) / 1000);
        const double x = static_cast<double>(increment - nyquistFadeStart) / static_cast<double>(band);
        const double expected = 0.5 * (1.0 + std::cos(std::numbers::pi * x));
        const float gain = nyquistFadeGain(increment);
        CAPTURE(k);
        CHECK_THAT(gain, WithinAbs(expected, 1e-6));
        CHECK(gain <= previous);
        CHECK(previous - gain <= 0.0016f);
        previous = gain;
    }
}

TEST_CASE("Above Nyquist, every waveform plays exactly its mean", "[dsp][oscillator][range]")
{
    // The band-limited ideal: a fundamental above fs/2 leaves nothing below
    // it but the waveform's DC. Exact from the first sample above Nyquist,
    // whatever came before (a note below it, residuals in flight, a jump).
    for (const double sampleRate : rates)
    {
        for (const auto shape : periodicShapes)
        {
            for (const double above : {0.5, 0.6, 1.0, 10.0, 1e30})
            {
                CAPTURE(sampleRate, shapeName(shape), above);
                auto osc = preparedOscillator(shape, sampleRate);
                osc.setFrequency(static_cast<float>(0.3 * sampleRate));
                for (int i = 0; i < 100; ++i)
                {
                    static_cast<void>(osc.next());
                }
                osc.setFrequency(static_cast<float>(above * sampleRate));
                const float mean = waveShape(shape).mean();
                for (int i = 0; i < 300; ++i)
                {
                    if (i == 150)
                    {
                        osc.jumpTo(0x12345678U, 0.5f);
                    }
                    REQUIRE(sameBits(osc.next(), mean + 0.0f));
                }
            }
        }
        CAPTURE(sampleRate);
        auto osc = preparedOscillator(Waveform::saw, sampleRate);
        osc.setFrequency(std::numeric_limits<float>::infinity());
        REQUIRE(sameBits(osc.next(), 0.0f));
    }
}

TEST_CASE("Through the fade band, the level follows the fade and the mean stays", "[dsp][oscillator][range]")
{
    // The sine is not filtered by the kernel, so its level is the fade gain
    // alone. Every waveform keeps its mean at every pitch, so a pulse's DC
    // does not move as its pitch crosses the band.
    for (const double cycles : {0.30, 0.44, 0.45, 0.46, 0.475, 0.49, 0.499})
    {
        CAPTURE(cycles);
        auto sine = preparedOscillator(Waveform::sine);
        sine.setIncrement(static_cast<std::uint32_t>(cycles * unitsPerCycle));
        std::vector<float> y(4096 + 64);
        std::ranges::generate(y, [&] { return sine.next(); });
        const std::span tail{y.data() + 64, 4096};
        const double actual = static_cast<double>(sine.phaseIncrement()) / unitsPerCycle;
        const double fade = actual <= 0.45 ? 1.0 : 0.5 * (1.0 + std::cos(std::numbers::pi * (actual - 0.45) / 0.05));
        CHECK_THAT(amplitudeAt(tail, actual), WithinAbs(fade, 1e-5));

        for (const auto shape : periodicShapes)
        {
            CAPTURE(shapeName(shape));
            auto osc = preparedOscillator(shape);
            osc.setIncrement(static_cast<std::uint32_t>(cycles * unitsPerCycle));
            std::ranges::generate(y, [&] { return osc.next(); });
            const double mean = std::accumulate(tail.begin(), tail.end(), 0.0) / 4096.0;
            // An AC part of amplitude <= 1 averages to at most 1/4096 over a
            // window that is not a whole number of cycles.
            CHECK_THAT(mean, WithinAbs(idealMean(shape), 3e-4));
        }
    }
}

TEST_CASE("A sweep or a fast vibrato across Nyquist puts nothing into the audible band", "[dsp][oscillator][range]")
{
    // Per sample, linear in pitch, at 48 kHz: 0.40 fs to 0.60 fs and back,
    // and a 13 Hz vibrato of +-1 semitone around 0.485 fs (0.458 to
    // 0.514 fs). Below 18 kHz there is no tone, only what the fade's
    // transitions put there. Limits: the worst measured over 16 start phases
    // (oscillator-range.md), plus 6 dB. A mute (ramped or not), or a fade
    // that takes the DC with it, measured -10 to -45 dB on the HPulse. The
    // sloped waveforms' vibrato is the slow error of per-sample pitch changes
    // below bandLimitedPitchStep (a wobble at the vibrato rate, not a click).
    constexpr double sampleRate = 48000.0;
    const auto sweep = sweepThroughNyquist(nyquistPathLength);
    const auto vibrato = vibratoAcrossNyquist(nyquistPathLength, sampleRate);
    struct Limit
    {
        Waveform shape;
        double sweepDb;
        double vibratoDb;
    };
    for (const auto& limit : {
             Limit{.shape = Waveform::saw, .sweepDb = -89.0, .vibratoDb = -68.0},
             Limit{.shape = Waveform::pulse, .sweepDb = -88.0, .vibratoDb = -89.0},
             Limit{.shape = Waveform::hpulse, .sweepDb = -88.0, .vibratoDb = -88.0},
             Limit{.shape = Waveform::qpulse, .sweepDb = -88.0, .vibratoDb = -87.0},
             Limit{.shape = Waveform::triangle, .sweepDb = -83.0, .vibratoDb = -76.0},
             Limit{.shape = Waveform::sine, .sweepDb = -93.0, .vibratoDb = -101.0},
             Limit{.shape = Waveform::trisaw, .sweepDb = -88.0, .vibratoDb = -73.0},
         })
    {
        for (const std::uint32_t start : {0U, 0x50000000U})
        {
            CAPTURE(shapeName(limit.shape), start);
            for (const auto* path : {&sweep, &vibrato})
            {
                auto osc = preparedOscillator(limit.shape, sampleRate);
                osc.restart(start);
                std::vector<float> y(nyquistPathLength);
                for (std::size_t i = 0; i < nyquistPathLength; ++i)
                {
                    osc.setFrequency(static_cast<float>((*path)[i] * sampleRate));
                    y[i] = osc.next();
                }
                const double db = powerBelowDb(y, 18000.0, sampleRate);
                CAPTURE(path == &sweep ? "sweep" : "vibrato", db);
                CHECK(db < (path == &sweep ? limit.sweepDb : limit.vibratoDb));
            }
        }
    }
}

// --- The bottom of the range --------------------------------------------------

TEST_CASE("The slot reaches 0.01 Hz and below through its note input", "[dsp][oscillator][range]")
{
    // 0.01 Hz is note -115.79 at A4 = 440 Hz: pitch modulation, bend, and
    // the master tune all arrive as notes, so no other input is needed. A
    // float holds that note to 7.6e-6 semitones; with noteToFrequency's
    // 1e-6, the target is within 1.6e-6 of exact (oscillator-controls.md),
    // and the increment is that, truncated: at most one unit flat.
    for (const double sampleRate : rates)
    {
        for (const double hz : {0.01, 0.0123, 0.001, 1e-5})
        {
            CAPTURE(sampleRate, hz);
            auto slot = preparedSlot(Waveform::saw, 1, sampleRate);
            Buffer out{2, 16};
            slot.process(out.block(), static_cast<float>(noteOf(hz)));
            const double units = hz * unitsPerCycle / sampleRate;
            const auto increment = static_cast<double>(slot.phaseIncrement());
            CHECK(increment <= (units * (1.0 + 1.6e-6)));
            CHECK(increment > (units * (1.0 - 1.6e-6)) - 1.0);
        }
    }
    // The same pitch through the tuning: the lowest octave and note offsets.
    auto slot = preparedSlot(Waveform::saw, 1, 48000.0);
    slot.setTuning({.octaves = -3, .semitones = -7, .fine = -1.0f});
    Buffer out{2, 16};
    slot.process(out.block(), static_cast<float>(noteOf(0.01) + 44.0));
    CHECK_THAT(static_cast<double>(slot.phaseIncrement()), WithinRel(0.01 * unitsPerCycle / 48000.0, 2e-3));
}

TEST_CASE("At 0.01 Hz the slot's period and phase are its fixed-point increment's, over whole cycles",
          "[dsp][oscillator][range]")
{
    // A sine started at Phase 359 crosses zero upwards after 1/360 of a
    // cycle, then once per cycle. Over one whole cycle and a little more,
    // the crossings land where the integer phase says, to the delayed
    // sine's float resolution (2^-24 cycles near the wrap: 0.3 samples at
    // 48 kHz), and the period is 2^32 / increment samples. The pitch is the
    // target less at most one fixed-point unit: the floor in
    // oscillator-controls.md (1.9 cents at 0.01 Hz and 48 kHz). 96 and
    // 192 kHz take 10 and 20 million samples a cycle: the range report
    // measures them.
    for (const double sampleRate : {44100.0, 48000.0})
    {
        CAPTURE(sampleRate);
        auto slot = preparedSlot(Waveform::sine, 1, sampleRate);
        slot.setPan(-1.0f);
        slot.setPhaseDegrees(359.0f);
        slot.noteOn();
        const std::uint32_t increment =
            incrementFor(slot.frequencyFor(static_cast<float>(noteOf(0.01))), static_cast<float>(sampleRate));
        const double period = unitsPerCycle / static_cast<double>(increment);
        const auto length = static_cast<std::size_t>(period * 1.01);
        Buffer out{2, length};
        for (std::size_t start = 0; start < length; start += 4096)
        {
            slot.process(out.block().subBlock(start, std::min<std::size_t>(4096, length - start)),
                         static_cast<float>(noteOf(0.01)));
        }
        const auto y = out.channel(0);
        std::vector<double> crossings;
        for (std::size_t i = 1; i < length; ++i)
        {
            if (y[i - 1] < 0.0f && y[i] >= 0.0f)
            {
                crossings.push_back(static_cast<double>(i - 1) + (y[i - 1] / (y[i - 1] - y[i])));
            }
        }
        REQUIRE(crossings.size() == 2);
        const auto startPhase = static_cast<double>(anoesis::dsp::phaseFromDegrees(359.0f));
        const double latency = slot.latencySamples();
        const double first = ((unitsPerCycle - startPhase) / static_cast<double>(increment)) + latency;
        CHECK_THAT(crossings[0], WithinAbs(first, 0.5));
        CHECK_THAT(crossings[1], WithinAbs(first + period, 0.5));
        const double measuredHz = sampleRate / (crossings[1] - crossings[0]);
        const double unitHz = sampleRate / unitsPerCycle;
        CHECK(measuredHz <= 0.01 * (1.0 + 2e-6));
        CHECK(measuredHz > 0.01 - unitHz - (0.01 * 2e-6));
    }
}

// --- Long runs ----------------------------------------------------------------

TEST_CASE("Nothing accumulates: after any history, the output depends only on the last 72 samples",
          "[dsp][oscillator][range]")
{
    // The oscillator's state is its integer phase, plus residuals that are
    // read and cleared within 64 samples and a sine history 8 samples long.
    // So whatever ran before, hours of it or jumps, an oscillator restarted
    // at the same phase 72 samples earlier gives the same output bit for
    // bit: no rounding error, DC, or level can build up. The phase itself
    // wraps exactly: N samples move it by exactly N x increment, mod 2^32.
    anoesis::dsp::Random random{11};
    for (const auto shape : periodicShapes)
    {
        for (const double cycles : {0.01 / 48000.0, 0.0123, 0.3125, 0.47})
        {
            CAPTURE(shapeName(shape), cycles);
            auto osc = preparedOscillator(shape);
            const auto increment = static_cast<std::uint32_t>(cycles * unitsPerCycle);
            osc.setIncrement(increment);
            for (int jump = 0; jump < 20; ++jump)
            {
                osc.jumpTo(random.nextUint32(), random.nextUnipolar());
                for (std::uint32_t i = 0; i < 1 + (random.nextUint32() % 64U); ++i)
                {
                    static_cast<void>(osc.next());
                }
            }
            const std::uint32_t from = osc.nextPhase();
            constexpr std::uint32_t run = 100000;
            for (std::uint32_t i = 0; i < run; ++i)
            {
                static_cast<void>(osc.next());
            }
            REQUIRE(osc.nextPhase() == from + (run * increment)); // exact, mod 2^32

            auto fresh = preparedOscillator(shape);
            fresh.setIncrement(increment);
            fresh.restart(osc.nextPhase());
            std::array<float, 128> a{};
            std::array<float, 128> b{};
            std::ranges::generate(a, [&] { return osc.next(); });
            std::ranges::generate(b, [&] { return fresh.next(); });
            CHECK(bitIdentical(std::span{a}.subspan(72), std::span{b}.subspan(72)));
        }
    }
}

// --- The whole slot under stress ----------------------------------------------

TEST_CASE("The slot is stable across its whole range, at every rate and block size", "[dsp][oscillator][range]")
{
    // Every waveform, Noise included; 0-8 voices at the largest detune and
    // full width; per-sample sweeps from 0.01 Hz to beyond Nyquist and back
    // and a fast vibrato across it; note-ons at random sub-sample times,
    // Retrig on and off, voice counts changed mid-fade, Inv toggled.
    //
    // The peak bound: each voice's oscillator peaks below perVoicePeak (2.61
    // measured under random jumps at fixed pitches, oscillator-phase.md; 2.33
    // on this path with jumps, oscillator-range.md; the fade only moves a
    // sample towards the mean, |mean| <= 0.75), every channel gain is at
    // most 1, and at most 8 oscillators sound (voices fading out are among
    // the 8). Noise is at most 8 voices' power: sqrt(2 x 8) = 4 per channel.
    constexpr float perVoicePeak = 2.7f;
    constexpr float bound = 8.0f * perVoicePeak;
    for (const double sampleRate : rates)
    {
        const auto notes = stressNotes(sampleRate);
        const auto events = stressEvents(notes.size(), static_cast<std::uint64_t>(sampleRate));
        for (const auto shape : allWaveforms)
        {
            CAPTURE(sampleRate, shapeName(shape));
            const auto whole = renderStress(shape, sampleRate, 4096, notes, events);
            CHECK(whole.finite);
            CAPTURE(whole.peak);
            CHECK(whole.peak <= bound);
            CHECK_FALSE(whole.subnormalOut);
            CHECK_FALSE(whole.subnormalState);
            for (const std::size_t blockSize : {1U, 37U})
            {
                CAPTURE(blockSize);
                const auto split = renderStress(shape, sampleRate, blockSize, notes, events);
                CHECK(bitIdentical(split.out.channel(0), whole.out.channel(0)));
                CHECK(bitIdentical(split.out.channel(1), whole.out.channel(1)));
                CHECK_FALSE(split.subnormalState);
            }
        }
    }
}
