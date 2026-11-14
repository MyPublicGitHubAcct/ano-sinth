// Tests for OscillatorSlot: one oscillator with its tuning (octave, note,
// fine, master tune), volume, pan, and invert, rendered into a stereo voice
// bus. Measurements: oscillator_controls_report.cpp.
// Doc: docs/dsp/oscillator-controls.md

#include "oscillator_support.hpp"

#include <anoesis/dsp/math/pan.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/lifecycle.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <span>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;
using anoesis::dsp::AudioBlock;
using anoesis::dsp::halfCycle;
using anoesis::dsp::noteToFrequency;
using anoesis::dsp::panGains;
using anoesis::dsp::PanLaw;
using anoesis::dsp::ProcessSpec;
using anoesis::dsp::Tuning;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

namespace
{

using Slot = anoesis::dsp::OscillatorSlot<anoesis::dsp::minBlepTaps>;

constexpr float rate = 48000.0f;

ProcessSpec specAt(float sampleRate)
{
    return {.sampleRate = sampleRate, .maxBlockSize = 512, .numChannels = 2};
}

Slot preparedSlot(Waveform shape, float sampleRate = rate)
{
    Slot slot{minBlepTable64()};
    slot.prepare(specAt(sampleRate));
    slot.setWaveform(shape);
    return slot;
}

// Hard left at unity volume: the left channel carries the oscillator's
// output times exactly 1, the right channel exactly 0.
void routeHardLeft(Slot& slot)
{
    slot.setPan(-1.0f);
    slot.reset();
}

Buffer render(Slot& slot, float note, std::size_t numSamples, std::size_t blockSize = 512)
{
    Buffer out{2, numSamples};
    for (std::size_t start = 0; start < numSamples; start += blockSize)
    {
        slot.process(out.block().subBlock(start, blockSize), note);
    }
    return out;
}

bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool bitIdentical(std::span<const float> a, std::span<const float> b)
{
    return std::ranges::equal(a, b, sameBits);
}

// The pitch the slot should play, in double: reference x 2^((n - 69) / 12)
// with n the note plus the tuning, summed from the same float inputs.
double targetHz(float note, const Tuning& tuning, float referenceHz)
{
    const double n =
        static_cast<double>(note) + (12.0 * tuning.octaves) + tuning.semitones + static_cast<double>(tuning.fine);
    return static_cast<double>(referenceHz) * std::exp2((n - 69.0) / 12.0);
}

double incrementHz(std::uint32_t increment, double sampleRate)
{
    return static_cast<double>(increment) * sampleRate / 4294967296.0;
}

} // namespace

TEST_CASE("OscillatorSlot hard left at unity volume is the bare oscillator, bit for bit", "[dsp][oscillator][slot]")
{
    const Tuning tuning{.octaves = 1, .semitones = -5, .fine = 0.37f};
    constexpr float reference = 432.0f;
    constexpr float note = 57.0f;
    for (const auto shape : periodicShapes)
    {
        CAPTURE(shapeName(shape));
        auto slot = preparedSlot(shape);
        slot.setTuning(tuning);
        slot.setReferenceHz(reference);
        routeHardLeft(slot);
        const auto out = render(slot, note, 3000);

        auto osc = Candidate<MinBlep64>::make();
        osc.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 1});
        osc.setWaveform(shape);
        osc.setFrequency(noteToFrequency(note + tuning.offset(), reference));
        for (std::size_t i = 0; i < out.numSamples(); ++i)
        {
            CAPTURE(i);
            REQUIRE_THAT(out.channel(0)[i], WithinULP(osc.next(), 0));
            REQUIRE_THAT(out.channel(1)[i], WithinULP(0.0f, 0));
        }
    }
}

TEST_CASE("OscillatorSlot plays note + octave x 12 + note offset + fine at the master tune",
          "[dsp][oscillator][slot][tuning]")
{
    auto slot = preparedSlot(Waveform::saw);
    CHECK_THAT(slot.frequencyFor(69.0f), WithinULP(440.0f, 0)); // concert pitch by default

    slot.setReferenceHz(432.0f);
    CHECK_THAT(slot.frequencyFor(69.0f), WithinULP(432.0f, 0));
    slot.setTuning({.octaves = -1});
    CHECK_THAT(slot.frequencyFor(69.0f), WithinULP(216.0f, 0));
    slot.setTuning({.octaves = 2, .semitones = -7, .fine = 0.5f});
    CHECK_THAT(slot.frequencyFor(60.0f), WithinULP(noteToFrequency(60.0f + 17.5f, 432.0f), 0));
}

TEST_CASE("OscillatorSlot's pitch is exact across the whole Octave/Note/Fine range", "[dsp][oscillator][slot][tuning]")
{
    // Every octave (-3..3), note (-7..7), and fine in eighths (-1..1), on 4
    // base notes, at 44.1 and 48 kHz, with A4 = 432 Hz. The increment's
    // frequency must match the double-precision target to within:
    //   1e-6 relative: noteToFrequency in float (its own test derives this);
    //   4.4e-7: rounding note + offset to float (half an ulp of up to 171 is
    //     7.6e-6 semitones, x ln2 / 12);
    //   6e-8: rounding f / fs to float in incrementFor (x 2^32 is exact);
    //   plus the fixed point: the increment is truncated, so up to one unit
    //     (fs / 2^32 Hz, about 1.1e-5 Hz) low.
    // That is 0.0026 cents plus the fixed-point term, which matters only at
    // the lowest pitches.
    constexpr float reference = 432.0f;
    for (const float sampleRate : {44100.0f, 48000.0f})
    {
        auto slot = preparedSlot(Waveform::sine, sampleRate);
        slot.setReferenceHz(reference);
        const auto fs = static_cast<double>(sampleRate);
        int checked = 0;
        for (const float note : {0.0f, 21.0f, 60.0f, 108.0f})
        {
            for (int octaves = -3; octaves <= 3; ++octaves)
            {
                for (int semitones = -7; semitones <= 7; ++semitones)
                {
                    for (int eighth = -8; eighth <= 8; ++eighth)
                    {
                        const float fine = static_cast<float>(eighth) / 8.0f;
                        const Tuning tuning{.octaves = octaves, .semitones = semitones, .fine = fine};
                        slot.setTuning(tuning);
                        Buffer out{2, 1};
                        slot.process(out.block(), note);
                        const auto target = targetHz(note, tuning, reference);
                        CAPTURE(sampleRate, note, octaves, semitones, eighth, target);
                        if (target >= fs / 2.0)
                        {
                            REQUIRE(slot.phaseIncrement() == halfCycle); // clamped at Nyquist
                            continue;
                        }
                        const auto played = incrementHz(slot.phaseIncrement(), fs);
                        const auto tolerance = 1.6e-6 + (fs / 4294967296.0 / target);
                        REQUIRE_THAT(played / target, WithinAbs(1.0, tolerance));
                        ++checked;
                    }
                }
            }
        }
        CHECK(checked > 6000);
    }
}

TEST_CASE("OscillatorSlot's output runs at its increment's frequency", "[dsp][oscillator][slot][tuning]")
{
    // The measured frequency of the rendered sine against the increment's
    // exact frequency. 1e-9 relative: measureFrequency's own bound (its test).
    constexpr std::array tunings{
        Tuning{.octaves = 0, .semitones = 0, .fine = 0.0f},
        Tuning{.octaves = -3, .semitones = -7, .fine = -1.0f},
        Tuning{.octaves = 3, .semitones = 7, .fine = 1.0f},
        Tuning{.octaves = 1, .semitones = -2, .fine = 0.09f},
    };
    for (const Tuning& tuning : tunings)
    {
        CAPTURE(tuning.octaves, tuning.semitones, tuning.fine);
        auto slot = preparedSlot(Waveform::sine);
        slot.setReferenceHz(443.0f);
        slot.setTuning(tuning);
        routeHardLeft(slot);
        const auto out = render(slot, 60.0f, 96000);
        const auto expected = incrementHz(slot.phaseIncrement(), rate);
        CHECK_THAT(measureFrequency(out.channel(0), rate), WithinRel(expected, 1e-9));
        // And the increment against the target, as in the test above.
        const auto target = targetHz(60.0f, tuning, 443.0f);
        CHECK_THAT(expected / target, WithinAbs(1.0, 1.6e-6 + (static_cast<double>(rate) / 4294967296.0 / target)));
    }
}

TEST_CASE("OscillatorSlot: volume scales, pan follows the equal-power law, invert negates exactly",
          "[dsp][oscillator][slot]")
{
    auto reference = preparedSlot(Waveform::saw);
    routeHardLeft(reference);
    const auto bare = render(reference, 45.0f, 2048);

    SECTION("volume")
    {
        auto slot = preparedSlot(Waveform::saw);
        slot.setVolume(0.5f);
        routeHardLeft(slot);
        const auto out = render(slot, 45.0f, 2048);
        for (std::size_t i = 0; i < out.numSamples(); ++i)
        {
            REQUIRE_THAT(out.channel(0)[i], WithinULP(0.5f * bare.channel(0)[i], 0));
        }
    }
    SECTION("pan")
    {
        auto slot = preparedSlot(Waveform::saw);
        slot.setPan(0.25f);
        slot.reset();
        const auto out = render(slot, 45.0f, 2048);
        const auto gains = panGains(0.25f, PanLaw::equalPower);
        for (std::size_t i = 0; i < out.numSamples(); ++i)
        {
            REQUIRE_THAT(out.channel(0)[i], WithinULP(gains.left * bare.channel(0)[i], 0));
            REQUIRE_THAT(out.channel(1)[i], WithinULP(gains.right * bare.channel(0)[i], 0));
        }
    }
    SECTION("invert")
    {
        auto slot = preparedSlot(Waveform::saw);
        slot.setInverted(true);
        routeHardLeft(slot);
        const auto out = render(slot, 45.0f, 2048);
        for (std::size_t i = 0; i < out.numSamples(); ++i)
        {
            REQUIRE_THAT(out.channel(0)[i], WithinULP(-bare.channel(0)[i], 0));
        }
    }
}

TEST_CASE("Two saws, one inverted and a quarter cycle ahead, sum to a DC-free 25% pulse",
          "[dsp][oscillator][slot][invert]")
{
    // Manual p. 15: a saw plus an inverted saw is a pulse whose width is
    // their phase offset. Retrigger/phase is not built yet, so the offset is
    // set by starting the inverted slot early: A4 = 46.875 Hz makes note 69
    // exactly 2^-10 cycles per sample at 48 kHz (increment 2^22), so 256
    // samples ahead is exactly a quarter cycle in fixed point.
    //
    // For a rising saw s(t) = 2t - 1, s(t) - s(t + d) is -2d for
    // t in [0, 1 - d) and 2 - 2d for t in [1 - d, 1): a pulse of width d,
    // high for the last quarter of A's cycle, with mean exactly 0.
    constexpr float reference = 46.875f;
    constexpr float note = 69.0f;
    constexpr std::size_t cycle = 1024;
    constexpr std::size_t ahead = cycle / 4;
    constexpr double width = 0.25;

    auto plain = preparedSlot(Waveform::saw);
    auto inverted = preparedSlot(Waveform::saw);
    for (auto* slot : {&plain, &inverted})
    {
        slot->setReferenceHz(reference);
        routeHardLeft(*slot);
    }
    inverted.setInverted(true);
    inverted.reset();
    static_cast<void>(render(inverted, note, ahead)); // the phase offset
    REQUIRE(inverted.phaseIncrement() == (1U << 22U));

    constexpr std::size_t length = 9 * cycle;
    Buffer sum{2, length};
    plain.process(sum.block(), note);
    inverted.process(sum.block(), note);
    const auto out = sum.channel(0);

    // Away from the edges (64 taps after each, plus the delay), the sum is
    // the ideal pulse. 2e-4: two oscillators, each within 1e-4 of its ideal
    // waveform there (the shared oscillator tests).
    const auto delay = static_cast<double>(plain.latencySamples());
    const double dt = 1.0 / static_cast<double>(cycle);
    const double guard = 64.0;
    std::size_t checked = 0;
    for (std::size_t n = 0; n < length; ++n)
    {
        const auto position = (static_cast<double>(n) - delay) * dt;
        const auto t = position - std::floor(position);
        const auto fromRise = std::abs(t - (1.0 - width)) / dt;
        const auto fromFall = std::min(t, 1.0 - t) / dt;
        if (static_cast<double>(n) <= guard + delay || fromRise <= guard || fromFall <= guard)
        {
            continue;
        }
        const double ideal = t >= 1.0 - width ? 2.0 - (2.0 * width) : -2.0 * width;
        CAPTURE(n, t);
        REQUIRE_THAT(out[n], WithinAbs(ideal, 2e-4));
        ++checked;
    }
    CHECK(checked > length / 2);

    // DC-free: the mean over whole cycles, after the onset. 1e-6: each
    // band-limited saw sits within 2e-7 of its ideal DC
    // (oscillator-waveforms.md).
    const auto steady = out.subspan(cycle, 8 * cycle);
    const double mean = std::accumulate(steady.begin(), steady.end(), 0.0) / static_cast<double>(steady.size());
    CHECK_THAT(mean, WithinAbs(0.0, 1e-6));

    // A 25% pulse has no 4th or 8th harmonic. 8 whole cycles, so every
    // harmonic sits on its own bin under a rectangular window.
    const auto harmonics = measureHarmonics(steady, 46.875, rate, rectangularWindow(steady.size()), 1000.0);
    REQUIRE(harmonics.size() >= 8);
    CHECK(toDb(harmonics[3] / harmonics[0]) < -80.0);
    CHECK(toDb(harmonics[7] / harmonics[0]) < -80.0);
    // ...and its fundamental is (4 / pi) sin(pi / 4) for a 2-unit swing.
    CHECK_THAT(harmonics[0], WithinRel(4.0 / std::numbers::pi * std::sin(std::numbers::pi * width), 1e-4));
}

TEST_CASE("OscillatorSlot: a pitch per sample is applied exactly, sample by sample", "[dsp][oscillator][slot]")
{
    const Tuning tuning{.octaves = -1, .semitones = 3, .fine = -0.2f};
    std::vector<float> notes(1500);
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        notes[i] = 64.0f + (0.7f * std::sin(0.02f * static_cast<float>(i)));
    }
    auto slot = preparedSlot(Waveform::trisaw);
    slot.setTuning(tuning);
    slot.setReferenceHz(445.0f);
    routeHardLeft(slot);
    Buffer out{2, notes.size()};
    slot.process(out.block(), notes);

    auto osc = Candidate<MinBlep64>::make();
    osc.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 1});
    osc.setWaveform(Waveform::trisaw);
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        CAPTURE(i);
        osc.setFrequency(noteToFrequency(notes[i] + tuning.offset(), 445.0f));
        REQUIRE_THAT(out.channel(0)[i], WithinULP(osc.next(), 0));
    }

    // A constant pitch per sample is the same as the per-block form.
    auto perBlock = preparedSlot(Waveform::trisaw);
    auto perSample = preparedSlot(Waveform::trisaw);
    const std::vector<float> constantNotes(1000, 50.5f);
    Buffer a{2, constantNotes.size()};
    Buffer b{2, constantNotes.size()};
    perBlock.process(a.block(), 50.5f);
    perSample.process(b.block(), constantNotes);
    CHECK(bitIdentical(a.channel(0), b.channel(0)));
    CHECK(bitIdentical(a.channel(1), b.channel(1)));
}

TEST_CASE("OscillatorSlot: output does not depend on the block size", "[dsp][oscillator][slot]")
{
    auto whole = preparedSlot(Waveform::pulse);
    whole.setPan(0.4f);
    whole.reset();
    const auto expected = render(whole, 70.0f, 2000, 2000);
    for (const std::size_t blockSize : {1U, 7U, 63U, 64U, 65U, 500U})
    {
        CAPTURE(blockSize);
        auto chunked = preparedSlot(Waveform::pulse);
        chunked.setPan(0.4f);
        chunked.reset();
        const auto out = render(chunked, 70.0f, 2000, blockSize);
        CHECK(bitIdentical(out.channel(0), expected.channel(0)));
        CHECK(bitIdentical(out.channel(1), expected.channel(1)));
    }
}

TEST_CASE("OscillatorSlot honours the lifecycle contract", "[dsp][oscillator][slot]")
{
    // The slot adds into a bus at a pitch; this adapter makes it a
    // BlockProcessor that adds a fixed note into the probe signal.
    struct SlotAtNote
    {
        Slot slot{minBlepTable64()};
        SlotAtNote() noexcept
        {
            slot.setWaveform(Waveform::triangle);
            slot.setTuning({.octaves = 1, .fine = 0.3f});
            slot.setVolume(0.7f);
            slot.setPan(0.3f);
        }
        void prepare(const ProcessSpec& spec) noexcept
        {
            slot.prepare(spec);
        }
        void reset() noexcept
        {
            slot.reset();
        }
        void process(const AudioBlock& block) noexcept
        {
            slot.process(block, 57.0f);
        }
    };
    const auto result = checkLifecycleContract([] { return SlotAtNote{}; }, specAt(rate), 2000);
    CHECK(result.resetMatchesFreshPrepare);
    CHECK(result.rePrepareMatchesFreshPrepare);
}

TEST_CASE("OscillatorSlot: reset() finishes a volume ramp; a mono block is left untouched", "[dsp][oscillator][slot]")
{
    auto ramped = preparedSlot(Waveform::saw);
    static_cast<void>(render(ramped, 60.0f, 100));
    ramped.setVolume(0.25f); // starts a 10 ms ramp
    ramped.reset();

    // The reference settles without reset(): a volume set before prepare()
    // is where prepare() starts.
    Slot settled{minBlepTable64()};
    settled.setWaveform(Waveform::saw);
    settled.setVolume(0.25f);
    settled.prepare(specAt(rate));
    const auto a = render(ramped, 60.0f, 1000);
    const auto b = render(settled, 60.0f, 1000);
    CHECK(bitIdentical(a.channel(0), b.channel(0)));

    Buffer mono{1, 256};
    auto slot = preparedSlot(Waveform::saw);
    slot.process(mono.block(), 60.0f);
    CHECK(std::ranges::all_of(mono.channel(0), [](float s) { return sameBits(s, 0.0f); }));
}

TEST_CASE("OscillatorSlot: noise is silent until the unison slot adds WhiteNoise", "[dsp][oscillator][slot]")
{
    auto slot = preparedSlot(Waveform::noise);
    const auto out = render(slot, 60.0f, 512);
    CHECK(std::ranges::all_of(out.channel(0), [](float s) { return sameBits(s, 0.0f); }));
    CHECK(std::ranges::all_of(out.channel(1), [](float s) { return sameBits(s, 0.0f); }));
}
